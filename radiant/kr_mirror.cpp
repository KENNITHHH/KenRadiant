/*
   KenRadiant: Mirror - mirror the selection (or a copy of it) across a plane you choose.

   Flip X / Y / Z mirror around the selection's centre, snapped to the grid, which is why "copy, paste, flip" rarely
   lines up: the halves end up overlapping or a unit off. Mirror puts the plane on the selection's own side by
   default, so the copy lands exactly against the original (3ds Max's Mirror tool works the same way: axis, offset,
   copy or not), with a gap / overlap you can fine-tune by the unit.

   Entities come along: origins mirror, "angle" / "angles" turn to face the mirrored way (spawn points, lights, NPCs),
   misc_models get a mirrored scale (modelscale_vec) so q3map2 compiles them mirrored.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QRadioButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QTimer>
#include <cmath>

#include "iselection.h"
#include "ientity.h"
#include "iundo.h"
#include "scenelib.h"
#include "string/string.h"
#include "stringio.h"
#include "stream/stringstream.h"
#include "commands.h"
#include "preferencesystem.h"
#include "math/vector.h"
#include "mainframe.h"
#include "select.h"
#include "grid.h"

void Scene_Clone_Selected();

namespace
{

/* settings (remembered) */
int g_axis = 0;          // 0 x, 1 y, 2 z
int g_where = 0;         // 0 + side, 1 - side, 2 centre, 3 world 0, 4 a coordinate
float g_coordinate = 0;  // g_where == 4
float g_gap = 0;         // between the halves (negative: overlap)
bool g_copy = true;      // keep the original

QDialog* g_window = nullptr;
QButtonGroup* g_axisGroup = nullptr;
QComboBox* g_whereBox = nullptr;
QDoubleSpinBox* g_coordSpin = nullptr;
QSlider* g_gapSlider = nullptr;
QDoubleSpinBox* g_gapSpin = nullptr;
QCheckBox* g_copyBox = nullptr;
QLabel* g_info = nullptr;
QPushButton* g_apply = nullptr;
QTimer* g_timer = nullptr;

const char* const c_axisName[3] = { "X", "Y", "Z" };

bool Selection_bounds( Vector3& mins, Vector3& maxs ){
	if( GlobalSelectionSystem().countSelected() == 0 )
		return false;
	Select_GetBounds( mins, maxs );
	return mins[0] <= maxs[0];
}

/// where the mirror plane is (on g_axis), without the gap
float Plane_base( const Vector3& mins, const Vector3& maxs ){
	switch( g_where ){
	case 0: return maxs[g_axis];
	case 1: return mins[g_axis];
	case 2: return ( mins[g_axis] + maxs[g_axis] ) * 0.5f;
	case 3: return 0;
	default: return g_coordinate;
	}
}

/// the plane with the gap: moving the plane by gap / 2 moves the mirrored half by the gap
float Plane( const Vector3& mins, const Vector3& maxs ){
	const float base = Plane_base( mins, maxs );
	if( g_where == 1 ) // the copy goes to the - side: a gap pushes the plane further down
		return base - g_gap * 0.5f;
	return base + g_gap * 0.5f;
}

/// "angle" / "angles" of a point entity, facing the mirrored way (mirroring is no rotation: forward is exact,
/// roll flips with the handedness)
void Entity_mirrorAngles( Entity& e, int axis ){
	if( string_equal_nocase( e.getClassName(), "misc_model" ) ) // gets a mirrored modelscale_vec instead
		return;
	const auto fmt = []( float v ){
		v = std::fmod( v, 360.f );
		if( v < 0 ) v += 360;
		if( std::fabs( v - std::round( v ) ) < 0.001f ) v = std::round( v );
		if( v >= 360 ) v -= 360;
		return StringStream<32>( v );
	};
	float yaw;
	if( string_parse_float( e.getKeyValue( "angle" ), yaw ) && axis != 2 && yaw >= 0 ) // -1 / -2: up / down, unchanged
		e.setKeyValue( "angle", fmt( axis == 0 ? 180 - yaw : -yaw ) );
	Vector3 a;
	if( string_parse_vector3( e.getKeyValue( "angles" ), a ) ){ // pitch yaw roll
		if( axis == 0 ){ a[1] = 180 - a[1]; a[2] = -a[2]; }
		else if( axis == 1 ){ a[1] = -a[1]; a[2] = -a[2]; }
		else{ a[0] = -a[0]; a[2] = -a[2]; }
		e.setKeyValue( "angles", StringStream<96>( fmt( a[0] ), ' ', fmt( a[1] ), ' ', fmt( a[2] ) ) );
	}
}

/// what is selected that does not mirror cleanly
void Selection_check( bool& terrain, bool& models ){
	terrain = models = false;
	class V : public SelectionSystem::Visitor {
		bool& m_terrain; bool& m_models;
	public:
		V( bool& t, bool& m ) : m_terrain( t ), m_models( m ){}
		void visit( scene::Instance& instance ) const override {
			const scene::Path& path = instance.path();
			if( Entity* e = Node_getEntity( path.top() ) ){
				if( !string_empty( e->getKeyValue( "alphamap" ) ) ) m_terrain = true;
				if( string_equal_nocase_n( e->getClassName(), "misc_", 5 ) && !string_empty( e->getKeyValue( "model" ) )
				 && !string_equal_nocase( e->getClassName(), "misc_model" ) )
					m_models = true;
			}
			else if( path.size() > 1 ){
				if( Entity* parent = Node_getEntity( path.parent() ) )
					if( !string_empty( parent->getKeyValue( "alphamap" ) ) ) m_terrain = true;
			}
		}
	};
	GlobalSelectionSystem().foreachSelected( V( terrain, models ) );
}

void Window_update(){
	if( g_window == nullptr )
		return;
	if( !g_window->isVisible() ){
		g_timer->stop();
		return;
	}
	const char* ax = c_axisName[g_axis];
	static int labelledAxis = -1;
	if( labelledAxis != g_axis || g_whereBox->count() == 0 ){ // the labels name the axis
		labelledAxis = g_axis;
		const QSignalBlocker b( g_whereBox );
		g_whereBox->clear();
		g_whereBox->addItem( QString( "The selection's +%1 side (the mirrored half goes next to it)" ).arg( ax ) );
		g_whereBox->addItem( QString( "The selection's -%1 side" ).arg( ax ) );
		g_whereBox->addItem( "The selection's centre (like Flip)" );
		g_whereBox->addItem( QString( "%1 = 0 (the middle of the map)" ).arg( ax ) );
		g_whereBox->addItem( QString( "A %1 coordinate" ).arg( ax ) );
		g_whereBox->setCurrentIndex( g_where );
	}
	Vector3 mins, maxs;
	const bool have = Selection_bounds( mins, maxs );
	g_coordSpin->setEnabled( g_where == 4 );
	if( g_where != 4 && have ){
		const QSignalBlocker b( g_coordSpin );
		g_coordSpin->setValue( Plane_base( mins, maxs ) );
	}
	g_apply->setEnabled( have && GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive );
	if( !have ){
		g_info->setText( "Select what you want to mirror (brushes, patches, entities)." );
		return;
	}
	const float plane = Plane( mins, maxs );
	const float lo = 2 * plane - maxs[g_axis], hi = 2 * plane - mins[g_axis];
	QString text = QString( "The selection spans %1 %2 .. %3. Mirror plane at %1 = %4, so the %5 lands at %1 %6 .. %7" )
	               .arg( ax ).arg( mins[g_axis], 0, 'g', 6 ).arg( maxs[g_axis], 0, 'g', 6 ).arg( plane, 0, 'g', 6 )
	               .arg( g_copy ? "copy" : "selection" ).arg( lo, 0, 'g', 6 ).arg( hi, 0, 'g', 6 );
	if( g_copy ){
		const float gap = std::max( lo, mins[g_axis] ) - std::min( hi, maxs[g_axis] );
		if( g_where <= 1 )
			text += g_gap == 0 ? " - touching the original, seamless." : g_gap > 0 ? QString( " - %1 units apart." ).arg( g_gap ) : QString( " - overlapping by %1 units." ).arg( -g_gap );
		else if( gap < 0 )
			text += QString( " - overlapping the original by %1 units." ).arg( -gap );
	}
	if( std::fabs( plane * 2 - std::round( plane * 2 ) ) > 0.001f )
		text += "<br><b>The plane is off the half-unit grid: the mirrored brushes will not be on whole units.</b>";
	bool terrain, models;
	Selection_check( terrain, models );
	if( terrain )
		text += "<br><b>Terrain:</b> the brushes mirror, its index map (the paint) does not - repaint the mirrored half.";
	if( models )
		text += "<br><b>Game models</b> (misc_gamemodel, breakables ...) only turn: the game can't draw a model mirrored.";
	g_info->setText( text );
}

void Mirror_apply(){
	if( GlobalSelectionSystem().Mode() != SelectionSystem::ePrimitive )
		return;
	Vector3 mins, maxs;
	if( !Selection_bounds( mins, maxs ) )
		return;
	const float plane = Plane( mins, maxs );
	UndoableCommand undo( StringStream<64>( "mirrorSelected -axis ", c_axisName[g_axis], g_copy ? " -copy" : "" ) );
	if( g_copy )
		Scene_Clone_Selected(); // the copies are selected now, the originals stay where they are
	// flip around the pivot (whatever it is), then move the result so it lies where the plane says
	Vector3 flip( 1, 1, 1 );
	flip[g_axis] = -1;
	GlobalSelectionSystem().scaleSelected( flip, false );
	Vector3 mins2, maxs2;
	Select_GetBounds( mins2, maxs2 );
	Vector3 move( 0, 0, 0 );
	move[g_axis] = ( 2 * plane - maxs[g_axis] ) - mins2[g_axis];
	if( std::fabs( move[g_axis] ) > 1e-4f )
		GlobalSelectionSystem().translateSelected( move );
	// point entities face the mirrored way
	class V : public SelectionSystem::Visitor {
	public:
		void visit( scene::Instance& instance ) const override {
			if( Entity* e = Node_getEntity( instance.path().top() ) )
				Entity_mirrorAngles( *e, g_axis );
		}
	};
	GlobalSelectionSystem().foreachSelected( V() );
	Window_update();
}

void Mirror_show(){
	if( g_window == nullptr ){
		g_window = new QDialog( MainFrame_getWindow(), Qt::Window );
		g_window->setWindowTitle( "Mirror" );
		auto *v = new QVBoxLayout( g_window );
		auto *grid = new QGridLayout;
		v->addLayout( grid );

		grid->addWidget( new QLabel( "Mirror along" ), 0, 0 );
		auto *axes = new QHBoxLayout;
		g_axisGroup = new QButtonGroup( g_window );
		for( int i = 0; i < 3; ++i ){
			auto *b = new QRadioButton( c_axisName[i] );
			b->setToolTip( i == 0 ? "Left / right in the top view" : i == 1 ? "Up / down in the top view" : "Up / down (upside down)" );
			g_axisGroup->addButton( b, i );
			axes->addWidget( b );
		}
		axes->addStretch();
		grid->addLayout( axes, 0, 1, 1, 3 );
		QObject::connect( g_axisGroup, &QButtonGroup::idClicked, []( int id ){ g_axis = id; Window_update(); } );

		grid->addWidget( new QLabel( "Mirror plane" ), 1, 0 );
		g_whereBox = new QComboBox;
		g_whereBox->setToolTip( "Where the mirror is. On one of the selection's sides the mirrored half lands right against it: "
		                        "a seamless double of the map. The centre is what Flip X / Y / Z do." );
		QObject::connect( g_whereBox, QOverload<int>::of( &QComboBox::currentIndexChanged ), []( int i ){ if( i >= 0 ){ g_where = i; Window_update(); } } );
		grid->addWidget( g_whereBox, 1, 1, 1, 3 );

		grid->addWidget( new QLabel( "At" ), 2, 0 );
		g_coordSpin = new QDoubleSpinBox;
		g_coordSpin->setRange( -131072, 131072 );
		g_coordSpin->setDecimals( 1 );
		g_coordSpin->setSingleStep( 8 );
		g_coordSpin->setToolTip( "The plane's coordinate. Type one when the plane is \"A coordinate\"." );
		QObject::connect( g_coordSpin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), []( double d ){ g_coordinate = float( d ); Window_update(); } );
		grid->addWidget( g_coordSpin, 2, 1 );

		grid->addWidget( new QLabel( "Gap" ), 3, 0 );
		g_gapSlider = new QSlider( Qt::Horizontal );
		g_gapSlider->setRange( -512, 512 );
		g_gapSlider->setToolTip( "Units between the two halves; below 0 they overlap. Fine-tune here when a ramp or bridge needs "
		                         "the halves a little apart or into each other." );
		grid->addWidget( g_gapSlider, 3, 1, 1, 2 );
		g_gapSpin = new QDoubleSpinBox;
		g_gapSpin->setRange( -8192, 8192 );
		g_gapSpin->setDecimals( 1 );
		g_gapSpin->setSingleStep( 1 );
		g_gapSpin->setSuffix( " units" );
		grid->addWidget( g_gapSpin, 3, 3 );
		QObject::connect( g_gapSlider, &QSlider::valueChanged, []( int d ){
			g_gap = float( d );
			const QSignalBlocker b( g_gapSpin );
			g_gapSpin->setValue( d );
			Window_update();
		} );
		QObject::connect( g_gapSpin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), []( double d ){
			g_gap = float( d );
			const QSignalBlocker b( g_gapSlider );
			g_gapSlider->setValue( int( std::lround( d ) ) );
			Window_update();
		} );
		auto *zero = new QPushButton( "Touching" );
		zero->setToolTip( "Gap 0: the halves meet exactly." );
		QObject::connect( zero, &QPushButton::clicked, [](){ g_gapSpin->setValue( 0 ); } );
		grid->addWidget( zero, 2, 3 );
		auto *gridStep = new QPushButton( "Grid steps" );
		gridStep->setToolTip( "The slider and arrows move by the current grid size (they move by 1 unit otherwise)." );
		gridStep->setCheckable( true );
		QObject::connect( gridStep, &QPushButton::toggled, []( bool on ){
			const float step = on ? std::max( 1.f, GetGridSize() ) : 1.f;
			g_gapSlider->setSingleStep( int( step ) );
			g_gapSlider->setPageStep( int( step * 8 ) );
			g_gapSpin->setSingleStep( step );
		} );
		grid->addWidget( gridStep, 2, 2 );
		grid->setColumnStretch( 1, 1 );

		g_copyBox = new QCheckBox( "Keep the original (mirror a copy)" );
		g_copyBox->setToolTip( "On: the mirrored half is a copy, the original stays. Off: the selection itself is mirrored." );
		QObject::connect( g_copyBox, &QCheckBox::toggled, []( bool on ){ g_copy = on; Window_update(); } );
		v->addWidget( g_copyBox );

		g_info = new QLabel;
		g_info->setWordWrap( true );
		g_info->setTextFormat( Qt::RichText );
		g_info->setMinimumHeight( 60 );
		v->addWidget( g_info );

		auto *row = new QHBoxLayout;
		g_apply = new QPushButton( "Mirror" );
		g_apply->setDefault( true );
		g_apply->setToolTip( "One undo step (Ctrl+Z). The window stays open: undo, change the gap and mirror again." );
		QObject::connect( g_apply, &QPushButton::clicked, Mirror_apply );
		row->addWidget( g_apply );
		row->addStretch();
		auto *note = new QLabel( "Point entities turn to face the mirrored way; misc_models are mirrored for q3map2 "
		                         "(sof2map can't compile mirrored models)." );
		note->setWordWrap( true );
		note->setStyleSheet( "color: gray;" );
		v->addLayout( row );
		v->addWidget( note );
		g_window->setMinimumWidth( 560 );

		g_timer = new QTimer( g_window ); // the selection changes while the window is open
		g_timer->setInterval( 300 );
		QObject::connect( g_timer, &QTimer::timeout, Window_update );
	}
	{
		const QSignalBlocker b1( g_axisGroup ), b2( g_gapSpin ), b3( g_gapSlider ), b4( g_copyBox ), b5( g_coordSpin );
		if( auto *b = g_axisGroup->button( g_axis ) ) b->setChecked( true );
		g_gapSpin->setValue( g_gap );
		g_gapSlider->setValue( int( std::lround( g_gap ) ) );
		g_copyBox->setChecked( g_copy );
		g_coordSpin->setValue( g_coordinate );
	}
	g_window->show();
	g_window->raise();
	g_timer->start();
	Window_update();
}

} // namespace

void KrMirror_registerCommands(){
	GlobalCommands_insert( "KrMirrorWindow", makeCallbackF( Mirror_show ) );
	GlobalPreferenceSystem().registerPreference( "KrMirrorAxis", IntImportStringCaller( g_axis ), IntExportStringCaller( g_axis ) );
	GlobalPreferenceSystem().registerPreference( "KrMirrorWhere", IntImportStringCaller( g_where ), IntExportStringCaller( g_where ) );
	GlobalPreferenceSystem().registerPreference( "KrMirrorGap", FloatImportStringCaller( g_gap ), FloatExportStringCaller( g_gap ) );
	GlobalPreferenceSystem().registerPreference( "KrMirrorCopy", BoolImportStringCaller( g_copy ), BoolExportStringCaller( g_copy ) );
}
