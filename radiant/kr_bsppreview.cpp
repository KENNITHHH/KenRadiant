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
struct Batch {
	CopiedString name; IShader* shader = nullptr; bool skip = false; int lightmap = -1; bool blend = false; std::vector<Vertex> verts;
	std::vector<std::vector<qtexture_t*>> stageTextures; // per stage: image / animMap frames (game shaders on)
	bool stagesLoaded = false;
};
void Batch_releaseTextures( Batch& b ){
	for( auto& frames : b.stageTextures )
		for( qtexture_t* t : frames )
			GlobalTexturesCache().release( t );
	b.stageTextures.clear();
	b.stagesLoaded = false;
}

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
	for( Batch& b : g_batches ){
		Batch_releaseTextures( b ); // (the stage list belongs to the shader)
	}
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
	for( Batch& b : g_batches ){
		Batch_releaseTextures( b );
		if( b.shader != nullptr )
			b.shader->DecRef();
	}
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
	"uniform vec2 u_tcscale; uniform vec2 u_tcscroll; uniform int u_tclm;\n"
	"varying vec2 v_st; varying vec2 v_lm; varying vec4 v_colour;\n"
	"void main(){ v_st = ( u_tclm != 0 ? gl_MultiTexCoord1.st : gl_MultiTexCoord0.st ) * u_tcscale + u_tcscroll; v_lm = gl_MultiTexCoord1.st; v_colour = gl_Color; gl_Position = ftransform(); }\n";
// u_src: 0 texture, 1 the lightmap ($lightmap stage), 2 texture * lightmap (a shader without stages), 3 texture * vertex light (same, vertex lit)
// u_rgb: 0 identity, 1 vertex light, 2 constant (u_const); u_alpha: 0 texture, 1 vertex alpha, 2 constant
const char* const c_fp =
	"uniform sampler2D u_tex; uniform sampler2D u_lm; uniform int u_src; uniform int u_rgb; uniform int u_alpha; uniform int u_afunc;\n"
	"uniform vec4 u_const; uniform float u_shift; uniform float u_gamma;\n"
	"varying vec2 v_st; varying vec2 v_lm; varying vec4 v_colour;\n"
	"vec3 shifted( vec3 l ){ l *= u_shift; float peak = max( l.r, max( l.g, l.b ) ); if( peak > 1.0 ) l /= peak; return l; } // the engine's overbright clamp keeps the colour\n"
	"void main(){\n"
	"  vec4 c;\n"
	"  if( u_src == 1 ) c = vec4( shifted( texture2D( u_lm, v_lm ).rgb ), 1.0 );\n"
	"  else if( u_src == 4 ) c = vec4( shifted( v_colour.rgb ), 1.0 );\n"
	"  else if( u_src == 5 ) c = vec4( 1.0 ); // $whiteimage\n"
	"  else {\n"
	"    c = texture2D( u_tex, v_st );\n"
	"    if( u_src == 2 ) c.rgb *= shifted( texture2D( u_lm, v_lm ).rgb );\n"
	"    else if( u_src == 3 ) c.rgb *= shifted( v_colour.rgb );\n"
	"  }\n"
	"  if( u_rgb == 1 ) c.rgb *= shifted( v_colour.rgb ) * 0.5; else if( u_rgb == 2 ) c.rgb *= u_const.rgb; // rgbGen vertex: halved by the engine (identityLight)\n"
	"  if( u_alpha == 1 ) c.a *= v_colour.a; else if( u_alpha == 2 ) c.a *= u_const.a;\n"
	"  if( ( u_afunc == 1 && c.a <= 0.0 ) || ( u_afunc == 2 && c.a >= 0.5 ) || ( u_afunc == 3 && c.a < 0.5 ) ) discard;\n"
	"  gl_FragColor = vec4( pow( c.rgb, vec3( 1.0 / u_gamma ) ), c.a );\n"
	"}\n";
GLint u_src, u_rgb, u_alpha, u_afunc, u_const, u_tcscale, u_tcscroll, u_tclm;

GLuint Program_create(){
	GLuint program = gl().glCreateProgram();
	for( auto [type, src] : { std::pair<GLenum, const char*>{ GL_VERTEX_SHADER, c_vp }, { GL_FRAGMENT_SHADER, c_fp } } ){
		GLuint sh = gl().glCreateShader( type );
		gl().glShaderSource( sh, 1, &src, nullptr );
		gl().glCompileShader( sh );
		GLint ok = 0;
		gl().glGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
		if( !ok ){
			char log[2048] = {};
			gl().glGetShaderInfoLog( sh, sizeof( log ) - 1, nullptr, log );
			globalErrorStream() << "compiled lighting: shader error: " << log << '\n';
		}
		gl().glAttachShader( program, sh );
		gl().glDeleteShader( sh );
	}
	gl().glLinkProgram( program );
	gl().glUseProgram( program );
	const auto u = [program]( const char* n ){ return gl().glGetUniformLocation( program, n ); };
	u_tex = u( "u_tex" ); u_lm = u( "u_lm" ); u_shift = u( "u_shift" ); u_gamma = u( "u_gamma" );
	u_src = u( "u_src" ); u_rgb = u( "u_rgb" ); u_alpha = u( "u_alpha" ); u_afunc = u( "u_afunc" ); u_const = u( "u_const" );
	u_tcscale = u( "u_tcscale" ); u_tcscroll = u( "u_tcscroll" ); u_tclm = u( "u_tclm" );
	gl().glUniform1i( u_tex, 0 );
	gl().glUniform1i( u_lm, 1 );
	gl().glUseProgram( 0 );
	return program;
}

GLenum Blend_gl( BlendFactor f ){
	switch( f ){
	case BLEND_ZERO: return GL_ZERO;
	case BLEND_ONE: return GL_ONE;
	case BLEND_SRC_COLOUR: return GL_SRC_COLOR;
	case BLEND_ONE_MINUS_SRC_COLOUR: return GL_ONE_MINUS_SRC_COLOR;
	case BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
	case BLEND_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
	case BLEND_DST_COLOUR: return GL_DST_COLOR;
	case BLEND_ONE_MINUS_DST_COLOUR: return GL_ONE_MINUS_DST_COLOR;
	case BLEND_DST_ALPHA: return GL_DST_ALPHA;
	case BLEND_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
	case BLEND_SRC_ALPHA_SATURATE: return GL_SRC_ALPHA_SATURATE;
	default: return GL_ONE;
	}
}
float Wave( int func, const float w[4], float t ){
	const float x = w[2] + t * w[3], f = x - std::floor( x );
	float v = 0;
	if( func == 1 ) v = std::sin( x * 6.2831853f );
	else if( func == 2 ) v = 4 * std::fabs( ( x - 0.25f ) - std::floor( x - 0.25f ) - 0.5f ) - 1;
	else if( func == 3 ) v = f < 0.5f ? 1.f : -1.f;
	else if( func == 4 ) v = f;
	else if( func == 5 ) v = 1 - f;
	return w[0] + w[1] * v;
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
	extern bool g_krGameShaders;
	extern float g_krShaderTime;
	const float time = g_krShaderTime;
	const auto setArrays = []( const Batch& b ){
		const Vertex* v = b.verts.data();
		gl().glVertexPointer( 3, GL_FLOAT, sizeof( Vertex ), v->xyz );
		gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( Vertex ), v->colour );
		gl().glClientActiveTexture( GL_TEXTURE0 );
		gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( Vertex ), v->st );
		gl().glClientActiveTexture( GL_TEXTURE1 );
		gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( Vertex ), v->lm );
	};
	const auto plain = [&]( const Batch& b ){
		gl().glUniform1i( u_src, b.lightmap >= 0 ? 2 : 3 );
		gl().glUniform1i( u_rgb, 0 );
		gl().glUniform1i( u_alpha, 0 );
		gl().glUniform1i( u_afunc, 0 );
		gl().glUniform2f( u_tcscale, 1, 1 );
		gl().glUniform2f( u_tcscroll, 0, 0 );
		gl().glUniform1i( u_tclm, 0 );
		gl().glActiveTexture( GL_TEXTURE0 );
		gl().glBindTexture( GL_TEXTURE_2D, b.shader->getTexture()->texture_number );
		gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( b.verts.size() ) );
	};
	for( int pass = 0; pass < 2; ++pass ) // opaque, then translucent
		for( Batch& b : g_batches ){
			if( b.shader == nullptr || b.verts.empty() )
				continue;
			const std::vector<KrStage>* stages = g_krGameShaders ? b.shader->getStages() : nullptr;
			const bool translucent = b.blend || ( stages != nullptr && !stages->empty() && stages->front().blend
			                                      && !( stages->front().src == BLEND_ONE && stages->front().dst == BLEND_ZERO ) && stages->front().special != 1 );
			if( translucent != ( pass == 1 ) )
				continue;
			gl().glActiveTexture( GL_TEXTURE1 );
			gl().glBindTexture( GL_TEXTURE_2D, b.lightmap >= 0 ? g_lightmaps[b.lightmap] : 0 );
			setArrays( b );
			if( stages == nullptr || stages->empty() ){
				if( translucent ){
					gl().glEnable( GL_BLEND );
					gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
					gl().glDepthMask( GL_FALSE );
				}
				else{
					gl().glDisable( GL_BLEND );
					gl().glDepthMask( GL_TRUE );
				}
				plain( b );
				continue;
			}
			// the shader's stages, as the engine draws them
			if( !b.stagesLoaded ){
				b.stagesLoaded = true;
				for( const KrStage& st : *stages ){
					std::vector<qtexture_t*> frames;
					if( !st.frames.empty() )
						for( const CopiedString& f : st.frames )
							frames.push_back( GlobalTexturesCache().capture( f.c_str() ) );
					else if( !st.image.empty() )
						frames.push_back( GlobalTexturesCache().capture( st.image.c_str() ) );
					b.stageTextures.push_back( std::move( frames ) );
				}
			}
			for( std::size_t i = 0; i < stages->size(); ++i ){
				const KrStage& st = ( *stages )[i];
				const bool replace = !st.blend || ( st.src == BLEND_ONE && st.dst == BLEND_ZERO );
				if( i == 0 && replace ){
					gl().glDisable( GL_BLEND );
					gl().glDepthMask( GL_TRUE );
				}
				else{
					gl().glEnable( GL_BLEND );
					if( replace )
						gl().glBlendFunc( GL_ONE, GL_ZERO );
					else
						gl().glBlendFunc( Blend_gl( st.src ), Blend_gl( st.dst ) );
					gl().glDepthMask( st.depthWrite ? GL_TRUE : GL_FALSE );
				}
				int src = 0;
				if( st.special == 1 || st.tcGen == 1 )
					src = b.lightmap >= 0 ? 1 : 4; // vertex lit surface: the vertex light stands in for the lightmap
				GLuint texture = 0;
				if( src == 0 ){
					const auto& frames = i < b.stageTextures.size() ? b.stageTextures[i] : std::vector<qtexture_t*>();
					if( st.special == 2 )
						texture = 0;
					else if( !frames.empty() ){
						const std::size_t frame = frames.size() > 1 && st.animFreq > 0 ? std::size_t( std::max( 0.f, time * st.animFreq ) ) % frames.size() : 0;
						texture = frames[frame] != nullptr ? frames[frame]->texture_number : 0;
					}
					if( texture == 0 && st.special != 2 )
						continue; // missing image: leave the stage out
				}
				gl().glActiveTexture( GL_TEXTURE0 );
				gl().glBindTexture( GL_TEXTURE_2D, texture );
				gl().glUniform1i( u_src, st.special == 2 ? 5 : src );
				gl().glUniform1i( u_rgb, ( st.rgbGen == 1 || st.rgbGen == 4 ) ? 1 : ( st.rgbGen == 2 || st.rgbGen == 3 || st.rgbGen == 5 ) ? 2 : 0 );
				Vector4 c( 1, 1, 1, 1 );
				if( st.rgbGen == 2 ){ const float v = std::clamp( Wave( st.rgbFunc, st.rgbWave, time ), 0.f, 1.f ); c = Vector4( v, v, v, 1 ); }
				else if( st.rgbGen == 5 ) c = Vector4( 0.5f, 0.5f, 0.5f, 1 ); // identityLighting
				else if( st.rgbGen == 3 ) c = Vector4( st.rgbConst[0], st.rgbConst[1], st.rgbConst[2], 1 );
				int alpha = 0;
				if( st.alphaGen == 1 ){ alpha = 2; c[3] = st.alphaConst; }
				else if( st.alphaGen == 2 ){ alpha = 2; c[3] = std::clamp( Wave( st.alphaFunc2, st.alphaWave, time ), 0.f, 1.f ); }
				else if( st.alphaGen == 3 ) alpha = 1;
				gl().glUniform4f( u_const, c[0], c[1], c[2], c[3] );
				gl().glUniform1i( u_alpha, alpha );
				gl().glUniform1i( u_afunc, st.alphaFunc );
				gl().glUniform2f( u_tcscale, st.fx.scale[0], st.fx.scale[1] );
				gl().glUniform2f( u_tcscroll, std::fmod( st.fx.scroll[0] * time, 1.f ), std::fmod( st.fx.scroll[1] * time, 1.f ) );
				gl().glUniform1i( u_tclm, 0 );
				gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( b.verts.size() ) );
			}
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
	for( Batch& b : g_batches ){
		Batch_releaseTextures( b );
		if( b.shader != nullptr )
			b.shader->DecRef();
	}
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
