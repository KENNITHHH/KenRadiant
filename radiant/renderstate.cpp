/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

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

#include "renderstate.h"

/// KenRadiant: draw sky brushes as the actual skybox (netradiant-custom) or with their editor image (GtkRadiant, default)
bool g_skyboxPreview = false;

#include "debugging/debugging.h"

#include "ishaders.h"
#include "irender.h"
#include "itextures.h"
#include "igl.h"
#include "iglrender.h"
#include "renderable.h"
#include "qerplugin.h"

#include <set>
#include <vector>
#include <list>
#include <map>

#include "math/matrix.h"
#include "math/aabb.h"
#include "generic/callback.h"
#include "texturelib.h"
#include "string/string.h"
#include "container/hashfunc.h"
#include "container/cache.h"
#include "generic/reference.h"
#include "moduleobservers.h"
#include "stream/filestream.h"
#include "stream/stringstream.h"
#include "os/file.h"
#include "preferences.h"

#include "xywindow.h"
#include "camwindow.h"



#define DEBUG_RENDER 0

inline void debug_string( const char* string ){
#if ( DEBUG_RENDER )
	globalOutputStream() << string << '\n';
#endif
}

inline void debug_int( const char* comment, int i ){
#if ( DEBUG_RENDER )
	globalOutputStream() << comment << ' ' << i << '\n';
#endif
}

inline void debug_colour( const char* comment ){
#if ( DEBUG_RENDER )
	Vector4 v;
	gl().glGetFloatv( GL_CURRENT_COLOR, reinterpret_cast<float*>( &v ) );
	globalOutputStream() << comment << " colour: "
	                     << v[0] << ' '
	                     << v[1] << ' '
	                     << v[2] << ' '
	                     << v[3];
	if ( gl().glIsEnabled( GL_COLOR_ARRAY ) ) {
		globalOutputStream() << " ARRAY";
	}
	if ( gl().glIsEnabled( GL_COLOR_MATERIAL ) ) {
		globalOutputStream() << " MATERIAL";
	}
	globalOutputStream() << '\n';
#endif
}

#include "timer.h"

StringOutputStream g_renderer_stats;
std::size_t g_count_prims;
std::size_t g_count_states;
std::size_t g_count_transforms;
Timer g_timer;

inline void count_prim(){
	++g_count_prims;
}

inline void count_state(){
	++g_count_states;
}

inline void count_transform(){
	++g_count_transforms;
}

void Renderer_ResetStats(){
	g_count_prims = 0;
	g_count_states = 0;
	g_count_transforms = 0;
	g_timer.start();
}

const char* Renderer_GetStats( int frame2frame ){
	return g_renderer_stats(
		"prims: ", g_count_prims,
		" | states: ", g_count_states,
		" | transforms: ", g_count_transforms,
		" | msec: ", g_timer.elapsed_msec(),
		" | f2f: ", frame2frame
	);
}


void printShaderLog( GLuint shader ){
	GLint log_length = 0;
	gl().glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &log_length );

	Array<char> log( log_length );
	gl().glGetShaderInfoLog( shader, log_length, &log_length, log.data() );

	globalErrorStream() << StringRange( log.begin(), log_length ) << '\n';
}

void printProgramLog( GLuint program ){
	GLint log_length = 0;
	gl().glGetProgramiv( program, GL_INFO_LOG_LENGTH, &log_length );

	Array<char> log( log_length );
	gl().glGetProgramInfoLog( program, log_length, &log_length, log.data() );

	globalErrorStream() << StringRange( log.begin(), log_length ) << '\n';
}

void createShader( GLuint program, const char* filename, GLenum type );
/// compiles with a source prefix (defines); returns false instead of asserting when it fails
bool createShaderTry( GLuint program, const char* filename, GLenum type, const char* prefix ){
	GLuint shader = gl().glCreateShader( type );
	std::size_t size = file_size( filename );
	FileInputStream file( filename );
	if( file.failed() ){
		gl().glDeleteShader( shader );
		return false;
	}
	Array<GLchar> buffer( size );
	size = file.read( reinterpret_cast<StreamBase::byte_type*>( buffer.data() ), size );
	const GLchar* strings[2] = { prefix, buffer.data() };
	GLint lengths[2] = { GLint( strlen( prefix ) ), GLint( size ) };
	gl().glShaderSource( shader, 2, strings, lengths );
	gl().glCompileShader( shader );
	GLint compiled = 0;
	gl().glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
	if( !compiled ){
		printShaderLog( shader );
		gl().glDeleteShader( shader );
		return false;
	}
	gl().glAttachShader( program, shader );
	gl().glDeleteShader( shader );
	return true;
}
void createShader( GLuint program, const char* filename, GLenum type ){
	const char* prefix = "";
	GLuint shader = gl().glCreateShader( type );
	GlobalOpenGL_debugAssertNoErrors();

	// load shader
	{
		std::size_t size = file_size( filename );
		FileInputStream file( filename );
		ASSERT_MESSAGE( !file.failed(), "failed to open " << Quoted( filename ) );
		Array<GLchar> buffer( size );
		size = file.read( reinterpret_cast<StreamBase::byte_type*>( buffer.data() ), size );

		const GLchar* strings[2] = { prefix, buffer.data() };
		GLint lengths[2] = { GLint( strlen( prefix ) ), GLint( size ) };
		gl().glShaderSource( shader, 2, strings, lengths );
	}

	// compile shader
	{
		gl().glCompileShader( shader );

		GLint compiled = 0;
		gl().glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );

		if ( !compiled ) {
			printShaderLog( shader );
		}

		ASSERT_MESSAGE( compiled, "shader compile failed: " << Quoted( filename ) );
	}

	// attach shader
	gl().glAttachShader( program, shader );

	gl().glDeleteShader( shader );

	GlobalOpenGL_debugAssertNoErrors();
}

void GLSLProgram_link( GLuint program ){
	gl().glLinkProgram( program );

	GLint linked = false;
	gl().glGetProgramiv( program, GL_LINK_STATUS, &linked );

	if ( !linked ) {
		printProgramLog( program );
	}

	ASSERT_MESSAGE( linked, "program link failed" );
}

void GLSLProgram_validate( GLuint program ){
	gl().glValidateProgram( program );

	GLint validated = false;
	gl().glGetProgramiv( program, GL_VALIDATE_STATUS, &validated );

	if ( !validated ) {
		printProgramLog( program );
	}

	ASSERT_MESSAGE( validated, "program validation failed" );
}

bool g_bumpGLSLPass_enabled = false;
bool g_depthfillPass_enabled = false;

class GLSLBumpProgram : public GLProgram
{
public:
	GLuint m_program;
	qtexture_t* m_light_attenuation_xy;
	qtexture_t* m_light_attenuation_z;
	GLint u_view_origin;
	GLint u_light_origin;
	GLint u_light_color;
	GLint u_bump_scale;
	GLint u_specular_exponent;

	GLSLBumpProgram() : m_program( 0 ), m_light_attenuation_xy( 0 ), m_light_attenuation_z( 0 ){
	}

	void create(){
		// create program
		m_program = gl().glCreateProgram();

		// create shader
		{
			StringOutputStream filename( 256 );
			createShader( m_program, filename( GlobalRadiant().getAppPath(), "gl/lighting_DBS_omni_vp.glsl" ), GL_VERTEX_SHADER );
			createShader( m_program, filename( GlobalRadiant().getAppPath(), "gl/lighting_DBS_omni_fp.glsl" ), GL_FRAGMENT_SHADER );
		}

		GLSLProgram_link( m_program );
		GLSLProgram_validate( m_program );

		gl().glUseProgram( m_program );

		gl().glBindAttribLocation( m_program, c_attr_TexCoord0, "attr_TexCoord0" );
		gl().glBindAttribLocation( m_program, c_attr_Tangent, "attr_Tangent" );
		gl().glBindAttribLocation( m_program, c_attr_Binormal, "attr_Binormal" );

		gl().glUniform1i( gl().glGetUniformLocation( m_program, "u_diffusemap" ), 0 );
		gl().glUniform1i( gl().glGetUniformLocation( m_program, "u_bumpmap" ), 1 );
		gl().glUniform1i( gl().glGetUniformLocation( m_program, "u_specularmap" ), 2 );
		gl().glUniform1i( gl().glGetUniformLocation( m_program, "u_attenuationmap_xy" ), 3 );
		gl().glUniform1i( gl().glGetUniformLocation( m_program, "u_attenuationmap_z" ), 4 );

		u_view_origin = gl().glGetUniformLocation( m_program, "u_view_origin" );
		u_light_origin = gl().glGetUniformLocation( m_program, "u_light_origin" );
		u_light_color = gl().glGetUniformLocation( m_program, "u_light_color" );
		u_bump_scale = gl().glGetUniformLocation( m_program, "u_bump_scale" );
		u_specular_exponent = gl().glGetUniformLocation( m_program, "u_specular_exponent" );

		gl().glUseProgram( 0 );

		GlobalOpenGL_debugAssertNoErrors();
	}

	void destroy(){
		gl().glDeleteProgram( m_program );
		m_program = 0;
	}

	void enable() override {
		gl().glUseProgram( m_program );

		gl().glEnableVertexAttribArray( c_attr_TexCoord0 );
		gl().glEnableVertexAttribArray( c_attr_Tangent );
		gl().glEnableVertexAttribArray( c_attr_Binormal );

		GlobalOpenGL_debugAssertNoErrors();

		debug_string( "enable bump" );
		g_bumpGLSLPass_enabled = true;
	}

	void disable() override {
		gl().glUseProgram( 0 );

		gl().glDisableVertexAttribArray( c_attr_TexCoord0 );
		gl().glDisableVertexAttribArray( c_attr_Tangent );
		gl().glDisableVertexAttribArray( c_attr_Binormal );

		GlobalOpenGL_debugAssertNoErrors();

		debug_string( "disable bump" );
		g_bumpGLSLPass_enabled = false;
	}

	void setParameters( const Vector3& viewer, const Matrix4& localToWorld, const Vector3& origin, const Vector3& colour, const Matrix4& world2light ) override {
		Matrix4 world2local( localToWorld );
		matrix4_affine_invert( world2local );

		Vector3 localLight( origin );
		matrix4_transform_point( world2local, localLight );

		Vector3 localViewer( viewer );
		matrix4_transform_point( world2local, localViewer );

		Matrix4 local2light( world2light );
		matrix4_multiply_by_matrix4( local2light, localToWorld ); // local->world->light

		gl().glUniform3f( u_view_origin, localViewer.x(), localViewer.y(), localViewer.z() );
		gl().glUniform3f( u_light_origin, localLight.x(), localLight.y(), localLight.z() );
		gl().glUniform3f( u_light_color, colour.x(), colour.y(), colour.z() );
		gl().glUniform1f( u_bump_scale, 1.f );
		gl().glUniform1f( u_specular_exponent, 32.f );

		gl().glActiveTexture( GL_TEXTURE3 );
		gl().glClientActiveTexture( GL_TEXTURE3 );

		gl().glMatrixMode( GL_TEXTURE );
		gl().glLoadMatrixf( reinterpret_cast<const float*>( &local2light ) );
		gl().glMatrixMode( GL_MODELVIEW );

		GlobalOpenGL_debugAssertNoErrors();
	}
};

GLSLBumpProgram g_bumpGLSL;


class GLSLDepthFillProgram : public GLProgram
{
public:
	GLuint m_program;

	void create(){
		// create program
		m_program = gl().glCreateProgram();

		// create shader
		{
			StringOutputStream filename( 256 );
			createShader( m_program, filename( GlobalRadiant().getAppPath(), "gl/zfill_vp.glsl" ), GL_VERTEX_SHADER );
			createShader( m_program, filename( GlobalRadiant().getAppPath(), "gl/zfill_fp.glsl" ), GL_FRAGMENT_SHADER );
		}

		GLSLProgram_link( m_program );
		GLSLProgram_validate( m_program );

		GlobalOpenGL_debugAssertNoErrors();
	}

	void destroy(){
		gl().glDeleteProgram( m_program );
		m_program = 0;
	}
	void enable() override {
		gl().glUseProgram( m_program );
		GlobalOpenGL_debugAssertNoErrors();
		debug_string( "enable depthfill" );
		g_depthfillPass_enabled = true;
	}
	void disable() override {
		gl().glUseProgram( 0 );
		GlobalOpenGL_debugAssertNoErrors();
		debug_string( "disable depthfill" );
		g_depthfillPass_enabled = false;
	}
	void setParameters( const Vector3& viewer, const Matrix4& localToWorld, const Vector3& origin, const Vector3& colour, const Matrix4& world2light ) override {
	}
};

GLSLDepthFillProgram g_depthFillGLSL;


class GLSLSkyboxProgram : public GLProgram
{
public:
	GLuint m_program;
	GLint u_view_origin;

	GLSLSkyboxProgram() : m_program( 0 ){
	}

	void create(){
		// create program
		m_program = gl().glCreateProgram();

		// create shader
		{
			StringOutputStream filename( 256 );
			createShader( m_program, filename( GlobalRadiant().getAppPath(), "gl/skybox_vp.glsl" ), GL_VERTEX_SHADER );
			createShader( m_program, filename( GlobalRadiant().getAppPath(), "gl/skybox_fp.glsl" ), GL_FRAGMENT_SHADER );
		}

		GLSLProgram_link( m_program );
		GLSLProgram_validate( m_program );

		gl().glUseProgram( m_program );

		u_view_origin = gl().glGetUniformLocation( m_program, "u_view_origin" );

		gl().glUseProgram( 0 );

		GlobalOpenGL_debugAssertNoErrors();
	}

	void destroy(){
		gl().glDeleteProgram( m_program );
		m_program = 0;
	}

	void enable() override {
		gl().glUseProgram( m_program );

		GlobalOpenGL_debugAssertNoErrors();

		debug_string( "enable skybox" );
	}

	void disable() override {
		gl().glUseProgram( 0 );

		GlobalOpenGL_debugAssertNoErrors();

		debug_string( "disable skybox" );
	}

	void setParameters( const Vector3& viewer, const Matrix4& localToWorld, const Vector3& origin, const Vector3& colour, const Matrix4& world2light ) override {
		gl().glUniform3f( u_view_origin, viewer.x(), viewer.y(), viewer.z() );

		GlobalOpenGL_debugAssertNoErrors();
	}
};

GLSLSkyboxProgram g_skyboxGLSL;

/* KenRadiant live light preview */
#include "kr_lightpreview.h"
#include "kr_terrain.h"
bool g_krLightPreview = false;
bool g_krLightPreviewDrawing = false;
bool g_krProbeDebug = false; // Lighting Probe frame: the light preview writes stored light values
Matrix4 g_krLightPreviewView = g_matrix4_identity;
std::vector<KrPreviewLight> g_krLightPreviewLights;
Vector3 g_krSunDirection( 0, 0, 1 );
Vector3 g_krSunColour( 0, 0, 0 );
Vector3 g_krAmbient( 0.15f, 0.15f, 0.15f );
Vector3 g_krSkyAmbient( 0, 0, 0 );
float g_krMinLight = 0;
bool g_krShadows = true;
bool g_krAnimateShaders = true;
bool g_krDeformVertexes = true;
bool g_krPolygonOffset = true;
bool g_krGlow = true;
bool g_krGameShaders = true; // draw every stage of a shader like the game (View > Shader Effects)

/// one stage of a shader as a render pass
struct KrStageGL
{
	const KrStage* stage = nullptr;
	std::vector<GLuint> frames;  // animMap
	bool lit = false;            // takes the light preview (else drawn at full brightness, e.g. glows)
	float lmScale = 1;           // the lightmap stage's blend: 2 for GL_DST_COLOR GL_SRC_COLOR (2x modulate)
	bool vertexCopy = false;     // the same image added again with rgbGen vertex (SOF2's 2x for vertex light): only lit
};
/// rgbGen / alphaGen of a stage at time t
inline Vector4 KrStage_colour( const KrStage& st, float t );
float g_krShaderTime = 0;
bool g_krAnimatedDrawn = false; // an animated shader was drawn this frame: the 3D view keeps redrawing
unsigned int g_krShadowTex = 0;
Matrix4 g_krEye2Shadow = g_matrix4_identity;
float g_krShadowTexel = 1.f / 4096;
float g_krShadowBiasUnit = 0;
float g_krSunSoftness = 1;
KrCompilePreview g_krCompile;
Vector3 g_krBounceFill( 0, 0, 0 );
bool g_krLightShadows = true;
int g_krShadowedLights = 4;
int g_krPreviewLightCount = int( c_krMaxPreviewLights );
int g_krMapLightCount = 0;
int g_krEngineShift = 0; // SOF2 as KENNITH plays it: lightmaps as stored, the shaders' own 2x modulate brightens them
float g_krEngineGamma = 1;
int g_krMaxPreviewLightsGPU = int( c_krMaxPreviewLights );
unsigned int g_krLightTex = 0;
Vector3 g_krGridMin( 0, 0, 0 );
int g_krLightUnit = 16;
std::size_t g_krMaxCubes = c_krMaxCubeShadows;
unsigned int g_krCubeTex[c_krMaxCubeShadows] = {};
float g_krCubeNear = 4, g_krCubeFar[c_krMaxCubeShadows] = {};
Matrix4 g_krEye2World = g_matrix4_identity;
unsigned int g_krSkyShadowTex = 0;
Matrix4 g_krEye2Sky[c_krSkyDirs];
Vector3 g_krSkyDirs[c_krSkyDirs];
float g_krSkyBias = 0;

/// Quake 3 waveform: base + amp * func( phase + time * freq )
inline float KrWave( int func, const float wave[4], float time ){
	const float x = wave[2] + time * wave[3];
	const float f = x - std::floor( x );
	float v = 0;
	switch( func ){
	case 1: v = std::sin( x * 2 * float( c_pi ) ); break;
	case 2: v = 4 * std::fabs( ( x - 0.25f ) - std::floor( x - 0.25f ) - 0.5f ) - 1; break;
	case 3: v = f < 0.5f ? 1.f : -1.f; break;
	case 4: v = f; break;
	case 5: v = 1 - f; break;
	}
	return wave[0] + wave[1] * v;
}

class GLSLKrLightProgram : public GLProgram
{
	GLint u( const char* name ) const { return gl().glGetUniformLocation( m_program, name ); }
public:
	GLuint m_program = 0;
	GLint u_sky, u_minlight;
	GLint u_has_lights, u_grid_min, u_cam_world, u_has_sky, u_eye2sky, u_sky_dir, u_sky_bias;
	GLint u_tex, u_count, u_sun_dir, u_sun_col, u_ambient, u_eye2shadow, u_has_shadow, u_texel, u_bias_unit, u_soft;
	GLint u_cube_far2;
	GLint u_terrain, u_tmins, u_tsize, u_tscale;
	GLint u_overbright, u_rgamma, u_stage, u_env, u_cam_vs, u_lmscale, u_brightness, u_vscale, u_vertexlit, u_debug, u_dbgcut;
	GLint u_cube_far, u_cube_near, u_eye2world, u_gamma, u_compensate, u_saturation, u_bounce;
	GLint u_mode, u_fflight, u_time, u_turb, u_turb_on, u_deform, u_dfunc, u_ddiv, u_dwave, u_dmove, u_bulge;
	bool m_preview = false; // lit preview pass (the 3D view with Ctrl+L on)
	bool m_bound = false;

	void create(){
		m_program = gl().glCreateProgram();
		{
			StringOutputStream filename( 256 );
			createShader( m_program, filename( GlobalRadiant().getAppPath(), "gl/kr_light_vp.glsl" ), GL_VERTEX_SHADER );
			// the lights texture layout comes from kr_lightpreview.h
			const auto prefix = StringStream<512>( "#define KR_LIGHTS_TEXELS ", int( c_krMaxPreviewLights ) * c_krLightTexels, ".0\n",
			                                       "#define KR_TH ", c_krLightTexRows, ".0\n",
			                                       "#define KR_GRID vec3( ", c_krGridX, ".0, ", c_krGridY, ".0, ", c_krGridZ, ".0 )\n",
			                                       "#define KR_CELL ", c_krCellSize, ".0\n",
			                                       "#define KR_CELL_TEXELS ", c_krCellSlots / 4, "\n" );
			if( !createShaderTry( m_program, filename( GlobalRadiant().getAppPath(), "gl/kr_light_fp.glsl" ), GL_FRAGMENT_SHADER, prefix ) )
				globalErrorStream() << "light preview: kr_light_fp.glsl failed to compile\n";
		}
		GLSLProgram_link( m_program );
		gl().glUseProgram( m_program );
		u_tex = u( "u_tex" ); u_has_lights = u( "u_has_lights" ); u_grid_min = u( "u_grid_min" ); u_cam_world = u( "u_cam_world" );
		u_has_sky = u( "u_has_sky" ); u_eye2sky = u( "u_eye2sky" ); u_sky_dir = u( "u_sky_dir" ); u_sky_bias = u( "u_sky_bias" );
		gl().glUniform1i( u( "u_skyshadow" ), 6 );
		u_sky = u( "u_sky" ); u_minlight = u( "u_minlight" );
		u_terrain = u( "u_terrain" ); u_tmins = u( "u_tmins" ); u_tsize = u( "u_tsize" ); u_tscale = u( "u_tscale" );
		u_sun_dir = u( "u_sun_dir" ); u_sun_col = u( "u_sun_col" ); u_ambient = u( "u_ambient" );
		u_eye2shadow = u( "u_eye2shadow" ); u_has_shadow = u( "u_has_shadow" ); u_texel = u( "u_texel" ); u_bias_unit = u( "u_bias_unit" ); u_soft = u( "u_soft" );
		u_cube_far = u( "u_cube_far" ); u_cube_far2 = u( "u_cube_far2" ); u_cube_near = u( "u_cube_near" ); u_eye2world = u( "u_eye2world" );
		u_lmscale = u( "u_lmscale" ); u_brightness = u( "u_brightness" ); u_vscale = u( "u_vscale" ); u_vertexlit = u( "u_vertexlit" ); u_debug = u( "u_debug" ); u_dbgcut = u( "u_dbgcut" ); u_stage = u( "u_stage" ); u_env = u( "u_env" ); u_cam_vs = u( "u_cam_vs" ); u_overbright = u( "u_overbright" ); u_rgamma = u( "u_rgamma" ); u_gamma = u( "u_gamma" ); u_compensate = u( "u_compensate" ); u_saturation = u( "u_saturation" ); u_bounce = u( "u_bounce" );
		u_mode = u( "u_mode" ); u_fflight = u( "u_fflight" ); u_time = u( "u_time" ); u_turb = u( "u_turb" ); u_turb_on = u( "u_turb_on" );
		u_deform = u( "u_deform" ); u_dfunc = u( "u_dfunc" ); u_ddiv = u( "u_ddiv" ); u_dwave = u( "u_dwave" ); u_dmove = u( "u_dmove" ); u_bulge = u( "u_bulge" );
		gl().glUniform1i( u_tex, 0 );
		gl().glUniform1i( u( "u_shadow" ), 7 );
		gl().glUniform1i( u( "u_cube0" ), 8 );
		gl().glUniform1i( u( "u_cube1" ), 9 );
		gl().glUniform1i( u( "u_cube2" ), 10 );
		gl().glUniform1i( u( "u_cube3" ), 11 );
		gl().glUniform1i( u( "u_tidx" ), 1 ); // terrain: index map, layers on 2-5
		gl().glUniform1i( u( "u_tl0" ), 2 );
		gl().glUniform1i( u( "u_tl1" ), 3 );
		gl().glUniform1i( u( "u_tl2" ), 4 );
		gl().glUniform1i( u( "u_tl3" ), 5 );
		gl().glUniform1i( u( "u_cube4" ), 12 );
		gl().glUniform1i( u( "u_cube5" ), 13 );
		gl().glUniform1i( u( "u_cube6" ), 14 );
		// the lights texture: unit 16 when the card has more than 16, else 15 (the 8th shadow cube gives way)
		GLint units = 16;
		gl().glGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS, &units );
		g_krLightUnit = units > 16 ? 16 : 15;
		g_krMaxCubes = units > 16 ? c_krMaxCubeShadows : c_krMaxCubeShadows - 1;
		gl().glUniform1i( u( "u_cube7" ), units > 16 ? 15 : 14 );
		gl().glUniform1i( u( "u_lights" ), g_krLightUnit );
		gl().glUseProgram( 0 );
		GlobalOpenGL_debugAssertNoErrors();
	}
	void destroy(){
		gl().glDeleteProgram( m_program );
		m_program = 0;
		m_bound = false;
	}
	void bind( bool on ){
		if( on != m_bound ){
			gl().glUseProgram( on ? m_program : 0 );
			m_bound = on;
		}
	}
	void enable() override {
		// the lit preview only in the 3D view; otherwise the program runs just for surfaces with vertex effects (applyState)
		m_preview = g_krLightPreview && g_krLightPreviewDrawing && m_program != 0;
		if( !m_preview )
			return;
		bind( true );
		const std::size_t count = g_krLightPreviewLights.size();
		gl().glUniform1i( u_has_lights, count != 0 && g_krLightTex != 0 ? 1 : 0 );
		gl().glUniform3f( u_grid_min, g_krGridMin.x(), g_krGridMin.y(), g_krGridMin.z() );
		gl().glActiveTexture( GL_TEXTURE0 + GLenum( g_krLightUnit ) );
		gl().glBindTexture( GL_TEXTURE_2D, g_krLightTex );
		gl().glActiveTexture( GL_TEXTURE0 );
		// sky light occlusion
		gl().glUniform1i( u_has_sky, g_krShadows && g_krSkyShadowTex != 0 ? 1 : 0 );
		gl().glUniformMatrix4fv( u_eye2sky, c_krSkyDirs, GL_FALSE, reinterpret_cast<const float*>( g_krEye2Sky ) );
		gl().glUniform3fv( u_sky_dir, c_krSkyDirs, reinterpret_cast<const float*>( g_krSkyDirs ) );
		gl().glUniform1f( u_sky_bias, g_krSkyBias );
		gl().glActiveTexture( GL_TEXTURE6 );
		gl().glBindTexture( GL_TEXTURE_2D, g_krShadows ? g_krSkyShadowTex : 0 );
		gl().glActiveTexture( GL_TEXTURE0 );
		const Vector3 sun = vector3_normalised( matrix4_transformed_direction( g_krLightPreviewView, g_krSunDirection ) );
		gl().glUniform3f( u_sun_dir, sun.x(), sun.y(), sun.z() );
		gl().glUniform3f( u_sun_col, g_krSunColour.x(), g_krSunColour.y(), g_krSunColour.z() );
		gl().glUniform3f( u_ambient, g_krAmbient.x(), g_krAmbient.y(), g_krAmbient.z() );
		gl().glUniform3f( u_sky, g_krSkyAmbient.x(), g_krSkyAmbient.y(), g_krSkyAmbient.z() );
		gl().glUniform1f( u_minlight, g_krMinLight );
		const bool shadow = g_krShadows && g_krShadowTex != 0;
		gl().glUniform1i( u_has_shadow, shadow ? 1 : 0 );
		gl().glUniform1f( u_texel, g_krShadowTexel );
		gl().glUniform1f( u_bias_unit, g_krShadowBiasUnit );
		gl().glUniform1f( u_soft, g_krSunSoftness );
		gl().glUniformMatrix4fv( u_eye2shadow, 1, GL_FALSE, reinterpret_cast<const float*>( &g_krEye2Shadow ) );
		gl().glActiveTexture( GL_TEXTURE7 );
		gl().glBindTexture( GL_TEXTURE_2D, shadow ? g_krShadowTex : 0 );
		// point light cube shadows
		float cubeFar[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		for( std::size_t i = 0; i < c_krMaxCubeShadows; ++i ){
			const bool on = g_krLightShadows && g_krCubeTex[i] != 0 && i < count && i < g_krMaxCubes;
			cubeFar[i] = on ? g_krCubeFar[i] : 0;
			gl().glActiveTexture( GL_TEXTURE8 + GLenum( i ) );
			gl().glBindTexture( GL_TEXTURE_CUBE_MAP, on ? g_krCubeTex[i] : 0 );
		}
		gl().glActiveTexture( GL_TEXTURE0 );
		gl().glUniform4f( u_cube_far, cubeFar[0], cubeFar[1], cubeFar[2], cubeFar[3] );
		gl().glUniform4f( u_cube_far2, cubeFar[4], cubeFar[5], cubeFar[6], cubeFar[7] );
		gl().glUniform1f( u_cube_near, g_krCubeNear );
		const float m3[9] = { g_krEye2World[0], g_krEye2World[1], g_krEye2World[2],
		                      g_krEye2World[4], g_krEye2World[5], g_krEye2World[6],
		                      g_krEye2World[8], g_krEye2World[9], g_krEye2World[10] };
		gl().glUniformMatrix3fv( u_eye2world, 1, GL_FALSE, m3 );
		gl().glUniform3f( u_cam_world, g_krEye2World[12], g_krEye2World[13], g_krEye2World[14] );
		// compile settings preview
		gl().glUniform1f( u_overbright, float( 1 << std::clamp( g_krEngineShift, 0, 2 ) ) );
		gl().glUniform1f( u_rgamma, std::clamp( g_krEngineGamma, 0.5f, 3.f ) );
		const bool c = g_krCompile.enabled;
		// what the LIGHT stage does to the lightmaps: the Compile window's -gamma / -compensate / -brightness / -vertexscale
		// (the Sun Calibrator's compile preview overrides gamma and compensate while it is on)
		float cgamma, ccompensate, cbrightness, cvscale;
		extern void KrCompile_lightParams( float& gamma, float& compensate, float& brightness, float& vertexScale );
		KrCompile_lightParams( cgamma, ccompensate, cbrightness, cvscale );
		gl().glUniform1f( u_gamma, c ? std::max( 0.1f, g_krCompile.gamma ) : cgamma );
		gl().glUniform1f( u_compensate, c ? std::max( 0.1f, g_krCompile.compensate ) : ccompensate );
		gl().glUniform1f( u_brightness, cbrightness );
		gl().glUniform1f( u_vscale, cvscale );
		gl().glUniform1f( u_saturation, c ? g_krCompile.saturation : 1.f );
		gl().glUniform3f( u_bounce, g_krBounceFill.x(), g_krBounceFill.y(), g_krBounceFill.z() );
		GlobalOpenGL_debugAssertNoErrors();
	}
	/// per surface: lit / fullbright glow / unlit, and the vertex effects (tcMod turb, deformVertexes)
	void applyState( const OpenGLState& state, unsigned int globalstate ){
		const KrShaderEffects* fx = state.m_krFx;
		const bool turb = fx != nullptr && fx->turb && g_krAnimateShaders;
		const int deform = fx != nullptr && g_krDeformVertexes ? fx->deform : 0;
		const KrTerrainGL* terrain = state.m_krTerrain >= 0 && ( globalstate & RENDER_TEXTURE ) ? KrTerrain_gl( state.m_krTerrain ) : nullptr;
		const KrStageGL* stage = ( globalstate & RENDER_TEXTURE ) ? state.m_krStage : nullptr;
		bind( m_preview || turb || deform != 0 || terrain != nullptr || stage != nullptr );
		if( !m_bound )
			return;
		gl().glUniform1i( u_terrain, terrain != nullptr ? terrain->layers : 0 );
		if( terrain != nullptr ){
			gl().glActiveTexture( GL_TEXTURE1 );
			gl().glBindTexture( GL_TEXTURE_2D, terrain->indexTex );
			for( int k = 0; k < 4; ++k ){
				gl().glActiveTexture( GL_TEXTURE2 + k );
				gl().glBindTexture( GL_TEXTURE_2D, k < terrain->layers ? terrain->layerTex[k] : 0 );
			}
			gl().glActiveTexture( GL_TEXTURE0 );
			const Vector3 size = terrain->maxs - terrain->mins;
			gl().glUniform2f( u_tmins, terrain->mins.x(), terrain->mins.y() );
			gl().glUniform2f( u_tsize, std::max( size.x(), 1.f ), std::max( size.y(), 1.f ) );
			gl().glUniform2fv( u_tscale, 4, &terrain->layerScale[0][0] );
		}
		if( turb || deform != 0 )
			g_krAnimatedDrawn = true;
		gl().glUniform1i( u_mode, m_preview ? ( state.m_krEmissive && ( g_krGlow || stage != nullptr ) ? 2 : 1 ) : 0 );
		gl().glUniform1i( u_fflight, ( globalstate & RENDER_LIGHTING ) && ( stage == nullptr || stage->lit ) ? 1 : 0 );
		// game look stage: rgbGen / alphaGen colour, tcGen environment, animMap frame
		Vector4 stageColour( 1, 1, 1, 1 );
		int env = 0;
		if( stage != nullptr ){
			const KrStage& st = *stage->stage;
			stageColour = KrStage_colour( st, g_krShaderTime );
			if( stage->vertexCopy && !m_preview )
				stageColour = Vector4( 0, 0, 0, 0 );
			if( st.rgbGen == 2 || st.alphaGen == 2 || stage->frames.size() > 1 )
				g_krAnimatedDrawn = true;
			env = st.tcGen == 2 ? 1 : 0;
			if( stage->frames.size() > 1 && st.animFreq > 0 ){
				const std::size_t frame = std::size_t( std::max( 0.f, g_krShaderTime * st.animFreq ) ) % stage->frames.size();
				gl().glActiveTexture( GL_TEXTURE0 );
				gl().glBindTexture( GL_TEXTURE_2D, stage->frames[frame] );
			}
			if( env ){
				const Matrix4 inv = matrix4_full_inverse( g_krLightPreviewView );
				gl().glUniform3f( u_cam_vs, inv[12], inv[13], inv[14] );
			}
		}
		gl().glUniform4f( u_stage, stageColour[0], stageColour[1], stageColour[2], stageColour[3] );
		gl().glUniform1f( u_lmscale, stage != nullptr ? stage->lmScale : 1.f );
		gl().glUniform1i( u_vertexlit, stage != nullptr && stage->stage->rgbGen == 1 ? 1 : 0 );
		// Lighting Probe: every surface writes its stored light value, no blending (the top surface wins)
		gl().glUniform1i( u_debug, m_preview && g_krProbeDebug ? 1 : 0 );
		if( m_preview && g_krProbeDebug ){
			gl().glUniform1i( u_dbgcut, ( state.m_state & globalstate & RENDER_BLEND ) ? 1 : 0 );
		}
		gl().glUniform1i( u_env, env );
		gl().glUniform1f( u_time, g_krShaderTime );
		gl().glUniform1i( u_turb_on, turb ? 1 : 0 );
		gl().glUniform1i( u_deform, deform );
		if( turb )
			gl().glUniform4fv( u_turb, 1, fx->turbWave );
		if( deform != 0 ){
			gl().glUniform1i( u_dfunc, fx->deformFunc );
			gl().glUniform1f( u_ddiv, fx->deformDiv );
			gl().glUniform4fv( u_dwave, 1, fx->deformWave );
			gl().glUniform3fv( u_dmove, 1, fx->deformMove );
			gl().glUniform3fv( u_bulge, 1, fx->bulge );
		}
	}
	void disable() override {
		bind( false );
		m_preview = false;
	}
	void setParameters( const Vector3& viewer, const Matrix4& localToWorld, const Vector3& origin, const Vector3& colour, const Matrix4& world2light ) override {
	}
};
GLSLKrLightProgram g_krLightGLSL;



bool g_vertexArray_enabled = false;
bool g_normalArray_enabled = false;
bool g_texcoordArray_enabled = false;
bool g_colorArray_enabled = false;

inline bool OpenGLState_less( const OpenGLState& self, const OpenGLState& other ){
	//! Sort by sort-order override.
	if ( self.m_sort != other.m_sort ) {
		return self.m_sort < other.m_sort;
	}
	//! Sort by texture handle.
	if ( self.m_texture != other.m_texture ) {
		return self.m_texture < other.m_texture;
	}
	if ( self.m_texture1 != other.m_texture1 ) {
		return self.m_texture1 < other.m_texture1;
	}
	if ( self.m_texture2 != other.m_texture2 ) {
		return self.m_texture2 < other.m_texture2;
	}
	if ( self.m_texture3 != other.m_texture3 ) {
		return self.m_texture3 < other.m_texture3;
	}
	if ( self.m_texture4 != other.m_texture4 ) {
		return self.m_texture4 < other.m_texture4;
	}
	if ( self.m_texture5 != other.m_texture5 ) {
		return self.m_texture5 < other.m_texture5;
	}
	if ( self.m_texture6 != other.m_texture6 ) {
		return self.m_texture6 < other.m_texture6;
	}
	if ( self.m_texture7 != other.m_texture7 ) {
		return self.m_texture7 < other.m_texture7;
	}
	if ( self.m_textureSkyBox != other.m_textureSkyBox ) {
		return self.m_textureSkyBox < other.m_textureSkyBox;
	}
	//! Sort by state bit-vector.
	if ( self.m_state != other.m_state ) {
		return self.m_state < other.m_state;
	}
	//! Comparing address makes sure states are never equal.
	return &self < &other;
}

void OpenGLState_constructDefault( OpenGLState& state ){
	state.m_state = RENDER_DEFAULT;

	state.m_texture = 0;
	state.m_texture1 = 0;
	state.m_texture2 = 0;
	state.m_texture3 = 0;
	state.m_texture4 = 0;
	state.m_texture5 = 0;
	state.m_texture6 = 0;
	state.m_texture7 = 0;
	state.m_textureSkyBox = 0;

	state.m_colour[0] = 1;
	state.m_colour[1] = 1;
	state.m_colour[2] = 1;
	state.m_colour[3] = 1;

	state.m_depthfunc = GL_LESS;

	state.m_blend_src = GL_SRC_ALPHA;
	state.m_blend_dst = GL_ONE_MINUS_SRC_ALPHA;

	state.m_alphafunc = GL_ALWAYS;
	state.m_alpharef = 0;

	state.m_linewidth = 1;
	state.m_pointsize = 1;

	state.m_linestipple_factor = 1;
	state.m_linestipple_pattern = 0xaaaa;

	state.m_fog = OpenGLFogState();
}




/// \brief A container of Renderable references.
/// May contain the same Renderable multiple times, with different transforms.
class OpenGLStateBucket
{
public:
	struct RenderTransform
	{
		const Matrix4* m_transform;
		const OpenGLRenderable *m_renderable;
		const RendererLight* m_light;

		RenderTransform( const OpenGLRenderable& renderable, const Matrix4& transform, const RendererLight* light )
			: m_transform( &transform ), m_renderable( &renderable ), m_light( light ){
		}
	};

	typedef std::vector<RenderTransform> Renderables;

private:

	OpenGLState m_state;
	Renderables m_renderables;

public:
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& modelview, const RendererLight* light = 0 ){
		m_renderables.push_back( RenderTransform( renderable, modelview, light ) );
	}

	OpenGLState& state(){
		return m_state;
	}

	void render( OpenGLState& current, unsigned int globalstate, const Vector3& viewer );
};

#define LIGHT_SHADER_DEBUG 0

#if LIGHT_SHADER_DEBUG
typedef std::vector<Shader*> LightDebugShaders;
LightDebugShaders g_lightDebugShaders;
#endif

class OpenGLStateLess
{
public:
	bool operator()( const OpenGLState& self, const OpenGLState& other ) const {
		return OpenGLState_less( self, other );
	}
};

typedef ConstReference<OpenGLState> OpenGLStateReference;
typedef std::map<OpenGLStateReference, OpenGLStateBucket*, OpenGLStateLess> OpenGLStates;
OpenGLStates g_state_sorted;

class OpenGLStateBucketAdd
{
	OpenGLStateBucket& m_bucket;
	const OpenGLRenderable& m_renderable;
	const Matrix4& m_modelview;
public:
	using func = void(const RendererLight&);

	OpenGLStateBucketAdd( OpenGLStateBucket& bucket, const OpenGLRenderable& renderable, const Matrix4& modelview ) :
		m_bucket( bucket ), m_renderable( renderable ), m_modelview( modelview ){
	}
	void operator()( const RendererLight& light ){
		m_bucket.addRenderable( m_renderable, m_modelview, &light );
	}
};

class CountLights
{
	std::size_t m_count;
public:
	using func = void(RendererLight&);

	CountLights() : m_count( 0 ){
	}
	void operator()( const RendererLight& light ){
		++m_count;
	}
	std::size_t count() const {
		return m_count;
	}
};

class OpenGLShader final : public Shader
{
	typedef std::list<OpenGLStateBucket*> Passes;
	Passes m_passes;
	IShader* m_shader;
	std::size_t m_used;
	ModuleObservers m_observers;
	std::vector<qtexture_t*> m_krTextures;                 // game look: stage images
	std::vector<std::unique_ptr<KrStageGL>> m_krStagesGL;
	bool constructStages( OpenGLState& first );
public:
	OpenGLShader() : m_shader( 0 ), m_used( 0 ){
	}
	~OpenGLShader() = default;
	void construct( const char* name );
	void destroy(){
		for( qtexture_t* t : m_krTextures )
			GlobalTexturesCache().release( t );
		m_krTextures.clear();
		m_krStagesGL.clear();
		if ( m_shader ) {
			m_shader->DecRef();
		}
		m_shader = 0;

		for ( auto *bucket : m_passes )
		{
			delete bucket;
		}
		m_passes.clear();
	}
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& modelview, const LightList* lights ) override {
		for ( auto *bucket : m_passes )
		{
#if LIGHT_SHADER_DEBUG
			if ( ( bucket->state().m_state & RENDER_BUMP ) != 0 ) {
				if ( lights != 0 ) {
					CountLights counter;
					lights->forEachLight( makeCallback( counter ) );
					globalOutputStream() << "count = " << counter.count() << '\n';
					for ( std::size_t i = 0; i < counter.count(); ++i )
					{
						g_lightDebugShaders[counter.count()]->addRenderable( renderable, modelview );
					}
				}
			}
			else
#else
			if ( ( bucket->state().m_state & RENDER_BUMP ) != 0 ) {
				if ( lights != 0 ) {
					OpenGLStateBucketAdd add( *bucket, renderable, modelview );
					lights->forEachLight( makeCallback( add ) );
				}
			}
			else
#endif
			{
				bucket->addRenderable( renderable, modelview );
			}
		}
	}
	void incrementUsed() override {
		if ( ++m_used == 1 && m_shader != 0 ) {
			m_shader->SetInUse( true );
		}
	}
	void decrementUsed() override {
		if ( --m_used == 0 && m_shader != 0 ) {
			m_shader->SetInUse( false );
		}
	}
	bool realised() const {
		return m_shader != 0;
	}
	void attach( ModuleObserver& observer ) override {
		if ( realised() ) {
			observer.realise();
		}
		m_observers.attach( observer );
	}
	void detach( ModuleObserver& observer ) override {
		if ( realised() ) {
			observer.unrealise();
		}
		m_observers.detach( observer );
	}
	void realise( const CopiedString& name ){
		construct( name.c_str() );

		if ( m_used != 0 && m_shader != 0 ) {
			m_shader->SetInUse( true );
		}

		for ( auto *bucket : m_passes )
		{
			g_state_sorted.insert( OpenGLStates::value_type( OpenGLStateReference( bucket->state() ), bucket ) );
		}

		m_observers.realise();
	}
	void unrealise(){
		m_observers.unrealise();

		for ( auto *bucket : m_passes )
		{
			g_state_sorted.erase( OpenGLStateReference( bucket->state() ) );
		}

		destroy();
	}
	qtexture_t& getTexture() const override {
		ASSERT_NOTNULL( m_shader );
		return *m_shader->getTexture();
	}
	unsigned int getFlags() const override {
		return m_shader != nullptr ? m_shader->getFlags() : 0; // KenRadiant: builtin states ($KR_TERRAIN ...) have no IShader
	}
	IShader& getShader() const {
		ASSERT_NOTNULL( m_shader );
		return *m_shader;
	}
	bool hasIShader() const {
		return m_shader != nullptr;
	}
	OpenGLState& appendDefaultPass(){
		m_passes.push_back( new OpenGLStateBucket );
		OpenGLState& state = m_passes.back()->state();
		OpenGLState_constructDefault( state );
		return state;
	}
};


inline bool lightEnabled( const RendererLight& light, const LightCullable& cullable ){
	return cullable.testLight( light );
}

typedef std::set<RendererLight*> RendererLights;

#define DEBUG_LIGHT_SYNC 0

class LinearLightList : public LightList
{
	LightCullable& m_cullable;
	RendererLights& m_allLights;
	Callback<void()> m_evaluateChanged;

	typedef std::list<RendererLight*> Lights;
	mutable Lights m_lights;
	mutable bool m_lightsChanged;
public:
	LinearLightList( LightCullable& cullable, RendererLights& lights, const Callback<void()>& evaluateChanged ) :
		m_cullable( cullable ), m_allLights( lights ), m_evaluateChanged( evaluateChanged ){
		m_lightsChanged = true;
	}
	void evaluateLights() const override {
		m_evaluateChanged();
		if ( m_lightsChanged ) {
			m_lightsChanged = false;

			m_lights.clear();
			m_cullable.clearLights();
			for ( auto *light : m_allLights )
			{
				if ( lightEnabled( *light, m_cullable ) ) {
					m_lights.push_back( light );
					m_cullable.insertLight( *light );
				}
			}
		}
#if ( DEBUG_LIGHT_SYNC )
		else
		{
			Lights lights;
			for ( auto *light : m_allLights )
			{
				if ( lightEnabled( *light, m_cullable ) ) {
					lights.push_back( light );
				}
			}
			ASSERT_MESSAGE(
			    !std::lexicographical_compare( lights.begin(), lights.end(), m_lights.begin(), m_lights.end() )
			    && !std::lexicographical_compare( m_lights.begin(), m_lights.end(), lights.begin(), lights.end() ),
			    "lights out of sync"
			);
		}
#endif
	}
	void forEachLight( const RendererLightCallback& callback ) const override {
		evaluateLights();

		for ( const auto *light : m_lights )
		{
			callback( *light );
		}
	}
	void lightsChanged() const override {
		m_lightsChanged = true;
	}
};

inline void setFogState( const OpenGLFogState& state ){
	gl().glFogi( GL_FOG_MODE, state.mode );
	gl().glFogf( GL_FOG_DENSITY, state.density );
	gl().glFogf( GL_FOG_START, state.start );
	gl().glFogf( GL_FOG_END, state.end );
	gl().glFogi( GL_FOG_INDEX, state.index );
	gl().glFogfv( GL_FOG_COLOR, vector4_to_array( state.colour ) );
}

#define DEBUG_SHADERS 0
void OpenGLState_apply( const OpenGLState& self, OpenGLState& current, unsigned int globalstate );

class OpenGLShaderCache final : public ShaderCache, public TexturesCacheObserver, public ModuleObserver
{
	class CreateOpenGLShader
	{
		OpenGLShaderCache* m_cache;
	public:
		explicit CreateOpenGLShader( OpenGLShaderCache* cache = 0 )
			: m_cache( cache ){
		}
		OpenGLShader* construct( const CopiedString& name ){
			auto *shader = new OpenGLShader;
			if ( m_cache->realised() ) {
				shader->realise( name );
			}
			return shader;
		}
		void destroy( OpenGLShader* shader ){
			if ( m_cache->realised() ) {
				shader->unrealise();
			}
			delete shader;
		}
	};

	typedef HashedCache<CopiedString, OpenGLShader, HashString, std::equal_to<CopiedString>, CreateOpenGLShader> Shaders;
	Shaders m_shaders;
	std::size_t m_unrealised;

	bool m_lightingEnabled;

public:
	OpenGLShaderCache() :
		m_shaders( CreateOpenGLShader( this ) ),
		m_unrealised( 3 ), // wait until shaders, gl-context and textures are realised before creating any render-states
		m_lightingEnabled( true ),
		m_lightsChanged( true ),
		m_traverseRenderablesMutex( false ){
	}
	~OpenGLShaderCache(){
		for ( auto& shader : m_shaders )
		{
			globalOutputStream() << "leaked shader: " << Quoted( shader.key ) << '\n';
		}
	}
	Shader* capture( const char* name ) override {
		ASSERT_MESSAGE( name[0] == '$'
		                || *name == '['
		                || *name == '<'
		                || *name == '('
		                || *name == '{'
		                || strchr( name, '\\' ) == 0, "shader name contains invalid characters: " << Quoted( name ) );
#if DEBUG_SHADERS
		globalOutputStream() << "shaders capture: " << Quoted( name ) << '\n';
#endif
		return m_shaders.capture( name ).get();
	}
	void release( const char *name ) override {
#if DEBUG_SHADERS
		globalOutputStream() << "shaders release: " << Quoted( name ) << '\n';
#endif
		m_shaders.release( name );
	}
	void render( RenderStateFlags globalstate, const Matrix4& modelview, const Matrix4& projection, const Vector3& viewer ) override {
		gl().glMatrixMode( GL_PROJECTION );
		gl().glLoadMatrixf( reinterpret_cast<const float*>( &projection ) );
#if 0
		//qglGetFloatv( GL_PROJECTION_MATRIX, reinterpret_cast<float*>( &projection ) );
#endif

		gl().glMatrixMode( GL_MODELVIEW );
		gl().glLoadMatrixf( reinterpret_cast<const float*>( &modelview ) );
#if 0
		//qglGetFloatv( GL_MODELVIEW_MATRIX, reinterpret_cast<float*>( &modelview ) );
#endif

		ASSERT_MESSAGE( realised(), "render states are not realised" );

		// global settings that are not set in renderstates
		gl().glFrontFace( GL_CW );
		gl().glCullFace( GL_BACK );
		gl().glPolygonOffset( -1, 1 );
		{
			const GLubyte pattern[132] = {
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
				0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55
			};
			gl().glPolygonStipple( pattern );
		}
		gl().glEnableClientState( GL_VERTEX_ARRAY );
		g_vertexArray_enabled = true;
		gl().glColorMaterial( GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE );

		gl().glActiveTexture( GL_TEXTURE0 );
		gl().glClientActiveTexture( GL_TEXTURE0 );

		gl().glUseProgram( 0 );
		gl().glDisableVertexAttribArray( c_attr_TexCoord0 );
		gl().glDisableVertexAttribArray( c_attr_Tangent );
		gl().glDisableVertexAttribArray( c_attr_Binormal );

		if ( globalstate & RENDER_TEXTURE ) {
			gl().glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
			gl().glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
		}

		OpenGLState current;
		OpenGLState_constructDefault( current );
		current.m_sort = OpenGLState::eSortFirst;

		// default renderstate settings
		gl().glLineStipple( current.m_linestipple_factor, current.m_linestipple_pattern );
		gl().glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		gl().glDisable( GL_LIGHTING );
		gl().glDisable( GL_TEXTURE_2D );
		gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
		g_texcoordArray_enabled = false;
		gl().glDisableClientState( GL_COLOR_ARRAY );
		g_colorArray_enabled = false;
		gl().glDisableClientState( GL_NORMAL_ARRAY );
		g_normalArray_enabled = false;
		gl().glDisable( GL_BLEND );
		gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		gl().glDisable( GL_CULL_FACE );
		gl().glShadeModel( GL_FLAT );
		gl().glDisable( GL_DEPTH_TEST );
		gl().glDepthMask( GL_FALSE );
		gl().glDisable( GL_ALPHA_TEST );
		gl().glDisable( GL_LINE_STIPPLE );
		gl().glDisable( GL_POLYGON_STIPPLE );
		gl().glDisable( GL_POLYGON_OFFSET_LINE );

		gl().glBindTexture( GL_TEXTURE_2D, 0 );
		gl().glColor4f( 1, 1, 1, 1 );
		gl().glDepthFunc( GL_LESS );
		gl().glAlphaFunc( GL_ALWAYS, 0 );
		gl().glLineWidth( 1 );
		gl().glPointSize( 1 );

		gl().glHint( GL_FOG_HINT, GL_NICEST );
		gl().glDisable( GL_FOG );
		setFogState( OpenGLFogState() );

		GlobalOpenGL_debugAssertNoErrors();

		debug_string( "begin rendering" );
		for ( auto& [ state, bucket ] : g_state_sorted )
		{
			bucket->render( current, globalstate, viewer );
		}
		debug_string( "end rendering" );

		OpenGLState reset = current; /* reset some states */
		reset.m_state = current.m_state & ~RENDER_TEXT; /* popmatrix after RENDER_TEXT */
		reset.m_program = nullptr; /* disable shader */
		reset.m_krFx = nullptr; /* KenRadiant: identity texture matrix, no polygon offset again */
		OpenGLState_apply( reset, current, globalstate );
	}
	void realise() override {
		if ( --m_unrealised == 0 ) {
			if ( lightingEnabled() ) {
				g_bumpGLSL.create();
				g_depthFillGLSL.create();
			}

			g_skyboxGLSL.create();
			g_krLightGLSL.create();

			for ( auto& shader : m_shaders )
			{
				if ( !shader.value.empty() ) {
					shader.value->realise( shader.key );
				}
			}
		}
	}
	void unrealise() override {
		if ( ++m_unrealised == 1 ) {
			for ( auto& shader : m_shaders )
			{
				if ( !shader.value.empty() ) {
					shader.value->unrealise();
				}
			}
			if ( GlobalOpenGL().contextValid && lightingEnabled() ) {
				g_bumpGLSL.destroy();
				g_depthFillGLSL.destroy();
			}
			if( GlobalOpenGL().contextValid ){
				g_skyboxGLSL.destroy();
				g_krLightGLSL.destroy();
			}
		}
	}
	bool realised(){
		return m_unrealised == 0;
	}


	bool lightingEnabled() const {
		return m_lightingEnabled;
	}
	void extensionsInitialised(){
		setLightingEnabled( m_lightingEnabled );
	}
	void setLightingEnabled( bool enabled ){
		if ( m_lightingEnabled != enabled ) {
			unrealise();
			GlobalShaderSystem().setLightingEnabled( enabled );

			m_lightingEnabled = enabled;

			realise();
		}
	}

// light culling

	RendererLights m_lights;
	bool m_lightsChanged;
	typedef std::map<LightCullable*, LinearLightList> LightLists;
	LightLists m_lightLists;

	const LightList& attach( LightCullable& cullable ) override {
		return ( *m_lightLists.insert( LightLists::value_type( &cullable, LinearLightList( cullable, m_lights, EvaluateChangedCaller( *this ) ) ) ).first ).second;
	}
	void detach( LightCullable& cullable ) override {
		m_lightLists.erase( &cullable );
	}
	void changed( LightCullable& cullable ) override {
		LightLists::iterator i = m_lightLists.find( &cullable );
		ASSERT_MESSAGE( i != m_lightLists.end(), "cullable not attached" );
		( *i ).second.lightsChanged();
	}
	void attach( RendererLight& light ) override {
		const bool inserted = m_lights.insert( &light ).second;
		ASSERT_MESSAGE( inserted, "light could not be attached" );
		changed( light );
	}
	void detach( RendererLight& light ) override {
		const bool erased = m_lights.erase( &light );
		ASSERT_MESSAGE( erased, "light could not be detached" );
		changed( light );
	}
	void changed( RendererLight& light ) override {
		m_lightsChanged = true;
	}
	void evaluateChanged(){
		if ( m_lightsChanged ) {
			m_lightsChanged = false;
			for ( auto& [ cullable, lightList ] : m_lightLists )
			{
				lightList.lightsChanged();
			}
		}
	}
	typedef MemberCaller<OpenGLShaderCache, void(), &OpenGLShaderCache::evaluateChanged> EvaluateChangedCaller;

	typedef std::set<const Renderable*> Renderables;
	Renderables m_renderables;
	mutable bool m_traverseRenderablesMutex;

// renderables
	void attachRenderable( const Renderable& renderable ) override {
		ASSERT_MESSAGE( !m_traverseRenderablesMutex, "attaching renderable during traversal" );
		const bool inserted = m_renderables.insert( &renderable ).second;
		ASSERT_MESSAGE( inserted, "renderable could not be attached" );
	}
	void detachRenderable( const Renderable& renderable ) override {
		ASSERT_MESSAGE( !m_traverseRenderablesMutex, "detaching renderable during traversal" );
		const bool erased = m_renderables.erase( &renderable );
		ASSERT_MESSAGE( erased, "renderable could not be detached" );
	}
	void forEachRenderable( const RenderableCallback& callback ) const override {
		ASSERT_MESSAGE( !m_traverseRenderablesMutex, "for-each during traversal" );
		m_traverseRenderablesMutex = true;
		for ( const auto *rend : m_renderables )
		{
			callback( *rend );
		}
		m_traverseRenderablesMutex = false;
	}
};

static OpenGLShaderCache* g_ShaderCache;

void ShaderCache_extensionsInitialised(){
	g_ShaderCache->extensionsInitialised();
}

void ShaderCache_setBumpEnabled( bool enabled ){
	g_ShaderCache->setLightingEnabled( enabled );
}


Vector3 g_DebugShaderColours[256];
Shader* g_defaultPointLight = 0;

void ShaderCache_Construct(){
	g_ShaderCache = new OpenGLShaderCache;
	GlobalTexturesCache().attach( *g_ShaderCache );
	GlobalShaderSystem().attach( *g_ShaderCache );

	if ( g_pGameDescription->mGameType == "doom3" ) {
		g_defaultPointLight = g_ShaderCache->capture( "lights/defaultPointLight" );
		//Shader* overbright =
		g_ShaderCache->capture( "$OVERBRIGHT" );

#if LIGHT_SHADER_DEBUG
		for ( std::size_t i = 0; i < 256; ++i )
		{
			g_DebugShaderColours[i] = Vector3( i / 256.0, i / 256.0, i / 256.0 );
		}

		g_DebugShaderColours[0] = Vector3( 1, 0, 0 );
		g_DebugShaderColours[1] = Vector3( 1, 0.5, 0 );
		g_DebugShaderColours[2] = Vector3( 1, 1, 0 );
		g_DebugShaderColours[3] = Vector3( 0.5, 1, 0 );
		g_DebugShaderColours[4] = Vector3( 0, 1, 0 );
		g_DebugShaderColours[5] = Vector3( 0, 1, 0.5 );
		g_DebugShaderColours[6] = Vector3( 0, 1, 1 );
		g_DebugShaderColours[7] = Vector3( 0, 0.5, 1 );
		g_DebugShaderColours[8] = Vector3( 0, 0, 1 );
		g_DebugShaderColours[9] = Vector3( 0.5, 0, 1 );
		g_DebugShaderColours[10] = Vector3( 1, 0, 1 );
		g_DebugShaderColours[11] = Vector3( 1, 0, 0.5 );

		g_lightDebugShaders.reserve( 256 );
		StringOutputStream buffer( 256 );
		for ( std::size_t i = 0; i < 256; ++i )
		{
			g_lightDebugShaders.push_back( g_ShaderCache->capture( buffer( '(', g_DebugShaderColours[i].x(), ' ', g_DebugShaderColours[i].y(), ' ', g_DebugShaderColours[i].z(), ')' ) ) );
		}
#endif
	}
}

void ShaderCache_Destroy(){
	if ( g_pGameDescription->mGameType == "doom3" ) {
		g_ShaderCache->release( "lights/defaultPointLight" );
		g_ShaderCache->release( "$OVERBRIGHT" );
		g_defaultPointLight = 0;

#if LIGHT_SHADER_DEBUG
		g_lightDebugShaders.clear();
		StringOutputStream buffer( 256 );
		for ( std::size_t i = 0; i < 256; ++i )
		{
			g_ShaderCache->release( buffer( '(', g_DebugShaderColours[i].x(), ' ', g_DebugShaderColours[i].y(), ' ', g_DebugShaderColours[i].z(), ')' ) );
		}
#endif
	}

	GlobalShaderSystem().detach( *g_ShaderCache );
	GlobalTexturesCache().detach( *g_ShaderCache );
	delete g_ShaderCache;
}

ShaderCache* GetShaderCache(){
	return g_ShaderCache;
}

inline void setTextureState( GLint& current, const GLint& texture, GLenum textureUnit ){
	if ( texture != current ) {
		gl().glActiveTexture( textureUnit );
		gl().glClientActiveTexture( textureUnit );
		gl().glBindTexture( GL_TEXTURE_2D, texture );
		GlobalOpenGL_debugAssertNoErrors();
		current = texture;
	}
}

inline void setTextureState( GLint& current, const GLint& texture ){
	if ( texture != current ) {
		gl().glBindTexture( GL_TEXTURE_2D, texture );
		GlobalOpenGL_debugAssertNoErrors();
		current = texture;
	}
}

inline void setState( unsigned int state, unsigned int delta, unsigned int flag, GLenum glflag ){
	if ( delta & state & flag ) {
		gl().glEnable( glflag );
		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & flag ) {
		gl().glDisable( glflag );
		GlobalOpenGL_debugAssertNoErrors();
	}
}

void OpenGLState_apply( const OpenGLState& self, OpenGLState& current, unsigned int globalstate ){
	// KenRadiant: tcMod scroll / rotate / scale / stretch via the texture matrix of unit 0, shader polygonOffset
	{
		const KrShaderEffects* fx = ( globalstate & RENDER_TEXTURE ) ? self.m_krFx : nullptr;
		const bool matrix = fx != nullptr && g_krAnimateShaders && fx->textureMatrix();
		if( matrix || current.m_krMatrix ){
			gl().glActiveTexture( GL_TEXTURE0 );
			gl().glMatrixMode( GL_TEXTURE );
			gl().glLoadIdentity();
			if( matrix ){
				const float t = g_krShaderTime;
				if( fx->animated() )
					g_krAnimatedDrawn = true;
				// applied to the texcoord in shader order: scale, stretch, rotate, scroll
				gl().glTranslatef( std::fmod( fx->scroll[0] * t, 1.f ), std::fmod( fx->scroll[1] * t, 1.f ), 0 );
				if( fx->rotate != 0 ){ // around the texture centre, like Quake 3
					gl().glTranslatef( 0.5f, 0.5f, 0 );
					gl().glRotatef( -std::fmod( fx->rotate * t, 360.f ), 0, 0, 1 );
					gl().glTranslatef( -0.5f, -0.5f, 0 );
				}
				if( fx->stretchFunc != 0 ){
					const float w = KrWave( fx->stretchFunc, fx->stretch, t );
					const float p = std::fabs( w ) > 1e-3f ? 1.f / w : 1000.f;
					gl().glTranslatef( 0.5f, 0.5f, 0 );
					gl().glScalef( p, p, 1 );
					gl().glTranslatef( -0.5f, -0.5f, 0 );
				}
				gl().glScalef( fx->scale[0], fx->scale[1], 1 );
			}
			current.m_krMatrix = matrix;
			gl().glMatrixMode( GL_MODELVIEW );
		}
		const bool offset = ( ( fx != nullptr && g_krPolygonOffset && fx->polygonOffset ) || self.m_krOffsetFill ) && ( globalstate & RENDER_FILL );
		if( offset != current.m_krPolygonOffset ){
			if( offset ){
				gl().glEnable( GL_POLYGON_OFFSET_FILL );
				gl().glPolygonOffset( -1, -2 ); // decals in front of the wall they sit on
			}
			else{
				gl().glDisable( GL_POLYGON_OFFSET_FILL );
				gl().glPolygonOffset( -1, 1 ); // the default the line offset uses
			}
			current.m_krPolygonOffset = offset;
		}
	}

	debug_int( "sort", int( self.m_sort ) );
	debug_int( "texture", self.m_texture );
	debug_int( "state", self.m_state );
	debug_int( "address", int( std::size_t( &self ) ) );

	count_state();

	if ( self.m_state & RENDER_OVERRIDE ) {
		globalstate |= RENDER_FILL;
	}
	if ( self.m_state & RENDER_TEXT ) {
		globalstate |= RENDER_TEXTURE | RENDER_BLEND | RENDER_FILL | RENDER_TEXT;
	}

	const unsigned int state = self.m_state & globalstate;
	const unsigned int delta = state ^ current.m_state;

	GlobalOpenGL_debugAssertNoErrors();

	if ( delta & state & RENDER_TEXT ) {
		gl().glMatrixMode( GL_PROJECTION );
		gl().glPushMatrix();
		gl().glLoadIdentity();
		GLint viewprt[4];
		gl().glGetIntegerv( GL_VIEWPORT, viewprt );
		//globalOutputStream() << viewprt[2] << ' ' << viewprt[3] << '\n';
		gl().glOrtho( 0, viewprt[2], 0, viewprt[3], -100, 100 );
		gl().glTranslated( double( viewprt[2] ) / 2.0, double( viewprt[3] ) / 2.0, 0 );
		gl().glMatrixMode( GL_MODELVIEW );
		gl().glPushMatrix();
		gl().glLoadIdentity();

		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & RENDER_TEXT ) {
		gl().glMatrixMode( GL_PROJECTION );
		gl().glPopMatrix();
		gl().glMatrixMode( GL_MODELVIEW );
		gl().glPopMatrix();

		GlobalOpenGL_debugAssertNoErrors();
	}

	GLProgram* program = ( state & RENDER_PROGRAM ) != 0 ? self.m_program : 0;

	if ( program != current.m_program ) {
		if ( current.m_program != 0 ) {
			current.m_program->disable();
//why?			gl().glColor4fv( vector4_to_array( current.m_colour ) );
			debug_colour( "cleaning program" );
		}

		current.m_program = program;

		if ( current.m_program != 0 ) {
			current.m_program->enable();
		}
	}
	if( current.m_program == &g_krLightGLSL ) // KenRadiant: per-state light preview / deform / glow parameters
		g_krLightGLSL.applyState( self, globalstate );

	if ( delta & state & RENDER_FILL ) {
		//qglPolygonMode ( GL_BACK, GL_LINE );
		//qglPolygonMode ( GL_FRONT, GL_FILL );
		gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & RENDER_FILL ) {
		gl().glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		GlobalOpenGL_debugAssertNoErrors();
	}

	setState( state, delta, RENDER_OFFSETLINE, GL_POLYGON_OFFSET_LINE );

	if ( delta & state & RENDER_LIGHTING ) {
		gl().glEnable( GL_LIGHTING );
		gl().glEnable( GL_COLOR_MATERIAL );
		gl().glEnable( GL_RESCALE_NORMAL );
		gl().glEnableClientState( GL_NORMAL_ARRAY );
		GlobalOpenGL_debugAssertNoErrors();
		g_normalArray_enabled = true;
	}
	else if ( delta & ~state & RENDER_LIGHTING ) {
		gl().glDisable( GL_LIGHTING );
		gl().glDisable( GL_COLOR_MATERIAL );
		gl().glDisable( GL_RESCALE_NORMAL );
		gl().glDisableClientState( GL_NORMAL_ARRAY );
		GlobalOpenGL_debugAssertNoErrors();
		g_normalArray_enabled = false;
	}

	if ( delta & state & RENDER_TEXTURE ) {
		GlobalOpenGL_debugAssertNoErrors();

		gl().glActiveTexture( GL_TEXTURE0 );
		gl().glClientActiveTexture( GL_TEXTURE0 );

		gl().glEnable( GL_TEXTURE_2D );

		gl().glColor4f( 1, 1, 1, self.m_colour[3] );
		debug_colour( "setting texture" );

		gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
		GlobalOpenGL_debugAssertNoErrors();
		g_texcoordArray_enabled = true;
	}
	else if ( delta & ~state & RENDER_TEXTURE ) {
		gl().glActiveTexture( GL_TEXTURE0 );
		gl().glClientActiveTexture( GL_TEXTURE0 );

		gl().glDisable( GL_TEXTURE_2D );
		gl().glBindTexture( GL_TEXTURE_2D, 0 );
		gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );

		GlobalOpenGL_debugAssertNoErrors();
		g_texcoordArray_enabled = false;
	}

	if ( delta & state & RENDER_BLEND ) {
// FIXME: some .TGA are buggy, have a completely empty alpha channel
// if such brushes are rendered in this loop they would be totally transparent with GL_MODULATE
// so I decided using GL_DECAL instead
// if an empty-alpha-channel or nearly-empty texture is used. It will be blank-transparent.
// this could get better if you can get glTexEnviv (GL_TEXTURE_ENV, to work .. patches are welcome

		gl().glEnable( GL_BLEND );
		gl().glActiveTexture( GL_TEXTURE0 );
//		gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL );
//		gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE ); //uses actual alpha channel, = invis, if qer_trans + empty alpha channel
		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & RENDER_BLEND ) {
		gl().glDisable( GL_BLEND );
		gl().glActiveTexture( GL_TEXTURE0 );
//		gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		GlobalOpenGL_debugAssertNoErrors();
	}

	setState( state, delta, RENDER_CULLFACE, GL_CULL_FACE );

	if ( delta & state & RENDER_SMOOTH ) {
		gl().glShadeModel( GL_SMOOTH );
		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & RENDER_SMOOTH ) {
		gl().glShadeModel( GL_FLAT );
		GlobalOpenGL_debugAssertNoErrors();
	}

	setState( state, delta, RENDER_SCALED, GL_NORMALIZE ); // not GL_RESCALE_NORMAL

	setState( state, delta, RENDER_DEPTHTEST, GL_DEPTH_TEST );

	if ( delta & state & RENDER_DEPTHWRITE ) {
		gl().glDepthMask( GL_TRUE );

#if DEBUG_RENDER
		GLboolean depthEnabled;
		gl().glGetBooleanv( GL_DEPTH_WRITEMASK, &depthEnabled );
		ASSERT_MESSAGE( depthEnabled, "failed to set depth buffer mask bit" );
#endif
		debug_string( "enabled depth-buffer writing" );

		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & RENDER_DEPTHWRITE ) {
		gl().glDepthMask( GL_FALSE );

#if DEBUG_RENDER
		GLboolean depthEnabled;
		gl().glGetBooleanv( GL_DEPTH_WRITEMASK, &depthEnabled );
		ASSERT_MESSAGE( !depthEnabled, "failed to set depth buffer mask bit" );
#endif
		debug_string( "disabled depth-buffer writing" );

		GlobalOpenGL_debugAssertNoErrors();
	}

	if ( delta & state & RENDER_COLOURWRITE ) {
		gl().glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & RENDER_COLOURWRITE ) {
		gl().glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
		GlobalOpenGL_debugAssertNoErrors();
	}

	setState( state, delta, RENDER_ALPHATEST, GL_ALPHA_TEST );

	if ( delta & state & RENDER_COLOURARRAY ) {
		gl().glEnableClientState( GL_COLOR_ARRAY );
		GlobalOpenGL_debugAssertNoErrors();
		debug_colour( "enabling color_array" );
		g_colorArray_enabled = true;
	}
	else if ( delta & ~state & RENDER_COLOURARRAY ) {
		gl().glDisableClientState( GL_COLOR_ARRAY );
		gl().glColor4fv( vector4_to_array( self.m_colour ) );
		debug_colour( "cleaning color_array" );
		GlobalOpenGL_debugAssertNoErrors();
		g_colorArray_enabled = false;
	}

	if ( delta & ~state & RENDER_COLOURCHANGE ) {
		gl().glColor4fv( vector4_to_array( self.m_colour ) );
		GlobalOpenGL_debugAssertNoErrors();
	}

	setState( state, delta, RENDER_LINESTIPPLE, GL_LINE_STIPPLE );

	setState( state, delta, RENDER_POLYGONSTIPPLE, GL_POLYGON_STIPPLE );

	setState( state, delta, RENDER_FOG, GL_FOG );

	if ( ( state & RENDER_FOG ) != 0 ) {
		setFogState( self.m_fog );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_fog = self.m_fog;
	}

	if ( state & RENDER_DEPTHTEST && self.m_depthfunc != current.m_depthfunc ) {
		gl().glDepthFunc( self.m_depthfunc );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_depthfunc = self.m_depthfunc;
	}

	if ( state & RENDER_LINESTIPPLE
	     && ( self.m_linestipple_factor != current.m_linestipple_factor
	          || self.m_linestipple_pattern != current.m_linestipple_pattern ) ) {
		gl().glLineStipple( self.m_linestipple_factor, self.m_linestipple_pattern );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_linestipple_factor = self.m_linestipple_factor;
		current.m_linestipple_pattern = self.m_linestipple_pattern;
	}


	if ( state & RENDER_ALPHATEST
	     && ( self.m_alphafunc != current.m_alphafunc
	          || self.m_alpharef != current.m_alpharef ) ) {
		gl().glAlphaFunc( self.m_alphafunc, self.m_alpharef );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_alphafunc = self.m_alphafunc;
		current.m_alpharef = self.m_alpharef;
	}

	{
		GLint texture0 = 0;
		GLint texture1 = 0;
		GLint texture2 = 0;
		GLint texture3 = 0;
		GLint texture4 = 0;
		GLint texture5 = 0;
		GLint texture6 = 0;
		GLint texture7 = 0;
		//if( state & RENDER_TEXTURE ) != 0)
		{
			texture0 = self.m_texture;
			texture1 = self.m_texture1;
			texture2 = self.m_texture2;
			texture3 = self.m_texture3;
			texture4 = self.m_texture4;
			texture5 = self.m_texture5;
			texture6 = self.m_texture6;
			texture7 = self.m_texture7;
		}

		{
			setTextureState( current.m_texture, texture0, GL_TEXTURE0 );
			setTextureState( current.m_texture1, texture1, GL_TEXTURE1 );
			setTextureState( current.m_texture2, texture2, GL_TEXTURE2 );
			setTextureState( current.m_texture3, texture3, GL_TEXTURE3 );
			setTextureState( current.m_texture4, texture4, GL_TEXTURE4 );
			setTextureState( current.m_texture5, texture5, GL_TEXTURE5 );
			setTextureState( current.m_texture6, texture6, GL_TEXTURE6 );
			setTextureState( current.m_texture7, texture7, GL_TEXTURE7 );
		}
	}


	if( current.m_textureSkyBox != self.m_textureSkyBox ){
		gl().glActiveTexture( GL_TEXTURE0 );
		gl().glClientActiveTexture( GL_TEXTURE0 );
		gl().glBindTexture( GL_TEXTURE_CUBE_MAP, self.m_textureSkyBox );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_textureSkyBox = self.m_textureSkyBox;
	}

	if ( state & RENDER_TEXTURE && self.m_colour[3] != current.m_colour[3] ) {
		debug_colour( "setting alpha" );
		gl().glColor4f( 1, 1, 1, self.m_colour[3] );
		GlobalOpenGL_debugAssertNoErrors();
	}

	if ( !( state & RENDER_TEXTURE )
	     && self.m_colour != current.m_colour ) {
		gl().glColor4fv( vector4_to_array( self.m_colour ) );
		debug_colour( "setting non-texture" );
		GlobalOpenGL_debugAssertNoErrors();
	}
	current.m_colour = self.m_colour;

	if ( state & RENDER_BLEND
	     && ( self.m_blend_src != current.m_blend_src || self.m_blend_dst != current.m_blend_dst ) ) {
		gl().glBlendFunc( self.m_blend_src, self.m_blend_dst );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_blend_src = self.m_blend_src;
		current.m_blend_dst = self.m_blend_dst;
	}

	if ( !( state & RENDER_FILL )
	     && self.m_linewidth != current.m_linewidth ) {
		gl().glLineWidth( self.m_linewidth );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_linewidth = self.m_linewidth;
	}

	if ( !( state & RENDER_FILL )
	     && self.m_pointsize != current.m_pointsize ) {
		gl().glPointSize( self.m_pointsize );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_pointsize = self.m_pointsize;
	}

	current.m_state = state;
	if( g_krProbeDebug && current.m_program == &g_krLightGLSL )
		gl().glDisable( GL_BLEND ); // Lighting Probe frame: values, not blended colours

	GlobalOpenGL_debugAssertNoErrors();
}

void Renderables_flush( OpenGLStateBucket::Renderables& renderables, OpenGLState& current, unsigned int globalstate, const Vector3& viewer ){
	const Matrix4* transform = 0;
	gl().glPushMatrix();

	if ( current.m_program != 0 && current.m_textureSkyBox != 0 && globalstate & RENDER_PROGRAM ) {
		current.m_program->setParameters( viewer, g_matrix4_identity, g_vector3_identity, g_vector3_identity, g_matrix4_identity );
	}

	for ( const auto& rend : renderables )
	{
		//qglLoadMatrixf( i->m_transform );
		if ( !transform || ( transform != rend.m_transform && !matrix4_affine_equal( *transform, *rend.m_transform ) ) ) {
			count_transform();
			transform = rend.m_transform;
			gl().glPopMatrix();
			gl().glPushMatrix();
			gl().glMultMatrixf( reinterpret_cast<const float*>( transform ) );
			gl().glFrontFace( ( ( current.m_state & RENDER_CULLFACE ) != 0 && matrix4_handedness( *transform ) == MATRIX4_RIGHTHANDED ) ? GL_CW : GL_CCW );
		}

		count_prim();

		if ( current.m_program != 0 && rend.m_light != 0 ) {
			const IShader& lightShader = static_cast<OpenGLShader*>( rend.m_light->getShader() )->getShader();
			if ( lightShader.firstLayer() != 0 ) {
				GLuint attenuation_xy = lightShader.firstLayer()->texture()->texture_number;
				GLuint attenuation_z = lightShader.lightFalloffImage() != 0
				                       ? lightShader.lightFalloffImage()->texture_number
				                       : static_cast<OpenGLShader*>( g_defaultPointLight )->getShader().lightFalloffImage()->texture_number;

				setTextureState( current.m_texture3, attenuation_xy, GL_TEXTURE3 );
				gl().glActiveTexture( GL_TEXTURE3 );
				gl().glBindTexture( GL_TEXTURE_2D, attenuation_xy );
				gl().glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
				gl().glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );

				setTextureState( current.m_texture4, attenuation_z, GL_TEXTURE4 );
				gl().glActiveTexture( GL_TEXTURE4 );
				gl().glBindTexture( GL_TEXTURE_2D, attenuation_z );
				gl().glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
				gl().glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );


				AABB lightBounds( rend.m_light->aabb() );

				Matrix4 world2light( g_matrix4_identity );

				if ( rend.m_light->isProjected() ) {
					world2light = rend.m_light->projection();
					matrix4_multiply_by_matrix4( world2light, matrix4_transposed( rend.m_light->rotation() ) );
					matrix4_translate_by_vec3( world2light, vector3_negated( lightBounds.origin ) ); // world->lightBounds
				}
				if ( !rend.m_light->isProjected() ) {
					matrix4_translate_by_vec3( world2light, Vector3( 0.5f, 0.5f, 0.5f ) );
					matrix4_scale_by_vec3( world2light, Vector3( 0.5f, 0.5f, 0.5f ) );
					matrix4_scale_by_vec3( world2light, Vector3( 1.0f / lightBounds.extents.x(), 1.0f / lightBounds.extents.y(), 1.0f / lightBounds.extents.z() ) );
					matrix4_multiply_by_matrix4( world2light, matrix4_transposed( rend.m_light->rotation() ) );
					matrix4_translate_by_vec3( world2light, vector3_negated( lightBounds.origin ) ); // world->lightBounds
				}

				current.m_program->setParameters( viewer, *rend.m_transform, lightBounds.origin + rend.m_light->offset(), rend.m_light->colour(), world2light );
				debug_string( "set lightBounds parameters" );
			}
		}

		rend.m_renderable->render( current.m_state );
	}
	gl().glPopMatrix();
	renderables.clear();
}

void OpenGLStateBucket::render( OpenGLState& current, unsigned int globalstate, const Vector3& viewer ){
	if ( ( globalstate & m_state.m_state & RENDER_SCREEN ) != 0 ) {
		OpenGLState_apply( m_state, current, globalstate );
		debug_colour( "screen fill" );

		gl().glMatrixMode( GL_PROJECTION );
		gl().glPushMatrix();
		gl().glLoadMatrixf( reinterpret_cast<const float*>( &g_matrix4_identity ) );

		gl().glMatrixMode( GL_MODELVIEW );
		gl().glPushMatrix();
		gl().glLoadMatrixf( reinterpret_cast<const float*>( &g_matrix4_identity ) );

		gl().glBegin( GL_QUADS );
		gl().glVertex3f( -1, -1, 0 );
		gl().glVertex3f( 1, -1, 0 );
		gl().glVertex3f( 1, 1, 0 );
		gl().glVertex3f( -1, 1, 0 );
		gl().glEnd();

		gl().glMatrixMode( GL_PROJECTION );
		gl().glPopMatrix();

		gl().glMatrixMode( GL_MODELVIEW );
		gl().glPopMatrix();
	}
	else if ( !m_renderables.empty() ) {
		OpenGLState_apply( m_state, current, globalstate );
		Renderables_flush( m_renderables, current, globalstate, viewer );
	}
}


class OpenGLStateMap : public OpenGLStateLibrary
{
	typedef std::map<CopiedString, OpenGLState> States;
	States m_states;
public:
	~OpenGLStateMap(){
		ASSERT_MESSAGE( m_states.empty(), "OpenGLStateMap::~OpenGLStateMap: not empty" );
	}

	typedef States::iterator iterator;
	iterator begin(){
		return m_states.begin();
	}
	iterator end(){
		return m_states.end();
	}

	void getDefaultState( OpenGLState& state ) const override {
		OpenGLState_constructDefault( state );
	}

	void insert( const char* name, const OpenGLState& state ) override {
		bool inserted = m_states.insert( States::value_type( name, state ) ).second;
		ASSERT_MESSAGE( inserted, "OpenGLStateMap::insert: " << name << " already exists" );
	}
	void erase( const char* name ) override {
		std::size_t count = m_states.erase( name );
		ASSERT_MESSAGE( count == 1, "OpenGLStateMap::erase: " << name << " does not exist" );
	}

	iterator find( const char* name ){
		return m_states.find( name );
	}
};

OpenGLStateMap* g_openglStates = 0;

inline GLenum convertBlendFactor( BlendFactor factor ){
	switch ( factor )
	{
	case BLEND_ZERO:
		return GL_ZERO;
	case BLEND_ONE:
		return GL_ONE;
	case BLEND_SRC_COLOUR:
		return GL_SRC_COLOR;
	case BLEND_ONE_MINUS_SRC_COLOUR:
		return GL_ONE_MINUS_SRC_COLOR;
	case BLEND_SRC_ALPHA:
		return GL_SRC_ALPHA;
	case BLEND_ONE_MINUS_SRC_ALPHA:
		return GL_ONE_MINUS_SRC_ALPHA;
	case BLEND_DST_COLOUR:
		return GL_DST_COLOR;
	case BLEND_ONE_MINUS_DST_COLOUR:
		return GL_ONE_MINUS_DST_COLOR;
	case BLEND_DST_ALPHA:
		return GL_DST_ALPHA;
	case BLEND_ONE_MINUS_DST_ALPHA:
		return GL_ONE_MINUS_DST_ALPHA;
	case BLEND_SRC_ALPHA_SATURATE:
		return GL_SRC_ALPHA_SATURATE;
	}
	return GL_ZERO;
}

/// \todo Define special-case shaders in a data file.
void OpenGLShader::construct( const char* name ){
	OpenGLState& state = appendDefaultPass();
	switch ( name[0] )
	{
	case '{':	//add
		sscanf( name, "{%g %g %g}", &state.m_colour[0], &state.m_colour[1], &state.m_colour[2] );
		state.m_colour[3] = 1;
		state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_BLEND | RENDER_FILL | RENDER_COLOURWRITE /*| RENDER_DEPTHWRITE */| RENDER_LIGHTING;
		state.m_blend_src = GL_ONE;
		state.m_blend_dst = GL_ONE;
//		state.m_blend_src = GL_DST_COLOR;
//		state.m_blend_dst = GL_SRC_COLOR;
//		state.m_blend_src = GL_DST_COLOR;
//		state.m_blend_dst = GL_ONE;
		state.m_sort = OpenGLState::eSortTranslucent;
		break;

	case '(':	//fill
		sscanf( name, "(%g %g %g)", &state.m_colour[0], &state.m_colour[1], &state.m_colour[2] );
		state.m_colour[3] = 1;
		state.m_state = RENDER_FILL | RENDER_LIGHTING | RENDER_DEPTHTEST | RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
		state.m_sort = OpenGLState::eSortFullbright;
		break;

	case '[':	//blend
		sscanf( name, "[%g %g %g]", &state.m_colour[0], &state.m_colour[1], &state.m_colour[2] );
		state.m_colour[3] = 0.5f;
		state.m_state = RENDER_FILL | RENDER_LIGHTING | RENDER_DEPTHTEST | RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_BLEND;
		state.m_sort = OpenGLState::eSortTranslucent;
		break;

	case '<':	//wire
		sscanf( name, "<%g %g %g>", &state.m_colour[0], &state.m_colour[1], &state.m_colour[2] );
		state.m_colour[3] = 1;
		state.m_state = RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
		state.m_sort = OpenGLState::eSortFullbright;
		state.m_depthfunc = GL_LESS;
		state.m_linewidth = 1;
		state.m_pointsize = 1;
		break;

	case '$':
		{
			OpenGLStateMap::iterator i = g_openglStates->find( name );
			if ( i != g_openglStates->end() ) {
				state = ( *i ).second;
				break;
			}
		}
		if ( string_equal( name + 1, "TEXT" ) ) {
			state.m_state = RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_FILL | RENDER_TEXTURE | RENDER_BLEND | RENDER_TEXT;
			state.m_sort = OpenGLState::eSortText;
		}
		else if ( string_equal( name + 1, "POINT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortControlFirst;
			state.m_pointsize = 6;
		}
		else if ( string_equal( name + 1, "DEEPPOINT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortControlFirst;
			state.m_pointsize = 6;

			OpenGLState& hiddenLine = appendDefaultPass(); // glBeginQuery glEndQuery
			hiddenLine.m_state = RENDER_DEPTHTEST;
			hiddenLine.m_sort = OpenGLState::eSortControlFirst - 1;
			hiddenLine.m_pointsize = 6;
			hiddenLine.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal( name + 1, "SELPOINT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortControlFirst + 1;
			state.m_pointsize = 4;
		}
		else if ( string_equal( name + 1, "BIGPOINT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortGUI1 + 1;
			state.m_pointsize = 6;
		}
		else if ( string_equal( name + 1, "PIVOT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHTEST | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_linewidth = 2;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHTEST | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortGUI0;
			hiddenLine.m_linewidth = 2;
			hiddenLine.m_depthfunc = GL_GREATER;
		}
		else if ( string_equal( name + 1, "BLENDLINE" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_BLEND;
			state.m_sort = OpenGLState::eSortGUI0 - 1;
			state.m_linewidth = 1;
		}
		else if ( string_equal( name + 1, "LATTICE" ) ) {
			state.m_colour = Vector4( 1, 0.5, 0, 1 );
			state.m_state = RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortControlFirst;
		}
		else if ( string_equal( name + 1, "WIREFRAME" ) ) {
			state.m_state = RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortFullbright;
		}
		else if ( string_equal( name + 1, "CAM_HIGHLIGHT" ) ) {
			state.m_colour = Vector4( g_camwindow_globals.color_selbrushes3d, 0.3f );
			state.m_krOffsetFill = true; // KenRadiant: pulled towards the camera, never z-fights with the face under it
			state.m_state = RENDER_FILL | RENDER_DEPTHTEST | RENDER_CULLFACE | RENDER_BLEND | RENDER_COLOURWRITE/* | RENDER_DEPTHWRITE*/;
			state.m_sort = OpenGLState::eSortHighlight;
			state.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal_n( name + 1, "KR_TERRAIN ", 11 ) ) { // KenRadiant: index-map terrain, layers blended by the program
			state.m_krTerrain = atoi( name + 12 );
			state.m_state = RENDER_FILL | RENDER_TEXTURE | RENDER_DEPTHTEST | RENDER_DEPTHWRITE | RENDER_COLOURWRITE | RENDER_LIGHTING | RENDER_SMOOTH | RENDER_CULLFACE | RENDER_PROGRAM;
			state.m_program = &g_krLightGLSL;
			state.m_colour = Vector4( 1, 1, 1, 1 );
			state.m_sort = OpenGLState::eSortFullbright;
			if( const KrTerrainGL* t = KrTerrain_gl( state.m_krTerrain ) )
				state.m_texture = t->layerTex[0];
		}
		else if ( string_equal( name + 1, "KR_MASK" ) ) { // KenRadiant: selection silhouette mask for the outline
			state.m_colour = Vector4( 1, 1, 1, 1 );
			state.m_state = RENDER_FILL | RENDER_COLOURWRITE;
			state.m_sort = OpenGLState::eSortFullbright;
		}
		else if ( string_equal( name + 1, "CAM_OVERLAY" ) ) {
#if 0
			state.m_state = RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortOverlayFirst;
#else
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_OFFSETLINE;
			state.m_sort = OpenGLState::eSortOverlayFirst + 1;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_colour = Vector4( 0.75, 0.75, 0.75, 1 );
			hiddenLine.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_OFFSETLINE | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortOverlayFirst;
			hiddenLine.m_depthfunc = GL_GREATER;
			hiddenLine.m_linestipple_factor = 2;
#endif
		}
		else if ( string_equal( name + 1, "CAM_WIRE" ) ) {
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE;// | RENDER_OFFSETLINE;
			state.m_colour = Vector4( 0.75, 0.75, 0.75, 1 );
			state.m_linewidth = 0.5;
			state.m_sort = OpenGLState::eSortOverlayFirst + 1;
			state.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal( name + 1, "CAM_FACEWIRE" ) ) {
			state.m_colour = Vector4( g_camwindow_globals.color_selbrushes3d, 1 );
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_OFFSETLINE;
			state.m_sort = OpenGLState::eSortOverlayFirst + 2;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_colour = Vector4( g_camwindow_globals.color_selbrushes3d, 1 );
			hiddenLine.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_OFFSETLINE | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortOverlayFirst + 1;
			hiddenLine.m_depthfunc = GL_GREATER;
			hiddenLine.m_linestipple_factor = 2;
		}
		else if ( string_equal( name + 1, "CAM_WORKZONE" ) ) {
			state.m_state = RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_BLEND | RENDER_COLOURARRAY | RENDER_OFFSETLINE | RENDER_SMOOTH;
			state.m_sort = OpenGLState::eSortOverlayFirst + 3;
			state.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal( name + 1, "XY_OVERLAY" ) ) {
			state.m_colour = Vector4( g_xywindow_globals.color_selbrushes, 1 );
			state.m_state = RENDER_COLOURWRITE | RENDER_LINESTIPPLE;
			state.m_sort = OpenGLState::eSortOverlayFirst;
			state.m_linewidth = 2;
			state.m_linestipple_factor = 3;
		}
		else if ( string_equal( name + 1, "DEBUG_CLIPPED" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortLast;
		}
		else if ( string_equal( name + 1, "POINTFILE" ) ) {
			state.m_colour = Vector4( 1, 0, 0, 1 );
			state.m_state = RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortFullbright;
			state.m_linewidth = 4;
		}
#if 0
		else if ( string_equal( name + 1, "LIGHT_SPHERE" ) ) {
			state.m_colour = Vector4( .15f * .95f, .15f * .95f, .15f * .95f, 1 );
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_BLEND | RENDER_FILL | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_blend_src = GL_ONE;
			state.m_blend_dst = GL_ONE;
			state.m_sort = OpenGLState::eSortTranslucent;
		}
		else if ( string_equal( name + 1, "Q3MAP2_LIGHT_SPHERE" ) ) {
			state.m_colour = Vector4( .05f, .05f, .05f, 1 );
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_BLEND | RENDER_FILL;
			state.m_blend_src = GL_ONE;
			state.m_blend_dst = GL_ONE;
			state.m_sort = OpenGLState::eSortTranslucent;
		}
#endif // 0
		else if ( string_equal( name + 1, "PLANE_WIRE_OVERLAY" ) ) {
			state.m_colour = Vector4( 1, 1, 0, 1 );
			state.m_state = RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_OFFSETLINE;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_depthfunc = GL_LEQUAL;
			state.m_linewidth = 2;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_colour = state.m_colour;
			hiddenLine.m_state = RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortGUI0;
			hiddenLine.m_depthfunc = GL_GREATER;
			hiddenLine.m_linestipple_factor = 2;
		}
		else if ( string_equal( name + 1, "WIRE_OVERLAY" ) ) {
#if 0
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST;
			state.m_sort = OpenGLState::eSortOverlayFirst;
#else
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortGUI0;
			hiddenLine.m_depthfunc = GL_GREATER;
#endif
		}
		else if ( string_equal( name + 1, "FLATSHADE_OVERLAY" ) ) {
			state.m_state = RENDER_CULLFACE | RENDER_LIGHTING | RENDER_SMOOTH | RENDER_SCALED | RENDER_COLOURARRAY | RENDER_FILL | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_OVERRIDE;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_state = RENDER_CULLFACE | RENDER_LIGHTING | RENDER_SMOOTH | RENDER_SCALED | RENDER_COLOURARRAY | RENDER_FILL | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_OVERRIDE | RENDER_POLYGONSTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortGUI0;
			hiddenLine.m_depthfunc = GL_GREATER;
		}
		else if ( string_equal( name + 1, "CLIPPER_OVERLAY" ) ) {
			state.m_colour = Vector4( g_xywindow_globals.color_clipper, 1 );
			state.m_state = RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_FILL | RENDER_POLYGONSTIPPLE;
			state.m_sort = OpenGLState::eSortOverlayFirst;
		}
		else if ( string_equal( name + 1, "OVERBRIGHT" ) ) {
			const float lightScale = 2;
			state.m_colour = Vector4( Vector3( lightScale * 0.5f ), 0.5 );
			state.m_state = RENDER_FILL | RENDER_BLEND | RENDER_COLOURWRITE | RENDER_SCREEN;
			state.m_sort = OpenGLState::eSortOverbrighten;
			state.m_blend_src = GL_DST_COLOR;
			state.m_blend_dst = GL_SRC_COLOR;
		}
		else
		{
			// default to something recognisable.. =)
			ERROR_MESSAGE( "hardcoded renderstate not found" );
			state.m_colour = Vector4( 1, 0, 1, 1 );
			state.m_state = RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortFirst;
		}
		break;
	default:
		// construction from IShader
		m_shader = QERApp_Shader_ForName( name );

		if ( g_ShaderCache->lightingEnabled() && m_shader->getBump() != 0 && m_shader->getBump()->texture_number != 0 ) { // is a bump shader
			state.m_state = RENDER_FILL | RENDER_CULLFACE | RENDER_TEXTURE | RENDER_DEPTHTEST | RENDER_DEPTHWRITE | RENDER_COLOURWRITE | RENDER_PROGRAM;
			state.m_colour = Vector4( 0, 0, 0, 1 );
			state.m_sort = OpenGLState::eSortOpaque;

			state.m_program = &g_depthFillGLSL;

			OpenGLState& bumpPass = appendDefaultPass();
			bumpPass.m_texture = m_shader->getDiffuse()->texture_number;
			bumpPass.m_texture1 = m_shader->getBump()->texture_number;
			bumpPass.m_texture2 = m_shader->getSpecular()->texture_number;

			bumpPass.m_state = RENDER_BLEND | RENDER_FILL | RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_SMOOTH | RENDER_BUMP | RENDER_PROGRAM | RENDER_LIGHTING;

			bumpPass.m_program = &g_bumpGLSL;

			bumpPass.m_depthfunc = GL_LEQUAL;
			bumpPass.m_sort = OpenGLState::eSortMultiFirst;
			bumpPass.m_blend_src = GL_ONE;
			bumpPass.m_blend_dst = GL_ONE;
		}
		else if( g_skyboxPreview && m_shader->getSkyBox() != nullptr && m_shader->getSkyBox()->texture_number != 0 )
		{
			state.m_texture = m_shader->getTexture()->texture_number;
			state.m_textureSkyBox = m_shader->getSkyBox()->texture_number;

			state.m_state = RENDER_FILL | RENDER_CULLFACE | RENDER_TEXTURE | RENDER_DEPTHTEST | RENDER_DEPTHWRITE | RENDER_COLOURWRITE | RENDER_PROGRAM;
			state.m_colour = Vector4( m_shader->getTexture()->color, 1 );
			state.m_sort = OpenGLState::eSortFullbright;

			state.m_program = &g_skyboxGLSL;
		}
		else if( g_krGameShaders && constructStages( state ) ){
			// game look: one pass per shader stage
		}
		else
		{
			state.m_texture = m_shader->getTexture()->texture_number;

			state.m_state = RENDER_FILL | RENDER_TEXTURE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_LIGHTING | RENDER_SMOOTH;
			// KenRadiant live light preview / deformVertexes / tcMod turb: the program only runs when the preview is on
			// in the 3D view, or for surfaces with vertex effects. Glows, q3map_surfacelight and additive surfaces
			// draw fullbright in the preview (they make light instead of receiving it)
			const BlendFunc blend = m_shader->getBlendFunc();
			const bool additive = ( m_shader->getFlags() & QER_TRANS ) && blend.m_dst == BLEND_ONE;
			if( !( m_shader->getFlags() & QER_SKY ) ){
				state.m_state |= RENDER_PROGRAM;
				state.m_program = &g_krLightGLSL;
				state.m_krEmissive = m_shader->isEmissive() || additive;
			}
			state.m_krFx = m_shader->getEffects();
			if ( ( m_shader->getFlags() & QER_CULL ) != 0 ) {
				if ( m_shader->getCull() == IShader::eCullBack ) {
					state.m_state |= RENDER_CULLFACE;
				}
			}
			else
			{
				state.m_state |= RENDER_CULLFACE;
			}
			if ( ( m_shader->getFlags() & QER_ALPHATEST ) != 0 ) {
				state.m_state |= RENDER_ALPHATEST;
				IShader::EAlphaFunc alphafunc;
				m_shader->getAlphaFunc( &alphafunc, &state.m_alpharef );
				switch ( alphafunc )
				{
				case IShader::eAlways:
					state.m_alphafunc = GL_ALWAYS;
					break;
				case IShader::eEqual:
					state.m_alphafunc = GL_EQUAL;
					break;
				case IShader::eLess:
					state.m_alphafunc = GL_LESS;
					break;
				case IShader::eGreater:
					state.m_alphafunc = GL_GREATER;
					break;
				case IShader::eLEqual:
					state.m_alphafunc = GL_LEQUAL;
					break;
				case IShader::eGEqual:
					state.m_alphafunc = GL_GEQUAL;
					break;
				}
			}
			state.m_colour = Vector4( m_shader->getTexture()->color, 1 );

			if ( ( m_shader->getFlags() & QER_TRANS ) != 0 ) {
				state.m_state |= RENDER_BLEND;
				state.m_colour[3] = m_shader->getTrans();
				state.m_sort = OpenGLState::eSortTranslucent;
				BlendFunc blendFunc = m_shader->getBlendFunc();
				state.m_blend_src = convertBlendFactor( blendFunc.m_src );
				state.m_blend_dst = convertBlendFactor( blendFunc.m_dst );
				state.m_depthfunc = GL_LEQUAL;
				if( state.m_krFx != nullptr && state.m_krFx->depthWrite )
					state.m_state |= RENDER_DEPTHWRITE; // KenRadiant: like the game, no see-through of its own back side
//				if ( state.m_blend_src == GL_SRC_ALPHA || state.m_blend_dst == GL_SRC_ALPHA ) {
//					state.m_state |= RENDER_DEPTHWRITE;
//				}
			}
			else
			{
				state.m_state |= RENDER_DEPTHWRITE;
				state.m_sort = OpenGLState::eSortFullbright;
			}
		}
	}
}


#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"

class OpenGLStateLibraryAPI
{
	OpenGLStateMap m_stateMap;
public:
	typedef OpenGLStateLibrary Type;
	STRING_CONSTANT( Name, "*" );

	OpenGLStateLibraryAPI(){
		g_openglStates = &m_stateMap;
	}
	~OpenGLStateLibraryAPI(){
		g_openglStates = 0;
	}
	OpenGLStateLibrary* getTable(){
		return &m_stateMap;
	}
};

typedef SingletonModule<OpenGLStateLibraryAPI> OpenGLStateLibraryModule;
typedef Static<OpenGLStateLibraryModule> StaticOpenGLStateLibraryModule;
StaticRegisterModule staticRegisterOpenGLStateLibrary( StaticOpenGLStateLibraryModule::instance() );

class ShaderCacheDependencies : public GlobalShadersModuleRef, public GlobalTexturesModuleRef, public GlobalOpenGLStateLibraryModuleRef
{
public:
	ShaderCacheDependencies() :
		GlobalShadersModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "shaders" ) ){
	}
};

class ShaderCacheAPI
{
	ShaderCache* m_shaderCache;
public:
	typedef ShaderCache Type;
	STRING_CONSTANT( Name, "*" );

	ShaderCacheAPI(){
		ShaderCache_Construct();

		m_shaderCache = GetShaderCache();
	}
	~ShaderCacheAPI(){
		ShaderCache_Destroy();
	}
	ShaderCache* getTable(){
		return m_shaderCache;
	}
};

typedef SingletonModule<ShaderCacheAPI, ShaderCacheDependencies> ShaderCacheModule;
typedef Static<ShaderCacheModule> StaticShaderCacheModule;
StaticRegisterModule staticRegisterShaderCache( StaticShaderCacheModule::instance() );

/// KenRadiant: which surfaces go into the sun shadow map
bool Shader_castsShadow( Shader* shader ){
	auto* glshader = static_cast<OpenGLShader*>( shader );
	if( glshader == nullptr || !glshader->hasIShader() )
		return false; // editor helpers: entity boxes, wires, selection overlays
	const int flags = glshader->getShader().getFlags();
	if( const KrShaderEffects* fx = glshader->getShader().getEffects(); fx != nullptr && fx->deform != 0 )
		return false; // moving surfaces (water waves) would shadow themselves
	if( flags & QER_KR_TERRAIN )
		return true; // q3map_terrain faces: the terrain (its tool shader is translucent in the editor only)
	if( flags & QER_KR_LIGHTPASS )
		return false; // tools q3map2 lets light through: clip, hint, skip, ladder, water ...
	return !( flags & ( QER_SKY | QER_TRANS | QER_FOG ) ) || ( flags & QER_ALPHATEST ); // masked foliage casts cut-out shadows
}



inline Vector4 KrStage_colour( const KrStage& st, float t ){
	Vector4 c( 1, 1, 1, 1 );
	if( st.rgbGen == 2 ){
		const float v = std::clamp( KrWave( st.rgbFunc, st.rgbWave, t ), 0.f, 1.f );
		c = Vector4( v, v, v, 1 );
	}
	else if( st.rgbGen == 5 ) // identityLighting = 1 / 2^r_overBrightBits: 1 as SOF2 is played (r_overBrightBits 0)
		c = Vector4( 1, 1, 1, 1 );
	else if( st.rgbGen == 3 )
		c = Vector4( st.rgbConst[0], st.rgbConst[1], st.rgbConst[2], 1 );
	if( st.alphaGen == 3 )
		c[3] = 0.5f; // vertex alpha: the editor has none (the compiled view shows the real one)
	else if( st.alphaGen == 1 )
		c[3] = st.alphaConst;
	else if( st.alphaGen == 2 )
		c[3] = std::clamp( KrWave( st.alphaFunc2, st.alphaWave, t ), 0.f, 1.f );
	return c;
}

/// KenRadiant game look: a pass per stage - the stage's image, blend, alpha test, rgbGen / alphaGen, tcGen environment,
/// tcMods. $lightmap stages are left out: the light preview (or nothing) stands in for the lightmap, and the stage
/// that multiplies onto it becomes the base
bool OpenGLShader::constructStages( OpenGLState& first ){
	const std::vector<KrStage>* stages = m_shader->getStages();
	const int flags = m_shader->getFlags();
	if( stages == nullptr || ( flags & ( QER_SKY | QER_KR_TERRAIN ) ) || m_shader->krNotFound() )
		return false;
	bool lightmapped = false;
	float lmScale = 1;
	std::vector<const KrStage*> list;
	for( const KrStage& st : *stages ){
		if( st.special == 1 || st.tcGen == 1 ){
			lightmapped = true;
			if( st.blend && ( ( st.src == BLEND_DST_COLOUR && st.dst == BLEND_SRC_COLOUR ) || ( st.src == BLEND_SRC_COLOUR && st.dst == BLEND_DST_COLOUR ) ) )
				lmScale = 2; // dst * lm + lm * dst
			continue;
		}
		list.push_back( &st );
	}
	if( list.empty() )
		return false;
	const auto multiplies = []( const KrStage& st ){
		return st.blend && ( st.src == BLEND_DST_COLOUR || ( st.src == BLEND_ZERO && st.dst == BLEND_SRC_COLOUR ) ); // filter, 2x modulate ...
	};
	const KrStage& base = *list.front();
	const bool baseOpaque = !base.blend || ( base.src == BLEND_ONE && base.dst == BLEND_ZERO ) || ( lightmapped && multiplies( base ) );
	static GLuint white = 0;
	if( white == 0 ){
		const unsigned char px[4] = { 255, 255, 255, 255 };
		gl().glGenTextures( 1, &white );
		gl().glBindTexture( GL_TEXTURE_2D, white );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px );
		gl().glBindTexture( GL_TEXTURE_2D, 0 );
	}
	const auto load = [this]( const CopiedString& name ) -> GLuint {
		qtexture_t* t = GlobalTexturesCache().capture( name.c_str() );
		m_krTextures.push_back( t );
		return t != nullptr ? t->texture_number : 0;
	};
	int pass = 0;
	for( std::size_t i = 0; i < list.size(); ++i ){
		const KrStage& st = *list[i];
		auto gls = std::make_unique<KrStageGL>();
		gls->stage = &st;
		GLuint texture = st.special == 2 ? white : 0;
		if( st.special != 2 ){
			if( !st.frames.empty() ){
				for( const CopiedString& f : st.frames )
					if( const GLuint t = load( f ); t != 0 )
						gls->frames.push_back( t );
				texture = gls->frames.empty() ? 0 : gls->frames.front();
			}
			else
				texture = load( st.image );
		}
		if( texture == 0 ){
			if( i == 0 )
				return false; // the base image is missing: draw it the editor way
			continue;
		}
		OpenGLState& state = pass == 0 ? first : appendDefaultPass();
		const bool isBase = pass == 0;
		++pass;
		state.m_texture = texture;
		state.m_state = RENDER_FILL | RENDER_TEXTURE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_LIGHTING | RENDER_SMOOTH | RENDER_PROGRAM;
		state.m_program = &g_krLightGLSL;
		state.m_colour = Vector4( 1, 1, 1, 1 );
		state.m_krFx = &st.fx;
		if( !( flags & QER_CULL ) || m_shader->getCull() == IShader::eCullBack )
			state.m_state |= RENDER_CULLFACE;
		if( st.alphaFunc != 0 ){
			state.m_state |= RENDER_ALPHATEST;
			state.m_alphafunc = st.alphaFunc == 1 ? GL_GREATER : st.alphaFunc == 2 ? GL_LESS : GL_GEQUAL;
			state.m_alpharef = st.alphaFunc == 1 ? 0.f : 0.5f;
		}
		if( isBase && baseOpaque ){
			state.m_state |= RENDER_DEPTHWRITE;
			state.m_sort = list.size() > 1 ? OpenGLState::eSortMultiFirst : OpenGLState::eSortFullbright;
		}
		else{
			if( st.blend ){
				state.m_state |= RENDER_BLEND;
				state.m_blend_src = convertBlendFactor( st.src );
				state.m_blend_dst = convertBlendFactor( st.dst );
			}
			state.m_depthfunc = GL_LEQUAL;
			if( st.depthWrite )
				state.m_state |= RENDER_DEPTHWRITE;
			state.m_sort = baseOpaque ? OpenGLState::ESort( std::min( int( OpenGLState::eSortMultiFirst ) + pass - 1, int( OpenGLState::eSortMultiLast ) ) )
			                          : OpenGLState::eSortTranslucent;
		}
		// the light preview lights vertex-lit stages and the stage standing in for the lightmapped base
		gls->lit = st.rgbGen == 1 || st.rgbGen == 4 || ( lightmapped && isBase && ( st.rgbGen == 0 || st.rgbGen == 5 ) );
		// without light (preview off) a vertex-lit "2x" copy would double the full-bright texture: it adds nothing then
		gls->vertexCopy = i > 0 && st.blend && st.dst == BLEND_ONE && st.rgbGen == 1
		               && std::any_of( list.begin(), list.begin() + i, [&st]( const KrStage* e ){ return e->image == st.image; } );
		if( lightmapped && isBase && gls->lit && st.rgbGen != 1 )
			gls->lmScale = lmScale;
		state.m_krEmissive = !gls->lit;
		state.m_krStage = gls.get();
		m_krStagesGL.push_back( std::move( gls ) );
	}
	return pass != 0;
}
