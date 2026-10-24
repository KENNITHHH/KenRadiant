/*
   KenRadiant: compiled lighting preview - draws the map's compiled .bsp (its surfaces with their baked lightmaps
   or vertex light, as the game shows them) over the editor geometry in the 3D view.
   Build menu > Show Compiled Lighting (.bsp); reloads by itself after a compile.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <vector>
#include <map>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

#include "igl.h"
#include "ishaders.h"
#include "itextures.h"
#include "texturelib.h"
#include "stream/stringstream.h"
#include "os/path.h"
#include "commands.h"
#include "gtkutil/widget.h"
#include "stringio.h"
#include "dialog.h"
#include "map.h"
#include "math/vector.h"
#include "kr_lightpreview.h"

void KrLightPreview_redraw();
void createShader( GLuint program, const char* filename, GLenum type );
void GLSLProgram_link( GLuint program );
#include "qerplugin.h"
#include "moduleobserver.h"

namespace
{

#pragma pack( push, 1 )
struct BspLump { int offset, length; };
struct BspHeader { char ident[4]; int version; BspLump lumps[18]; };
struct BspShader { char name[64]; int surfaceFlags, contentFlags; };
struct BspVert { float xyz[3]; float st[2]; float lm[4][2]; float normal[3]; unsigned char colour[4][4]; };
struct BspSurface {
	int shader, fog, type, firstVert, numVerts, firstIndex, numIndexes;
	unsigned char lmStyles[4], vtxStyles[4];
	int lmNum[4], lmX[4], lmY[4], lmW, lmH;
	float lmOrigin[3], lmVecs[3][3];
	int patchW, patchH;
};
#pragma pack( pop )
static_assert( sizeof( BspVert ) == 80 && sizeof( BspSurface ) == 148 && sizeof( BspShader ) == 72, "RBSP layout" );

enum { LUMP_SHADERS = 1, LUMP_DRAWVERTS = 10, LUMP_DRAWINDEXES = 11, LUMP_SURFACES = 13, LUMP_LIGHTMAPS = 14 };
enum { MST_PLANAR = 1, MST_PATCH = 2, MST_TRIANGLE_SOUP = 3 };

struct Vertex { float xyz[3]; float st[2]; float lm[2]; unsigned char colour[4]; };
struct Batch { CopiedString name; IShader* shader = nullptr; bool skip = false; int lightmap = -1; bool blend = false; std::vector<Vertex> verts; };

/// shader reloads (Flush & Reload, skybox / game shader toggles) must not find our shader references: give them back
/// and pick them up again on the next draw
bool g_shadersReleased = false;
class ShaderObserver : public ModuleObserver
{
public:
	void realise() override {}
	void unrealise() override;
};
ShaderObserver g_shaderObserver;
bool g_observing = false;

bool g_show = false;
std::vector<Batch> g_batches;
std::vector<GLuint> g_lightmaps;
CopiedString g_loadedFile;
QDateTime g_loadedTime;
GLuint g_program = 0;
GLint u_lm, u_tex, u_uselm, u_shift, u_gamma;

void ShaderObserver::unrealise(){
	for( Batch& b : g_batches )
		if( b.shader != nullptr ){
			b.shader->DecRef();
			b.shader = nullptr;
		}
	g_shadersReleased = true;
}
void Shaders_acquire(){
	if( !g_shadersReleased )
		return;
	g_shadersReleased = false;
	for( Batch& b : g_batches )
		if( !b.skip && b.shader == nullptr )
			b.shader = GlobalShaderSystem().getShaderForName( b.name.c_str() );
}

void Bsp_unload(){
	for( Batch& b : g_batches )
		if( b.shader != nullptr )
			b.shader->DecRef();
	g_batches.clear();
	if( !g_lightmaps.empty() )
		gl().glDeleteTextures( GLsizei( g_lightmaps.size() ), g_lightmaps.data() );
	g_lightmaps.clear();
	g_loadedFile = "";
}

template<typename T>
std::vector<T> Lump( const QByteArray& data, const BspHeader& h, int lump ){
	std::vector<T> out;
	const BspLump& l = h.lumps[lump];
	if( l.offset < 0 || l.length < 0 || std::size_t( l.offset ) + l.length > std::size_t( data.size() ) )
		return out;
	out.resize( l.length / sizeof( T ) );
	if( !out.empty() )
		std::memcpy( out.data(), data.constData() + l.offset, out.size() * sizeof( T ) );
	return out;
}

Vertex Vertex_from( const BspVert& v ){
	Vertex o;
	std::copy( v.xyz, v.xyz + 3, o.xyz );
	std::copy( v.st, v.st + 2, o.st );
	o.lm[0] = v.lm[0][0]; o.lm[1] = v.lm[0][1];
	std::copy( v.colour[0], v.colour[0] + 4, o.colour );
	return o;
}
Vertex Vertex_lerp3( const Vertex& a, const Vertex& b, const Vertex& c, float t ){ // quadratic bezier
	const float u = 1 - t, wa = u * u, wb = 2 * u * t, wc = t * t;
	Vertex o;
	for( int i = 0; i < 3; ++i ) o.xyz[i] = a.xyz[i] * wa + b.xyz[i] * wb + c.xyz[i] * wc;
	for( int i = 0; i < 2; ++i ) o.st[i] = a.st[i] * wa + b.st[i] * wb + c.st[i] * wc;
	for( int i = 0; i < 2; ++i ) o.lm[i] = a.lm[i] * wa + b.lm[i] * wb + c.lm[i] * wc;
	for( int i = 0; i < 4; ++i ) o.colour[i] = (unsigned char)std::clamp( a.colour[i] * wa + b.colour[i] * wb + c.colour[i] * wc, 0.f, 255.f );
	return o;
}

bool Bsp_load( const char* path ){
	Bsp_unload();
	QFile file( path );
	if( !file.open( QIODevice::ReadOnly ) ){
		globalWarningStream() << "compiled lighting: no " << path << " - compile the map first (Build menu or F9)\n";
		return false;
	}
	const QByteArray data = file.readAll();
	if( data.size() < int( sizeof( BspHeader ) ) )
		return false;
	BspHeader h;
	std::memcpy( &h, data.constData(), sizeof( h ) );
	if( std::strncmp( h.ident, "RBSP", 4 ) != 0 && std::strncmp( h.ident, "IBSP", 4 ) != 0 ){
		globalWarningStream() << "compiled lighting: " << path << " is not a Quake 3 / SOF2 .bsp\n";
		return false;
	}
	const bool rbsp = std::strncmp( h.ident, "RBSP", 4 ) == 0;
	if( !rbsp ){
		globalWarningStream() << "compiled lighting: only RBSP (SOF2 / JK2 / JA) .bsp files are read\n";
		return false;
	}
	const auto shaders = Lump<BspShader>( data, h, LUMP_SHADERS );
	const auto verts = Lump<BspVert>( data, h, LUMP_DRAWVERTS );
	const auto indexes = Lump<int>( data, h, LUMP_DRAWINDEXES );
	const auto surfaces = Lump<BspSurface>( data, h, LUMP_SURFACES );
	const auto lightBytes = Lump<unsigned char>( data, h, LUMP_LIGHTMAPS );

	// lightmaps: 128 x 128 RGB each
	constexpr int lmSize = 128;
	const std::size_t lmCount = lightBytes.size() / ( lmSize * lmSize * 3 );
	g_lightmaps.resize( lmCount );
	if( lmCount != 0 ){
		gl().glGenTextures( GLsizei( lmCount ), g_lightmaps.data() );
		for( std::size_t i = 0; i < lmCount; ++i ){
			gl().glBindTexture( GL_TEXTURE_2D, g_lightmaps[i] );
			gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
			gl().glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
			gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB, lmSize, lmSize, 0, GL_RGB, GL_UNSIGNED_BYTE, lightBytes.data() + i * lmSize * lmSize * 3 );
		}
		gl().glBindTexture( GL_TEXTURE_2D, 0 );
	}

	std::map<std::pair<int, int>, std::size_t> batchOf; // ( shader, lightmap ) -> batch
	std::size_t triangles = 0;
	for( const BspSurface& s : surfaces ){
		if( s.shader < 0 || s.shader >= int( shaders.size() ) || s.numVerts <= 0 )
			continue;
		if( s.firstVert < 0 || std::size_t( s.firstVert ) + s.numVerts > verts.size() )
			continue;
		const int lightmap = s.lmNum[0] >= 0 && s.lmNum[0] < int( lmCount ) ? s.lmNum[0] : -1;
		auto [it, added] = batchOf.emplace( std::make_pair( s.shader, lightmap ), g_batches.size() );
		if( added ){
			Batch b;
			char name[65] = {};
			std::memcpy( name, shaders[s.shader].name, 64 );
			b.name = name;
			b.shader = GlobalShaderSystem().getShaderForName( name );
			if( b.shader->getFlags() & ( QER_SKY | QER_NODRAW ) ){ // the editor draws the sky; nodraw is never seen
				b.shader->DecRef();
				b.shader = nullptr;
				b.skip = true;
			}
			else{
				b.blend = ( b.shader->getFlags() & QER_TRANS ) != 0;
			}
			b.lightmap = lightmap;
			g_batches.push_back( std::move( b ) );
		}
		Batch& b = g_batches[it->second];
		if( b.skip )
			continue;
		const BspVert* v = verts.data() + s.firstVert;
		if( s.type == MST_PLANAR || s.type == MST_TRIANGLE_SOUP ){
			if( s.firstIndex < 0 || std::size_t( s.firstIndex ) + s.numIndexes > indexes.size() )
				continue;
			for( int i = 0; i < s.numIndexes; ++i ){
				const int k = indexes[s.firstIndex + i];
				if( k >= 0 && k < s.numVerts )
					b.verts.push_back( Vertex_from( v[k] ) );
			}
			triangles += s.numIndexes / 3;
		}
		else if( s.type == MST_PATCH && s.patchW >= 3 && s.patchH >= 3 && s.patchW * s.patchH <= s.numVerts ){
			// quadratic bezier sub-patches, 8 x 8 each
			constexpr int steps = 8;
			for( int py = 0; py + 2 < s.patchH; py += 2 )
				for( int px = 0; px + 2 < s.patchW; px += 2 ){
					Vertex grid[steps + 1][steps + 1];
					for( int j = 0; j <= steps; ++j ){
						const float t = float( j ) / steps;
						Vertex col[3];
						for( int r = 0; r < 3; ++r ){
							const Vertex a = Vertex_from( v[( py + r ) * s.patchW + px] );
							const Vertex m = Vertex_from( v[( py + r ) * s.patchW + px + 1] );
							const Vertex c = Vertex_from( v[( py + r ) * s.patchW + px + 2] );
							col[r] = Vertex_lerp3( a, m, c, t );
						}
						for( int i = 0; i <= steps; ++i )
							grid[i][j] = Vertex_lerp3( col[0], col[1], col[2], float( i ) / steps );
					}
					for( int i = 0; i < steps; ++i )
						for( int j = 0; j < steps; ++j ){
							b.verts.insert( b.verts.end(), { grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j], grid[i + 1][j + 1], grid[i][j + 1] } );
							triangles += 2;
						}
				}
		}
	}
	g_loadedFile = path;
	g_loadedTime = QFileInfo( path ).lastModified();
	globalOutputStream() << "compiled lighting: " << path << " - " << int( surfaces.size() ) << " surfaces, " << int( triangles ) << " triangles, "
	                     << int( lmCount ) << " lightmaps\n";
	return true;
}

const char* const c_vp =
	"varying vec2 v_st; varying vec2 v_lm; varying vec4 v_colour;\n"
	"void main(){ v_st = gl_MultiTexCoord0.st; v_lm = gl_MultiTexCoord1.st; v_colour = gl_Color; gl_Position = ftransform(); }\n";
const char* const c_fp =
	"uniform sampler2D u_tex; uniform sampler2D u_lm; uniform int u_uselm; uniform float u_shift; uniform float u_gamma;\n"
	"varying vec2 v_st; varying vec2 v_lm; varying vec4 v_colour;\n"
	"void main(){\n"
	"  vec4 t = texture2D( u_tex, v_st );\n"
	"  vec3 l = ( u_uselm != 0 ? texture2D( u_lm, v_lm ).rgb : v_colour.rgb ) * u_shift;\n"
	"  float peak = max( l.r, max( l.g, l.b ) ); if( peak > 1.0 ) l /= peak; // the engine's overbright clamp keeps the colour\n"
	"  gl_FragColor = vec4( pow( t.rgb * l, vec3( 1.0 / u_gamma ) ), t.a );\n"
	"}\n";

GLuint Program_create(){
	GLuint program = gl().glCreateProgram();
	for( auto [type, src] : { std::pair<GLenum, const char*>{ GL_VERTEX_SHADER, c_vp }, { GL_FRAGMENT_SHADER, c_fp } } ){
		GLuint s = gl().glCreateShader( type );
		gl().glShaderSource( s, 1, &src, nullptr );
		gl().glCompileShader( s );
		gl().glAttachShader( program, s );
		gl().glDeleteShader( s );
	}
	gl().glLinkProgram( program );
	gl().glUseProgram( program );
	u_tex = gl().glGetUniformLocation( program, "u_tex" );
	u_lm = gl().glGetUniformLocation( program, "u_lm" );
	u_uselm = gl().glGetUniformLocation( program, "u_uselm" );
	u_shift = gl().glGetUniformLocation( program, "u_shift" );
	u_gamma = gl().glGetUniformLocation( program, "u_gamma" );
	gl().glUniform1i( u_tex, 0 );
	gl().glUniform1i( u_lm, 1 );
	gl().glUseProgram( 0 );
	return program;
}

CopiedString Bsp_pathForMap(){
	return CopiedString( StringStream( PathExtensionless( Map_Name( g_map ) ), ".bsp" ) );
}

ToggleItem g_show_item{ BoolExportCaller( g_show ) };
void Show_toggle(){
	g_show = !g_show;
	g_show_item.update();
	globalOutputStream() << ( g_show ? "compiled lighting: on\n" : "compiled lighting: off\n" );
	KrLightPreview_redraw();
}

} // namespace

/// called by the 3D view before the scene: draws the compiled map; the editor's own surfaces lie at the same depth
/// and lose (GL_LESS), so the compiled look shows while selections still draw on top
void KrBspPreview_draw( const Matrix4& modelview, const Matrix4& projection ){
	if( !g_show ){
		if( !g_batches.empty() || !g_lightmaps.empty() )
			Bsp_unload();
		return;
	}
	if( Map_Unnamed( g_map ) )
		return;
	const CopiedString path = Bsp_pathForMap();
	const QFileInfo info( path.c_str() );
	if( !string_equal( path.c_str(), g_loadedFile.c_str() ) || ( info.exists() && info.lastModified() != g_loadedTime ) ){
		static CopiedString failed;
		static QDateTime failedTime;
		if( !( string_equal( failed.c_str(), path.c_str() ) && failedTime == info.lastModified() ) && !Bsp_load( path.c_str() ) ){
			failed = path;
			failedTime = info.lastModified();
		}
	}
	if( g_batches.empty() )
		return;
	if( !g_observing ){
		g_observing = true;
		GlobalShaderSystem().attach( g_shaderObserver );
	}
	Shaders_acquire();
	if( g_program == 0 )
		g_program = Program_create();
	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &projection ) );
	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &modelview ) );
	gl().glUseProgram( g_program );
	gl().glUniform1f( u_shift, float( 1 << std::clamp( g_krEngineShift, 0, 2 ) ) );
	gl().glUniform1f( u_gamma, std::clamp( g_krEngineGamma, 0.5f, 3.f ) );
	gl().glEnable( GL_DEPTH_TEST );
	gl().glDepthFunc( GL_LEQUAL );
	gl().glDepthMask( GL_TRUE );
	gl().glDisable( GL_CULL_FACE );
	gl().glDisable( GL_ALPHA_TEST );
	gl().glEnable( GL_POLYGON_OFFSET_FILL );
	gl().glPolygonOffset( -1, -1 );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
	gl().glEnableClientState( GL_VERTEX_ARRAY );
	gl().glEnableClientState( GL_COLOR_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE1 );
	gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	for( int pass = 0; pass < 2; ++pass ) // opaque, then translucent
		for( const Batch& b : g_batches ){
			if( b.shader == nullptr || b.verts.empty() || b.blend != ( pass == 1 ) )
				continue;
			if( b.blend ){
				gl().glEnable( GL_BLEND );
				gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
				gl().glDepthMask( GL_FALSE );
			}
			gl().glActiveTexture( GL_TEXTURE1 );
			gl().glBindTexture( GL_TEXTURE_2D, b.lightmap >= 0 ? g_lightmaps[b.lightmap] : 0 );
			gl().glActiveTexture( GL_TEXTURE0 );
			gl().glBindTexture( GL_TEXTURE_2D, b.shader->getTexture()->texture_number ); // (after a shader reload too)
			gl().glUniform1i( u_uselm, b.lightmap >= 0 ? 1 : 0 );
			const Vertex* v = b.verts.data();
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( Vertex ), v->xyz );
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( Vertex ), v->colour );
			gl().glClientActiveTexture( GL_TEXTURE0 );
			gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( Vertex ), v->st );
			gl().glClientActiveTexture( GL_TEXTURE1 );
			gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( Vertex ), v->lm );
			gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( b.verts.size() ) );
		}
	gl().glDisable( GL_BLEND );
	gl().glDepthMask( GL_TRUE );
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glDisableClientState( GL_COLOR_ARRAY );
	gl().glDisable( GL_POLYGON_OFFSET_FILL );
	gl().glPolygonOffset( -1, 1 );
	gl().glDepthFunc( GL_LESS );
	gl().glActiveTexture( GL_TEXTURE1 );
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
	gl().glActiveTexture( GL_TEXTURE0 );
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
	gl().glUseProgram( 0 );
}

/// the editor closes: give back the shaders while the shader system still exists
void KrBspPreview_shutdown(){
	for( Batch& b : g_batches )
		if( b.shader != nullptr )
			b.shader->DecRef();
	g_batches.clear();
	g_lightmaps.clear(); // (the textures die with the GL context)
	if( g_observing ){
		g_observing = false;
		GlobalShaderSystem().detach( g_shaderObserver );
	}
}

void KrBspPreview_registerCommands(){
	GlobalToggles_insert( "ToggleCompiledLighting", makeCallbackF( Show_toggle ), ToggleItem::AddCallbackCaller( g_show_item ) );
}
