/*
   KenRadiant: fog in the 3D view, like SOF2 draws it.

   The map's worldspawn "fog" key names a shader with fogparms ( r g b ) <distance>; the engine (idTech3) fades a
   surface into that colour over the distance from the camera, on a square root curve - full fog at <distance> -,
   and shows the sky as the flat fog colour. The Fog window takes the map's values and lets you change them.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <QDialog>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QSpinBox>
#include <QColorDialog>

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

bool g_enabled = true;      // show fog at all
bool g_fromMap = true;      // take the colour and the distance from the map's fog shader
int g_colour[3] = { 148, 189, 163 };
int g_depth = 4000;         // units to full fog
int g_sky = 1;              // draw the sky / background as the fog colour, like the game

CopiedString g_mapShader;   // the worldspawn fog shader, for the window
bool g_mapHas = false;
Vector3 g_mapColour( 0.58f, 0.74f, 0.64f );
float g_mapDepth = 4000;

QDialog* g_window = nullptr;
QCheckBox* g_onBox = nullptr;
QCheckBox* g_mapBox = nullptr;
QCheckBox* g_skyBox = nullptr;
QPushButton* g_colourButton = nullptr;
QSpinBox* g_depthSpin = nullptr;
QLabel* g_info = nullptr;

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

void Window_update();

} // namespace

/// the fog the 3D view should draw: false = none
bool KrFog_active( Vector3& colour, float& depth, bool& sky ){
	if( !g_enabled )
		return false;
	if( g_fromMap ){
		Fog_readMap();
		if( !g_mapHas )
			return false;
		colour = g_mapColour;
		depth = g_mapDepth;
	}
	else{
		colour = Vector3( g_colour[0] / 255.f, g_colour[1] / 255.f, g_colour[2] / 255.f );
		depth = float( std::max( g_depth, 1 ) );
	}
	sky = g_sky != 0;
	return depth > 0;
}

namespace
{

void Window_update(){
	if( g_window == nullptr )
		return;
	const QSignalBlocker b1( g_onBox ), b2( g_mapBox ), b3( g_depthSpin ), b4( g_skyBox );
	g_onBox->setChecked( g_enabled );
	g_mapBox->setChecked( g_fromMap );
	g_skyBox->setChecked( g_sky != 0 );
	Vector3 c( g_colour[0] / 255.f, g_colour[1] / 255.f, g_colour[2] / 255.f );
	int depth = g_depth;
	if( g_fromMap && g_mapHas ){
		c = g_mapColour;
		depth = int( g_mapDepth );
	}
	g_depthSpin->setValue( depth );
	g_depthSpin->setEnabled( !g_fromMap && g_enabled );
	g_colourButton->setEnabled( !g_fromMap && g_enabled );
	g_mapBox->setEnabled( g_enabled );
	g_skyBox->setEnabled( g_enabled );
	const QColor qc( byte255( c.x() ), byte255( c.y() ), byte255( c.z() ) );
	g_colourButton->setStyleSheet( QString( "background: %1; min-width: 60px;" ).arg( qc.name() ) );
	g_colourButton->setText( qc.name() );
	if( g_mapHas )
		g_info->setText( QString( "The map's fog: <b>%1</b>, colour %2, full fog at %3 units.<br>"
		                          "The game fades a surface into the fog colour over the distance from you (square root curve, "
		                          "like idTech3) and draws the sky as the flat fog colour." )
		                 .arg( g_mapShader.c_str() )
		                 .arg( QColor( byte255( g_mapColour.x() ), byte255( g_mapColour.y() ), byte255( g_mapColour.z() ) ).name() )
		                 .arg( int( g_mapDepth ) ) );
	else if( !g_mapShader.empty() )
		g_info->setText( QString( "The worldspawn \"fog\" key names <b>%1</b>, but that shader has no fogparms - "
		                          "untick \"from the map\" to set the fog here." ).arg( g_mapShader.c_str() ) );
	else
		g_info->setText( "This map has no worldspawn \"fog\" key. Untick \"from the map\" to set the fog here, "
		                 "or add the key (e.g. textures/threewave/mp_3w3fog) for the game to show fog." );
}

void Fog_changed(){
	Window_update();
	KrLightPreview_redraw();
}

void Fog_show(){
	Fog_readMap( true );
	if( g_window == nullptr ){
		g_window = new QDialog( MainFrame_getWindow(), Qt::Window );
		g_window->setWindowTitle( "Fog" );
		auto *v = new QVBoxLayout( g_window );
		g_onBox = new QCheckBox( "Show the fog in the 3D view" );
		g_onBox->setToolTip( "The game fades everything into the fog colour with distance. Off: the 3D view as before." );
		QObject::connect( g_onBox, &QCheckBox::toggled, []( bool on ){ g_enabled = on; Fog_changed(); } );
		v->addWidget( g_onBox );

		g_mapBox = new QCheckBox( "Use the map's fog shader" );
		g_mapBox->setToolTip( "The colour and distance of the shader named by the worldspawn \"fog\" key." );
		QObject::connect( g_mapBox, &QCheckBox::toggled, []( bool on ){ g_fromMap = on; Fog_changed(); } );
		v->addWidget( g_mapBox );

		auto *grid = new QGridLayout;
		grid->addWidget( new QLabel( "Colour" ), 0, 0 );
		g_colourButton = new QPushButton;
		QObject::connect( g_colourButton, &QPushButton::clicked, [](){
			const QColor c = QColorDialog::getColor( QColor( g_colour[0], g_colour[1], g_colour[2] ), g_window, "Fog colour" );
			if( c.isValid() ){
				g_colour[0] = c.red(); g_colour[1] = c.green(); g_colour[2] = c.blue();
				Fog_changed();
			}
		} );
		grid->addWidget( g_colourButton, 0, 1 );
		grid->addWidget( new QLabel( "Full fog at" ), 1, 0 );
		g_depthSpin = new QSpinBox;
		g_depthSpin->setRange( 64, 65536 );
		g_depthSpin->setSingleStep( 128 );
		g_depthSpin->setSuffix( " units" );
		g_depthSpin->setToolTip( "The distance at which a surface is completely fogged (fogparms' second value)." );
		QObject::connect( g_depthSpin, QOverload<int>::of( &QSpinBox::valueChanged ), []( int d ){ g_depth = d; Fog_changed(); } );
		grid->addWidget( g_depthSpin, 1, 1 );
		grid->setColumnStretch( 2, 1 );
		v->addLayout( grid );

		g_skyBox = new QCheckBox( "Sky and background in the fog colour (like the game)" );
		QObject::connect( g_skyBox, &QCheckBox::toggled, []( bool on ){ g_sky = on ? 1 : 0; Fog_changed(); } );
		v->addWidget( g_skyBox );

		auto *copy = new QPushButton( "Copy the map's values here" );
		copy->setToolTip( "Starts from the map's fog, then change it above without touching the shader." );
		QObject::connect( copy, &QPushButton::clicked, [](){
			if( !g_mapHas )
				return;
			g_colour[0] = byte255( g_mapColour.x() ); g_colour[1] = byte255( g_mapColour.y() ); g_colour[2] = byte255( g_mapColour.z() );
			g_depth = int( g_mapDepth );
			g_fromMap = false;
			Fog_changed();
		} );
		v->addWidget( copy );

		g_info = new QLabel;
		g_info->setWordWrap( true );
		g_info->setStyleSheet( "color: gray;" );
		v->addWidget( g_info );
		g_window->setMinimumWidth( 520 );
	}
	Window_update();
	g_window->adjustSize(); // (the note at the bottom is only filled in by Window_update)
	g_window->show();
	g_window->raise();
}

} // namespace

void KrFog_registerCommands(){
	GlobalCommands_insert( "KrFogWindow", makeCallbackF( Fog_show ) );
	auto prefB = []( const char* name, bool& v ){ GlobalPreferenceSystem().registerPreference( name, BoolImportStringCaller( v ), BoolExportStringCaller( v ) ); };
	auto prefI = []( const char* name, int& v ){ GlobalPreferenceSystem().registerPreference( name, IntImportStringCaller( v ), IntExportStringCaller( v ) ); };
	prefB( "KrFogEnabled", g_enabled );
	prefB( "KrFogFromMap", g_fromMap );
	prefI( "KrFogSky", g_sky );
	prefI( "KrFogRed", g_colour[0] ); prefI( "KrFogGreen", g_colour[1] ); prefI( "KrFogBlue", g_colour[2] );
	prefI( "KrFogDepth", g_depth );
}
