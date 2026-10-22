/* -------------------------------------------------------------------------------

   -mergesurfaces (SOF2 branch)

   Post-light pass that cuts the number of world drawsurfaces, for Q3-family
   engines whose CPU cost scales with surfaces drawn (SOF2: per-surface overhead,
   ~27k surfaces in view on CoD4-converted maps).

   Runs on a finished (lit) bsp. Because lightmap UVs are final at this point,
   surfaces are only merged with others on the same lightmap page, so no luxel is
   wasted and the (shader, lightmap) pair count cannot grow.

   Merge kinds
   - face: MST_PLANAR, same shader/fog/lightmap pages/styles, same normal, plane
     distances within -planerange (default 64), within one -cell (default 1024).
     Written as one MST_PLANAR with at most maxLMSurfaceVerts verts (64 on SOF2 =
     renderer MAX_FACE_POINTS). The rearmost piece goes first: the renderer derives
     the face plane from the first vertex, and back-face culling against the
     rearmost plane never culls a piece the viewer can see.
   - soup: vertex-lit triangle soups (baked misc_models), plus vertex-lit faces
     whose shader has surfaceparm nomarks (a soup gets no bullet marks, so only
     where none are wanted). Up to maxSurfaceVerts / maxSurfaceIndexes.

   Never merged: patches, flares, foliage, lightmapped soups, autosprite, sky.
   Leaf-surface and brushside->surface links are remapped.

   ------------------------------------------------------------------------------- */

#include "q3map2.h"
#include <map>
#include <tuple>
#include <algorithm>
#include <cmath>

namespace
{

enum class MergeKind { Keep, Face, Soup };

struct MergeKey
{
	MergeKind kind;
	int shaderNum, fogNum;
	std::array<int, 4> lightmapNum;
	std::array<int, 4> styles;   // lightmap styles
	std::array<int, 4> vstyles;  // vertex styles
	std::array<int, 3> normal;   // quantized, faces only
	std::array<int, 3> cell;
	auto tie() const { return std::tie( kind, shaderNum, fogNum, lightmapNum, styles, vstyles, normal, cell ); }
	bool operator<( const MergeKey& o ) const { return tie() < o.tie(); }
};

struct Piece
{
	int surf;
	double dist;
};

}

/* merge the drawsurfaces of a lit bsp file in place; shaders must already be loaded */
int MergeSurfacesBSP( const char *bspPath, double cellSize, double planeRange, bool soups ){
	Sys_Printf( "--- MergeSurfaces ---\n" );
	Sys_Printf( "cell %g units, plane range %g units, soups %s, face vert limit %d, soup limits %d verts / %d indexes\n",
	            cellSize, planeRange, soups ? "on" : "off", maxLMSurfaceVerts, maxSurfaceVerts, maxSurfaceIndexes );

	Sys_Printf( "Loading %s\n", bspPath );
	LoadBSPFile( bspPath );
	ParseEntities();

	/* per-map overrides from worldspawn (front-ends that can't pass arguments can set these in the .map) */
	if ( const char *v = entities[ 0 ].valueForKey( "_mergecell" ); !strEmpty( v ) ) {
		cellSize = std::max( 64.0, atof( v ) );
		Sys_Printf( "worldspawn _mergecell %g\n", cellSize );
	}
	if ( const char *v = entities[ 0 ].valueForKey( "_mergeplanerange" ); !strEmpty( v ) ) {
		planeRange = std::max( 0.0, atof( v ) );
		Sys_Printf( "worldspawn _mergeplanerange %g\n", planeRange );
	}

	if ( entities[ 0 ].boolForKey( "_mergedsurfaces" ) ) {
		Sys_Printf( "bsp already merged, nothing to do\n" );
		return 0;
	}
	if ( bspLightBytes.empty() ) {
		Sys_Warning( "bsp has no lightmaps: run -mergesurfaces after -light (relighting a merged bsp wastes lightmap space)\n" );
	}

	const int oldNumSurfs = bspDrawSurfaces.size();
	std::vector<int> remap( oldNumSurfs, -1 );
	std::vector<bspDrawSurface_t> newSurfs;
	DrawVerts newVerts;
	DrawIndexes newIndexes;
	newSurfs.reserve( oldNumSurfs );
	newVerts.reserve( bspDrawVerts.size() );
	newIndexes.reserve( bspDrawIndexes.size() );

	int numFaceIn = 0, numFaceOut = 0, numSoupIn = 0, numSoupOut = 0, numKept = 0;

	const auto copySurface = [&]( int s ) -> bspDrawSurface_t& {
		bspDrawSurface_t out = bspDrawSurfaces[ s ];
		out.firstVert = newVerts.size();
		out.firstIndex = newIndexes.size();
		newVerts.insert( newVerts.end(), bspDrawVerts.begin() + bspDrawSurfaces[ s ].firstVert,
		                 bspDrawVerts.begin() + bspDrawSurfaces[ s ].firstVert + bspDrawSurfaces[ s ].numVerts );
		newIndexes.insert( newIndexes.end(), bspDrawIndexes.begin() + bspDrawSurfaces[ s ].firstIndex,
		                   bspDrawIndexes.begin() + bspDrawSurfaces[ s ].firstIndex + bspDrawSurfaces[ s ].numIndexes );
		remap[ s ] = newSurfs.size();
		return newSurfs.emplace_back( out );
	};

	/* emit a group of pieces as one surface (pieces already ordered) */
	const auto emitMerged = [&]( const std::vector<int>& pieces, MergeKind kind ){
		bspDrawSurface_t& out = copySurface( pieces.front() );
		for ( size_t p = 1; p < pieces.size(); ++p ){
			const bspDrawSurface_t& ds = bspDrawSurfaces[ pieces[ p ] ];
			const int base = out.numVerts;
			newVerts.insert( newVerts.end(), bspDrawVerts.begin() + ds.firstVert, bspDrawVerts.begin() + ds.firstVert + ds.numVerts );
			for ( int i = 0; i < ds.numIndexes; ++i )
				newIndexes.push_back( bspDrawIndexes[ ds.firstIndex + i ] + base );
			out.numVerts += ds.numVerts;
			out.numIndexes += ds.numIndexes;
			remap[ pieces[ p ] ] = remap[ pieces.front() ];
		}
		if ( kind == MergeKind::Soup ) {
			out.surfaceType = MST_TRIANGLE_SOUP;
			out.lightmapVecs[ 0 ] = out.lightmapVecs[ 1 ] = out.lightmapVecs[ 2 ] = Vector3( 0 );
		}
		else{
			/* bounds of all pieces' lightmap rects on the (shared) page */
			int x0 = out.lightmapX[ 0 ], y0 = out.lightmapY[ 0 ], x1 = x0 + out.lightmapWidth, y1 = y0 + out.lightmapHeight;
			for ( const int s : pieces ){
				const bspDrawSurface_t& ds = bspDrawSurfaces[ s ];
				x0 = std::min( x0, ds.lightmapX[ 0 ] ); y0 = std::min( y0, ds.lightmapY[ 0 ] );
				x1 = std::max( x1, ds.lightmapX[ 0 ] + ds.lightmapWidth ); y1 = std::max( y1, ds.lightmapY[ 0 ] + ds.lightmapHeight );
			}
			if ( out.lightmapNum[ 0 ] >= 0 ) {
				out.lightmapX[ 0 ] = x0; out.lightmapY[ 0 ] = y0;
				out.lightmapWidth = x1 - x0; out.lightmapHeight = y1 - y0;
			}
		}
		( kind == MergeKind::Face ? numFaceOut : numSoupOut )++;
	};

	for ( bspModel_t& model : bspModels )
	{
		const int first = model.firstBSPSurface, last = model.firstBSPSurface + model.numBSPSurfaces;
		const int newFirst = newSurfs.size();
		std::map<MergeKey, std::vector<Piece>> groups;

		for ( int s = first; s < last; ++s )
		{
			const bspDrawSurface_t& ds = bspDrawSurfaces[ s ];
			MergeKind kind = MergeKind::Keep;
			const shaderInfo_t *si = ( ds.shaderNum >= 0 && ds.shaderNum < int( bspShaders.size() ) )
			                         ? &ShaderInfoForShader( bspShaders[ ds.shaderNum ].shader ) : nullptr;
			const bool vertexLit = ds.lightmapNum[ 0 ] < 0;
			if ( si != nullptr && !si->autosprite && !( si->compileFlags & C_SKY ) && ds.numVerts > 0 && ds.numIndexes >= 3 ) {
				if ( ds.surfaceType == MST_PLANAR && ds.numVerts <= maxLMSurfaceVerts ) {
					kind = ( soups && vertexLit && ( si->compileFlags & C_NOMARKS ) ) ? MergeKind::Soup : MergeKind::Face;
				}
				else if ( ds.surfaceType == MST_TRIANGLE_SOUP && vertexLit && soups ) {
					kind = MergeKind::Soup;
				}
			}
			if ( kind == MergeKind::Soup && ( ds.numVerts > maxSurfaceVerts || ds.numIndexes > maxSurfaceIndexes ) ) {
				kind = MergeKind::Keep;
			}
			if ( kind == MergeKind::Keep ) {
				copySurface( s );
				++numKept;
				continue;
			}

			MinMax mm;
			for ( int v = 0; v < ds.numVerts; ++v )
				mm.extend( bspDrawVerts[ ds.firstVert + v ].xyz );
			const Vector3 c = mm.origin();

			MergeKey key{};
			key.kind = kind;
			key.shaderNum = ds.shaderNum;
			key.fogNum = ds.fogNum;
			for ( int i = 0; i < 4; ++i ){
				key.lightmapNum[ i ] = ds.lightmapNum[ i ];
				key.styles[ i ] = ds.lightmapStyles[ i ];
				key.vstyles[ i ] = ds.vertexStyles[ i ];
			}
			for ( int i = 0; i < 3; ++i )
				key.cell[ i ] = int( std::floor( c[ i ] / cellSize ) );
			double dist = 0;
			if ( kind == MergeKind::Face ) {
				const Vector3& n = ds.lightmapVecs[ 2 ];
				for ( int i = 0; i < 3; ++i )
					key.normal[ i ] = int( std::lround( n[ i ] * 4096.0 ) );
				dist = vector3_dot( n, bspDrawVerts[ ds.firstVert ].xyz );
				++numFaceIn;
			}
			else{
				++numSoupIn;
			}
			groups[ key ].push_back( { s, dist } );
		}

		for ( auto& [ key, pieces ] : groups )
		{
			const int vertCap = key.kind == MergeKind::Face ? maxLMSurfaceVerts : maxSurfaceVerts;
			const int indexCap = maxSurfaceIndexes;

			/* faces: split into runs whose plane distances lie within planeRange */
			std::vector<std::vector<Piece>> runs;
			if ( key.kind == MergeKind::Face ) {
				std::ranges::sort( pieces, []( const Piece& a, const Piece& b ){ return a.dist < b.dist; } );
				for ( const Piece& p : pieces ){
					if ( runs.empty() || p.dist - runs.back().front().dist > planeRange )
						runs.emplace_back();
					runs.back().push_back( p );
				}
			}
			else{
				runs.push_back( pieces );
			}

			for ( auto& run : runs )
			{
				/* first-fit decreasing into bins of vertCap verts / indexCap indexes */
				std::ranges::stable_sort( run, []( const Piece& a, const Piece& b ){
					return bspDrawSurfaces[ a.surf ].numVerts > bspDrawSurfaces[ b.surf ].numVerts;
				} );
				struct Bin { int verts = 0, indexes = 0; std::vector<Piece> pieces; };
				std::vector<Bin> bins;
				for ( const Piece& p : run ){
					const bspDrawSurface_t& ds = bspDrawSurfaces[ p.surf ];
					Bin *target = nullptr;
					for ( Bin& b : bins )
						if ( b.verts + ds.numVerts <= vertCap && b.indexes + ds.numIndexes <= indexCap ) { target = &b; break; }
					if ( target == nullptr )
						target = &bins.emplace_back();
					target->verts += ds.numVerts;
					target->indexes += ds.numIndexes;
					target->pieces.push_back( p );
				}
				for ( Bin& b : bins ){
					/* rearmost plane first (face plane comes from the first vertex) */
					std::ranges::stable_sort( b.pieces, []( const Piece& a, const Piece& c ){ return a.dist < c.dist; } );
					std::vector<int> ids;
					for ( const Piece& p : b.pieces )
						ids.push_back( p.surf );
					emitMerged( ids, key.kind );
				}
			}
		}

		model.firstBSPSurface = newFirst;
		model.numBSPSurfaces = newSurfs.size() - newFirst;
	}

	/* leaf surfaces: remap and drop duplicates within a leaf */
	std::vector<int> newLeafSurfaces;
	newLeafSurfaces.reserve( bspLeafSurfaces.size() );
	for ( bspLeaf_t& leaf : bspLeafs ){
		const int start = newLeafSurfaces.size();
		for ( int i = 0; i < leaf.numBSPLeafSurfaces; ++i ){
			const int old = bspLeafSurfaces[ leaf.firstBSPLeafSurface + i ];
			const int n = ( old >= 0 && old < oldNumSurfs ) ? remap[ old ] : old;
			if ( std::find( newLeafSurfaces.begin() + start, newLeafSurfaces.end(), n ) == newLeafSurfaces.end() )
				newLeafSurfaces.push_back( n );
		}
		leaf.firstBSPLeafSurface = start;
		leaf.numBSPLeafSurfaces = newLeafSurfaces.size() - start;
	}

	/* brushside -> surface links (RBSP) */
	for ( bspBrushSide_t& side : bspBrushSides )
		if ( side.surfaceNum >= 0 && side.surfaceNum < oldNumSurfs )
			side.surfaceNum = remap[ side.surfaceNum ];

	const size_t oldLeafSurfs = bspLeafSurfaces.size();
	bspDrawSurfaces = std::move( newSurfs );
	bspDrawVerts = std::move( newVerts );
	bspDrawIndexes = std::move( newIndexes );
	bspLeafSurfaces = std::move( newLeafSurfaces );

	Sys_Printf( "%9d faces merged into %d\n", numFaceIn, numFaceOut );
	Sys_Printf( "%9d vertex-lit soups/nomarks faces merged into %d\n", numSoupIn, numSoupOut );
	Sys_Printf( "%9d surfaces left as they were (patches, flares, lightmapped soups, ...)\n", numKept );
	Sys_Printf( "%9d -> %zu drawsurfaces, %zu -> %zu leaf surface refs\n", oldNumSurfs, bspDrawSurfaces.size(), oldLeafSurfs, bspLeafSurfaces.size() );

	entities[ 0 ].setKeyValue( "_mergedsurfaces", "1" );
	UnparseEntities();
	Sys_Printf( "Writing %s\n", bspPath );
	WriteBSPFile( bspPath );
	return 0;
}

int MergeSurfacesMain( Args& args ){
	double cellSize = 1024, planeRange = 64;
	bool soups = true;

	if ( args.empty() ) {
		Sys_Printf( "Usage: q3map2 -game sof2 -mergesurfaces [-cell 1024] [-planerange 64] [-nosoups] <mapname>\n" );
		return 0;
	}
	const char *fileName = args.takeBack();
	while ( args.takeArg( "-cell" ) ) {
		cellSize = std::max( 64.0, atof( args.takeNext() ) );
	}
	while ( args.takeArg( "-planerange" ) ) {
		planeRange = std::max( 0.0, atof( args.takeNext() ) );
	}
	while ( args.takeArg( "-nosoups" ) ) {
		soups = false;
	}

	strcpy( source, ExpandArg( fileName ) );
	path_set_extension( source, ".bsp" );

	LoadShaderInfo();
	return MergeSurfacesBSP( source, cellSize, planeRange, soups );
}
