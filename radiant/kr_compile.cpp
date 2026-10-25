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
#include <type_traits>
#include <iterator>
#include <algorithm>
#include <memory>
#include <map>

#include "commands.h"
#include "preferencesystem.h"
#include "stringio.h"
#include "string/string.h"
#include "os/path.h"
#include "mainframe.h"
#include "map.h"
#include "qerplugin.h"
#include "math/vector.h"
#include "kr_compile_flags.h"
#include <QToolButton>
#include <QInputDialog>
#include <QFileDialog>
#include <QScrollArea>
#include <QGuiApplication>
#include <QClipboard>
#include <QMenu>
#include <QSettings>

void Build_setPreset( const char* name, const std::vector<CopiedString>& commands );
bool Build_runPreset( const char* name );
bool Build_runPresetNoSave( const char* name );
std::vector<CopiedString> Build_presetNames();
std::vector<CopiedString> Build_presetCommands( const char* name );
void Map_LoadFile( const char* filename );
void Map_Free();
void MRU_AddFile( const char* str );
bool ConfirmModified( const char* title );
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
	CopiedString decompBsp;
	int decompFormat = 0; // map, map_bp
	bool decompFast = false, decompModelclip = false, decompWtf = false, decompLegacy = false;
	bool runGame = false;
	CopiedString gameExe = "SoF2MP.exe";
	CopiedString gameArgs = "+set sv_pure 0 +set developer 1 +devmap %mapname%";
};
CompileOptions g_options;

const char* const c_presetName = "KenRadiant: (Compile window)";

/// options that compile but don't work in the loaded game (greyed out in the lists); nullptr = fine
const char* Flag_unsupported( const QString& flag ){
	const char* game = GlobalRadiant().getGameDescriptionKeyValue( "q3map2_type" );
	if( !string_equal_nocase( game, "sof2" ) )
		return nullptr;
	if( flag == "-deluxe" || flag == "-deluxemode" )
		return "SOF2's renderer has no deluxemaps (no bump mapping): the extra lightmaps are wasted and can break the lightmap order";
	if( flag == "-external" || flag == "-lightmapsize" || flag == "-extlmhacksize" || flag == "-lightmapdir" )
		return "SOF2MP only reads the 128 x 128 lightmaps stored inside the .bsp";
	if( flag == "-wolf" )
		return "Wolfenstein light falloff - not how SOF2 lights are meant to look";
	if( flag == "-cpma" )
		return "vertex light mode of CPMA (a Quake 3 mod)";
	return nullptr;
}

/// the ticked entries of a stage's "all options" list
QString Compile_advanced( const CopiedString& adv ){
	QString out;
	for( const QString& line : QString( adv.c_str() ).split( ';', Qt::SkipEmptyParts ) ){
		const QStringList p = line.split( '=' );
		if( Flag_unsupported( p[0] ) != nullptr )
			continue;
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

/// a Build menu entry's / saved commands back into compile settings (game and decompile settings kept from o)
CompileOptions Options_fromCommands( const CompileOptions& o, const std::vector<CopiedString>& commands ){
	CompileOptions p;
	p.runGame = o.runGame; p.gameExe = o.gameExe; p.gameArgs = o.gameArgs;
	p.decompBsp = o.decompBsp; p.decompFormat = o.decompFormat; p.decompFast = o.decompFast;
	p.decompModelclip = o.decompModelclip; p.decompWtf = o.decompWtf; p.decompLegacy = o.decompLegacy;
	p.bsp = p.vis = p.light = false;
	p.meta = p.saveprt = p.filter = false;
	p.super = 1; p.bounce = 0;
	const auto argCount = []( const char* arg ){
		const QString a = QString( arg ).remove( '<' ).remove( '>' ).trimmed();
		return a.isEmpty() ? 0 : int( a.split( ' ', Qt::SkipEmptyParts ).size() );
	};
	for( const CopiedString& command : commands ){
		QStringList t = QProcess::splitCommand( QString( command.c_str() ) );
		t.erase( std::remove_if( t.begin(), t.end(), []( const QString& x ){ return x.contains( "[q3map2]" ) || x.contains( "[MapFile]" ) || x.contains( "[ExtraResourcePaths]" ); } ), t.end() );
		const int stage = t.contains( "-vis" ) ? 1 : t.contains( "-light" ) ? 2 : t.contains( "-convert" ) ? 3 : 0;
		if( stage == 3 )
			continue;
		QStringList extra, adv;
		bool fast = false, faster = false;
		for( int i = 0; i < t.size(); ++i ){
			const QString f = t[i];
			const auto next = [&]( auto& value ){
				if( i + 1 < t.size() ){
					using T = std::decay_t<decltype( value )>;
					if constexpr ( std::is_same_v<T, int> ) value = t[++i].toInt();
					else value = t[++i].toFloat();
				}
			};
			const KrCompileFlag* table = stage == 0 ? c_krFlags_bsp : stage == 1 ? c_krFlags_vis : c_krFlags_light;
			const std::size_t size = stage == 0 ? std::size( c_krFlags_bsp ) : stage == 1 ? std::size( c_krFlags_vis ) : std::size( c_krFlags_light );
			if( stage == 0 ){
				p.bsp = true;
				if( f == "-meta" ) { p.meta = true; continue; }
				if( f == "-patchmeta" ) { p.patchmeta = true; continue; }
				if( f == "-leaktest" ) { p.leaktest = true; continue; }
				if( f == "-onlyents" ) { p.onlyents = true; continue; }
				if( f == "-nocurves" ) { p.nocurves = true; continue; }
				if( f == "-nodetail" ) { p.nodetail = true; continue; }
				if( f == "-notjunc" ) { p.notjunc = true; continue; }
				if( f == "-samplesize" ) { next( p.samplesize ); continue; }
			}
			else if( stage == 1 ){
				p.vis = true;
				if( f == "-vis" ) continue;
				if( f == "-fast" ) { p.visFast = true; continue; }
				if( f == "-saveprt" ) { p.saveprt = true; continue; }
				if( f == "-nopassage" ) { p.nopassage = true; continue; }
			}
			else{
				p.light = true;
				if( f == "-light" ) continue;
				if( f == "-fast" ) { fast = true; continue; }
				if( f == "-faster" ) { faster = true; continue; }
				if( f == "-super" ) { next( p.super ); continue; }
				if( f == "-samples" ) { next( p.samples ); continue; }
				if( f == "-filter" ) { p.filter = true; continue; }
				if( f == "-bounce" ) { next( p.bounce ); continue; }
				if( f == "-dirty" ) { p.dirty = true; continue; }
				if( f == "-patchshadows" ) { p.patchshadows = true; continue; }
				if( f == "-shade" ) { p.shade = true; continue; }
				if( f == "-nocollapse" ) { p.nocollapse = true; continue; }
				if( f == "-nomerge" ) { p.nomerge = true; continue; }
				if( f == "-gamma" ) { next( p.gamma ); continue; }
				if( f == "-compensate" ) { next( p.compensate ); continue; }
			}
			// one of the "all options"?
			const KrCompileFlag* found = nullptr;
			for( std::size_t k = 0; k < size; ++k )
				if( f == table[k].flag )
					found = &table[k];
			if( found != nullptr ){
				QStringList values;
				for( int n = argCount( found->arg ); n > 0 && i + 1 < t.size(); --n )
					values << t[++i];
				adv << f + '=' + values.join( ' ' );
			}
			else
				extra << ( f.contains( ' ' ) ? '"' + f + '"' : f );
		}
		if( stage == 2 )
			p.quality = faster ? 0 : fast ? 1 : 2;
		CopiedString& ex = stage == 0 ? p.extraBsp : stage == 1 ? p.extraVis : p.extraLight;
		CopiedString& ad = stage == 0 ? p.advBsp : stage == 1 ? p.advVis : p.advLight;
		ex = extra.join( ' ' ).toLatin1().constData();
		ad = ( adv.join( ';' ) + ( adv.isEmpty() ? "" : ";" ) ).toLatin1().constData();
	}
	return p;
}

/* compile settings per map: the last ones used for a map come back when it is opened again (the live light preview
   follows them too: a map compiled with -brightness / -gamma must not be previewed with another map's settings) */
QString Compile_mapKey(){
	return QFileInfo( Map_Name( g_map ) ).absoluteFilePath().toLower();
}
QSettings& Compile_mapSettings(){
	static QSettings settings( QString( SettingsPath_get() ) + "kr_compile_maps.ini", QSettings::IniFormat );
	return settings;
}
bool g_restoring = false;
void Compile_rememberForMap(){
	if( g_restoring || Map_Unnamed( g_map ) )
		return;
	std::vector<CopiedString> commands;
	Compile_commandsText( g_options, &commands );
	QStringList lines;
	for( const CopiedString& c : commands )
		lines << c.c_str();
	QSettings& st = Compile_mapSettings();
	st.beginGroup( "maps" );
	st.setValue( Compile_mapKey().replace( '/', '|' ), lines );
	st.endGroup();
}
void Compile_dialogReload();
/// the map changed: its own last settings, if it has any
void Compile_restoreForMap(){
	static QString last;
	const QString key = Map_Unnamed( g_map ) ? QString() : Compile_mapKey();
	if( key == last )
		return;
	last = key;
	if( key.isEmpty() )
		return;
	QSettings& st = Compile_mapSettings();
	st.beginGroup( "maps" );
	const QStringList lines = st.value( QString( key ).replace( '/', '|' ) ).toStringList();
	st.endGroup();
	if( lines.isEmpty() )
		return;
	std::vector<CopiedString> commands;
	for( const QString& l : lines )
		commands.emplace_back( l.toLatin1().constData() );
	g_restoring = true;
	g_options = Options_fromCommands( g_options, commands );
	Compile_dialogReload();
	g_restoring = false;
	globalOutputStream() << "compile settings: the ones last used for this map (" << lines.join( " / " ).toLatin1().constData() << ")\n";
}

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
	QString m_lastPresetName;
	std::vector<std::function<void()>> m_loaders; // options -> widgets

	void hint( QWidget* widget, const QString& text ){
		widget->setProperty( "krHint", text );
		widget->setToolTip( text );
		widget->installEventFilter( &m_hints );
	}
	void changed(){
		Compile_rememberForMap();
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
		std::vector<std::size_t> order( N );
		for( std::size_t i = 0; i < N; ++i )
			order[i] = i;
		std::sort( order.begin(), order.end(), [&flags]( std::size_t a, std::size_t b ){ return string_less_nocase( flags[a].flag, flags[b].flag ); } );
		for( std::size_t i = 0; i < N; ++i ){
			const KrCompileFlag& f = flags[order[i]];
			auto *cell = new QWidget;
			auto *h = new QHBoxLayout( cell );
			h->setContentsMargins( 0, 0, 0, 0 );
			auto *box = new QCheckBox( f.flag );
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
			const char* unsupported = Flag_unsupported( f.flag );
			const QString help = unsupported != nullptr ? QString( "%1 - doesn't work in SOF2: %2" ).arg( f.flag, unsupported )
			                                            : QString( "%1 %2 - %3" ).arg( f.flag, f.arg, f.help );
			if( unsupported != nullptr ){
				box->setEnabled( false );
				if( value != nullptr )
					value->setEnabled( false );
				cell->setProperty( "krHint", help ); // disabled widgets get no hover events: the cell shows it
				cell->setToolTip( help );
				cell->installEventFilter( &m_hints );
			}
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
public:
	void load(){
		for( auto& f : m_loaders )
			f();
		changed();
	}
	/// a Build menu entry's commands back into the window's settings
	void loadCommands( const std::vector<CopiedString>& commands ){
		o = Options_fromCommands( o, commands );
		load();
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
			auto *fromMenu = new QPushButton( "Load..." );
			fromMenu->setAutoDefault( false );
			hint( fromMenu, "Load the settings of a Build menu entry (yours or KenRadiant's) into this window." );
			QObject::connect( fromMenu, &QPushButton::clicked, [this](){
				QStringList names;
				for( const CopiedString& n : Build_presetNames() ){
					const auto commands = Build_presetCommands( n.c_str() );
					if( std::any_of( commands.begin(), commands.end(), []( const CopiedString& c ){ return strstr( c.c_str(), "[q3map2]" ) != nullptr && strstr( c.c_str(), "-convert" ) == nullptr; } ) )
						names << n.c_str();
				}
				bool ok = false;
				const QString name = QInputDialog::getItem( this, "Load from the Build menu", "Build menu entry:", names, 0, false, &ok );
				if( !ok || name.isEmpty() )
					return;
				loadCommands( Build_presetCommands( name.toLatin1().constData() ) );
				m_lastPresetName = name;
				globalOutputStream() << "Compile: loaded '" << name.toLatin1().constData() << "'\n";
			} );
			row->addWidget( fromMenu );
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
		// each tab: the options sorted by their q3map2 name, "-name  (what it is)"
		{
			QGridLayout* g = page( "BSP" );
			int n = 0;
			const auto at = [&n]( int& row, int& col ){ row = n / 3; col = n % 3; ++n; };
			int r, c;
			at( r, c ); check( g, r, c, "-leaktest  (stop on leaks)", o.leaktest, "Stop when the map leaks instead of writing a broken .bsp. The pointfile (File > Pointfile) shows the way to the hole." );
			at( r, c ); check( g, r, c, "-meta  (meta surfaces)", o.meta, "Merges faces into big triangle surfaces per shader: faster in game, better lightmap packing. Keep it on." );
			at( r, c ); check( g, r, c, "-nocurves  (no patches)", o.nocurves, "Leaves all patches out: a quick test of the brush work only." );
			at( r, c ); check( g, r, c, "-nodetail  (no detail)", o.nodetail, "Leaves detail brushes out: shows what the structural hull looks like (testing)." );
			at( r, c ); check( g, r, c, "-notjunc  (no T-junction fix)", o.notjunc, "Skips fixing T-junctions. Faster, but sparkles / cracks can appear between faces. Testing only." );
			at( r, c ); check( g, r, c, "-onlyents  (entities only)", o.onlyents, "Only rewrites the entities in the existing .bsp (moved light, changed key). Seconds. Skips VIS; tick LIGHT if lights moved." );
			at( r, c ); check( g, r, c, "-patchmeta  (curves to meshes)", o.patchmeta, "Patches become fixed triangle meshes that merge and light like brushes. Off = normal curves." );
			at( r, c ); spin( g, r, c, "-samplesize  (lightmap texel)", o.samplesize, 0, 128, "default 16", "World units per lightmap pixel for the whole map. Lower = sharper shadows, bigger lightmaps, slower light (8 sharp, 32 soft)." );
			const int row = n / 3 + 1;
			text( g, row, "Extra options", o.extraBsp, "Anything else for the BSP stage, as on the command line." );
			advanced( g, row + 1, c_krFlags_bsp, o.advBsp );
		}
		{
			QGridLayout* g = page( "VIS" );
			check( g, 0, 0, "-fast  (rough, quick)", o.visFast, "Rough visibility in a fraction of the time. For testing; use full VIS for a release." );
			check( g, 0, 1, "-nopassage  (old method)", o.nopassage, "Uses the older, slower vis method. Only if passage vis gives you wrong visibility." );
			check( g, 0, 2, "-saveprt  (keep portals)", o.saveprt, "Keeps the .prt file to look at the portals (Plugins > Prtview) when placing hint brushes." );
			text( g, 1, "Extra options", o.extraVis, "Anything else for the VIS stage." );
			advanced( g, 2, c_krFlags_vis, o.advVis );
		}
		{
			QGridLayout* g = page( "LIGHT" );
			int n = 0;
			const auto at = [&n]( int& row, int& col ){ row = n / 3; col = n % 3; ++n; };
			int r, c;
			at( r, c ); spin( g, r, c, "-bounce  (light bounces)", o.bounce, 0, 8, "off", "Light reflects off surfaces this many times and picks up their colour: fills rooms lit only through a door or window. Each bounce costs about one direct pass." );
			at( r, c ); dspin( g, r, c, "-compensate", o.compensate, "Divides the lightmap brightness (1 = unchanged). With gamma, keeps bright spots from clipping." );
			at( r, c ); check( g, r, c, "-dirty  (corner dirt)", o.dirty, "Darkens corners, creases and where objects meet the floor (ambient occlusion). Looks grounded; adds time." );
			at( r, c ); combo( g, r, c, "-fast / -faster  (speed)", o.quality, { "-faster", "-fast", "full" }, { 0, 1, 2 },
			                   "-faster: very rough, light ends abruptly - quick looks only. -fast: what most maps ship with. full (neither): most accurate, slowest." );
			at( r, c ); check( g, r, c, "-filter  (smooth lightmaps)", o.filter, "Blurs the lightmaps slightly to hide pixel steps on shadow edges. Cheap, usually on." );
			at( r, c ); dspin( g, r, c, "-gamma", o.gamma, "Above 1 brightens dark and mid tones (1 = unchanged). The Sun Calibrator previews it." );
			at( r, c ); check( g, r, c, "-nocollapse  (keep lightmaps apart)", o.nocollapse, "Keeps identical lightmaps separate. Only for lightmap debugging." );
			at( r, c ); check( g, r, c, "-nomerge  (no merge after light)", o.nomerge, "This q3map2 merges surfaces sharing a lightmap after -light (always, it makes the map faster). Turn on only if you see seams or wrong light on merged surfaces." );
			at( r, c ); check( g, r, c, "-patchshadows  (curves cast shadows)", o.patchshadows, "q3map2 lets light pass through patches unless this is on. Arches, pipes, curved roofs." );
			at( r, c ); spin( g, r, c, "-samples  (adaptive)", o.samples, 0, 4, "off", "Extra samples only where the light changes (shadow edges). Like -super but cheaper on flat areas." );
			at( r, c ); check( g, r, c, "-shade  (smooth shading)", o.shade, "Phong shading: light flows smoothly over round brush work and models instead of faceting." );
			at( r, c ); combo( g, r, c, "-super  (supersample)", o.super, { "1", "2", "4" }, { 1, 2, 4 }, "Each lightmap pixel from N x N samples: smoother shadow edges. Costs N x N time. 2 is a good final value." );
			const int row = n / 3 + 1;
			text( g, row, "Extra options", o.extraLight, "Anything else for the LIGHT stage, e.g. -samplesize 8." );
			advanced( g, row + 1, c_krFlags_light, o.advLight );
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
			QGridLayout* g = page( "Decompile" );
			int r = 0;
			auto *file = text( g, r++, ".bsp to decompile", o.decompBsp, "Any compiled map. q3map2 writes <name>_converted.map next to it (brushes, patches, entities, misc_models), "
			                   "with the texture alignment worked back out of the .bsp." );
			auto *browse = new QPushButton( "Browse..." );
			browse->setAutoDefault( false );
			QObject::connect( browse, &QPushButton::clicked, [this, file](){
				QString start = QString( o.decompBsp.c_str() );
				if( start.isEmpty() )
					start = QString( EnginePath_get() );
				const QString f = QFileDialog::getOpenFileName( this, "Decompile .bsp", start, "Compiled maps (*.bsp);;All files (*)" );
				if( f.isEmpty() )
					return;
				file->setText( f );
				o.decompBsp = f.toLatin1().constData();
			} );
			g->addWidget( browse, r++, 1, Qt::AlignLeft );
			combo( g, r, 0, "-format", o.decompFormat, { "map (Quake 3 / SOF2)", "map_bp (brush primitives)" }, { 0, 1 },
			       "map: the normal SOF2 .map format KenRadiant uses. map_bp: brush primitives texture format." );
			check( g, r, 1, "-fast  (no texture alignment)", o.decompFast, "Much quicker, but textures are not aligned like in the .bsp. For a quick look at the layout." );
			check( g, r++, 2, "-legacydecompile  (old method)", o.decompLegacy, "The original q3map2 decompiler instead of this q3map2's 1:1 one. Only if the new one fails on a map." );
			check( g, r, 0, "-modelclip  (model clip brushes)", o.decompModelclip, "Also decompile the clip brushes q3map2 made for models (skipped by default, they come back when you compile)." );
			check( g, r++, 1, "-wtf  (mark failed alignment)", o.decompWtf, "Faces whose texture alignment couldn't be worked out get common/WTF, so you can find and fix them." );
			auto *row = new QHBoxLayout;
			auto *run = new QPushButton( "Decompile" );
			run->setAutoDefault( false );
			auto *open = new QPushButton( "Open the result" );
			open->setAutoDefault( false );
			hint( run, "Runs q3map2 -convert; progress in the console. The open map is not touched or saved." );
			hint( open, "Opens <name>_converted.map (asks to save your current map first)." );
			QObject::connect( run, &QPushButton::clicked, [this](){
				if( o.decompBsp.empty() || !QFileInfo::exists( o.decompBsp.c_str() ) ){
					QMessageBox::information( this, "Decompile", "Choose a .bsp file first." );
					return;
				}
				QString c = QString( "[q3map2] -convert -format %1 -readbsp" ).arg( o.decompFormat == 1 ? "map_bp" : "map" );
				if( o.decompFast ) c += " -fast";
				if( o.decompLegacy ) c += " -legacydecompile";
				if( o.decompModelclip ) c += " -modelclip";
				if( o.decompWtf ) c += " -wtf";
				c += QString( " \"%1\"" ).arg( QString( o.decompBsp.c_str() ).replace( '\\', '/' ) );
				Build_setPreset( "KenRadiant: (Decompile)", { CopiedString( c.toLatin1().constData() ) } );
				Build_runPresetNoSave( "KenRadiant: (Decompile)" );
			} );
			QObject::connect( open, &QPushButton::clicked, [this](){
				const QString result = QString( o.decompBsp.c_str() ).replace( QRegularExpression( "\\.bsp$", QRegularExpression::CaseInsensitiveOption ), "" ) + "_converted.map";
				if( !QFileInfo::exists( result ) ){
					QMessageBox::information( this, "Decompile", "No " + result + " yet - press Decompile and wait for it to finish." );
					return;
				}
				if( ConfirmModified( "Open Map" ) ){ // like File > Open: the old map goes first
					MRU_AddFile( result.toLatin1().constData() );
					Map_Free();
					Map_LoadFile( result.toLatin1().constData() );
				}
			} );
			row->addWidget( run );
			row->addWidget( open );
			row->addStretch();
			g->addLayout( row, r++, 0, 1, 3 );
			g->setRowStretch( r, 1 );
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
			auto *copy = new QPushButton( "Copy" );
			copy->setToolTip( "Copy the selected lines (Ctrl+C; Shift / Ctrl + click selects more)" );
			auto *copyAll = new QPushButton( "Copy all" );
			m_problemList->setSelectionMode( QAbstractItemView::ExtendedSelection );
			m_problemList->setContextMenuPolicy( Qt::CustomContextMenu );
			const auto copyLines = [this]( bool all ){
				QStringList lines;
				for( int i = 0; i < m_problemList->count(); ++i )
					if( all || m_problemList->item( i )->isSelected() )
						lines << m_problemList->item( i )->text();
				if( !lines.isEmpty() )
					QGuiApplication::clipboard()->setText( lines.join( '\n' ) );
			};
			QObject::connect( copy, &QPushButton::clicked, [copyLines](){ copyLines( false ); } );
			QObject::connect( copyAll, &QPushButton::clicked, [copyLines](){ copyLines( true ); } );
			QObject::connect( m_problemList, &QWidget::customContextMenuRequested, [this, copyLines]( const QPoint& pos ){
				QMenu menu;
				menu.addAction( "Copy", [copyLines](){ copyLines( false ); } );
				menu.addAction( "Copy all", [copyLines](){ copyLines( true ); } );
				menu.exec( m_problemList->viewport()->mapToGlobal( pos ) );
			} );
			// Ctrl+C here copies the lines (not the editor's Copy of the selection)
			class CopyKey : public QObject
			{
				std::function<void()> m_copy;
			public:
				CopyKey( QObject* parent, std::function<void()> copy ) : QObject( parent ), m_copy( std::move( copy ) ){}
				bool eventFilter( QObject* obj, QEvent* event ) override {
					if( event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ){
						auto *k = static_cast<QKeyEvent*>( event );
						if( k->matches( QKeySequence::Copy ) ){
							if( event->type() == QEvent::KeyPress )
								m_copy();
							event->accept();
							return true;
						}
					}
					return QObject::eventFilter( obj, event );
				}
			};
			m_problemList->installEventFilter( new CopyKey( m_problemList, [copyLines](){ copyLines( false ); } ) );
			for( auto *b : { prev, next, clear, copy, copyAll } ){
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
			hint( menu, "Keeps these commands as a Build menu entry with a name you choose (the same name replaces it)." );
			QObject::connect( menu, &QPushButton::clicked, [this](){
				std::vector<CopiedString> commands;
				Compile_commandsText( o, &commands );
				if( commands.empty() )
					return;
				bool ok = false;
				const QString name = QInputDialog::getText( this, "Add to Build menu", "Name of the Build menu entry:", QLineEdit::Normal,
				                                            m_lastPresetName.isEmpty() ? QString( "KenRadiant: my settings" ) : m_lastPresetName, &ok ).trimmed();
				if( !ok || name.isEmpty() )
					return;
				m_lastPresetName = name;
				Build_setPreset( name.toLatin1().constData(), commands );
				globalOutputStream() << "Compile: Build menu entry '" << name.toLatin1().constData() << "' added\n";
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
void Compile_dialogReload(){
	if( g_compile != nullptr )
		g_compile->load();
}
void Compile_show(){
	Compile_restoreForMap();
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
	prefS( "KrDecompileBsp", o.decompBsp ); prefI( "KrDecompileFormat", o.decompFormat ); prefB( "KrDecompileFast", o.decompFast );
	prefB( "KrDecompileModelclip", o.decompModelclip ); prefB( "KrDecompileWtf", o.decompWtf ); prefB( "KrDecompileLegacy", o.decompLegacy );
	prefB( "KrCompileRunGame", o.runGame ); prefS( "KrCompileGameExe", o.gameExe ); prefS( "KrCompileGameArgs", o.gameArgs );
}

/// what the LIGHT stage will do to the lightmaps, for the live light preview: -gamma, -compensate, -brightness, -vertexscale
/// (from the Compile window's settings, its extra options and ticked "all options")
void KrCompile_lightParams( float& gamma, float& compensate, float& brightness, float& vertexScale ){
	Compile_restoreForMap(); // (a map just opened: its own settings)
	gamma = g_options.gamma; compensate = g_options.compensate; brightness = 1; vertexScale = 1;
	const QStringList t = QProcess::splitCommand( QString( g_options.extraLight.c_str() ) + Compile_advanced( g_options.advLight ) );
	for( int i = 0; i + 1 < t.size(); ++i ){
		if( t[i] == "-gamma" ) gamma = t[i + 1].toFloat();
		else if( t[i] == "-compensate" ) compensate = t[i + 1].toFloat();
		else if( t[i] == "-brightness" ) brightness = t[i + 1].toFloat();
		else if( t[i] == "-vertexscale" ) vertexScale = t[i + 1].toFloat();
	}
	if( gamma <= 0 ) gamma = 1;
	if( compensate <= 0 ) compensate = 1;
	if( brightness <= 0 ) brightness = 1;
	if( vertexScale <= 0 ) vertexScale = 1;
}

/// the Compile window's commands as text (Lighting Probe report)
QString KrCompile_commands(){
	return Compile_commandsText( g_options, nullptr );
}
