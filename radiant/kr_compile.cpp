/*
   KenRadiant: Compile window (F9) - pick the q3map2 stages and options with a plain explanation for each,
   see the exact commands, run them (map is saved first, output in the build monitor / console)
   or keep them as a Build menu entry.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QFontDatabase>
#include <QMessageBox>
#include <vector>
#include <functional>

#include "commands.h"
#include "preferencesystem.h"
#include "stringio.h"
#include "string/string.h"
#include "mainframe.h"
#include "map.h"

void Build_setPreset( const char* name, const std::vector<CopiedString>& commands );
bool Build_runPreset( const char* name );

namespace
{

/// everything the window remembers (saved in the preferences)
struct CompileOptions
{
	bool bsp = true, meta = true, patchmeta = false, leaktest = false, onlyents = false;
	int samplesize = 0;
	bool vis = true, visFast = false, saveprt = true;
	bool light = true;
	int quality = 1;  // 0 faster, 1 fast, 2 full
	int super = 2;    // 1, 2, 4
	bool filter = true, dirty = false, patchshadows = false, nomerge = false;
	int bounce = 2;
	float gamma = 1, compensate = 1;
	CopiedString extraBsp, extraLight;
};
CompileOptions g_options;

const char* const c_presetName = "KenRadiant: (Compile window)";

QString Compile_commandsText( const CompileOptions& o, std::vector<CopiedString>* commands ){
	QStringList lines;
	auto add = [&]( QString line ){
		line = line.simplified() + " \"[MapFile]\"";
		lines << line;
		if( commands != nullptr )
			commands->emplace_back( line.toLatin1().constData() );
	};
	if( o.bsp ){
		QString c = "[q3map2]";
		if( o.meta ) c += " -meta";
		if( o.patchmeta ) c += " -patchmeta";
		if( o.samplesize > 0 ) c += QString( " -samplesize %1" ).arg( o.samplesize );
		if( o.leaktest ) c += " -leaktest";
		if( o.onlyents ) c = "[q3map2] -onlyents";
		c += " " + QString( o.extraBsp.c_str() );
		add( c );
	}
	if( o.vis && !( o.bsp && o.onlyents ) ){
		QString c = "[q3map2] -vis";
		if( o.saveprt ) c += " -saveprt";
		if( o.visFast ) c += " -fast";
		add( c );
	}
	if( o.light ){
		QString c = "[q3map2] -light";
		if( o.quality == 0 ) c += " -faster";
		else if( o.quality == 1 ) c += " -fast";
		if( o.super > 1 ) c += QString( " -super %1" ).arg( o.super );
		if( o.filter ) c += " -filter";
		if( o.bounce > 0 ) c += QString( " -bounce %1" ).arg( o.bounce );
		if( o.dirty ) c += " -dirty";
		if( o.patchshadows ) c += " -patchshadows";
		if( o.nomerge ) c += " -nomerge";
		if( o.gamma != 1 ) c += QString( " -gamma %1" ).arg( o.gamma, 0, 'g', 3 );
		if( o.compensate != 1 ) c += QString( " -compensate %1" ).arg( o.compensate, 0, 'g', 3 );
		c += " " + QString( o.extraLight.c_str() );
		add( c );
	}
	return lines.join( '\n' );
}

class CompileDialog : public QDialog
{
	CompileOptions& o = g_options;
	QPlainTextEdit* m_preview;
	QLabel* m_summary;
	std::vector<std::function<void()>> m_loaders; // options -> widgets
	QGroupBox *m_bspGroup, *m_visGroup, *m_lightGroup;

	static QLabel* explanation( const QString& text ){
		auto *label = new QLabel( text );
		label->setWordWrap( true );
		label->setStyleSheet( "color: palette(mid); margin-left: 22px; margin-bottom: 4px;" );
		label->setTextInteractionFlags( Qt::TextSelectableByMouse );
		return label;
	}
	void changed(){
		m_preview->setPlainText( Compile_commandsText( o, nullptr ) );
		m_visGroup->setEnabled( !( o.bsp && o.onlyents ) );
		QStringList what;
		if( o.bsp ) what << ( o.onlyents ? "entities only" : "geometry" );
		if( o.vis && !( o.bsp && o.onlyents ) ) what << ( o.visFast ? "fast visibility" : "visibility" );
		if( o.light ) what << QString( "lighting (%1%2)" ).arg( o.quality == 0 ? "very rough" : o.quality == 1 ? "normal" : "full quality" ).arg( o.bounce > 0 ? QString( ", %1 bounces" ).arg( o.bounce ) : "" );
		m_summary->setText( what.isEmpty() ? "Nothing selected." : "Runs: " + what.join( ", then " ) + "." );
	}
	QCheckBox* check( QGridLayout* grid, int& row, const char* text, bool& value, const QString& help ){
		auto *box = new QCheckBox( text );
		QObject::connect( box, &QCheckBox::toggled, [this, &value]( bool on ){ value = on; changed(); } );
		m_loaders.push_back( [box, &value](){ box->setChecked( value ); } );
		grid->addWidget( box, row++, 0, 1, 2 );
		grid->addWidget( explanation( help ), row++, 0, 1, 2 );
		return box;
	}
	void spin( QGridLayout* grid, int& row, const char* text, int& value, int min, int max, const QString& special, const QString& help ){
		auto *box = new QSpinBox;
		box->setRange( min, max );
		if( !special.isEmpty() )
			box->setSpecialValueText( special );
		QObject::connect( box, QOverload<int>::of( &QSpinBox::valueChanged ), [this, &value]( int v ){ value = v; changed(); } );
		m_loaders.push_back( [box, &value](){ box->setValue( value ); } );
		grid->addWidget( new QLabel( text ), row, 0 );
		grid->addWidget( box, row++, 1, Qt::AlignLeft );
		grid->addWidget( explanation( help ), row++, 0, 1, 2 );
	}
	void dspin( QGridLayout* grid, int& row, const char* text, float& value, const QString& help ){
		auto *box = new QDoubleSpinBox;
		box->setRange( 0.1, 4 );
		box->setSingleStep( 0.1 );
		box->setDecimals( 2 );
		QObject::connect( box, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [this, &value]( double v ){ value = float( v ); changed(); } );
		m_loaders.push_back( [box, &value](){ box->setValue( value ); } );
		grid->addWidget( new QLabel( text ), row, 0 );
		grid->addWidget( box, row++, 1, Qt::AlignLeft );
		grid->addWidget( explanation( help ), row++, 0, 1, 2 );
	}
	void combo( QGridLayout* grid, int& row, const char* text, int& value, const QStringList& items, const std::vector<int>& values, const QString& help ){
		auto *box = new QComboBox;
		box->addItems( items );
		QObject::connect( box, QOverload<int>::of( &QComboBox::currentIndexChanged ), [this, &value, values]( int i ){ if( i >= 0 ) value = values[i]; changed(); } );
		m_loaders.push_back( [box, &value, values](){
			for( std::size_t i = 0; i < values.size(); ++i )
				if( values[i] == value )
					box->setCurrentIndex( int( i ) );
		} );
		grid->addWidget( new QLabel( text ), row, 0 );
		grid->addWidget( box, row++, 1, Qt::AlignLeft );
		grid->addWidget( explanation( help ), row++, 0, 1, 2 );
	}
	void text( QGridLayout* grid, int& row, const char* label, CopiedString& value, const QString& help ){
		auto *edit = new QLineEdit;
		QObject::connect( edit, &QLineEdit::textEdited, [this, &value]( const QString& t ){ value = t.toLatin1().constData(); changed(); } );
		m_loaders.push_back( [edit, &value](){ edit->setText( value.c_str() ); } );
		grid->addWidget( new QLabel( label ), row, 0 );
		grid->addWidget( edit, row++, 1 );
		grid->addWidget( explanation( help ), row++, 0, 1, 2 );
	}
	QGroupBox* stage( const char* title, bool& value, QGridLayout*& grid ){
		auto *group = new QGroupBox( title );
		group->setCheckable( true );
		QObject::connect( group, &QGroupBox::toggled, [this, &value]( bool on ){ value = on; changed(); } );
		m_loaders.push_back( [group, &value](){ group->setChecked( value ); } );
		grid = new QGridLayout( group );
		grid->setColumnStretch( 1, 1 );
		return group;
	}
	void load(){
		for( auto& f : m_loaders )
			f();
		changed();
	}
	void preset( int which ){
		CompileOptions p;
		p.extraBsp = o.extraBsp;
		p.extraLight = o.extraLight;
		switch( which ){
		case 0: // quick test
			p.visFast = true; p.quality = 0; p.super = 1; p.filter = false; p.bounce = 0;
			break;
		case 1: // final
			break;
		case 2: // high quality
			p.quality = 2; p.super = 2; p.bounce = 4; p.dirty = true; p.patchshadows = true;
			break;
		case 3: // re-light only
			p.bsp = false; p.vis = false;
			break;
		case 4: // entities only
			p.onlyents = true; p.vis = false; p.light = false;
			break;
		}
		o = p;
		load();
	}

public:
	CompileDialog( QWidget* parent ) : QDialog( parent, Qt::Window ){
		setWindowTitle( "Compile" );
		resize( 640, 860 );
		auto *outer = new QVBoxLayout( this );
		{
			auto *row = new QHBoxLayout;
			row->addWidget( new QLabel( "Start from:" ) );
			auto *presets = new QComboBox;
			presets->addItems( { "(choose a preset)", "Quick test - fast, rough light", "Final - good light, 2 bounces", "High quality - full light, 4 bounces, dirt",
			                     "Re-light only - keep the .bsp geometry", "Entities only - moved a light / changed keys" } );
			QObject::connect( presets, QOverload<int>::of( &QComboBox::activated ), [this, presets]( int i ){
				if( i > 0 )
					preset( i - 1 );
				presets->setCurrentIndex( 0 );
			} );
			row->addWidget( presets, 1 );
			outer->addLayout( row );
		}
		auto *scroll = new QScrollArea;
		scroll->setWidgetResizable( true );
		auto *page = new QWidget;
		auto *vbox = new QVBoxLayout( page );
		scroll->setWidget( page );
		outer->addWidget( scroll, 1 );

		QGridLayout* grid;
		int r = 0;
		m_bspGroup = stage( "1. BSP - turn the brushes into the game's geometry (.bsp)", o.bsp, grid );
		grid->addWidget( explanation( "Always needed after you change brushes, patches or models. Splits the world into areas, removes faces nobody can see "
		                              "(outside the sealed hull, caulk) and writes the .bsp. If the map leaks (a hole to the void) the result is wrong: see Leak test." ), r++, 0, 1, 2 );
		check( grid, r, "Meta surfaces  (-meta)", o.meta, "Cuts faces into triangle surfaces and merges them per shader. Fewer, bigger surfaces = faster in game and "
		       "better lightmap packing. Keep it on for SOF2." );
		check( grid, r, "Curves to meshes  (-patchmeta)", o.patchmeta, "Turns patches (curves) into fixed triangle meshes. They then merge and light like the rest; "
		       "you lose the engine's curve detail setting. Off = normal curves." );
		spin( grid, r, "Lightmap texel size  (-samplesize)", o.samplesize, 0, 128, "default (16)", "World units per lightmap pixel for the whole map. Lower = sharper shadows but "
		      "much bigger lightmaps and longer light compiles (8 is sharp, 32 is soft). Shaders can set their own with q3map_lightmapSampleSize." );
		check( grid, r, "Leak test  (-leaktest)", o.leaktest, "Stop when the map leaks instead of building a broken .bsp. After a leak: File > Pointfile shows "
		       "a red line from the inside to the hole." );
		check( grid, r, "Entities only  (-onlyents)", o.onlyents, "Only writes the entities into the existing .bsp: a moved light, a changed key, a new spawn. Takes seconds. "
		       "Geometry stays as it was compiled; add LIGHT if you moved lights. Skips VIS." );
		text( grid, r, "Extra BSP options", o.extraBsp, "Anything else for this stage, typed as on the command line (e.g. -nocurves, -nodetail)." );
		vbox->addWidget( m_bspGroup );

		r = 0;
		m_visGroup = stage( "2. VIS - work out what can be seen from where", o.vis, grid );
		grid->addWidget( explanation( "Lets the engine skip everything behind walls: the frame rate in game depends on it. Without VIS the whole map is drawn "
		                              "all the time. Uses the portals between areas (hint brushes help it, detail brushes are ignored)." ), r++, 0, 1, 2 );
		check( grid, r, "Fast  (-fast)", o.visFast, "Rough visibility in a fraction of the time. Fine for testing; use full VIS for the release." );
		check( grid, r, "Keep the portal file  (-saveprt)", o.saveprt, "Keeps <map>.prt so you can look at the portals (Plugins > Prtview) when tuning hint brushes." );
		vbox->addWidget( m_visGroup );

		r = 0;
		m_lightGroup = stage( "3. LIGHT - bake the lightmaps", o.light, grid );
		grid->addWidget( explanation( "Computes light and shadow from light entities, the sky shader's sun (q3map_sun) and sky light (q3map_skyLight), "
		                              "glowing shaders (q3map_surfaceLight) and stores it in lightmaps. The slowest stage; the Live light preview (Ctrl+L) "
		                              "approximates it in the editor." ), r++, 0, 1, 2 );
		combo( grid, r, "Speed", o.quality, { "Faster (-faster): very rough", "Fast (-fast): normal", "Full: most accurate, slowest" }, { 0, 1, 2 },
		       "How exactly light falloff is traced. Faster is for quick looks only (light ends abruptly); Fast is what most maps ship with; Full is a bit "
		       "smoother on big lights and takes much longer." );
		combo( grid, r, "Supersampling  (-super)", o.super, { "1 (off)", "2 (good)", "4 (very smooth, slow)" }, { 1, 2, 4 },
		       "Each lightmap pixel is computed from N x N samples: smoother shadow edges, less jagged stair steps. Costs N x N time." );
		check( grid, r, "Smooth the lightmaps  (-filter)", o.filter, "Blurs the lightmaps slightly to hide pixel steps on shadow edges. Cheap; usually on." );
		spin( grid, r, "Light bounces  (-bounce)", o.bounce, 0, 8, "0 (off)", "Radiosity: light reflects off surfaces this many times and takes their colour. "
		      "Fills rooms that only get light through a window or door. Each bounce costs about as much as the direct light; 2 is plenty for most maps." );
		check( grid, r, "Dirt / corner shadows  (-dirty)", o.dirty, "Darkens corners, creases and places where objects meet the floor (ambient occlusion). "
		       "Makes things look grounded; adds time." );
		check( grid, r, "Curves cast shadows  (-patchshadows)", o.patchshadows, "q3map2 lets light pass through patches (curves) unless this is on. Turn on when arches, pipes "
		       "or terrain patches should throw shadows." );
		check( grid, r, "Don't merge surfaces after lighting  (-nomerge)", o.nomerge, "This q3map2 merges drawsurfaces that share a lightmap after -light (fewer draw calls). "
		       "Turn on only if you see lightmap seams or wrong light on merged surfaces." );
		dspin( grid, r, "Gamma  (-gamma)", o.gamma, "Above 1 brightens the dark and mid tones of the lightmaps (1 = unchanged). The Sun Calibrator previews it." );
		dspin( grid, r, "Compensate  (-compensate)", o.compensate, "Divides the lightmap brightness (1 = unchanged). Use with gamma to keep bright spots from clipping." );
		text( grid, r, "Extra LIGHT options", o.extraLight, "Anything else for this stage (e.g. -samples 2, -lightmapsize 256)." );
		vbox->addWidget( m_lightGroup );
		vbox->addStretch();

		m_summary = new QLabel;
		m_summary->setWordWrap( true );
		outer->addWidget( m_summary );
		outer->addWidget( new QLabel( "Commands ([q3map2] and [MapFile] are filled in by the build system):" ) );
		m_preview = new QPlainTextEdit;
		m_preview->setReadOnly( true );
		m_preview->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
		m_preview->setMaximumHeight( 90 );
		m_preview->setLineWrapMode( QPlainTextEdit::WidgetWidth );
		outer->addWidget( m_preview );
		{
			auto *row = new QHBoxLayout;
			auto *menu = new QPushButton( "Add to Build menu" );
			menu->setAutoDefault( false );
			menu->setToolTip( QString( "Keeps these settings as '%1' at the top of the Build menu" ).arg( c_presetName ) );
			QObject::connect( menu, &QPushButton::clicked, [this](){
				std::vector<CopiedString> commands;
				Compile_commandsText( o, &commands );
				if( commands.empty() )
					return;
				Build_setPreset( c_presetName, commands );
				globalOutputStream() << "Compile: Build menu entry '" << c_presetName << "' updated\n";
			} );
			auto *run = new QPushButton( "Compile" );
			run->setDefault( true );
			run->setToolTip( "Saves the map and runs the commands; progress and errors appear in the console / build monitor" );
			QObject::connect( run, &QPushButton::clicked, [this](){ compile(); } );
			auto *close = new QPushButton( "Close" );
			close->setAutoDefault( false );
			QObject::connect( close, &QPushButton::clicked, [this](){ hide(); } );
			row->addWidget( menu );
			row->addStretch();
			row->addWidget( run );
			row->addWidget( close );
			outer->addLayout( row );
		}
		load();
	}
	void compile(){
		std::vector<CopiedString> commands;
		Compile_commandsText( o, &commands );
		if( commands.empty() ){
			QMessageBox::information( this, "Compile", "Tick at least one stage." );
			return;
		}
		if( Map_Unnamed( g_map ) ){
			QMessageBox::information( this, "Compile", "Save the map first (File > Save As): q3map2 compiles the .map file." );
			return;
		}
		Build_setPreset( c_presetName, commands );
		if( !Build_runPreset( c_presetName ) )
			globalErrorStream() << "Compile: couldn't start the build\n";
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

void KrCompile_registerCommands(){
	GlobalCommands_insert( "CompileWindow", makeCallbackF( Compile_show ), QKeySequence( "F9" ) );
	auto prefB = []( const char* name, bool& v ){ GlobalPreferenceSystem().registerPreference( name, BoolImportStringCaller( v ), BoolExportStringCaller( v ) ); };
	auto prefI = []( const char* name, int& v ){ GlobalPreferenceSystem().registerPreference( name, IntImportStringCaller( v ), IntExportStringCaller( v ) ); };
	auto prefF = []( const char* name, float& v ){ GlobalPreferenceSystem().registerPreference( name, FloatImportStringCaller( v ), FloatExportStringCaller( v ) ); };
	auto prefS = []( const char* name, CopiedString& v ){ GlobalPreferenceSystem().registerPreference( name, CopiedStringImportStringCaller( v ), CopiedStringExportStringCaller( v ) ); };
	CompileOptions& o = g_options;
	prefB( "KrCompileBsp", o.bsp ); prefB( "KrCompileMeta", o.meta ); prefB( "KrCompilePatchmeta", o.patchmeta );
	prefB( "KrCompileLeaktest", o.leaktest ); prefB( "KrCompileOnlyents", o.onlyents ); prefI( "KrCompileSamplesize", o.samplesize );
	prefB( "KrCompileVis", o.vis ); prefB( "KrCompileVisFast", o.visFast ); prefB( "KrCompileSaveprt", o.saveprt );
	prefB( "KrCompileLight", o.light ); prefI( "KrCompileQuality", o.quality ); prefI( "KrCompileSuper", o.super );
	prefB( "KrCompileFilter", o.filter ); prefB( "KrCompileDirty", o.dirty ); prefB( "KrCompilePatchshadows", o.patchshadows );
	prefB( "KrCompileNomerge", o.nomerge ); prefI( "KrCompileBounce", o.bounce );
	prefF( "KrCompileGamma", o.gamma ); prefF( "KrCompileCompensate", o.compensate );
	prefS( "KrCompileExtraBsp", o.extraBsp ); prefS( "KrCompileExtraLight", o.extraLight );
}
