/*
   Copyright (C) 1999-2006 Id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

//
// Camera Window
//
// Leonardo Zide (leo@lokigames.com)
//

#include "camwindow.h"
#include "kr_terrain.h"
#include "modelwindow.h"
#include <optional>
#include "selection_.h"
struct testSelect_unselected_scene_point_return_t{ DoubleVector3 point; std::optional<Plane3> plane; };
std::optional<testSelect_unselected_scene_point_return_t>
testSelect_unselected_scene_point( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon );
#include <QMimeData>
#include <QDragEnterEvent>

#include "debugging/debugging.h"

#include "iscenegraph.h"
#include "irender.h"
#include "igl.h"
#include "icamera.h"
#include "cullable.h"
#include "renderable.h"
#include "preferencesystem.h"

#include "signal/signal.h"
#include "scenelib.h"
#include "render.h"
#include "math/frustum.h"

#include "gtkutil/widget.h"
#include "gtkutil/glwidget.h"
#include "gtkutil/xorrectangle.h"
#include "gtkutil/cursor.h"
#include "gtkutil/fbo.h"
#include "gtkmisc.h"
#include "selection.h"
#include "mainframe.h"
#include "preferences.h"
#include "commands.h"
#include "xywindow.h"
#include "windowobservers.h"
#include "renderstate.h"

#include "timer.h"
#include "gtkutil/widget.h"
extern bool g_krLightPreview;
ToggleItem g_lightPreview_item{ BoolExportCaller( g_krLightPreview ) };

#include <QOpenGLWidget>
#include <QElapsedTimer>
#include <QTimer>

#include <QApplication>

// https://stackoverflow.com/questions/42566421/how-to-queue-lambda-function-into-qts-event-loop/42566867#42566867
template <typename Fun> void postCall( QObject * obj, Fun && fun ) {
	// qDebug() << __FUNCTION__;
	struct Event : public QEvent {
		using F = typename std::decay<Fun>::type;
		F fun;
		Event( F && fun ) : QEvent( QEvent::None ), fun( std::move( fun ) ) {}
		Event( const F & fun ) : QEvent( QEvent::None ), fun( fun ) {}
		~Event() { fun(); }
	};
	QCoreApplication::postEvent(
			obj->thread() ? obj : qApp, new Event( std::forward<Fun>( fun ) ) );
}

class IdleDraw2 : public QObject
{
	Callback<void()> m_redraw;
	std::vector<Callback<void()>> m_funcs;
	Callback<void()> m_loopFunc;
	bool m_running{};
	bool m_queued{};
	bool m_redrawDo{};

	void invoke(){
		if( !m_running ){
			m_running = true;
			for( auto& f : m_funcs )
				f();
			m_funcs.clear();
			m_loopFunc();
			if( m_redrawDo )
				m_redraw();
		}
		else
			globalWarningStream() << "invoke() during m_running\n";

		m_running = false;
		m_queued = false;
		m_redrawDo = false;

		doLoop();
	}
public:
	IdleDraw2( const Callback<void()>& redrawCallback ) : m_redraw( redrawCallback ){}
	void queueDraw( const Callback<void()>& func, bool redrawDo ){
		if( !m_running ){
			if( std::ranges::find( m_funcs, func ) == m_funcs.cend() ){
				m_funcs.push_back( func );
				// globalOutputStream() << m_funcs.size() << " m_funcs.size()\n";
			}
			m_redrawDo |= redrawDo;
			if( !m_queued ){
				m_queued = true;
				postCall( this, [this](){ invoke(); } );
			}
		}
		else
			globalWarningStream() << "queueDraw() during m_running\n";
	}
	void startLoop( const Callback<void()>& func ){
		m_loopFunc = func;
		doLoop();
	}
	void doLoop(){
		if( m_loopFunc != Callback<void()>() )
			queueDraw( Callback<void()>(), true );
	}
	void breakLoop(){
		m_loopFunc = {};
	}
};


Signal0 g_cameraMoved_callbacks;

void AddCameraMovedCallback( const SignalHandler& handler ){
	g_cameraMoved_callbacks.connectLast( handler );
}

void CameraMovedNotify(){
	g_cameraMoved_callbacks();
}


struct camwindow_globals_private_t
{
	int m_nMoveSpeed = 500;
	int m_time_toMaxSpeed = 200;
	int m_nScrollMoveSpeed = 100;
	bool m_bZoomToPointer = true;
	float m_strafeSpeed = 1;
	float m_angleSpeed = 3;
	bool m_bCamInverseMouse = false;
	bool m_bCamDiscrete = true;
	bool m_bCubicClipping = false;
	int m_strafeMode = 3;
	bool m_bFaceWire = true;
	bool m_bFaceFill = true;
	int m_MSAA = 8;
	bool m_bShowWorkzone = false; // KenRadiant: off by default
	bool m_bShowSize = true;
};

camwindow_globals_private_t g_camwindow_globals_private;


const Matrix4 g_opengl2radiant(
	 0, 0,-1, 0,
	-1, 0, 0, 0,
	 0, 1, 0, 0,
	 0, 0, 0, 1
	);

const Matrix4 g_radiant2opengl(
	 0,-1, 0, 0,
	 0, 0, 1, 0,
	-1, 0, 0, 0,
	 0, 0, 0, 1
	);

struct camera_t;
void Camera_mouseMove( camera_t& camera, int x, int y, const QMouseEvent& event );

struct MotionDeltaValues {
	int x;
	int y;
	QMouseEvent mouseMoveEvent;
	MotionDeltaValues( int x_, int y_, QMouseEvent mouseMoveEvent_ ) :
		x( x_ ), y( y_ ), mouseMoveEvent( mouseMoveEvent_ ) {
	}
};

enum camera_draw_mode
{
	cd_wire = 0,
	cd_solid = 1,
	cd_texture = 2,
	cd_texture_plus_wire = 3,
	cd_lighting = 4
};
const int camera_draw_mode_count = 5;

struct camera_t
{
	int width, height;

	Vector3 origin;
	Vector3 angles;

	Vector3 color; // background

	Vector3 forward, right; // move matrix (TTimo: used to have up but it was not updated)
	Vector3 vup, vpn, vright; // view matrix (taken from the modelview matrix)

	Matrix4 projection;
	Matrix4 modelview;

	bool m_strafe; // true when in strafemode toggled by the ctrl-key
	bool m_strafe_forward; // true when in strafemode by ctrl-key and shift is pressed for forward strafing
	bool m_strafe_forward_invert; //silly option to invert forward strafing to support old fegs
	bool m_orbit = false;
	Vector3 m_orbit_center;
	Vector3 m_orbit_initial_pos;
	int m_orbit_offset = 0;
	int m_focus_offset = 0;

	unsigned int movementflags; // movement flags
	Timer m_keycontrol_timer;
	float m_keymove_speed_current;


	static float fieldOfView;
	static const float near_z;

	DeferredMotionDelta2 m_mouseMove;

	View* m_view;
	Callback<void()> m_update;
	IdleDraw2 m_idleDraw;

	Callback<void(const MotionDeltaValues&)> m_update_motion_freemove;

	static camera_draw_mode draw_mode;

	camera_t( View* view, const Callback<void()>& update, const Callback<void(const MotionDeltaValues&)>& update_motion_freemove ) :
		width( 0 ),
		height( 0 ),
		origin( 0, 0, 0 ),
		angles( 0, 0, 0 ),
		color( 0, 0, 0 ),
		movementflags( 0 ),
		m_keymove_speed_current( 0 ),
		m_mouseMove( [this]( int x, int y, const QMouseEvent& event ){ Camera_mouseMove( *this, x, y, event ); } ),
		m_view( view ),
		m_update( update ),
		m_idleDraw( update ),
		m_update_motion_freemove( update_motion_freemove ){
	}
};

float camera_t::fieldOfView = 100.0f;
const float camera_t::near_z = 1;
camera_draw_mode camera_t::draw_mode = cd_texture;

inline Matrix4 projection_for_camera( float near_z, float far_z, float fieldOfView, int width, int height ){
	float half_width = near_z * tan( degrees_to_radians( fieldOfView * 0.5 ) );
	const bool swap = height > width;
	if( swap )
		std::swap( width, height );
	float half_height = half_width * ( static_cast<float>( height ) / static_cast<float>( width ) );
	if( swap )
		std::swap( half_width, half_height );

	return matrix4_frustum(
	           -half_width,
	           half_width,
	           -half_height,
	           half_height,
	           near_z,
	           far_z
	       );
}

float Camera_getFarClipPlane( camera_t& camera ){
	return ( g_camwindow_globals_private.m_bCubicClipping )
	       ? pow( 2.0, ( g_camwindow_globals.m_nCubicScale + 7 ) / 2.0 )
	       : ( ( g_MaxWorldCoord - g_MinWorldCoord ) * sqrt( 3 ) );
}

void Camera_updateProjection( camera_t& camera ){
	float farClip = Camera_getFarClipPlane( camera );
	//~near_z = farClip / 4096.0f
	camera.projection = projection_for_camera( camera_t::near_z, farClip, camera_t::fieldOfView, camera.width, camera.height );

	camera.m_view->Construct( camera.projection, camera.modelview, camera.width, camera.height );
}

void Camera_updateVectors( camera_t& camera ){
	for ( int i = 0; i < 3; ++i )
	{
		camera.vright[i] = camera.modelview[( i << 2 ) + 0];
		camera.vup[i] = camera.modelview[( i << 2 ) + 1];
		camera.vpn[i] = camera.modelview[( i << 2 ) + 2];
	}
}

void Camera_updateModelview( camera_t& camera ){
	camera.modelview = g_matrix4_identity;

	// roll, pitch, yaw
	Vector3 radiant_eulerXYZ( 0, -camera.angles[CAMERA_PITCH], camera.angles[CAMERA_YAW] );

	matrix4_translate_by_vec3( camera.modelview, camera.origin );
	matrix4_rotate_by_euler_xyz_degrees( camera.modelview, radiant_eulerXYZ );
	matrix4_multiply_by_matrix4( camera.modelview, g_radiant2opengl );
	matrix4_affine_invert( camera.modelview );

	Camera_updateVectors( camera );

	camera.m_view->Construct( camera.projection, camera.modelview, camera.width, camera.height );
}


void Camera_Move_updateAxes( camera_t& camera ){
	double ya = degrees_to_radians( camera.angles[CAMERA_YAW] );

	// the movement matrix is kept 2d
	camera.forward[0] = cos( ya );
	camera.forward[1] = sin( ya );
	camera.forward[2] = 0;
	camera.right[0] = camera.forward[1];
	camera.right[1] = -camera.forward[0];
}

void Camera_Freemove_updateAxes( camera_t& camera ){
	camera.right = camera.vright;
	camera.forward = vector3_negated( camera.vpn );
}

const Vector3& Camera_getOrigin( camera_t& camera ){
	return camera.origin;
}

void Camera_setOrigin( camera_t& camera, const Vector3& origin ){
	camera.origin = origin;
	Camera_updateModelview( camera );
	camera.m_update();
	CameraMovedNotify();
}

const Vector3& Camera_getAngles( camera_t& camera ){
	return camera.angles;
}

void Camera_setAngles( camera_t& camera, const Vector3& angles ){
	camera.angles = angles;
	Camera_updateModelview( camera );
	camera.m_update();
	CameraMovedNotify();
}


void Camera_FreeMove( camera_t& camera, int dx, int dy ){
	// free strafe mode, toggled by the ctrl key with optional shift for forward movement
	if ( camera.m_strafe ) {
		const float speed = g_camwindow_globals_private.m_strafeSpeed;
		camera.origin -= camera.vright * speed * dx;
		if ( camera.m_strafe_forward )
			camera.origin += camera.vpn * speed * dy * ( camera.m_strafe_forward_invert ? 1 : -1 );
		else
			camera.origin += camera.vup * speed * dy;
	}
	else // free rotation
	{
		const float dtime = 0.0333333f;
		Vector3 angles0( camera.angles );

		if ( g_camwindow_globals_private.m_bCamInverseMouse ) {
			camera.angles[CAMERA_PITCH] -= dy * dtime * g_camwindow_globals_private.m_angleSpeed;
		}
		else{
			camera.angles[CAMERA_PITCH] += dy * dtime * g_camwindow_globals_private.m_angleSpeed;
		}

		camera.angles[CAMERA_YAW] += dx * dtime * g_camwindow_globals_private.m_angleSpeed;

		camera.angles[CAMERA_PITCH] = std::clamp( camera.angles[CAMERA_PITCH], -90.f, 90.f );

		if ( camera.angles[CAMERA_YAW] >= 360 ) {
			camera.angles[CAMERA_YAW] -= 360;
		}
		else if ( camera.angles[CAMERA_YAW] <= 0 ) {
			camera.angles[CAMERA_YAW] += 360;
		}

		if( camera.m_orbit ){
#if 0
			const Vector3 radangles( euler_degrees_to_radians( camera.angles ) );
			const Vector3 viewdir( cos( radangles[1] ) * cos( radangles[0] ),
			                       sin( radangles[1] ) * cos( radangles[0] ),
			                       sin( radangles[0] )
			                     );
			const float len = vector3_length( camera.origin - camera.m_orbit_center );
			camera.origin = camera.m_orbit_center - viewdir * len;
#else
			Matrix4 rot0 = matrix4_rotation_for_euler_xyz_degrees( Vector3( 0, -angles0[CAMERA_PITCH], angles0[CAMERA_YAW] ) );
			matrix4_affine_invert( rot0 );
			Matrix4 rot1 = matrix4_rotation_for_euler_xyz_degrees( Vector3( 0, -camera.angles[CAMERA_PITCH], camera.angles[CAMERA_YAW] ) );
			matrix4_multiply_by_matrix4( rot1, rot0 );

			Matrix4 mat = matrix4_translation_for_vec3( camera.m_orbit_center );
			matrix4_multiply_by_matrix4( mat, rot1 );
			matrix4_translate_by_vec3( mat, -camera.m_orbit_center );

			camera.origin = matrix4_transformed_point( mat, camera.origin );
#endif
		}
	}

	Camera_updateModelview( camera );
	Camera_Freemove_updateAxes( camera );
}


void Camera_mouseMove( camera_t& camera, int x, int y, const QMouseEvent& event ){
	//globalOutputStream() << "mousemove... ";
	Camera_FreeMove( camera, -x, -y );
	camera.m_update_motion_freemove( MotionDeltaValues( x, y, event ) );
	CameraMovedNotify();
}

const unsigned int MOVE_NONE = 0;
const unsigned int MOVE_FORWARD = 1 << 0;
const unsigned int MOVE_BACK = 1 << 1;
const unsigned int MOVE_ROTRIGHT = 1 << 2;
const unsigned int MOVE_ROTLEFT = 1 << 3;
const unsigned int MOVE_STRAFERIGHT = 1 << 4;
const unsigned int MOVE_STRAFELEFT = 1 << 5;
const unsigned int MOVE_UP = 1 << 6;
const unsigned int MOVE_DOWN = 1 << 7;
const unsigned int MOVE_PITCHUP = 1 << 8;
const unsigned int MOVE_PITCHDOWN = 1 << 9;
const unsigned int MOVE_FOCUS = 1 << 10;
const unsigned int MOVE_ALL = MOVE_FORWARD | MOVE_BACK | MOVE_ROTRIGHT | MOVE_ROTLEFT | MOVE_STRAFERIGHT | MOVE_STRAFELEFT | MOVE_UP | MOVE_DOWN | MOVE_PITCHUP | MOVE_PITCHDOWN | MOVE_FOCUS;

Vector3 Camera_getFocusPos( camera_t& camera );

void Cam_KeyControl( camera_t& camera, float dtime ){
	// Update angles
	const float dangle = 25.f * dtime * g_camwindow_globals_private.m_angleSpeed;
	if ( camera.movementflags & MOVE_ROTLEFT ) {
		camera.angles[CAMERA_YAW] += dangle;
	}
	if ( camera.movementflags & MOVE_ROTRIGHT ) {
		camera.angles[CAMERA_YAW] -= dangle;
	}
	if ( camera.movementflags & MOVE_PITCHUP ) {
		camera.angles[CAMERA_PITCH] = std::min( camera.angles[CAMERA_PITCH] + dangle, 90.f );
	}
	if ( camera.movementflags & MOVE_PITCHDOWN ) {
		camera.angles[CAMERA_PITCH] = std::max( camera.angles[CAMERA_PITCH] - dangle, -90.f );
	}

	Camera_updateModelview( camera );
	Camera_Freemove_updateAxes( camera );

	if( g_camwindow_globals_private.m_time_toMaxSpeed == 0 ){
		camera.m_keymove_speed_current = g_camwindow_globals_private.m_nMoveSpeed;
	}
	else{ /* accelerate */
		camera.m_keymove_speed_current = std::min( camera.m_keymove_speed_current
		                                           + g_camwindow_globals_private.m_nMoveSpeed * dtime
		                                           / g_camwindow_globals_private.m_time_toMaxSpeed * 1000,
		                                           static_cast<float>( g_camwindow_globals_private.m_nMoveSpeed ) );
	}
	const float dpos = dtime * camera.m_keymove_speed_current;
	// Update position
	if ( camera.movementflags & MOVE_FORWARD ) {
		camera.origin += camera.forward * dpos;
	}
	if ( camera.movementflags & MOVE_BACK ) {
		camera.origin -= camera.forward * dpos;
	}
	if ( camera.movementflags & MOVE_STRAFERIGHT ) {
		camera.origin += camera.right * dpos;
	}
	if ( camera.movementflags & MOVE_STRAFELEFT ) {
		camera.origin -= camera.right * dpos;
	}
	if ( camera.movementflags & MOVE_UP ) {
		camera.origin += g_vector3_axis_z * dpos;
	}
	if ( camera.movementflags & MOVE_DOWN ) {
		camera.origin -= g_vector3_axis_z * dpos;
	}
	if ( camera.movementflags & MOVE_FOCUS ) {
		camera.origin = Camera_getFocusPos( camera );
	}

	Camera_updateModelview( camera );
}

void Camera_keyMove( camera_t& camera ){
	//globalOutputStream() << "keymove... ";
	float time_seconds = camera.m_keycontrol_timer.elapsed_sec();
	if( time_seconds == 0 ) /* some reasonable move at the very start */
		time_seconds = 0.008f;
	camera.m_keycontrol_timer.start();

	Cam_KeyControl( camera, time_seconds );

	CameraMovedNotify();
}

void Camera_setMovementFlags( camera_t& camera, unsigned int mask ){
	if ( ( ~camera.movementflags & mask ) != 0 && camera.movementflags == 0 ) {
		camera.m_idleDraw.startLoop( ReferenceCaller<camera_t, void(), Camera_keyMove>( camera ) );
		camera.m_keycontrol_timer.start();
		camera.m_keymove_speed_current = 0;
	}
	camera.movementflags |= mask;
}
void Camera_clearMovementFlags( camera_t& camera, unsigned int mask ){
	if ( ( camera.movementflags & ~mask ) == 0 && camera.movementflags != 0 ) {
		camera.m_idleDraw.breakLoop();
	}
	camera.movementflags &= ~mask;
}

void Camera_MoveForward_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_FORWARD );
}
void Camera_MoveForward_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_FORWARD );
}
void Camera_MoveBack_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_BACK );
}
void Camera_MoveBack_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_BACK );
}

void Camera_MoveLeft_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_STRAFELEFT );
}
void Camera_MoveLeft_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_STRAFELEFT );
}
void Camera_MoveRight_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_STRAFERIGHT );
}
void Camera_MoveRight_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_STRAFERIGHT );
}

void Camera_MoveUp_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_UP );
}
void Camera_MoveUp_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_UP );
}
void Camera_MoveDown_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_DOWN );
}
void Camera_MoveDown_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_DOWN );
}

void Camera_RotateLeft_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_ROTLEFT );
}
void Camera_RotateLeft_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_ROTLEFT );
}
void Camera_RotateRight_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_ROTRIGHT );
}
void Camera_RotateRight_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_ROTRIGHT );
}

void Camera_PitchUp_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_PITCHUP );
}
void Camera_PitchUp_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_PITCHUP );
}
void Camera_PitchDown_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_PITCHDOWN );
}
void Camera_PitchDown_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_PITCHDOWN );
}

void Camera_Focus_KeyDown( camera_t& camera ){
	Camera_setMovementFlags( camera, MOVE_FOCUS );
	camera.m_focus_offset = 0;
}
void Camera_Focus_KeyUp( camera_t& camera ){
	Camera_clearMovementFlags( camera, MOVE_FOCUS );
	camera.m_focus_offset = 0;
}

typedef ReferenceCaller<camera_t, void(), &Camera_MoveForward_KeyDown> FreeMoveCameraMoveForwardKeyDownCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveForward_KeyUp> FreeMoveCameraMoveForwardKeyUpCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveBack_KeyDown> FreeMoveCameraMoveBackKeyDownCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveBack_KeyUp> FreeMoveCameraMoveBackKeyUpCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveLeft_KeyDown> FreeMoveCameraMoveLeftKeyDownCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveLeft_KeyUp> FreeMoveCameraMoveLeftKeyUpCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveRight_KeyDown> FreeMoveCameraMoveRightKeyDownCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveRight_KeyUp> FreeMoveCameraMoveRightKeyUpCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveUp_KeyDown> FreeMoveCameraMoveUpKeyDownCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveUp_KeyUp> FreeMoveCameraMoveUpKeyUpCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveDown_KeyDown> FreeMoveCameraMoveDownKeyDownCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_MoveDown_KeyUp> FreeMoveCameraMoveDownKeyUpCaller;

typedef ReferenceCaller<camera_t, void(), &Camera_Focus_KeyDown> FreeMoveCameraFocusKeyDownCaller;
typedef ReferenceCaller<camera_t, void(), &Camera_Focus_KeyUp> FreeMoveCameraFocusKeyUpCaller;

#define SPEED_MOVE 32
#define SPEED_TURN 22.5
#define CAM_MIN_SPEED 50
#define CAM_MAX_SPEED 9999
#define CAM_SPEED_STEP 250

void Camera_MoveForward_Discrete( camera_t& camera ){
	Camera_Move_updateAxes( camera );
	Camera_setOrigin( camera, vector3_added( Camera_getOrigin( camera ), vector3_scaled( camera.forward, SPEED_MOVE ) ) );
}
void Camera_MoveBack_Discrete( camera_t& camera ){
	Camera_Move_updateAxes( camera );
	Camera_setOrigin( camera, vector3_added( Camera_getOrigin( camera ), vector3_scaled( camera.forward, -SPEED_MOVE ) ) );
}

void Camera_MoveUp_Discrete( camera_t& camera ){
	Vector3 origin( Camera_getOrigin( camera ) );
	origin[2] += SPEED_MOVE;
	Camera_setOrigin( camera, origin );
}
void Camera_MoveDown_Discrete( camera_t& camera ){
	Vector3 origin( Camera_getOrigin( camera ) );
	origin[2] -= SPEED_MOVE;
	Camera_setOrigin( camera, origin );
}

void Camera_MoveLeft_Discrete( camera_t& camera ){
	Camera_Move_updateAxes( camera );
	Camera_setOrigin( camera, vector3_added( Camera_getOrigin( camera ), vector3_scaled( camera.right, -SPEED_MOVE ) ) );
}
void Camera_MoveRight_Discrete( camera_t& camera ){
	Camera_Move_updateAxes( camera );
	Camera_setOrigin( camera, vector3_added( Camera_getOrigin( camera ), vector3_scaled( camera.right, SPEED_MOVE ) ) );
}

void Camera_RotateLeft_Discrete( camera_t& camera ){
	Vector3 angles( Camera_getAngles( camera ) );
	angles[CAMERA_YAW] += SPEED_TURN;
	Camera_setAngles( camera, angles );
}
void Camera_RotateRight_Discrete( camera_t& camera ){
	Vector3 angles( Camera_getAngles( camera ) );
	angles[CAMERA_YAW] -= SPEED_TURN;
	Camera_setAngles( camera, angles );
}

void Camera_PitchUp_Discrete( camera_t& camera ){
	Vector3 angles( Camera_getAngles( camera ) );
	angles[CAMERA_PITCH] += SPEED_TURN;
	if ( angles[CAMERA_PITCH] > 90 ) {
		angles[CAMERA_PITCH] = 90;
	}
	Camera_setAngles( camera, angles );
}
void Camera_PitchDown_Discrete( camera_t& camera ){
	Vector3 angles( Camera_getAngles( camera ) );
	angles[CAMERA_PITCH] -= SPEED_TURN;
	if ( angles[CAMERA_PITCH] < -90 ) {
		angles[CAMERA_PITCH] = -90;
	}
	Camera_setAngles( camera, angles );
}


class RadiantCameraView : public CameraView
{
	camera_t& m_camera;
	View* m_view;
	Callback<void()> m_update;
public:
	RadiantCameraView( camera_t& camera, View* view, const Callback<void()>& update ) : m_camera( camera ), m_view( view ), m_update( update ){
	}
	void update(){
		m_view->Construct( m_camera.projection, m_camera.modelview, m_camera.width, m_camera.height );
		m_update();
	}
	void setModelview( const Matrix4& modelview ) override {
		m_camera.modelview = modelview;
		matrix4_multiply_by_matrix4( m_camera.modelview, g_radiant2opengl );
		matrix4_affine_invert( m_camera.modelview );
		Camera_updateVectors( m_camera );
		update();
	}
	void setFieldOfView( float fieldOfView ) override {
		float farClip = Camera_getFarClipPlane( m_camera );
		m_camera.projection = projection_for_camera( camera_t::near_z, farClip, fieldOfView, m_camera.width, m_camera.height );
		update();
	}
};


static void Camera_motionDelta( int x, int y, const QMouseEvent *event, camera_t& cam ){
	cam.m_mouseMove.motion_delta( x, y, event );
	cam.m_idleDraw.queueDraw( DeferredMotionDelta2::InvokeCaller( cam.m_mouseMove ), true );

	cam.m_orbit = ( event->modifiers() & Qt::KeyboardModifier::AltModifier ) && ( event->buttons() & Qt::MouseButton::RightButton );
	if( cam.m_orbit ){
		cam.m_strafe = false;
		return;
	}

	cam.m_strafe_forward_invert = false;

	switch ( g_camwindow_globals_private.m_strafeMode )
	{
	case 0:
		cam.m_strafe = false;
		break;
	case 1:
		cam.m_strafe = ( event->modifiers() & Qt::KeyboardModifier::ControlModifier ) || ( event->buttons() & Qt::MouseButton::RightButton );
		cam.m_strafe_forward = false;
		break;
	case 2:
		cam.m_strafe = ( event->modifiers() & Qt::KeyboardModifier::ControlModifier ) || ( event->buttons() & Qt::MouseButton::RightButton );
		cam.m_strafe_forward = true;
		break;
	case 4:
		cam.m_strafe_forward_invert = true; // fall through
	default: /* 3 & 4 */
		cam.m_strafe = ( event->modifiers() & Qt::KeyboardModifier::ControlModifier ) || ( event->buttons() & Qt::MouseButton::RightButton ) || ( event->modifiers() & Qt::KeyboardModifier::ShiftModifier );
		cam.m_strafe_forward = ( event->modifiers() & Qt::KeyboardModifier::ShiftModifier ) != 0;
		break;
	}

	if( ( event->buttons() & Qt::MouseButton::LeftButton ) != 0 && g_camwindow_globals_private.m_strafeMode != 0 ){
		cam.m_strafe = true;
		cam.m_strafe_forward = false;
	}
}

#include "stream/stringstream.h"

class CamDrawSize
{
	Vector3 _extents;
	RenderTextLabel m_labels[3];
public:
	CamDrawSize() : _extents( -99.9f, -99.9f, -99.9f ){
	}
	void render( Renderer& renderer, Shader* shader, const View& view ){
		const AABB bounds = GlobalSelectionSystem().getBoundsSelected();
		if( bounds.extents.x() != 0 || bounds.extents.y() != 0 || bounds.extents.z() != 0 ){
			renderer.SetState( shader, Renderer::eFullMaterials );

			Vector4 points[3] = { Vector4( bounds.origin - g_vector3_axes[1] * bounds.extents - g_vector3_axes[2] * bounds.extents, 1 ),
			                      Vector4( bounds.origin - g_vector3_axes[0] * bounds.extents - g_vector3_axes[2] * bounds.extents, 1 ),
			                      Vector4( bounds.origin - g_vector3_axes[0] * bounds.extents - g_vector3_axes[1] * bounds.extents, 1 ),
			                    };
			for( std::size_t i = 0; i < 3; ++i ){
				matrix4_transform_vector4( view.GetViewMatrix(), points[i] );
				points[i].x() /= points[i].w();
				points[i].y() /= points[i].w();
//				points[i].z() /= points[i].w();
				matrix4_transform_vector4( view.GetViewport(), points[i] );
			}

			for( std::size_t i = 0; i < 3; ++i ){
				if( points[i].w() > 0.005f ){
					updateTex( i, bounds.extents[i] );
					m_labels[i].screenPos = points[i].vec3().vec2();
					renderer.addRenderable( m_labels[i], g_matrix4_identity );
				}
			}
		}
	}
private:
	const Vector3 getColor( const std::size_t i ) const {
		switch ( i )
		{
		case 0:
			return Vector3( g_colour_x.r, g_colour_x.g, g_colour_x.b ) / 255.f;
		case 1:
			return Vector3( g_colour_y.r, g_colour_y.g, g_colour_y.b ) / 255.f;
		default:
			return Vector3( g_colour_z.r, 178, g_colour_z.b ) / 255.f; //hack to make default blue visible better
		}
	}
	void updateTex( const std::size_t i, const float extent ){
		if( extent != _extents[i] ){
			_extents[i] = extent;
			m_labels[i].texFree();
			m_labels[i].texAlloc( StringStream<16>( extent * 2 ), getColor( i ) );
		}
	}
};

#include "grid.h"
class RenderableCamWorkzone : public OpenGLRenderable
{
	mutable std::array<Vector3, 9999> m_verticesarr[3];
	mutable std::array<GLboolean, 9999> m_edgearr[3];
	mutable std::array<Colour4b, 9999> m_colorarr0[3];
	mutable std::array<Colour4b, 9999> m_colorarr1[3];
public:
	void render( RenderStateFlags state ) const override {
		gl().glEnableClientState( GL_EDGE_FLAG_ARRAY );

		const AABB bounds = GlobalSelectionSystem().getBoundsSelected();

		for( std::size_t i = 0; i < 3; ++i ){
			const std::size_t i2 = ( i + 1 ) % 3;
			const std::size_t i3 = ( i + 2 ) % 3;
//			const Vector3 normal = g_vector3_axes[i];
			const float offset = 1024;
			std::vector<Vector3> points;
			points.reserve( 4 );
			points.push_back( bounds.origin + g_vector3_axes[i2] * bounds.extents + g_vector3_axes[i3] * bounds.extents );
			if( bounds.extents[i2] != 0 ){
				points.push_back( bounds.origin - g_vector3_axes[i2] * bounds.extents + g_vector3_axes[i3] * bounds.extents );
			}
			if( bounds.extents[i3] != 0 ){
				points.push_back( bounds.origin + g_vector3_axes[i2] * bounds.extents - g_vector3_axes[i3] * bounds.extents );
				if( bounds.extents[i2] != 0 ){
					points.push_back( bounds.origin - g_vector3_axes[i2] * bounds.extents - g_vector3_axes[i3] * bounds.extents );
				}
			}

			float grid = GetGridSize();
			std::size_t approx_count = ( std::max( 0.f, bounds.extents[i] ) + offset ) * 4 / grid + 8;
			if( approx_count > m_verticesarr->size() ){ // prevent overlow
				approx_count = m_verticesarr->size();
				grid = ( std::max( 0.f, bounds.extents[i] ) + offset ) * 4 / ( approx_count - 8 ); // adjust grid accordingly
				grid = std::exp2( std::ceil( std::log2( grid ) ) ); // round to next PoT
			}

			auto& verticesarr( m_verticesarr[i] );
			auto& edgearr( m_edgearr[i] );
			auto& colorarr0( m_colorarr0[i] );
			auto& colorarr1( m_colorarr1[i] );

			float coord = float_snapped( bounds.origin[i] - std::max( 0.f, bounds.extents[i] ) - offset, grid );
//			const float coord_end = float_snapped( bounds.origin[i] + std::max( 0.f, bounds.extents[i] ) + offset, grid ) + 0.1f;
			const bool start0 = float_snapped( coord, grid * 2 ) == coord;
			std::size_t count = 0;

			for( ; count < approx_count - 4; count += 4 ){
				verticesarr[count][i] =
				verticesarr[count + 1][i] = coord;
				const float alpha = std::clamp( ( offset + bounds.extents[i] - std::fabs( coord - bounds.origin[i] ) ) / offset, 0.f, 1.f );
				colorarr0[count] = colorarr0[count + 1] = Colour4b( 255, 0, 0, alpha * 255 );
				colorarr1[count] = colorarr1[count + 1] = Colour4b( 255, 255, 255, alpha * 255 );
				coord += grid;
				verticesarr[count + 2][i] =
				verticesarr[count + 3][i] = coord;
				const float alpha2 = std::clamp( ( offset + bounds.extents[i] - std::fabs( coord - bounds.origin[i] ) ) / offset, 0.f, 1.f );
				colorarr0[count + 2] = colorarr0[count + 3] = Colour4b( 255, 0, 0, alpha2 * 255 );
				colorarr1[count + 2] = colorarr1[count + 3] = Colour4b( 255, 255, 255, alpha2 * 255 );
				coord += grid;
				edgearr[count] =
				edgearr[count + 2] = GL_FALSE;
				edgearr[count + 1] =
				edgearr[count + 3] = GL_TRUE;
			}

			if( points.size() == 1 ){
				points.push_back( points[0] + g_vector3_axes[i2] * 8 );
				for( std::size_t k = 0; k < count; k += 4 ){
					edgearr[k + 1] = GL_FALSE;
				}
			}

			gl().glVertexPointer( 3, GL_FLOAT, sizeof( Vector3 ), verticesarr.data()->data() );
			gl().glEdgeFlagPointer( sizeof( GLboolean ), edgearr.data() );
			for( std::vector<Vector3>::const_iterator j = points.begin(); j != points.end(); ++++j ){
				const std::vector<Vector3>::const_iterator jj = j + 1;
				for( std::size_t k = 0; k < count; k += 4 ){
					verticesarr[k][i2] = ( *j )[i2];
					verticesarr[k][i3] = ( *j )[i3];
					verticesarr[k + 1][i2] = ( *jj )[i2];
					verticesarr[k + 1][i3] = ( *jj )[i3];
					verticesarr[k + 2][i2] = ( *jj )[i2];
					verticesarr[k + 2][i3] = ( *jj )[i3];
					verticesarr[k + 3][i2] = ( *j )[i2];
					verticesarr[k + 3][i3] = ( *j )[i3];
				}

				gl().glPolygonOffset( -2, 2 );
				gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( Colour4b ), colorarr0.data() );
				gl().glDrawArrays( GL_QUADS, start0? 0 : 2, GLsizei( count - ( start0? 4 : 2 ) ) );

				gl().glPolygonOffset( 1, -1 );
				gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( Colour4b ), colorarr1.data() );
				gl().glDrawArrays( GL_QUADS, start0? 2 : 0, GLsizei( count - ( start0? 2 : 4 ) ) );
				gl().glPolygonOffset( -1, 1 ); // restore default
			}
		}

		gl().glDisableClientState( GL_EDGE_FLAG_ARRAY );
	}

	void render( Renderer& renderer, Shader* shader ) const {
		renderer.SetState( shader, Renderer::eFullMaterials );
		renderer.addRenderable( *this, g_matrix4_identity );
	}
};





class CamWnd
{
	View m_view;
public:
	const View& getView() const { return m_view; } // KenRadiant: model drag & drop placement
private:
	camera_t m_Camera;
	RadiantCameraView m_cameraview;
#if 0
	int m_PositionDragCursorX;
	int m_PositionDragCursorY;
#endif

	static Shader* m_state_select0;
	static Shader* m_state_select1;
	static Shader* m_state_wire;
	static Shader* m_state_facewire;
	static Shader* m_state_workzone;
	static Shader* m_state_text;

	FreezePointer m_freezePointer;

	CamDrawSize m_draw_size;
	RenderableCamWorkzone m_draw_workzone;

public:
	QWidget* m_gl_widget;
	QWidget* m_parent;

	SelectionSystemWindowObserver* m_window_observer;
	XORRectangle m_XORRectangle;
	rect_t m_XORRect;

	DeferredDraw m_deferredDraw;
	DeferredMotion2 m_deferred_motion;

	Timer m_render_time;

	CamWnd();
	~CamWnd();

	bool m_drawing;
	void queue_draw(){
		//ASSERT_MESSAGE( !m_drawing, "CamWnd::queue_draw(): called while draw is already in progress" );
		if ( m_drawing ) {
			return;
		}
		//globalOutputStream() << "queue... ";
		m_deferredDraw.draw();
	}
	void draw();

	static void captureStates(){
		m_state_text = GlobalShaderCache().capture( "$TEXT" );
		m_state_workzone = GlobalShaderCache().capture( "$CAM_WORKZONE" );
		m_state_facewire = GlobalShaderCache().capture( "$CAM_FACEWIRE" );
		m_state_wire = GlobalShaderCache().capture( "$CAM_WIRE" );
		m_state_select0 = GlobalShaderCache().capture( "$CAM_OVERLAY" );
		m_state_select1 = GlobalShaderCache().capture( "$CAM_HIGHLIGHT" );
	}
	static void releaseStates(){
		GlobalShaderCache().release( "$CAM_HIGHLIGHT" );
		GlobalShaderCache().release( "$CAM_OVERLAY" );
		GlobalShaderCache().release( "$CAM_WIRE" );
		GlobalShaderCache().release( "$CAM_FACEWIRE" );
		GlobalShaderCache().release( "$CAM_WORKZONE" );
		GlobalShaderCache().release( "$TEXT" );
	}

	camera_t& getCamera(){
		return m_Camera;
	}

	void BenchMark();
	void Cam_ChangeFloor( bool up );

	void DisableFreeMove();
	void EnableFreeMove();
	bool m_bFreeMove;
	bool m_bFreeMove_entering;

	CameraView& getCameraView(){
		return m_cameraview;
	}

	Timer m_rightClickTimer;
	float m_rightClickMove;

	void selection_motion_freemove( const MotionDeltaValues& delta );

	bool m_drawRequired{}; // whether complete redraw is required, or just overlay update is enough
private:
	void Cam_Draw();
};

Shader* CamWnd::m_state_select0 = 0;
Shader* CamWnd::m_state_select1 = 0;
Shader* CamWnd::m_state_wire = 0;
Shader* CamWnd::m_state_facewire = 0;
Shader* CamWnd::m_state_workzone = 0;
Shader* CamWnd::m_state_text = 0;

CamWnd* NewCamWnd(){
	return new CamWnd;
}
void DeleteCamWnd( CamWnd* camwnd ){
	delete camwnd;
}

void CamWnd_constructStatic(){
	CamWnd::captureStates();
}

Shader* g_krMaskState = nullptr; // selection outline mask (captured on first use)
void CamWnd_destroyStatic(){
	CamWnd::releaseStates();
	if( g_krMaskState != nullptr ){ // (an unreleased state asserts when the editor closes)
		GlobalShaderCache().release( "$KR_MASK" );
		g_krMaskState = nullptr;
	}
}

void CamWnd_reconstructStatic(){
	CamWnd_destroyStatic();
	CamWnd_constructStatic();
}

static CamWnd* g_camwnd = 0;

void GlobalCamera_setCamWnd( CamWnd& camwnd ){
	g_camwnd = &camwnd;
}


QWidget* CamWnd_getWidget( CamWnd& camwnd ){
	return camwnd.m_gl_widget;
}

ToggleShown g_camera_shown( true );

void CamWnd_Shown_Construct( QWidget* parent ){
	g_camera_shown.connect( parent );
}

void CamWnd_setParent( CamWnd& camwnd, QWidget* parent ){
	camwnd.m_parent = parent;
}

void CamWnd_Update( CamWnd& camwnd ){
	camwnd.m_drawRequired = true;
	camwnd.queue_draw();
}



camwindow_globals_t g_camwindow_globals;

const Vector3& Camera_getOrigin( CamWnd& camwnd ){
	return Camera_getOrigin( camwnd.getCamera() );
}

void Camera_setOrigin( CamWnd& camwnd, const Vector3& origin ){
	Camera_setOrigin( camwnd.getCamera(), origin );
}

const Vector3& Camera_getAngles( CamWnd& camwnd ){
	return Camera_getAngles( camwnd.getCamera() );
}

void Camera_setAngles( CamWnd& camwnd, const Vector3& angles ){
	Camera_setAngles( camwnd.getCamera(), angles );
}

const Vector3& Camera_getViewVector( CamWnd& camwnd ){
	return camwnd.getCamera().vpn;
}


// =============================================================================
// CamWnd class

void context_menu_show(){
	if( g_pParentWnd->ActiveXY() ){
		g_pParentWnd->ActiveXY()->OnContextMenu();
		g_bCamEntityMenu = true;
	}
}

/* GDK_2BUTTON_PRESS doesn't always work in this case, so... */
/* with Qt freezepointer interrupts internal doubleclick timer, so use custom one */
inline bool context_menu_try( const CamWnd& camwnd ){
	//globalOutputStream() << camwnd->m_rightClickTimer.elapsed_msec() << '\n';
	return camwnd.m_rightClickTimer.elapsed_msec() < 250;
	//doesn't work if cam redraw > 200msec (3x click works): gtk_widget_queue_draw proceeds after timer.start()
}

void camera_orbit_init( camera_t& cam, Vector2 xy ){
	xy.x() = ( ( 2.0f * xy.x() ) / cam.width ) - 1.0f; // window_to_normalised_device
	xy.y() = ( ( 2.0f * ( cam.height - 1 - xy.y() ) ) / cam.height ) - 1.0f;

	const Vector2 epsilon( 8.f / cam.width, 8.f / cam.height ); //device epsilon

	Scene_Intersect( *cam.m_view, xy, epsilon, cam.m_orbit_center );

	cam.m_orbit_initial_pos = cam.origin;
	cam.m_orbit_offset = 0;
	cam.m_orbit = true;
}

inline bool ORBIT_EVENT( const QMouseEvent& event ){
	return event.button() == Qt::MouseButton::RightButton && modifiers_for_state( event.modifiers() ) == c_modifierAlt;
}
inline bool M2_EVENT( const QMouseEvent& event ){
	return event.button() == Qt::MouseButton::RightButton && modifiers_for_state( event.modifiers() ) == c_modifierNone;
}

static void enable_freelook_button_press( const QMouseEvent& event, CamWnd& camwnd ){
	const bool m2    = M2_EVENT( event );
	const bool m2alt = ORBIT_EVENT( event );
	if ( m2 || m2alt ) {
		camwnd.m_bFreeMove_entering = true;
		if( m2 && context_menu_try( camwnd ) ){
			context_menu_show();
		}
		else{
			if( m2alt )
				camera_orbit_init( camwnd.getCamera(), Vector2( event.x(), event.y() ) );
			camwnd.EnableFreeMove();
			camwnd.m_rightClickTimer.start();
			camwnd.m_rightClickMove = 0;
		}
	}
}

static void disable_freelook_button_press( const QMouseEvent& event, CamWnd& camwnd ){
	const bool m2    = M2_EVENT( event );
	const bool m2alt = ORBIT_EVENT( event );
	if ( m2 || m2alt ) {
		camwnd.m_bFreeMove_entering = false;
		if( m2 && context_menu_try( camwnd ) ){
			camwnd.DisableFreeMove();
			context_menu_show();
		}
		else{
			if( m2alt )
				camera_orbit_init( camwnd.getCamera(), Vector2( event.x(), event.y() ) );
			camwnd.m_rightClickTimer.start();
			camwnd.m_rightClickMove = 0;
		}
	}
}

static void disable_freelook_button_release( const QMouseEvent& event, CamWnd& camwnd ){
	const bool m2    = M2_EVENT( event );
	const bool m2alt = ORBIT_EVENT( event );
	if ( m2 || m2alt ) {
		camwnd.getCamera().m_orbit = false;
		if( ( ( camwnd.m_rightClickTimer.elapsed_msec() < 300 && camwnd.m_rightClickMove < 56 ) == !camwnd.m_bFreeMove_entering ) ){
			camwnd.DisableFreeMove();
		}
	}
}


void camwnd_update_xor_rectangle( CamWnd& self, rect_t area ){
	self.m_XORRect = area;
	self.queue_draw();
 }


static void selection_button_press( const QMouseEvent& event, WindowObserver* observer ){
	if( !ORBIT_EVENT( event ) )
		observer->onMouseDown( WindowVector( event.x(), event.y() ), button_for_button( event.button() ), modifiers_for_state( event.modifiers() ) );
}

static void selection_button_release( const QMouseEvent& event, WindowObserver* observer ){
	observer->onMouseUp( WindowVector( event.x(), event.y() ), button_for_button( event.button() ), modifiers_for_state( event.modifiers() ) );
}

void selection_motion( const QMouseEvent& event, WindowObserver* observer ){
	//globalOutputStream() << "motion... ";
	observer->onMouseMotion( WindowVector( event.x(), event.y() ), modifiers_for_state( event.modifiers() ) );
}

inline WindowVector windowvector_for_widget_centre( const QWidget* widget ){
	const QPoint center = widget->rect().center();
	return WindowVector( center.x(), center.y() ) * widget->devicePixelRatioF();
}

static void selection_button_press_freemove( QWidget* widget, const QMouseEvent& event, WindowObserver* observer ){
	if( !ORBIT_EVENT( event ) )
		observer->onMouseDown( windowvector_for_widget_centre( widget ), button_for_button( event.button() ), modifiers_for_state( event.modifiers() ) );
}

static void selection_button_release_freemove( QWidget* widget, const QMouseEvent& event, WindowObserver* observer ){
	observer->onMouseUp( windowvector_for_widget_centre( widget ), button_for_button( event.button() ), modifiers_for_state( event.modifiers() ) );
}

void CamWnd::selection_motion_freemove( const MotionDeltaValues& delta ){
	m_rightClickMove += sqrt( delta.x * delta.x + delta.y * delta.y );
	m_window_observer->incMouseMove( WindowVector( delta.x, delta.y ) );
	m_window_observer->onMouseMotion( windowvector_for_widget_centre( m_gl_widget ), modifiers_for_state( delta.mouseMoveEvent.modifiers() ) );
}
typedef MemberCaller<CamWnd, void(const MotionDeltaValues&), &CamWnd::selection_motion_freemove> CamWnd_selection_motion_freemove;


void camera_orbit_scroll( camera_t& camera ){
	Vector3 viewvector = vector3_normalised( camera.m_orbit_center - Camera_getOrigin( camera ) );
	if( vector3_dot( viewvector, -camera.vpn ) < 0 )
		vector3_negate( viewvector );
	float offset = vector3_length( camera.m_orbit_center - camera.m_orbit_initial_pos );
	const int off = camera.m_orbit_offset;
	if( off < 0 || off > 16 ){
		offset -= offset * off / 8 * pow( 2.0f, ( off < 0 ? -off : off - 16 ) / 8.f );
	}
	else if( off == 8 ){
		offset = std::min( 8.f, offset / 16.f ); //prevent zero offset, resulting in NAN viewvector in the next scroll step
	}
	else{
		offset -= offset * off / 8;
	}
	Camera_setOrigin( camera, camera.m_orbit_center - viewvector * offset );
}

/*
two alt solutions:
		const Matrix4 screen2world = matrix4_affine_inverse( cam.m_view->GetViewMatrix() );
		Vector3 normalized;
		normalized[0] = ( ( 2.0f * x ) / cam.width ) - 1.0f; // window_to_normalised_device
		normalized[1] = ( ( 2.0f * ( cam.height - 1 - y ) ) / cam.height ) - 1.0f;
		normalized[2] = 1.f;
		normalized = vector4_projected( matrix4_transformed_vector4( screen2world, Vector4( normalized, 1 ) ) );

		const Matrix4 screen2world = matrix4_full_inverse( cam.m_view->GetViewMatrix() );
		Vector3 normalized;
		normalized[0] = ( ( 2.0f * x ) / cam.width ) - 1.0f; // window_to_normalised_device
		normalized[1] = ( ( 2.0f * ( cam.height - 1 - y ) ) / cam.height ) - 1.0f;
		normalized[2] = -1;
		normalized = vector4_projected( matrix4_transformed_vector4( screen2world, Vector4( normalized, 1 ) ) );
*/
static void camera_zoom( CamWnd& camwnd, float x, float y, float step ){
	const camera_t& cam = camwnd.getCamera();
	if( camwnd.m_bFreeMove || !g_camwindow_globals_private.m_bZoomToPointer ){
		Camera_setOrigin( camwnd, Camera_getOrigin( camwnd ) + cam.forward * step );
	}
	else{
		//const Matrix4 screen2world = matrix4_affine_inverse( matrix4_multiplied_by_matrix4( cam.projection, cam.modelview ) );
		const Matrix4 screen2world = matrix4_affine_inverse( cam.m_view->GetViewMatrix() );

		Vector3 normalized;

		normalized[0] = 2.f * x / cam.width - 1.f;
		normalized[1] = 2.f * y / cam.height - 1.f;
		normalized[1] *= -1;
		normalized[2] = 0;

		normalized *= ( camera_t::near_z * 2.f );
			//globalOutputStream() << normalized << " normalized    ";
		matrix4_transform_point( screen2world, normalized );
			//globalOutputStream() << normalized << '\n';
		const Vector3 norm = vector3_normalised( normalized - Camera_getOrigin( camwnd ) );
			//globalOutputStream() << normalized - Camera_getOrigin( *camwnd ) << "  normalized - Camera_getOrigin( *camwnd )\n";
			//globalOutputStream() << norm << "  norm\n";
		Camera_setOrigin( camwnd, Camera_getOrigin( camwnd ) + norm * step );
	}
}

static void wheelmove_scroll( const QWheelEvent& event, CamWnd& camwnd ){
	camera_t& cam = camwnd.getCamera();

	// KenRadiant: like Unreal, the wheel changes fly speed while the right mouse button is held in fly mode
	if( camwnd.m_bFreeMove && ( event.buttons() & Qt::MouseButton::RightButton ) && !cam.m_orbit ){
		const int delta = event.angleDelta().y() != 0 ? event.angleDelta().y() : event.angleDelta().x();
		if( delta != 0 ){
			int& speed = g_camwindow_globals_private.m_nMoveSpeed;
			speed = std::clamp( static_cast<int>( delta > 0 ? speed * 1.25f + 0.5f : speed / 1.25f + 0.5f ), CAM_MIN_SPEED, CAM_MAX_SPEED );
			globalOutputStream() << "Camera Move Speed: ";
			globalWarningStream() << speed << '\n';
		}
		return;
	}

	const int angleDelta = ( std::abs( event.angleDelta().y() ) > std::abs( event.angleDelta().x() ) ) // normal y() goes to x() with ALT pressed
	                      ? event.angleDelta().y()
						  : event.angleDelta().x();

	if ( angleDelta > 0 ) {
		if ( cam.movementflags & MOVE_FOCUS ) {
			++cam.m_focus_offset;
			return;
		}
		else if( cam.m_orbit ){
			++cam.m_orbit_offset;
			camera_orbit_scroll( cam );
			return;
		}

		Camera_Freemove_updateAxes( cam );
		camera_zoom( camwnd, event.position().x(), event.position().y(), g_camwindow_globals_private.m_nScrollMoveSpeed );
	}
	else if ( angleDelta < 0 ) {
		if ( cam.movementflags & MOVE_FOCUS ) {
			--cam.m_focus_offset;
			return;
		}
		else if( cam.m_orbit ){
			--cam.m_orbit_offset;
			camera_orbit_scroll( cam );
			return;
		}

		Camera_Freemove_updateAxes( cam );
		camera_zoom( camwnd, event.position().x(), event.position().y(), -g_camwindow_globals_private.m_nScrollMoveSpeed );
	}
}

void KeyEvent_connect( const char* name ){
	const KeyEvent& keyEvent = GlobalKeyEvents_find( name );
	keydown_accelerators_add( keyEvent.m_accelerator, keyEvent.m_keyDown );
	keyup_accelerators_add( keyEvent.m_accelerator, keyEvent.m_keyUp );
}

void KeyEvent_disconnect( const char* name ){
	const KeyEvent& keyEvent = GlobalKeyEvents_find( name );
	keydown_accelerators_remove( keyEvent.m_accelerator );
	keyup_accelerators_remove( keyEvent.m_accelerator );
}

void CamWnd_registerCommands( CamWnd& camwnd ){
	GlobalKeyEvents_insert( "CameraForward",
	                        ReferenceCaller<camera_t, void(), Camera_MoveForward_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_MoveForward_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraBack",
	                        ReferenceCaller<camera_t, void(), Camera_MoveBack_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_MoveBack_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraLeft",
	                        ReferenceCaller<camera_t, void(), Camera_RotateLeft_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_RotateLeft_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraRight",
	                        ReferenceCaller<camera_t, void(), Camera_RotateRight_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_RotateRight_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraStrafeRight",
	                        ReferenceCaller<camera_t, void(), Camera_MoveRight_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_MoveRight_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraStrafeLeft",
	                        ReferenceCaller<camera_t, void(), Camera_MoveLeft_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_MoveLeft_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraUp",
	                        ReferenceCaller<camera_t, void(), Camera_MoveUp_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_MoveUp_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraDown",
	                        ReferenceCaller<camera_t, void(), Camera_MoveDown_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_MoveDown_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraAngleUp",
	                        ReferenceCaller<camera_t, void(), Camera_PitchUp_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_PitchUp_KeyUp>( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraAngleDown",
	                        ReferenceCaller<camera_t, void(), Camera_PitchDown_KeyDown>( camwnd.getCamera() ),
	                        ReferenceCaller<camera_t, void(), Camera_PitchDown_KeyUp>( camwnd.getCamera() )
	                      );

	GlobalKeyEvents_insert( "CameraFreeMoveForward",
	                        FreeMoveCameraMoveForwardKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveForwardKeyUpCaller( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraFreeMoveBack",
	                        FreeMoveCameraMoveBackKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveBackKeyUpCaller( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraFreeMoveLeft",
	                        FreeMoveCameraMoveLeftKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveLeftKeyUpCaller( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraFreeMoveRight",
	                        FreeMoveCameraMoveRightKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveRightKeyUpCaller( camwnd.getCamera() )
	                      );

	GlobalKeyEvents_insert( "CameraFreeMoveForward2",
	                        FreeMoveCameraMoveForwardKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveForwardKeyUpCaller( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraFreeMoveBack2",
	                        FreeMoveCameraMoveBackKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveBackKeyUpCaller( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraFreeMoveLeft2",
	                        FreeMoveCameraMoveLeftKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveLeftKeyUpCaller( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraFreeMoveRight2",
	                        FreeMoveCameraMoveRightKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveRightKeyUpCaller( camwnd.getCamera() )
	                      );

	GlobalKeyEvents_insert( "CameraFreeMoveUp",
	                        FreeMoveCameraMoveUpKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveUpKeyUpCaller( camwnd.getCamera() )
	                      );
	GlobalKeyEvents_insert( "CameraFreeMoveDown",
	                        FreeMoveCameraMoveDownKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraMoveDownKeyUpCaller( camwnd.getCamera() )
	                      );

	GlobalKeyEvents_insert( "CameraFreeFocus",
	                        FreeMoveCameraFocusKeyDownCaller( camwnd.getCamera() ),
	                        FreeMoveCameraFocusKeyUpCaller( camwnd.getCamera() )
	                      );

	GlobalCommands_insert( "CameraForward", ReferenceCaller<camera_t, void(), Camera_MoveForward_Discrete>( camwnd.getCamera() ) );
	GlobalCommands_insert( "CameraBack", ReferenceCaller<camera_t, void(), Camera_MoveBack_Discrete>( camwnd.getCamera() ) );
	GlobalCommands_insert( "CameraLeft", ReferenceCaller<camera_t, void(), Camera_RotateLeft_Discrete>( camwnd.getCamera() ) );
	GlobalCommands_insert( "CameraRight", ReferenceCaller<camera_t, void(), Camera_RotateRight_Discrete>( camwnd.getCamera() ) );
	GlobalCommands_insert( "CameraStrafeRight", ReferenceCaller<camera_t, void(), Camera_MoveRight_Discrete>( camwnd.getCamera() ) );
	GlobalCommands_insert( "CameraStrafeLeft", ReferenceCaller<camera_t, void(), Camera_MoveLeft_Discrete>( camwnd.getCamera() ) );

	GlobalCommands_insert( "CameraUp", ReferenceCaller<camera_t, void(), Camera_MoveUp_Discrete>( camwnd.getCamera() ) );
	GlobalCommands_insert( "CameraDown", ReferenceCaller<camera_t, void(), Camera_MoveDown_Discrete>( camwnd.getCamera() ) );
	GlobalCommands_insert( "CameraAngleUp", ReferenceCaller<camera_t, void(), Camera_PitchUp_Discrete>( camwnd.getCamera() ) );
	GlobalCommands_insert( "CameraAngleDown", ReferenceCaller<camera_t, void(), Camera_PitchDown_Discrete>( camwnd.getCamera() ) );
}

void CamWnd_Move_Enable( CamWnd& camwnd ){
	KeyEvent_connect( "CameraForward" );
	KeyEvent_connect( "CameraBack" );
	KeyEvent_connect( "CameraLeft" );
	KeyEvent_connect( "CameraRight" );
	KeyEvent_connect( "CameraStrafeRight" );
	KeyEvent_connect( "CameraStrafeLeft" );
	KeyEvent_connect( "CameraUp" );
	KeyEvent_connect( "CameraDown" );
	KeyEvent_connect( "CameraAngleUp" );
	KeyEvent_connect( "CameraAngleDown" );
}

void CamWnd_Move_Disable( CamWnd& camwnd ){
	KeyEvent_disconnect( "CameraForward" );
	KeyEvent_disconnect( "CameraBack" );
	KeyEvent_disconnect( "CameraLeft" );
	KeyEvent_disconnect( "CameraRight" );
	KeyEvent_disconnect( "CameraStrafeRight" );
	KeyEvent_disconnect( "CameraStrafeLeft" );
	KeyEvent_disconnect( "CameraUp" );
	KeyEvent_disconnect( "CameraDown" );
	KeyEvent_disconnect( "CameraAngleUp" );
	KeyEvent_disconnect( "CameraAngleDown" );
}

void CamWnd_Move_Discrete_Enable( CamWnd& camwnd ){
	command_connect_accelerator( "CameraForward" );
	command_connect_accelerator( "CameraBack" );
	command_connect_accelerator( "CameraLeft" );
	command_connect_accelerator( "CameraRight" );
	command_connect_accelerator( "CameraStrafeRight" );
	command_connect_accelerator( "CameraStrafeLeft" );
	command_connect_accelerator( "CameraUp" );
	command_connect_accelerator( "CameraDown" );
	command_connect_accelerator( "CameraAngleUp" );
	command_connect_accelerator( "CameraAngleDown" );
}

void CamWnd_Move_Discrete_Disable( CamWnd& camwnd ){
	command_disconnect_accelerator( "CameraForward" );
	command_disconnect_accelerator( "CameraBack" );
	command_disconnect_accelerator( "CameraLeft" );
	command_disconnect_accelerator( "CameraRight" );
	command_disconnect_accelerator( "CameraStrafeRight" );
	command_disconnect_accelerator( "CameraStrafeLeft" );
	command_disconnect_accelerator( "CameraUp" );
	command_disconnect_accelerator( "CameraDown" );
	command_disconnect_accelerator( "CameraAngleUp" );
	command_disconnect_accelerator( "CameraAngleDown" );
}

void CamWnd_Move_Discrete_Import( CamWnd& camwnd, bool value ){
	if ( g_camwindow_globals_private.m_bCamDiscrete ) {
		CamWnd_Move_Discrete_Disable( camwnd );
	}
	else
	{
		CamWnd_Move_Disable( camwnd );
	}

	g_camwindow_globals_private.m_bCamDiscrete = value;

	if ( g_camwindow_globals_private.m_bCamDiscrete ) {
		CamWnd_Move_Discrete_Enable( camwnd );
	}
	else
	{
		CamWnd_Move_Enable( camwnd );
	}
}

void CamWnd_Move_Discrete_Import( bool value ){
	if ( g_camwnd != 0 ) {
		CamWnd_Move_Discrete_Import( *g_camwnd, value );
	}
	else
	{
		g_camwindow_globals_private.m_bCamDiscrete = value;
	}
}



void CamWnd_Add_Handlers_Move( CamWnd& camwnd ){
	if ( g_camwindow_globals_private.m_bCamDiscrete ) {
		CamWnd_Move_Discrete_Enable( camwnd );
	}
	else
	{
		CamWnd_Move_Enable( camwnd );
	}
}

void CamWnd_Remove_Handlers_Move( CamWnd& camwnd ){
	if ( g_camwindow_globals_private.m_bCamDiscrete ) {
		CamWnd_Move_Discrete_Disable( camwnd );
	}
	else
	{
		CamWnd_Move_Disable( camwnd );
	}
}

void CamWnd_Add_Handlers_FreeMove( CamWnd& camwnd ){
	KeyEvent_connect( "CameraFreeMoveForward" );
	KeyEvent_connect( "CameraFreeMoveBack" );
	KeyEvent_connect( "CameraFreeMoveLeft" );
	KeyEvent_connect( "CameraFreeMoveRight" );

	KeyEvent_connect( "CameraFreeMoveForward2" );
	KeyEvent_connect( "CameraFreeMoveBack2" );
	KeyEvent_connect( "CameraFreeMoveLeft2" );
	KeyEvent_connect( "CameraFreeMoveRight2" );

	KeyEvent_connect( "CameraFreeMoveUp" );
	KeyEvent_connect( "CameraFreeMoveDown" );

	KeyEvent_connect( "CameraFreeFocus" );
}

void CamWnd_Remove_Handlers_FreeMove( CamWnd& camwnd ){
	KeyEvent_disconnect( "CameraFreeMoveForward" );
	KeyEvent_disconnect( "CameraFreeMoveBack" );
	KeyEvent_disconnect( "CameraFreeMoveLeft" );
	KeyEvent_disconnect( "CameraFreeMoveRight" );

	KeyEvent_disconnect( "CameraFreeMoveForward2" );
	KeyEvent_disconnect( "CameraFreeMoveBack2" );
	KeyEvent_disconnect( "CameraFreeMoveLeft2" );
	KeyEvent_disconnect( "CameraFreeMoveRight2" );

	KeyEvent_disconnect( "CameraFreeMoveUp" );
	KeyEvent_disconnect( "CameraFreeMoveDown" );

	KeyEvent_disconnect( "CameraFreeFocus" );
}

/* ---- KenRadiant: measure tool - click two points on surfaces in the 3D view ---- */
bool g_krMeasure = false;
static int s_measureCount = 0;               // points placed: 0, 1 (second follows the mouse), 2
static Vector3 s_measureA, s_measureB;
static bool s_measureHover = false;
ToggleItem g_measure_item{ BoolExportCaller( g_krMeasure ) };
static void Measure_toggle(){
	g_krMeasure = !g_krMeasure;
	s_measureCount = 0;
	s_measureHover = false;
	g_measure_item.update();
	globalOutputStream() << ( g_krMeasure ? "measure: click two points in the 3D view (Shift: snap to the grid)\n" : "measure: off\n" );
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}
static std::optional<Vector3> Measure_point( const View& view, const DeviceVector device, const DeviceVector epsilon, bool snap ){
	if( const auto hit = testSelect_unselected_scene_point( view, device, epsilon ) ){
		Vector3 p( hit->point );
		if( snap )
			p = vector3_snapped( p, GetSnapGridSize() );
		return p;
	}
	return {};
}

class CamGLWidget : public QOpenGLWidget
{
	CamWnd& m_camwnd;
	FBO *m_fbo{};
	qreal m_scale;
public:
	CamGLWidget( CamWnd& camwnd ) : QOpenGLWidget(), m_camwnd( camwnd ) {
		setMouseTracking( true );
		setAcceptDrops( true ); // KenRadiant: models dragged from the model browser
	}

	/// where a dragged model goes: the surface under the cursor, else 256 units in front of the camera
	Vector3 dropPoint( const QPoint& pos ) const {
		const float w = width() * m_scale, h = height() * m_scale;
		const DeviceVector device( 2.f * pos.x() * m_scale / w - 1.f, 1.f - 2.f * pos.y() * m_scale / h );
		ModelDrop_park();
		if( const auto hit = testSelect_unselected_scene_point( m_camwnd.getView(), device, DeviceVector( 4.f / w, 4.f / h ) ) ){
			return vector3_snapped( Vector3( hit->point ), GetSnapGridSize() );
		}
		return vector3_snapped( Camera_getOrigin( m_camwnd ) - Camera_getViewVector( m_camwnd ) * 256.f, GetSnapGridSize() );
	}
	void dragEnterEvent( QDragEnterEvent *event ) override {
		if( event->mimeData()->hasFormat( c_modelDropMime ) ){
			event->acceptProposedAction();
			ModelDrop_begin( event->mimeData()->data( c_modelDropMime ).constData(), dropPoint( event->pos() ) );
		}
	}
	void dragMoveEvent( QDragMoveEvent *event ) override {
		if( ModelDrop_active() ){
			event->acceptProposedAction();
			ModelDrop_move( dropPoint( event->pos() ) );
		}
	}
	void dragLeaveEvent( QDragLeaveEvent *event ) override {
		ModelDrop_end( false );
	}
	void dropEvent( QDropEvent *event ) override {
		if( ModelDrop_active() ){
			ModelDrop_move( dropPoint( event->pos() ) );
			ModelDrop_end( true );
			event->acceptProposedAction();
			activateWindow();
			setFocus();
		}
	}

	~CamGLWidget() override {
		delete m_fbo;
		glwidget_context_destroyed();
	}

protected:
	void initializeGL() override
	{
		glwidget_context_created( *this );
	}
	void resizeGL( int w, int h ) override
	{
		m_scale = devicePixelRatioF();
		m_camwnd.getCamera().width = float_to_integer( w * m_scale );
		m_camwnd.getCamera().height = float_to_integer( h * m_scale );
		Camera_updateProjection( m_camwnd.getCamera() );
		m_camwnd.m_window_observer->onSizeChanged( m_camwnd.getCamera().width, m_camwnd.getCamera().height );

		m_camwnd.m_drawRequired = true;

		delete m_fbo;
		m_fbo = new FBO( m_camwnd.getCamera().width, m_camwnd.getCamera().height, true, g_camwindow_globals_private.m_MSAA );
	}
	void paintGL() override
	{
		if( m_fbo->m_samples != g_camwindow_globals_private.m_MSAA ){
			delete m_fbo;
			m_fbo = new FBO( m_camwnd.getCamera().width, m_camwnd.getCamera().height, true, g_camwindow_globals_private.m_MSAA );
		}

		if ( Map_Valid( g_map ) && ScreenUpdates_Enabled() && m_fbo->bind() ) {
			if( m_camwnd.m_drawRequired ){
				m_camwnd.m_drawRequired = false;
				m_camwnd.draw();
			}
			m_fbo->blit();
			m_fbo->release();
			m_camwnd.m_XORRectangle.render( m_camwnd.m_XORRect, m_camwnd.getCamera().width, m_camwnd.getCamera().height );
			GlobalOpenGL_debugAssertNoErrors();
		}
	}

	DeviceVector deviceFor( const QMouseEvent *event ) const {
		const float w = width() * m_scale, h = height() * m_scale;
		return DeviceVector( 2.f * event->localPos().x() * m_scale / w - 1.f, 1.f - 2.f * event->localPos().y() * m_scale / h );
	}
	DeviceVector measureEpsilon() const {
		return DeviceVector( 4.f / ( width() * m_scale ), 4.f / ( height() * m_scale ) );
	}
	void terrainPaint( int phase, const QMouseEvent *event ){
		const DeviceVector d = deviceFor( event ), e = measureEpsilon();
		KrTerrainPaint_mouse( phase, m_camwnd.getView(), d.x(), d.y(), e.x(), e.y() );
	}
	void mousePressEvent( QMouseEvent *event ) override {
		if( KrTerrainPaint_active() && !m_camwnd.m_bFreeMove && event->button() == Qt::LeftButton ){ // KenRadiant terrain paint
			setFocus();
			m_krPainting = true;
			terrainPaint( 0, event );
			return;
		}
		if( g_krMeasure && !m_camwnd.m_bFreeMove && event->button() == Qt::LeftButton ){ // KenRadiant measure tool
			setFocus();
			if( const auto p = Measure_point( m_camwnd.getView(), deviceFor( event ), measureEpsilon(), event->modifiers() & Qt::ShiftModifier ) ){
				if( s_measureCount != 1 ){
					s_measureA = *p;
					s_measureCount = 1;
				}
				else{
					s_measureB = *p;
					s_measureCount = 2;
					const Vector3 d = s_measureB - s_measureA;
					globalOutputStream() << "measure: " << vector3_length( d ) << " units (x " << std::fabs( d.x() ) << ", y " << std::fabs( d.y() ) << ", z " << std::fabs( d.z() ) << ")\n";
				}
				CamWnd_Update( m_camwnd );
			}
			return;
		}
		if( !m_camwnd.m_bFreeMove ){
			setFocus();
			selection_button_press( scaledEvent( event ), m_camwnd.m_window_observer );
			enable_freelook_button_press( scaledEvent( event ), m_camwnd );
		}
		else{
			selection_button_press_freemove( this, scaledEvent( event ), m_camwnd.m_window_observer );
			disable_freelook_button_press( scaledEvent( event ), m_camwnd );
		}
	}
	/// KenRadiant: double click an entity (model, light, door brush ...) = select it and open the entity window
	void mouseDoubleClickEvent( QMouseEvent *event ) override {
		if( event->button() == Qt::LeftButton && ( event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::ShiftModifier ) && !m_camwnd.m_bFreeMove && !g_krMeasure ){
			m_krSwallowRelease = true; // the release of the second click must not run a click-select (it would cycle to the next object)
			extern bool KrSelection_selectEntityAt( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon );
			const float w = width() * m_scale, h = height() * m_scale;
			const DeviceVector device( 2.f * event->localPos().x() * m_scale / w - 1.f, 1.f - 2.f * event->localPos().y() * m_scale / h );
			if( KrSelection_selectEntityAt( m_camwnd.getView(), device, DeviceVector( 6.f / w, 6.f / h ) ) ){
				extern void GroupDialog_raisePage( QWidget* page );
				extern QWidget* g_page_entity;
				GroupDialog_raisePage( g_page_entity );
			}
			return; // no press: a double click must not start dragging the gizmo
		}
		mousePressEvent( event );
	}
	bool m_krPainting = false;
	void mouseMoveEvent( QMouseEvent *event ) override {
		if( KrTerrainPaint_active() && !m_camwnd.m_bFreeMove ){
			terrainPaint( m_krPainting ? 1 : 3, event );
			if( m_krPainting )
				return;
		}
		if( g_krMeasure && !m_camwnd.m_bFreeMove && s_measureCount == 1 ){
			if( const auto p = Measure_point( m_camwnd.getView(), deviceFor( event ), measureEpsilon(), event->modifiers() & Qt::ShiftModifier ) ){
				s_measureB = *p;
				s_measureHover = true;
				CamWnd_Update( m_camwnd );
			}
			return;
		}
		if( !m_camwnd.m_bFreeMove ){
			m_camwnd.m_deferred_motion.motion( scaledEvent( event ) );
			m_camwnd.getCamera().m_idleDraw.queueDraw( DeferredMotion2::InvokeCaller( m_camwnd.m_deferred_motion ), false );
		}
		else{
			;
		}
	}
	bool m_krSwallowRelease = false;
	void mouseReleaseEvent( QMouseEvent *event ) override {
		if( m_krPainting && event->button() == Qt::LeftButton ){
			m_krPainting = false;
			terrainPaint( 2, event );
			return;
		}
		if( m_krSwallowRelease && event->button() == Qt::LeftButton ){
			m_krSwallowRelease = false;
			return;
		}
		if( g_krMeasure && !m_camwnd.m_bFreeMove && event->button() == Qt::LeftButton )
			return;
		if( !m_camwnd.m_bFreeMove ){
			selection_button_release( scaledEvent( event ), m_camwnd.m_window_observer );
		}
		else{
			selection_button_release_freemove( this, scaledEvent( event ), m_camwnd.m_window_observer );
			disable_freelook_button_release( scaledEvent( event ), m_camwnd );
		}
	}
	void wheelEvent( QWheelEvent *event ) override {
		setFocus();
		if( !m_camwnd.m_parent->isActiveWindow() ){
			m_camwnd.m_parent->activateWindow();
			m_camwnd.m_parent->raise();
		}
		wheelmove_scroll( scaledEvent( event ), m_camwnd );
	}
private:
	QMouseEvent scaledEvent( const QMouseEvent *event ) const {
		return QMouseEvent( event->type(), event->localPos() * m_scale, event->windowPos() * m_scale, event->screenPos() * m_scale, event->button(), event->buttons(), event->modifiers() );
	}
	QWheelEvent scaledEvent( const QWheelEvent *event ) const {
		return QWheelEvent( event->position() * m_scale, event->globalPosition() * m_scale, event->pixelDelta(), event->angleDelta(), event->buttons(), event->modifiers(), event->phase(), false );
	}
};

CamWnd::CamWnd() :
	m_view( true ),
	m_Camera( &m_view, ReferenceCaller<CamWnd, void(), CamWnd_Update>( *this ), CamWnd_selection_motion_freemove( *this ) ),
	m_cameraview( m_Camera, &m_view, ReferenceCaller<CamWnd, void(), CamWnd_Update>( *this ) ),
	m_gl_widget( new CamGLWidget( *this ) ),
	m_window_observer( NewWindowObserver() ),
	m_deferredDraw( WidgetQueueDrawCaller( *m_gl_widget ) ),
	m_deferred_motion( [this]( const QMouseEvent& event ){ selection_motion( event, m_window_observer ); } ),
	m_drawing( false )
{
	m_bFreeMove = false;

	GlobalWindowObservers_add( m_window_observer );
	GlobalWindowObservers_connectWidget( m_gl_widget );

	m_window_observer->setRectangleDrawCallback( ReferenceCaller<CamWnd, void(rect_t), camwnd_update_xor_rectangle>( *this ) );
	m_window_observer->setView( m_view );

	Map_addValidCallback( g_map, DeferredDrawOnMapValidChangedCaller( m_deferredDraw ) ); //. correct would be m_drawRequired = true here

	CamWnd_registerCommands( *this );

	CamWnd_Add_Handlers_Move( *this );

	AddSceneChangeCallback( ReferenceCaller<CamWnd, void(), CamWnd_Update>( *this ) );
}

CamWnd::~CamWnd(){
	if ( m_bFreeMove ) {
		DisableFreeMove();
	}

	CamWnd_Remove_Handlers_Move( *this );

	m_window_observer->release();
}

class FloorHeightWalker : public scene::Graph::Walker
{
	Vector3 m_current;
public:
	mutable float m_bestUp;
	mutable float m_bestDown;
	mutable float m_bottom;
	FloorHeightWalker( const Vector3& current ) :
		m_current( current ), m_bestUp( g_MaxWorldCoord ), m_bestDown( g_MinWorldCoord ), m_bottom( g_MaxWorldCoord ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( !path.top().get().visible() )
			return false;
		if ( !path.top().get().isRoot() && !node_is_group( path.top() ) ) {
			const AABB& aabb = instance.worldAABB();
			if( instance.isSelected() || ( m_current.x() > aabb.origin.x() - aabb.extents.x()
			                               && m_current.x() < aabb.origin.x() + aabb.extents.x()
			                               && m_current.y() > aabb.origin.y() - aabb.extents.y()
			                               && m_current.y() < aabb.origin.y() + aabb.extents.y() ) ){
				const float floorHeight = aabb.origin.z() + aabb.extents.z() + 32.f;
				if ( floorHeight > m_current.z() + 0.1f && floorHeight < m_bestUp ) /* 0.1f epsilon to prevent jam at (close?) coords */
					m_bestUp = floorHeight;
				if ( floorHeight < m_current.z() - 0.1f && floorHeight > m_bestDown )
					m_bestDown = floorHeight;
				const float bottom = aabb.origin.z() - aabb.extents.z() - 16.f;
				if( m_bottom > bottom )
					m_bottom = bottom;
			}
		}
		return true;
	}
};

void CamWnd::Cam_ChangeFloor( bool up ){
	FloorHeightWalker walker( m_Camera.origin );
	GlobalSceneGraph().traverse( walker );
	float current = m_Camera.origin.z();

	if ( up ){
		if( walker.m_bottom != g_MaxWorldCoord && walker.m_bottom > current )
			current = walker.m_bottom;
		else if( walker.m_bestUp != g_MaxWorldCoord )
			current = walker.m_bestUp;
	}
	else{
		if ( walker.m_bestDown != g_MinWorldCoord )
			current = walker.m_bestDown;
		else if( walker.m_bottom != g_MaxWorldCoord && walker.m_bottom < current )
			current = walker.m_bottom;
	}

	if( m_Camera.origin.z() != current ){
		m_Camera.origin.z() = current;
		Camera_updateModelview( getCamera() );
		CamWnd_Update( *this );
		CameraMovedNotify();
	}
}


void CamWnd::EnableFreeMove(){
	//globalOutputStream() << "EnableFreeMove\n";

	ASSERT_MESSAGE( !m_bFreeMove, "EnableFreeMove: free-move was already enabled" );
	m_bFreeMove = true;
	Camera_clearMovementFlags( getCamera(), MOVE_ALL );

	CamWnd_Remove_Handlers_Move( *this );
	CamWnd_Add_Handlers_FreeMove( *this );

	m_gl_widget->setFocus();
	m_freezePointer.freeze_pointer( m_gl_widget,
		[this]( int x, int y, const QMouseEvent *event ){
			Camera_motionDelta( x, y, event, m_Camera );
		},
		[this](){
			DisableFreeMove(); // if there's an OS-level focus out of the application then we can release the camera cursor grab
		} );

	CamWnd_Update( *this );
}

void CamWnd::DisableFreeMove(){
	//globalOutputStream() << "DisableFreeMove\n";

	ASSERT_MESSAGE( m_bFreeMove, "DisableFreeMove: free-move was not enabled" );
	m_bFreeMove = false;
	Camera_clearMovementFlags( getCamera(), MOVE_ALL );

	CamWnd_Remove_Handlers_FreeMove( *this );
	CamWnd_Add_Handlers_Move( *this );

	m_freezePointer.unfreeze_pointer( true );

	CamWnd_Update( *this );
}


#include "renderer.h"

#include "ishaders.h"
class CamRenderer : public Renderer
{
	struct state_type
	{
		state_type() : m_highlight( 0 ), m_state( 0 ), m_lights( 0 ){
		}
		unsigned int m_highlight;
		Shader* m_state;
		const LightList* m_lights;
		Shader* m_terrain = nullptr; // KenRadiant: index-map terrain state for q3map_terrain faces
	};

	std::vector<state_type> m_state_stack;
	RenderStateFlags m_globalstate;
	Shader* m_state_facewire;
	Shader* m_state_wire;
	Shader* m_state_select0;
	Shader* m_state_select1;
	const Vector3& m_viewer;

public:
	CamRenderer( RenderStateFlags globalstate, Shader* facewire, Shader* wire, Shader* select0, Shader* select1, const Vector3& viewer ) :
		m_globalstate( globalstate ),
		m_state_facewire( facewire ),
		m_state_wire( wire ),
		m_state_select0( select0 ),
		m_state_select1( select1 ),
		m_viewer( viewer ){
		ASSERT_NOTNULL( select0 );
	//	ASSERT_NOTNULL( select1 );
		m_state_stack.push_back( state_type() );
	}

	void SetState( Shader* state, EStyle style ) override {
		ASSERT_NOTNULL( state );
		if ( style == eFullMaterials ) {
			if ( m_state_stack.back().m_terrain != nullptr && ( state->getFlags() & QER_KR_TERRAIN ) )
				state = m_state_stack.back().m_terrain; // KenRadiant: the layers blended by the index map
			m_state_stack.back().m_state = state;
		}
	}
	void KrSetTerrain( Shader* terrain ) override {
		m_state_stack.back().m_terrain = terrain;
	}
	EStyle getStyle() const override {
		return eFullMaterials;
	}
	void PushState() override {
		m_state_stack.push_back( m_state_stack.back() );
	}
	void PopState() override {
		ASSERT_MESSAGE( !m_state_stack.empty(), "popping empty stack" );
		m_state_stack.pop_back();
	}
	void Highlight( EHighlightMode mode, bool bEnable = true ) override {
		( bEnable )
		? m_state_stack.back().m_highlight |= mode
		: m_state_stack.back().m_highlight &= ~mode;
	}
	void setLights( const LightList& lights ) override {
		m_state_stack.back().m_lights = &lights;
	}
	bool m_outline = false; // KenRadiant: selected objects get an outline instead of overlay + wire
	bool m_outlineSkipBrushes = false; // ... except brushes / patches, which keep edges + tint
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& world ) override {
		if ( m_outline && ( m_state_stack.back().m_highlight & ePrimitive )
		  && !( m_outlineSkipBrushes && ( m_state_stack.back().m_highlight & eKrBrushLike ) ) ) {
			m_state_stack.back().m_state->addRenderable( renderable, world, m_state_stack.back().m_lights );
			return;
		}
		if ( m_state_stack.back().m_highlight & ePrimitive ) {
			m_state_select0->addRenderable( renderable, world, m_state_stack.back().m_lights );
		}
		else if ( m_state_wire && m_state_stack.back().m_highlight & ePrimitiveWire ) {
			m_state_wire->addRenderable( renderable, world, m_state_stack.back().m_lights );
		}
		if ( m_state_select1 && m_state_stack.back().m_highlight & eFace ) {
			m_state_select1->addRenderable( renderable, world, m_state_stack.back().m_lights );
		}
		if ( m_state_facewire && m_state_stack.back().m_highlight & eFaceWire ) {
			m_state_facewire->addRenderable( renderable, world, m_state_stack.back().m_lights );
		}

		m_state_stack.back().m_state->addRenderable( renderable, world, m_state_stack.back().m_lights );
	}

	void render( const Matrix4& modelview, const Matrix4& projection ){
		GlobalShaderCache().render( m_globalstate, modelview, projection, m_viewer );
	}
};

/*
   ==============
   Cam_Draw
   ==============
 */
/*
void ShowStatsToggle(){
	g_camwindow_globals_private.m_showStats ^= 1;
}

void ShowStatsExport( const BoolImportCallback& importer ){
	importer( g_camwindow_globals_private.m_showStats );
}

FreeCaller<void(const BoolImportCallback&), ShowStatsExport> g_show_stats_caller;
BoolExportCallback g_show_stats_callback( g_show_stats_caller );
ToggleItem g_show_stats( g_show_stats_callback );
*/
ToggleItem g_show_stats( BoolExportCaller( g_camwindow_globals.m_showStats ) );
void ShowStatsToggle(){
	g_camwindow_globals.m_showStats ^= 1;
	g_show_stats.update();
	UpdateAllWindows();
}

ToggleItem g_show_workzone3d( BoolExportCaller( g_camwindow_globals_private.m_bShowWorkzone ) );
void ShowWorkzone3dToggle(){
	g_camwindow_globals_private.m_bShowWorkzone ^= 1;
	g_show_workzone3d.update();
	if ( g_camwnd != 0 ) {
		CamWnd_Update( *g_camwnd );
	}
}

ToggleItem g_show_size3d( BoolExportCaller( g_camwindow_globals_private.m_bShowSize ) );
void ShowSize3dToggle(){
	g_camwindow_globals_private.m_bShowSize ^= 1;
	g_show_size3d.update();
	if ( g_camwnd != 0 ) {
		CamWnd_Update( *g_camwnd );
	}
}

#include "kr_lightpreview.h"
#include "kr_terrain.h"
#include "stringio.h"
#include "ientity.h"
#include "ishaders.h"
std::vector<CopiedString> Scene_findSkyShaders();
KrSun g_krSun;
static bool s_krSunDirty = true;
static void KrSun_sceneChanged(){ s_krSunDirty = true; }
void KrSun_changed(); // calibrator window refresh
static void KrSun_readMap(){
	s_krSunDirty = false;
	g_krSun.valid = false;
	g_krSun.shader = "";
	for( const CopiedString& name : Scene_findSkyShaders() ){
		IShader* shader = GlobalShaderSystem().getShaderForName( name.c_str() );
		if( shader == nullptr )
			continue;
		float colour[3], intensity, degrees, elevation, sky;
		if( shader->getSun( colour, intensity, degrees, elevation, sky ) ){
			g_krSun.valid = true;
			g_krSun.shader = name;
			g_krSun.colour = Vector3( colour[0], colour[1], colour[2] );
			g_krSun.intensity = intensity;
			g_krSun.degrees = degrees;
			g_krSun.elevation = elevation;
			g_krSun.skyLight = sky;
			shader->getSunExt( g_krSun.ext, g_krSun.deviance, g_krSun.samples, g_krSun.skyIterations );
			shader->DecRef();
			break;
		}
		shader->DecRef();
	}
	KrSun_changed();
}
static void KrSun_update(){
	static bool registered = false;
	if( !registered ){
		registered = true;
		AddSceneChangeCallback( FreeCaller<void(), KrSun_sceneChanged>() );
	}
	extern int g_krMapGeneration;
	static int generation = -1;
	static QElapsedTimer lastScan;
	if( generation != g_krMapGeneration ){
		generation = g_krMapGeneration;
		g_krSun.overridden = false; // new map: back to its own sky shader
		s_krSunDirty = true;
	}
	else if( s_krSunDirty && lastScan.isValid() && lastScan.elapsed() < 500 )
		return; // dragging things around: rescan at most twice a second
	if( !s_krSunDirty || g_krSun.overridden )
		return;
	lastScan.start();
	KrSun_readMap();
}
void KrSun_rescan(){
	g_krSun.overridden = false;
	KrSun_readMap();
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}

/// gathers the lights for the live preview: light entities (origin, "light" intensity, "_color"), worldspawn ambient,
/// and the sun of the first loaded sky shader with q3map_sun / q3map_skyLight
#include <map>
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
static void KrLightPreview_collect( const Vector3& viewer ){
	struct Candidate{ KrPreviewLight light; float score; CopiedString target; };
	std::vector<Candidate> candidates;
	std::map<CopiedString, Vector3> targets; // targetname -> origin (spotlight targets)
	float ambient = 0, minlight = 0;
	class LightWalker : public scene::Traversable::Walker
	{
		std::vector<Candidate>& m_candidates; std::map<CopiedString, Vector3>& m_targets; float& m_ambient; float& m_minlight; const Vector3& m_viewer;
	public:
		LightWalker( std::vector<Candidate>& c, std::map<CopiedString, Vector3>& t, float& a, float& m, const Vector3& v )
			: m_candidates( c ), m_targets( t ), m_ambient( a ), m_minlight( m ), m_viewer( v ){}
		bool pre( scene::Node& node ) const override {
			Entity* entity = Node_getEntity( node );
			if( entity == nullptr ) // filtered (e.g. Game View) lights still shine, only their icon is hidden
				return false;
			const char* classname = entity->getClassName();
			if( string_equal( classname, "worldspawn" ) ){
				float a = 0;
				if( string_parse_float( entity->getKeyValue( "_ambient" ), a ) || string_parse_float( entity->getKeyValue( "ambient" ), a ) )
					m_ambient = a;
				if( string_parse_float( entity->getKeyValue( "_minlight" ), a ) )
					m_minlight = a;
				return false;
			}
			Vector3 origin;
			const bool hasOrigin = string_parse_vector3( entity->getKeyValue( "origin" ), origin );
			if( hasOrigin && !string_empty( entity->getKeyValue( "targetname" ) ) )
				m_targets.emplace( entity->getKeyValue( "targetname" ), origin );
			if( !string_equal_prefix( classname, "light" ) || !hasOrigin )
				return false;
			// as q3map2 SetupLights(): _light / light (0 = 300), scale, _color normalised, spawnflags 1 linear (no angle
			// attenuation, fade), 2 no angle attenuation, _anglescale, a target makes it a spotlight
			float intensity = 0;
			if( !string_parse_float( entity->getKeyValue( "_light" ), intensity ) )
				string_parse_float( entity->getKeyValue( "light" ), intensity );
			if( intensity == 0 )
				intensity = 300;
			float scale = 1;
			if( string_parse_float( entity->getKeyValue( "scale" ), scale ) && scale != 0 )
				intensity *= scale;
			if( intensity <= 0 )
				return false; // dark lights: not previewed
			Vector3 colour( 1, 1, 1 );
			if( string_parse_vector3( entity->getKeyValue( "_color" ), colour ) ){
				const float m = std::max( { colour.x(), colour.y(), colour.z() } );
				colour = m > 0 ? colour / m : Vector3( 1, 1, 1 );
			}
			int spawnflags = 0;
			string_parse_int( entity->getKeyValue( "spawnflags" ), spawnflags );
			KrPreviewLight light;
			light.origin = origin;
			light.colour = colour;
			string_parse_float( entity->getKeyValue( "_anglescale" ), light.angleScale );
			const char* target = entity->getKeyValue( "target" );
			const bool linear = ( spawnflags & 1 ) && string_empty( target ); // spotlights are always inverse square
			light.noAngle = ( linear || ( spawnflags & 2 ) ) && light.angleScale == 0 && string_empty( target );
			if( linear ){
				float fade = 0;
				string_parse_float( entity->getKeyValue( "fade" ), fade );
				if( fade == 0 )
					fade = 1;
				light.falloff = -intensity * 7500.f / 8000.f / fade; // ( light * 7500 / 8000 - dist * fade ) = fade * ( that / fade - dist )
				light.colour *= fade;
				light.radius = std::min( -light.falloff, 8192.f );
			}
			else{
				light.falloff = intensity * 7500.f / 255.f;
				light.radius = std::min( std::sqrt( intensity * 7500.f ), 4096.f ); // below 1/255 further away
			}
			// prefer lights that reach the camera's surroundings
			m_candidates.push_back( { light, vector3_length( origin - m_viewer ) - light.radius, target } );
			if( !string_empty( entity->getKeyValue( "radius" ) ) ) // spot radius, read when the target is known
				m_candidates.back().light.spotRadiusByDist = -std::max( 1.f, float( atof( entity->getKeyValue( "radius" ) ) ) );
			return false;
		}
	};
	Node_getTraversable( GlobalSceneGraph().root() )->traverse( LightWalker( candidates, targets, ambient, minlight, viewer ) );
	// spotlights: cone towards the target
	for( Candidate& c : candidates ){
		KrPreviewLight& l = c.light;
		const float radiusKey = l.spotRadiusByDist < 0 ? -l.spotRadiusByDist : 64.f;
		l.spotRadiusByDist = 0;
		if( c.target.empty() )
			continue;
		const auto it = targets.find( c.target );
		if( it == targets.end() )
			continue; // missing target: q3map2 keeps it a point light
		Vector3 dir = it->second - l.origin;
		float dist = vector3_length( dir );
		if( dist == 0 )
			dist = 64;
		else
			dir /= dist;
		l.spotDir = dir;
		l.spotRadiusByDist = ( radiusKey + 16 ) / dist;
	}
	std::sort( candidates.begin(), candidates.end(), []( const Candidate& a, const Candidate& b ){ return a.score < b.score; } );
	g_krLightPreviewLights.clear();
	for( std::size_t i = 0; i < candidates.size() && i < std::min( std::size_t( std::max( g_krPreviewLightCount, 1 ) ), std::size_t( g_krMaxPreviewLightsGPU ) ); ++i )
		g_krLightPreviewLights.push_back( candidates[i].light );

	// the lights texture: light data, then per grid cell the lights that reach it (strongest first)
	{
		const Vector3 snapped( float_snapped( viewer.x(), float( c_krCellSize ) ), float_snapped( viewer.y(), float( c_krCellSize ) ), float_snapped( viewer.z(), float( c_krCellSize ) ) );
		const Vector3 gridMin = snapped - Vector3( c_krGridX / 2, c_krGridY / 2, c_krGridZ / 2 ) * float( c_krCellSize );
		static std::vector<float> lastLights;
		std::vector<float> key( g_krLightPreviewLights.size() * c_krLightTexels * 4 + 3 );
		for( std::size_t i = 0; i < g_krLightPreviewLights.size(); ++i ){
			const KrPreviewLight& l = g_krLightPreviewLights[i];
			float* t = key.data() + i * c_krLightTexels * 4;
			t[0] = l.origin.x(); t[1] = l.origin.y(); t[2] = l.origin.z(); t[3] = l.falloff;
			t[4] = l.colour.x(); t[5] = l.colour.y(); t[6] = l.colour.z(); t[7] = l.radius * l.radius;
			t[8] = l.spotDir.x(); t[9] = l.spotDir.y(); t[10] = l.spotDir.z(); t[11] = l.spotRadiusByDist;
			t[12] = l.noAngle ? 1.f : 0.f; t[13] = l.angleScale; t[14] = 0; t[15] = 0;
		}
		std::copy( gridMin.data(), gridMin.data() + 3, key.end() - 3 );
		extern int g_krMapGeneration;
		static int generation = -1;
		if( key != lastLights || g_krLightTex == 0 || generation != g_krMapGeneration ){
			generation = g_krMapGeneration;
			std::vector<float> data( std::size_t( c_krLightTexW ) * c_krLightTexRows * 4, -1.f );
			std::copy( key.begin(), key.end() - 3, data.begin() );
			lastLights = std::move( key );
			g_krGridMin = gridMin;
			// per cell: ( strength at the cell, light ) of every light whose reach touches the cell box
			std::vector<std::vector<std::pair<float, int>>> cells( c_krGridX * c_krGridY * c_krGridZ );
			for( std::size_t i = 0; i < g_krLightPreviewLights.size(); ++i ){
				const KrPreviewLight& l = g_krLightPreviewLights[i];
				const Vector3 lo = ( l.origin - Vector3( l.radius, l.radius, l.radius ) - gridMin ) / float( c_krCellSize );
				const Vector3 hi = ( l.origin + Vector3( l.radius, l.radius, l.radius ) - gridMin ) / float( c_krCellSize );
				const int x0 = std::max( 0, int( std::floor( lo.x() ) ) ), x1 = std::min( c_krGridX - 1, int( std::floor( hi.x() ) ) );
				const int y0 = std::max( 0, int( std::floor( lo.y() ) ) ), y1 = std::min( c_krGridY - 1, int( std::floor( hi.y() ) ) );
				const int z0 = std::max( 0, int( std::floor( lo.z() ) ) ), z1 = std::min( c_krGridZ - 1, int( std::floor( hi.z() ) ) );
				const float peak = std::max( { l.colour.x(), l.colour.y(), l.colour.z() } );
				for( int z = z0; z <= z1; ++z )
					for( int y = y0; y <= y1; ++y )
						for( int x = x0; x <= x1; ++x ){
							const Vector3 cmin = gridMin + Vector3( x, y, z ) * float( c_krCellSize );
							const Vector3 nearest( std::clamp( l.origin.x(), cmin.x(), cmin.x() + c_krCellSize ),
							                       std::clamp( l.origin.y(), cmin.y(), cmin.y() + c_krCellSize ),
							                       std::clamp( l.origin.z(), cmin.z(), cmin.z() + c_krCellSize ) );
							const float d2 = vector3_length_squared( nearest - l.origin );
							if( d2 > l.radius * l.radius )
								continue;
							const float strength = peak * ( l.falloff > 0 ? l.falloff / std::max( d2, 256.f ) : std::max( -l.falloff - std::sqrt( d2 ), 0.f ) / 255.f );
							cells[( z * c_krGridY + y ) * c_krGridX + x].emplace_back( strength, int( i ) );
						}
			}
			float* list = data.data() + std::size_t( c_krMaxPreviewLights ) * c_krLightTexels * 4;
			for( std::size_t c = 0; c < cells.size(); ++c ){
				auto& v = cells[c];
				if( v.size() > std::size_t( c_krCellSlots ) )
					std::partial_sort( v.begin(), v.begin() + c_krCellSlots, v.end(), []( const auto& a, const auto& b ){ return a.first > b.first; } );
				for( std::size_t k = 0; k < v.size() && k < std::size_t( c_krCellSlots ); ++k )
					list[c * c_krCellSlots + k] = float( v[k].second );
			}
			if( g_krLightTex == 0 ){
				gl().glGenTextures( 1, &g_krLightTex );
				gl().glBindTexture( GL_TEXTURE_2D, g_krLightTex );
				gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
				gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
				gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
				gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
				gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, c_krLightTexW, c_krLightTexRows, 0, GL_RGBA, GL_FLOAT, data.data() );
			}
			else{
				gl().glBindTexture( GL_TEXTURE_2D, g_krLightTex );
				gl().glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, c_krLightTexW, c_krLightTexRows, GL_RGBA, GL_FLOAT, data.data() );
			}
			gl().glBindTexture( GL_TEXTURE_2D, 0 );
		}
	}

	// sun from the sky shader on the map's sky brushes (rescanned when the map or the scene changes)
	KrSun_update();
	g_krSunColour = Vector3( 0, 0, 0 );
	float skyLight = 0;
	if( g_krSun.valid ){
		const float az = degrees_to_radians( g_krSun.degrees ), el = degrees_to_radians( g_krSun.elevation );
		g_krSunDirection = Vector3( cos( el ) * cos( az ), cos( el ) * sin( az ), sin( el ) );
		g_krSunColour = g_krSun.colour * ( g_krSun.intensity / 255.f ); // lightmap units like q3map2
		skyLight = g_krSun.skyLight;
	}
	// sun softness from q3map_sunExt deviance (degrees): wider shadow filtering
	g_krSunSoftness = 1 + ( g_krSun.ext ? std::clamp( g_krSun.deviance, 0.f, 30.f ) * 1.5f : 0.f );
	// bounce approximation: a share of the direct light comes back as fill (real bounce needs a compile)
	g_krBounceFill = Vector3( 0, 0, 0 );
	if( g_krCompile.enabled && g_krCompile.bounce > 0 ){
		Vector3 lights( 0, 0, 0 );
		for( const KrPreviewLight& l : g_krLightPreviewLights )
			lights += l.colour;
		if( !g_krLightPreviewLights.empty() )
			lights /= float( g_krLightPreviewLights.size() );
		g_krBounceFill = ( g_krSunColour * 0.35f + lights * 0.1f ) * ( 1 - std::pow( 0.6f, float( g_krCompile.bounce ) ) ) * 0.6f;
	}
	// ambient: worldspawn _ambient (0..255 like q3map2); q3map_skyLight as light from the sky dome (tinted a little by the sun)
	g_krAmbient = Vector3( 1, 1, 1 ) * std::clamp( ambient / 255.f, 0.f, 1.f );
	const Vector3 tint = vector3_length_squared( g_krSunColour ) > 0 ? vector3_normalised( g_krSun.colour ) * 1.732f : Vector3( 1, 1, 1 );
	// an open floor gets about skyLight / 400 (lightmap units); walls, corners and rooms get less - the sky maps see the roofs
	g_krSkyAmbient = ( Vector3( 1, 1, 1 ) * 0.7f + tint * 0.3f ) * std::clamp( skyLight / 400.f, 0.f, 1.5f );
	g_krMinLight = std::clamp( minlight / 255.f, 0.f, 1.f );
	if( ambient <= 0 && minlight <= 0 ) // never fully black (q3map2's bounce light fills rooms a little)
		g_krAmbient = Vector3( 1, 1, 1 ) * ( skyLight > 0 ? 0.035f : 0.06f );
}

/* ---- KenRadiant: sun shadow map for the live light preview ---- */
#include <QOpenGLContext>
#include <QOpenGLFunctions>

/// only opaque textured surfaces go to the shadow map (no entity boxes, overlays, sky, translucent)
class ShadowRenderer : public Renderer
{
	struct State{ Shader* shader = nullptr; bool noShadow = false; Shader* terrain = nullptr; };
	std::vector<State> m_stack{ State() };
public:
	void SetState( Shader* state, EStyle style ) override {
		if ( style == eFullMaterials ){
			m_stack.back().shader = Shader_castsShadow( state ) ? state : nullptr;
			if ( m_stack.back().shader != nullptr && m_stack.back().terrain != nullptr && ( state->getFlags() & QER_KR_TERRAIN ) )
				m_stack.back().shader = m_stack.back().terrain; // opaque terrain state (the tool shader writes no depth)
		}
	}
	void KrSetTerrain( Shader* terrain ) override {
		m_stack.back().terrain = terrain;
	}
	EStyle getStyle() const override { return eFullMaterials; }
	void PushState() override { m_stack.push_back( m_stack.back() ); }
	void PopState() override { m_stack.pop_back(); }
	void Highlight( EHighlightMode mode, bool bEnable = true ) override {
		if( mode & eKrNoShadow ) // entities with _castShadows 0 (and doors etc. without _castShadows 1), like q3map2
			m_stack.back().noShadow = bEnable;
	}
	void setLights( const LightList& lights ) override {}
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& world ) override {
		if( m_stack.back().shader != nullptr && !m_stack.back().noShadow )
			m_stack.back().shader->addRenderable( renderable, world );
	}
};

static GLuint s_shadowFbo = 0, s_shadowDepthTex = 0;
static int s_shadowSize = 0;
static bool s_shadowDirty = true;
static Vector3 s_shadowCentre( 1e9f, 0, 0 ), s_shadowSun( 0, 0, 0 );
static void KrShadow_sceneChanged(){ s_shadowDirty = true; }

static Matrix4 kr_ortho( float l, float r, float b, float t, float n, float f ){
	return Matrix4( 2 / ( r - l ), 0, 0, 0,
	                0, 2 / ( t - b ), 0, 0,
	                0, 0, -2 / ( f - n ), 0,
	                -( r + l ) / ( r - l ), -( t + b ) / ( t - b ), -( f + n ) / ( f - n ), 1 );
}
static Matrix4 kr_lookat( const Vector3& eye, const Vector3& forward ){
	const Vector3 up = std::fabs( forward.z() ) > 0.99f ? Vector3( 1, 0, 0 ) : Vector3( 0, 0, 1 );
	const Vector3 side = vector3_normalised( vector3_cross( forward, up ) );
	const Vector3 u = vector3_cross( side, forward );
	return Matrix4( side.x(), u.x(), -forward.x(), 0,
	                side.y(), u.y(), -forward.y(), 0,
	                side.z(), u.z(), -forward.z(), 0,
	                -vector3_dot( side, eye ), -vector3_dot( u, eye ), vector3_dot( forward, eye ), 1 );
}


/* hidden shadow casters: brush faces that are filtered out of the view (caulk / nodraw in Game View, hidden brushes,
   hidden layers) still block light in the compiled map, so they go into every shadow pass too */
void Scene_collectHiddenCasters( std::vector<Vector3>& triangles );
static std::vector<Vector3> s_hiddenCasters;
static bool s_hiddenCastersDirty = true;
static int s_hiddenCastersGeneration = -1;
static void KrHiddenCasters_sceneChanged(){ s_hiddenCastersDirty = true; }
static void KrHiddenCasters_draw( const Matrix4& view, const Matrix4& proj ){
	static bool registered = false;
	if( !registered ){
		registered = true;
		AddSceneChangeCallback( FreeCaller<void(), KrHiddenCasters_sceneChanged>() );
	}
	extern int g_krMapGeneration;
	if( s_hiddenCastersDirty || s_hiddenCastersGeneration != g_krMapGeneration ){
		s_hiddenCastersDirty = false;
		s_hiddenCastersGeneration = g_krMapGeneration;
		Scene_collectHiddenCasters( s_hiddenCasters );
	}
	if( s_hiddenCasters.empty() )
		return;
	gl().glUseProgram( 0 );
	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &proj ) );
	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &view ) );
	gl().glDisable( GL_CULL_FACE );
	gl().glDisable( GL_BLEND );
	gl().glDisable( GL_ALPHA_TEST );
	gl().glDisable( GL_TEXTURE_2D );
	gl().glEnable( GL_DEPTH_TEST );
	gl().glDepthFunc( GL_LESS );
	gl().glDepthMask( GL_TRUE );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
	gl().glDisableClientState( GL_COLOR_ARRAY );
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glEnableClientState( GL_VERTEX_ARRAY );
	gl().glVertexPointer( 3, GL_FLOAT, sizeof( Vector3 ), s_hiddenCasters.data()->data() );
	gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( s_hiddenCasters.size() ) );
}

/// renders the scene's depth from the sun into a 4096² map around the camera (only when the scene, sun or area changes)
static void KrShadow_render( camera_t& camera ){
	static bool registered = false;
	if( !registered ){
		registered = true;
		AddSceneChangeCallback( FreeCaller<void(), KrShadow_sceneChanged>() );
	}
	if( !g_krShadows || vector3_length_squared( g_krSunColour ) == 0 ){
		g_krShadowTex = 0;
		return;
	}
	QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
	constexpr int size = 4096;
	constexpr float halfExtent = 3072, depthRange = 32768;
	if( s_shadowFbo == 0 || s_shadowSize != size ){
		GLuint tex = 0;
		gl().glGenTextures( 1, &tex );
		gl().glBindTexture( GL_TEXTURE_2D, tex );
		gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL );
		gl().glBindTexture( GL_TEXTURE_2D, 0 );
		f->glGenFramebuffers( 1, &s_shadowFbo );
		GLint previous = 0;
		gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
		f->glBindFramebuffer( GL_FRAMEBUFFER, s_shadowFbo );
		f->glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0 );
		gl().glDrawBuffer( GL_NONE );
		gl().glReadBuffer( GL_NONE );
		const bool ok = f->glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
		f->glBindFramebuffer( GL_FRAMEBUFFER, previous );
		if( !ok ){
			globalWarningStream() << "light preview: shadow map framebuffer not supported, sun shadows off\n";
			g_krShadows = false;
			return;
		}
		s_shadowDepthTex = tex;
		s_shadowSize = size;
		s_shadowDirty = true;
	}
	g_krShadowTex = s_shadowDepthTex; // (a map without sun cleared it)
	// the map follows the camera in 512-unit steps, so shadows only re-render when you move far
	const Vector3 origin = Camera_getOrigin( camera );
	const Vector3 centre( float_snapped( origin.x(), 512.f ), float_snapped( origin.y(), 512.f ), float_snapped( origin.z(), 512.f ) );
	const Vector3 forward = -vector3_normalised( g_krSunDirection );
	const Matrix4 lightView = kr_lookat( centre - forward * ( depthRange * 0.5f ), forward );
	const Matrix4 lightProj = kr_ortho( -halfExtent, halfExtent, -halfExtent, halfExtent, 1, depthRange );
	g_krShadowTexel = 1.f / size;
	g_krShadowBiasUnit = ( 2 * halfExtent / size ) / depthRange;

	extern int g_krMapGeneration;
	static int s_shadowMapGeneration = -1;
	if( s_shadowMapGeneration != g_krMapGeneration ){ // another map loaded: the cached depth belongs to the old one
		s_shadowMapGeneration = g_krMapGeneration;
		s_shadowDirty = true;
	}
	if( s_shadowDirty || !vector3_equal( centre, s_shadowCentre ) || !vector3_equal_epsilon( g_krSunDirection, s_shadowSun, 1e-4f ) ){
		s_shadowDirty = false;
		s_shadowCentre = centre;
		s_shadowSun = g_krSunDirection;

		GLint previous = 0, viewport[4];
		gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
		gl().glGetIntegerv( GL_VIEWPORT, viewport );
		f->glBindFramebuffer( GL_FRAMEBUFFER, s_shadowFbo );
		gl().glViewport( 0, 0, size, size );
		gl().glDepthMask( GL_TRUE );
		gl().glClear( GL_DEPTH_BUFFER_BIT );

		View view( true );
		view.Construct( lightProj, lightView, size, size );
		ShadowRenderer renderer;
		Scene_Render( renderer, view );
		GlobalShaderCache().render( RENDER_DEPTHTEST | RENDER_DEPTHWRITE | RENDER_FILL | RENDER_TEXTURE | RENDER_ALPHATEST,
		                            lightView, lightProj, centre );
		KrHiddenCasters_draw( lightView, lightProj );

		f->glBindFramebuffer( GL_FRAMEBUFFER, previous );
		gl().glViewport( viewport[0], viewport[1], viewport[2], viewport[3] );
		gl().glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		gl().glDepthMask( GL_TRUE );
	}

	// camera eye -> world -> sun clip -> [0,1]
	const Matrix4 bias( 0.5f, 0, 0, 0,  0, 0.5f, 0, 0,  0, 0, 0.5f, 0,  0.5f, 0.5f, 0.5f, 1 );
	g_krEye2Shadow = matrix4_multiplied_by_matrix4( matrix4_multiplied_by_matrix4( bias, lightProj ),
	                 matrix4_multiplied_by_matrix4( lightView, matrix4_full_inverse( camera.modelview ) ) );
}

/* ---- KenRadiant: sky light occlusion - 9 directional depth maps (zenith + 8 around), one 3 x 3 atlas ---- */
static GLuint s_skyFbo = 0, s_skyTex = 0;
static bool s_skyDirty = true;
static Vector3 s_skyCentre( 1e9f, 0, 0 );
static void KrSky_sceneChanged(){ s_skyDirty = true; }
static void KrSkyShadow_render( camera_t& camera ){
	static bool registered = false;
	if( !registered ){
		registered = true;
		AddSceneChangeCallback( FreeCaller<void(), KrSky_sceneChanged>() );
		// zenith, 6 directions at 62 degrees, 9 at 28 degrees: about even over the sky dome
		g_krSkyDirs[0] = Vector3( 0, 0, 1 );
		const auto ring = []( int first, int count, float elevation, float offset ){
			const float e = degrees_to_radians( elevation );
			for( int i = 0; i < count; ++i ){
				const float a = degrees_to_radians( 360.f * i / count + offset );
				g_krSkyDirs[first + i] = Vector3( std::cos( e ) * std::cos( a ), std::cos( e ) * std::sin( a ), std::sin( e ) );
			}
		};
		ring( 1, 6, 62.f, 15.f );
		ring( 7, 9, 28.f, 0.f );
	}
	g_krSkyShadowTex = 0;
	if( !g_krShadows || vector3_length_squared( g_krSkyAmbient ) == 0 )
		return;
	QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
	constexpr int tile = 1024, size = tile * c_krSkyGrid;
	constexpr float halfExtent = 3072, depthRange = 32768;
	if( s_skyFbo == 0 ){
		gl().glGenTextures( 1, &s_skyTex );
		gl().glBindTexture( GL_TEXTURE_2D, s_skyTex );
		gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL );
		gl().glBindTexture( GL_TEXTURE_2D, 0 );
		f->glGenFramebuffers( 1, &s_skyFbo );
		GLint previous = 0;
		gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
		f->glBindFramebuffer( GL_FRAMEBUFFER, s_skyFbo );
		f->glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_skyTex, 0 );
		gl().glDrawBuffer( GL_NONE );
		gl().glReadBuffer( GL_NONE );
		f->glBindFramebuffer( GL_FRAMEBUFFER, previous );
		s_skyDirty = true;
	}
	extern int g_krMapGeneration;
	static int generation = -1;
	if( generation != g_krMapGeneration ){
		generation = g_krMapGeneration;
		s_skyDirty = true;
	}
	const Vector3 origin = Camera_getOrigin( camera );
	const Vector3 centre( float_snapped( origin.x(), 512.f ), float_snapped( origin.y(), 512.f ), float_snapped( origin.z(), 512.f ) );
	const Matrix4 proj = kr_ortho( -halfExtent, halfExtent, -halfExtent, halfExtent, 1, depthRange );
	Matrix4 views[c_krSkyDirs];
	for( int i = 0; i < c_krSkyDirs; ++i ){
		const Vector3 forward = -g_krSkyDirs[i];
		views[i] = kr_lookat( centre - forward * ( depthRange * 0.5f ), forward );
	}
	// while you edit, the sky maps update at most ~4 times a second
	static QElapsedTimer lastUpdate;
	if( s_skyDirty || !vector3_equal( centre, s_skyCentre ) ){
		if( s_skyDirty && lastUpdate.isValid() && lastUpdate.elapsed() < 250 && vector3_equal( centre, s_skyCentre ) )
			QTimer::singleShot( 260, [](){ if( g_camwnd ) CamWnd_Update( *g_camwnd ); } );
		else{
			s_skyDirty = false;
			s_skyCentre = centre;
			lastUpdate.start();
			GLint previous = 0, viewport[4];
			gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
			gl().glGetIntegerv( GL_VIEWPORT, viewport );
			f->glBindFramebuffer( GL_FRAMEBUFFER, s_skyFbo );
			gl().glDepthMask( GL_TRUE );
			gl().glViewport( 0, 0, size, size );
			gl().glClear( GL_DEPTH_BUFFER_BIT );
			for( int i = 0; i < c_krSkyDirs; ++i ){
				gl().glViewport( ( i % c_krSkyGrid ) * tile, ( i / c_krSkyGrid ) * tile, tile, tile );
				View view( true );
				view.Construct( proj, views[i], tile, tile );
				ShadowRenderer renderer;
				Scene_Render( renderer, view );
				GlobalShaderCache().render( RENDER_DEPTHTEST | RENDER_DEPTHWRITE | RENDER_FILL | RENDER_TEXTURE | RENDER_ALPHATEST, views[i], proj, centre );
				KrHiddenCasters_draw( views[i], proj );
			}
			f->glBindFramebuffer( GL_FRAMEBUFFER, previous );
			gl().glViewport( viewport[0], viewport[1], viewport[2], viewport[3] );
			gl().glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
			gl().glDepthMask( GL_TRUE );
		}
	}
	g_krSkyShadowTex = s_skyTex;
	g_krSkyBias = ( 2 * halfExtent / tile ) / depthRange;
	const Matrix4 eye2world = matrix4_full_inverse( camera.modelview );
	for( int i = 0; i < c_krSkyDirs; ++i ){
		// clip -> [0,1] of the tile -> atlas
		constexpr float g = c_krSkyGrid;
		const float ox = float( i % c_krSkyGrid ) / g, oy = float( i / c_krSkyGrid ) / g;
		const Matrix4 bias( 0.5f / g, 0, 0, 0,  0, 0.5f / g, 0, 0,  0, 0, 0.5f, 0,  ox + 0.5f / g, oy + 0.5f / g, 0.5f, 1 );
		g_krEye2Sky[i] = matrix4_multiplied_by_matrix4( matrix4_multiplied_by_matrix4( bias, proj ), matrix4_multiplied_by_matrix4( views[i], eye2world ) );
	}
}

/* ---- KenRadiant: point light shadows - depth cube maps of the nearest lights ---- */
static Matrix4 kr_lookat_up( const Vector3& eye, const Vector3& forward, const Vector3& up ){
	const Vector3 side = vector3_normalised( vector3_cross( forward, up ) );
	const Vector3 u = vector3_cross( side, forward );
	return Matrix4( side.x(), u.x(), -forward.x(), 0,
	                side.y(), u.y(), -forward.y(), 0,
	                side.z(), u.z(), -forward.z(), 0,
	                -vector3_dot( side, eye ), -vector3_dot( u, eye ), vector3_dot( forward, eye ), 1 );
}
static Matrix4 kr_perspective90( float n, float f ){
	return Matrix4( 1, 0, 0, 0,
	                0, 1, 0, 0,
	                0, 0, -( f + n ) / ( f - n ), -1,
	                0, 0, -2 * f * n / ( f - n ), 0 );
}

struct KrCube{
	GLuint tex = 0;
	Vector3 origin{ 1e9f, 0, 0 };
	float radius = 0;
	bool dirty = true;
	int lastUsed = 0;
};
static KrCube s_cubes[c_krMaxCubeShadows];
static GLuint s_cubeFbo = 0;
static bool s_cubesDirty = true;
static int s_cubeFrame = 0;
static void KrCubes_sceneChanged(){ s_cubesDirty = true; }

static void KrCube_render( KrCube& cube, QOpenGLFunctions* f ){
	constexpr int size = 512;
	if( cube.tex == 0 ){
		gl().glGenTextures( 1, &cube.tex );
		gl().glBindTexture( GL_TEXTURE_CUBE_MAP, cube.tex );
		for( int i = 0; i < 6; ++i )
			gl().glTexImage2D( GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_MODE, GL_NONE );
		gl().glBindTexture( GL_TEXTURE_CUBE_MAP, 0 );
	}
	static const Vector3 dirs[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	static const Vector3 ups[6] = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 } };
	const Matrix4 proj = kr_perspective90( g_krCubeNear, cube.radius );
	GLint previous = 0, viewport[4];
	gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	gl().glGetIntegerv( GL_VIEWPORT, viewport );
	f->glBindFramebuffer( GL_FRAMEBUFFER, s_cubeFbo );
	gl().glDrawBuffer( GL_NONE );
	gl().glReadBuffer( GL_NONE );
	gl().glViewport( 0, 0, size, size );
	for( int i = 0; i < 6; ++i ){
		f->glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, cube.tex, 0 );
		gl().glDepthMask( GL_TRUE );
		gl().glClear( GL_DEPTH_BUFFER_BIT );
		const Matrix4 view = kr_lookat_up( cube.origin, dirs[i], ups[i] );
		View v( true );
		v.Construct( proj, view, size, size );
		ShadowRenderer renderer;
		Scene_Render( renderer, v );
		GlobalShaderCache().render( RENDER_DEPTHTEST | RENDER_DEPTHWRITE | RENDER_FILL | RENDER_TEXTURE | RENDER_ALPHATEST, view, proj, cube.origin );
		KrHiddenCasters_draw( view, proj );
	}
	f->glBindFramebuffer( GL_FRAMEBUFFER, previous );
	gl().glViewport( viewport[0], viewport[1], viewport[2], viewport[3] );
	gl().glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	gl().glDepthMask( GL_TRUE );
	cube.dirty = false;
}

/// gives the first lights of the preview list a shadow cube; re-renders a cube only when its light or the scene changes
static void KrCubeShadows_render( camera_t& camera ){
	static bool registered = false;
	if( !registered ){
		registered = true;
		AddSceneChangeCallback( FreeCaller<void(), KrCubes_sceneChanged>() );
	}
	g_krEye2World = matrix4_full_inverse( camera.modelview );
	for( auto& t : g_krCubeTex ) t = 0;
	if( !g_krLightShadows )
		return;
	QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
	if( s_cubeFbo == 0 )
		f->glGenFramebuffers( 1, &s_cubeFbo );
	extern int g_krMapGeneration;
	static int generation = -1;
	if( generation != g_krMapGeneration ){
		generation = g_krMapGeneration;
		s_cubesDirty = true;
	}
	// while you drag things around, cube maps update at most ~6 times a second
	static QElapsedTimer lastUpdate;
	bool allowUpdate = true;
	if( s_cubesDirty ){
		if( lastUpdate.isValid() && lastUpdate.elapsed() < 150 ){
			allowUpdate = false;
			QTimer::singleShot( 160, [](){ if( g_camwnd ) CamWnd_Update( *g_camwnd ); } );
		}
		else{
			for( auto& c : s_cubes ) c.dirty = true;
			s_cubesDirty = false;
			lastUpdate.start();
		}
	}
	++s_cubeFrame;
	const std::size_t count = std::min( { g_krLightPreviewLights.size(), c_krMaxCubeShadows, std::size_t( std::clamp( g_krShadowedLights, 0, int( g_krMaxCubes ) ) ) } );
	bool used[c_krMaxCubeShadows] = {};
	int slotCube[c_krMaxCubeShadows];
	// keep each light on the cube it already has
	for( std::size_t i = 0; i < count; ++i ){
		slotCube[i] = -1;
		const KrPreviewLight& l = g_krLightPreviewLights[i];
		for( std::size_t c = 0; c < c_krMaxCubeShadows; ++c )
			if( !used[c] && vector3_equal( s_cubes[c].origin, l.origin ) && s_cubes[c].radius == std::min( l.radius, 4096.f ) ){
				slotCube[i] = int( c );
				used[c] = true;
				break;
			}
	}
	for( std::size_t i = 0; i < count; ++i ){
		if( slotCube[i] >= 0 )
			continue;
		const KrPreviewLight& l = g_krLightPreviewLights[i];
		int best = -1; // a free cube, least recently used
		for( std::size_t c = 0; c < c_krMaxCubeShadows; ++c )
			if( !used[c] && ( best < 0 || s_cubes[c].lastUsed < s_cubes[best].lastUsed ) )
				best = int( c );
		used[best] = true;
		slotCube[i] = best;
		s_cubes[best].origin = l.origin;
		s_cubes[best].radius = std::min( l.radius, 4096.f );
		s_cubes[best].dirty = true;
	}
	const bool drawing = g_krLightPreviewDrawing;
	g_krLightPreviewDrawing = false;
	for( std::size_t i = 0; i < count; ++i ){
		KrCube& cube = s_cubes[slotCube[i]];
		cube.lastUsed = s_cubeFrame;
		if( cube.dirty && ( allowUpdate || cube.tex == 0 ) )
			KrCube_render( cube, f );
		g_krCubeTex[i] = cube.tex;
		g_krCubeFar[i] = cube.radius;
	}
	g_krLightPreviewDrawing = drawing;
}

extern bool g_krLightShadows;
ToggleItem g_lightShadows_item{ BoolExportCaller( g_krLightShadows ) };
static void LightShadows_toggle(){
	g_krLightShadows = !g_krLightShadows;
	g_lightShadows_item.update();
	s_cubesDirty = true;
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}

ToggleItem g_deformVertexes_item{ BoolExportCaller( g_krDeformVertexes ) };
static void DeformVertexes_toggle(){
	g_krDeformVertexes = !g_krDeformVertexes;
	g_deformVertexes_item.update();
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}
ToggleItem g_polygonOffset_item{ BoolExportCaller( g_krPolygonOffset ) };
static void PolygonOffset_toggle(){
	g_krPolygonOffset = !g_krPolygonOffset;
	g_polygonOffset_item.update();
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}
extern bool g_krGameShaders;
void RefreshShaders();
ToggleItem g_gameShaders_item{ BoolExportCaller( g_krGameShaders ) };
static void GameShaders_toggle(){
	g_krGameShaders = !g_krGameShaders;
	g_gameShaders_item.update();
	RefreshShaders(); // render states are built per shader
}
ToggleItem g_glow_item{ BoolExportCaller( g_krGlow ) };
static void Glow_toggle(){
	g_krGlow = !g_krGlow;
	g_glow_item.update();
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}

extern bool g_krShadows;
ToggleItem g_sunShadows_item{ BoolExportCaller( g_krShadows ) };
static void SunShadows_toggle(){
	g_krShadows = !g_krShadows;
	g_sunShadows_item.update();
	s_shadowDirty = true;
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}

extern bool g_krAnimateShaders;
ToggleItem g_animateShaders_item{ BoolExportCaller( g_krAnimateShaders ) };
static void AnimateShaders_toggle(){
	g_krAnimateShaders = !g_krAnimateShaders;
	g_animateShaders_item.update();
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}

/* ---- KenRadiant: Unreal-style selection outline ---- */
#include <QOpenGLFramebufferObject>
void createShader( GLuint program, const char* filename, GLenum type );
void GLSLProgram_link( GLuint program );
/// selection look in the 3D view, cycled with J:
/// 0 = models / entities outlined, brushes edges + tint (default), 1 = everything outlined, 2 = everything edges + tint (classic)
int g_krSelectionStyle = 0;
static bool KrSelection_outlineAny(){ return g_krSelectionStyle != 2; }
static bool KrSelection_outlineBrushes(){ return g_krSelectionStyle == 1; }
static void SelectionStyle_cycle(){
	g_krSelectionStyle = ( g_krSelectionStyle + 1 ) % 3;
	const char* names[] = { "models outlined, brushes edges + tint", "everything outlined", "classic (edges + tint everywhere)" };
	globalOutputStream() << "selection style: " << names[g_krSelectionStyle] << '\n';
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}

/// collects only whole selected objects (not selected faces)
class MaskRenderer : public Renderer
{
	std::vector<unsigned int> m_highlight{ 0 };
	Shader* m_mask;
public:
	bool m_any = false;
	MaskRenderer( Shader* mask ) : m_mask( mask ){}
	void SetState( Shader* state, EStyle style ) override {}
	EStyle getStyle() const override { return eFullMaterials; }
	void PushState() override { m_highlight.push_back( m_highlight.back() ); }
	void PopState() override { m_highlight.pop_back(); }
	void Highlight( EHighlightMode mode, bool bEnable = true ) override {
		bEnable ? m_highlight.back() |= mode : m_highlight.back() &= ~mode;
	}
	void setLights( const LightList& lights ) override {}
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& world ) override {
		if( ( m_highlight.back() & ePrimitive ) && ( KrSelection_outlineBrushes() || !( m_highlight.back() & eKrBrushLike ) ) ){
			m_mask->addRenderable( renderable, world );
			m_any = true;
		}
	}
};

static void KrOutline_draw( camera_t& camera, View& view ){
	extern Shader* g_krMaskState;
	if( g_krMaskState == nullptr )
		g_krMaskState = GlobalShaderCache().capture( "$KR_MASK" );
	Shader* mask = g_krMaskState;
	static GLuint program = 0;
	static GLint u_mask, u_texel, u_colour;
	static QOpenGLFramebufferObject* fbo = nullptr;
	if( program == 0 ){
		program = gl().glCreateProgram();
		StringOutputStream filename( 256 );
		createShader( program, filename( GlobalRadiant().getAppPath(), "gl/kr_outline_vp.glsl" ), GL_VERTEX_SHADER );
		createShader( program, filename( GlobalRadiant().getAppPath(), "gl/kr_outline_fp.glsl" ), GL_FRAGMENT_SHADER );
		GLSLProgram_link( program );
		u_mask = gl().glGetUniformLocation( program, "u_mask" );
		u_texel = gl().glGetUniformLocation( program, "u_texel" );
		u_colour = gl().glGetUniformLocation( program, "u_colour" );
	}
	const int w = camera.width, h = camera.height;
	GLint previous = 0; // the camera FBO; read before creating ours, whose constructor changes the binding
	gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	if( fbo == nullptr || fbo->width() != w || fbo->height() != h ){
		delete fbo;
		fbo = new QOpenGLFramebufferObject( w, h );
	}

	// 1: silhouette of the selected objects
	fbo->bind();
	gl().glViewport( 0, 0, w, h );
	gl().glClearColor( 0, 0, 0, 0 );
	gl().glClear( GL_COLOR_BUFFER_BIT );
	MaskRenderer renderer( mask );
	Scene_Render( renderer, view );
	GlobalShaderCache().render( RENDER_FILL | RENDER_COLOURWRITE, camera.modelview, camera.projection, Camera_getOrigin( camera ) );
	QOpenGLContext::currentContext()->functions()->glBindFramebuffer( GL_FRAMEBUFFER, previous );
	gl().glViewport( 0, 0, w, h );
	if( !renderer.m_any )
		return;

	// 2: orange ring around it, over the finished 3D view
	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadIdentity();
	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadIdentity();
	gl().glDisable( GL_DEPTH_TEST );
	gl().glDisable( GL_CULL_FACE );
	gl().glDisable( GL_ALPHA_TEST );
	gl().glDisable( GL_LIGHTING );
	gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
	gl().glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	gl().glDisable( GL_POLYGON_STIPPLE );
	gl().glDisable( GL_SCISSOR_TEST );
	gl().glDisableClientState( GL_VERTEX_ARRAY );
	gl().glDisableClientState( GL_COLOR_ARRAY );
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
	gl().glEnable( GL_BLEND );
	gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	gl().glActiveTexture( GL_TEXTURE0 );
	gl().glEnable( GL_TEXTURE_2D );
	gl().glBindTexture( GL_TEXTURE_2D, fbo->texture() );
	gl().glUseProgram( program );
	gl().glUniform1i( u_mask, 0 );
	gl().glUniform2f( u_texel, 1.f / w, 1.f / h );
	gl().glUniform4f( u_colour, 1.f, 0.55f, 0.05f, 1.f ); // Unreal's selection orange
	gl().glBegin( GL_QUADS );
	gl().glTexCoord2f( 0, 0 ); gl().glVertex2f( -1, -1 );
	gl().glTexCoord2f( 1, 0 ); gl().glVertex2f( 1, -1 );
	gl().glTexCoord2f( 1, 1 ); gl().glVertex2f( 1, 1 );
	gl().glTexCoord2f( 0, 1 ); gl().glVertex2f( -1, 1 );
	gl().glEnd();
	gl().glUseProgram( 0 );
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
	gl().glDisable( GL_TEXTURE_2D );
	gl().glDisable( GL_BLEND );
	gl().glEnableClientState( GL_VERTEX_ARRAY );
}

static void LightPreview_toggle(){
	g_krLightPreview = !g_krLightPreview;
	g_lightPreview_item.update();
	globalOutputStream() << "live light preview: " << ( g_krLightPreview ? "on (lights + sky shader sun, sun shadows)" : "off" ) << '\n';
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}

void KrLightPreview_enable(){
	if( !g_krLightPreview )
		LightPreview_toggle();
}
void KrLightPreview_redraw(){
	if( g_camwnd )
		CamWnd_Update( *g_camwnd );
}
/// reads the sun from the map now (the calibrator opens before the next preview frame)
void KrLightPreview_prepareSun(){
	if( !g_krSun.overridden )
		KrSun_readMap();
}
void KrSunCalibrator_show();
Vector3 KrCamera_origin(){
	return g_camwnd != nullptr ? Camera_getOrigin( *g_camwnd ) : Vector3( 0, 0, 0 );
}
void KrCamera_goTo( const Vector3& origin ){
	if( g_camwnd != nullptr )
		Camera_setOrigin( *g_camwnd, origin );
}
/// KenRadiant: the surface point in the middle of the 3D view (grid snapped), or 256 units in front of the camera
Vector3 KrCamera_targetPoint(){
	if( g_camwnd == nullptr )
		return Vector3( 0, 0, 0 );
	CamWnd& cam = *g_camwnd;
	const DeviceVector eps( 4.f / std::max( 1, cam.getCamera().width ), 4.f / std::max( 1, cam.getCamera().height ) );
	if( const auto hit = testSelect_unselected_scene_point( cam.getView(), DeviceVector( 0, 0 ), eps ) )
		return vector3_snapped( Vector3( hit->point ), GetSnapGridSize() );
	return vector3_snapped( Camera_getOrigin( cam ) - Camera_getViewVector( cam ) * 256.f, GetSnapGridSize() );
}

void CamWnd::Cam_Draw(){
//		globalOutputStream() << "Cam_Draw()\n";

	gl().glViewport( 0, 0, m_Camera.width, m_Camera.height );
#if 0
	GLint viewprt[4];
	gl().glGetIntegerv( GL_VIEWPORT, viewprt );
#endif

	// enable depth buffer writes
	gl().glDepthMask( GL_TRUE );
	gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

	Vector3 clearColour( 0, 0, 0 );
	if ( m_Camera.draw_mode != cd_lighting ) {
		clearColour = g_camwindow_globals.color_cameraback;
	}

	gl().glClearColor( clearColour[0], clearColour[1], clearColour[2], 0 );
	gl().glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	extern void Renderer_ResetStats();
	Renderer_ResetStats();
	extern void Cull_ResetStats();
	Cull_ResetStats();

	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &m_Camera.projection ) );

	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &m_Camera.modelview ) );


	// one directional light source directly behind the viewer
	{
		GLfloat inverse_cam_dir[4], ambient[4], diffuse[4]; //, material[4];

		ambient[0] = ambient[1] = ambient[2] = 0.4f;
		ambient[3] = 1;
		diffuse[0] = diffuse[1] = diffuse[2] = 0.4f;
		diffuse[3] = 1;
		//material[0] = material[1] = material[2] = 0.8f;
		//material[3] = 1;

		inverse_cam_dir[0] = m_Camera.vpn[0];
		inverse_cam_dir[1] = m_Camera.vpn[1];
		inverse_cam_dir[2] = m_Camera.vpn[2];
		inverse_cam_dir[3] = 0;

		gl().glLightfv( GL_LIGHT0, GL_POSITION, inverse_cam_dir );

		gl().glLightfv( GL_LIGHT0, GL_AMBIENT, ambient );
		gl().glLightfv( GL_LIGHT0, GL_DIFFUSE, diffuse );

		gl().glEnable( GL_LIGHT0 );
	}


	unsigned int globalstate = RENDER_DEPTHTEST
	                         | RENDER_COLOURWRITE
							 | RENDER_DEPTHWRITE
							 | RENDER_ALPHATEST
							 | RENDER_BLEND
							 | RENDER_CULLFACE
							 | RENDER_COLOURARRAY
							 | RENDER_OFFSETLINE
							 | RENDER_FOG
							 | RENDER_COLOURCHANGE;
	switch ( m_Camera.draw_mode )
	{
	case cd_wire:
		break;
	case cd_solid:
		globalstate |= RENDER_FILL
		            | RENDER_LIGHTING
		            | RENDER_SMOOTH
		            | RENDER_SCALED;
		break;
	case cd_texture:
	case cd_texture_plus_wire:
		globalstate |= RENDER_FILL
		            | RENDER_LIGHTING
		            | RENDER_TEXTURE
		            | RENDER_SMOOTH
		            | RENDER_SCALED
		            | RENDER_PROGRAM;
		break;
	case cd_lighting:
		globalstate |= RENDER_FILL
		            | RENDER_LIGHTING
		            | RENDER_TEXTURE
		            | RENDER_SMOOTH
		            | RENDER_SCALED
		            | RENDER_BUMP
		            | RENDER_PROGRAM
		            | RENDER_SCREEN;
		break;
	default:
		globalstate = 0;
		break;
	}

//	if ( !g_xywindow_globals.m_bNoStipple ) {
		globalstate |= RENDER_LINESTIPPLE | RENDER_POLYGONSTIPPLE;
//	}

	g_krLightPreviewView = m_Camera.modelview; // (tcGen environment needs the camera too)
	// KenRadiant: shader animation clock
	{
		static QElapsedTimer clock;
		if( !clock.isValid() )
			clock.start();
		g_krShaderTime = clock.elapsed() / 1000.f;
		g_krAnimatedDrawn = false;
	}
	// KenRadiant live light preview: lights, sun and its shadow map before the scene is queued for this view
	g_krLightPreviewDrawing = g_krLightPreview && ( m_Camera.draw_mode == cd_texture || m_Camera.draw_mode == cd_texture_plus_wire );
	if( g_krLightPreviewDrawing ){
		KrLightPreview_collect( Camera_getOrigin( m_Camera ) );
		g_krLightPreviewView = m_Camera.modelview;
		g_krLightPreviewDrawing = false; // the shadow passes draw plain depth
		KrShadow_render( m_Camera );
		KrSkyShadow_render( m_Camera );
		KrCubeShadows_render( m_Camera );
		g_krLightPreviewDrawing = true;
	}

	{
		CamRenderer renderer( globalstate,
		                      g_camwindow_globals_private.m_bFaceWire ? m_state_facewire : 0,
		                      m_Camera.draw_mode == cd_texture_plus_wire ? m_state_wire : 0,
		                      m_state_select0,
		                      g_camwindow_globals_private.m_bFaceFill ? m_state_select1 : 0,
		                      m_view.getViewer() );

		renderer.m_outline = KrSelection_outlineAny() && GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive;
		renderer.m_outlineSkipBrushes = !KrSelection_outlineBrushes();
		Scene_Render( renderer, m_view );

		if( g_camwindow_globals_private.m_bShowWorkzone && GlobalSelectionSystem().countSelected() != 0 && GlobalSelectionSystem().ManipulatorMode() != SelectionSystem::eUV ){
			m_draw_workzone.render( renderer, m_state_workzone );
		}

		if( g_camwindow_globals_private.m_bShowSize && GlobalSelectionSystem().countSelected() != 0 ){
			m_draw_size.render( renderer, m_state_text, m_view );
		}

		extern void KrBspPreview_draw( const Matrix4& modelview, const Matrix4& projection );
		KrBspPreview_draw( m_Camera.modelview, m_Camera.projection ); // compiled lighting (.bsp), when on
		renderer.render( m_Camera.modelview, m_Camera.projection );
		g_krLightPreviewDrawing = false;
		if( KrSelection_outlineAny() && GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive && GlobalSelectionSystem().countSelected() != 0 )
			KrOutline_draw( m_Camera, m_view );
		// keep animated water / scrolling shaders moving (~30 fps) only while one is on screen
		if( g_krAnimateShaders && g_krAnimatedDrawn ){
			QTimer::singleShot( 33, m_gl_widget, [this](){ CamWnd_Update( *this ); } );
		}
	}

	// prepare for 2d stuff
	gl().glColor4f( 1, 1, 1, 1 );
	gl().glDisable( GL_BLEND );
	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadIdentity();
	gl().glOrtho( 0, m_Camera.width, 0, m_Camera.height, -100, 100 );
	gl().glScalef( 1, -1, 1 );
	gl().glTranslatef( 0, -m_Camera.height, 0 );
	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadIdentity();

	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glActiveTexture( GL_TEXTURE0 );

	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
	gl().glDisableClientState( GL_COLOR_ARRAY );

	gl().glDisable( GL_TEXTURE_2D );
	gl().glDisable( GL_LIGHTING );
	gl().glDisable( GL_COLOR_MATERIAL );
	gl().glDisable( GL_DEPTH_TEST );
	gl().glLineWidth( 1 );

	// draw the crosshair
	if ( m_bFreeMove ) {
		 // .5 coords for exact pixel rendering
		const Vector2 cent( ( m_Camera.width + 1 ) / 2 + .5f, ( m_Camera.height + 1 ) / 2 + .5f );
		constexpr float a = 6.5, b = 2.5;
		for( float f : { 0.f, 1.f } )
		{
			gl().glColor3f( f, f, f );
			const Vector2 verts[]{
				cent + Vector2(  0,  a ) - Vector2( f, f ),
				cent + Vector2(  0,  b ) - Vector2( f, f ),
				cent + Vector2(  0, -a ) - Vector2( f, f ),
				cent + Vector2(  0, -b ) - Vector2( f, f ),
				cent + Vector2(  a,  0 ) - Vector2( f, f ),
				cent + Vector2(  b,  0 ) - Vector2( f, f ),
				cent + Vector2( -a,  0 ) - Vector2( f, f ),
				cent + Vector2( -b,  0 ) - Vector2( f, f ) };
			gl().glVertexPointer( 2, GL_FLOAT, sizeof( *verts ), verts->data() );
			gl().glDrawArrays( GL_LINES, 0, std::size( verts ) );
		}
	}

	KrTerrainPaint_drawOverlay( matrix4_multiplied_by_matrix4( m_Camera.projection, m_Camera.modelview ), m_Camera.width, m_Camera.height );
	// KenRadiant measure tool: line between the two points, length on it
	if( g_krMeasure && s_measureCount != 0 ){
		const Matrix4 world2screen = matrix4_multiplied_by_matrix4( m_Camera.projection, m_Camera.modelview );
		const auto toScreen = [&]( const Vector3& p, Vector2& out ){
			const Vector4 c = matrix4_transformed_vector4( world2screen, Vector4( p, 1 ) );
			if( c.w() <= 0.01f )
				return false;
			out = Vector2( ( c.x() / c.w() + 1 ) * 0.5f * m_Camera.width, ( 1 - c.y() / c.w() ) * 0.5f * m_Camera.height );
			return true;
		};
		const bool second = s_measureCount == 2 || s_measureHover;
		Vector2 a, b;
		const bool va = toScreen( s_measureA, a ), vb = second && toScreen( s_measureB, b );
		gl().glLineWidth( 2 );
		gl().glColor3f( 1, 0.85f, 0.1f );
		if( va && vb ){
			const Vector2 line[]{ a, b };
			gl().glVertexPointer( 2, GL_FLOAT, sizeof( Vector2 ), line[0].data() );
			gl().glDrawArrays( GL_LINES, 0, 2 );
		}
		gl().glPointSize( 7 );
		for( const auto& [ ok, p ] : { std::pair{ va, a }, std::pair{ vb, b } } )
			if( ok ){
				gl().glVertexPointer( 2, GL_FLOAT, sizeof( Vector2 ), p.data() );
				gl().glDrawArrays( GL_POINTS, 0, 1 );
			}
		gl().glPointSize( 1 );
		gl().glLineWidth( 1 );
		if( va && vb ){
			const Vector3 d = s_measureB - s_measureA;
			char text[128];
			std::snprintf( text, sizeof( text ), "  %.1f units   (x %.1f  y %.1f  z %.1f)", vector3_length( d ), std::fabs( d.x() ), std::fabs( d.y() ), std::fabs( d.z() ) );
			const Vector2 mid = ( a + b ) * 0.5f;
			gl().glColor3f( 0, 0, 0 );
			gl().glRasterPos3f( mid.x() + 1, mid.y() + 1, 0 );
			GlobalOpenGL().drawString( text );
			gl().glColor3f( 1, 0.85f, 0.1f );
			gl().glRasterPos3f( mid.x(), mid.y(), 0 );
			GlobalOpenGL().drawString( text );
		}
	}

	if ( g_camwindow_globals.m_showStats ) {
		gl().glRasterPos3f( 1, m_Camera.height, 0 );
		extern const char* Renderer_GetStats( int frame2frame );
		GlobalOpenGL().drawString( Renderer_GetStats( m_render_time.elapsed_msec() ) );
		m_render_time.start();

		gl().glRasterPos3f( 1, m_Camera.height - GlobalOpenGL().m_font->getPixelHeight(), 0 );
		extern const char* Cull_GetStats();
		GlobalOpenGL().drawString( Cull_GetStats() );
	}

	// bind back to the default texture so that we don't have problems
	// elsewhere using/modifying texture maps between contexts
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
}

void CamWnd::draw(){
	m_drawing = true;

	//globalOutputStream() << "draw...\n";
	GlobalOpenGL_debugAssertNoErrors();
	Cam_Draw();
	GlobalOpenGL_debugAssertNoErrors();

	m_drawing = false;
}

void CamWnd::BenchMark(){
	Timer timer;
	timer.start();
	for ( int i = 0; i < 100; ++i )
	{
		Vector3 angles;
		angles[CAMERA_ROLL] = 0;
		angles[CAMERA_PITCH] = 0;
		angles[CAMERA_YAW] = i * ( 360.0 / 100.0 );
		Camera_setAngles( *this, angles );
	}
	globalOutputStream() << timer.elapsed_msec() << " milliseconds\n";
}


void GlobalCamera_ResetAngles(){
	CamWnd& camwnd = *g_camwnd;
	Vector3 angles;
	angles[CAMERA_ROLL] = angles[CAMERA_PITCH] = 0;
	angles[CAMERA_YAW] = 22.5 * floor( ( Camera_getAngles( camwnd )[CAMERA_YAW] + 11 ) / 22.5 );
	Camera_setAngles( camwnd, angles );
}

#include "select.h"

Vector3 Camera_getFocusPos( camera_t& camera ){
	const Vector3 camorigin( Camera_getOrigin( camera ) );
	const AABB aabb( aabb_for_minmax( Select_getWorkZone().d_work_min, Select_getWorkZone().d_work_max ) );
	const View& view = *( camera.m_view );
#if 0
	Vector3 angles( Camera_getAngles( camera ) );
	Vector3 radangles( degrees_to_radians( angles[0] ), degrees_to_radians( angles[1] ), degrees_to_radians( angles[2] ) );
	Vector3 viewvector;
	viewvector[0] = cos( radangles[1] ) * cos( radangles[0] );
	viewvector[1] = sin( radangles[1] ) * cos( radangles[0] );
	viewvector[2] = sin( radangles[0] );
#elif 0
	const Vector3 viewvector( view.getViewDir() );
#elif 1
	const Vector3 viewvector( -camera.vpn );
#endif

	const Plane3 frustumPlanes[4] = {
		plane3_translated( view.getFrustum().left,   aabb.origin - camorigin ),
		plane3_translated( view.getFrustum().right,  aabb.origin - camorigin ),
		plane3_translated( view.getFrustum().top,    aabb.origin - camorigin ),
		plane3_translated( view.getFrustum().bottom, aabb.origin - camorigin ),
	};
	float offset = 64.0f;

	const std::array<Vector3, 8> corners = aabb_corners( aabb );

	for ( const Plane3& plane : frustumPlanes ){
		for ( const Vector3& corner : corners ){
			const Ray ray( aabb.origin, -viewvector );
			//Plane3 newplane( plane.normal(), vector3_dot( plane.normal(), corner - plane.normal() * 16.0f ) );
			const Plane3 newplane( plane.normal(), vector3_dot( plane.normal(), corner ) );
			const float d = vector3_dot( ray.direction, newplane.normal() );
			if( d != 0 ){
				const float s = vector3_dot( newplane.normal() * newplane.dist() - ray.origin, newplane.normal() ) / d;
				offset = std::max( offset, s );
			}
		}
	}

	const int off = camera.m_focus_offset;
	if( off < 0 || off > 16 ){
		offset -= offset * off / 8 * pow( 2.0f, ( off < 0 ? -off : off - 16 ) / 8.f );
	}
	else{
		offset -= offset * off / 8;
	}
	return ( aabb.origin - viewvector * offset );
}

void GlobalCamera_FocusOnSelected(){
	Camera_setOrigin( *g_camwnd, Camera_getFocusPos( g_camwnd->getCamera() ) );
}

void Camera_ChangeFloorUp(){
	CamWnd& camwnd = *g_camwnd;
	camwnd.Cam_ChangeFloor( true );
}

void Camera_ChangeFloorDown(){
	CamWnd& camwnd = *g_camwnd;
	camwnd.Cam_ChangeFloor( false );
}

void Camera_CubeIn(){
	CamWnd& camwnd = *g_camwnd;
	g_camwindow_globals.m_nCubicScale--;
	if ( g_camwindow_globals.m_nCubicScale < 1 ) {
		g_camwindow_globals.m_nCubicScale = 1;
	}
	Camera_updateProjection( camwnd.getCamera() );
	CamWnd_Update( camwnd );
	GridStatus_changed();
}

void Camera_CubeOut(){
	CamWnd& camwnd = *g_camwnd;
	g_camwindow_globals.m_nCubicScale++;
	if ( g_camwindow_globals.m_nCubicScale > 23 ) {
		g_camwindow_globals.m_nCubicScale = 23;
	}
	Camera_updateProjection( camwnd.getCamera() );
	CamWnd_Update( camwnd );
	GridStatus_changed();
}

bool Camera_GetFarClip(){
	return g_camwindow_globals_private.m_bCubicClipping;
}

ToggleItem g_getfarclip_item( BoolExportCaller( g_camwindow_globals_private.m_bCubicClipping ) );

void Camera_SetFarClip( bool value ){
	CamWnd& camwnd = *g_camwnd;
	g_camwindow_globals_private.m_bCubicClipping = value;
	g_getfarclip_item.update();
	Camera_updateProjection( camwnd.getCamera() );
	CamWnd_Update( camwnd );
}

void Camera_ToggleFarClip(){
	Camera_SetFarClip( !Camera_GetFarClip() );
}


void CamWnd_constructToolbar( QToolBar* toolbar ){
	toolbar_append_toggle_button( toolbar, "Cubic clip the camera view", "view_cubicclipping.png", "ToggleCubicClip" );
}

void CamWnd_registerShortcuts(){
	if ( g_pGameDescription->mGameType == "doom3" ) {
		command_connect_accelerator( "TogglePreview" );
	}

	command_connect_accelerator( "CameraModeNext" );
	command_connect_accelerator( "CameraModePrev" );

	command_connect_accelerator( "CameraSpeedInc" );
	command_connect_accelerator( "CameraSpeedDec" );
}


void GlobalCamera_Benchmark(){
	CamWnd& camwnd = *g_camwnd;
	camwnd.BenchMark();
}

void GlobalCamera_Update(){
	CamWnd& camwnd = *g_camwnd;
	CamWnd_Update( camwnd );
}

camera_draw_mode CamWnd_GetMode(){
	return camera_t::draw_mode;
}
void CamWnd_SetMode( camera_draw_mode mode ){
	// workaround cd_lighting not being correctly applied on start (fixme?)
	camera_t::draw_mode = ( g_camwnd == 0 && mode == cd_lighting )? cd_texture : mode;

	ShaderCache_setBumpEnabled( camera_t::draw_mode == cd_lighting );
	if ( g_camwnd != 0 ) {
		CamWnd_Update( *g_camwnd );
	}
}

void CamWnd_TogglePreview(){
	// gametype must be doom3 for this function to work
	// if the gametype is not doom3 something is wrong with the
	// global command list or somebody else calls this function.
	ASSERT_MESSAGE( g_pGameDescription->mGameType == "doom3", "CamWnd_TogglePreview called although mGameType is not doom3 compatible" );

	// switch between textured and lighting mode
	CamWnd_SetMode( ( CamWnd_GetMode() == cd_lighting ) ? cd_texture : cd_lighting );
}


CameraModel* g_camera_model = 0;

void CamWnd_LookThroughCamera( CamWnd& camwnd ){
	if ( g_camera_model != 0 ) {
		CamWnd_Add_Handlers_Move( camwnd );
		g_camera_model->setCameraView( 0, Callback<void()>() );
		g_camera_model = 0;
		Camera_updateModelview( camwnd.getCamera() );
		Camera_updateProjection( camwnd.getCamera() );
		CamWnd_Update( camwnd );
	}
}

inline CameraModel* Instance_getCameraModel( scene::Instance& instance ){
	return InstanceTypeCast<CameraModel>::cast( instance );
}

void CamWnd_LookThroughSelected( CamWnd& camwnd ){
	if ( g_camera_model != 0 ) {
		CamWnd_LookThroughCamera( camwnd );
	}

	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		scene::Instance& instance = GlobalSelectionSystem().ultimateSelected();
		CameraModel* cameraModel = Instance_getCameraModel( instance );
		if ( cameraModel != 0 ) {
			CamWnd_Remove_Handlers_Move( camwnd );
			g_camera_model = cameraModel;
			g_camera_model->setCameraView( &camwnd.getCameraView(), ReferenceCaller<CamWnd, void(), CamWnd_LookThroughCamera>( camwnd ) );
		}
	}
}

void GlobalCamera_LookThroughSelected(){
	CamWnd_LookThroughSelected( *g_camwnd );
}

void GlobalCamera_LookThroughCamera(){
	CamWnd_LookThroughCamera( *g_camwnd );
}


void RenderModeImport( int value ){
	CamWnd_SetMode( static_cast<camera_draw_mode>( ( value < 0 || value >= camera_draw_mode_count )? cd_texture : value ) );
}
typedef FreeCaller<void(int), RenderModeImport> RenderModeImportCaller;

void RenderModeExport( const IntImportCallback& importer ){
	importer( CamWnd_GetMode() );
}
typedef FreeCaller<void(const IntImportCallback&), RenderModeExport> RenderModeExportCaller;

void CameraModeNext(){
	const int count = camera_draw_mode_count - ( g_pGameDescription->mGameType == "doom3"? 0 : 1 );
	CamWnd_SetMode( static_cast<camera_draw_mode>( ( CamWnd_GetMode() + 1 ) % count ) );
}
void CameraModePrev(){
	const int count = camera_draw_mode_count - ( g_pGameDescription->mGameType == "doom3"? 0 : 1 );
	CamWnd_SetMode( static_cast<camera_draw_mode>( ( CamWnd_GetMode() + count - 1 ) % count ) );
}

void CamMSAAImport( int value ){
	g_camwindow_globals_private.m_MSAA = value ? 1 << value : value;
}
typedef FreeCaller<void(int), CamMSAAImport> MSAAImportCaller;

void CamMSAAExport( const IntImportCallback& importer ){
	if( g_camwindow_globals_private.m_MSAA <= 0 ){
		importer( 0 );
	}
	else{
		int exponent = 1;
		while( !( ( g_camwindow_globals_private.m_MSAA >> exponent ) & 1 ) ){
			++exponent;
		}
		importer( exponent );
	}
}
typedef FreeCaller<void(const IntImportCallback&), CamMSAAExport> MSAAExportCaller;

static std::vector<Callback<void(float)>> g_fieldOfViewObservers; // KenRadiant: toolbar FOV slider
void fieldOfViewImport( float value ){
	camera_t::fieldOfView = value;
	if( g_camwnd ){
		Camera_updateProjection( g_camwnd->getCamera() );
		CamWnd_Update( *g_camwnd );
	}
	for( const auto& observer : g_fieldOfViewObservers )
		observer( value );
}
float Camera_getFieldOfView(){
	return camera_t::fieldOfView;
}
void Camera_setFieldOfView( float fov ){
	if( fov != camera_t::fieldOfView )
		fieldOfViewImport( fov );
}
void Camera_addFieldOfViewObserver( const Callback<void(float)>& observer ){
	g_fieldOfViewObservers.push_back( observer );
}
typedef FreeCaller<void(float), fieldOfViewImport> fieldOfViewImportCaller;

void Camera_constructPreferences( PreferencesPage& page ){
	page.appendSpinner( "Movement Speed", g_camwindow_globals_private.m_nMoveSpeed, 1, CAM_MAX_SPEED );
	page.appendSpinner( "Time to Max Speed", g_camwindow_globals_private.m_time_toMaxSpeed, 0, 5000 );
	page.appendSpinner( "Scroll Move Speed", g_camwindow_globals_private.m_nScrollMoveSpeed, 0, 999 );
	page.appendSpinner( "Strafe Speed", g_camwindow_globals_private.m_strafeSpeed, 0.1, 10 );
	page.appendSpinner( "Mouse Sensitivity", g_camwindow_globals_private.m_angleSpeed, 0.1, 30 );
	page.appendCheckBox( "", "Invert mouse vertical axis", g_camwindow_globals_private.m_bCamInverseMouse );
	page.appendCheckBox( "", "Zoom to Mouse pointer", g_camwindow_globals_private.m_bZoomToPointer );
	page.appendCheckBox(
	    "", "Discrete movement",
	    FreeCaller<void(bool), CamWnd_Move_Discrete_Import>(),
	    BoolExportCaller( g_camwindow_globals_private.m_bCamDiscrete )
	);
	page.appendCheckBox(
	    "", "Enable far-clip plane",
	    FreeCaller<void(bool), Camera_SetFarClip>(),
	    BoolExportCaller( g_camwindow_globals_private.m_bCubicClipping )
	);
	page.appendCheckBox(
	    "", "Colorize selection",
	    BoolImportCaller( g_camwindow_globals_private.m_bFaceFill ),
	    BoolExportCaller( g_camwindow_globals_private.m_bFaceFill )
	);
	page.appendCheckBox(
	    "", "Selected faces wire",
	    BoolImportCaller( g_camwindow_globals_private.m_bFaceWire ),
	    BoolExportCaller( g_camwindow_globals_private.m_bFaceWire )
	);

	const char* render_modes[]{ "Wireframe", "Flatshade", "Textured", "Textured+Wire", "Lighting" };
	page.appendCombo(
	    "Render Mode",
	    StringArrayRange( render_modes, std::size( render_modes ) - ( g_pGameDescription->mGameType == "doom3"? 0 : 1 ) ),
	    IntImportCallback( RenderModeImportCaller() ),
	    IntExportCallback( RenderModeExportCaller() )
	);

	{
		const char* samples[] = { "0", "2", "4", "8", "16", "32" };

		page.appendCombo(
		    "MSAA",
		    StringArrayRange( samples ),
		    IntImportCallback( MSAAImportCaller() ),
		    IntExportCallback( MSAAExportCaller() )
		);
	}

	const char* strafe_mode[] = { "None", "Up", "Forward", "Both", "Both Inverted" };

	page.appendCombo(
	    "Strafe Mode",
	    g_camwindow_globals_private.m_strafeMode,
	    StringArrayRange( strafe_mode )
	);

	page.appendSpinner( "Field Of View", 1.0, 175.0,
	                    FloatImportCallback( fieldOfViewImportCaller() ),
	                    FloatExportCallback( FloatExportCaller( camera_t::fieldOfView ) ),
	                    0
	                  );
}
void Camera_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Camera", "Camera View Preferences" ) );
	Camera_constructPreferences( page );
}
void Camera_registerPreferencesPage(){
	PreferencesDialog_addSettingsPage( makeCallbackF( Camera_constructPage ) );
}

#include "preferencesystem.h"
#include "stringio.h"
#include "dialog.h"

typedef FreeCaller<void(bool), CamWnd_Move_Discrete_Import> CamWndMoveDiscreteImportCaller;

void CameraSpeed_increase(){
	g_camwindow_globals_private.m_nMoveSpeed = std::min( g_camwindow_globals_private.m_nMoveSpeed + CAM_SPEED_STEP, CAM_MAX_SPEED );
	globalOutputStream() << " ++Camera Move Speed: ";
	globalWarningStream() << g_camwindow_globals_private.m_nMoveSpeed << '\n';
}

void CameraSpeed_decrease(){
	g_camwindow_globals_private.m_nMoveSpeed = std::max( g_camwindow_globals_private.m_nMoveSpeed - CAM_SPEED_STEP, CAM_MIN_SPEED );
	globalOutputStream() << " --Camera Move Speed: ";
	globalWarningStream() << g_camwindow_globals_private.m_nMoveSpeed << '\n';
}

/// \brief Initialisation for things that have the same lifespan as this module.
void CamWnd_Construct(){
	GlobalCommands_insert( "CenterView", makeCallbackF( GlobalCamera_ResetAngles ), QKeySequence( "Shift+End" ) ); // KenRadiant: End drops the selection to the floor (Unreal)
	GlobalCommands_insert( "CameraFocusOnSelected", makeCallbackF( GlobalCamera_FocusOnSelected ), QKeySequence( "Tab" ) );
	GlobalToggles_insert( "ToggleLightPreview", makeCallbackF( LightPreview_toggle ), ToggleItem::AddCallbackCaller( g_lightPreview_item ), QKeySequence( "Ctrl+L" ) ); // KenRadiant
	GlobalToggles_insert( "ToggleSunShadows", makeCallbackF( SunShadows_toggle ), ToggleItem::AddCallbackCaller( g_sunShadows_item ) ); // KenRadiant
	GlobalToggles_insert( "ToggleAnimateShaders", makeCallbackF( AnimateShaders_toggle ), ToggleItem::AddCallbackCaller( g_animateShaders_item ) ); // KenRadiant
	GlobalToggles_insert( "ToggleLightShadows", makeCallbackF( LightShadows_toggle ), ToggleItem::AddCallbackCaller( g_lightShadows_item ) ); // KenRadiant
	GlobalToggles_insert( "ToggleDeformVertexes", makeCallbackF( DeformVertexes_toggle ), ToggleItem::AddCallbackCaller( g_deformVertexes_item ) ); // KenRadiant
	GlobalToggles_insert( "TogglePolygonOffset", makeCallbackF( PolygonOffset_toggle ), ToggleItem::AddCallbackCaller( g_polygonOffset_item ) ); // KenRadiant
	GlobalToggles_insert( "ToggleGameShaders", makeCallbackF( GameShaders_toggle ), ToggleItem::AddCallbackCaller( g_gameShaders_item ) ); // KenRadiant
	GlobalPreferenceSystem().registerPreference( "KrGameShaders", BoolImportStringCaller( g_krGameShaders ), BoolExportStringCaller( g_krGameShaders ) );
	GlobalToggles_insert( "ToggleGlowShaders", makeCallbackF( Glow_toggle ), ToggleItem::AddCallbackCaller( g_glow_item ) ); // KenRadiant
	GlobalPreferenceSystem().registerPreference( "KrEngineShift", IntImportStringCaller( g_krEngineShift ), IntExportStringCaller( g_krEngineShift ) );
	GlobalPreferenceSystem().registerPreference( "KrEngineGamma", FloatImportStringCaller( g_krEngineGamma ), FloatExportStringCaller( g_krEngineGamma ) );
	GlobalPreferenceSystem().registerPreference( "KrShadowedLights", IntImportStringCaller( g_krShadowedLights ), IntExportStringCaller( g_krShadowedLights ) );
	GlobalPreferenceSystem().registerPreference( "KrPreviewLightCount2048", IntImportStringCaller( g_krPreviewLightCount ), IntExportStringCaller( g_krPreviewLightCount ) );
	GlobalPreferenceSystem().registerPreference( "KrLightShadows", BoolImportStringCaller( g_krLightShadows ), BoolExportStringCaller( g_krLightShadows ) );
	GlobalPreferenceSystem().registerPreference( "KrDeformVertexes", BoolImportStringCaller( g_krDeformVertexes ), BoolExportStringCaller( g_krDeformVertexes ) );
	GlobalPreferenceSystem().registerPreference( "KrPolygonOffset", BoolImportStringCaller( g_krPolygonOffset ), BoolExportStringCaller( g_krPolygonOffset ) );
	GlobalPreferenceSystem().registerPreference( "KrGlowShaders", BoolImportStringCaller( g_krGlow ), BoolExportStringCaller( g_krGlow ) );
	GlobalToggles_insert( "ToggleMeasure", makeCallbackF( Measure_toggle ), ToggleItem::AddCallbackCaller( g_measure_item ) ); // KenRadiant
	extern void KrBrushShapes_show();
	extern void KrCheckProblems_show();
	GlobalCommands_insert( "BrushShapes", makeCallbackF( KrBrushShapes_show ) ); // KenRadiant
	GlobalCommands_insert( "CheckForProblems", makeCallbackF( KrCheckProblems_show ), QKeySequence( "Ctrl+Alt+P" ) ); // KenRadiant (Hammer: Alt+P, which opens the Plugins menu here)
	extern void KrTerrain_registerCommands();
	KrTerrain_registerCommands();
	extern void KrMapText_registerCommands();
	KrMapText_registerCommands();
	extern void KrMisc_registerCommands();
	KrMisc_registerCommands();
	extern void KrCompile_registerCommands();
	KrCompile_registerCommands();
	extern void KrBspPreview_registerCommands();
	KrBspPreview_registerCommands();
	GlobalCommands_insert( "SunCalibrator", makeCallbackF( KrSunCalibrator_show ) ); // KenRadiant
	GlobalCommands_insert( "CycleSelectionStyle", makeCallbackF( SelectionStyle_cycle ), QKeySequence( "J" ) ); // KenRadiant
	GlobalPreferenceSystem().registerPreference( "KrSelectionStyle", IntImportStringCaller( g_krSelectionStyle ), IntExportStringCaller( g_krSelectionStyle ) );
	GlobalPreferenceSystem().registerPreference( "KrAnimateShaders", BoolImportStringCaller( g_krAnimateShaders ), BoolExportStringCaller( g_krAnimateShaders ) );
	GlobalPreferenceSystem().registerPreference( "KrLightPreviewShadows", BoolImportStringCaller( g_krShadows ), BoolExportStringCaller( g_krShadows ) );

	GlobalToggles_insert( "ToggleCubicClip", makeCallbackF( Camera_ToggleFarClip ), ToggleItem::AddCallbackCaller( g_getfarclip_item ), QKeySequence( "Ctrl+\\" ) );
	GlobalCommands_insert( "CubicClipZoomIn", makeCallbackF( Camera_CubeIn ), QKeySequence( "Ctrl+[" ) );
	GlobalCommands_insert( "CubicClipZoomOut", makeCallbackF( Camera_CubeOut ), QKeySequence( "Ctrl+]" ) );

	GlobalCommands_insert( "UpFloor", makeCallbackF( Camera_ChangeFloorUp ), QKeySequence( "PgUp" ) );
	GlobalCommands_insert( "DownFloor", makeCallbackF( Camera_ChangeFloorDown ), QKeySequence( "PgDown" ) );

	GlobalToggles_insert( "ToggleCamera", ToggleShown::ToggleCaller( g_camera_shown ), ToggleItem::AddCallbackCaller( g_camera_shown.m_item ), QKeySequence( "Ctrl+Shift+C" ) );
//	GlobalCommands_insert( "LookThroughSelected", makeCallbackF( GlobalCamera_LookThroughSelected ) );
//	GlobalCommands_insert( "LookThroughCamera", makeCallbackF( GlobalCamera_LookThroughCamera ) );

	if ( g_pGameDescription->mGameType == "doom3" ) {
		GlobalCommands_insert( "TogglePreview", makeCallbackF( CamWnd_TogglePreview ), QKeySequence( "F3" ) );
	}

	GlobalCommands_insert( "CameraModeNext", makeCallbackF( CameraModeNext ), QKeySequence( "Shift+]" ) );
	GlobalCommands_insert( "CameraModePrev", makeCallbackF( CameraModePrev ), QKeySequence( "Shift+[" ) );

	GlobalCommands_insert( "CameraSpeedInc", makeCallbackF( CameraSpeed_increase ), QKeySequence( +Qt::SHIFT + Qt::Key_Plus + Qt::KeypadModifier ) );
	GlobalCommands_insert( "CameraSpeedDec", makeCallbackF( CameraSpeed_decrease ), QKeySequence( +Qt::SHIFT + Qt::Key_Minus + Qt::KeypadModifier ) );

	GlobalShortcuts_insert( "CameraForward", QKeySequence( "Up" ) );
	GlobalShortcuts_insert( "CameraBack", QKeySequence( "Down" ) );
	GlobalShortcuts_insert( "CameraLeft", QKeySequence( "Left" ) );
	GlobalShortcuts_insert( "CameraRight", QKeySequence( "Right" ) );
	GlobalShortcuts_insert( "CameraStrafeRight", QKeySequence( "D" ) );
	GlobalShortcuts_insert( "CameraStrafeLeft", QKeySequence( "A" ) );

	GlobalShortcuts_insert( "CameraUp" );
	GlobalShortcuts_insert( "CameraDown" );
	GlobalShortcuts_insert( "CameraAngleUp" );
	GlobalShortcuts_insert( "CameraAngleDown" );

	GlobalShortcuts_insert( "CameraFreeMoveForward", QKeySequence( "W" ) );
	GlobalShortcuts_insert( "CameraFreeMoveBack", QKeySequence( "S" ) );
	GlobalShortcuts_insert( "CameraFreeMoveLeft", QKeySequence( "A" ) );
	GlobalShortcuts_insert( "CameraFreeMoveRight", QKeySequence( "D" ) );

	GlobalShortcuts_insert( "CameraFreeMoveForward2", QKeySequence( "Up" ) );
	GlobalShortcuts_insert( "CameraFreeMoveBack2", QKeySequence( "Down" ) );
	GlobalShortcuts_insert( "CameraFreeMoveLeft2", QKeySequence( "Left" ) );
	GlobalShortcuts_insert( "CameraFreeMoveRight2", QKeySequence( "Right" ) );

	GlobalShortcuts_insert( "CameraFreeMoveUp", QKeySequence( "Space" ) ); // KenRadiant: while flying (right mouse held); Space = duplicate otherwise
	GlobalShortcuts_insert( "CameraFreeMoveDown", QKeySequence( "C" ) ); // KenRadiant: while flying; C = deselect otherwise

	GlobalShortcuts_insert( "CameraFreeFocus", QKeySequence( "Tab" ) );

	GlobalToggles_insert( "ShowStats", makeCallbackF( ShowStatsToggle ), ToggleItem::AddCallbackCaller( g_show_stats ) );
	GlobalToggles_insert( "ShowWorkzone3d", makeCallbackF( ShowWorkzone3dToggle ), ToggleItem::AddCallbackCaller( g_show_workzone3d ) );
	GlobalToggles_insert( "ShowSize3d", makeCallbackF( ShowSize3dToggle ), ToggleItem::AddCallbackCaller( g_show_size3d ) );

	GlobalPreferenceSystem().registerPreference( "ShowStats", BoolImportStringCaller( g_camwindow_globals.m_showStats ), BoolExportStringCaller( g_camwindow_globals.m_showStats ) );
	GlobalPreferenceSystem().registerPreference( "ShowWorkzone3d", BoolImportStringCaller( g_camwindow_globals_private.m_bShowWorkzone ), BoolExportStringCaller( g_camwindow_globals_private.m_bShowWorkzone ) );
	GlobalPreferenceSystem().registerPreference( "ShowSize3d", BoolImportStringCaller( g_camwindow_globals_private.m_bShowSize ), BoolExportStringCaller( g_camwindow_globals_private.m_bShowSize ) );
	GlobalPreferenceSystem().registerPreference( "CamMoveSpeed", IntImportStringCaller( g_camwindow_globals_private.m_nMoveSpeed ), IntExportStringCaller( g_camwindow_globals_private.m_nMoveSpeed ) );
	GlobalPreferenceSystem().registerPreference( "CamMoveTimeToMaxSpeed", IntImportStringCaller( g_camwindow_globals_private.m_time_toMaxSpeed ), IntExportStringCaller( g_camwindow_globals_private.m_time_toMaxSpeed ) );
	GlobalPreferenceSystem().registerPreference( "ScrollMoveSpeed", IntImportStringCaller( g_camwindow_globals_private.m_nScrollMoveSpeed ), IntExportStringCaller( g_camwindow_globals_private.m_nScrollMoveSpeed ) );
	GlobalPreferenceSystem().registerPreference( "CamStrafeSpeed", FloatImportStringCaller( g_camwindow_globals_private.m_strafeSpeed ), FloatExportStringCaller( g_camwindow_globals_private.m_strafeSpeed ) );
	GlobalPreferenceSystem().registerPreference( "Sensitivity", FloatImportStringCaller( g_camwindow_globals_private.m_angleSpeed ), FloatExportStringCaller( g_camwindow_globals_private.m_angleSpeed ) );
	GlobalPreferenceSystem().registerPreference( "CamInverseMouse", BoolImportStringCaller( g_camwindow_globals_private.m_bCamInverseMouse ), BoolExportStringCaller( g_camwindow_globals_private.m_bCamInverseMouse ) );
	GlobalPreferenceSystem().registerPreference( "CamDiscrete", makeBoolStringImportCallback( CamWndMoveDiscreteImportCaller() ), BoolExportStringCaller( g_camwindow_globals_private.m_bCamDiscrete ) );
	GlobalPreferenceSystem().registerPreference( "CubicClipping", BoolImportStringCaller( g_camwindow_globals_private.m_bCubicClipping ), BoolExportStringCaller( g_camwindow_globals_private.m_bCubicClipping ) );
	GlobalPreferenceSystem().registerPreference( "CubicScale", IntImportStringCaller( g_camwindow_globals.m_nCubicScale ), IntExportStringCaller( g_camwindow_globals.m_nCubicScale ) );
	GlobalPreferenceSystem().registerPreference( "ColorCameraBackground", Vector3ImportStringCaller( g_camwindow_globals.color_cameraback ), Vector3ExportStringCaller( g_camwindow_globals.color_cameraback ) );
	GlobalPreferenceSystem().registerPreference( "ColorCameraSelection", Vector3ImportStringCaller( g_camwindow_globals.color_selbrushes3d ), Vector3ExportStringCaller( g_camwindow_globals.color_selbrushes3d ) );
	GlobalPreferenceSystem().registerPreference( "CameraRenderMode", makeIntStringImportCallback( RenderModeImportCaller() ), makeIntStringExportCallback( RenderModeExportCaller() ) );
	GlobalPreferenceSystem().registerPreference( "CameraMSAA", IntImportStringCaller( g_camwindow_globals_private.m_MSAA ), IntExportStringCaller( g_camwindow_globals_private.m_MSAA ) );
	GlobalPreferenceSystem().registerPreference( "StrafeMode", IntImportStringCaller( g_camwindow_globals_private.m_strafeMode ), IntExportStringCaller( g_camwindow_globals_private.m_strafeMode ) );
	GlobalPreferenceSystem().registerPreference( "CameraFaceWire", BoolImportStringCaller( g_camwindow_globals_private.m_bFaceWire ), BoolExportStringCaller( g_camwindow_globals_private.m_bFaceWire ) );
	GlobalPreferenceSystem().registerPreference( "CameraFaceFill", BoolImportStringCaller( g_camwindow_globals_private.m_bFaceFill ), BoolExportStringCaller( g_camwindow_globals_private.m_bFaceFill ) );
	GlobalPreferenceSystem().registerPreference( "3DZoomInToPointer", BoolImportStringCaller( g_camwindow_globals_private.m_bZoomToPointer ), BoolExportStringCaller( g_camwindow_globals_private.m_bZoomToPointer ) );
	GlobalPreferenceSystem().registerPreference( "fieldOfView", FloatImportStringCaller( camera_t::fieldOfView ), FloatExportStringCaller( camera_t::fieldOfView ) );
	//.  HACK: always show camera from start to have at least one ogl viewport shown = ogl initialized; otherwise loading map = loading textures = crash
//	GlobalPreferenceSystem().registerPreference( "CamVIS", makeBoolStringImportCallback( ToggleShownImportBoolCaller( g_camera_shown ) ), makeBoolStringExportCallback( ToggleShownExportBoolCaller( g_camera_shown ) ) );

	CamWnd_constructStatic();

	Camera_registerPreferencesPage();
}
void CamWnd_Destroy(){
	CamWnd_destroyStatic();
}
