/*
   Copyright (c) 2001, Loki software, inc.
   All rights reserved.

   Redistribution and use in source and binary forms, with or without modification,
   are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice, this list
   of conditions and the following disclaimer.

   Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.

   Neither the name of Loki software nor the names of its contributors may be used
   to endorse or promote products derived from this software without specific prior
   written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
   DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//
// Shaders Manager Plugin
//
// Leonardo Zide (leo@lokigames.com)
//

#include "shaders.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <list>
#include <vector>

#include "ifilesystem.h"
#include "ishaders.h"
#include "iscriplib.h"
#include "itextures.h"
#include "qerplugin.h"
#include "irender.h"

#include "debugging/debugging.h"
#include "string/pooledstring.h"
#include "math/vector.h"
#include "generic/callback.h"
#include "generic/referencecounted.h"
#include "stream/memstream.h"
#include "stream/stringstream.h"
#include "stream/textfilestream.h"
#include "os/path.h"
#include "os/dir.h"
#include "os/file.h"
#include "stringio.h"
#include "shaderlib.h"
#include "texturelib.h"
#include "commandlib.h"
#include "moduleobservers.h"
#include "archivelib.h"
#include "imagelib.h"

const char* g_shadersExtension = "";
const char* g_shadersDirectory = "";
bool g_enableDefaultShaders = true;
ShaderLanguage g_shaderLanguage = SHADERLANGUAGE_QUAKE3;
bool g_useShaderList = true;
_QERPlugImageTable* g_bitmapModule = 0;
const char* g_texturePrefix = "textures/";

void ActiveShaders_IteratorBegin();
bool ActiveShaders_IteratorAtEnd();
IShader *ActiveShaders_IteratorCurrent();
void ActiveShaders_IteratorIncrement();
Callback<void()> g_ActiveShadersChangedNotify;

void FreeShaders();
void LoadShaderFile( const char *filename );

/*!
   NOTE TTimo: there is an important distinction between SHADER_NOT_FOUND and SHADER_NOTEX:
   SHADER_NOT_FOUND means we didn't find the raw texture or the shader for this
   SHADER_NOTEX means we recognize this as a shader script, but we are missing the texture to represent it
   this was in the initial design of the shader code since early GtkRadiant alpha, and got sort of foxed in 1.2 and put back in
 */

Image* loadBitmap( void* environment, const char* name ){
	DirectoryArchiveFile file( name, name );
	if ( !file.failed() ) {
		return g_bitmapModule->loadImage( file );
	}
	return 0;
}

inline byte* getPixel( byte* pixels, int width, int height, int x, int y ){
	return pixels + ( ( ( ( ( y + height ) % height ) * width ) + ( ( x + width ) % width ) ) * 4 );
}

class KernelElement
{
public:
	int x, y;
	float w;
};

Image& convertHeightmapToNormalmap( Image& heightmap, float scale ){
	int w = heightmap.getWidth();
	int h = heightmap.getHeight();

	Image& normalmap = *( new RGBAImage( heightmap.getWidth(), heightmap.getHeight() ) );

	byte* in = heightmap.getRGBAPixels();
	byte* out = normalmap.getRGBAPixels();

#if 1
	// no filtering
	const int kernelSize = 2;
	KernelElement kernel_du[kernelSize] = {
		{-1, 0,-0.5f },
		{ 1, 0, 0.5f }
	};
	KernelElement kernel_dv[kernelSize] = {
		{ 0, 1, 0.5f },
		{ 0,-1,-0.5f }
	};
#else
	// 3x3 Prewitt
	const int kernelSize = 6;
	KernelElement kernel_du[kernelSize] = {
		{-1, 1,-1 },
		{-1, 0,-1 },
		{-1,-1,-1 },
		{ 1, 1, 1 },
		{ 1, 0, 1 },
		{ 1,-1, 1 }
	};
	KernelElement kernel_dv[kernelSize] = {
		{-1, 1, 1 },
		{ 0, 1, 1 },
		{ 1, 1, 1 },
		{-1,-1,-1 },
		{ 0,-1,-1 },
		{ 1,-1,-1 }
	};
#endif

	int x, y = 0;
	while ( y < h )
	{
		x = 0;
		while ( x < w )
		{
			float du = 0;
			for ( KernelElement* i = kernel_du; i != kernel_du + kernelSize; ++i )
			{
				du += ( getPixel( in, w, h, x + ( *i ).x, y + ( *i ).y )[0] / 255.0 ) * ( *i ).w;
			}
			float dv = 0;
			for ( KernelElement* i = kernel_dv; i != kernel_dv + kernelSize; ++i )
			{
				dv += ( getPixel( in, w, h, x + ( *i ).x, y + ( *i ).y )[0] / 255.0 ) * ( *i ).w;
			}

			float nx = -du * scale;
			float ny = -dv * scale;
			float nz = 1;

			// Normalize
			float norm = 1.0 / sqrt( nx * nx + ny * ny + nz * nz );
			out[0] = float_to_integer( ( ( nx * norm ) + 1 ) * 127.5 );
			out[1] = float_to_integer( ( ( ny * norm ) + 1 ) * 127.5 );
			out[2] = float_to_integer( ( ( nz * norm ) + 1 ) * 127.5 );
			out[3] = 255;

			x++;
			out += 4;
		}

		y++;
	}

	return normalmap;
}

Image* loadHeightmap( void* environment, const char* name ){
	Image* heightmap = GlobalTexturesCache().loadImage( name );
	if ( heightmap != 0 ) {
		Image& normalmap = convertHeightmapToNormalmap( *heightmap, *reinterpret_cast<float*>( environment ) );
		heightmap->release();
		return &normalmap;
	}
	return 0;
}


Image* loadSpecial( void* environment, const char* name ){
	if ( *name == '_' ) { // special image
		Image* image = loadBitmap( environment, StringStream( GlobalRadiant().getAppPath(), "bitmaps/", name + 1, ".png" ) );
		if ( image != 0 ) {
			return image;
		}
	}
	return GlobalTexturesCache().loadImage( name );
}

class ShaderPoolContext
{
};
typedef Static<StringPool, ShaderPoolContext> ShaderPool;
typedef PooledString<ShaderPool> ShaderString;
typedef ShaderString ShaderVariable;
typedef ShaderString ShaderValue;
typedef CopiedString TextureExpression;

// clean a texture name to the qtexture_t name format we use internally
// NOTE: case sensitivity: the engine is case sensitive. we store the shader name with case information and save with case
// information as well. but we assume there won't be any case conflict and so when doing lookups based on shader name,
// we compare as case insensitive. That is Radiant is case insensitive, but knows that the engine is case sensitive.
//++timo FIXME: we need to put code somewhere to detect when two shaders that are case insensitive equal are present
template<typename StringType>
void parseTextureName( StringType& name, const char* token ){
	name = StringStream<64>( PathCleaned( PathExtensionless( token ) ) ).c_str(); // remove extension
}

bool Tokeniser_parseTextureName( Tokeniser& tokeniser, TextureExpression& name ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#texture-name" );
		return false;
	}
	parseTextureName( name, token );
	return true;
}

bool Tokeniser_parseShaderName( Tokeniser& tokeniser, CopiedString& name ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#shader-name" );
		return false;
	}
	parseTextureName( name, token );
	return true;
}

bool Tokeniser_parseString( Tokeniser& tokeniser, ShaderString& string ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#string" );
		return false;
	}
	string = token;
	return true;
}



typedef std::list<ShaderVariable> ShaderParameters;
typedef std::list<ShaderVariable> ShaderArguments;

typedef std::pair<ShaderVariable, ShaderVariable> BlendFuncExpression;

/// KenRadiant: Quake 3 wave function names -> KrShaderEffects func ids (noise animates like sin)
inline int KrWaveFunc( const char* name ){
	if( name == 0 ) return 0;
	if( string_equal_nocase( name, "sin" ) || string_equal_nocase( name, "noise" ) ) return 1;
	if( string_equal_nocase( name, "triangle" ) ) return 2;
	if( string_equal_nocase( name, "square" ) ) return 3;
	if( string_equal_nocase( name, "sawtooth" ) ) return 4;
	if( string_equal_nocase( name, "inversesawtooth" ) ) return 5;
	return 0;
}

class ShaderTemplate
{
	std::size_t m_refcount;
	CopiedString m_Name;
public:

	ShaderParameters m_params;

	TextureExpression m_textureName;
	TextureExpression m_firstStageImage; // KenRadiant: first map/clampMap/animMap image, editor-image fallback
	TextureExpression m_skyBox;
	TextureExpression m_diffuse;
	TextureExpression m_bump;
	ShaderValue m_heightmapScale;
	TextureExpression m_specular;
	TextureExpression m_lightFalloffImage;

	int m_nFlags;
	float m_fTrans;

// alphafunc stuff
	IShader::EAlphaFunc m_AlphaFunc;
	float m_AlphaRef;
// cull stuff
	IShader::ECull m_Cull;
	// KenRadiant: blendFunc of the first stage of a Quake 3 shader, so translucent/additive/multiply shaders look that way in the editor
	BlendFuncExpression m_q3BlendFunc{ "", "" };
	// KenRadiant: sun of a sky shader, for the live light preview
	bool m_hasSun = false;
	float m_sun[6] = { 1, 1, 1, 0, 0, 90 }; // r g b intensity degrees elevation
	float m_skyLight = 0;
	float m_skyIterations = 3;
	bool m_sunExt = false;
	float m_sunDeviance = 0, m_sunSamples = 16; // q3map_sunExt extras
	bool m_krEmissive = false;
	KrShaderEffects m_krFx;
	std::vector<KrStage> m_krStages; // KenRadiant game look
	float m_krSurfaceLight = 0;
	bool m_krHasLightRGB = false;
	float m_krLightRGB[3] = { 1, 1, 1 };      // q3map_surfacelight (on a sky shader: light from the whole sky, like q3map_skyLight)
	bool m_krLightPass = false, m_krAlphaShadow = false;
	bool m_krHasFog = false;                  // fogparms ( r g b ) <distance>
	float m_krFogColour[3] = { 1, 1, 1 };
	float m_krFogDepth = 0;

	ShaderTemplate() :
		m_refcount( 0 ){
		m_nFlags = 0;
		m_fTrans = 1;
	}

	void IncRef(){
		++m_refcount;
	}
	void DecRef(){
		ASSERT_MESSAGE( m_refcount != 0, "shader reference-count going below zero" );
		if ( --m_refcount == 0 ) {
			delete this;
		}
	}

	std::size_t refcount(){
		return m_refcount;
	}

	const char* getName() const {
		return m_Name.c_str();
	}
	void setName( const char* name ){
		m_Name = name;
		/* KenRadiant: SOF2's sof2map has textures/tools/_terrain built in; the stock tools.shader (Raven, 2002) does
		   not say q3map_terrain, so the terrain layers would only show with an edited shader file */
		if ( string_equal_nocase( name, "textures/tools/_terrain" )
		  && string_equal_nocase( GlobalRadiant().getGameDescriptionKeyValue( "q3map2_type" ), "sof2" ) ) {
			m_nFlags |= QER_KR_TERRAIN;
		}
	}

// -----------------------------------------

	bool parseDoom3( Tokeniser& tokeniser );
	bool parseQuake3( Tokeniser& tokeniser );
	bool parseTemplate( Tokeniser& tokeniser );


	void CreateDefault( const char *name ){
		if ( g_enableDefaultShaders ) {
			m_textureName = name;
		}
		else
		{
			m_textureName = "";
		}
		setName( name );
	}


	class MapLayerTemplate
	{
		TextureExpression m_texture;
		BlendFuncExpression m_blendFunc;
		bool m_clampToBorder;
		ShaderValue m_alphaTest;
	public:
		MapLayerTemplate( const TextureExpression& texture, const BlendFuncExpression& blendFunc, bool clampToBorder, const ShaderValue& alphaTest ) :
			m_texture( texture ),
			m_blendFunc( blendFunc ),
			m_clampToBorder( false ),
			m_alphaTest( alphaTest ){
		}
		const TextureExpression& texture() const {
			return m_texture;
		}
		const BlendFuncExpression& blendFunc() const {
			return m_blendFunc;
		}
		bool clampToBorder() const {
			return m_clampToBorder;
		}
		const ShaderValue& alphaTest() const {
			return m_alphaTest;
		}
	};
	typedef std::vector<MapLayerTemplate> MapLayers;
	MapLayers m_layers;
};


bool Doom3Shader_parseHeightmap( Tokeniser& tokeniser, TextureExpression& bump, ShaderValue& heightmapScale ){
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "(" ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, bump ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "," ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseString( tokeniser, heightmapScale ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
	return true;
}

bool Doom3Shader_parseAddnormals( Tokeniser& tokeniser, TextureExpression& bump ){
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "(" ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, bump ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "," ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "heightmap" ) );
	TextureExpression heightmapName;
	ShaderValue heightmapScale;
	RETURN_FALSE_IF_FAIL( Doom3Shader_parseHeightmap( tokeniser, heightmapName, heightmapScale ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
	return true;
}

bool Doom3Shader_parseBumpmap( Tokeniser& tokeniser, TextureExpression& bump, ShaderValue& heightmapScale ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#bumpmap" );
		return false;
	}
	if ( string_equal( token, "heightmap" ) ) {
		RETURN_FALSE_IF_FAIL( Doom3Shader_parseHeightmap( tokeniser, bump, heightmapScale ) );
	}
	else if ( string_equal( token, "addnormals" ) ) {
		RETURN_FALSE_IF_FAIL( Doom3Shader_parseAddnormals( tokeniser, bump ) );
	}
	else
	{
		parseTextureName( bump, token );
	}
	return true;
}

enum LayerTypeId
{
	LAYER_NONE,
	LAYER_BLEND,
	LAYER_DIFFUSEMAP,
	LAYER_BUMPMAP,
	LAYER_SPECULARMAP
};

class LayerTemplate
{
public:
	LayerTypeId m_type;
	TextureExpression m_texture;
	BlendFuncExpression m_blendFunc;
	bool m_clampToBorder;
	ShaderValue m_alphaTest;
	ShaderValue m_heightmapScale;

	LayerTemplate() : m_type( LAYER_NONE ), m_blendFunc( "GL_ONE", "GL_ZERO" ), m_clampToBorder( false ), m_alphaTest( "-1" ), m_heightmapScale( "0" ){
	}
};

bool parseShaderParameters( Tokeniser& tokeniser, ShaderParameters& params ){
	Tokeniser_parseToken( tokeniser, "(" );
	for (;; )
	{
		const char* param = tokeniser.getToken();
		if ( string_equal( param, ")" ) ) {
			break;
		}
		params.push_back( param );
		const char* comma = tokeniser.getToken();
		if ( string_equal( comma, ")" ) ) {
			break;
		}
		if ( !string_equal( comma, "," ) ) {
			Tokeniser_unexpectedError( tokeniser, comma, "," );
			return false;
		}
	}
	return true;
}

bool ShaderTemplate::parseTemplate( Tokeniser& tokeniser ){
	m_Name = tokeniser.getToken();
	if ( !parseShaderParameters( tokeniser, m_params ) ) {
		globalErrorStream() << "shader template: " << Quoted( m_Name ) << ": parameter parse failed\n";
		return false;
	}

	return parseDoom3( tokeniser );
}

bool ShaderTemplate::parseDoom3( Tokeniser& tokeniser ){
	LayerTemplate currentLayer;
	bool isFog = false;

	// we need to read until we hit a balanced }
	int depth = 0;
	for (;; )
	{
		tokeniser.nextLine();
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			return false;
		}

		if ( string_equal( token, "{" ) ) {
			++depth;
			continue;
		}
		else if ( string_equal( token, "}" ) ) {
			--depth;
			if ( depth < 0 ) { // error
				return false;
			}
			if ( depth == 0 ) { // end of shader
				break;
			}
			if ( depth == 1 ) { // end of layer
				if ( currentLayer.m_type == LAYER_DIFFUSEMAP ) {
					m_diffuse = currentLayer.m_texture;
				}
				else if ( currentLayer.m_type == LAYER_BUMPMAP ) {
					m_bump = currentLayer.m_texture;
				}
				else if ( currentLayer.m_type == LAYER_SPECULARMAP ) {
					m_specular = currentLayer.m_texture;
				}
				else if ( !currentLayer.m_texture.empty() ) {
					m_layers.push_back( MapLayerTemplate(
					                        currentLayer.m_texture.c_str(),
					                        currentLayer.m_blendFunc,
					                        currentLayer.m_clampToBorder,
					                        currentLayer.m_alphaTest
					                    ) );
				}
				currentLayer.m_type = LAYER_NONE;
				currentLayer.m_texture = "";
			}
			continue;
		}

		if ( depth == 2 ) { // in layer
			if ( string_equal_nocase( token, "blend" ) ) {
				const char* blend = tokeniser.getToken();

				if ( blend == 0 ) {
					Tokeniser_unexpectedError( tokeniser, blend, "#blend" );
					return false;
				}

				if ( string_equal_nocase( blend, "diffusemap" ) ) {
					currentLayer.m_type = LAYER_DIFFUSEMAP;
				}
				else if ( string_equal_nocase( blend, "bumpmap" ) ) {
					currentLayer.m_type = LAYER_BUMPMAP;
				}
				else if ( string_equal_nocase( blend, "specularmap" ) ) {
					currentLayer.m_type = LAYER_SPECULARMAP;
				}
				else
				{
					currentLayer.m_blendFunc.first = blend;

					const char* comma = tokeniser.getToken();

					if ( comma == 0 ) {
						Tokeniser_unexpectedError( tokeniser, comma, "#comma" );
						return false;
					}

					if ( string_equal( comma, "," ) ) {
						RETURN_FALSE_IF_FAIL( Tokeniser_parseString( tokeniser, currentLayer.m_blendFunc.second ) );
					}
					else
					{
						currentLayer.m_blendFunc.second = "";
						tokeniser.ungetToken();
					}
				}
			}
			else if ( string_equal_nocase( token, "map" ) ) {
				if ( currentLayer.m_type == LAYER_BUMPMAP ) {
					RETURN_FALSE_IF_FAIL( Doom3Shader_parseBumpmap( tokeniser, currentLayer.m_texture, currentLayer.m_heightmapScale ) );
				}
				else
				{
					const char* map = tokeniser.getToken();

					if ( map == 0 ) {
						Tokeniser_unexpectedError( tokeniser, map, "#map" );
						return false;
					}

					if ( string_equal( map, "makealpha" ) ) {
						RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "(" ) );
						const char* texture = tokeniser.getToken();
						if ( texture == 0 ) {
							Tokeniser_unexpectedError( tokeniser, texture, "#texture" );
							return false;
						}
						currentLayer.m_texture = texture;
						RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
					}
					else
					{
						parseTextureName( currentLayer.m_texture, map );
					}
				}
			}
			else if ( string_equal_nocase( token, "zeroclamp" ) ) {
				currentLayer.m_clampToBorder = true;
			}
#if 0
			else if ( string_equal_nocase( token, "alphaTest" ) ) {
				Tokeniser_getFloat( tokeniser, currentLayer.m_alphaTest );
			}
#endif
		}
		else if ( depth == 1 ) {
			if ( string_equal_nocase( token, "qer_editorimage" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, m_textureName ) );
			}
			else if ( string_equal_nocase( token, "qer_trans" ) ) {
				m_fTrans = string_read_float( tokeniser.getToken() );
				m_nFlags |= QER_TRANS;
			}
			else if ( string_equal_nocase( token, "translucent" ) ) {
				m_fTrans = 1;
				m_nFlags |= QER_TRANS;
			}
			else if ( string_equal( token, "DECAL_MACRO" ) ) {
				m_fTrans = 1;
				m_nFlags |= QER_TRANS;
			}
			else if ( string_equal_nocase( token, "bumpmap" ) ) {
				RETURN_FALSE_IF_FAIL( Doom3Shader_parseBumpmap( tokeniser, m_bump, m_heightmapScale ) );
			}
			else if ( string_equal_nocase( token, "diffusemap" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, m_diffuse ) );
			}
			else if ( string_equal_nocase( token, "specularmap" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, m_specular ) );
			}
			else if ( string_equal_nocase( token, "twosided" ) ) {
				m_Cull = IShader::eCullNone;
				m_nFlags |= QER_CULL;
			}
			else if ( string_equal_nocase( token, "nodraw" ) ) {
				m_nFlags |= QER_NODRAW;
			}
			else if ( string_equal_nocase( token, "nonsolid" ) ) {
				m_nFlags |= QER_NONSOLID;
			}
			else if ( string_equal_nocase( token, "liquid" ) ) {
				m_nFlags |= QER_LIQUID;
			}
			else if ( string_equal_nocase( token, "areaportal" ) ) {
				m_nFlags |= QER_AREAPORTAL;
			}
			else if ( string_equal_nocase( token, "playerclip" )
			       || string_equal_nocase( token, "monsterclip" )
			       || string_equal_nocase( token, "ikclip" )
			       || string_equal_nocase( token, "moveableclip" ) ) {
				m_nFlags |= QER_CLIP;
			}
			if ( string_equal_nocase( token, "fogLight" ) ) {
				isFog = true;
			}
			else if ( !isFog && string_equal_nocase( token, "lightFalloffImage" ) ) {
				const char* lightFalloffImage = tokeniser.getToken();
				if ( lightFalloffImage == 0 ) {
					Tokeniser_unexpectedError( tokeniser, lightFalloffImage, "#lightFalloffImage" );
					return false;
				}
				if ( string_equal_nocase( lightFalloffImage, "makeintensity" ) ) {
					RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "(" ) );
					TextureExpression name;
					RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, name ) );
					m_lightFalloffImage = name;
					RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
				}
				else
				{
					m_lightFalloffImage = lightFalloffImage;
				}
			}
		}
	}

	if ( m_textureName.empty() ) {
		m_textureName = m_diffuse;
	}

	return true;
}

typedef SmartPointer<ShaderTemplate> ShaderTemplatePointer;
typedef std::map<CopiedString, ShaderTemplatePointer> ShaderTemplateMap;

ShaderTemplateMap g_shaders;
ShaderTemplateMap g_shaderTemplates;

ShaderTemplate* findTemplate( const char* name ){
	ShaderTemplateMap::iterator i = g_shaderTemplates.find( name );
	if ( i != g_shaderTemplates.end() ) {
		return ( *i ).second.get();
	}
	return 0;
}

class ShaderDefinition
{
public:
	ShaderDefinition( ShaderTemplate* shaderTemplate, const ShaderArguments& args, const char* filename )
		: shaderTemplate( shaderTemplate ), args( args ), filename( filename ){
	}
	ShaderTemplate* shaderTemplate;
	ShaderArguments args;
	const char* filename;
};

typedef std::map<CopiedString, ShaderDefinition, shader_less_t> ShaderDefinitionMap;

ShaderDefinitionMap g_shaderDefinitions;

bool parseTemplateInstance( Tokeniser& tokeniser, const char* filename ){
	CopiedString name;
	RETURN_FALSE_IF_FAIL( Tokeniser_parseShaderName( tokeniser, name ) );
	const char* templateName = tokeniser.getToken();
	ShaderTemplate* shaderTemplate = findTemplate( templateName );
	if ( shaderTemplate == 0 ) {
		globalErrorStream() << "shader instance: " << Quoted( name ) << ": shader template not found: " << Quoted( templateName ) << '\n';
	}

	ShaderArguments args;
	if ( !parseShaderParameters( tokeniser, args ) ) {
		globalErrorStream() << "shader instance: " << Quoted( name ) << ": argument parse failed\n";
		return false;
	}

	if ( shaderTemplate != 0 ) {
		if ( !g_shaderDefinitions.insert( ShaderDefinitionMap::value_type( name, ShaderDefinition( shaderTemplate, args, filename ) ) ).second ) {
			globalErrorStream() << "shader instance: " << Quoted( name ) << ": already exists, second definition ignored\n";
		}
	}
	return true;
}


const char* evaluateShaderValue( const char* value, const ShaderParameters& params, const ShaderArguments& args ){
	ShaderArguments::const_iterator j = args.begin();
	for ( ShaderParameters::const_iterator i = params.begin(); i != params.end(); ++i, ++j )
	{
		const char* other = ( *i ).c_str();
		if ( string_equal( value, other ) ) {
			return ( *j ).c_str();
		}
	}
	return value;
}

///\todo BlendFunc parsing
BlendFunc evaluateBlendFunc( const BlendFuncExpression& blendFunc, const ShaderParameters& params, const ShaderArguments& args ){
	return BlendFunc( BLEND_ONE, BLEND_ZERO );
}

qtexture_t* evaluateTexture( const TextureExpression& texture, const ShaderParameters& params, const ShaderArguments& args, const LoadImageCallback& loader = GlobalTexturesCache().defaultLoader() ){
	StringOutputStream result( 64 );
	const char* expression = texture.c_str();
	const char* end = expression + string_length( expression );
	if ( !string_empty( expression ) ) {
		for (;; )
		{
			const char* best = end;
			const char* bestParam = 0;
			const char* bestArg = 0;
			ShaderArguments::const_iterator j = args.begin();
			for ( ShaderParameters::const_iterator i = params.begin(); i != params.end(); ++i, ++j )
			{
				const char* found = strstr( expression, ( *i ).c_str() );
				if ( found != 0 && found < best ) {
					best = found;
					bestParam = ( *i ).c_str();
					bestArg = ( *j ).c_str();
				}
			}
			if ( best != end ) {
				result << StringRange( expression, best );
				result << PathCleaned( bestArg );
				expression = best + string_length( bestParam );
			}
			else
			{
				break;
			}
		}
		result << expression;
	}
	return GlobalTexturesCache().capture( loader, result );
}

float evaluateFloat( const ShaderValue& value, const ShaderParameters& params, const ShaderArguments& args ){
	const char* result = evaluateShaderValue( value.c_str(), params, args );
	float f;
	if ( !string_parse_float( result, f ) ) {
		globalErrorStream() << "parsing float value failed: " << Quoted( result ) << '\n';
		return 1;
	}
	return f;
}

BlendFactor evaluateBlendFactor( const ShaderValue& value, const ShaderParameters& params, const ShaderArguments& args ){
	const char* result = evaluateShaderValue( value.c_str(), params, args );

	if ( string_equal_nocase( result, "gl_zero" ) ) {
		return BLEND_ZERO;
	}
	if ( string_equal_nocase( result, "gl_one" ) ) {
		return BLEND_ONE;
	}
	if ( string_equal_nocase( result, "gl_src_color" ) ) {
		return BLEND_SRC_COLOUR;
	}
	if ( string_equal_nocase( result, "gl_one_minus_src_color" ) ) {
		return BLEND_ONE_MINUS_SRC_COLOUR;
	}
	if ( string_equal_nocase( result, "gl_src_alpha" ) ) {
		return BLEND_SRC_ALPHA;
	}
	if ( string_equal_nocase( result, "gl_one_minus_src_alpha" ) ) {
		return BLEND_ONE_MINUS_SRC_ALPHA;
	}
	if ( string_equal_nocase( result, "gl_dst_color" ) ) {
		return BLEND_DST_COLOUR;
	}
	if ( string_equal_nocase( result, "gl_one_minus_dst_color" ) ) {
		return BLEND_ONE_MINUS_DST_COLOUR;
	}
	if ( string_equal_nocase( result, "gl_dst_alpha" ) ) {
		return BLEND_DST_ALPHA;
	}
	if ( string_equal_nocase( result, "gl_one_minus_dst_alpha" ) ) {
		return BLEND_ONE_MINUS_DST_ALPHA;
	}
	if ( string_equal_nocase( result, "gl_src_alpha_saturate" ) ) {
		return BLEND_SRC_ALPHA_SATURATE;
	}

	globalErrorStream() << "parsing blend-factor value failed: " << Quoted( result ) << '\n';
	return BLEND_ZERO;
}

class CShader final : public IShader
{
	std::size_t m_refcount;

	const ShaderTemplate& m_template;
	const ShaderArguments& m_args;
	const char* m_filename;
// name is shader-name, otherwise texture-name (if not a real shader)
	CopiedString m_Name;

	qtexture_t* m_pTexture;
	qtexture_t* m_pSkyBox;
	qtexture_t* m_notfound;
	qtexture_t* m_pDiffuse;
	float m_heightmapScale;
	qtexture_t* m_pBump;
	qtexture_t* m_pSpecular;
	qtexture_t* m_pLightFalloffImage;
	BlendFunc m_blendFunc;

	bool m_bInUse;


public:
	static bool m_lightingEnabled;

	CShader( const ShaderDefinition& definition ) :
		m_refcount( 0 ),
		m_template( *definition.shaderTemplate ),
		m_args( definition.args ),
		m_filename( definition.filename ),
		m_blendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA ),
		m_bInUse( false ){
		m_pTexture = 0;
		m_pSkyBox = 0;
		m_pDiffuse = 0;
		m_pBump = 0;
		m_pSpecular = 0;

		m_notfound = 0;

		applyQ3BlendFunc();
		realise();
	}
	void applyQ3BlendFunc(){
		const BlendFuncExpression& blendFunc = m_template.m_q3BlendFunc;
		if( string_empty( blendFunc.first.c_str() ) )
			return;
		if ( !string_empty( blendFunc.second.c_str() ) ) {
			m_blendFunc = BlendFunc( evaluateBlendFactor( blendFunc.first, m_template.m_params, m_args ),
			                         evaluateBlendFactor( blendFunc.second, m_template.m_params, m_args ) );
		}
		else if ( string_equal_nocase( blendFunc.first.c_str(), "add" ) ) {
			m_blendFunc = BlendFunc( BLEND_ONE, BLEND_ONE );
		}
		else if ( string_equal_nocase( blendFunc.first.c_str(), "filter" ) ) {
			m_blendFunc = BlendFunc( BLEND_DST_COLOUR, BLEND_ZERO );
		}
		else {
			m_blendFunc = BlendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA );
		}
	}
	~CShader(){
		unrealise();

		ASSERT_MESSAGE( m_refcount == 0, "deleting active shader" );
	}

	bool getSunExt( bool& ext, float& deviance, float& samples, float& skyIterations ) const override {
		ext = m_template.m_sunExt;
		deviance = m_template.m_sunDeviance;
		samples = m_template.m_sunSamples;
		skyIterations = m_template.m_skyIterations;
		return m_template.m_hasSun || m_template.m_skyLight != 0;
	}
	bool getSun( float colour[3], float& intensity, float& degrees, float& elevation, float& skyLight ) const override {
		if( !m_template.m_hasSun && m_template.m_skyLight == 0 )
			return false;
		colour[0] = m_template.m_sun[0]; colour[1] = m_template.m_sun[1]; colour[2] = m_template.m_sun[2];
		intensity = m_template.m_hasSun ? m_template.m_sun[3] : 0;
		degrees = m_template.m_sun[4];
		elevation = m_template.m_sun[5];
		skyLight = m_template.m_skyLight;
		return true;
	}
	bool krNotFound() const override {
		return m_notfound != 0;
	}
	bool isEmissive() const override {
		return m_template.m_krEmissive;
	}
	const KrShaderEffects* getEffects() const override {
		return m_template.m_krFx.any() ? &m_template.m_krFx : nullptr;
	}
	float krSurfaceLight() const override {
		return m_template.m_krSurfaceLight;
	}
	bool krLightRGB( float colour[3] ) const override {
		if( !m_template.m_krHasLightRGB )
			return false;
		std::copy( m_template.m_krLightRGB, m_template.m_krLightRGB + 3, colour );
		return true;
	}
	bool krFogParms( float colour[3], float& depth ) const override {
		if( !m_template.m_krHasFog )
			return false;
		std::copy( m_template.m_krFogColour, m_template.m_krFogColour + 3, colour );
		depth = m_template.m_krFogDepth;
		return true;
	}
	const std::vector<KrStage>* getStages() const override {
		return m_template.m_krStages.empty() ? nullptr : &m_template.m_krStages;
	}
// IShaders implementation -----------------
	void IncRef() override {
		++m_refcount;
	}
	void DecRef() override {
		ASSERT_MESSAGE( m_refcount != 0, "shader reference-count going below zero" );
		if ( --m_refcount == 0 ) {
			delete this;
		}
	}

	std::size_t refcount(){
		return m_refcount;
	}

// get/set the qtexture_t* Radiant uses to represent this shader object
	qtexture_t* getTexture() const override {
		return m_pTexture;
	}
	qtexture_t* getSkyBox() override {
		/* load skybox if only used */
		if( m_pSkyBox == nullptr && !m_template.m_skyBox.empty() )
			m_pSkyBox = GlobalTexturesCache().capture( LoadImageCallback( 0, GlobalTexturesCache().defaultLoader().m_func, true ), m_template.m_skyBox.c_str() );

		return m_pSkyBox;
	}
	qtexture_t* getDiffuse() const override {
		return m_pDiffuse;
	}
	qtexture_t* getBump() const override {
		return m_pBump;
	}
	qtexture_t* getSpecular() const override {
		return m_pSpecular;
	}
// get shader name
	const char* getName() const override {
		return m_Name.c_str();
	}
	bool IsInUse() const override {
		return m_bInUse;
	}
	void SetInUse( bool bInUse ) override {
		m_bInUse = bInUse;
		g_ActiveShadersChangedNotify();
	}
// get the shader flags
	int getFlags() const override {
		return m_template.m_nFlags;
	}
// get the transparency value
	float getTrans() const override {
		return m_template.m_fTrans;
	}
// test if it's a true shader, or a default shader created to wrap around a texture
	bool IsDefault() const override {
		return string_empty( m_filename );
	}
// get the alphaFunc
	void getAlphaFunc( EAlphaFunc *func, float *ref ) override {
		*func = m_template.m_AlphaFunc;
		*ref = m_template.m_AlphaRef;
	};
	BlendFunc getBlendFunc() const override {
		return m_blendFunc;
	}
// get the cull type
	ECull getCull() override {
		return m_template.m_Cull;
	};
// get shader file name (ie the file where this one is defined)
	const char* getShaderFileName() const override {
		return m_filename;
	}
// -----------------------------------------

	void realise(){
		m_pTexture = evaluateTexture( m_template.m_textureName, m_template.m_params, m_args );

		/* KenRadiant: the editor image didn't load (missing qer_editorimage target, case mismatch in a pk3, ...):
		   try what q3map2 / the game would show before giving up */
		if ( m_pTexture->texture_number == 0 && !IsDefault() ) {
			std::vector<CopiedString> candidates;
			const auto add = [&candidates]( const char* name ){
				if ( !string_empty( name ) ) {
					candidates.emplace_back( name );
					CopiedString lower( name );
					string_to_lowercase( const_cast<char*>( lower.c_str() ) );
					if ( !string_equal( lower.c_str(), name ) )
						candidates.emplace_back( lower );
				}
			};
			add( m_template.m_firstStageImage.c_str() );
			add( m_template.getName() );
			if ( !m_template.m_skyBox.empty() )
				add( StringStream<64>( m_template.m_skyBox, "_ft" ) );
			add( m_template.m_textureName.c_str() ); // lower-case retry of the original
			for ( const CopiedString& name : candidates ) {
				if ( string_equal( name.c_str(), m_template.m_textureName.c_str() ) )
					continue;
				qtexture_t* tex = evaluateTexture( TextureExpression( name.c_str() ), m_template.m_params, m_args );
				if ( tex->texture_number != 0 ) {
					GlobalTexturesCache().release( m_pTexture );
					m_pTexture = tex;
					break;
				}
				GlobalTexturesCache().release( tex );
			}
		}

		if ( m_pTexture->texture_number == 0 ) {
			m_notfound = m_pTexture;

			const auto name = StringStream( GlobalRadiant().getAppPath(), "bitmaps/",
				( string_equal( m_template.getName(), "nomodel" )? "nomodel.png"
				: IsDefault() ? "notex.png"
				: "shadernotex.png" ) );
			m_pTexture = GlobalTexturesCache().capture( LoadImageCallback( 0, loadBitmap ), name );
		}

		realiseLighting();
	}

	void unrealise(){
		GlobalTexturesCache().release( m_pTexture );

		if ( m_notfound != 0 ) {
			GlobalTexturesCache().release( m_notfound );
		}

		if ( m_pSkyBox != 0 ) {
			GlobalTexturesCache().release( m_pSkyBox );
		}

		unrealiseLighting();
	}

	void realiseLighting(){
		if ( m_lightingEnabled ) {
			LoadImageCallback loader = GlobalTexturesCache().defaultLoader();
			if ( !string_empty( m_template.m_heightmapScale.c_str() ) ) {
				m_heightmapScale = evaluateFloat( m_template.m_heightmapScale, m_template.m_params, m_args );
				loader = LoadImageCallback( &m_heightmapScale, loadHeightmap );
			}
			m_pDiffuse = evaluateTexture( m_template.m_diffuse, m_template.m_params, m_args );
			m_pBump = evaluateTexture( m_template.m_bump, m_template.m_params, m_args, loader );
			m_pSpecular = evaluateTexture( m_template.m_specular, m_template.m_params, m_args );
			m_pLightFalloffImage = evaluateTexture( m_template.m_lightFalloffImage, m_template.m_params, m_args );

			for ( const auto& layer : m_template.m_layers )
			{
				m_layers.push_back( evaluateLayer( layer, m_template.m_params, m_args ) );
			}

			if ( m_layers.size() == 1 ) {
				const BlendFuncExpression& blendFunc = m_template.m_layers.front().blendFunc();
				if ( !string_empty( blendFunc.second.c_str() ) ) {
					m_blendFunc = BlendFunc(
					                  evaluateBlendFactor( blendFunc.first.c_str(), m_template.m_params, m_args ),
					                  evaluateBlendFactor( blendFunc.second.c_str(), m_template.m_params, m_args )
					              );
				}
				else
				{
					const char* blend = evaluateShaderValue( blendFunc.first.c_str(), m_template.m_params, m_args );

					if ( string_equal_nocase( blend, "add" ) ) {
						m_blendFunc = BlendFunc( BLEND_ONE, BLEND_ONE );
					}
					else if ( string_equal_nocase( blend, "filter" ) ) {
						m_blendFunc = BlendFunc( BLEND_DST_COLOUR, BLEND_ZERO );
					}
					else if ( string_equal_nocase( blend, "blend" ) ) {
						m_blendFunc = BlendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA );
					}
					else
					{
						globalErrorStream() << "parsing blend value failed: " << Quoted( blend ) << '\n';
					}
				}
			}
		}
	}

	void unrealiseLighting(){
		if ( m_lightingEnabled ) {
			GlobalTexturesCache().release( m_pDiffuse );
			GlobalTexturesCache().release( m_pBump );
			GlobalTexturesCache().release( m_pSpecular );

			GlobalTexturesCache().release( m_pLightFalloffImage );

			for ( auto& layer : m_layers )
			{
				GlobalTexturesCache().release( layer.texture() );
			}
			m_layers.clear();

			m_blendFunc = BlendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA );
			applyQ3BlendFunc();
		}
	}

// set shader name
	void setName( const char* name ){
		m_Name = name;
	}

	class MapLayer final : public ShaderLayer
	{
		qtexture_t* m_texture;
		BlendFunc m_blendFunc;
		bool m_clampToBorder;
		float m_alphaTest;
	public:
		MapLayer( qtexture_t* texture, BlendFunc blendFunc, bool clampToBorder, float alphaTest ) :
			m_texture( texture ),
			m_blendFunc( blendFunc ),
			m_clampToBorder( false ),
			m_alphaTest( alphaTest ){
		}
		qtexture_t* texture() const override {
			return m_texture;
		}
		BlendFunc blendFunc() const override {
			return m_blendFunc;
		}
		bool clampToBorder() const override {
			return m_clampToBorder;
		}
		float alphaTest() const override {
			return m_alphaTest;
		}
	};

	static MapLayer evaluateLayer( const ShaderTemplate::MapLayerTemplate& layerTemplate, const ShaderParameters& params, const ShaderArguments& args ){
		return MapLayer(
		           evaluateTexture( layerTemplate.texture(), params, args ),
		           evaluateBlendFunc( layerTemplate.blendFunc(), params, args ),
		           layerTemplate.clampToBorder(),
		           evaluateFloat( layerTemplate.alphaTest(), params, args )
		       );
	}

	typedef std::vector<MapLayer> MapLayers;
	MapLayers m_layers;

	const ShaderLayer* firstLayer() const override {
		if ( m_layers.empty() ) {
			return 0;
		}
		return &m_layers.front();
	}
	void forEachLayer( const ShaderLayerCallback& callback ) const override {
		for ( const auto& layer : m_layers )
		{
			callback( layer );
		}
	}

	qtexture_t* lightFalloffImage() const override {
		if ( !m_template.m_lightFalloffImage.empty() ) {
			return m_pLightFalloffImage;
		}
		return 0;
	}
};

bool CShader::m_lightingEnabled = false;

typedef SmartPointer<CShader> ShaderPointer;
typedef std::map<CopiedString, ShaderPointer, shader_less_t> shaders_t;

shaders_t g_ActiveShaders;

static shaders_t::iterator g_ActiveShadersIterator;

void ActiveShaders_IteratorBegin(){
	g_ActiveShadersIterator = g_ActiveShaders.begin();
}

bool ActiveShaders_IteratorAtEnd(){
	return g_ActiveShadersIterator == g_ActiveShaders.end();
}

IShader *ActiveShaders_IteratorCurrent(){
	return static_cast<CShader*>( g_ActiveShadersIterator->second );
}

void ActiveShaders_IteratorIncrement(){
	++g_ActiveShadersIterator;
}

void debug_check_shaders( shaders_t& shaders ){
	for ( const auto& [ name, shader ] : shaders )
	{
		ASSERT_MESSAGE( shader->refcount() == 1, "orphan shader still referenced" );
	}
}

// will free all GL binded qtextures and shaders
// NOTE: doesn't make much sense out of Radiant exit or called during a reload
void FreeShaders(){
	// reload shaders
	// empty the actives shaders list
	debug_check_shaders( g_ActiveShaders );
	g_ActiveShaders.clear();
	g_shaders.clear();
	g_shaderTemplates.clear();
	g_shaderDefinitions.clear();
	g_ActiveShadersChangedNotify();
}

bool ShaderTemplate::parseQuake3( Tokeniser& tokeniser ){
	// name of the qtexture_t we'll use to represent this shader (this one has the "textures\" before)
	m_textureName = m_Name;
	bool hasEditorImage = false;
	CopiedString firstStageImage; // like q3map2: without qer_editorimage, show the first stage's image
	// KenRadiant: what the game does with the first stages - masked (alphaFunc) / translucent (blendFunc) preview
	int stageNum = 0;
	bool stageHasImage = false, imageStageDone = false;
	CopiedString stageAlphaFunc;
	// KenRadiant: emissive / animated preview
	bool stageAdditive = false, stageVertexLit = false, stageLightmap = false, stageGlow = false;
	KrShaderEffects stageFx; // tcMods of the current stage
	KrStage stage;           // the current stage as the game draws it
	m_krStages.clear();
	const auto blendFactor = []( const char* f ) -> BlendFactor {
		if( string_equal_nocase( f, "gl_zero" ) ) return BLEND_ZERO;
		if( string_equal_nocase( f, "gl_one" ) ) return BLEND_ONE;
		if( string_equal_nocase( f, "gl_src_color" ) ) return BLEND_SRC_COLOUR;
		if( string_equal_nocase( f, "gl_one_minus_src_color" ) ) return BLEND_ONE_MINUS_SRC_COLOUR;
		if( string_equal_nocase( f, "gl_src_alpha" ) ) return BLEND_SRC_ALPHA;
		if( string_equal_nocase( f, "gl_one_minus_src_alpha" ) ) return BLEND_ONE_MINUS_SRC_ALPHA;
		if( string_equal_nocase( f, "gl_dst_color" ) ) return BLEND_DST_COLOUR;
		if( string_equal_nocase( f, "gl_one_minus_dst_color" ) ) return BLEND_ONE_MINUS_DST_COLOUR;
		if( string_equal_nocase( f, "gl_dst_alpha" ) ) return BLEND_DST_ALPHA;
		if( string_equal_nocase( f, "gl_one_minus_dst_alpha" ) ) return BLEND_ONE_MINUS_DST_ALPHA;
		if( string_equal_nocase( f, "gl_src_alpha_saturate" ) ) return BLEND_SRC_ALPHA_SATURATE;
		return BLEND_ONE;
	};

	tokeniser.nextLine();

	// we need to read until we hit a balanced }
	int depth = 0;
	for (;; )
	{
		tokeniser.nextLine();
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			return false;
		}

		if ( string_equal( token, "{" ) ) {
			++depth;
			if( depth == 2 ){
				++stageNum;
				stageHasImage = false;
				stageAlphaFunc = "";
				stageAdditive = stageVertexLit = stageLightmap = stageGlow = false;
				stageFx = KrShaderEffects(); // tcMods are per stage
				stage = KrStage();
			}
			continue;
		}
		else if ( string_equal( token, "}" ) ) {
			if( depth == 2 ){
				// an extra additive stage that is not vertex lit (the "2x" overbright copy is) = glow, or SOF2's glow keyword
				if( ( stageGlow || ( stageAdditive && stageNum > 1 && !stageVertexLit ) ) && !stageLightmap )
					m_krEmissive = true;
				stage.fx = stageFx;
				if( !stage.image.empty() || stage.special != 0 || !stage.frames.empty() )
					m_krStages.push_back( stage );
				if( stageHasImage && !imageStageDone ){
					std::copy( std::begin( stageFx.scroll ), std::end( stageFx.scroll ), std::begin( m_krFx.scroll ) );
					m_krFx.rotate = stageFx.rotate;
					std::copy( std::begin( stageFx.scale ), std::end( stageFx.scale ), std::begin( m_krFx.scale ) );
					m_krFx.stretchFunc = stageFx.stretchFunc;
					std::copy( std::begin( stageFx.stretch ), std::end( stageFx.stretch ), std::begin( m_krFx.stretch ) );
					m_krFx.turb = stageFx.turb;
					std::copy( std::begin( stageFx.turbWave ), std::end( stageFx.turbWave ), std::begin( m_krFx.turbWave ) );
				}
			}
			if( depth == 2 && stageHasImage && !imageStageDone ){
				imageStageDone = true;
				if( !stageAlphaFunc.empty() && !( m_nFlags & QER_ALPHATEST ) ){
					if( string_equal_nocase( stageAlphaFunc.c_str(), "GT0" ) ){
						m_AlphaFunc = IShader::eGreater; m_AlphaRef = 0;
					}
					else if( string_equal_nocase( stageAlphaFunc.c_str(), "LT128" ) ){
						m_AlphaFunc = IShader::eLess; m_AlphaRef = 0.5f;
					}
					else{ // GE128
						m_AlphaFunc = IShader::eGEqual; m_AlphaRef = 0.5f;
					}
					m_nFlags |= QER_ALPHATEST;
				}
			}
			--depth;
			if ( depth < 0 ) { // underflow
				return false;
			}
			if ( depth == 0 ) { // end of shader
				if( m_skyLight == 0 && m_krSurfaceLight > 0 && ( m_nFlags & QER_SKY ) ){ // SOF2 skies: surfacelight on the sky lights the map
					m_skyLight = m_krSurfaceLight * 2;
					m_skyIterations = 3;
				}
				for( KrStage& st : m_krStages ){ // the shader's vertex deforms move every stage
					st.fx.deform = m_krFx.deform; st.fx.deformFunc = m_krFx.deformFunc; st.fx.deformDiv = m_krFx.deformDiv;
					std::copy( std::begin( m_krFx.deformWave ), std::end( m_krFx.deformWave ), std::begin( st.fx.deformWave ) );
					std::copy( std::begin( m_krFx.deformMove ), std::end( m_krFx.deformMove ), std::begin( st.fx.deformMove ) );
					std::copy( std::begin( m_krFx.bulge ), std::end( m_krFx.bulge ), std::begin( st.fx.bulge ) );
					st.fx.polygonOffset = m_krFx.polygonOffset;
				}
				break;
			}

			continue;
		}

		if ( depth == 2 ) {
			if ( string_equal_nocase( token, "map" ) || string_equal_nocase( token, "clampmap" ) || string_equal_nocase( token, "animmap" ) ) {
				if ( string_equal_nocase( token, "animmap" ) ) {
					const std::size_t line = tokeniser.getLine();
					Tokeniser_getFloat( tokeniser, stage.animFreq );
					// the frames: the rest of the line
					for( const char* frame; ( frame = tokeniser.getToken() ) != 0; ){
						if( tokeniser.getLine() != line ){
							tokeniser.ungetToken();
							break;
						}
						if( stage.frames.size() < 16 )
							stage.frames.emplace_back( StringStream<64>( PathExtensionless( frame ) ) );
					}
					if( !stage.frames.empty() ){
						stageHasImage = true;
						if( firstStageImage.empty() )
							firstStageImage = stage.frames.front();
						stage.image = stage.frames.front();
					}
					continue;
				}
				if ( const char* image = tokeniser.getToken(); image != 0 && image[0] != '$' && image[0] != '*' ) {
					stageHasImage = true;
					stage.image = StringStream<64>( PathExtensionless( image ) );
					if( firstStageImage.empty() )
						firstStageImage = StringStream<64>( PathExtensionless( image ) );
				}
				else if( image != 0 && string_equal_nocase( image, "$lightmap" ) ){
					stageLightmap = true;
					stage.special = 1;
				}
				else if( image != 0 && ( string_equal_nocase( image, "$whiteimage" ) || string_equal_nocase( image, "*white" ) ) ){
					stage.special = 2;
				}
				continue;
			}
			else if ( string_equal_nocase( token, "rgbgen" ) ) {
				if( const char* gen = tokeniser.getToken() ){
					// vertex, exactVertex, lightingDiffuse, lightingDiffuseEntity (JK2 / SOF2) ...: lit by the game
					stageVertexLit = string_equal_nocase( gen, "vertex" ) || string_equal_nocase( gen, "exactVertex" ) || string_equal_nocase_n( gen, "lightingDiffuse", 15 );
					if( stageVertexLit )
						stage.rgbGen = 1;
					else if( string_equal_nocase( gen, "wave" ) ){
						stage.rgbGen = 2;
						stage.rgbFunc = KrWaveFunc( tokeniser.getToken() );
						for( float& v : stage.rgbWave )
							Tokeniser_getFloat( tokeniser, v );
					}
					else if( string_equal_nocase( gen, "const" ) || string_equal_nocase( gen, "constant" ) ){
						stage.rgbGen = 3;
						const char* t = tokeniser.getToken(); // ( r g b )
						if( t != 0 && !string_equal( t, "(" ) )
							tokeniser.ungetToken();
						for( float& v : stage.rgbConst )
							Tokeniser_getFloat( tokeniser, v );
						t = tokeniser.getToken();
						if( t != 0 && !string_equal( t, ")" ) )
							tokeniser.ungetToken();
					}
					else if( string_equal_nocase( gen, "oneMinusVertex" ) )
						stage.rgbGen = 4;
					else if( string_equal_nocase( gen, "identityLighting" ) )
						stage.rgbGen = 5;
				}
				continue;
			}
			else if ( string_equal_nocase( token, "glow" ) ) {
				stageGlow = true;
				stage.glow = true;
				continue;
			}
			else if ( string_equal_nocase( token, "depthWrite" ) ) {
				if( stageNum == 1 )
					m_krFx.depthWrite = true;
				stage.depthWrite = true;
				continue;
			}
			else if ( string_equal_nocase( token, "alphagen" ) ) {
				if( const char* gen = tokeniser.getToken() ){
					if( string_equal_nocase( gen, "const" ) || string_equal_nocase( gen, "constant" ) ){
						stage.alphaGen = 1;
						Tokeniser_getFloat( tokeniser, stage.alphaConst );
					}
					else if( string_equal_nocase( gen, "vertex" ) )
						stage.alphaGen = 3; // vertex alpha (terrain blends, q3map_alphaMod ...)
					else if( string_equal_nocase( gen, "wave" ) ){
						stage.alphaGen = 2;
						stage.alphaFunc2 = KrWaveFunc( tokeniser.getToken() );
						for( float& v : stage.alphaWave )
							Tokeniser_getFloat( tokeniser, v );
					}
				}
				continue;
			}
			else if ( string_equal_nocase( token, "tcgen" ) || string_equal_nocase( token, "texgen" ) ) {
				if( const char* gen = tokeniser.getToken() ){
					if( string_equal_nocase( gen, "environment" ) )
						stage.tcGen = 2;
					else if( string_equal_nocase( gen, "lightmap" ) )
						stage.tcGen = 1;
				}
				continue;
			}
			else if ( string_equal_nocase( token, "tcmod" ) ) {
				const char* kind = tokeniser.getToken();
				if( kind != 0 && string_equal_nocase( kind, "scroll" ) ){
					Tokeniser_getFloat( tokeniser, stageFx.scroll[0] );
					Tokeniser_getFloat( tokeniser, stageFx.scroll[1] );
				}
				else if( kind != 0 && string_equal_nocase( kind, "rotate" ) ){
					Tokeniser_getFloat( tokeniser, stageFx.rotate );
				}
				else if( kind != 0 && string_equal_nocase( kind, "scale" ) ){
					Tokeniser_getFloat( tokeniser, stageFx.scale[0] );
					Tokeniser_getFloat( tokeniser, stageFx.scale[1] );
				}
				else if( kind != 0 && string_equal_nocase( kind, "stretch" ) ){
					stageFx.stretchFunc = KrWaveFunc( tokeniser.getToken() );
					for( float& v : stageFx.stretch )
						Tokeniser_getFloat( tokeniser, v );
				}
				else if( kind != 0 && string_equal_nocase( kind, "turb" ) ){
					stageFx.turb = true;
					for( float& v : stageFx.turbWave )
						Tokeniser_getFloat( tokeniser, v );
				}
				continue;
			}
			else if ( string_equal_nocase( token, "alphafunc" ) ) {
				if( const char* func = tokeniser.getToken() ){
					stageAlphaFunc = func;
					stage.alphaFunc = string_equal_nocase( func, "GT0" ) ? 1 : string_equal_nocase( func, "LT128" ) ? 2 : 3;
				}
				continue;
			}
			else if ( string_equal_nocase( token, "blendfunc" ) ) {
				const char* a = tokeniser.getToken();
				if( a == 0 )
					return false;
				const CopiedString first( a );
				CopiedString second;
				if( !string_equal_nocase( a, "add" ) && !string_equal_nocase( a, "blend" ) && !string_equal_nocase( a, "filter" ) ){
					const char* b = tokeniser.getToken();
					if( b == 0 )
						return false;
					second = b;
				}
				if( stageNum == 1 )
					m_q3BlendFunc = BlendFuncExpression( first.c_str(), second.c_str() );
				stage.blend = true;
				if( string_equal_nocase( first.c_str(), "add" ) ){ stage.src = BLEND_ONE; stage.dst = BLEND_ONE; }
				else if( string_equal_nocase( first.c_str(), "filter" ) ){ stage.src = BLEND_DST_COLOUR; stage.dst = BLEND_ZERO; }
				else if( string_equal_nocase( first.c_str(), "blend" ) ){ stage.src = BLEND_SRC_ALPHA; stage.dst = BLEND_ONE_MINUS_SRC_ALPHA; }
				else{ stage.src = blendFactor( first.c_str() ); stage.dst = blendFactor( second.c_str() ); }
				stageAdditive = string_equal_nocase( first.c_str(), "add" )
				             || ( string_equal_nocase( second.c_str(), "gl_one" ) && ( string_equal_nocase( first.c_str(), "gl_one" ) || string_equal_nocase( first.c_str(), "gl_src_alpha" ) ) );
				continue;
			}
		}

		if ( depth == 2 && firstStageImage.empty() ) {
			if ( string_equal_nocase( token, "map" ) || string_equal_nocase( token, "clampmap" ) || string_equal_nocase( token, "animmap" ) ) {
				const bool anim = string_equal_nocase( token, "animmap" );
				if ( anim ) {
					tokeniser.getToken(); // frequency
				}
				if ( const char* image = tokeniser.getToken(); image != 0 && image[0] != '$' && image[0] != '*' ) {
					firstStageImage = StringStream<64>( PathExtensionless( image ) );
				}
			}
		}

		if ( depth == 1 ) {
			if ( string_equal_nocase( token, "qer_nocarve" ) ) {
				m_nFlags |= QER_NOCARVE;
			}
			else if ( string_equal_nocase( token, "qer_trans" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, m_fTrans ) );
				m_nFlags |= QER_TRANS;
			}
			else if ( string_equal_nocase( token, "qer_editorimage" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, m_textureName ) );
				hasEditorImage = true;
			}
			else if ( string_equal_nocase( token, "q3map_sun" ) || string_equal_nocase( token, "q3map_sunExt" ) || string_equal_nocase( token, "sun" ) /* SOF2's own shaders */ ) {
				bool ok = true;
				for( float& value : m_sun )
					ok = ok && Tokeniser_getFloat( tokeniser, value );
				m_hasSun = ok;
				m_sunExt = string_equal_nocase( token, "q3map_sunExt" );
				if( ok && m_sunExt ){
					Tokeniser_getFloat( tokeniser, m_sunDeviance );
					Tokeniser_getFloat( tokeniser, m_sunSamples );
				}
			}
			else if ( string_equal_nocase( token, "fogparms" ) ) {
				// fogparms ( r g b ) <distance to full fog> [opaque]; the brackets are optional in some shaders
				const char* t = tokeniser.getToken();
				if ( t != 0 && string_equal( t, "(" ) )
					t = tokeniser.getToken();
				float v[4] = { 1, 1, 1, 0 };
				bool ok = t != 0;
				for ( int k = 0; ok && k < 3; ++k ) {
					if ( k != 0 )
						t = tokeniser.getToken();
					if ( t == 0 || string_equal( t, ")" ) ) { ok = false; break; }
					v[k] = float( atof( t ) );
				}
				if ( ok ) {
					t = tokeniser.getToken();
					if ( t != 0 && string_equal( t, ")" ) )
						t = tokeniser.getToken();
					if ( t != 0 )
						v[3] = float( atof( t ) );
				}
				if ( ok && v[3] > 0 ) {
					m_krHasFog = true;
					std::copy( v, v + 3, m_krFogColour );
					m_krFogDepth = v[3];
				}
			}
			else if ( string_equal_nocase( token, "q3map_skyLight" ) ) {
				Tokeniser_getFloat( tokeniser, m_skyLight );
				Tokeniser_getFloat( tokeniser, m_skyIterations );
			}
			else if ( string_equal_nocase( token, "q3map_terrain" ) || string_equal_nocase( token, "q3map_indexed" ) ) {
				m_nFlags |= QER_KR_TERRAIN;
			}
			else if ( string_equal_nocase( token, "polygonOffset" ) ) {
				m_krFx.polygonOffset = true;
			}
			else if ( string_equal_nocase( token, "deformVertexes" ) ) {
				const char* kind = tokeniser.getToken();
				if( kind != 0 && m_krFx.deform == 0 ){
					if( string_equal_nocase( kind, "wave" ) ){
						Tokeniser_getFloat( tokeniser, m_krFx.deformDiv );
						m_krFx.deformFunc = KrWaveFunc( tokeniser.getToken() );
						for( float& v : m_krFx.deformWave )
							Tokeniser_getFloat( tokeniser, v );
						m_krFx.deform = m_krFx.deformFunc != 0 ? 1 : 0;
					}
					else if( string_equal_nocase( kind, "move" ) ){
						for( float& v : m_krFx.deformMove )
							Tokeniser_getFloat( tokeniser, v );
						m_krFx.deformFunc = KrWaveFunc( tokeniser.getToken() );
						for( float& v : m_krFx.deformWave )
							Tokeniser_getFloat( tokeniser, v );
						m_krFx.deform = m_krFx.deformFunc != 0 ? 2 : 0;
					}
					else if( string_equal_nocase( kind, "bulge" ) ){
						for( float& v : m_krFx.bulge )
							Tokeniser_getFloat( tokeniser, v );
						m_krFx.deform = 3;
					}
				}
			}
			else if ( string_equal_nocase( token, "q3map_lightRGB" ) ) {
				m_krHasLightRGB = Tokeniser_getFloat( tokeniser, m_krLightRGB[0] ) && Tokeniser_getFloat( tokeniser, m_krLightRGB[1] ) && Tokeniser_getFloat( tokeniser, m_krLightRGB[2] );
			}
			else if ( string_equal_nocase( token, "q3map_surfacelight" ) ) {
				m_krEmissive = true; // light emitting surface: fullbright in the light preview
				float value = 0;
				if( Tokeniser_getFloat( tokeniser, value ) )
					m_krSurfaceLight = value;
			}
			else if ( string_equal_nocase( token, "qer_alphafunc" ) ) {
				const char* alphafunc = tokeniser.getToken();

				if ( alphafunc == 0 ) {
					Tokeniser_unexpectedError( tokeniser, alphafunc, "#alphafunc" );
					return false;
				}

				if ( string_equal_nocase( alphafunc, "equal" ) ) {
					m_AlphaFunc = IShader::eEqual;
				}
				else if ( string_equal_nocase( alphafunc, "greater" ) ) {
					m_AlphaFunc = IShader::eGreater;
				}
				else if ( string_equal_nocase( alphafunc, "less" ) ) {
					m_AlphaFunc = IShader::eLess;
				}
				else if ( string_equal_nocase( alphafunc, "gequal" ) ) {
					m_AlphaFunc = IShader::eGEqual;
				}
				else if ( string_equal_nocase( alphafunc, "lequal" ) ) {
					m_AlphaFunc = IShader::eLEqual;
				}
				else
				{
					m_AlphaFunc = IShader::eAlways;
				}

				m_nFlags |= QER_ALPHATEST;

				RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, m_AlphaRef ) );
			}
			else if ( string_equal_nocase( token, "skyparms" ) ) {
				const char* sky = tokeniser.getToken();

				if ( sky == 0 ) {
					Tokeniser_unexpectedError( tokeniser, sky, "#skyparms" );
					return false;
				}

				if( !string_equal( sky, "-" ) ){
					m_skyBox = sky;
				}

				m_nFlags |= QER_SKY;
			}
			else if ( string_equal_nocase( token, "cull" ) ) {
				const char* cull = tokeniser.getToken();

				if ( cull == 0 ) {
					Tokeniser_unexpectedError( tokeniser, cull, "#cull" );
					return false;
				}

				if ( string_equal_nocase( cull, "none" )
				  || string_equal_nocase( cull, "twosided" )
				  || string_equal_nocase( cull, "disable" ) ) {
					m_Cull = IShader::eCullNone;
				}
				else if ( string_equal_nocase( cull, "back" )
				       || string_equal_nocase( cull, "backside" )
				       || string_equal_nocase( cull, "backsided" ) ) {
					m_Cull = IShader::eCullBack;
				}
				else
				{
					m_Cull = IShader::eCullBack;
				}

				m_nFlags |= QER_CULL;
			}
			else if ( string_equal_nocase( token, "surfaceparm" ) ) {
				const char* surfaceparm = tokeniser.getToken();

				if ( surfaceparm == 0 ) {
					Tokeniser_unexpectedError( tokeniser, surfaceparm, "#surfaceparm" );
					return false;
				}

				{ // KenRadiant: not solid or translucent for q3map2: no shadow in the light preview
					static const char* const passes[] = { "trans", "nonsolid", "playerclip", "monsterclip", "botclip", "water", "slime", "lava", "fog",
					                                       "areaportal", "clusterportal", "donotenter", "origin", "trigger", "ladder", "shotclip", "hint", "skip" };
					for( const char* pass : passes )
						if( string_equal_nocase( surfaceparm, pass ) )
							m_krLightPass = true;
					if( string_equal_nocase( surfaceparm, "alphashadow" ) || string_equal_nocase( surfaceparm, "lightfilter" ) )
						m_krAlphaShadow = true; // foliage: cut-out shadow even though it is trans
				}
				if ( string_equal_nocase( surfaceparm, "fog" ) ) {
					m_nFlags |= QER_FOG;
					m_nFlags |= QER_TRANS;
					if ( m_fTrans == 1 ) { // has not been explicitly set by qer_trans
						m_fTrans = 0.35f;
					}
				}
				else if ( string_equal_nocase( surfaceparm, "nodraw" ) ) {
					m_nFlags |= QER_NODRAW;
				}
				else if ( string_equal_nocase( surfaceparm, "nonsolid" ) ) {
					m_nFlags |= QER_NONSOLID;
				}
				else if ( string_equal_nocase( surfaceparm, "water" ) ||
				          string_equal_nocase( surfaceparm, "lava" ) ||
				          string_equal_nocase( surfaceparm, "slime") ){
					m_nFlags |= QER_LIQUID;
				}
				else if ( string_equal_nocase( surfaceparm, "areaportal" ) ) {
					m_nFlags |= QER_AREAPORTAL;
				}
				else if ( string_equal_nocase( surfaceparm, "playerclip" ) ) {
					m_nFlags |= QER_CLIP;
				}
				else if ( string_equal_nocase( surfaceparm, "botclip" ) ) {
					m_nFlags |= QER_BOTCLIP;
				}
			}
		}
	}

	/* first stage blends with what is behind it: show it translucent (qer_trans and skies keep their own handling) */
	if ( !string_empty( m_q3BlendFunc.first.c_str() ) && !( m_nFlags & ( QER_TRANS | QER_SKY ) ) ) {
		// "X GL_ZERO" never reads what is behind (unless X is a GL_DST_* factor): it covers it, e.g. GL_SRC_ALPHA GL_ZERO + alphaFunc
		const bool opaque = string_equal_nocase( m_q3BlendFunc.second.c_str(), "gl_zero" )
		                 && !string_equal_prefix_nocase( m_q3BlendFunc.first.c_str(), "gl_dst" )
		                 && !string_equal_prefix_nocase( m_q3BlendFunc.first.c_str(), "gl_one_minus_dst" );
		if( !opaque )
			m_nFlags |= QER_TRANS;
	}
	if ( m_nFlags & QER_SKY )
		m_q3BlendFunc = BlendFuncExpression( "", "" );
	if ( m_krLightPass && !m_krAlphaShadow )
		m_nFlags |= QER_KR_LIGHTPASS;

	/* no qer_editorimage: fall back to the first stage image (what q3map2 and the game show),
	   unless an image with the shader's own name exists - that is what stock radiant used */
	m_firstStageImage = firstStageImage.c_str();
	if ( !hasEditorImage && !firstStageImage.empty() ) {
		const auto fileExists = []( const char* path ){
			if ( ArchiveFile* file = GlobalFileSystem().openFile( path ) ) {
				file->release();
				return true;
			}
			return false;
		};
		if ( !fileExists( StringStream<64>( m_Name, ".tga" ) )
		  && !fileExists( StringStream<64>( m_Name, ".jpg" ) )
		  && !fileExists( StringStream<64>( m_Name, ".png" ) ) ) {
			m_textureName = firstStageImage.c_str();
		}
	}
	/* skies without an image: show the box's front side */
	if ( !hasEditorImage && firstStageImage.empty() && !m_skyBox.empty() ) {
		m_textureName = StringStream<64>( m_skyBox, "_ft" ).c_str();
	}

	return true;
}

class Layer
{
public:
	LayerTypeId m_type;
	TextureExpression m_texture;
	BlendFunc m_blendFunc;
	bool m_clampToBorder;
	float m_alphaTest;
	float m_heightmapScale;

	Layer() : m_type( LAYER_NONE ), m_blendFunc( BLEND_ONE, BLEND_ZERO ), m_clampToBorder( false ), m_alphaTest( -1 ), m_heightmapScale( 0 ){
	}
};

std::list<CopiedString> g_shaderFilenames;

void ParseShaderFile( Tokeniser& tokeniser, const char* filename ){
	g_shaderFilenames.push_back( filename );
	filename = g_shaderFilenames.back().c_str();
	tokeniser.nextLine();
	for (;; )
	{
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			break;
		}

		if ( string_equal( token, "table" ) ) {
			if ( tokeniser.getToken() == 0 ) {
				Tokeniser_unexpectedError( tokeniser, 0, "#table-name" );
				return;
			}
			if ( !Tokeniser_parseToken( tokeniser, "{" ) ) {
				return;
			}
			for (;; )
			{
				const char* option = tokeniser.getToken();
				if ( string_equal( option, "{" ) ) {
					for (;; )
					{
						const char* value = tokeniser.getToken();
						if ( string_equal( value, "}" ) ) {
							break;
						}
					}

					if ( !Tokeniser_parseToken( tokeniser, "}" ) ) {
						return;
					}
					break;
				}
			}
		}
		else
		{
			if ( string_equal( token, "guide" ) ) {
				parseTemplateInstance( tokeniser, filename );
			}
			else
			{
				if ( !string_equal( token, "material" )
				  && !string_equal( token, "particle" )
				  && !string_equal( token, "skin" ) ) {
					tokeniser.ungetToken();
				}
				// first token should be the path + name.. (from base)
				CopiedString name;
				if ( !Tokeniser_parseShaderName( tokeniser, name ) ) {
				}
				ShaderTemplatePointer shaderTemplate( new ShaderTemplate() );
				shaderTemplate->setName( name.c_str() );

				g_shaders.insert( ShaderTemplateMap::value_type( shaderTemplate->getName(), shaderTemplate ) );

				bool result = ( g_shaderLanguage == SHADERLANGUAGE_QUAKE3 )
				              ? shaderTemplate->parseQuake3( tokeniser )
				              : shaderTemplate->parseDoom3( tokeniser );
				if ( result ) {
					// do we already have this shader?
					if ( !g_shaderDefinitions.insert( ShaderDefinitionMap::value_type( shaderTemplate->getName(), ShaderDefinition( shaderTemplate.get(), ShaderArguments(), filename ) ) ).second ) {
#ifdef _DEBUG
						globalWarningStream() << "WARNING: shader " << shaderTemplate->getName() << " is already in memory, definition in " << filename << " ignored.\n";
#endif
					}
				}
				else
				{
					globalErrorStream() << "Error parsing shader " << shaderTemplate->getName() << '\n';
					return;
				}
			}
		}
	}
}

void parseGuideFile( Tokeniser& tokeniser, const char* filename ){
	tokeniser.nextLine();
	for (;; )
	{
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			break;
		}

		if ( string_equal( token, "guide" ) ) {
			// first token should be the path + name.. (from base)
			ShaderTemplatePointer shaderTemplate( new ShaderTemplate );
			shaderTemplate->parseTemplate( tokeniser );
			if ( !g_shaderTemplates.insert( ShaderTemplateMap::value_type( shaderTemplate->getName(), shaderTemplate ) ).second ) {
				globalErrorStream() << "guide " << Quoted( shaderTemplate->getName() ) << ": already defined, second definition ignored\n";
			}
		}
		else if ( string_equal( token, "inlineGuide" ) ) {
			// skip entire inlineGuide definition
			std::size_t depth = 0;
			for (;; )
			{
				tokeniser.nextLine();
				token = tokeniser.getToken();
				if ( string_equal( token, "{" ) ) {
					++depth;
				}
				else if ( string_equal( token, "}" ) ) {
					if ( --depth == 0 ) {
						break;
					}
				}
			}
		}
	}
}

void LoadShaderFile( const char* filename ){
	ArchiveTextFile* file = GlobalFileSystem().openTextFile( filename );

	if ( file != 0 ) {
		globalOutputStream() << "Parsing shaderfile " << filename << '\n';

		Tokeniser& tokeniser = GlobalScriptLibrary().m_pfnNewShaderTokeniser( file->getInputStream() );

		ParseShaderFile( tokeniser, filename );

		tokeniser.release();
		file->release();
	}
	else
	{
		globalWarningStream() << "Unable to read shaderfile " << filename << '\n';
	}
}

void loadGuideFile( const char* filename ){
	const auto fullname = StringStream( "guides/", filename );
	ArchiveTextFile* file = GlobalFileSystem().openTextFile( fullname );

	if ( file != 0 ) {
		globalOutputStream() << "Parsing guide file " << fullname << '\n';

		Tokeniser& tokeniser = GlobalScriptLibrary().m_pfnNewScriptTokeniser( file->getInputStream() );

		parseGuideFile( tokeniser, fullname );

		tokeniser.release();
		file->release();
	}
	else
	{
		globalWarningStream() << "Unable to read guide file " << fullname << '\n';
	}
}

CShader* Try_Shader_ForName( const char* name ){
	{
		shaders_t::iterator i = g_ActiveShaders.find( name );
		if ( i != g_ActiveShaders.end() ) {
			return ( *i ).second;
		}
	}
	// active shader was not found

	// find matching shader definition
	ShaderDefinitionMap::iterator i = g_shaderDefinitions.find( name );
	if ( i == g_shaderDefinitions.end() ) {
		// shader definition was not found

		// create new shader definition from default shader template
		ShaderTemplatePointer shaderTemplate( new ShaderTemplate() );
		shaderTemplate->CreateDefault( name );
		g_shaderTemplates.insert( ShaderTemplateMap::value_type( shaderTemplate->getName(), shaderTemplate ) );

		i = g_shaderDefinitions.insert( ShaderDefinitionMap::value_type( name, ShaderDefinition( shaderTemplate.get(), ShaderArguments(), "" ) ) ).first;
	}

	// create shader from existing definition
	ShaderPointer pShader( new CShader( ( *i ).second ) );
	pShader->setName( name );
	g_ActiveShaders.insert( shaders_t::value_type( name, pShader ) );
	g_ActiveShadersChangedNotify();
	return pShader;
}

IShader *Shader_ForName( const char *name ){
	ASSERT_NOTNULL( name );

	IShader *pShader = Try_Shader_ForName( name );
	pShader->IncRef();
	return pShader;
}




// the list of scripts/*.shader files we need to work with
// those are listed in shaderlist file
std::vector<CopiedString> l_shaderfiles;

/*
   ==================
   DumpUnreferencedShaders
   useful function: dumps the list of .shader files that are not referenced to the console
   ==================
 */
void IfFound_dumpUnreferencedShader( bool& bFound, const char* filename ){
	bool listed = false;

	for ( const CopiedString& sh : l_shaderfiles )
	{
		if ( !strcmp( sh.c_str(), filename ) ) {
			listed = true;
			break;
		}
	}

	if ( !listed ) {
		if ( !bFound ) {
			bFound = true;
			globalOutputStream() << "Following shader files are not referenced in any shaderlist.txt:\n";
		}
		globalOutputStream() << '\t' << filename << '\n';
	}
}
typedef ReferenceCaller<bool, void(const char*), IfFound_dumpUnreferencedShader> IfFoundDumpUnreferencedShaderCaller;

void DumpUnreferencedShaders(){
	bool bFound = false;
	GlobalFileSystem().forEachFile( g_shadersDirectory, g_shadersExtension, IfFoundDumpUnreferencedShaderCaller( bFound ) );
}

void ShaderList_addShaderFile( const char* dirstring ){
	bool found = false;

	for ( const CopiedString& sh : l_shaderfiles )
	{
		if ( string_equal_nocase( dirstring, sh.c_str() ) ) {
			found = true;
			globalOutputStream() << "duplicate entry " << Quoted( sh ) << " in shaderlist.txt\n";
			break;
		}
	}

	if ( !found ) {
		l_shaderfiles.emplace_back( dirstring );
	}
}

/*
   ==================
   BuildShaderList
   build a CStringList of shader names
   ==================
 */
void BuildShaderList( TextInputStream& shaderlist ){
	Tokeniser& tokeniser = GlobalScriptLibrary().m_pfnNewSimpleTokeniser( shaderlist );
	StringOutputStream shaderFile( 64 );
	for( const char* token; tokeniser.nextLine(), token = tokeniser.getToken(); )
	{
		// each token should be a shader filename
		shaderFile( token );
		if( !path_extension_is( token, g_shadersExtension ) )
			shaderFile << '.' << g_shadersExtension;

		ShaderList_addShaderFile( shaderFile );
	}
	tokeniser.release();
}

void ShaderList_addFromArchive( const char *archivename ){
	const char *shaderpath = GlobalRadiant().getGameDescriptionKeyValue( "shaderpath" );
	if ( string_empty( shaderpath ) ) {
		return;
	}

	Archive *archive = GlobalFileSystem().getArchive( archivename, false );
	if ( archive ) {
		ArchiveTextFile *file = archive->openTextFile( StringStream<64>( DirectoryCleaned( shaderpath ), "shaderlist.txt" ) );
		if ( file ) {
			globalOutputStream() << "Found shaderlist.txt in " << archivename << '\n';
			BuildShaderList( file->getInputStream() );
			file->release();
		}
	}
}

#include "stream/filestream.h"

bool shaderlist_findOrInstall( const char* enginePath, const char* toolsPath, const char* shaderPath, const char* gamename ){
	const auto absShaderList = StringStream( enginePath, gamename, '/', shaderPath, "shaderlist.txt" );
	if ( file_exists( absShaderList ) ) {
		return true;
	}
	{
		const auto directory = StringStream( enginePath, gamename, '/', shaderPath );
		if ( !file_exists( directory ) && !Q_mkdir( directory ) ) {
			return false;
		}
	}
	{
		const auto defaultShaderList = StringStream( toolsPath, gamename, '/', "default_shaderlist.txt" );
		if ( file_exists( defaultShaderList ) ) {
			return file_copy( defaultShaderList, absShaderList );
		}
	}
	return false;
}

void Shaders_Load(){
	if ( g_shaderLanguage == SHADERLANGUAGE_QUAKE4 ) {
		GlobalFileSystem().forEachFile( "guides/", "guide", makeCallbackF( loadGuideFile ), 0 );
	}

	const char* shaderPath = GlobalRadiant().getGameDescriptionKeyValue( "shaderpath" );
	if ( !string_empty( shaderPath ) ) {
		const auto path = StringStream<64>( DirectoryCleaned( shaderPath ) );

		if ( g_useShaderList ) {
			// preload shader files that have been listed in shaderlist.txt
			const char* basegame = GlobalRadiant().getRequiredGameDescriptionKeyValue( "basegame" );
			const char* gamename = GlobalRadiant().getGameName();
			const char* enginePath = GlobalRadiant().getEnginePath();
			const char* toolsPath = GlobalRadiant().getGameToolsPath();

			bool isMod = !string_equal( basegame, gamename );

			if ( !isMod || !shaderlist_findOrInstall( enginePath, toolsPath, path, gamename ) ) {
				gamename = basegame;
				shaderlist_findOrInstall( enginePath, toolsPath, path, gamename );
			}

			GlobalFileSystem().forEachArchive( makeCallbackF( ShaderList_addFromArchive ), false, true );
			if( !l_shaderfiles.empty() ){
				DumpUnreferencedShaders();
			}
			else{
				globalOutputStream() << "No shaderlist.txt found: loading all shaders\n";
				GlobalFileSystem().forEachFile( path, g_shadersExtension, makeCallbackF( ShaderList_addShaderFile ), 1 );
			}
		}
		else
		{
			GlobalFileSystem().forEachFile( path, g_shadersExtension, makeCallbackF( ShaderList_addShaderFile ), 0 );
		}

		StringOutputStream shadername( 256 );
		for( const CopiedString& sh : l_shaderfiles )
		{
			LoadShaderFile( shadername( path, sh ) );
		}
	}

	//StringPool_analyse( ShaderPool::instance() );
}

void Shaders_Free(){
	FreeShaders();
	l_shaderfiles.clear();
	g_shaderFilenames.clear();
}

ModuleObservers g_observers;

std::size_t g_shaders_unrealised = 1; // wait until filesystem and is realised before loading anything
bool Shaders_realised(){
	return g_shaders_unrealised == 0;
}
void Shaders_Realise(){
	if ( --g_shaders_unrealised == 0 ) {
		Shaders_Load();
		g_observers.realise();
	}
}
void Shaders_Unrealise(){
	if ( ++g_shaders_unrealised == 1 ) {
		g_observers.unrealise();
		Shaders_Free();
	}
}

void Shaders_Refresh(){
	Shaders_Unrealise();
	Shaders_Realise();
}

class Quake3ShaderSystem : public ShaderSystem, public ModuleObserver
{
public:
	void realise() override {
		Shaders_Realise();
	}
	void unrealise() override {
		Shaders_Unrealise();
	}
	void refresh() override {
		Shaders_Refresh();
	}

	IShader* getShaderForName( const char* name ) override {
		return Shader_ForName( name );
	}

	void foreachShaderName( const ShaderNameCallback& callback ) override {
		for ( const auto& [ name, shader ] : g_shaderDefinitions )
		{
			callback( name.c_str() );
		}
	}

	void beginActiveShadersIterator() override {
		ActiveShaders_IteratorBegin();
	}
	bool endActiveShadersIterator() override {
		return ActiveShaders_IteratorAtEnd();
	}
	IShader* dereferenceActiveShadersIterator() override {
		return ActiveShaders_IteratorCurrent();
	}
	void incrementActiveShadersIterator() override {
		ActiveShaders_IteratorIncrement();
	}
	void setActiveShadersChangedNotify( const Callback<void()>& notify ) override {
		g_ActiveShadersChangedNotify = notify;
	}

	void attach( ModuleObserver& observer ) override {
		g_observers.attach( observer );
	}
	void detach( ModuleObserver& observer ) override {
		g_observers.detach( observer );
	}

	void setLightingEnabled( bool enabled ) override {
		if ( CShader::m_lightingEnabled != enabled ) {
			for ( const auto& [ name, shader ] : g_ActiveShaders )
			{
				shader->unrealiseLighting();
			}
			CShader::m_lightingEnabled = enabled;
			for ( const auto& [ name, shader ] : g_ActiveShaders )
			{
				shader->realiseLighting();
			}
		}
	}

	const char* getTexturePrefix() const override {
		return g_texturePrefix;
	}
};

Quake3ShaderSystem g_Quake3ShaderSystem;

ShaderSystem& GetShaderSystem(){
	return g_Quake3ShaderSystem;
}

void Shaders_Construct(){
	GlobalFileSystem().attach( g_Quake3ShaderSystem );
}
void Shaders_Destroy(){
	GlobalFileSystem().detach( g_Quake3ShaderSystem );

	if ( Shaders_realised() ) {
		Shaders_Free();
	}
}
