/*
   KenRadiant: q3map2 index-map terrain in the editor.

   A func_group (or any brush entity) with "alphamap" / "_indexmap" (a 256 colour .pcx), "layers" / "_layers" and
   "shader" / "_shader" is terrain for q3map2: every vertex of its q3map_terrain faces (textures/tools/_terrain) gets the
   layer the index map has at that point, and the face uses textures/<shader>_<n> or _<a>to<b> with vertex alpha.

   Here the 3D view draws those faces with the layer textures blended from the index map, and the Terrain Paint tool
   paints layers into the index map (undo-able, saved as .pcx when the map is saved).

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include "kr_terrain.h"

#include <vector>
#include <memory>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <optional>

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QPushButton>
#include <QComboBox>
#include <QGroupBox>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QListWidget>
#include <QDoubleSpinBox>
#include <QTextStream>
#include <QRegularExpression>

#include "igl.h"
#include "ientity.h"
#include "ishaders.h"
#include "ifilesystem.h"
#include "iundo.h"
#include "irender.h"
#include "iscenegraph.h"
#include "iselection.h"
#include "scenelib.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "texturelib.h"
#include "brush.h"
#include "brushnode.h"
#include "patch.h"
#include "map.h"
#include "mainframe.h"
#include "commands.h"
#include "gtkutil/widget.h"
#include "view.h"
#include "dialog.h"
#include "signal/isignal.h"
#include "math/plane.h"
#include "texwindow.h"
#include "qerplugin.h"
#include "entity.h"

using DeviceVector = Vector2;
struct testSelect_unselected_scene_point_return_t{ DoubleVector3 point; std::optional<Plane3> plane; };
std::optional<testSelect_unselected_scene_point_return_t>
testSelect_unselected_scene_point( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon );

void KrLightPreview_redraw();
void RefreshShaders();
extern int g_krMapGeneration;
bool g_krTerrainShadersRealised = true; // false while the shader system reloads

namespace
{

QString Terrain_baseDir();

/* ---------------- .pcx (8 bit, RLE) ---------------- */

bool Pcx_read( const unsigned char* data, std::size_t size, int& w, int& h, std::vector<unsigned char>& pixels, unsigned char palette[768] ){
	if( size < 128 + 769 || data[0] != 10 || data[3] != 8 || data[65] != 1 )
		return false;
	const auto u16 = [data]( int at ){ return int( data[at] ) | ( int( data[at + 1] ) << 8 ); };
	w = u16( 8 ) - u16( 4 ) + 1;
	h = u16( 10 ) - u16( 6 ) + 1;
	const int bytesPerLine = u16( 66 );
	if( w < 1 || h < 1 || w > 8192 || h > 8192 || bytesPerLine < w )
		return false;
	pixels.assign( std::size_t( w ) * h, 0 );
	std::size_t at = 128;
	const std::size_t end = size - 769;
	for( int y = 0; y < h; ++y ){
		int x = 0;
		while( x < bytesPerLine && at < end ){
			unsigned char b = data[at++];
			int run = 1;
			if( ( b & 0xC0 ) == 0xC0 ){
				run = b & 0x3F;
				b = at < end ? data[at++] : 0;
			}
			for( ; run > 0 && x < bytesPerLine; --run, ++x )
				if( x < w )
					pixels[std::size_t( y ) * w + x] = b;
		}
	}
	if( data[size - 769] == 12 )
		std::memcpy( palette, data + size - 768, 768 );
	return true;
}

std::vector<unsigned char> Pcx_write( int w, int h, const std::vector<unsigned char>& pixels, const unsigned char palette[768] ){
	std::vector<unsigned char> out( 128, 0 );
	const int bytesPerLine = ( w + 1 ) & ~1; // must be even
	const auto put16 = [&out]( int at, int v ){ out[at] = v & 255; out[at + 1] = ( v >> 8 ) & 255; };
	out[0] = 10; out[1] = 5; out[2] = 1; out[3] = 8;
	put16( 4, 0 ); put16( 6, 0 ); put16( 8, w - 1 ); put16( 10, h - 1 );
	put16( 12, 72 ); put16( 14, 72 );
	out[65] = 1;
	put16( 66, bytesPerLine );
	put16( 68, 1 );
	for( int y = 0; y < h; ++y ){
		int x = 0;
		while( x < bytesPerLine ){
			const unsigned char v = x < w ? pixels[std::size_t( y ) * w + x] : 0;
			int run = 1;
			while( x + run < bytesPerLine && run < 63 && ( x + run < w ? pixels[std::size_t( y ) * w + x + run] : 0 ) == v )
				++run;
			if( run > 1 || ( v & 0xC0 ) == 0xC0 )
				out.push_back( 0xC0 | run );
			out.push_back( v );
			x += run;
		}
	}
	out.push_back( 12 );
	out.insert( out.end(), palette, palette + 768 );
	return out;
}

/// a colour per layer for a new index map (index maps are read by number, the colours only help in paint programs)
void Palette_default( unsigned char palette[768] ){
	static const unsigned char colours[8][3] = { { 0, 0, 0 }, { 0, 160, 0 }, { 160, 110, 40 }, { 128, 128, 128 }, { 0, 0, 200 }, { 200, 200, 0 }, { 200, 0, 200 }, { 0, 200, 200 } };
	for( int i = 0; i < 256; ++i )
		for( int c = 0; c < 3; ++c )
			palette[i * 3 + c] = i < 8 ? colours[i][c] : 0;
}

/* ---------------- terrain records ---------------- */

int s_sceneGeneration = 0;
void Terrain_sceneChanged(){ ++s_sceneGeneration; }

class PixelsMemento : public UndoMemento
{
public:
	std::vector<unsigned char> m_pixels;
	PixelsMemento( const std::vector<unsigned char>& pixels ) : m_pixels( pixels ){}
	void release() override { delete this; }
};

struct TerrainRecord : public Undoable
{
	const Entity* entity = nullptr;
	scene::Node* node = nullptr;
	CopiedString indexMap, shader;
	int layers = 0;
	int w = 0, h = 0;
	std::vector<unsigned char> pixels;
	unsigned char palette[768];
	bool loaded = false, unsaved = false, glDirty = true;
	KrTerrainGL tgl;
	std::vector<IShader*> layerShaders;
	CopiedString stateName;
	Shader* state = nullptr;
	int boundsGeneration = -1;
	UndoObserver* undoObserver = nullptr;

	TerrainRecord(){
		Palette_default( palette );
		undoObserver = GlobalUndoSystem().observer( this );
	}
	~TerrainRecord(){
		GlobalUndoSystem().release( this );
		for( IShader* s : layerShaders )
			if( s != nullptr )
				s->DecRef();
		if( state != nullptr )
			GlobalShaderCache().release( stateName.c_str() );
		if( tgl.indexTex != 0 )
			gl().glDeleteTextures( 1, &tgl.indexTex );
	}
	UndoMemento* exportState() const override {
		return new PixelsMemento( pixels );
	}
	void importState( const UndoMemento* state ) override {
		undoObserver->save( this );
		pixels = static_cast<const PixelsMemento*>( state )->m_pixels;
		glDirty = true;
		unsaved = true;
		KrLightPreview_redraw();
	}

	void load(){
		loaded = true;
		void* buffer = nullptr;
		const std::size_t size = GlobalFileSystem().loadFile( indexMap.c_str(), &buffer );
		bool ok = false;
		if( size != 0 && buffer != nullptr ){
			ok = Pcx_read( static_cast<const unsigned char*>( buffer ), size, w, h, pixels, palette );
			GlobalFileSystem().freeFile( buffer );
		}
		if( !ok ){
			if( size != 0 )
				globalWarningStream() << "terrain: " << indexMap.c_str() << " is not an 8 bit .pcx, a new one replaces it when you paint and save\n";
			else
				globalOutputStream() << "terrain: " << indexMap.c_str() << " not found, starting a new 256 x 256 index map (layer 0)\n";
			w = h = 256;
			pixels.assign( std::size_t( w ) * h, 0 );
			Palette_default( palette );
		}
		for( unsigned char& p : pixels ) // like q3map2: out of range = last layer
			if( p >= layers )
				p = layers - 1;
		glDirty = true;
	}
	void releaseLayers(){
		for( IShader* s : layerShaders )
			if( s != nullptr )
				s->DecRef();
		layerShaders.clear();
	}
	void updateLayers(){
		if( !g_krTerrainShadersRealised ){ // shaders are being reloaded: hold nothing
			releaseLayers();
			tgl.layers = std::min( layers, 4 );
			std::fill( std::begin( tgl.layerTex ), std::end( tgl.layerTex ), 0u );
			return;
		}
		if( int( layerShaders.size() ) != std::min( layers, 4 ) ){
			for( IShader* s : layerShaders )
				if( s != nullptr )
					s->DecRef();
			layerShaders.clear();
			for( int k = 0; k < std::min( layers, 4 ); ++k )
				layerShaders.push_back( GlobalShaderSystem().getShaderForName( StringStream<128>( "textures/", shader, '_', k ) ) );
		}
		tgl.layers = std::min( layers, 4 );
		for( int k = 0; k < tgl.layers; ++k ){
			IShader* s = layerShaders[k];
			tgl.layerTex[k] = s != nullptr && s->getTexture() != nullptr ? s->getTexture()->texture_number : 0;
			const KrShaderEffects* fx = s != nullptr ? s->getEffects() : nullptr;
			tgl.layerScale[k][0] = fx != nullptr ? fx->scale[0] : 1;
			tgl.layerScale[k][1] = fx != nullptr ? fx->scale[1] : 1;
		}
	}
	void updateBounds(){
		if( boundsGeneration == s_sceneGeneration || node == nullptr )
			return;
		boundsGeneration = s_sceneGeneration;
		AABB bounds;
		if( scene::Traversable* traversable = Node_getTraversable( *node ) ){
			class Walker : public scene::Traversable::Walker
			{
				AABB& m_bounds;
			public:
				Walker( AABB& bounds ) : m_bounds( bounds ){}
				bool pre( scene::Node& child ) const override {
					if( Brush* brush = Node_getBrush( child ) )
						aabb_extend_by_aabb_safe( m_bounds, brush->localAABB() );
					else if( Patch* patch = Node_getPatch( child ) )
						aabb_extend_by_aabb_safe( m_bounds, patch->localAABB() );
					return false;
				}
			};
			traversable->traverse( Walker( bounds ) );
		}
		if( aabb_valid( bounds ) ){
			tgl.mins = bounds.origin - bounds.extents;
			tgl.maxs = bounds.origin + bounds.extents;
		}
		Vector3 v;
		if( string_parse_vector3( entity->getKeyValue( "min" ), v ) ) tgl.mins = v;
		if( string_parse_vector3( entity->getKeyValue( "max" ), v ) ) tgl.maxs = v;
	}
	void uploadIndex(){
		if( !glDirty )
			return;
		glDirty = false;
		std::vector<unsigned char> rgba( std::size_t( w ) * h * 4, 0 );
		for( std::size_t i = 0; i < pixels.size(); ++i )
			if( pixels[i] < 4 )
				rgba[i * 4 + pixels[i]] = 255;
			else
				rgba[i * 4 + 3] = 255;
		if( tgl.indexTex == 0 )
			gl().glGenTextures( 1, &tgl.indexTex );
		gl().glBindTexture( GL_TEXTURE_2D, tgl.indexTex );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		gl().glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
		gl().glBindTexture( GL_TEXTURE_2D, 0 );
	}
	/// where the file goes: next to where it was found, else in the base folder above the map's maps/ folder
	QString savePath() const {
		const char* root = GlobalFileSystem().findFile( indexMap.c_str() );
		if( !string_empty( root ) && root[string_length( root ) - 1] == '/' )
			return QString( root ) + indexMap.c_str();
		return Terrain_baseDir() + indexMap.c_str();
	}
	bool save(){
		const QString path = savePath();
		QDir().mkpath( QFileInfo( path ).path() );
		QFile file( path );
		if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ){
			globalErrorStream() << "terrain: can't write " << path.toLatin1().constData() << '\n';
			return false;
		}
		const auto data = Pcx_write( w, h, pixels, palette );
		file.write( reinterpret_cast<const char*>( data.data() ), qint64( data.size() ) );
		unsaved = false;
		globalOutputStream() << "terrain: saved " << path.toLatin1().constData() << '\n';
		return true;
	}
	/// index map pixel for a world point
	bool pixelFor( float x, float y, float& px, float& py ) const {
		const Vector3 size = tgl.maxs - tgl.mins;
		if( size.x() <= 0 || size.y() <= 0 )
			return false;
		px = ( x - tgl.mins.x() ) / size.x() * w;
		py = ( tgl.maxs.y() - y ) / size.y() * h;
		return true;
	}
};

std::vector<std::unique_ptr<TerrainRecord>> g_records; // index = render state id

/// shader reloads (Flush & Reload, skybox toggle, map change ...) must not find our references to the layer shaders
class TerrainShaderObserver : public ModuleObserver
{
public:
	void realise() override;
	void unrealise() override;
};
TerrainShaderObserver g_terrainShaderObserver;
bool g_terrainObserverAttached = false;

const char* keyOf( const Entity& entity, const char* a, const char* b ){
	const char* value = entity.getKeyValue( a );
	return string_empty( value ) ? entity.getKeyValue( b ) : value;
}

void Terrain_clearIfNewMap(){
	if( !g_terrainObserverAttached ){
		g_terrainObserverAttached = true;
		GlobalShaderSystem().attach( g_terrainShaderObserver );
	}
	static int generation = -1;
	static bool registered = false;
	if( !registered ){
		registered = true;
		AddSceneChangeCallback( FreeCaller<void(), Terrain_sceneChanged>() );
	}
	if( generation != g_krMapGeneration ){
		generation = g_krMapGeneration;
		g_records.clear();
	}
}

TerrainRecord* Terrain_find( const Entity* entity ){
	for( auto& r : g_records )
		if( r && r->entity == entity )
			return r.get();
	return nullptr;
}

void TerrainShaderObserver::realise(){
	g_krTerrainShadersRealised = true;
}
void TerrainShaderObserver::unrealise(){
	g_krTerrainShadersRealised = false;
	for( auto& r : g_records )
		if( r )
			r->releaseLayers();
}

} // namespace

Shader* KrTerrain_state( scene::Node& node, const Entity& entity ){
	const char* indexMap = keyOf( entity, "_indexmap", "alphamap" );
	if( string_empty( indexMap ) )
		return nullptr;
	const char* shader = keyOf( entity, "_shader", "shader" );
	int layers = 0;
	if( string_empty( shader ) || !string_parse_int( keyOf( entity, "_layers", "layers" ), layers ) || layers < 1 )
		return nullptr;
	if( string_equal_prefix_nocase( shader, "textures/" ) )
		shader += 9;
	Terrain_clearIfNewMap();
	TerrainRecord* r = Terrain_find( &entity );
	if( r != nullptr && ( !string_equal_nocase( r->indexMap.c_str(), indexMap ) || !string_equal_nocase( r->shader.c_str(), shader ) || r->layers != layers ) ){
		// keys edited: start over (the new index map is loaded)
		for( auto& p : g_records )
			if( p.get() == r )
				p.reset();
		r = nullptr;
	}
	if( r == nullptr ){
		auto record = std::make_unique<TerrainRecord>();
		record->entity = &entity;
		record->indexMap = indexMap;
		record->shader = shader;
		record->layers = std::min( layers, 256 );
		std::size_t id = 0;
		while( id < g_records.size() && g_records[id] )
			++id;
		if( id == g_records.size() )
			g_records.emplace_back();
		record->stateName = StringStream<32>( "$KR_TERRAIN ", int( id ) );
		g_records[id] = std::move( record );
		r = g_records[id].get();
	}
	r->node = &node;
	if( !r->loaded )
		r->load();
	r->updateLayers();
	r->updateBounds();
	r->uploadIndex();
	if( r->state == nullptr )
		r->state = GlobalShaderCache().capture( r->stateName.c_str() );
	return r->state;
}

const KrTerrainGL* KrTerrain_gl( int id ){
	if( id < 0 || id >= int( g_records.size() ) || !g_records[id] )
		return nullptr;
	return &g_records[id]->tgl;
}

void KrTerrain_saveAll(){
	for( auto& r : g_records )
		if( r && r->unsaved )
			r->save();
}


/* ---------------- terrains with KenRadiant-made layer shaders ---------------- */

namespace
{
/// the game folder the editor, q3map2 and the game read from (engine path + base/mod) - not the map's own folder:
/// a map kept elsewhere (an Extracted copy) still gets its shaders and index maps where they are found
QString Terrain_baseDir(){
	return QString( GlobalRadiant().getEnginePath() ).replace( '\\', '/' ) + GlobalRadiant().getGameName() + '/';
}
QString Terrain_shaderFile(){
	return Terrain_baseDir() + "shaders/" + QFileInfo( Map_Name( g_map ) ).completeBaseName() + "_terrain.shader";
}
/// a terrain whose layer shaders KenRadiant wrote: key (textures/<key>_n), layer images, tcMod scale, lightmap blend
struct GenTerrain { QString key; QStringList images; double scale = 1; bool twoX = true; };
const char* const c_genTag = "// KenRadiant terrain ";

QString readText( const QString& path ){
	QFile f( path );
	if( !f.open( QIODevice::ReadOnly | QIODevice::Text ) )
		return QString();
	return QString::fromUtf8( f.readAll() );
}
/// the block of a KenRadiant terrain in the map's _terrain.shader file
bool GenTerrain_read( const QString& key, GenTerrain& out ){
	const QString text = readText( Terrain_shaderFile() );
	for( const QString& line : text.split( '\n' ) ){
		if( !line.startsWith( c_genTag ) )
			continue;
		const QStringList parts = line.mid( int( strlen( c_genTag ) ) ).split( " | " );
		if( parts.size() < 5 || parts[0].trimmed() != key )
			continue;
		out.key = key;
		out.scale = parts[1].section( ' ', 1 ).toDouble();
		out.twoX = parts[2].trimmed() != "blend 1x";
		out.images = parts.mid( 3 );
		for( QString& i : out.images )
			i = i.trimmed();
		if( out.scale <= 0 )
			out.scale = 1;
		return true;
	}
	return false;
}
QString GenTerrain_shaders( const GenTerrain& t ){
	QString o;
	QTextStream s( &o );
	s << c_genTag << t.key << " | scale " << t.scale << " | blend " << ( t.twoX ? "2x" : "1x" );
	for( const QString& i : t.images )
		s << " | " << i;
	s << "\n// (made by the Terrain Paint tool - change the layers there, or this block is written over)\n";
	const QString lightmap = t.twoX ? "GL_DST_COLOR GL_SRC_COLOR" : "GL_DST_COLOR GL_ZERO";
	const QString tc = t.scale != 1 ? QString( "\t\ttcMod scale %1 %1\n" ).arg( t.scale ) : QString();
	for( int a = 0; a < t.images.size(); ++a ){
		s << "textures/" << t.key << '_' << a << "\n{\n\tqer_editorimage " << t.images[a] << '\n'
		  << "\t{\n\t\tmap " << t.images[a] << "\n\t\trgbGen identity\n" << tc << "\t}\n"
		  << "\t{\n\t\tmap $lightmap\n\t\trgbGen identity\n\t\tblendFunc " << lightmap << "\n\t}\n}\n";
		for( int b = a + 1; b < t.images.size(); ++b ){
			s << "textures/" << t.key << '_' << a << "to" << b << "\n{\n\tqer_editorimage " << t.images[a] << '\n'
			  << "\t{\n\t\tmap " << t.images[a] << "\n\t\trgbGen identity\n" << tc << "\t}\n"
			  << "\t{\n\t\tmap " << t.images[b] << "\n\t\tblendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA\n\t\trgbGen identity\n\t\talphaGen vertex\n" << tc << "\t}\n"
			  << "\t{\n\t\tmap $lightmap\n\t\trgbGen identity\n\t\tblendFunc " << lightmap << "\n\t}\n}\n";
		}
	}
	s << c_genTag << t.key << " end\n";
	return o;
}
/// writes (or replaces) the terrain's block; keeps a .bak; lists the file in shaderlist.txt when there is one
bool GenTerrain_write( const GenTerrain& t ){
	const QString path = Terrain_shaderFile();
	QString text = readText( path );
	if( !text.isEmpty() )
		QFile::copy( path, path + ".bak" ) || ( QFile::remove( path + ".bak" ) && QFile::copy( path, path + ".bak" ) );
	// cut the old block out
	const QString begin = QString( c_genTag ) + t.key + " | ", end = QString( c_genTag ) + t.key + " end";
	const int b = text.indexOf( begin );
	if( b >= 0 ){
		int e = text.indexOf( end, b );
		e = e < 0 ? text.size() : text.indexOf( '\n', e );
		text.remove( b, ( e < 0 ? text.size() : e + 1 ) - b );
	}
	if( !text.isEmpty() && !text.endsWith( '\n' ) )
		text += '\n';
	text += GenTerrain_shaders( t );
	QDir().mkpath( QFileInfo( path ).path() );
	QFile f( path );
	if( !f.open( QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text ) ){
		globalErrorStream() << "terrain: can't write " << path.toLatin1().constData() << '\n';
		return false;
	}
	f.write( text.toUtf8() );
	f.close();
	globalOutputStream() << "terrain: wrote the layer shaders of " << t.key.toLatin1().constData() << " to " << path.toLatin1().constData() << '\n';
	const QString list = QFileInfo( path ).path() + "/shaderlist.txt";
	const QString name = QFileInfo( path ).completeBaseName();
	if( QFile::exists( list ) ){
		const QString listed = readText( list );
		bool found = false;
		for( const QString& l : listed.split( '\n' ) )
			found = found || l.trimmed().compare( name, Qt::CaseInsensitive ) == 0;
		if( !found ){
			QFile lf( list );
			if( lf.open( QIODevice::Append | QIODevice::Text ) ){
				lf.write( ( ( listed.isEmpty() || listed.endsWith( '\n' ) ? "" : "\n" ) + name + "\n" ).toUtf8() );
				globalOutputStream() << "terrain: added " << name.toLatin1().constData() << " to shaderlist.txt\n";
			}
		}
	}
	return true;
}
/// the image the game draws for a shader (its first image stage), else the shader / texture name itself
QString Shader_image( const char* name ){
	QString image = name;
	IShader* shader = GlobalShaderSystem().getShaderForName( name );
	if( !shader->IsDefault() ){
		if( const std::vector<KrStage>* st = shader->getStages(); st != nullptr )
			for( const KrStage& stage : *st )
				if( stage.special == 0 && !stage.image.empty() ){
					image = stage.image.c_str();
					break;
				}
	}
	shader->DecRef();
	image.replace( '\\', '/' );
	const int dot = image.lastIndexOf( '.' );
	if( dot > image.lastIndexOf( '/' ) )
		image.truncate( dot );
	return image;
}
/// how the map's own lightmapped shaders multiply their lightmap: 2x (GL_DST_COLOR GL_SRC_COLOR) or 1x (GL_ZERO), most faces win
bool Map_lightmapTwoX(){
	std::map<CopiedString, int> kind; // shader -> 2 / 1 / 0
	int two = 0, one = 0;
	Scene_ForEachBrush_ForEachFace( GlobalSceneGraph(), [&]( Face& face ){
		const char* name = face.GetShader();
		auto it = kind.find( name );
		if( it == kind.end() ){
			int k = 0;
			IShader* shader = GlobalShaderSystem().getShaderForName( name );
			if( const std::vector<KrStage>* st = shader->getStages(); st != nullptr )
				for( const KrStage& stage : *st )
					if( stage.special == 1 && stage.blend && stage.src == BLEND_DST_COLOUR )
						k = stage.dst == BLEND_SRC_COLOUR ? 2 : stage.dst == BLEND_ZERO ? 1 : 0;
			shader->DecRef();
			it = kind.emplace( name, k ).first;
		}
		two += it->second == 2;
		one += it->second == 1;
	} );
	return two >= one;
}
}

/* ---------------- Terrain Paint tool ---------------- */

namespace
{
bool g_paint = false;
int g_paintLayer = 1;
float g_paintRadius = 64;
std::optional<Vector3> g_hover;
TerrainRecord* g_stroke = nullptr;

TerrainRecord* Terrain_at( const Vector3& point ){
	TerrainRecord* best = nullptr;
	float bestArea = 1e30f;
	for( auto& r : g_records ){
		if( !r )
			continue;
		const KrTerrainGL& g = r->tgl;
		if( point.x() >= g.mins.x() - 1 && point.x() <= g.maxs.x() + 1 && point.y() >= g.mins.y() - 1 && point.y() <= g.maxs.y() + 1 ){
			const float area = ( g.maxs.x() - g.mins.x() ) * ( g.maxs.y() - g.mins.y() );
			if( area < bestArea ){
				bestArea = area;
				best = r.get();
			}
		}
	}
	return best;
}

void Terrain_paint( TerrainRecord& r, const Vector3& point ){
	float px, py;
	if( !r.pixelFor( point.x(), point.y(), px, py ) )
		return;
	const Vector3 size = r.tgl.maxs - r.tgl.mins;
	const float rx = g_paintRadius / size.x() * r.w, ry = g_paintRadius / size.y() * r.h;
	const unsigned char layer = std::clamp( g_paintLayer, 0, r.layers - 1 );
	bool changed = false;
	for( int y = std::max( 0, int( py - ry - 1 ) ); y <= std::min( r.h - 1, int( py + ry + 1 ) ); ++y )
		for( int x = std::max( 0, int( px - rx - 1 ) ); x <= std::min( r.w - 1, int( px + rx + 1 ) ); ++x ){
			const float dx = ( x + 0.5f - px ) / std::max( rx, 0.5f ), dy = ( y + 0.5f - py ) / std::max( ry, 0.5f );
			if( dx * dx + dy * dy <= 1 ){
				unsigned char& p = r.pixels[std::size_t( y ) * r.w + x];
				if( p != layer ){
					p = layer;
					changed = true;
				}
			}
		}
	if( changed ){
		r.glDirty = true;
		r.unsaved = true;
	}
}

class TerrainPaintDialog : public QDialog
{
	QLabel* m_info;
	QComboBox* m_layerCombo;
	QSlider* m_radius;
	QSpinBox* m_radiusSpin;
	QComboBox* m_base;
	QComboBox* m_size;
	QListWidget* m_list;
	QDoubleSpinBox* m_scale;
	QPushButton* m_apply;
	QLabel* m_listInfo;
	const TerrainRecord* m_shown = nullptr;
public:
	TerrainPaintDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ){
		setWindowTitle( "Terrain Paint" );
		auto *vbox = new QVBoxLayout( this );
		m_info = new QLabel;
		m_info->setWordWrap( true );
		vbox->addWidget( m_info );
		{
			auto *row = new QHBoxLayout;
			row->addWidget( new QLabel( "Layer to paint" ) );
			m_layerCombo = new QComboBox;
			m_layerCombo->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
			m_layerCombo->setMinimumContentsLength( 32 );
			QObject::connect( m_layerCombo, QOverload<int>::of( &QComboBox::currentIndexChanged ), []( int i ){ if( i >= 0 ) g_paintLayer = i; } );
			row->addWidget( m_layerCombo, 1 );
			vbox->addLayout( row );
		}
		{
			auto *row = new QHBoxLayout;
			row->addWidget( new QLabel( "Radius" ) );
			m_radius = new QSlider( Qt::Horizontal );
			m_radius->setRange( 8, 2048 );
			m_radius->setValue( int( g_paintRadius ) );
			m_radiusSpin = new QSpinBox;
			m_radiusSpin->setRange( 8, 2048 );
			m_radiusSpin->setValue( int( g_paintRadius ) );
			QObject::connect( m_radius, &QSlider::valueChanged, [this]( int v ){ g_paintRadius = float( v ); const QSignalBlocker b( m_radiusSpin ); m_radiusSpin->setValue( v ); KrLightPreview_redraw(); } );
			QObject::connect( m_radiusSpin, QOverload<int>::of( &QSpinBox::valueChanged ), [this]( int v ){ g_paintRadius = float( v ); const QSignalBlocker b( m_radius ); m_radius->setValue( v ); KrLightPreview_redraw(); } );
			row->addWidget( m_radius );
			row->addWidget( m_radiusSpin );
			vbox->addLayout( row );
		}
		{
			auto *save = new QPushButton( "Save index maps now" );
			save->setToolTip( "They are also saved every time you save the map" );
			QObject::connect( save, &QPushButton::clicked, [](){ KrTerrain_saveAll(); } );
			vbox->addWidget( save );
		}
		{
			auto *group = new QGroupBox( "Your own layers (KenRadiant writes the layer shaders)" );
			auto *grid = new QGridLayout( group );
			m_list = new QListWidget;
			m_list->setToolTip( "Layer 0 first. Up to 4 layers (the 3D view shows 4)" );
			m_list->setMinimumHeight( 90 );
			grid->addWidget( m_list, 0, 0, 4, 1 );
			auto *add = new QPushButton( "+ Texture browser's texture" );
			add->setToolTip( "Click a texture in the Textures tab first" );
			auto *remove = new QPushButton( "Remove" );
			auto *up = new QPushButton( "Up" );
			auto *down = new QPushButton( "Down" );
			grid->addWidget( add, 0, 1 );
			grid->addWidget( remove, 1, 1 );
			grid->addWidget( up, 2, 1 );
			grid->addWidget( down, 3, 1 );
			auto *scaleRow = new QHBoxLayout;
			scaleRow->addWidget( new QLabel( "Texture scale" ) );
			m_scale = new QDoubleSpinBox;
			m_scale->setRange( 0.01, 16 );
			m_scale->setDecimals( 3 );
			m_scale->setSingleStep( 0.05 );
			m_scale->setValue( 1 );
			m_scale->setToolTip( "tcMod scale of every layer: 0.5 = textures twice as big" );
			scaleRow->addWidget( m_scale );
			scaleRow->addSpacing( 16 );
			scaleRow->addWidget( new QLabel( "Index map size" ) );
			m_size = new QComboBox;
			m_size->addItems( { "64", "128", "256", "512" } );
			m_size->setCurrentIndex( 2 );
			m_size->setToolTip( "Pixels of the .pcx (new terrains). q3map2 reads it at the brush vertices, so small brushes give finer painting" );
			scaleRow->addWidget( m_size );
			scaleRow->addStretch( 1 );
			grid->addLayout( scaleRow, 4, 0, 1, 2 );
			auto *create = new QPushButton( "Create terrain from the selected brushes" );
			create->setToolTip( "Groups the brushes in a func_group (if they are not), writes the layer shaders to shaders/<map>_terrain.shader, "
			                    "makes the .pcx index map and gives the upward faces the q3map_terrain shader. Then paint." );
			grid->addWidget( create, 5, 0, 1, 2 );
			m_apply = new QPushButton( "Apply these layers to the terrain" );
			m_apply->setToolTip( "Rewrites its layer shaders; removed / moved layers are remapped in the index map (saved right away, not undoable)" );
			m_apply->setEnabled( false );
			grid->addWidget( m_apply, 6, 0, 1, 2 );
			m_listInfo = new QLabel;
			m_listInfo->setWordWrap( true );
			m_listInfo->setStyleSheet( "color: gray;" );
			grid->addWidget( m_listInfo, 7, 0, 1, 2 );
			QObject::connect( add, &QPushButton::clicked, [this](){
				const char* selected = TextureBrowser_GetSelectedShader();
				bool missing = string_empty( selected ) || selected[string_length( selected ) - 1] == '/';
				if( !missing ){
					IShader* shader = GlobalShaderSystem().getShaderForName( selected );
					missing = shader->krNotFound();
					shader->DecRef();
				}
				if( missing || string_equal_nocase( selected, "textures/tools/_terrain" ) ){
					QMessageBox::information( this, "Terrain Paint", "Click a texture in the Textures tab first." );
					return;
				}
				if( m_list->count() >= 4 ){
					QMessageBox::information( this, "Terrain Paint", "4 layers at most (what the 3D view can show)." );
					return;
				}
				auto *item = new QListWidgetItem( Shader_image( selected ) );
				item->setData( Qt::UserRole, -1 ); // new layer
				m_list->addItem( item );
				updateListInfo();
			} );
			QObject::connect( remove, &QPushButton::clicked, [this](){ delete m_list->currentItem(); updateListInfo(); } );
			QObject::connect( up, &QPushButton::clicked, [this](){ move( -1 ); } );
			QObject::connect( down, &QPushButton::clicked, [this](){ move( 1 ); } );
			QObject::connect( create, &QPushButton::clicked, [this](){ createTerrain(); } );
			QObject::connect( m_apply, &QPushButton::clicked, [this](){ applyLayers(); } );
			vbox->addWidget( group );
		}
		{
			auto *group = new QGroupBox( "Or use an existing layer shader set on the selected func_group" );
			auto *grid = new QGridLayout( group );
			grid->addWidget( new QLabel( "Layer shaders" ), 0, 0 );
			m_base = new QComboBox;
			m_base->setToolTip( "Shader sets with textures/<name>_0, _1 ... (and _0to1 ...), e.g. the metashader terrains" );
			grid->addWidget( m_base, 0, 1 );
			auto *make = new QPushButton( "Make terrain" );
			make->setToolTip( "Sets alphamap / layers / shader on the selected brushes' func_group and gives them the q3map_terrain shader" );
			QObject::connect( make, &QPushButton::clicked, [this](){ makeTerrain(); } );
			grid->addWidget( make, 2, 0, 1, 2 );
			vbox->addWidget( group );
		}
		auto *help = new QLabel( "Left-drag on the terrain in the 3D view paints the layer. Ctrl+Z undoes a stroke. "
		                         "The index maps are saved as .pcx when you save the map." );
		help->setWordWrap( true );
		vbox->addWidget( help );
	}
	void move( int by ){
		const int row = m_list->currentRow();
		if( row < 0 || row + by < 0 || row + by >= m_list->count() )
			return;
		QListWidgetItem* item = m_list->takeItem( row );
		m_list->insertItem( row + by, item );
		m_list->setCurrentRow( row + by );
		updateListInfo();
	}
	void updateListInfo(){
		for( int i = 0; i < m_list->count(); ++i ){
			QListWidgetItem* item = m_list->item( i );
			const QString image = item->data( Qt::UserRole + 1 ).toString().isEmpty() ? item->text().section( ": ", -1 ) : item->data( Qt::UserRole + 1 ).toString();
			item->setData( Qt::UserRole + 1, image );
			item->setText( QString( "%1: %2" ).arg( i ).arg( image ) );
		}
		GenTerrain g;
		const bool editable = m_shown != nullptr && GenTerrain_read( m_shown->shader.c_str(), g );
		m_apply->setEnabled( editable && m_list->count() >= 2 );
		m_listInfo->setText( editable ? QString( "Editing terrain %1 (the one under the mouse last)." ).arg( m_shown->shader.c_str() )
		                              : "Select brushes and create a terrain - or move the mouse over a terrain made here to edit its layers." );
	}
	QStringList listImages() const {
		QStringList images;
		for( int i = 0; i < m_list->count(); ++i )
			images << m_list->item( i )->data( Qt::UserRole + 1 ).toString();
		return images;
	}
	void createTerrain(){
		if( Map_Unnamed( g_map ) ){
			QMessageBox::warning( this, "Terrain Paint", "Save the map first (the shader and index map files are named after it)." );
			return;
		}
		if( m_list->count() < 2 ){
			QMessageBox::warning( this, "Terrain Paint", "Add at least 2 layer textures first." );
			return;
		}
		if( GlobalSelectionSystem().countSelected() == 0 ){
			QMessageBox::warning( this, "Terrain Paint", "Select the brushes that make the ground first." );
			return;
		}
		const auto entityOfSelection = []() -> Entity* {
			const scene::Path& path = GlobalSelectionSystem().ultimateSelected().path();
			for( std::size_t i = path.size(); i-- > 0; )
				if( Entity* e = Node_getEntity( path[i] ) )
					return e;
			return nullptr;
		};
		Entity* entity = entityOfSelection();
		if( entity != nullptr && string_equal( entity->getClassName(), "worldspawn" ) ){
			Entity_createFromSelection( "func_group", Vector3( 0, 0, 0 ) ); // (its own undo step)
			entity = GlobalSelectionSystem().countSelected() != 0 ? entityOfSelection() : nullptr;
		}
		if( entity == nullptr || string_equal( entity->getClassName(), "worldspawn" ) ){
			QMessageBox::warning( this, "Terrain Paint", "Could not put the brushes in a func_group - select only brushes and try again." );
			return;
		}
		if( !string_empty( keyOf( *entity, "_indexmap", "alphamap" ) ) ){
			QMessageBox::warning( this, "Terrain Paint", "These brushes are a terrain already - move the mouse over it and use Apply to change its layers." );
			return;
		}
		const QString mapName = QFileInfo( Map_Name( g_map ) ).completeBaseName();
		GenTerrain t;
		int n = 1;
		for( ; n < 100; ++n ){
			GenTerrain existing;
			t.key = QString( "%1/terrain%2" ).arg( mapName ).arg( n );
			const QString index = QString( "maps/%1_terrain%2.pcx" ).arg( mapName ).arg( n );
			if( !GenTerrain_read( t.key, existing ) && string_empty( GlobalFileSystem().findFile( index.toLatin1().constData() ) ) )
				break;
		}
		t.images = listImages();
		t.scale = m_scale->value();
		t.twoX = Map_lightmapTwoX();
		if( !GenTerrain_write( t ) )
			return;
		RefreshShaders(); // load the new layer shaders before anything asks for them
		const QString index = QString( "maps/%1_terrain%2.pcx" ).arg( mapName ).arg( n );
		{
			UndoableCommand undo( "makeTerrain" );
			entity->setKeyValue( "alphamap", index.toLatin1().constData() );
			entity->setKeyValue( "layers", QByteArray::number( int( t.images.size() ) ).constData() );
			entity->setKeyValue( "shader", t.key.toLatin1().constData() );
			// the faces looking up get the q3map_terrain tool shader (sides and bottoms keep theirs)
			Scene_forEachSelectedBrush( []( BrushInstance& brush ){
				for( Face* face : brush.getBrush() )
					if( face->getPlane().plane3().normal().z() > 0.2 )
						face->SetShader( "textures/tools/_terrain" );
			} );
		}
		{ // the index map file, all layer 0 (the terrain loads it when it is drawn)
			const int size = m_size->currentText().toInt();
			unsigned char palette[768];
			Palette_default( palette );
			const auto data = Pcx_write( size, size, std::vector<unsigned char>( std::size_t( size ) * size, 0 ), palette );
			const QString file = Terrain_baseDir() + index;
			QDir().mkpath( QFileInfo( file ).path() );
			QFile f( file );
			if( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
				f.write( reinterpret_cast<const char*>( data.data() ), qint64( data.size() ) );
			else
				globalErrorStream() << "terrain: can't write " << file.toLatin1().constData() << '\n';
		}
		globalOutputStream() << "terrain: " << t.key.toLatin1().constData() << " - " << int( t.images.size() ) << " layers, index map " << index.toLatin1().constData()
		                     << ", lightmap blend " << ( t.twoX ? "2x like the map's shaders" : "1x like the map's shaders" ) << " - paint it with left-drag\n";
		KrLightPreview_redraw();
	}
	void applyLayers(){
		TerrainRecord* r = nullptr;
		for( auto& p : g_records )
			if( p.get() == m_shown )
				r = p.get();
		GenTerrain t;
		if( r == nullptr || !GenTerrain_read( r->shader.c_str(), t ) || m_list->count() < 2 )
			return;
		// old layer -> new layer (a removed layer becomes the first layer)
		std::vector<unsigned char> remap( 256, 0 );
		for( int i = 0; i < m_list->count(); ++i ){
			const int old = m_list->item( i )->data( Qt::UserRole ).toInt();
			if( old >= 0 && old < 256 )
				remap[old] = (unsigned char)i;
		}
		if( !r->loaded )
			r->load();
		for( unsigned char& p : r->pixels )
			p = remap[p];
		r->glDirty = true;
		r->unsaved = true;
		r->save(); // the record may be made again from the file when the layer count changes
		t.images = listImages();
		t.scale = m_scale->value();
		if( !GenTerrain_write( t ) )
			return;
		const int layers = int( t.images.size() );
		if( Entity* e = const_cast<Entity*>( r->entity ); e != nullptr && r->layers != layers ){
			UndoableCommand undo( "terrainLayers" );
			e->setKeyValue( string_empty( e->getKeyValue( "_layers" ) ) ? "layers" : "_layers", QByteArray::number( layers ).constData() );
		}
		m_shown = nullptr;
		RefreshShaders();
		globalOutputStream() << "terrain: " << t.key.toLatin1().constData() << " now has " << layers << " layers\n";
		KrLightPreview_redraw();
	}
	void fillBases(){
		m_base->clear();
		struct Names{
			std::vector<CopiedString> list;
			void add( const char* name ){ list.emplace_back( name ); }
		} collector;
		GlobalShaderSystem().foreachShaderName( MemberCaller<Names, void(const char*), &Names::add>( collector ) );
		std::vector<CopiedString>& names = collector.list;
		std::sort( names.begin(), names.end(), []( const CopiedString& a, const CopiedString& b ){ return string_less_nocase( a.c_str(), b.c_str() ); } );
		const auto has = [&names]( const char* n ){ return std::binary_search( names.begin(), names.end(), CopiedString( n ), []( const CopiedString& a, const CopiedString& b ){ return string_less_nocase( a.c_str(), b.c_str() ); } ); };
		for( const CopiedString& n : names ){
			const std::size_t len = string_length( n.c_str() );
			if( len > 11 && string_equal( n.c_str() + len - 2, "_0" ) && string_equal_prefix_nocase( n.c_str(), "textures/" ) ){
				const std::string base( n.c_str() + 9, len - 11 );
				int layers = 1;
				while( layers < 8 && has( StringStream<128>( "textures/", base.c_str(), '_', layers ) ) )
					++layers;
				if( layers >= 2 )
					m_base->addItem( QString( "%1  (%2 layers)" ).arg( base.c_str() ).arg( layers ), QStringList{ base.c_str(), QString::number( layers ) } );
			}
		}
	}
	void makeTerrain(){
		if( m_base->count() == 0 ){
			QMessageBox::warning( this, "Terrain Paint", "No layer shader set found (textures/<name>_0, _1 ...)." );
			return;
		}
		if( GlobalSelectionSystem().countSelected() == 0 ){
			QMessageBox::warning( this, "Terrain Paint", "Select the terrain brushes first (in a func_group: Ctrl+G groups them)." );
			return;
		}
		const scene::Path& path = GlobalSelectionSystem().ultimateSelected().path();
		Entity* entity = nullptr;
		for( std::size_t i = path.size(); i-- > 0 && entity == nullptr; )
			entity = Node_getEntity( path[i] );
		if( entity == nullptr || string_equal( entity->getClassName(), "worldspawn" ) ){
			QMessageBox::warning( this, "Terrain Paint", "The selected brushes must be in a func_group (select them and use Ctrl+G or Entity -> func_group)." );
			return;
		}
		const QStringList data = m_base->currentData().toStringList();
		const QString mapName = QFileInfo( Map_Name( g_map ) ).completeBaseName();
		int n = 1;
		QString index;
		do{
			index = QString( "maps/%1_terrain%2.pcx" ).arg( mapName ).arg( n++ );
		} while( !string_empty( GlobalFileSystem().findFile( index.toLatin1().constData() ) ) && n < 100 );
		{
			UndoableCommand undo( "makeTerrain" );
			entity->setKeyValue( "alphamap", index.toLatin1().constData() );
			entity->setKeyValue( "layers", data[1].toLatin1().constData() );
			entity->setKeyValue( "shader", data[0].toLatin1().constData() );
			// the selected brushes get the q3map_terrain tool shader on their faces
			Scene_forEachSelectedBrush( []( BrushInstance& brush ){
				for( Face* face : brush.getBrush() )
					face->SetShader( "textures/tools/_terrain" );
			} );
		}
		// size of the new index map
		if( TerrainRecord* r = Terrain_find( entity ) ){
			const int size = m_size->currentText().toInt();
			r->loaded = true;
			r->w = r->h = size;
			r->pixels.assign( std::size_t( size ) * size, 0 );
			r->glDirty = true;
			r->unsaved = true;
		}
		globalOutputStream() << "terrain: " << entity->getClassName() << " uses " << index.toLatin1().constData() << " (" << data[1].toLatin1().constData() << " layers of " << data[0].toLatin1().constData() << ")\n";
		KrLightPreview_redraw();
	}
	/// layers of the terrain under the cursor (the last one stays while the cursor is elsewhere)
	void showTerrain( const TerrainRecord* r ){
		if( r == nullptr && m_shown != nullptr )
			return;
		if( r == m_shown && r != nullptr )
			return;
		m_shown = r;
		const QSignalBlocker blocker( m_layerCombo );
		m_layerCombo->clear();
		if( r == nullptr ){
			m_info->setText( "Move the mouse over a terrain (a func_group with alphamap, layers and shader keys)." );
			return;
		}
		m_info->setText( QString( "Terrain <b>%1</b> - %2 layers, index map %3 (%4 x %5)%6" )
		                 .arg( r->shader.c_str() ).arg( r->layers ).arg( r->indexMap.c_str() ).arg( r->w ).arg( r->h ).arg( r->unsaved ? ", not saved" : "" ) );
		for( int k = 0; k < r->layers; ++k ){
			QString name = QString( "%1: textures/%2_%1" ).arg( k ).arg( r->shader.c_str() );
			if( k < int( r->layerShaders.size() ) && r->layerShaders[k] != nullptr && r->layerShaders[k]->getTexture() != nullptr )
				name += QString( "  (%1)" ).arg( r->layerShaders[k]->getTexture()->name );
			m_layerCombo->addItem( name );
		}
		g_paintLayer = std::clamp( g_paintLayer, 0, r->layers - 1 );
		m_layerCombo->setCurrentIndex( g_paintLayer );
		GenTerrain g;
		if( GenTerrain_read( r->shader.c_str(), g ) ){
			m_list->clear();
			for( int k = 0; k < g.images.size(); ++k ){
				auto *item = new QListWidgetItem( g.images[k] );
				item->setData( Qt::UserRole, k );
				item->setData( Qt::UserRole + 1, g.images[k] );
				m_list->addItem( item );
			}
			m_scale->setValue( g.scale );
		}
		updateListInfo();
	}
	void forget(){ m_shown = nullptr; }
	void refreshInfo(){
		const TerrainRecord* r = m_shown;
		if( std::none_of( g_records.begin(), g_records.end(), [r]( const auto& p ){ return p.get() == r; } ) )
			r = nullptr; // another map
		m_shown = nullptr;
		showTerrain( r );
	}
};

TerrainPaintDialog* g_paintDialog = nullptr;

ToggleItem g_paint_item{ BoolExportCaller( g_paint ) };
void TerrainPaint_toggle(){
	g_paint = !g_paint;
	g_paint_item.update();
	g_hover.reset();
	if( g_paint ){
		if( g_paintDialog == nullptr ){
			g_paintDialog = new TerrainPaintDialog( MainFrame_getWindow() );
			QObject::connect( g_paintDialog, &QObject::destroyed, [](){ g_paintDialog = nullptr; } );
		}
		g_paintDialog->fillBases();
		g_paintDialog->forget();
		g_paintDialog->showTerrain( nullptr );
		g_paintDialog->show();
		g_paintDialog->raise();
		globalOutputStream() << "terrain paint: on - left-drag on a terrain in the 3D view\n";
	}
	else{
		if( g_paintDialog != nullptr )
			g_paintDialog->hide();
		globalOutputStream() << "terrain paint: off\n";
	}
	KrLightPreview_redraw();
}

} // namespace

bool KrTerrainPaint_active(){
	return g_paint;
}

void KrTerrainPaint_mouse( int phase, const View& view, float deviceX, float deviceY, float epsX, float epsY ){
	const auto hit = testSelect_unselected_scene_point( view, DeviceVector( deviceX, deviceY ), DeviceVector( epsX, epsY ) );
	g_hover.reset();
	if( hit )
		g_hover = Vector3( hit->point );
	TerrainRecord* r = g_hover ? Terrain_at( *g_hover ) : nullptr;
	if( g_paintDialog != nullptr && ( r != nullptr || g_stroke == nullptr ) )
		g_paintDialog->showTerrain( r );
	if( phase == 0 && r != nullptr ){
		g_stroke = r;
		GlobalUndoSystem().start();
		r->undoObserver->save( r );
		Terrain_paint( *r, *g_hover );
		Map_SetModified( g_map, true );
	}
	else if( phase == 1 && g_stroke != nullptr && g_hover ){
		Terrain_paint( *g_stroke, *g_hover );
	}
	else if( phase == 2 && g_stroke != nullptr ){
		GlobalUndoSystem().finish( "terrainPaint" );
		g_stroke = nullptr;
		if( g_paintDialog != nullptr )
			g_paintDialog->refreshInfo();
	}
	KrLightPreview_redraw();
}

void KrTerrainPaint_drawOverlay( const Matrix4& world2screen, int width, int height ){
	if( !g_paint || !g_hover )
		return;
	std::vector<Vector2> points;
	for( int i = 0; i <= 48; ++i ){
		const float a = float( i ) / 48 * 2 * float( c_pi );
		const Vector3 p = *g_hover + Vector3( std::cos( a ), std::sin( a ), 0 ) * g_paintRadius;
		const Vector4 c = matrix4_transformed_vector4( world2screen, Vector4( p, 1 ) );
		if( c.w() <= 0.01f )
			return;
		points.emplace_back( ( c.x() / c.w() + 1 ) * 0.5f * width, ( 1 - c.y() / c.w() ) * 0.5f * height );
	}
	gl().glLineWidth( 2 );
	gl().glColor3f( Terrain_at( *g_hover ) != nullptr ? 0.2f : 0.8f, 0.9f, 0.2f );
	gl().glVertexPointer( 2, GL_FLOAT, sizeof( Vector2 ), points[0].data() );
	gl().glDrawArrays( GL_LINE_STRIP, 0, GLsizei( points.size() ) );
	gl().glLineWidth( 1 );
}

void KrTerrain_registerCommands(){
	GlobalToggles_insert( "ToggleTerrainPaint", makeCallbackF( TerrainPaint_toggle ), ToggleItem::AddCallbackCaller( g_paint_item ) );
}

/// before the modules shut down (the undo and shader systems go away): drop the terrain records while they still exist;
/// the GL textures die with the context
void KrTerrain_shutdown(){
	for( auto& r : g_records )
		if( r )
			r->tgl.indexTex = 0;
	g_records.clear();
	if( g_terrainObserverAttached ){
		g_terrainObserverAttached = false;
		GlobalShaderSystem().detach( g_terrainShaderObserver );
	}
}
