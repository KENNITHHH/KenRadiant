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

using DeviceVector = Vector2;
struct testSelect_unselected_scene_point_return_t{ DoubleVector3 point; std::optional<Plane3> plane; };
std::optional<testSelect_unselected_scene_point_return_t>
testSelect_unselected_scene_point( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon );

void KrLightPreview_redraw();
extern int g_krMapGeneration;
bool g_krTerrainShadersRealised = true; // false while the shader system reloads

namespace
{

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
		const QString map = QString( Map_Name( g_map ) ).replace( '\\', '/' );
		const int at = map.lastIndexOf( "/maps/", -1, Qt::CaseInsensitive );
		const QString base = at >= 0 ? map.left( at + 1 ) : QFileInfo( map ).path() + '/';
		return base + indexMap.c_str();
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

const char* keyOf( const Entity& entity, const char* a, const char* b ){
	const char* value = entity.getKeyValue( a );
	return string_empty( value ) ? entity.getKeyValue( b ) : value;
}

void Terrain_clearIfNewMap(){
	static bool attached = false;
	if( !attached ){
		attached = true;
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
			auto *group = new QGroupBox( "Make the selected func_group terrain" );
			auto *grid = new QGridLayout( group );
			grid->addWidget( new QLabel( "Layer shaders" ), 0, 0 );
			m_base = new QComboBox;
			m_base->setToolTip( "Shader sets with textures/<name>_0, _1 ... (and _0to1 ...), e.g. the metashader terrains" );
			grid->addWidget( m_base, 0, 1 );
			grid->addWidget( new QLabel( "Index map size" ), 1, 0 );
			m_size = new QComboBox;
			m_size->addItems( { "64", "128", "256", "512" } );
			m_size->setCurrentIndex( 2 );
			grid->addWidget( m_size, 1, 1 );
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
		if( g_paintDialog == nullptr )
			g_paintDialog = new TerrainPaintDialog( MainFrame_getWindow() );
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
