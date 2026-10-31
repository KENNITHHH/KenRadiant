/*
   KenRadiant: fog in the 3D view, like SOF2 draws it.

   The map's worldspawn "fog" key names a shader with fogparms ( r g b ) <distance>; the engine (idTech3) fades a
   surface into that colour over the distance in front of you (square root curve) and shows the sky as the flat fog
   colour. The Fog window takes the map's values, can try any other fog shader of the game, or a colour and distance
   set by hand.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <QDialog>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QColorDialog>
#include <vector>

#include "gtkutil/guisettings.h"
#include "ientity.h"
#include "ishaders.h"
#include "iscenegraph.h"
#include "scenelib.h"
#include "string/string.h"
#include "stringio.h"
#include "commands.h"
#include "preferencesystem.h"
#include "math/vector.h"
#include "mainframe.h"

void KrLightPreview_redraw();

namespace
{

inline int byte255( float v ){ return int( v * 255.f + 0.5f ); }

/* where the fog comes from */
enum Mode { eMap = 0, eShader = 1, eCustom = 2 };

bool g_enabled = true;      // show fog at all
int g_mode = eMap;
int g_colour[3] = { 148, 189, 163 };   // eCustom
int g_depth = 4000;
int g_sky = 1;              // draw the sky / background as the fog colour, like the game
CopiedString g_pick;        // eShader: the chosen fog shader
float g_customFrom = 4000;  // eCustom: the distance the fog it was started from really has (the Reset button)

CopiedString g_mapShader;   // the worldspawn "fog" key
bool g_mapHas = false;
Vector3 g_mapColour( 0.58f, 0.74f, 0.64f );
float g_mapDepth = 4000;

struct FogShader{ CopiedString name; Vector3 colour; float depth; };
std::vector<FogShader> g_list;   // every shader of the game with fogparms
bool g_listRead = false;

QDialog* g_window = nullptr;
QCheckBox* g_onBox = nullptr;
QCheckBox* g_skyBox = nullptr;
QComboBox* g_combo = nullptr;
QPushButton* g_colourButton = nullptr;
QPushButton* g_resetButton = nullptr;
QSlider* g_depthSlider = nullptr;
QSpinBox* g_depthSpin = nullptr;
QLabel* g_info = nullptr;
QColorDialog* g_picker = nullptr;

void Window_update();

/// the worldspawn "fog" shader's fogparms (called once per 3D frame: the shader is only looked up when the key changes)
void Fog_readMap( bool force = false ){
	CopiedString name;
	Scene_forEachEntity( [&name]( scene::Instance& instance ){
		const Entity* e = Node_getEntity( instance.path().top() );
		if( e != nullptr && string_equal( e->getClassName(), "worldspawn" ) )
			name = e->getKeyValue( "fog" );
	} );
	if( !force && string_equal( name.c_str(), g_mapShader.c_str() ) )
		return; // unchanged
	g_mapShader = name;
	g_mapHas = false;
	if( name.empty() )
		return;
	IShader* shader = GlobalShaderSystem().getShaderForName( name.c_str() );
	float rgb[3], depth = 0;
	if( shader->krFogParms( rgb, depth ) && depth > 0 ){
		g_mapHas = true;
		g_mapColour = Vector3( rgb[0], rgb[1], rgb[2] );
		g_mapDepth = depth;
	}
	shader->DecRef();
}

/// every fog shader of the game, for the list (the shaders are already parsed: nothing is loaded here)
void Fog_readList(){
	g_list.clear();
	GlobalShaderSystem().krForEachFogShader( makeCallbackF( +[]( const char* name, const float* colour, float depth ){
		if( depth > 0 )
			g_list.push_back( { name, Vector3( colour[0], colour[1], colour[2] ), depth } );
	} ) );
	std::sort( g_list.begin(), g_list.end(), []( const FogShader& a, const FogShader& b ){ return string_less_nocase( a.name.c_str(), b.name.c_str() ); } );
	g_listRead = true;
}

const FogShader* Fog_picked(){
	for( const FogShader& f : g_list )
		if( string_equal_nocase( f.name.c_str(), g_pick.c_str() ) )
			return &f;
	return nullptr;
}

/// the colour and distance the window shows and the 3D view draws, for the current mode
bool Fog_current( Vector3& colour, float& depth ){
	if( g_mode == eMap ){
		Fog_readMap();
		if( !g_mapHas )
			return false;
		colour = g_mapColour;
		depth = g_mapDepth;
		return true;
	}
	if( g_mode == eShader ){
		if( !g_listRead )
			Fog_readList();
		if( const FogShader* f = Fog_picked() ){
			colour = f->colour;
			depth = f->depth;
			return true;
		}
		return false;
	}
	colour = Vector3( g_colour[0] / 255.f, g_colour[1] / 255.f, g_colour[2] / 255.f );
	depth = float( std::max( g_depth, 1 ) );
	return true;
}

} // namespace

/// the fog the 3D view should draw: false = none
bool KrFog_active( Vector3& colour, float& depth, bool& sky ){
	if( !g_enabled )
		return false;
	sky = g_sky != 0;
	return Fog_current( colour, depth ) && depth > 0;
}

namespace
{

void Fog_changed(){
	Window_update();
	KrLightPreview_redraw();
}

/// changing the colour or the distance means a fog of your own: keep the values the current fog has
void Fog_toCustom(){
	if( g_mode == eCustom )
		return;
	Vector3 c( 0.58f, 0.74f, 0.64f );
	float d = 4000;
	Fog_current( c, d );
	g_colour[0] = byte255( c.x() ); g_colour[1] = byte255( c.y() ); g_colour[2] = byte255( c.z() );
	g_depth = int( d );
	g_customFrom = d;
	g_mode = eCustom;
}

/// the distance the chosen fog shader (or the map's) really has, for the reset button
float Fog_sourceDepth(){
	if( g_mode == eCustom )
		return g_customFrom;
	if( g_mode == eMap )
		return g_mapHas ? g_mapDepth : 4000;
	if( const FogShader* f = Fog_picked() )
		return f->depth;
	return 4000;
}

void Window_update(){
	if( g_window == nullptr )
		return;
	const QSignalBlocker b1( g_onBox ), b2( g_combo ), b3( g_depthSpin ), b4( g_skyBox ), b5( g_depthSlider );
	g_onBox->setChecked( g_enabled );
	g_skyBox->setChecked( g_sky != 0 );

	Vector3 c( 0.58f, 0.74f, 0.64f );
	float d = 4000;
	const bool have = Fog_current( c, d );

	g_combo->setCurrentIndex( g_mode == eMap ? 0 : g_mode == eCustom ? g_combo->count() - 1 : std::max( 1, g_combo->findData( QString( g_pick.c_str() ) ) ) );
	g_depthSpin->setValue( int( d ) );
	g_depthSlider->setValue( int( d ) );
	g_resetButton->setEnabled( int( d ) != int( Fog_sourceDepth() ) );
	const QColor qc( byte255( c.x() ), byte255( c.y() ), byte255( c.z() ) );
	g_colourButton->setStyleSheet( QString( "background: %1; color: %2; min-width: 70px;" )
	                               .arg( qc.name() ).arg( qc.lightness() > 128 ? "#202020" : "#e0e0e0" ) );
	g_colourButton->setText( qc.name() );

	if( g_mode == eMap && g_mapHas )
		g_info->setText( QString( "This map's fog: <b>%1</b>.<br>"
		                          "The game fades a surface into the fog colour over the distance in front of you "
		                          "(square root curve, like idTech3) and draws the sky as the flat fog colour." )
		                 .arg( g_mapShader.c_str() ) );
	else if( g_mode == eMap && !g_mapShader.empty() )
		g_info->setText( QString( "The worldspawn \"fog\" key names <b>%1</b>, but that shader has no fogparms - "
		                          "pick another fog above, or set one yourself." ).arg( g_mapShader.c_str() ) );
	else if( g_mode == eMap )
		g_info->setText( "This map has no worldspawn \"fog\" key, so the game shows no fog in it. Pick a fog above to "
		                 "try one; to keep it, put its shader in the worldspawn \"fog\" key." );
	else if( g_mode == eShader )
		g_info->setText( QString( "Showing <b>%1</b>. The map itself is unchanged - for the game to show this fog, put "
		                          "the shader in the worldspawn \"fog\" key." ).arg( g_pick.c_str() ) );
	else
		g_info->setText( QString( "A fog of your own: <b>fogparms ( %1 %2 %3 ) %4</b> - the lines to put in a shader of "
		                          "your own if you want the game to show it. Nothing is written to the map." )
		                 .arg( g_colour[0] / 255.f, 0, 'g', 3 ).arg( g_colour[1] / 255.f, 0, 'g', 3 )
		                 .arg( g_colour[2] / 255.f, 0, 'g', 3 ).arg( g_depth ) );
	if( !have && g_enabled )
		g_info->setText( g_info->text() + "<br><b>No fog is drawn right now.</b>" );
}

void Combo_fill(){
	Fog_readMap( true );
	Fog_readList();
	const QSignalBlocker b( g_combo );
	g_combo->clear();
	g_combo->addItem( g_mapHas ? QString( "This map's fog - %1" ).arg( g_mapShader.c_str() ) : QString( "This map's fog - none" ), QString() );
	for( const FogShader& f : g_list ){
		const QColor qc( byte255( f.colour.x() ), byte255( f.colour.y() ), byte255( f.colour.z() ) );
		g_combo->addItem( QString( "%1   -   %2, %3 units" ).arg( f.name.c_str() ).arg( qc.name() ).arg( int( f.depth ) ),
		                  QString( f.name.c_str() ) );
		g_combo->setItemData( g_combo->count() - 1, qc, Qt::DecorationRole );
	}
	g_combo->addItem( "A fog of your own (set below)", QString( "\x01" ) );
}

void Colour_pick(){
	if( g_picker == nullptr ){
		g_picker = new QColorDialog( g_window );
		g_picker->setWindowTitle( "Fog colour" );
		g_picker->setOption( QColorDialog::DontUseNativeDialog ); // the Qt dialog is the one that reports every change
		// live: the 3D view follows the colour while it is being dragged
		QObject::connect( g_picker, &QColorDialog::currentColorChanged, []( const QColor& c ){
			if( !c.isValid() )
				return;
			Fog_toCustom();
			g_colour[0] = c.red(); g_colour[1] = c.green(); g_colour[2] = c.blue();
			Fog_changed();
		} );
	}
	Vector3 c( 0.58f, 0.74f, 0.64f );
	float d = 4000;
	Fog_current( c, d );
	const int before[3] = { byte255( c.x() ), byte255( c.y() ), byte255( c.z() ) };
	const int beforeMode = g_mode;
	g_picker->setCurrentColor( QColor( before[0], before[1], before[2] ) );
	if( g_picker->exec() != QDialog::Accepted ){ // Cancel: back to the colour it had
		g_mode = beforeMode;
		std::copy( before, before + 3, g_colour );
		Fog_changed();
	}
}

void Depth_set( int d ){
	Fog_toCustom();
	g_depth = d;
	Fog_changed();
}

void Fog_show(){
	if( g_window == nullptr ){
		g_window = new QDialog( MainFrame_getWindow(), Qt::Window );
		g_window->setWindowTitle( "Fog" );
		auto *v = new QVBoxLayout( g_window );
		g_onBox = new QCheckBox( "Show the fog in the 3D view" );
		g_onBox->setToolTip( "The game fades everything into the fog colour with distance. Off: the 3D view as before." );
		QObject::connect( g_onBox, &QCheckBox::toggled, []( bool on ){ g_enabled = on; Fog_changed(); } );
		v->addWidget( g_onBox );

		auto *grid = new QGridLayout;
		grid->addWidget( new QLabel( "Fog" ), 0, 0 );
		g_combo = new QComboBox;
		g_combo->setToolTip( "The map's own fog, any fog shader of the game, or one you set yourself." );
		QObject::connect( g_combo, QOverload<int>::of( &QComboBox::currentIndexChanged ), []( int i ){
			const QString data = g_combo->itemData( i ).toString();
			if( data.isEmpty() )
				g_mode = eMap;
			else if( data == "\x01" )
				Fog_toCustom();
			else{
				g_mode = eShader;
				g_pick = data.toLatin1().constData();
			}
			Fog_changed();
		} );
		grid->addWidget( g_combo, 0, 1, 1, 3 );

		grid->addWidget( new QLabel( "Colour" ), 1, 0 );
		g_colourButton = new QPushButton;
		g_colourButton->setToolTip( "fogparms' colour. The 3D view follows it while you drag in the colour picker." );
		QObject::connect( g_colourButton, &QPushButton::clicked, Colour_pick );
		grid->addWidget( g_colourButton, 1, 1 );

		grid->addWidget( new QLabel( "Full fog at" ), 2, 0 );
		g_depthSlider = new QSlider( Qt::Horizontal );
		g_depthSlider->setRange( 64, 16384 );
		g_depthSlider->setSingleStep( 64 );
		g_depthSlider->setPageStep( 512 );
		g_depthSlider->setToolTip( "The distance at which a surface is completely fogged (fogparms' second value)." );
		QObject::connect( g_depthSlider, &QSlider::valueChanged, Depth_set );
		grid->addWidget( g_depthSlider, 2, 1, 1, 2 );
		g_depthSpin = new QSpinBox;
		g_depthSpin->setRange( 1, 262144 );
		g_depthSpin->setSingleStep( 128 );
		g_depthSpin->setSuffix( " units" );
		QObject::connect( g_depthSpin, QOverload<int>::of( &QSpinBox::valueChanged ), Depth_set );
		grid->addWidget( g_depthSpin, 2, 3 );
		g_resetButton = new QPushButton( "Reset" );
		g_resetButton->setToolTip( "Back to the distance this fog shader really has." );
		QObject::connect( g_resetButton, &QPushButton::clicked, [](){
			const float d = Fog_sourceDepth();
			if( g_mode == eCustom )
				g_depth = int( d );
			Fog_changed();
		} );
		grid->addWidget( g_resetButton, 1, 3 );
		grid->setColumnStretch( 2, 1 );
		v->addLayout( grid );

		g_skyBox = new QCheckBox( "Sky and background in the fog colour (like the game)" );
		QObject::connect( g_skyBox, &QCheckBox::toggled, []( bool on ){ g_sky = on ? 1 : 0; Fog_changed(); } );
		v->addWidget( g_skyBox );

		g_info = new QLabel;
		g_info->setWordWrap( true );
		g_info->setStyleSheet( "color: gray;" );
		v->addWidget( g_info );
		g_window->setMinimumWidth( 620 );
	}
	Combo_fill();
	Window_update();
	g_window->adjustSize(); // (the note at the bottom is only filled in by Window_update)
	static const bool remembered = ( g_guiSettings.addWindow( g_window, "KenRadiant/Fog" ), true ); // where you left it, next time too
	(void)remembered;
	g_window->show();
	g_window->raise();
}

} // namespace

void KrFog_registerCommands(){
	GlobalCommands_insert( "KrFogWindow", makeCallbackF( Fog_show ) );
	auto prefB = []( const char* name, bool& v ){ GlobalPreferenceSystem().registerPreference( name, BoolImportStringCaller( v ), BoolExportStringCaller( v ) ); };
	auto prefI = []( const char* name, int& v ){ GlobalPreferenceSystem().registerPreference( name, IntImportStringCaller( v ), IntExportStringCaller( v ) ); };
	prefB( "KrFogEnabled", g_enabled );
	prefI( "KrFogMode", g_mode );
	prefI( "KrFogSky", g_sky );
	prefI( "KrFogRed", g_colour[0] ); prefI( "KrFogGreen", g_colour[1] ); prefI( "KrFogBlue", g_colour[2] );
	prefI( "KrFogDepth", g_depth );
	GlobalPreferenceSystem().registerPreference( "KrFogShader", CopiedStringImportStringCaller( g_pick ), CopiedStringExportStringCaller( g_pick ) );
}
