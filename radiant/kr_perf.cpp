/*
   KenRadiant: Performance view - what the game has to draw, from the compiled .bsp the way the engine decides it:
   the camera's vis cluster -> its PVS -> the leafs in view -> their surfaces (r_speeds style numbers), the heaviest
   shaders, the worst spots of the whole map (with Go buttons), and suggestions with fix buttons.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <vector>
#include <map>
#include <set>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <functional>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QProgressBar>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QTextStream>

#include "gtkutil/guisettings.h"
#include "math/matrix.h"
#include "math/vector.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "os/path.h"
#include "commands.h"
#include "ishaders.h"
#include "iselection.h"
#include "mainframe.h"
#include "map.h"
#include "qerplugin.h"

bool KrCamera_getMatrices( Matrix4& modelview, Matrix4& projection );
void KrCamera_getView( Vector3& origin, Vector3& angles, float& fov );
void KrCamera_setView( const Vector3& origin, const Vector3& angles );
int Scene_smallStructural( float maxSize, bool select, int& structural, int& detail );
void Select_MakeDetail();

namespace
{

#pragma pack( push, 1 )
struct Lump { int offset, length; };
struct Header { char ident[4]; int version; Lump lumps[18]; };
struct DShader { char name[64]; int surfaceFlags, contentFlags; };
struct DPlane { float normal[3]; float dist; };
struct DNode { int plane; int children[2]; int mins[3], maxs[3]; };
struct DLeaf { int cluster, area; int mins[3], maxs[3]; int firstSurface, numSurfaces, firstBrush, numBrushes; };
struct DModel { float mins[3], maxs[3]; int firstSurface, numSurfaces, firstBrush, numBrushes; };
struct DSurface {
	int shader, fog, type, firstVert, numVerts, firstIndex, numIndexes;
	unsigned char lmStyles[4], vtxStyles[4];
	int lmNum[4], lmX[4], lmY[4], lmW, lmH;
	float lmOrigin[3], lmVecs[3][3];
	int patchW, patchH;
};
#pragma pack( pop )

struct Surf {
	int shader; int tris; bool translucent; int passes;
	float mins[3], maxs[3];           // bounds: the engine culls each surface against the view
	bool planar = false, twoSided = false; float normal[3], dist = 0; // planar faces: back-face culled (unless cull none)
};
struct ShaderInfo { CopiedString name; bool translucent = false; int passes = 1; bool twoSided = false; };

struct Bsp {
	bool loaded = false;
	CopiedString path;
	QDateTime time;
	std::vector<DPlane> planes;
	std::vector<DNode> nodes;
	std::vector<DLeaf> leafs;
	std::vector<int> leafSurfaces;
	std::vector<DModel> models;
	std::vector<Surf> surfs;
	std::vector<ShaderInfo> shaders;
	int clusters = 0, clusterBytes = 0;
	std::vector<unsigned char> vis;
	int lightmaps = 0;
	std::vector<std::vector<int>> clusterSurfs; // surfaces in each cluster's leafs
	bool visible( int from, int to ) const {
		if( vis.empty() || from < 0 || to < 0 )
			return true;
		return vis[std::size_t( from ) * clusterBytes + ( to >> 3 )] & ( 1 << ( to & 7 ) );
	}
};
Bsp g_bsp;

template<typename T>
std::vector<T> lump( const QByteArray& data, const Header& h, int i ){
	std::vector<T> out;
	const Lump& l = h.lumps[i];
	if( l.offset < 0 || l.length < 0 || std::size_t( l.offset ) + l.length > std::size_t( data.size() ) )
		return out;
	out.resize( l.length / sizeof( T ) );
	if( !out.empty() )
		std::memcpy( out.data(), data.constData() + l.offset, out.size() * sizeof( T ) );
	return out;
}

CopiedString bspPath(){
	return CopiedString( StringStream( PathExtensionless( Map_Name( g_map ) ), ".bsp" ) );
}

bool load(){
	if( Map_Unnamed( g_map ) )
		return false;
	const CopiedString path = bspPath();
	const QFileInfo info( path.c_str() );
	if( g_bsp.loaded && string_equal( g_bsp.path.c_str(), path.c_str() ) && info.lastModified() == g_bsp.time )
		return true;
	g_bsp = Bsp();
	QFile file( path.c_str() );
	if( !file.open( QIODevice::ReadOnly ) )
		return false;
	const QByteArray data = file.readAll();
	if( data.size() < int( sizeof( Header ) ) )
		return false;
	Header h;
	std::memcpy( &h, data.constData(), sizeof( h ) );
	if( std::strncmp( h.ident, "RBSP", 4 ) != 0 )
		return false;
	g_bsp.path = path;
	g_bsp.time = info.lastModified();
	const auto shaders = lump<DShader>( data, h, 1 );
	g_bsp.planes = lump<DPlane>( data, h, 2 );
	g_bsp.nodes = lump<DNode>( data, h, 3 );
	g_bsp.leafs = lump<DLeaf>( data, h, 4 );
	g_bsp.leafSurfaces = lump<int>( data, h, 5 );
	g_bsp.models = lump<DModel>( data, h, 7 );
	const auto surfaces = lump<DSurface>( data, h, 13 );
	struct DVert { float xyz[3]; float rest[17]; };
	static_assert( sizeof( DVert ) == 80, "RBSP vertex" );
	const auto verts = lump<DVert>( data, h, 10 );
	g_bsp.lightmaps = h.lumps[14].length / ( 128 * 128 * 3 );
	const auto visData = lump<unsigned char>( data, h, 16 );
	if( visData.size() >= 8 ){
		std::memcpy( &g_bsp.clusters, visData.data(), 4 );
		std::memcpy( &g_bsp.clusterBytes, visData.data() + 4, 4 );
		if( g_bsp.clusters > 0 && g_bsp.clusterBytes > 0 && visData.size() >= 8 + std::size_t( g_bsp.clusters ) * g_bsp.clusterBytes )
			g_bsp.vis.assign( visData.begin() + 8, visData.begin() + 8 + std::size_t( g_bsp.clusters ) * g_bsp.clusterBytes );
		else
			g_bsp.clusters = 0;
	}
	for( const DShader& s : shaders ){
		ShaderInfo si;
		char name[65] = {};
		std::memcpy( name, s.name, 64 );
		si.name = name;
		IShader* shader = GlobalShaderSystem().getShaderForName( name );
		si.translucent = ( shader->getFlags() & QER_TRANS ) != 0;
		if( const std::vector<KrStage>* st = shader->getStages(); st != nullptr && !st->empty() ){
			si.passes = int( st->size() );
			si.translucent = si.translucent || ( st->front().blend && !( st->front().src == BLEND_ONE && st->front().dst == BLEND_ZERO ) && st->front().special != 1 );
		}
		si.twoSided = ( shader->getFlags() & QER_CULL ) && shader->getCull() == IShader::eCullNone;
		if( shader->getFlags() & ( QER_SKY | QER_NODRAW ) )
			si.passes = 0; // sky is drawn apart, nodraw never
		shader->DecRef();
		g_bsp.shaders.push_back( si );
	}
	for( const DSurface& s : surfaces ){
		Surf o;
		o.shader = s.shader >= 0 && s.shader < int( g_bsp.shaders.size() ) ? s.shader : -1;
		o.tris = s.type == 2 ? std::max( 0, ( s.patchW - 1 ) * ( s.patchH - 1 ) * 4 ) // patches: tessellated by the engine (estimate)
		       : s.type == 4 ? 0 : s.numIndexes / 3;
		o.translucent = o.shader >= 0 && g_bsp.shaders[o.shader].translucent;
		o.passes = o.shader >= 0 ? g_bsp.shaders[o.shader].passes : 1;
		if( o.passes == 0 )
			o.tris = 0;
		for( int k = 0; k < 3; ++k ){ o.mins[k] = 1e30f; o.maxs[k] = -1e30f; }
		for( int v = s.firstVert; v >= 0 && v < s.firstVert + s.numVerts && v < int( verts.size() ); ++v )
			for( int k = 0; k < 3; ++k ){
				o.mins[k] = std::min( o.mins[k], verts[v].xyz[k] );
				o.maxs[k] = std::max( o.maxs[k], verts[v].xyz[k] );
			}
		o.twoSided = o.shader >= 0 && g_bsp.shaders[o.shader].twoSided;
		if( s.type == 1 && s.numVerts > 0 && s.firstVert >= 0 && s.firstVert < int( verts.size() ) ){
			o.planar = true;
			for( int k = 0; k < 3; ++k )
				o.normal[k] = s.lmVecs[2][k];
			o.dist = o.normal[0] * verts[s.firstVert].xyz[0] + o.normal[1] * verts[s.firstVert].xyz[1] + o.normal[2] * verts[s.firstVert].xyz[2];
		}
		g_bsp.surfs.push_back( o );
	}
	// surfaces per cluster (each once)
	const int clusters = std::max( g_bsp.clusters, 1 );
	g_bsp.clusterSurfs.assign( clusters, {} );
	std::vector<int> stamp( g_bsp.surfs.size(), -1 );
	std::vector<std::vector<int>> leafsOf( clusters );
	for( std::size_t i = 0; i < g_bsp.leafs.size(); ++i )
		if( g_bsp.leafs[i].cluster >= 0 && g_bsp.leafs[i].cluster < clusters )
			leafsOf[g_bsp.leafs[i].cluster].push_back( int( i ) );
	for( int c = 0; c < clusters; ++c )
		for( int li : leafsOf[c] ){
			const DLeaf& l = g_bsp.leafs[li];
			for( int k = 0; k < l.numSurfaces; ++k ){
				const int idx = l.firstSurface + k;
				if( idx < 0 || idx >= int( g_bsp.leafSurfaces.size() ) )
					continue;
				const int s = g_bsp.leafSurfaces[idx];
				if( s >= 0 && s < int( g_bsp.surfs.size() ) && stamp[s] != c ){
					stamp[s] = c;
					g_bsp.clusterSurfs[c].push_back( s );
				}
			}
		}
	g_bsp.loaded = true;
	return true;
}

int leafAt( const Vector3& p ){
	if( g_bsp.nodes.empty() )
		return -1;
	int n = 0;
	for( int guard = 0; n >= 0 && guard < 100000; ++guard ){
		const DNode& node = g_bsp.nodes[n];
		if( node.plane < 0 || node.plane >= int( g_bsp.planes.size() ) )
			return -1;
		const DPlane& pl = g_bsp.planes[node.plane];
		const float d = pl.normal[0] * p.x() + pl.normal[1] * p.y() + pl.normal[2] * p.z() - pl.dist;
		n = node.children[d >= 0 ? 0 : 1];
		if( n >= int( g_bsp.nodes.size() ) )
			return -1;
	}
	const int leaf = -( n + 1 );
	return leaf >= 0 && leaf < int( g_bsp.leafs.size() ) ? leaf : -1;
}

template<typename T>
bool boxInView( const Matrix4& viewproj, const T mins[3], const T maxs[3] ){
	int outside[6] = { 0, 0, 0, 0, 0, 0 };
	for( int k = 0; k < 8; ++k ){
		const Vector4 c = matrix4_transformed_vector4( viewproj, Vector4( float( ( k & 1 ) ? maxs[0] : mins[0] ), float( ( k & 2 ) ? maxs[1] : mins[1] ), float( ( k & 4 ) ? maxs[2] : mins[2] ), 1 ) );
		outside[0] += c.x() < -c.w(); outside[1] += c.x() > c.w();
		outside[2] += c.y() < -c.w(); outside[3] += c.y() > c.w();
		outside[4] += c.z() < -c.w(); outside[5] += c.z() > c.w();
	}
	for( int o : outside )
		if( o == 8 )
			return false;
	return true;
}

/// the engine's own per-surface culling (R_CullSurface): outside the view, or a one-sided face seen from behind
bool surfaceInView( const Surf& s, const Matrix4& viewproj, const Vector3& eye ){
	if( s.mins[0] > s.maxs[0] )
		return false;
	if( s.planar && !s.twoSided && s.normal[0] * eye.x() + s.normal[1] * eye.y() + s.normal[2] * eye.z() < s.dist - 8 )
		return false;
	return boxInView( viewproj, s.mins, s.maxs );
}

struct Counts {
	int surfaces = 0, tris = 0, passes = 0, translucent = 0, shaders = 0, leafs = 0;
	std::map<int, std::pair<int, int>> byShader; // shader -> ( tris, surfaces )
	void add( const Surf& s, std::vector<char>& shaderSeen ){
		if( s.passes == 0 )
			return;
		++surfaces;
		tris += s.tris;
		passes += s.passes;
		translucent += s.translucent;
		if( s.shader >= 0 ){
			if( !shaderSeen[s.shader] ){
				shaderSeen[s.shader] = 1;
				++shaders;
			}
			auto& b = byShader[s.shader];
			b.first += s.tris;
			b.second += 1;
		}
	}
};

/// what the engine draws from a point: PVS of its cluster; with a view, only leafs in the frustum
Counts countFrom( const Vector3& eye, const Matrix4* viewproj, int* clusterOut = nullptr ){
	Counts c;
	const int leaf = leafAt( eye );
	const int cluster = leaf >= 0 ? g_bsp.leafs[leaf].cluster : -1;
	if( clusterOut != nullptr )
		*clusterOut = cluster;
	std::vector<char> seen( g_bsp.surfs.size(), 0 ), shaderSeen( g_bsp.shaders.size(), 0 );
	const int worldSurfaces = g_bsp.models.empty() ? int( g_bsp.surfs.size() ) : g_bsp.models[0].firstSurface + g_bsp.models[0].numSurfaces;
	for( const DLeaf& l : g_bsp.leafs ){
		if( l.cluster < 0 || ( cluster >= 0 && !g_bsp.visible( cluster, l.cluster ) ) )
			continue;
		if( viewproj != nullptr && !boxInView( *viewproj, l.mins, l.maxs ) )
			continue;
		++c.leafs;
		for( int k = 0; k < l.numSurfaces; ++k ){
			const int idx = l.firstSurface + k;
			if( idx < 0 || idx >= int( g_bsp.leafSurfaces.size() ) )
				continue;
			const int s = g_bsp.leafSurfaces[idx];
			if( s < 0 || s >= worldSurfaces || seen[s] )
				continue;
			seen[s] = 1;
			if( viewproj != nullptr && !surfaceInView( g_bsp.surfs[s], *viewproj, eye ) )
				continue;
			c.add( g_bsp.surfs[s], shaderSeen );
		}
	}
	// brush entities (func_*): when their middle is in a visible cluster
	for( std::size_t m = 1; m < g_bsp.models.size(); ++m ){
		const DModel& model = g_bsp.models[m];
		const Vector3 mid( ( model.mins[0] + model.maxs[0] ) / 2, ( model.mins[1] + model.maxs[1] ) / 2, ( model.mins[2] + model.maxs[2] ) / 2 );
		const int ml = leafAt( mid );
		if( ml >= 0 && cluster >= 0 && !g_bsp.visible( cluster, g_bsp.leafs[ml].cluster ) )
			continue;
		if( viewproj != nullptr ){
			const int mins[3] = { int( model.mins[0] ), int( model.mins[1] ), int( model.mins[2] ) }, maxs[3] = { int( model.maxs[0] ), int( model.maxs[1] ), int( model.maxs[2] ) };
			if( !boxInView( *viewproj, mins, maxs ) )
				continue;
		}
		for( int s = model.firstSurface; s < model.firstSurface + model.numSurfaces && s < int( g_bsp.surfs.size() ); ++s )
			if( s >= 0 && !seen[s] ){
				seen[s] = 1;
				if( viewproj != nullptr && !surfaceInView( g_bsp.surfs[s], *viewproj, eye ) )
					continue;
				c.add( g_bsp.surfs[s], shaderSeen );
			}
	}
	return c;
}

/// PVS triangles of a cluster (no frustum: everything the engine may draw somewhere in that cluster)
Counts countCluster( int cluster ){
	Counts c;
	std::vector<char> seen( g_bsp.surfs.size(), 0 ), shaderSeen( g_bsp.shaders.size(), 0 );
	for( int v = 0; v < int( g_bsp.clusterSurfs.size() ); ++v ){
		if( !g_bsp.visible( cluster, v ) )
			continue;
		for( int s : g_bsp.clusterSurfs[v] )
			if( !seen[s] ){
				seen[s] = 1;
				c.add( g_bsp.surfs[s], shaderSeen );
			}
	}
	return c;
}

/// a standing point inside a cluster: the middle of its biggest leaf, a little above the floor
Vector3 clusterSpot( int cluster ){
	const DLeaf* best = nullptr;
	double bestVolume = -1;
	for( const DLeaf& l : g_bsp.leafs )
		if( l.cluster == cluster ){
			const double v = double( l.maxs[0] - l.mins[0] ) * ( l.maxs[1] - l.mins[1] ) * ( l.maxs[2] - l.mins[2] );
			if( v > bestVolume ){
				bestVolume = v;
				best = &l;
			}
		}
	if( best == nullptr )
		return Vector3( 0, 0, 0 );
	return Vector3( ( best->mins[0] + best->maxs[0] ) * 0.5f, ( best->mins[1] + best->maxs[1] ) * 0.5f,
	                std::min( float( best->mins[2] ) + 64.f, ( best->mins[2] + best->maxs[2] ) * 0.5f ) );
}

int portalCount(){
	QFile f( QString( StringStream( PathExtensionless( Map_Name( g_map ) ), ".prt" ).c_str() ) );
	if( !f.open( QIODevice::ReadOnly | QIODevice::Text ) )
		return -1;
	QTextStream t( &f );
	t.readLine(); // PRT1
	t.readLine(); // clusters
	return t.readLine().trimmed().toInt();
}

// rough budgets for SOF2 (CPU-bound fixed pipeline engine): good / heavy
constexpr int c_trisGood = 20000, c_trisHeavy = 45000;
constexpr int c_surfGood = 700, c_surfHeavy = 1400;
constexpr int c_passGood = 1200, c_passHeavy = 2500;

QDialog* g_window = nullptr;
QLabel* g_here = nullptr;
QProgressBar* g_bars[3] = {};
QTreeWidget* g_shaders = nullptr;
QTreeWidget* g_spots = nullptr;
QTreeWidget* g_advice = nullptr;
Vector3 g_lastEye( 1e9f, 0, 0 ), g_lastAngles( 0, 0, 0 );

void setBar( QProgressBar* bar, int value, int good, int heavy, const char* what ){
	bar->setMaximum( heavy * 3 / 2 );
	bar->setValue( std::min( value, bar->maximum() ) );
	bar->setFormat( QString( "%1 %2" ).arg( value ).arg( what ) );
	const char* colour = value <= good ? "#3c9a4a" : value <= heavy ? "#c89a2a" : "#c0392b";
	bar->setStyleSheet( QString( "QProgressBar{ text-align: center; } QProgressBar::chunk{ background: %1; }" ).arg( colour ) );
	bar->setToolTip( QString( "%1: up to %2 is light, above %3 is heavy for SOF2 (rough guide - check with r_speeds 1 in game)" ).arg( what ).arg( good ).arg( heavy ) );
}

void updateHere( bool force ){
	if( g_window == nullptr || !g_window->isVisible() )
		return;
	Vector3 eye, angles;
	float fov;
	KrCamera_getView( eye, angles, fov );
	if( !force && vector3_equal_epsilon( eye, g_lastEye, 0.5f ) && vector3_equal_epsilon( angles, g_lastAngles, 0.2f ) )
		return;
	g_lastEye = eye; g_lastAngles = angles;
	if( !load() ){
		g_here->setText( "No compiled .bsp for this map - compile it (F9), then the numbers are what the game draws." );
		return;
	}
	Matrix4 mv, proj;
	if( !KrCamera_getMatrices( mv, proj ) )
		return;
	const Matrix4 viewproj = matrix4_multiplied_by_matrix4( proj, mv );
	int cluster = -1;
	const Counts view = countFrom( eye, &viewproj, &cluster );
	const Counts pvs = countFrom( eye, nullptr );
	const bool stale = QFileInfo( Map_Name( g_map ) ).lastModified() > g_bsp.time;
	g_here->setText( QString( "<b>Here</b> (cluster %1): in view <b>%2</b> triangles, <b>%3</b> surfaces, %4 passes, %5 shaders, %6 blended, %11 leafs"
	                          " &nbsp;&middot;&nbsp; whole PVS %7 triangles, %8 surfaces%9%10<br><span style='color:gray'>In game: r_speeds 1 shows shaders/surfaces and leafs - compare with the setviewpos line of the Lighting Probe.</span>" )
	                 .arg( cluster ).arg( view.tris ).arg( view.surfaces ).arg( view.passes ).arg( view.shaders ).arg( view.translucent )
	                 .arg( pvs.tris ).arg( pvs.surfaces )
	                 .arg( cluster < 0 ? "<br><span style='color:#c0392b'>The camera is outside the map (in the void): everything counts.</span>" : "" )
	                 .arg( stale ? "<br><span style='color:#c89a2a'>The map was saved after this compile - numbers are from the last compile.</span>" : "" )
	                 .arg( view.leafs ) );
	setBar( g_bars[0], view.tris, c_trisGood, c_trisHeavy, "triangles" );
	setBar( g_bars[1], view.surfaces, c_surfGood, c_surfHeavy, "surfaces (draw calls)" );
	setBar( g_bars[2], view.passes, c_passGood, c_passHeavy, "passes (surfaces x shader stages)" );
	std::vector<std::pair<int, int>> order;
	for( const auto& [shader, v] : view.byShader )
		order.emplace_back( v.first, shader );
	std::sort( order.rbegin(), order.rend() );
	g_shaders->clear();
	for( std::size_t i = 0; i < order.size() && i < 12; ++i ){
		const int sh = order[i].second;
		auto *item = new QTreeWidgetItem( g_shaders );
		item->setText( 0, g_bsp.shaders[sh].name.c_str() );
		item->setText( 1, QString::number( view.byShader.at( sh ).first ) );
		item->setText( 2, QString::number( view.byShader.at( sh ).second ) );
		item->setText( 3, QString::number( g_bsp.shaders[sh].passes ) );
	}
}

void addAdvice( const QString& text, const QString& button, std::function<void()> action ){
	auto *item = new QTreeWidgetItem( g_advice );
	item->setText( 0, text );
	item->setToolTip( 0, text );
	if( !button.isEmpty() ){
		auto *b = new QPushButton( button );
		QObject::connect( b, &QPushButton::clicked, action );
		g_advice->setItemWidget( item, 1, b );
	}
}

void scan(){
	if( !load() ){
		g_here->setText( "No compiled .bsp for this map - compile it first (F9)." );
		return;
	}
	g_spots->clear();
	g_advice->clear();
	struct Spot { int cluster; Counts c; };
	std::vector<Spot> spots;
	for( int c = 0; c < g_bsp.clusters; ++c )
		spots.push_back( { c, countCluster( c ) } );
	std::sort( spots.begin(), spots.end(), []( const Spot& a, const Spot& b ){ return a.c.tris > b.c.tris; } );
	for( std::size_t i = 0; i < spots.size() && i < 12; ++i ){
		const Spot& s = spots[i];
		auto *item = new QTreeWidgetItem( g_spots );
		item->setText( 0, QString::number( s.cluster ) );
		item->setText( 1, QString::number( s.c.tris ) );
		item->setText( 2, QString::number( s.c.surfaces ) );
		item->setText( 3, QString::number( s.c.passes ) );
		std::vector<std::pair<int, int>> order;
		for( const auto& [shader, v] : s.c.byShader )
			order.emplace_back( v.first, shader );
		std::sort( order.rbegin(), order.rend() );
		QStringList top;
		for( std::size_t k = 0; k < order.size() && k < 3; ++k )
			top << QString( "%1 (%2)" ).arg( QString( g_bsp.shaders[order[k].second].name.c_str() ).section( '/', -1 ) ).arg( order[k].first );
		item->setText( 4, top.join( ", " ) );
		auto *go = new QPushButton( "Go" );
		const int cluster = s.cluster;
		QObject::connect( go, &QPushButton::clicked, [cluster](){
			Vector3 eye, angles;
			float fov;
			KrCamera_getView( eye, angles, fov );
			KrCamera_setView( clusterSpot( cluster ), angles );
		} );
		g_spots->setItemWidget( item, 5, go );
	}
	// suggestions
	if( g_bsp.vis.empty() )
		addAdvice( "No VIS data: the game draws the whole map from everywhere. Run the VIS stage (Compile window, VIS ticked).", "", {} );
	if( !spots.empty() && spots.front().c.tris > c_trisHeavy )
		addAdvice( QString( "The heaviest spot sees %1 triangles (PVS). Block long sight lines (walls / structural brushes), add hint brushes "
		                    "at doorways and corners, or lower-poly the top shaders listed there." ).arg( spots.front().c.tris ), "Go there", [c = spots.front().cluster](){
			Vector3 eye, angles;
			float fov;
			KrCamera_getView( eye, angles, fov );
			KrCamera_setView( clusterSpot( c ), angles );
		} );
	if( !spots.empty() && spots.front().c.tris > 0 ){
		int modelTris = 0;
		for( const auto& [shader, v] : spots.front().c.byShader )
			if( string_equal_prefix( g_bsp.shaders[shader].name.c_str(), "models/" ) )
				modelTris += v.first;
		const int share = modelTris * 100 / spots.front().c.tris;
		if( share >= 40 )
			addAdvice( QString( "%1% of the triangles in the heaviest spot are baked models (misc_model, models/... shaders). "
			                    "Lower-poly versions, fewer copies in open areas, or a spawnflag / distance cull for small props help most." ).arg( share ), "", {} );
	}
	int structural = 0, detail = 0;
	const int small = Scene_smallStructural( 64, false, structural, detail );
	const int portals = portalCount();
	if( small > 0 && ( portals > 1500 || small > 200 ) )
		addAdvice( QString( "%1 small structural brushes (64 units or less) - they cut %2 vis portals without blocking sight. "
		                    "Make them detail: faster VIS, fewer leafs, usually fewer draws. (%3 structural, %4 detail in the map.)" )
		           .arg( small ).arg( portals < 0 ? QString( "the" ) : QString::number( portals ) ).arg( structural ).arg( detail ),
		           "Make them detail", [](){
			int s, d;
			Scene_smallStructural( 64, true, s, d );
			Select_MakeDetail(); // undoable
			globalOutputStream() << "performance: made the small structural brushes detail (Undo to take it back) - compile again\n";
		} );
	if( small > 0 )
		addAdvice( QString( "Select the %1 small structural brushes to look at them first." ).arg( small ), "Select", [](){
			int s, d;
			Scene_smallStructural( 64, true, s, d );
		} );
	if( g_bsp.lightmaps > 96 )
		addAdvice( QString( "%1 lightmaps (128 x 128): use -samplesize 16+ or q3map_lightmapSampleSize on big, plain surfaces - less memory, faster loading." ).arg( g_bsp.lightmaps ), "", {} );
	std::map<int, int> heavyShaders; // shader -> passes, among the worst spots
	for( std::size_t i = 0; i < spots.size() && i < 12; ++i )
		for( const auto& [shader, v] : spots[i].c.byShader )
			if( g_bsp.shaders[shader].passes >= 3 && v.second >= 20 )
				heavyShaders[shader] = g_bsp.shaders[shader].passes;
	for( const auto& [shader, passes] : heavyShaders )
		addAdvice( QString( "%1 has %2 stages and is used a lot in the heavy spots: every stage is another pass in SOF2's renderer." )
		           .arg( g_bsp.shaders[shader].name.c_str() ).arg( passes ), "", {} );
	if( g_advice->topLevelItemCount() == 0 )
		addAdvice( "Nothing stands out - the map is within the usual budgets.", "", {} );
	updateHere( true );
}

} // namespace

void KrPerf_show(){
	if( g_window == nullptr ){
		g_window = new QDialog( MainFrame_getWindow(), Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint );
		g_window->setWindowTitle( "Performance" );
		auto *v = new QVBoxLayout( g_window );
		g_here = new QLabel;
		g_here->setWordWrap( true );
		g_here->setTextFormat( Qt::RichText );
		v->addWidget( g_here );
		for( auto*& bar : g_bars ){
			bar = new QProgressBar;
			bar->setTextVisible( true );
			v->addWidget( bar );
		}
		v->addWidget( new QLabel( "Heaviest shaders in view:" ) );
		g_shaders = new QTreeWidget;
		g_shaders->setHeaderLabels( { "Shader", "Triangles", "Surfaces", "Stages" } );
		g_shaders->setRootIsDecorated( false );
		g_shaders->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
		g_shaders->setMinimumHeight( 150 );
		v->addWidget( g_shaders );
		auto *row = new QHBoxLayout;
		auto *scanButton = new QPushButton( "Find worst spots + suggestions" );
		scanButton->setToolTip( "Goes through every vis cluster of the compiled map: where the game has the most to draw" );
		row->addWidget( scanButton );
		row->addStretch( 1 );
		v->addLayout( row );
		g_spots = new QTreeWidget;
		g_spots->setHeaderLabels( { "Cluster", "Triangles", "Surfaces", "Passes", "Top shaders", "" } );
		g_spots->setRootIsDecorated( false );
		g_spots->header()->setSectionResizeMode( 4, QHeaderView::Stretch );
		g_spots->setMinimumHeight( 170 );
		v->addWidget( g_spots );
		v->addWidget( new QLabel( "Suggestions:" ) );
		g_advice = new QTreeWidget;
		g_advice->setHeaderLabels( { "", "" } );
		g_advice->setHeaderHidden( true );
		g_advice->setRootIsDecorated( false );
		g_advice->setWordWrap( true );
		g_advice->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
		g_advice->header()->setStretchLastSection( false );
		g_advice->setMinimumHeight( 120 );
		v->addWidget( g_advice );
		auto *note = new QLabel( "Numbers come from the compiled .bsp the way the engine picks what to draw (vis cluster, PVS, view). "
		                         "Models placed as entities and effects are not counted. Patches are estimated." );
		note->setWordWrap( true );
		note->setStyleSheet( "color: gray;" );
		v->addWidget( note );
		g_window->resize( 820, 760 );
		QObject::connect( scanButton, &QPushButton::clicked, [](){ scan(); } );
		auto *timer = new QTimer( g_window );
		QObject::connect( timer, &QTimer::timeout, [](){ updateHere( false ); } );
		timer->start( 300 );
	}
	static const bool remembered = ( g_guiSettings.addWindow( g_window, "KenRadiant/Performance" ), true ); // where you left it, next time too
	(void)remembered;
	g_window->show();
	g_window->raise();
	updateHere( true );
}

void KrPerf_registerCommands(){
	GlobalCommands_insert( "KrPerformance", makeCallbackF( KrPerf_show ) );
}
