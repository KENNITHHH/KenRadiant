/*
   KenRadiant: Compile window (F9) - stage switches, compact option tabs (explanations on hover),
   presets, the exact commands, optional SOF2 start after the compile, and a Problems list of the
   compiler's warnings / errors (click one to select the brush / entity or fly to the point).

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTabWidget>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QFontDatabase>
#include <QMessageBox>
#include <QProcess>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QDir>
#include <QFileInfo>
#include <vector>
#include <functional>
#include <memory>
#include <map>

#include "commands.h"
#include "preferencesystem.h"
#include "stringio.h"
#include "string/string.h"
#include "os/path.h"
#include "mainframe.h"
#include "map.h"
#include "math/vector.h"
#include "kr_compile_flags.h"
#include <QToolButton>
#include <QScrollArea>

void Build_setPreset( const char* name, const std::vector<CopiedString>& commands );
bool Build_runPreset( const char* name );
bool KrMapText_selectPrimitive( int entity, int brush, bool focus );
void KrCamera_goTo( const Vector3& origin );

namespace
{

/// everything the window remembers (saved in the preferences)
struct CompileOptions
{
	bool bsp = true, meta = true, patchmeta = false, leaktest = false, onlyents = false, nocurves = false, nodetail = false, notjunc = false;
	int samplesize = 0;
	bool vis = true, visFast = false, saveprt = true, nopassage = false;
	bool light = true;
	int quality = 1;  // 0 faster, 1 fast, 2 full
	int super = 2;    // 1, 2, 4
	int samples = 0;
	bool filter = true, dirty = false, patchshadows = false, nomerge = false, shade = false, nocollapse = false;
	int bounce = 2;
	float gamma = 1, compensate = 1;
	CopiedString extraBsp, extraVis, extraLight;
	CopiedString advBsp, advVis, advLight; // ticked "all options": "flag=value;flag=;..."
	bool runGame = false;
	CopiedString gameExe = "SoF2MP.exe";
	CopiedString gameArgs = "+set sv_pure 0 +set developer 1 +devmap %mapname%";
};
CompileOptions g_options;

const char* const c_presetName = "KenRadiant: (Compile window)";

/// the ticked entries of a stage's "all options" list
QString Compile_advanced( const CopiedString& adv ){
	QString out;
	for( const QString& line : QString( adv.c_str() ).split( ';', Qt::SkipEmptyParts ) ){
		const QStringList p = line.split( '=' );
		out += ' ' + p[0];
		if( p.size() > 1 && !p[1].trimmed().isEmpty() )
			out += ' ' + p[1].trimmed();
	}
	return out;
}

QString Compile_commandsText( const CompileOptions& o, std::vector<CopiedString>* commands ){
	QStringList lines;
	auto add = [&]( QString line, const CopiedString& extra ){
		line = ( line + ' ' + extra.c_str() ).simplified() + " \"[MapFile]\"";
		lines << line;
		if( commands != nullptr )
			commands->emplace_back( line.toLatin1().constData() );
	};
	const bool onlyents = o.bsp && o.onlyents;
	if( o.bsp ){
		QString c = "[q3map2]";
		if( onlyents )
			c += " -onlyents";
		else{
			if( o.meta ) c += " -meta";
			if( o.patchmeta ) c += " -patchmeta";
			if( o.samplesize > 0 ) c += QString( " -samplesize %1" ).arg( o.samplesize );
			if( o.leaktest ) c += " -leaktest";
			if( o.nocurves ) c += " -nocurves";
			if( o.nodetail ) c += " -nodetail";
			if( o.notjunc ) c += " -notjunc";
		}
		if( !onlyents ) c += Compile_advanced( o.advBsp );
		add( c, o.extraBsp );
	}
	if( o.vis && !onlyents ){
		QString c = "[q3map2] -vis";
		if( o.saveprt ) c += " -saveprt";
		if( o.visFast ) c += " -fast";
		if( o.nopassage ) c += " -nopassage";
		c += Compile_advanced( o.advVis );
		add( c, o.extraVis );
	}
	if( o.light ){
		QString c = "[q3map2] -light";
		if( o.quality == 0 ) c += " -faster";
		else if( o.quality == 1 ) c += " -fast";
		if( o.super > 1 ) c += QString( " -super %1" ).arg( o.super );
		if( o.samples > 0 ) c += QString( " -samples %1" ).arg( o.samples );
		if( o.filter ) c += " -filter";
		if( o.bounce > 0 ) c += QString( " -bounce %1" ).arg( o.bounce );
		if( o.dirty ) c += " -dirty";
		if( o.patchshadows ) c += " -patchshadows";
		if( o.shade ) c += " -shade";
		if( o.nocollapse ) c += " -nocollapse";
		if( o.nomerge ) c += " -nomerge";
		if( o.gamma != 1 ) c += QString( " -gamma %1" ).arg( o.gamma, 0, 'g', 3 );
		if( o.compensate != 1 ) c += QString( " -compensate %1" ).arg( o.compensate, 0, 'g', 3 );
		c += Compile_advanced( o.advLight );
		add( c, o.extraLight );
	}
	return lines.join( '\n' );
}

/* ---- compiler messages: warnings and errors with where they are ---- */

struct Problem
{
	QString text;
	bool error = false;
	int entity = -1, brush = -1;
	bool hasPoint = false;
	Vector3 point;
	int count = 1;     // the same message for other files / numbers
	QString pattern;
};
std::vector<Problem> g_problems;
QString g_lineBuffer;
int g_lineLevel = 0;
bool g_gamePending = false, g_gameArmed = false;
std::function<void()> g_problemsChanged;

void Problems_addLine( const QString& raw, int level ){
	const QString line = raw.trimmed();
	if( line.isEmpty() )
		return;
	const bool error = level >= 3 || line.contains( "ERROR", Qt::CaseInsensitive );
	if( !error && level < 2 && !line.contains( "WARNING", Qt::CaseInsensitive ) )
		return;
	Problem p;
	p.text = line;
	p.error = error;
	static const QRegularExpression entityRx( "entity\\s*[:#]?\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption );
	static const QRegularExpression brushRx( "(?:brush|primitive)\\s*[:#]?\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption );
	static const QRegularExpression pointRx( "\\(\\s*(-?\\d+(?:\\.\\d+)?)[\\s,]+(-?\\d+(?:\\.\\d+)?)[\\s,]+(-?\\d+(?:\\.\\d+)?)\\s*\\)" );
	if( auto m = entityRx.match( line ); m.hasMatch() )
		p.entity = m.captured( 1 ).toInt();
	if( auto m = brushRx.match( line ); m.hasMatch() )
		p.brush = m.captured( 1 ).toInt();
	if( auto m = pointRx.match( line ); m.hasMatch() ){
		p.hasPoint = true;
		p.point = Vector3( m.captured( 1 ).toFloat(), m.captured( 2 ).toFloat(), m.captured( 3 ).toFloat() );
	}
	// the same message for many files / shaders / surfaces: one line with a count
	if( p.entity < 0 && p.brush < 0 && !p.hasPoint ){
		static const QRegularExpression fileRx( "\\S*[/\\\\.]\\S*" ), numberRx( "-?\\d+(\\.\\d+)?" );
		p.pattern = QString( line ).replace( fileRx, "*" ).replace( numberRx, "#" );
		for( Problem& q : g_problems )
			if( q.pattern == p.pattern ){
				++q.count;
				return;
			}
	}
	if( g_problems.size() < 5000 )
		g_problems.push_back( std::move( p ) );
}

/* ---- the window ---- */

/// the editor's shortcuts are application wide (Enter = clip ...): text boxes keep their keys
class KeepKeysFilter : public QObject
{
protected:
	bool eventFilter( QObject* obj, QEvent* event ) override {
		if( event->type() == QEvent::ShortcutOverride ){
			auto *key = static_cast<QKeyEvent*>( event );
			if( key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::ShiftModifier || key->modifiers() == Qt::KeypadModifier )
				event->accept();
		}
		return QObject::eventFilter( obj, event );
	}
};

/// shows an option's explanation in the hint line while the mouse is over it
class HintFilter : public QObject
{
public:
	QLabel* hint = nullptr;
	QString idle;
protected:
	bool eventFilter( QObject* obj, QEvent* event ) override {
		if( event->type() == QEvent::Enter ){
			const QString text = obj->property( "krHint" ).toString();
			if( !text.isEmpty() )
				hint->setText( text );
		}
		else if( event->type() == QEvent::Leave )
			hint->setText( idle );
		return QObject::eventFilter( obj, event );
	}
};

class CompileDialog : public QDialog
{
	CompileOptions& o = g_options;
	KeepKeysFilter m_keepKeys;
	HintFilter m_hints;
	QPlainTextEdit* m_preview;
	QLabel* m_hint;
	QTabWidget* m_tabs;
	QListWidget* m_problemList;
	int m_problemsTab = -1;
	QCheckBox *m_bspBox, *m_visBox, *m_lightBox;
	std::vector<std::function<void()>> m_loaders; // options -> widgets

	void hint( QWidget* widget, const QString& text ){
		widget->setProperty( "krHint", text );
		widget->setToolTip( text );
		widget->installEventFilter( &m_hints );
	}
	void changed(){
		m_preview->setPlainText( Compile_commandsText( o, nullptr ) );
		m_visBox->setEnabled( !( o.bsp && o.onlyents ) );
		for( int i = 0; i < 3; ++i )
			m_tabs->widget( i )->setEnabled( i == 0 ? o.bsp : i == 1 ? o.vis && !( o.bsp && o.onlyents ) : o.light );
	}
	QCheckBox* check( QGridLayout* grid, int row, int col, const char* text, bool& value, const QString& help ){
		auto *box = new QCheckBox( text );
		QObject::connect( box, &QCheckBox::toggled, [this, &value]( bool on ){ value = on; changed(); } );
		m_loaders.push_back( [box, &value](){ const QSignalBlocker b( box ); box->setChecked( value ); } );
		hint( box, help );
		grid->addWidget( box, row, col );
		return box;
	}
	QWidget* labelled( const char* text, QWidget* field, const QString& help ){
		auto *w = new QWidget;
		auto *h = new QHBoxLayout( w );
		h->setContentsMargins( 0, 0, 0, 0 );
		h->addWidget( new QLabel( text ) );
		h->addWidget( field );
		h->addStretch();
		hint( w, help );
		hint( field, help );
		return w;
	}
	void spin( QGridLayout* grid, int row, int col, const char* text, int& value, int min, int max, const char* special, const QString& help ){
		auto *box = new QSpinBox;
		box->setRange( min, max );
		if( special != nullptr )
			box->setSpecialValueText( special );
		QObject::connect( box, QOverload<int>::of( &QSpinBox::valueChanged ), [this, &value]( int v ){ value = v; changed(); } );
		m_loaders.push_back( [box, &value](){ const QSignalBlocker b( box ); box->setValue( value ); } );
		box->installEventFilter( &m_keepKeys );
		grid->addWidget( labelled( text, box, help ), row, col );
	}
	void dspin( QGridLayout* grid, int row, int col, const char* text, float& value, const QString& help ){
		auto *box = new QDoubleSpinBox;
		box->setRange( 0.1, 4 );
		box->setSingleStep( 0.1 );
		box->setDecimals( 2 );
		QObject::connect( box, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [this, &value]( double v ){ value = float( v ); changed(); } );
		m_loaders.push_back( [box, &value](){ const QSignalBlocker b( box ); box->setValue( value ); } );
		box->installEventFilter( &m_keepKeys );
		grid->addWidget( labelled( text, box, help ), row, col );
	}
	void combo( QGridLayout* grid, int row, int col, const char* text, int& value, const QStringList& items, const std::vector<int>& values, const QString& help ){
		auto *box = new QComboBox;
		box->addItems( items );
		QObject::connect( box, QOverload<int>::of( &QComboBox::currentIndexChanged ), [this, &value, values]( int i ){ if( i >= 0 ) value = values[i]; changed(); } );
		m_loaders.push_back( [box, &value, values](){
			const QSignalBlocker b( box );
			for( std::size_t i = 0; i < values.size(); ++i )
				if( values[i] == value )
					box->setCurrentIndex( int( i ) );
		} );
		grid->addWidget( labelled( text, box, help ), row, col );
	}
	QLineEdit* text( QGridLayout* grid, int row, const char* label, CopiedString& value, const QString& help ){
		auto *edit = new QLineEdit;
		edit->installEventFilter( &m_keepKeys );
		QObject::connect( edit, &QLineEdit::textEdited, [this, &value]( const QString& t ){ value = t.toLatin1().constData(); changed(); } );
		m_loaders.push_back( [edit, &value](){ edit->setText( value.c_str() ); } );
		auto *l = new QLabel( label );
		hint( l, help );
		hint( edit, help );
		grid->addWidget( l, row, 0 );
		grid->addWidget( edit, row, 1, 1, 2 );
		return edit;
	}
	QCheckBox* stageBox( const char* text, bool& value, const QString& help ){
		auto *box = new QCheckBox( text );
		box->setStyleSheet( "QCheckBox { font-weight: bold; }" );
		QObject::connect( box, &QCheckBox::toggled, [this, &value]( bool on ){ value = on; changed(); } );
		m_loaders.push_back( [box, &value](){ const QSignalBlocker b( box ); box->setChecked( value ); } );
		hint( box, help );
		return box;
	}
	QGridLayout* page( const char* title ){
		auto *w = new QWidget;
		auto *grid = new QGridLayout( w );
		grid->setColumnStretch( 0, 1 );
		grid->setColumnStretch( 1, 1 );
		grid->setColumnStretch( 2, 1 );
		grid->setVerticalSpacing( 8 );
		m_tabs->addTab( w, title );
		return grid;
	}
	/// every other q3map2 option of the stage (from q3map2's own help), folded away under a button
	template<std::size_t N>
	void advanced( QGridLayout* g, int row, const KrCompileFlag ( &flags )[N], CopiedString& adv ){
		auto *toggle = new QToolButton;
		toggle->setText( QString( "All %1 q3map2 options" ).arg( N ) );
		toggle->setCheckable( true );
		toggle->setArrowType( Qt::RightArrow );
		toggle->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
		toggle->setAutoRaise( true );
		hint( toggle, "Every other option of this stage, with q3map2's own description (point at one). Ticked ones are added to the command." );
		auto *area = new QScrollArea;
		area->setWidgetResizable( true );
		area->setVisible( false );
		auto *inner = new QWidget;
		auto *grid = new QGridLayout( inner );
		grid->setVerticalSpacing( 2 );
		area->setWidget( inner );
		QObject::connect( toggle, &QToolButton::toggled, [toggle, area]( bool on ){
			toggle->setArrowType( on ? Qt::DownArrow : Qt::RightArrow );
			area->setVisible( on );
		} );
		g->addWidget( toggle, row, 0, 1, 3 );
		g->addWidget( area, row + 1, 0, 1, 3 );
		g->setRowStretch( row + 1, 1 );
		struct Item{ QCheckBox* box; QLineEdit* value; const char* flag; };
		auto items = std::make_shared<std::vector<Item>>();
		const auto store = [this, items, &adv](){
			QString text;
			for( const Item& it : *items )
				if( it.box->isChecked() )
					text += QString( it.flag ) + '=' + ( it.value != nullptr ? it.value->text().trimmed().replace( ';', ' ' ) : QString() ) + ';';
			adv = text.toLatin1().constData();
			changed();
		};
		constexpr int columns = 4;
		for( std::size_t i = 0; i < N; ++i ){
			const KrCompileFlag& f = flags[i];
			auto *cell = new QWidget;
			auto *h = new QHBoxLayout( cell );
			h->setContentsMargins( 0, 0, 0, 0 );
			auto *box = new QCheckBox( QString( f.flag ).mid( 1 ) );
			h->addWidget( box );
			QLineEdit* value = nullptr;
			if( !string_empty( f.arg ) ){
				value = new QLineEdit;
				value->setPlaceholderText( f.arg );
				value->setMaximumWidth( 70 );
				value->installEventFilter( &m_keepKeys );
				h->addWidget( value );
				QObject::connect( value, &QLineEdit::textEdited, [box, store]( const QString& t ){
					if( !t.isEmpty() && !box->isChecked() )
						box->setChecked( true ); // typing a value ticks it
					else
						store();
				} );
			}
			h->addStretch();
			const QString help = QString( "%1 %2 - %3" ).arg( f.flag, f.arg, f.help );
			hint( box, help );
			if( value != nullptr )
				hint( value, help );
			QObject::connect( box, &QCheckBox::toggled, [store]( bool ){ store(); } );
			items->push_back( { box, value, f.flag } );
			grid->addWidget( cell, int( i ) / columns, int( i ) % columns );
		}
		m_loaders.push_back( [items, &adv](){
			std::map<QString, QString> on;
			for( const QString& line : QString( adv.c_str() ).split( ';', Qt::SkipEmptyParts ) ){
				const QStringList p = line.split( '=' );
				on[p[0]] = p.size() > 1 ? p[1] : QString();
			}
			for( const Item& it : *items ){
				const QSignalBlocker b( it.box );
				const auto found = on.find( it.flag );
				it.box->setChecked( found != on.end() );
				if( it.value != nullptr ){
					const QSignalBlocker b2( it.value );
					it.value->setText( found != on.end() ? found->second : QString() );
				}
			}
		} );
	}
	void load(){
		for( auto& f : m_loaders )
			f();
		changed();
	}
	void preset( int which ){
		CompileOptions p;
		p.extraBsp = o.extraBsp; p.extraVis = o.extraVis; p.extraLight = o.extraLight;
		p.advBsp = o.advBsp; p.advVis = o.advVis; p.advLight = o.advLight;
		p.runGame = o.runGame; p.gameExe = o.gameExe; p.gameArgs = o.gameArgs;
		switch( which ){
		case 0: p.visFast = true; p.quality = 0; p.super = 1; p.filter = false; p.bounce = 0; break;  // quick test
		case 1: break;                                                                                   // final
		case 2: p.quality = 2; p.bounce = 4; p.dirty = true; p.patchshadows = true; p.samples = 2; break; // high quality
		case 3: p.bsp = false; p.vis = false; break;                                                     // re-light only
		case 4: p.onlyents = true; p.vis = false; p.light = false; break;                                // entities only
		}
		o = p;
		load();
	}

public:
	CompileDialog( QWidget* parent ) : QDialog( parent, Qt::Window ){
		setWindowTitle( "Compile" );
		resize( 760, 440 );
		auto *outer = new QVBoxLayout( this );
		m_hint = new QLabel;
		m_hints.hint = m_hint;
		m_hints.idle = "Point at an option to see what it does.";

		// top: preset, stages, compile
		{
			auto *row = new QHBoxLayout;
			auto *presets = new QComboBox;
			presets->addItems( { "Preset...", "Quick test", "Final", "High quality", "Re-light only", "Entities only" } );
			hint( presets, "Quick test: fast, rough light. Final: good light, 2 bounces. High quality: full light, 4 bounces, dirt, curve shadows. "
			               "Re-light only: keeps the .bsp geometry. Entities only: moved a light / changed keys (seconds)." );
			QObject::connect( presets, QOverload<int>::of( &QComboBox::activated ), [this, presets]( int i ){
				if( i > 0 )
					preset( i - 1 );
				presets->setCurrentIndex( 0 );
			} );
			row->addWidget( presets );
			row->addSpacing( 12 );
			m_bspBox = stageBox( "BSP", o.bsp, "Builds the geometry (.bsp) from the brushes, patches and models. Needed after any brush change." );
			m_visBox = stageBox( "VIS", o.vis, "Works out what can be seen from where, so the game skips what's behind walls. The frame rate depends on it." );
			m_lightBox = stageBox( "LIGHT", o.light, "Bakes the lightmaps: lights, sun, sky light, glowing shaders. The slowest stage." );
			row->addWidget( m_bspBox );
			row->addWidget( m_visBox );
			row->addWidget( m_lightBox );
			row->addStretch();
			auto *game = new QCheckBox( "Start SOF2 after" );
			QObject::connect( game, &QCheckBox::toggled, [this]( bool on ){ o.runGame = on; } );
			m_loaders.push_back( [game, this](){ const QSignalBlocker b( game ); game->setChecked( o.runGame ); } );
			hint( game, "When the compile finished without an error, starts the game with the map (Game tab: program and arguments)." );
			row->addWidget( game );
			auto *run = new QPushButton( "Compile" );
			run->setDefault( true );
			run->setStyleSheet( "QPushButton { background: #2e8b57; color: white; font-weight: bold; padding: 4px 18px; }" );
			hint( run, "Saves the map and runs the commands below. Progress in the console; warnings and errors in the Problems tab." );
			QObject::connect( run, &QPushButton::clicked, [this](){ compile(); } );
			row->addWidget( run );
			outer->addLayout( row );
		}

		m_tabs = new QTabWidget;
		outer->addWidget( m_tabs, 1 );
		{
			QGridLayout* g = page( "BSP" );
			check( g, 0, 0, "Meta surfaces (-meta)", o.meta, "Merges faces into big triangle surfaces per shader: faster in game, better lightmap packing. Keep it on." );
			check( g, 0, 1, "Curves to meshes (-patchmeta)", o.patchmeta, "Patches become fixed triangle meshes that merge and light like brushes. Off = normal curves." );
			check( g, 0, 2, "Leak test (-leaktest)", o.leaktest, "Stop when the map leaks instead of writing a broken .bsp. The pointfile (File > Pointfile) shows the way to the hole." );
			check( g, 1, 0, "Entities only (-onlyents)", o.onlyents, "Only rewrites the entities in the existing .bsp (moved light, changed key). Seconds. Skips VIS; tick LIGHT if lights moved." );
			check( g, 1, 1, "No curves (-nocurves)", o.nocurves, "Leaves all patches out: a quick test of the brush work only." );
			check( g, 1, 2, "No detail (-nodetail)", o.nodetail, "Leaves detail brushes out: shows what the structural hull looks like (testing)." );
			check( g, 2, 0, "No T-junction fix (-notjunc)", o.notjunc, "Skips fixing T-junctions. Faster, but sparkles / cracks can appear between faces. Testing only." );
			spin( g, 2, 1, "Lightmap texel (-samplesize)", o.samplesize, 0, 128, "default 16", "World units per lightmap pixel for the whole map. Lower = sharper shadows, bigger lightmaps, slower light (8 sharp, 32 soft)." );
			text( g, 3, "Extra options", o.extraBsp, "Anything else for the BSP stage, as on the command line." );
			advanced( g, 4, c_krFlags_bsp, o.advBsp );
		}
		{
			QGridLayout* g = page( "VIS" );
			check( g, 0, 0, "Fast (-fast)", o.visFast, "Rough visibility in a fraction of the time. For testing; use full VIS for a release." );
			check( g, 0, 1, "Keep portals (-saveprt)", o.saveprt, "Keeps the .prt file to look at the portals (Plugins > Prtview) when placing hint brushes." );
			check( g, 0, 2, "No passage vis (-nopassage)", o.nopassage, "Uses the older, slower vis method. Only if passage vis gives you wrong visibility." );
			text( g, 1, "Extra options", o.extraVis, "Anything else for the VIS stage." );
			advanced( g, 2, c_krFlags_vis, o.advVis );
		}
		{
			QGridLayout* g = page( "LIGHT" );
			combo( g, 0, 0, "Speed", o.quality, { "Faster", "Fast", "Full" }, { 0, 1, 2 },
			       "Faster (-faster): very rough, light ends abruptly - quick looks only. Fast (-fast): what most maps ship with. Full: most accurate, slowest." );
			combo( g, 0, 1, "Supersample (-super)", o.super, { "1", "2", "4" }, { 1, 2, 4 }, "Each lightmap pixel from N x N samples: smoother shadow edges. Costs N x N time. 2 is a good final value." );
			spin( g, 0, 2, "Bounces (-bounce)", o.bounce, 0, 8, "off", "Light reflects off surfaces this many times and picks up their colour: fills rooms lit only through a door or window. Each bounce costs about one direct pass." );
			check( g, 1, 0, "Smooth (-filter)", o.filter, "Blurs the lightmaps slightly to hide pixel steps on shadow edges. Cheap, usually on." );
			check( g, 1, 1, "Corner dirt (-dirty)", o.dirty, "Darkens corners, creases and where objects meet the floor (ambient occlusion). Looks grounded; adds time." );
			check( g, 1, 2, "Curves cast shadows (-patchshadows)", o.patchshadows, "q3map2 lets light pass through patches unless this is on. Arches, pipes, curved roofs." );
			check( g, 2, 0, "Smooth shading (-shade)", o.shade, "Phong shading: light flows smoothly over round brush work and models instead of faceting." );
			check( g, 2, 1, "No collapse (-nocollapse)", o.nocollapse, "Keeps identical lightmaps separate. Only for lightmap debugging." );
			check( g, 2, 2, "No merge after light (-nomerge)", o.nomerge, "This q3map2 merges surfaces sharing a lightmap after -light. Turn on only if you see seams or wrong light on merged surfaces." );
			spin( g, 3, 0, "Adaptive samples (-samples)", o.samples, 0, 4, "off", "Extra samples only where the light changes (shadow edges). Like -super but cheaper on flat areas." );
			dspin( g, 3, 1, "Gamma (-gamma)", o.gamma, "Above 1 brightens dark and mid tones (1 = unchanged). The Sun Calibrator previews it." );
			dspin( g, 3, 2, "Compensate (-compensate)", o.compensate, "Divides the lightmap brightness (1 = unchanged). With gamma, keeps bright spots from clipping." );
			text( g, 4, "Extra options", o.extraLight, "Anything else for the LIGHT stage, e.g. -lightmapsize 256." );
			advanced( g, 5, c_krFlags_light, o.advLight );
		}
		{
			QGridLayout* g = page( "Game" );
			text( g, 0, "Program", o.gameExe, QString( "The game in the engine folder (%1). SoF2MP.exe for multiplayer maps." ).arg( EnginePath_get() ) );
			text( g, 1, "Arguments", o.gameArgs, "%mapname% becomes the map's name. Add e.g. +set g_gametype inf for a gametype, +set fs_game <mod> for a mod folder." );
			auto *now = new QPushButton( "Start SOF2 now" );
			now->setAutoDefault( false );
			hint( now, "Starts the game with the last compiled .bsp, without compiling." );
			QObject::connect( now, &QPushButton::clicked, [](){ Game_start(); } );
			g->addWidget( now, 2, 0 );
			g->setRowStretch( 3, 1 );
		}
		{
			auto *w = new QWidget;
			auto *v = new QVBoxLayout( w );
			m_problemList = new QListWidget;
			m_problemList->setToolTip( "Click: select the entity / brush or fly to the point; double-click: also fly the camera there" );
			QObject::connect( m_problemList, &QListWidget::currentRowChanged, [this]( int row ){ goToProblem( row, false ); } );
			QObject::connect( m_problemList, &QListWidget::itemDoubleClicked, [this]( QListWidgetItem* item ){ goToProblem( m_problemList->row( item ), true ); } );
			v->addWidget( m_problemList );
			auto *row = new QHBoxLayout;
			auto *prev = new QPushButton( "Previous" );
			auto *next = new QPushButton( "Next" );
			auto *clear = new QPushButton( "Clear" );
			for( auto *b : { prev, next, clear } ){
				b->setAutoDefault( false );
				row->addWidget( b );
			}
			row->addStretch();
			QObject::connect( prev, &QPushButton::clicked, [this](){ if( m_problemList->count() ) m_problemList->setCurrentRow( std::max( 0, m_problemList->currentRow() - 1 ) ); } );
			QObject::connect( next, &QPushButton::clicked, [this](){ if( m_problemList->count() ) m_problemList->setCurrentRow( std::min( m_problemList->count() - 1, m_problemList->currentRow() + 1 ) ); } );
			QObject::connect( clear, &QPushButton::clicked, [this](){ g_problems.clear(); problemsChanged(); } );
			v->addLayout( row );
			m_problemsTab = m_tabs->addTab( w, "Problems" );
		}

		m_hint->setWordWrap( true );
		m_hint->setMinimumHeight( m_hint->fontMetrics().lineSpacing() * 2 + 4 );
		m_hint->setAlignment( Qt::AlignTop | Qt::AlignLeft );
		m_hint->setStyleSheet( "color: palette(mid);" );
		m_hint->setText( m_hints.idle );
		outer->addWidget( m_hint );

		m_preview = new QPlainTextEdit;
		m_preview->setReadOnly( true );
		m_preview->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
		m_preview->setFixedHeight( m_preview->fontMetrics().lineSpacing() * 3 + 12 );
		m_preview->setLineWrapMode( QPlainTextEdit::NoWrap );
		hint( m_preview, "The commands that run ([q3map2] and [MapFile] are filled in by the build system)." );
		outer->addWidget( m_preview );
		{
			auto *row = new QHBoxLayout;
			auto *menu = new QPushButton( "Add to Build menu" );
			menu->setAutoDefault( false );
			hint( menu, QString( "Keeps these commands as '%1' at the top of the Build menu." ).arg( c_presetName ) );
			QObject::connect( menu, &QPushButton::clicked, [this](){
				std::vector<CopiedString> commands;
				Compile_commandsText( o, &commands );
				if( commands.empty() )
					return;
				Build_setPreset( c_presetName, commands );
				globalOutputStream() << "Compile: Build menu entry '" << c_presetName << "' updated\n";
			} );
			auto *close = new QPushButton( "Close" );
			close->setAutoDefault( false );
			QObject::connect( close, &QPushButton::clicked, [this](){ hide(); } );
			row->addWidget( menu );
			row->addStretch();
			row->addWidget( close );
			outer->addLayout( row );
		}
		load();
		g_problemsChanged = [this](){ problemsChanged(); };
		problemsChanged();
	}
	~CompileDialog() override {
		g_problemsChanged = nullptr;
	}
	void problemsChanged(){
		const int keep = m_problemList->currentRow();
		{
			const QSignalBlocker b( m_problemList );
			m_problemList->clear();
			int errors = 0;
			for( const Problem& p : g_problems ){
				QString where;
				if( p.entity >= 0 ) where += QString( "entity %1 " ).arg( p.entity );
				if( p.brush >= 0 ) where += QString( "brush %1 " ).arg( p.brush );
				if( p.hasPoint ) where += QString( "at %1 %2 %3 " ).arg( p.point.x() ).arg( p.point.y() ).arg( p.point.z() );
				auto *item = new QListWidgetItem( ( where.isEmpty() ? QString() : "[" + where.trimmed() + "]  " ) + p.text
				                                  + ( p.count > 1 ? QString( "   (and %1 more like it)" ).arg( p.count - 1 ) : QString() ) );
				item->setForeground( p.error ? QColor( 255, 90, 80 ) : QColor( 255, 190, 60 ) );
				m_problemList->addItem( item );
				errors += p.error;
			}
			if( keep >= 0 && keep < m_problemList->count() )
				m_problemList->setCurrentRow( keep );
		}
		m_tabs->setTabText( m_problemsTab, g_problems.empty() ? QString( "Problems" ) : QString( "Problems (%1)" ).arg( g_problems.size() ) );
	}
	void goToProblem( int row, bool focus ){
		if( row < 0 || row >= int( g_problems.size() ) )
			return;
		const Problem& p = g_problems[row];
		if( p.entity >= 0 && KrMapText_selectPrimitive( p.entity, p.brush, focus || !p.hasPoint ) )
			return;
		if( p.hasPoint )
			KrCamera_goTo( p.point + Vector3( 0, -96, 48 ) );
	}
	void showProblems(){
		m_tabs->setCurrentIndex( m_problemsTab );
	}
	void compile(){
		std::vector<CopiedString> commands;
		Compile_commandsText( o, &commands );
		if( commands.empty() ){
			QMessageBox::information( this, "Compile", "Tick BSP, VIS or LIGHT." );
			return;
		}
		if( Map_Unnamed( g_map ) ){
			QMessageBox::information( this, "Compile", "Save the map first (File > Save As): q3map2 compiles the .map file." );
			return;
		}
		Build_setPreset( c_presetName, commands );
		g_gamePending = o.runGame;
		if( !Build_runPreset( c_presetName ) ){
			g_gamePending = false;
			globalErrorStream() << "Compile: couldn't start the build\n";
		}
	}
	static QString gameCommand( const char* bspName ){
		QString args = QString( g_options.gameArgs.c_str() );
		args.replace( "%mapname%", bspName );
		return QString( "\"%1%2\" %3" ).arg( EnginePath_get(), g_options.gameExe.c_str(), args );
	}
	static void Game_start(){
		if( Map_Unnamed( g_map ) )
			return;
		const QString base = QFileInfo( Map_Name( g_map ) ).completeBaseName();
		const QString exe = QDir( EnginePath_get() ).filePath( g_options.gameExe.c_str() );
		QString args = QString( g_options.gameArgs.c_str() );
		args.replace( "%mapname%", base );
		globalOutputStream() << "Starting " << exe.toLatin1().constData() << ' ' << args.toLatin1().constData() << '\n';
		if( !QProcess::startDetached( exe, QProcess::splitCommand( args ), EnginePath_get() ) )
			globalErrorStream() << "Compile: couldn't start " << exe.toLatin1().constData() << " - check the Game tab and the engine path in the preferences\n";
	}
};

CompileDialog* g_compile = nullptr;
void Compile_show(){
	if( g_compile == nullptr ){
		g_compile = new CompileDialog( MainFrame_getWindow() );
		QObject::connect( g_compile, &QObject::destroyed, [](){ g_compile = nullptr; } );
	}
	g_compile->show();
	g_compile->raise();
	g_compile->activateWindow();
}

} // namespace

/* ---- called by the build monitor ---- */

/// a build starts: clear the problems, arm the game start if the Compile window asked for it
void KrCompile_buildStarted(){
	g_problems.clear();
	g_lineBuffer.clear();
	g_gameArmed = g_gamePending;
	g_gamePending = false;
	if( g_problemsChanged )
		g_problemsChanged();
}
/// compiler text as it arrives (level 2 = warning, 3 = error)
void KrCompile_text( int level, const char* text, std::size_t length ){
	if( level != g_lineLevel && !g_lineBuffer.isEmpty() ){
		Problems_addLine( g_lineBuffer, g_lineLevel );
		g_lineBuffer.clear();
	}
	g_lineLevel = level;
	g_lineBuffer += QString::fromLatin1( text, int( length ) );
	const std::size_t before = g_problems.size();
	for( int nl; ( nl = g_lineBuffer.indexOf( '\n' ) ) >= 0; ){
		Problems_addLine( g_lineBuffer.left( nl ), level );
		g_lineBuffer.remove( 0, nl + 1 );
	}
	if( g_problems.size() != before && g_problemsChanged )
		g_problemsChanged();
}
/// the build finished: the game command to run, if the Compile window asked for one (nullptr otherwise)
const char* KrCompile_takeGameCommand( const char* bspName ){
	if( !g_gameArmed )
		return nullptr;
	g_gameArmed = false;
	static CopiedString command;
	command = CompileDialog::gameCommand( bspName ).toLatin1().constData();
	return command.c_str();
}
void KrCompile_buildFinished(){
	if( !g_lineBuffer.isEmpty() ){
		Problems_addLine( g_lineBuffer, g_lineLevel );
		g_lineBuffer.clear();
	}
	if( g_problemsChanged )
		g_problemsChanged();
	if( !g_problems.empty() ){
		std::size_t errors = 0;
		for( const Problem& p : g_problems )
			errors += p.error;
		globalOutputStream() << "Compile: " << g_problems.size() << " warnings / errors (" << errors << " errors) - Compile window > Problems lists them\n";
		if( g_compile != nullptr && g_compile->isVisible() )
			g_compile->showProblems();
	}
}

void KrCompile_registerCommands(){
	GlobalCommands_insert( "CompileWindow", makeCallbackF( Compile_show ), QKeySequence( "F9" ) );
	auto prefB = []( const char* name, bool& v ){ GlobalPreferenceSystem().registerPreference( name, BoolImportStringCaller( v ), BoolExportStringCaller( v ) ); };
	auto prefI = []( const char* name, int& v ){ GlobalPreferenceSystem().registerPreference( name, IntImportStringCaller( v ), IntExportStringCaller( v ) ); };
	auto prefF = []( const char* name, float& v ){ GlobalPreferenceSystem().registerPreference( name, FloatImportStringCaller( v ), FloatExportStringCaller( v ) ); };
	auto prefS = []( const char* name, CopiedString& v ){ GlobalPreferenceSystem().registerPreference( name, CopiedStringImportStringCaller( v ), CopiedStringExportStringCaller( v ) ); };
	CompileOptions& o = g_options;
	prefB( "KrCompileBsp", o.bsp ); prefB( "KrCompileMeta", o.meta ); prefB( "KrCompilePatchmeta", o.patchmeta );
	prefB( "KrCompileLeaktest", o.leaktest ); prefB( "KrCompileOnlyents", o.onlyents ); prefI( "KrCompileSamplesize", o.samplesize );
	prefB( "KrCompileNocurves", o.nocurves ); prefB( "KrCompileNodetail", o.nodetail ); prefB( "KrCompileNotjunc", o.notjunc );
	prefB( "KrCompileVis", o.vis ); prefB( "KrCompileVisFast", o.visFast ); prefB( "KrCompileSaveprt", o.saveprt ); prefB( "KrCompileNopassage", o.nopassage );
	prefB( "KrCompileLight", o.light ); prefI( "KrCompileQuality", o.quality ); prefI( "KrCompileSuper", o.super ); prefI( "KrCompileSamples", o.samples );
	prefB( "KrCompileFilter", o.filter ); prefB( "KrCompileDirty", o.dirty ); prefB( "KrCompilePatchshadows", o.patchshadows );
	prefB( "KrCompileShade", o.shade ); prefB( "KrCompileNocollapse", o.nocollapse );
	prefB( "KrCompileNomerge", o.nomerge ); prefI( "KrCompileBounce", o.bounce );
	prefF( "KrCompileGamma", o.gamma ); prefF( "KrCompileCompensate", o.compensate );
	prefS( "KrCompileAdvBsp", o.advBsp ); prefS( "KrCompileAdvVis", o.advVis ); prefS( "KrCompileAdvLight", o.advLight );
	prefS( "KrCompileExtraBsp", o.extraBsp ); prefS( "KrCompileExtraVis", o.extraVis ); prefS( "KrCompileExtraLight", o.extraLight );
	prefB( "KrCompileRunGame", o.runGame ); prefS( "KrCompileGameExe", o.gameExe ); prefS( "KrCompileGameArgs", o.gameArgs );
}
