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

struct Vertex { float xyz[3]; float st[2]; float lm[2]; unsigned char colour[4]; float normal[3]; };
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
	std::copy( v.normal, v.normal + 3, o.normal );
	return o;
}
Vertex Vertex_lerp3( const Vertex& a, const Vertex& b, const Vertex& c, float t ){ // quadratic bezier
	const float u = 1 - t, wa = u * u, wb = 2 * u * t, wc = t * t;
	Vertex o;
	for( int i = 0; i < 3; ++i ) o.xyz[i] = a.xyz[i] * wa + b.xyz[i] * wb + c.xyz[i] * wc;
	for( int i = 0; i < 2; ++i ) o.st[i] = a.st[i] * wa + b.st[i] * wb + c.st[i] * wc;
	for( int i = 0; i < 2; ++i ) o.lm[i] = a.lm[i] * wa + b.lm[i] * wb + c.lm[i] * wc;
	for( int i = 0; i < 4; ++i ) o.colour[i] = (unsigned char)std::clamp( a.colour[i] * wa + b.colour[i] * wb + c.colour[i] * wc, 0.f, 255.f );
	for( int i = 0; i < 3; ++i ) o.normal[i] = a.normal[i] * wa + b.normal[i] * wb + c.normal[i] * wc;
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
	"uniform vec2 u_tcscale; uniform vec2 u_tcscroll; uniform int u_tclm; uniform int u_env; uniform vec3 u_campos;\n"
	"varying vec2 v_st; varying vec2 v_lm; varying vec4 v_colour;\n"
	"void main(){\n"
	"  vec2 st = u_tclm != 0 ? gl_MultiTexCoord1.st : gl_MultiTexCoord0.st;\n"
	"  if( u_env != 0 ){ // tcGen environment, like the engine (RB_CalcEnvironmentTexCoords)\n"
	"    vec3 n = normalize( gl_Normal ); vec3 viewer = normalize( u_campos - gl_Vertex.xyz );\n"
	"    vec3 r = n * ( 2.0 * dot( n, viewer ) ) - viewer; st = vec2( 0.5 + r.y * 0.5, 0.5 - r.z * 0.5 );\n"
	"  }\n"
	"  v_st = st * u_tcscale + u_tcscroll; v_lm = gl_MultiTexCoord1.st; v_colour = gl_Color; gl_Position = ftransform();\n"
	"}\n";
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
	"  if( u_rgb == 1 ) c.rgb *= shifted( v_colour.rgb ); else if( u_rgb == 2 ) c.rgb *= u_const.rgb; // rgbGen vertex: the stored colour as it is (measured in game with the Lighting Probe)\n"
	"  if( u_alpha == 1 ) c.a *= v_colour.a; else if( u_alpha == 2 ) c.a *= u_const.a;\n"
	"  if( ( u_afunc == 1 && c.a <= 0.0 ) || ( u_afunc == 2 && c.a >= 0.5 ) || ( u_afunc == 3 && c.a < 0.5 ) ) discard;\n"
	"  gl_FragColor = vec4( pow( c.rgb, vec3( 1.0 / u_gamma ) ), c.a );\n"
	"}\n";
GLint u_src, u_rgb, u_alpha, u_afunc, u_const, u_tcscale, u_tcscroll, u_tclm, u_env, u_campos;

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
	u_tcscale = u( "u_tcscale" ); u_tcscroll = u( "u_tcscroll" ); u_tclm = u( "u_tclm" ); u_env = u( "u_env" ); u_campos = u( "u_campos" );
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


/// the map's .bsp loaded (again when it changed), shaders held
bool Bsp_ensure(){
	if( Map_Unnamed( g_map ) )
		return false;
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
		return false;
	if( !g_observing ){
		g_observing = true;
		GlobalShaderSystem().attach( g_shaderObserver );
	}
	Shaders_acquire();
	return true;
}

void Batch_loadStages( Batch& b, const std::vector<KrStage>& stages ){
	if( b.stagesLoaded )
		return;
	b.stagesLoaded = true;
	for( const KrStage& st : stages ){
		std::vector<qtexture_t*> frames;
		if( !st.frames.empty() )
			for( const CopiedString& f : st.frames )
				frames.push_back( GlobalTexturesCache().capture( f.c_str() ) );
		else if( !st.image.empty() )
			frames.push_back( GlobalTexturesCache().capture( st.image.c_str() ) );
		b.stageTextures.push_back( std::move( frames ) );
	}
}
bool Batch_translucent( const Batch& b, const std::vector<KrStage>* stages ){
	return b.blend || ( stages != nullptr && !stages->empty() && stages->front().blend
	                    && !( stages->front().src == BLEND_ONE && stages->front().dst == BLEND_ZERO ) && stages->front().special != 1 );
}

GLuint Batch_albedoTexture( Batch& b );
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
	extern bool g_krProbeDebug;
	if( g_krProbeDebug )
		return; // Lighting Probe: the frame that reads the live preview's values
	if( !g_show ){
		if( !g_batches.empty() || !g_lightmaps.empty() )
			Bsp_unload();
		return;
	}
	if( !Bsp_ensure() )
		return;
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
	gl().glEnableClientState( GL_NORMAL_ARRAY );
	gl().glEnableClientState( GL_VERTEX_ARRAY );
	gl().glEnableClientState( GL_COLOR_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE1 );
	gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	{
		const Matrix4 inv = matrix4_full_inverse( modelview );
		gl().glUniform3f( u_campos, inv[12], inv[13], inv[14] );
	}
	gl().glUniform1i( u_env, 0 );
	extern bool g_krGameShaders;
	extern float g_krShaderTime;
	const float time = g_krShaderTime;
	const auto setArrays = []( const Batch& b ){
		const Vertex* v = b.verts.data();
		gl().glVertexPointer( 3, GL_FLOAT, sizeof( Vertex ), v->xyz );
		gl().glNormalPointer( GL_FLOAT, sizeof( Vertex ), v->normal );
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
			const bool translucent = Batch_translucent( b, stages );
			if( translucent != ( pass == 1 ) )
				continue;
			// decals: nearer than the editor's own polygonOffset faces (-1, -2), so the compiled look covers them
			gl().glPolygonOffset( -1, translucent ? -4.f : -1.f );
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
			Batch_loadStages( b, *stages );
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
				else if( st.rgbGen == 5 ) c = Vector4( 1, 1, 1, 1 ); // identityLighting: 1 with r_overBrightBits 0
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
				gl().glUniform1i( u_env, st.tcGen == 2 ? 1 : 0 );
				gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( b.verts.size() ) );
				gl().glUniform1i( u_env, 0 );
			}
		}
	// blended surfaces (decals) hide the editor's own faces behind them too: their depth, where they are seen
	gl().glDisable( GL_BLEND );
	gl().glDepthMask( GL_TRUE );
	gl().glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	gl().glPolygonOffset( -1, -4 );
	for( Batch& b : g_batches ){
		if( b.shader == nullptr || b.verts.empty() || !Batch_translucent( b, g_krGameShaders ? b.shader->getStages() : nullptr ) )
			continue;
		// only decals (polygonOffset): glass and other see-through surfaces must not hide what is behind them
		if( const KrShaderEffects* fx = b.shader->getEffects(); fx == nullptr || !fx->polygonOffset )
			continue;
		setArrays( b );
		gl().glUniform1i( u_src, 0 );
		gl().glUniform1i( u_rgb, 0 );
		gl().glUniform1i( u_alpha, 0 );
		gl().glUniform1i( u_afunc, 3 ); // see-through texels stay open
		gl().glUniform2f( u_tcscale, 1, 1 );
		gl().glUniform2f( u_tcscroll, 0, 0 );
		gl().glActiveTexture( GL_TEXTURE0 );
		gl().glBindTexture( GL_TEXTURE_2D, Batch_albedoTexture( b ) );
		gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( b.verts.size() ) );
	}
	gl().glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	gl().glDisable( GL_BLEND );
	gl().glDepthMask( GL_TRUE );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
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


namespace
{
const char* const c_dvp =
	"varying vec2 v_st; varying vec2 v_lm; varying vec4 v_colour;\n"
	"void main(){ v_st = gl_MultiTexCoord0.st; v_lm = gl_MultiTexCoord1.st; v_colour = gl_Color; gl_Position = ftransform(); }\n";
const char* const c_dfp =
	"uniform sampler2D u_tex; uniform sampler2D u_lm; uniform int u_mode; uniform int u_lmd; uniform int u_cut; uniform vec3 u_id;\n"
	"varying vec2 v_st; varying vec2 v_lm; varying vec4 v_colour;\n"
	"void main(){\n"
	"  vec4 t = texture2D( u_tex, v_st );\n"
	"  if( u_cut != 0 && t.a < 0.5 ) discard;\n"
	"  if( u_mode == 1 ) gl_FragColor = vec4( u_lmd != 0 ? texture2D( u_lm, v_lm ).rgb : v_colour.rgb, 1.0 );\n"
	"  else if( u_mode == 2 ) gl_FragColor = t;\n"
	"  else gl_FragColor = vec4( u_id, 1.0 );\n"
	"}\n";
GLuint g_debugProgram = 0;
GLint ud_mode, ud_lmd, ud_cut, ud_id;

GLuint Batch_albedoTexture( Batch& b ){
	if( const std::vector<KrStage>* stages = b.shader->getStages(); stages != nullptr && !stages->empty() ){
		Batch_loadStages( b, *stages );
		for( std::size_t i = 0; i < stages->size() && i < b.stageTextures.size(); ++i ){
			const KrStage& st = ( *stages )[i];
			if( st.special == 0 && st.tcGen == 0 && !b.stageTextures[i].empty() && b.stageTextures[i][0] != nullptr )
				return b.stageTextures[i][0]->texture_number;
		}
	}
	return b.shader->getTexture()->texture_number;
}
}

/// Lighting Probe: draws the .bsp as data instead of pictures - mode 1 the stored light (lightmap texel or vertex
/// colour, unchanged), 2 the shader's first image (with its alpha), 3 the surface batch id (index + 1 in RGB).
/// Blended surfaces count where their image is at least half opaque. Returns false without a .bsp.
bool KrBspPreview_drawDebug( int mode, const Matrix4& modelview, const Matrix4& projection ){
	if( !Bsp_ensure() )
		return false;
	if( g_debugProgram == 0 ){
		g_debugProgram = gl().glCreateProgram();
		for( auto [type, src] : { std::pair<GLenum, const char*>{ GL_VERTEX_SHADER, c_dvp }, { GL_FRAGMENT_SHADER, c_dfp } } ){
			GLuint sh = gl().glCreateShader( type );
			gl().glShaderSource( sh, 1, &src, nullptr );
			gl().glCompileShader( sh );
			gl().glAttachShader( g_debugProgram, sh );
			gl().glDeleteShader( sh );
		}
		gl().glLinkProgram( g_debugProgram );
		gl().glUseProgram( g_debugProgram );
		gl().glUniform1i( gl().glGetUniformLocation( g_debugProgram, "u_tex" ), 0 );
		gl().glUniform1i( gl().glGetUniformLocation( g_debugProgram, "u_lm" ), 1 );
		ud_mode = gl().glGetUniformLocation( g_debugProgram, "u_mode" );
		ud_lmd = gl().glGetUniformLocation( g_debugProgram, "u_lmd" );
		ud_cut = gl().glGetUniformLocation( g_debugProgram, "u_cut" );
		ud_id = gl().glGetUniformLocation( g_debugProgram, "u_id" );
	}
	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &projection ) );
	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &modelview ) );
	gl().glUseProgram( g_debugProgram );
	gl().glUniform1i( ud_mode, mode );
	gl().glDisable( GL_BLEND );
	gl().glDisable( GL_DITHER );
	gl().glEnable( GL_DEPTH_TEST );
	gl().glDepthFunc( GL_LEQUAL );
	gl().glDepthMask( GL_TRUE );
	gl().glDisable( GL_CULL_FACE );
	gl().glDisable( GL_ALPHA_TEST );
	gl().glEnable( GL_POLYGON_OFFSET_FILL );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
	gl().glEnableClientState( GL_VERTEX_ARRAY );
	gl().glEnableClientState( GL_COLOR_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE1 );
	gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	for( int pass = 0; pass < 2; ++pass )
		for( std::size_t k = 0; k < g_batches.size(); ++k ){
			Batch& b = g_batches[k];
			if( b.shader == nullptr || b.verts.empty() )
				continue;
			const bool translucent = Batch_translucent( b, b.shader->getStages() );
			if( translucent != ( pass == 1 ) )
				continue;
			// decals in front of their wall: a little nearer, like polygonOffset / sort banner in game
			gl().glPolygonOffset( -1, translucent ? -4.f : -1.f );
			const Vertex* v = b.verts.data();
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( Vertex ), v->xyz );
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( Vertex ), v->colour );
			gl().glClientActiveTexture( GL_TEXTURE0 );
			gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( Vertex ), v->st );
			gl().glClientActiveTexture( GL_TEXTURE1 );
			gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( Vertex ), v->lm );
			gl().glActiveTexture( GL_TEXTURE1 );
			gl().glBindTexture( GL_TEXTURE_2D, b.lightmap >= 0 ? g_lightmaps[b.lightmap] : 0 );
			gl().glActiveTexture( GL_TEXTURE0 );
			gl().glBindTexture( GL_TEXTURE_2D, Batch_albedoTexture( b ) );
			const unsigned id = unsigned( k + 1 );
			gl().glUniform3f( ud_id, ( id & 255 ) / 255.f, ( ( id >> 8 ) & 255 ) / 255.f, ( ( id >> 16 ) & 255 ) / 255.f );
			gl().glUniform1i( ud_lmd, b.lightmap >= 0 ? 1 : 0 );
			gl().glUniform1i( ud_cut, translucent ? 1 : 0 );
			gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( b.verts.size() ) );
		}
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glDisableClientState( GL_COLOR_ARRAY );
	gl().glDisable( GL_POLYGON_OFFSET_FILL );
	gl().glPolygonOffset( -1, 1 );
	gl().glDepthFunc( GL_LESS );
	gl().glEnable( GL_DITHER );
	gl().glActiveTexture( GL_TEXTURE1 );
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
	gl().glActiveTexture( GL_TEXTURE0 );
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
	gl().glUseProgram( 0 );
	return true;
}

/// Lighting Probe: what a batch id stands for
bool KrBspPreview_batchInfo( int id, CopiedString& shader, bool& lightmapped, bool& translucent, CopiedString& stages ){
	if( id <= 0 || id > int( g_batches.size() ) )
		return false;
	const Batch& b = g_batches[id - 1];
	shader = b.name;
	lightmapped = b.lightmap >= 0;
	const std::vector<KrStage>* st = b.shader != nullptr ? b.shader->getStages() : nullptr;
	translucent = b.shader != nullptr && Batch_translucent( b, st );
	StringOutputStream text( 128 );
	if( st != nullptr )
		for( const KrStage& s : *st ){
			const char* const blend[] = { "ZERO", "ONE", "SRC_COLOR", "1-SRC_COLOR", "SRC_ALPHA", "1-SRC_ALPHA", "DST_COLOR", "1-DST_COLOR", "DST_ALPHA", "1-DST_ALPHA", "SRC_ALPHA_SAT" };
			const char* const rgb[] = { "identity", "vertex", "wave", "const", "oneMinusVertex", "identityLighting" };
			text << "{" << ( s.special == 1 ? "$lightmap" : s.special == 2 ? "$whiteimage" : !s.image.empty() ? s.image.c_str() : !s.frames.empty() ? s.frames.front().c_str() : "?" );
			if( s.blend )
				text << " " << ( s.src < 11 ? blend[s.src] : "?" ) << " " << ( s.dst < 11 ? blend[s.dst] : "?" );
			text << " rgbGen " << ( s.rgbGen >= 0 && s.rgbGen < 6 ? rgb[s.rgbGen] : "?" ) << "}";
		}
	stages = text.c_str();
	return true;
}
bool KrBspPreview_shown(){
	return g_show;
}
const char* KrBspPreview_file(){
	return g_loadedFile.c_str();
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
