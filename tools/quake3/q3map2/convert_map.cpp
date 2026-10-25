/* -------------------------------------------------------------------------------

   Copyright (C) 1999-2007 id Software, Inc. and contributors.
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

   ----------------------------------------------------------------------------------

   This code has been altered significantly from its original form, to support
   several games based on the Quake III Arena engine, in the form of "Q3Map2."

   ------------------------------------------------------------------------------- */



/* dependencies */
#include "q3map2.h"
#include "bspfile_rbsp.h"
#include "qspatial.h"
#include <map>
#include "convert_map_exact.h"
#include <set>



/*
   ConvertBrush()
   exports a map brush
 */

inline double Det3x3( double a00, double a01, double a02,
                      double a10, double a11, double a12,
                      double a20, double a21, double a22 ){
	return
	        a00 * ( a11 * a22 - a12 * a21 )
	    -   a01 * ( a10 * a22 - a12 * a20 )
	    +   a02 * ( a10 * a21 - a11 * a20 );
}

struct BspTriangleRef
{
	bspSurfaceType_t surfaceType;
	TriRef tri;
	MinMax minmax; // X is on c_spatial_sort_direction

	BspTriangleRef( bspSurfaceType_t surfaceType, const bspDrawVert_t& v0, const bspDrawVert_t& v1, const bspDrawVert_t& v2 )
	:	surfaceType( surfaceType ),
		tri{ &v0, &v1, &v2 }
	{
		minmax.extend( Vector3( spatial_distance( v0.xyz ), v0.xyz.y(), v0.xyz.z() ) );
		minmax.extend( Vector3( spatial_distance( v1.xyz ), v1.xyz.y(), v1.xyz.z() ) );
		minmax.extend( Vector3( spatial_distance( v2.xyz ), v2.xyz.y(), v2.xyz.z() ) );
	}
	bool operator<( const BspTriangleRef& other ) const noexcept {
		return minmax.maxs.x() < other.minmax.maxs.x();
	}
};

class ModelTriangles
{
	std::map<CopiedString, std::vector<BspTriangleRef>> m_modelTriangles;
public:
	/* excludeSurfs: surfaces already owned by a brush side (RBSP side->surface links) - never candidates for other sides */
	ModelTriangles( const bspModel_t& model, const std::vector<char> *excludeSurfs = nullptr ){
		for ( const bspDrawSurface_t& s : Span( &bspDrawSurfaces[ model.firstBSPSurface ], model.numBSPSurfaces ) )
		{
			if ( excludeSurfs != nullptr && ( s.surfaceType != MST_PLANAR || ( *excludeSurfs )[ &s - bspDrawSurfaces.data() ] ) )
				continue; // with links present, misc_model soups and linked faces would only produce false matches
			if ( s.surfaceType == MST_PLANAR || s.surfaceType == MST_TRIANGLE_SOUP ) {
				auto& vec = m_modelTriangles[bspShaders[s.shaderNum].shader];
				for ( int t = 0; t + 3 <= s.numIndexes; t += 3 )
				{
					vec.push_back( BspTriangleRef( s.surfaceType,
						bspDrawVerts[s.firstVert + bspDrawIndexes[s.firstIndex + t + 0]],
						bspDrawVerts[s.firstVert + bspDrawIndexes[s.firstIndex + t + 1]],
						bspDrawVerts[s.firstVert + bspDrawIndexes[s.firstIndex + t + 2]]
					) );
				}
			}
		}

		for( auto& [ k, v ] : m_modelTriangles )
			std::sort( v.begin(), v.end() );
	}
	TriRef GetBestSurfaceTriangleMatchForBrushside( const side_t& buildSide ) const {
		const float nepsilon = normalEpsilon * 100; // default target 0.005 - gives worthy results practically
		const float depsilon = 2;
		winding_t polygon;
		float bestarea = 0;
		float thisarea;
		const plane_t& buildPlane = mapplanes[buildSide.planenum];
		int matches = 0;

		// first, start out with NULLs
		TriRef bestVert{ nullptr };

		// brute force through all bmodel surfaces
		if( const auto triangles = m_modelTriangles.find( buildSide.shaderInfo->shader.c_str() ); triangles != m_modelTriangles.end() ){
			MinMax minmax;
			for( const Vector3& v : buildSide.winding )
				minmax.extend( Vector3( spatial_distance( v ), v.y(), v.z() ) );
			minmax.mins -= Vector3( 32, depsilon, depsilon ); // 32 helps to spot more triangles, when brush is noticeably smaller
			minmax.maxs += Vector3( 32, depsilon, depsilon ); // e.g. produced by original model autoclip

			auto tri = std::lower_bound( triangles->second.begin(), triangles->second.end(), minmax.mins.x(),
			[]( const BspTriangleRef& tri, const float spatialMin ){
				return tri.minmax.maxs.x() < spatialMin;
			} );

			for( const auto end = triangles->second.end(); tri != end && minmax.maxs.x() > tri->minmax.maxs.x(); ++tri )
			{
				if ( !minmax.test( tri->minmax ) ) {
					continue;
				}
				const TriRef& vert = tri->tri;
				if ( tri->surfaceType == MST_PLANAR
				&& VectorCompare( vert[0]->normal, vert[1]->normal )
				&& VectorCompare( vert[1]->normal, vert[2]->normal ) ) {
					if ( !vector3_equal_epsilon( vert[0]->normal, buildPlane.normal(), float( nepsilon ) )
					  || !vector3_equal_epsilon( vert[1]->normal, buildPlane.normal(), float( nepsilon ) )
					  || !vector3_equal_epsilon( vert[2]->normal, buildPlane.normal(), float( nepsilon ) ) ) {
						continue;
					}
				}
				else
				{
					// this is more prone to roundoff errors, but with embedded
					// models, there is no better way
					Plane3f plane;
					PlaneFromPoints( plane, vert[0]->xyz, vert[1]->xyz, vert[2]->xyz );
					if ( !vector3_equal_epsilon( plane.normal(), buildPlane.normal(), float( nepsilon ) ) ) {
						continue;
					}
				}

				if ( std::fabs( plane3_distance_to_point( buildPlane.plane, vert[0]->xyz ) ) > depsilon
				  || std::fabs( plane3_distance_to_point( buildPlane.plane, vert[1]->xyz ) ) > depsilon
				  || std::fabs( plane3_distance_to_point( buildPlane.plane, vert[2]->xyz ) ) > depsilon ) {
					continue;
				}
				// Okay. Correct surface type, correct shader, correct plane. Let's start with the business...
				// we now need to generate the plane spanned by normal and (v2 - v1).
				Plane3f planes[3]{
					{ VectorNormalized( vector3_cross( vert[ 0 ]->xyz - vert[ 2 ]->xyz, buildPlane.normal() ) ), 0 },
					{ VectorNormalized( vector3_cross( vert[ 1 ]->xyz - vert[ 0 ]->xyz, buildPlane.normal() ) ), 0 },
					{ VectorNormalized( vector3_cross( vert[ 2 ]->xyz - vert[ 1 ]->xyz, buildPlane.normal() ) ), 0 },
				};
				planes[ 0 ].dist() = vector3_dot( vert[ 2 ]->xyz, planes[ 0 ].normal() );
				planes[ 1 ].dist() = vector3_dot( vert[ 0 ]->xyz, planes[ 1 ].normal() );
				planes[ 2 ].dist() = vector3_dot( vert[ 1 ]->xyz, planes[ 2 ].normal() );

				polygon = buildSide.winding;
				for ( const Plane3f& plane : planes )
				{
					ChopWindingInPlace( polygon, plane, distanceEpsilon );
					if ( polygon.empty() ) {
						goto exwinding;
					}
				}
				thisarea = WindingArea( polygon );
				if ( thisarea > 0 ) {
					++matches;
				}
				if ( thisarea > bestarea ) {
					bestarea = thisarea;
					bestVert = vert;
				}
		exwinding:
				;
			}
		}
		//if( !striEqualPrefix( buildSide.shaderInfo->shader, "textures/common/" ) )
		//	fprintf( stderr, "brushside with %s: %d matches (%f area)\n", buildSide.shaderInfo->shader, matches, bestarea );
		return bestVert;
	}
};

#define FRAC( x ) ( ( x ) - floor( x ) )
static void ConvertOriginBrush( FILE *f, int num, const Vector3& origin, EBrushType brushType ){
	const int ext = 8; // extent, box size is 2x
	const int size = ext * 2;
	const int texSize = 64; // can find out from shader
	const float texScale = float( size ) / texSize;
	static const char * const shader = strEqual( g_game->arg, "sof2" )? "tools/_origin"
	                                 : strEqual( g_game->arg, "ja" )
	                                || strEqual( g_game->arg, "jk2" )? "system/origin" : "common/origin";
	// 6: +Z +Y +X -Z -Y -X
	char pattern[6][7][4] = {
		{ "+++", "+-+", "-++", "-  ", " + ", " - ", "-  " },
		{ "+++", "-++", "++-", "-  ", "  +", "+  ", "  +" },
		{ "+++", "++-", "+-+", " - ", "  +", " - ", "  +" },
		{ "---", "+--", "-+-", "-  ", " + ", " - ", "+  " },
		{ "---", "--+", "+--", "-  ", "  +", "-  ", "  +" },
		{ "---", "-+-", "--+", " - ", "  +", " + ", "  +" }
	};
#define S( a, b, c ) ( pattern[a][b][c] == '+' ? +1 : pattern[a][b][c] == '-' ? -1 : 0 )

	/* start brush */
	fprintf( f, "\t// brush %d\n", num );
	fprintf( f, "\t{\n" );
	if ( brushType == EBrushType::Bp ) {
		fprintf( f, "\tbrushDef\n" );
		fprintf( f, "\t{\n" );
	}
	/* print brush side */
	/* ( 640 24 -224 ) ( 448 24 -224 ) ( 448 -232 -224 ) common/caulk         0            48      90 0.5 0.5 0 0 0 */
	/* ( 640 24 -224 ) ( 448 24 -224 ) ( 448 -232 -224 ) common/caulk [ 1 0 0 0 ] [ 0 -1 0 48 ]    90 0.5 0.5 0 0 0 */
	/* ( 640 24 -224 ) ( 448 24 -224 ) ( 448 -232 -224 ) ( ( 0 0.03125 0 ) ( -0.03125 0 0.75 ) ) common/caulk 0 0 0 */

	for ( int i = 0; i < 6; ++i )
	{
		fprintf( f, "\t\t( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ",
		         origin[0] + ext * S( i, 0, 0 ), origin[1] + ext * S( i, 0, 1 ), origin[2] + ext * S( i, 0, 2 ),
		         origin[0] + ext * S( i, 1, 0 ), origin[1] + ext * S( i, 1, 1 ), origin[2] + ext * S( i, 1, 2 ),
		         origin[0] + ext * S( i, 2, 0 ), origin[1] + ext * S( i, 2, 1 ), origin[2] + ext * S( i, 2, 2 )
		       );
		if ( brushType == EBrushType::Quake ){
			fprintf( f, "%s %.8f %.8f 0 %.8f %.8f 0 0 0\n",
			         shader,
			         FRAC( ( S( i, 3, 0 ) * origin[0] + S( i, 3, 1 ) * origin[1] + S( i, 3, 2 ) * origin[2] ) / size + 0.5 ) * texSize,
			         FRAC( ( S( i, 4, 0 ) * origin[0] + S( i, 4, 1 ) * origin[1] + S( i, 4, 2 ) * origin[2] ) / size + 0.5 ) * texSize,
			         texScale, texScale
			       );
		}
		else if ( brushType == EBrushType::Valve220 ){
			const Vector3 texX( S( i, 3, 0 ), S( i, 3, 1 ), S( i, 3, 2 ) );
			const Vector3 texY( S( i, 4, 0 ), S( i, 4, 1 ), S( i, 4, 2 ) );
			fprintf( f, "%s [ %.8f %.8f %.8f %.8f ] [ %.8f %.8f %.8f %.8f ] 0 %.8f %.8f 0 0 0\n",
			         shader,
			         texX.x(), texX.y(), texX.z(),
			         FRAC( ( S( i, 3, 0 ) * origin[0] + S( i, 3, 1 ) * origin[1] + S( i, 3, 2 ) * origin[2] ) / size + 0.5 ) * texSize,
			         texY.x(), texY.y(), texY.z(),
			         FRAC( ( S( i, 4, 0 ) * origin[0] + S( i, 4, 1 ) * origin[1] + S( i, 4, 2 ) * origin[2] ) / size + 0.5 ) * texSize,
			         texScale, texScale
			       );
		}
		else if ( brushType == EBrushType::Bp ) {
			fprintf( f, "( ( %.8f %.8f %.8f ) ( %.8f %.8f %.8f ) ) %s 0 0 0\n",
			         1.0f / size, 0.0f, FRAC( ( S( i, 5, 0 ) * origin[0] + S( i, 5, 1 ) * origin[1] + S( i, 5, 2 ) * origin[2] ) / size + 0.5 ),
			         0.0f, 1.0f / size, FRAC( ( S( i, 6, 0 ) * origin[0] + S( i, 6, 1 ) * origin[1] + S( i, 6, 2 ) * origin[2] ) / size + 0.5 ),
			         shader
			       );
		}
	}
#undef S

	/* end brush */
	if ( brushType == EBrushType::Bp ) {
		fprintf( f, "\t}\n" );
	}
	fprintf( f, "\t}\n\n" );
}

static void bspBrush_to_buildBrush( const bspBrush_t& brush ){
	/* clear out build brush */
	buildBrush.sides.clear();

	bool modelclip = false;
	/* try to guess if this is model clip */
	if ( g_decompile_modelClip ){
		int notNoShader = 0;
		modelclip = true;
		for ( const bspBrushSide_t& side : Span( &bspBrushSides[ brush.firstSide ], brush.numSides ) )
		{
			/* get shader */
			if ( side.shaderNum < 0 || side.shaderNum >= int( bspShaders.size() ) ) {
				continue;
			}
			const bspShader_t& shader = bspShaders[ side.shaderNum ];
			//"noshader" happens on modelclip and unwanted sides ( usually breaking complex brushes )
			if( !striEqual( shader.shader, "noshader" ) ){
				notNoShader++;
			}
			if( notNoShader > 1 ){
				modelclip = false;
				break;
			}
		}
	}

	/* iterate through bsp brush sides */
	for ( const bspBrushSide_t& side : Span( &bspBrushSides[ brush.firstSide ], brush.numSides ) )
	{
		/* get shader */
		if ( side.shaderNum < 0 || side.shaderNum >= int( bspShaders.size() ) ) {
			continue;
		}
		const bspShader_t& shader = bspShaders[ side.shaderNum ];
		//"noshader" happens on modelclip and unwanted sides ( usually breaking complex brushes )
		if( striEqual( shader.shader, "default" ) || ( striEqual( shader.shader, "noshader" ) && !modelclip ) )
			continue;

		/* add build side */
		buildBrush.sides.emplace_back();

		/* tag it */
		buildBrush.sides.back().shaderInfo = &ShaderInfoForShader( shader.shader );
		buildBrush.sides.back().planenum = side.planeNum;
		buildBrush.sides.back().outputNum = int( &side - bspBrushSides.data() );
	}
}

static void ConvertBrushFast( FILE *f, int bspBrushNum, const Vector3& origin, EBrushType brushType ){

	bspBrush_to_buildBrush( bspBrushes[bspBrushNum] );

	if ( !CreateBrushWindings( buildBrush ) ) {
		//Sys_Printf( "CreateBrushWindings failed\n" );
		return;
	}

	/* start brush */
	fprintf( f, "\t// brush %d\n", bspBrushNum );
	fprintf( f, "\t{\n" );
	if ( brushType == EBrushType::Bp ) {
		fprintf( f, "\tbrushDef\n" );
		fprintf( f, "\t{\n" );
	}

	/* iterate through build brush sides */
	for ( side_t& buildSide : buildBrush.sides )
	{
		/* get plane */
		const plane_t& buildPlane = mapplanes[ buildSide.planenum ];

		/* dummy check */
		if ( buildSide.shaderInfo == nullptr || buildSide.winding.empty() ) {
			continue;
		}

		/* get texture name */
		const char *texture = striEqualPrefix( buildSide.shaderInfo->shader, "textures/" )
		                      ? buildSide.shaderInfo->shader + 9
		                      : buildSide.shaderInfo->shader;

		Vector3 pts[ 3 ];
		{
			Vector3 vecs[ 2 ];
			MakeNormalVectors( buildPlane.normal(), vecs[ 0 ], vecs[ 1 ] );
			pts[ 0 ] = buildPlane.normal() * buildPlane.dist() + origin;
			pts[ 1 ] = pts[ 0 ] + vecs[ 0 ] * 256.0f;
			pts[ 2 ] = pts[ 0 ] + vecs[ 1 ] * 256.0f;
		}

		{
			fprintf( f, "\t\t( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ",
			         pts[ 0 ][ 0 ], pts[ 0 ][ 1 ], pts[ 0 ][ 2 ],
			         pts[ 1 ][ 0 ], pts[ 1 ][ 1 ], pts[ 1 ][ 2 ],
			         pts[ 2 ][ 0 ], pts[ 2 ][ 1 ], pts[ 2 ][ 2 ]
			       );
			if ( brushType == EBrushType::Quake ) {
				fprintf( f, "%s %.8f %.8f %.8f %.8f %.8f 0 0 0\n",
				         texture,
				         0.0f, 0.0f, 0.0f, 0.5f, 0.5f
				       );
			}
			else if ( brushType == EBrushType::Valve220 ) {
				Vector3 texX, texY;
				ComputeAxisBase( buildPlane.normal(), texX, texY );
				fprintf( f, "%s [ %.8f %.8f %.8f %.8f ] [ %.8f %.8f %.8f %.8f ] 0 0.5 0.5 0 0 0\n",
				         texture,
				         texX.x(), texX.y(), texX.z(), 0.f,
				         texY.x(), texY.y(), texY.z(), 0.f
				       );
			}
			else if ( brushType == EBrushType::Bp ) {
				fprintf( f, "( ( %.8f %.8f %.8f ) ( %.8f %.8f %.8f ) ) %s 0 0 0\n",
				         1.0f / 32.0f, 0.0f, 0.0f,
				         0.0f, 1.0f / 32.0f, 0.0f,
				         texture
				       );
			}
		}
	}

	/* end brush */
	if ( brushType == EBrushType::Bp ) {
		fprintf( f, "\t}\n" );
	}
	fprintf( f, "\t}\n\n" );
}

/* detail brushes are referenced from non-opaque leafs; precomputed once (was O(brushes*leafs)) */
static std::vector<char> g_brushInVisLeaf;
static int BrushDetailFlag( int bspBrushNum ){
	if ( g_brushInVisLeaf.size() != bspBrushes.size() ) {
		g_brushInVisLeaf.assign( bspBrushes.size(), 0 );
		for ( const auto& leaf : bspLeafs )
			if ( leaf.cluster > CLUSTER_OPAQUE )
				for ( const int id : Span( &bspLeafBrushes[ leaf.firstBSPLeafBrush ], leaf.numBSPLeafBrushes ) )
					if ( id >= 0 && id < int( g_brushInVisLeaf.size() ) ) g_brushInVisLeaf[ id ] = 1;
	}
	if ( bspShaders[ bspBrushes[ bspBrushNum ].shaderNum ].contentFlags & GetRequiredSurfaceParm<"structural">().contentFlags )
		return 0; // structural transparent brushes, e.g. hints
	/* a side whose shader is structural (hint, ...) makes the brush structural when compiled - writing it as detail
	   only gives "Mixed detail and structural" warnings */
	{
		const bspBrush_t& brush = bspBrushes[ bspBrushNum ];
		for ( const bspBrushSide_t& side : Span( &bspBrushSides[ brush.firstSide ], brush.numSides ) )
			if ( side.shaderNum >= 0 && side.shaderNum < int( bspShaders.size() )
			  && ( ShaderInfoForShader( bspShaders[ side.shaderNum ].shader ).compileFlags & C_STRUCTURAL ) )
				return 0;
	}
	return g_brushInVisLeaf[ bspBrushNum ] ? C_DETAIL : 0;
}

/* ===== exact standard-format decompile (SOF2 / RBSP focused) ===== */
static bool PointInsideBuildBrush( const Vector3& p, double eps ){
	for ( const side_t& s : buildBrush.sides )
		if ( plane3_distance_to_point( mapplanes[ s.planenum ].plane, p ) > eps )
			return false;
	return true;
}

static void CollectSideVerts( const side_t& side, const Vector3& origin, const ModelTriangles& mt,
                              std::vector<DoubleVector3>& xyz, std::vector<BasicVector2<double>>& st, bool& linked ){
	xyz.clear(); st.clear(); linked = false;
	const double W = side.shaderInfo->shaderWidth, H = side.shaderInfo->shaderHeight;
	const plane_t& pl = mapplanes[ side.planenum ];
	if ( side.outputNum >= 0 && side.outputNum < int( bspBrushSides.size() ) ) {
		const bspBrushSide_t& bs = bspBrushSides[ side.outputNum ];
		if ( bs.surfaceNum >= 0 && bs.surfaceNum < int( bspDrawSurfaces.size() ) ) {
			const bspDrawSurface_t& ds = bspDrawSurfaces[ bs.surfaceNum ];
			/* triangle soups too: -mergesurfaces turns nomarks faces into soups; verts are filtered by plane below */
			if ( ds.surfaceType == MST_PLANAR || ds.surfaceType == MST_TRIANGLE_SOUP ) {
				/* take whole triangles lying on this side: all corners on the plane, centroid inside the brush.
				   (a surface can hold faces of neighbouring brushes - meta merge, -mergesurfaces - whose
				   edge vertices touch this brush; testing single vertices would mix in their texture mapping) */
				std::vector<char> used( ds.numVerts, 0 );
				for ( int t = 0; t + 2 < ds.numIndexes; t += 3 ) {
					const bspDrawVert_t *tv[3];
					bool onPlane = true;
					for ( int k = 0; k < 3; ++k ) {
						const int vi = bspDrawIndexes[ ds.firstIndex + t + k ];
						if ( vi < 0 || vi >= ds.numVerts ) { onPlane = false; break; }
						tv[k] = &bspDrawVerts[ ds.firstVert + vi ];
						if ( std::fabs( plane3_distance_to_point( pl.plane, tv[k]->xyz ) ) > 0.1 ) onPlane = false;
					}
					if ( !onPlane ) continue;
					const Vector3 centroid = ( tv[0]->xyz + tv[1]->xyz + tv[2]->xyz ) / 3.f;
					if ( !PointInsideBuildBrush( centroid, 0.1 ) ) continue; // a neighbour's triangle has its centroid outside this brush
					for ( int k = 0; k < 3; ++k ) {
						const int vi = bspDrawIndexes[ ds.firstIndex + t + k ];
						if ( used[ vi ] ) continue;
						used[ vi ] = 1;
						xyz.push_back( DoubleVector3( tv[k]->xyz + origin ) );
						st.push_back( { tv[k]->st[0] * W, tv[k]->st[1] * H } );
					}
				}
				if ( xyz.size() >= 3 ) { linked = true; return; }
				xyz.clear(); st.clear();
			}
		}
	}
	const TriRef vert = mt.GetBestSurfaceTriangleMatchForBrushside( side );
	if ( vert[0] != nullptr && vert[1] != nullptr && vert[2] != nullptr )
		for ( const bspDrawVert_t *v : vert ) {
			xyz.push_back( DoubleVector3( v->xyz + origin ) );
			st.push_back( { v->st[0] * W, v->st[1] * H } );
		}
}

/* 3 plane points from the winding: biggest triangle, snapped to the integer grid when that keeps the plane */
static void ExactPlanePoints( const side_t& side, const Vector3& origin, Vector3 out[3] ){
	const plane_t& pl = mapplanes[ side.planenum ];
	const winding_t& w = side.winding;
	const size_t n = w.size();
	size_t bi = 0, bj = 1, bk = 2; double best = -1;
	for ( size_t i = 0; i < n; ++i ) for ( size_t j = i + 1; j < n; ++j ) for ( size_t k = j + 1; k < n; ++k ) {
		const double a = vector3_length( vector3_cross( DoubleVector3( w[j] - w[i] ), DoubleVector3( w[k] - w[i] ) ) );
		if ( a > best ) { best = a; bi = i; bj = j; bk = k; }
	}
	Vector3 raw[3] = { w[bi], w[bj], w[bk] };
	Vector3 snapped[3];
	bool canSnap = true;
	for ( int i = 0; i < 3; ++i ) for ( int c = 0; c < 3; ++c ) {
		const double r = std::round( raw[i][c] );
		if ( std::fabs( r - raw[i][c] ) > 0.01 ) canSnap = false;
		snapped[i][c] = r;
	}
	Vector3 *use = raw;
	if ( canSnap ) {
		Plane3f tp;
		if ( PlaneFromPoints( tp, snapped ) && PlaneEqual( pl, tp ) ) use = snapped;
	}
	// keep orientation consistent with the side plane
	Plane3f tp;
	if ( PlaneFromPoints( tp, use ) && vector3_dot( tp.normal(), pl.normal() ) < 0 ) std::swap( use[1], use[2] );
	for ( int i = 0; i < 3; ++i ) out[i] = use[i] + origin;
}


/* KenRadiant: index-map terrain comes back as a terrain. The bsp keeps the layer shaders (<base>_N, <base>_AtoB) on the
   brush sides and the layer of every vertex in its alpha; the decompile groups those brushes into a func_group with
   alphamap / layers / shader / min / max, gives their layer sides textures/tools/_terrain again and writes the .pcx so
   that q3map2 reads the same layer at every vertex. */
namespace terraindec
{
struct Family {
	std::string base;                 // without textures/
	int layers = 0;
	bool blend = false;
	std::vector<int> brushes;
	MinMax bounds;
	std::string alphamap;             // maps/<file>.pcx
};
std::vector<Family> g_families;
std::map<std::string, std::pair<int, std::pair<int, int>>> g_shaderLayer; // shader name -> ( family, ( a, b ) )
std::vector<int> g_brushFamily;       // bsp brush -> family or -1
bool g_enabled = false;

bool parseLayerShader( const char *name, std::string& base, int& a, int& b ){
	if ( !striEqualPrefix( name, "textures/" ) )
		return false;
	const std::string n( name + 9 );
	const std::size_t us = n.rfind( '_' );
	if ( us == std::string::npos || us == 0 || us + 1 >= n.size() )
		return false;
	const std::string tail = n.substr( us + 1 );
	const std::size_t to = tail.find( "to" );
	const auto digits = []( const std::string& t ){ return !t.empty() && t.size() <= 3 && std::all_of( t.begin(), t.end(), []( char c ){ return c >= '0' && c <= '9'; } ); };
	if ( to == std::string::npos ) {
		if ( !digits( tail ) ) return false;
		a = atoi( tail.c_str() ); b = -1;
	}
	else {
		if ( !digits( tail.substr( 0, to ) ) || !digits( tail.substr( to + 2 ) ) ) return false;
		a = atoi( tail.substr( 0, to ).c_str() ); b = atoi( tail.substr( to + 2 ).c_str() );
		if ( b <= a ) return false;
	}
	base = n.substr( 0, us );
	return true;
}

void setup( const char *bspName ){
	g_families.clear(); g_shaderLayer.clear(); g_brushFamily.assign( bspBrushes.size(), -1 );
	if ( bspModels.empty() )
		return;
	std::map<std::string, int> byBase;
	for ( const bspShader_t& sh : bspShaders ) {
		std::string base; int a, b;
		if ( !parseLayerShader( sh.shader, base, a, b ) )
			continue;
		auto [it, added] = byBase.emplace( base, int( g_families.size() ) );
		if ( added ) { g_families.emplace_back(); g_families.back().base = base; }
		Family& f = g_families[ it->second ];
		f.layers = std::max( f.layers, std::max( a, b ) + 1 );
		f.blend = f.blend || b >= 0;
		g_shaderLayer[ sh.shader ] = { it->second, { a, b } };
	}
	const bspModel_t& world = bspModels[ 0 ];
	for ( int i = 0; i < world.numBSPBrushes; ++i ) {
		const int bn = world.firstBSPBrush + i;
		const bspBrush_t& brush = bspBrushes[ bn ];
		for ( const bspBrushSide_t& side : Span( &bspBrushSides[ brush.firstSide ], brush.numSides ) ) {
			if ( side.shaderNum < 0 || side.shaderNum >= int( bspShaders.size() ) )
				continue;
			const auto it = g_shaderLayer.find( bspShaders[ side.shaderNum ].shader );
			if ( it != g_shaderLayer.end() && g_families[ it->second.first ].blend && g_families[ it->second.first ].layers >= 2 ) {
				g_brushFamily[ bn ] = it->second.first;
				break;
			}
		}
		if ( g_brushFamily[ bn ] >= 0 )
			g_families[ g_brushFamily[ bn ] ].brushes.push_back( bn );
	}
	// bounds (what q3map2 takes as the entity bounds), vertex layers -> index map
	int terrainNum = 0;
	for ( std::size_t fi = 0; fi < g_families.size(); ++fi ) {
		Family& f = g_families[ fi ];
		if ( f.brushes.empty() )
			continue;
		for ( int bn : f.brushes ) {
			bspBrush_to_buildBrush( bspBrushes[ bn ] );
			if ( CreateBrushWindings( buildBrush ) )
				f.bounds.extend( buildBrush.minmax );
		}
		if ( !f.bounds.valid() )
			continue;
		struct V { float x, y; int layer; };
		std::vector<V> verts;
		for ( int si = 0; si < world.numBSPSurfaces; ++si ) {
			const bspDrawSurface_t& ds = bspDrawSurfaces[ world.firstBSPSurface + si ];
			if ( ds.shaderNum < 0 || ds.shaderNum >= int( bspShaders.size() ) )
				continue;
			const auto it = g_shaderLayer.find( bspShaders[ ds.shaderNum ].shader );
			if ( it == g_shaderLayer.end() || it->second.first != int( fi ) )
				continue;
			const auto [a, b] = it->second.second;
			for ( int v = ds.firstVert; v < ds.firstVert + ds.numVerts && v < int( bspDrawVerts.size() ); ++v ) {
				const bspDrawVert_t& dv = bspDrawVerts[ v ];
				verts.push_back( { dv.xyz[0], dv.xyz[1], b < 0 ? a : ( dv.color[0].alpha() >= 128 ? b : a ) } );
			}
		}
		if ( verts.empty() )
			continue;
		const Vector3 size = f.bounds.maxs - f.bounds.mins;
		if ( size[0] <= 0 || size[1] <= 0 )
			continue;
		// the smallest index map where no two vertices with different layers share a pixel
		// (and fine enough to paint on: a pixel no bigger than 32 units)
		int res = 64;
		while ( res < 1024 && std::max( size[0], size[1] ) / res > 32 )
			res *= 2;
		std::vector<int> px;
		for ( ; ; res *= 2 ) {
			px.assign( std::size_t( res ) * res, -1 );
			bool clash = false;
			for ( const V& v : verts ) {
				const int x = std::clamp( int( ( v.x - f.bounds.mins[0] ) / size[0] * res ), 0, res - 1 );
				const int y = std::clamp( int( ( f.bounds.maxs[1] - v.y ) / size[1] * res ), 0, res - 1 );
				int& p = px[ std::size_t( y ) * res + x ];
				if ( p >= 0 && p != v.layer ) clash = true;
				p = v.layer;
			}
			if ( !clash || res >= 1024 )
				break;
		}
		// the rest of the map: the nearest painted pixel (nice to paint on later)
		std::vector<int> queue;
		for ( std::size_t i = 0; i < px.size(); ++i )
			if ( px[i] >= 0 ) queue.push_back( int( i ) );
		for ( std::size_t q = 0; q < queue.size(); ++q ) {
			const int i = queue[q], x = i % res, y = i / res;
			const int nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
			for ( const auto& n : nb )
				if ( n[0] >= 0 && n[0] < res && n[1] >= 0 && n[1] < res && px[ std::size_t( n[1] ) * res + n[0] ] < 0 ) {
					px[ std::size_t( n[1] ) * res + n[0] ] = px[i];
					queue.push_back( n[1] * res + n[0] );
				}
		}
		std::vector<byte> pixels( px.size() );
		for ( std::size_t i = 0; i < px.size(); ++i )
			pixels[i] = byte( std::max( px[i], 0 ) );
		byte palette[768];
		for ( int i = 0; i < 256; ++i )
			palette[ i * 3 ] = palette[ i * 3 + 1 ] = palette[ i * 3 + 2 ] = byte( std::min( 255, i * 64 ) );
		++terrainNum;
		const auto file = StringStream( PathExtensionless( bspName ), "_terrain", terrainNum, ".pcx" );
		WritePCXfile( file, pixels.data(), res, res, palette );
		{
			const std::string path( file.c_str() );
			const std::size_t slash = path.find_last_of( "/\\" );
			f.alphamap = "maps/" + ( slash == std::string::npos ? path : path.substr( slash + 1 ) );
		}
		Sys_Printf( "terrain: %s (%d layers, %zu brushes) -> func_group with index map %s (%d x %d)\n",
		            f.base.c_str(), f.layers, f.brushes.size(), f.alphamap.c_str(), res, res );
	}
	for ( std::size_t i = 0; i < g_brushFamily.size(); ++i )
		if ( g_brushFamily[i] >= 0 && g_families[ g_brushFamily[i] ].alphamap.empty() )
			g_brushFamily[i] = -1; // no index map made: leave the brush as it was
	g_enabled = true;
}
}

static std::set<CopiedString> g_missingImages;
static void ConvertBrushExact( FILE *f, int bspBrushNum, const Vector3& origin, const ModelTriangles& mt, int contentFlag, bool terrain = false ){
	using namespace exactdec;
	bspBrush_to_buildBrush( bspBrushes[ bspBrushNum ] );
	if ( !CreateBrushWindings( buildBrush ) )
		return;

	struct Out { side_t *side; TexDef td; };
	std::vector<Out> outs;
	std::vector<DoubleVector3> xyz; std::vector<BasicVector2<double>> st;
	for ( side_t& side : buildBrush.sides ) {
		if ( side.shaderInfo == nullptr || side.winding.size() < 3 || WindingArea( side.winding ) < 0.1f ) {
			if ( side.shaderInfo != nullptr ) ++g_stats.bevelsDropped;
			continue;
		}
		Out o{ &side, {} };
		if ( !( side.shaderInfo->compileFlags & C_NODRAW ) && side.shaderInfo->shaderImage && strEqual( side.shaderInfo->shaderImage->name.c_str(), DEFAULT_IMAGE ) ) {
			++g_stats.missingImage;
			if ( g_missingImages.insert( side.shaderInfo->shader.c_str() ).second )
				Sys_Printf( "exact decompile: no image for %s (texdef scale uses 64x64)\n", side.shaderInfo->shader.c_str() );
		}
		bool linked;
		CollectSideVerts( side, origin, mt, xyz, st, linked );
		if ( xyz.size() >= 3 && Solve( mapplanes[ side.planenum ].normal(), xyz, st,
		                               side.shaderInfo->shaderWidth, side.shaderInfo->shaderHeight, o.td ) ) {
			++( linked ? g_stats.linked : g_stats.searched );
			++( o.td.err <= g_tolTexel ? g_stats.exact : g_stats.inexact );
			if ( o.td.err > g_tolTexel )
				Sys_FPrintf( SYS_VRB, "exact decompile: brush %d side %d %s: best texdef off by %.3f texel (%zu verts, %s)\n",
				             bspBrushNum, int( &side - buildBrush.sides.data() ), side.shaderInfo->shader.c_str(), o.td.err, xyz.size(), linked ? "linked" : "searched" );
			g_stats.worstErr = std::max( g_stats.worstErr, o.td.err );
		}
		outs.push_back( o );
	}

	/* hidden sides: inherit from visible sides of this brush (same shader first) */
	for ( Out& o : outs ) {
		if ( o.td.solved ) continue;
		const Out *src = nullptr;
		std::map<std::tuple<double, double, double, double, double>, int> hist;
		for ( int pass = 0; pass < 2 && !src; ++pass ) {
			hist.clear();
			for ( const Out& v : outs )
				if ( v.td.solved && ( pass || v.side->shaderInfo == o.side->shaderInfo ) )
					++hist[ { v.td.rotate, v.td.scale[0], v.td.scale[1], v.td.shift[0], v.td.shift[1] } ];
			int bestc = 0;
			for ( const auto& [k, c] : hist ) if ( c > bestc ) {
				bestc = c;
				for ( const Out& v : outs )
					if ( v.td.solved && std::tie( v.td.rotate, v.td.scale[0], v.td.scale[1], v.td.shift[0], v.td.shift[1] ) == std::tie( std::get<0>( k ), std::get<1>( k ), std::get<2>( k ), std::get<3>( k ), std::get<4>( k ) ) ) { src = &v; break; }
			}
		}
		if ( src ) {
			o.td = src->td; o.td.solved = false;
			// shifts are only meaningful modulo texture size; tool textures are almost always unshifted
			if ( striEqualPrefix( o.side->shaderInfo->shader, "textures/tools/" ) )
				o.td.shift[0] = o.td.shift[1] = 0;
			o.td.shift[0] = Wrap( o.td.shift[0], o.side->shaderInfo->shaderWidth );
			o.td.shift[1] = Wrap( o.td.shift[1], o.side->shaderInfo->shaderHeight );
			++g_stats.hiddenBrush;
		}
		else {
			o.td.rotate = g_defaultRotate; o.td.scale[0] = g_defaultScale[0]; o.td.scale[1] = g_defaultScale[1];
			o.td.shift[0] = o.td.shift[1] = 0;
			++g_stats.hiddenGlobal;
		}
	}

	fprintf( f, "\t// brush %d\n\t{\n", bspBrushNum );
	char b[9][32];
	for ( const Out& o : outs ) {
		Vector3 pts[3];
		ExactPlanePoints( *o.side, origin, pts );
		const char *texture = striEqualPrefix( o.side->shaderInfo->shader, "textures/" )
		                      ? o.side->shaderInfo->shader + 9 : o.side->shaderInfo->shader.c_str();
		TexDef td = o.td;
		if ( terrain && terraindec::g_shaderLayer.count( o.side->shaderInfo->shader.c_str() ) ) {
			// a layer side: the terrain tool shader again with the same texdef - q3map2 divides a terrain side's texture
			// coordinates by the layer shader's image size, the one this texdef was solved with
			texture = "tools/_terrain";
		}
		fprintf( f, "\t\t( %s %s %s ) ( %s %s %s ) ( %s %s %s ) ",
		         Num( pts[0][0], b[0] ), Num( pts[0][1], b[1] ), Num( pts[0][2], b[2] ),
		         Num( pts[1][0], b[3] ), Num( pts[1][1], b[4] ), Num( pts[1][2], b[5] ),
		         Num( pts[2][0], b[6] ), Num( pts[2][1], b[7] ), Num( pts[2][2], b[8] ) );
		fprintf( f, "%s %s %s %s %s %s %d 0 0\n", texture,
		         Num( td.shift[0], b[0] ), Num( td.shift[1], b[1] ), Num( td.rotate, b[2] ),
		         Num( td.scale[0], b[3] ), Num( td.scale[1], b[4] ), contentFlag );
	}
	fprintf( f, "\t}\n\n" );
}

/* map-wide most common rotate/scale over all linked planar sides -> default for fully hidden brushes */
static void ExactComputeDefaults(){
	using namespace exactdec;
	std::map<std::tuple<double, double, double>, int> hist;
	std::vector<DoubleVector3> xyz; std::vector<BasicVector2<double>> st;
	for ( const bspBrushSide_t& bs : bspBrushSides ) {
		if ( bs.surfaceNum < 0 || bs.surfaceNum >= int( bspDrawSurfaces.size() ) || bs.shaderNum < 0 ) continue;
		const bspDrawSurface_t& ds = bspDrawSurfaces[ bs.surfaceNum ];
		if ( ds.surfaceType != MST_PLANAR ) continue;
		const shaderInfo_t& si = ShaderInfoForShader( bspShaders[ bs.shaderNum ].shader );
		xyz.clear(); st.clear();
		for ( const bspDrawVert_t& v : Span( &bspDrawVerts[ ds.firstVert ], ds.numVerts ) ) {
			xyz.push_back( DoubleVector3( v.xyz ) );
			st.push_back( { v.st[0] * si.shaderWidth, v.st[1] * si.shaderHeight } );
		}
		TexDef td;
		if ( Solve( bspPlanes[ bs.planeNum ].normal(), xyz, st, si.shaderWidth, si.shaderHeight, td ) && td.err <= g_tolTexel )
			++hist[ { td.rotate, td.scale[0], td.scale[1] } ];
	}
	int best = 0;
	for ( const auto& [k, c] : hist ) if ( c > best ) {
		best = c; g_defaultRotate = std::get<0>( k ); g_defaultScale[0] = std::get<1>( k ); g_defaultScale[1] = std::get<2>( k );
	}
	Sys_Printf( "exact decompile: default texdef for hidden brushes = rotate %g scale %g %g (%d sides)\n",
	            g_defaultRotate, g_defaultScale[0], g_defaultScale[1], best );
}

static void ConvertBrush( FILE *f, int bspBrushNum, const Vector3& origin, EBrushType brushType, const ModelTriangles& modelTriangles ){

	bspBrush_to_buildBrush( bspBrushes[bspBrushNum] );

	/* make brush windings */
	if ( !CreateBrushWindings( buildBrush ) ) {
		//Sys_Printf( "CreateBrushWindings failed\n" );
		return;
	}

	/* start brush */
	fprintf( f, "\t// brush %d\n", bspBrushNum );
	fprintf( f, "\t{\n" );
	if ( brushType == EBrushType::Bp ) {
		fprintf( f, "\tbrushDef\n" );
		fprintf( f, "\t{\n" );
	}

	const int contentFlag = BrushDetailFlag( bspBrushNum );

	/* iterate through build brush sides */
	for ( side_t& buildSide : buildBrush.sides )
	{
		/* get plane */
		const plane_t& buildPlane = mapplanes[ buildSide.planenum ];

		/* dummy check */
		if ( buildSide.shaderInfo == nullptr || buildSide.winding.empty() ) {
			continue;
		}

		// st-texcoords -> texMat block
		// start out with dummy
		buildSide.texMat[0] = { 1 / 32.0, 0, 0 };
		buildSide.texMat[1] = { 0, 1 / 32.0, 0 };

		// find surface for this side (by brute force)
		// surface format:
		//   - meshverts point in pairs of three into verts
		//   - (triangles)
		//   - find the triangle that has most in common with our
		const TriRef vert = modelTriangles.GetBestSurfaceTriangleMatchForBrushside( buildSide );

		/* get texture name */
		const char *texture = striEqualPrefix( buildSide.shaderInfo->shader, "textures/" )
		                      ? buildSide.shaderInfo->shader + 9
		                      : buildSide.shaderInfo->shader;

		Vector3 pts[ 3 ];
		/* recheck and fix winding points, fails occur somehow */
		int match = 0;
		for ( const Vector3& p : buildSide.winding ){
			if ( std::fabs( plane3_distance_to_point( buildPlane.plane, p ) ) < distanceEpsilon ) {
				pts[ match ] = p;
				match++;
				/* got 3 fine points? */
				if( match > 2 )
					break;
			}
		}

		if( match > 2 ){
			//Sys_Printf( "pointsKK " );
			if ( Plane3f testplane; PlaneFromPoints( testplane, pts ) ){
				if( !PlaneEqual( buildPlane, testplane ) ){
					//Sys_Printf( "1: %f %f %f %f\n2: %f %f %f %f\n", buildPlane->normal[0], buildPlane->normal[1], buildPlane->normal[2], buildPlane->dist, testplane[0], testplane[1], testplane[2], testplane[3] );
					match--;
					//Sys_Printf( "planentEQ " );
				}
			}
			else{
				match--;
			}
		}


		if( match > 2 ){
			//Sys_Printf( "ok " );
			/* offset by origin */
			for ( Vector3& pt : pts )
				pt += origin;
		}
		else{
			Vector3 vecs[ 2 ];
			MakeNormalVectors( buildPlane.normal(), vecs[ 0 ], vecs[ 1 ] );
			pts[ 0 ] = buildPlane.normal() * buildPlane.dist() + origin;
			pts[ 1 ] = pts[ 0 ] + vecs[ 0 ] * 256.0f;
			pts[ 2 ] = pts[ 0 ] + vecs[ 1 ] * 256.0f;
			//Sys_Printf( "not\n" );
		}
		/* print planepoints */
		fprintf( f, "\t\t( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ",
		         pts[ 0 ][ 0 ], pts[ 0 ][ 1 ], pts[ 0 ][ 2 ],
		         pts[ 1 ][ 0 ], pts[ 1 ][ 1 ], pts[ 1 ][ 2 ],
		         pts[ 2 ][ 0 ], pts[ 2 ][ 1 ], pts[ 2 ][ 2 ]
		       );

		if ( vert[0] != nullptr && vert[1] != nullptr && vert[2] != nullptr ) {
			const Vector3 verts[3] = { vert[0]->xyz + origin,
			                           vert[1]->xyz + origin,
			                           vert[2]->xyz + origin };
			const Vector2 sts[3] = { vert[0]->st, vert[1]->st, vert[2]->st };

			if ( brushType == EBrushType::Bp || brushType == EBrushType::Valve220 ) {
				BasicVector2<double> xyI, xyJ, xyK;
				BasicVector2<double> stI, stJ, stK;
				double D, D0, D1, D2;
				DoubleVector3 texX, texY;
				ComputeAxisBase( buildPlane.normal(), texX, texY );

				xyI[0] = vector3_dot( verts[0], texX );
				xyI[1] = vector3_dot( verts[0], texY );
				xyJ[0] = vector3_dot( verts[1], texX );
				xyJ[1] = vector3_dot( verts[1], texY );
				xyK[0] = vector3_dot( verts[2], texX );
				xyK[1] = vector3_dot( verts[2], texY );
				stI = sts[0];
				stJ = sts[1];
				stK = sts[2];

				//   - solve linear equations:
				//     - (x, y) := xyz . (texX, texY)
				//     - st[i] = texMat[i][0]*x + texMat[i][1]*y + texMat[i][2]
				//       (for three vertices)
				D = Det3x3(
				        xyI[0], xyI[1], 1,
				        xyJ[0], xyJ[1], 1,
				        xyK[0], xyK[1], 1
				    );
				if ( D != 0 ) {
					for ( int i = 0; i < 2; ++i )
					{
						D0 = Det3x3(
						         stI[i], xyI[1], 1,
						         stJ[i], xyJ[1], 1,
						         stK[i], xyK[1], 1
						     );
						D1 = Det3x3(
						         xyI[0], stI[i], 1,
						         xyJ[0], stJ[i], 1,
						         xyK[0], stK[i], 1
						     );
						D2 = Det3x3(
						         xyI[0], xyI[1], stI[i],
						         xyJ[0], xyJ[1], stJ[i],
						         xyK[0], xyK[1], stK[i]
						     );
						buildSide.texMat[i] = Vector3( D0 / D, D1 / D, D2 / D );
					}
				}
				else{
					fprintf( stderr, "degenerate triangle found when solving texMat equations for\n(%f %f %f) (%f %f %f) (%f %f %f)\n( %f %f %f )\n( %f %f %f ) -> ( %f %f )\n( %f %f %f ) -> ( %f %f )\n( %f %f %f ) -> ( %f %f )\n",
					         buildPlane.normal()[0], buildPlane.normal()[1], buildPlane.normal()[2],
					         vert[0]->normal[0], vert[0]->normal[1], vert[0]->normal[2],
					         texX[0], texX[1], texX[2], texY[0], texY[1], texY[2],
					         vert[0]->xyz[0], vert[0]->xyz[1], vert[0]->xyz[2], xyI[0], xyI[1],
					         vert[1]->xyz[0], vert[1]->xyz[1], vert[1]->xyz[2], xyJ[0], xyJ[1],
					         vert[2]->xyz[0], vert[2]->xyz[1], vert[2]->xyz[2], xyK[0], xyK[1]
					       );
				}

				/* print brush side */
				if( brushType == EBrushType::Bp ){
					/* ( 640 24 -224 ) ( 448 24 -224 ) ( 448 -232 -224 ) ( ( 0 0.03125 0 ) ( -0.03125 0 0.75 ) ) common/caulk 0 0 0 */
					fprintf( f, "( ( %.8f %.8f %.8f ) ( %.8f %.8f %.8f ) ) %s %d 0 0\n",
					         buildSide.texMat[0][0], buildSide.texMat[0][1], FRAC( buildSide.texMat[0][2] ),
					         buildSide.texMat[1][0], buildSide.texMat[1][1], FRAC( buildSide.texMat[1][2] ),
					         texture,
					         contentFlag
					       );
				}
				else if( brushType == EBrushType::Valve220 ){
					// brush_primit.cpp Valve220_from_BP()
					const double scale[2]{ 1.0 / ( vector2_length( buildSide.texMat[0].vec2() ) * buildSide.shaderInfo->shaderWidth ),
					                       1.0 / ( vector2_length( buildSide.texMat[1].vec2() ) * buildSide.shaderInfo->shaderHeight ) };
					const double shift[2]{ FRAC( buildSide.texMat[0][2] ) * buildSide.shaderInfo->shaderWidth,
					                       FRAC( buildSide.texMat[1][2] ) * buildSide.shaderInfo->shaderHeight };

					const DoubleVector3 basis_s = vector3_normalised( texX * buildSide.texMat[0][0] + texY * buildSide.texMat[0][1] );
					const DoubleVector3 basis_t = vector3_normalised( texX * buildSide.texMat[1][0] + texY * buildSide.texMat[1][1] );

					/* ( 640 24 -224 ) ( 448 24 -224 ) ( 448 -232 -224 ) common/caulk [ 1 0 0 0 ] [ 0 -1 0 48 ] 90 0.5 0.5 0 0 0 */
					fprintf( f, "%s [ %.8f %.8f %.8f %.8f ] [ %.8f %.8f %.8f %.8f ] 0 %.8f %.8f %d 0 0\n",
					         texture,
					         basis_s.x(), basis_s.y(), basis_s.z(), shift[0],
					         basis_t.x(), basis_t.y(), basis_t.z(), shift[1],
					         scale[0], scale[1],
					         contentFlag
					       );
				}
			}
			else if ( brushType == EBrushType::Quake ) {
				// invert QuakeTextureVecs
				int sv, tv;
				BasicVector2<double> stI, stJ, stK;
				double D, D0, D1, D2;
				DoubleVector3 texMat[2];
				float shift[2], scale[2];
				float rotate;

				const auto vecs = TextureAxisFromPlane( buildPlane );
				sv = vecs[0][0]? 0
				   : vecs[0][1]? 1: 2;
				tv = vecs[1][0]? 0
				   : vecs[1][1]? 1: 2;

				stI[0] = sts[0][0] * buildSide.shaderInfo->shaderWidth;
				stI[1] = sts[0][1] * buildSide.shaderInfo->shaderHeight;
				stJ[0] = sts[1][0] * buildSide.shaderInfo->shaderWidth;
				stJ[1] = sts[1][1] * buildSide.shaderInfo->shaderHeight;
				stK[0] = sts[2][0] * buildSide.shaderInfo->shaderWidth;
				stK[1] = sts[2][1] * buildSide.shaderInfo->shaderHeight;

				D = Det3x3(
				        verts[0][sv], verts[0][tv], 1,
				        verts[1][sv], verts[1][tv], 1,
				        verts[2][sv], verts[2][tv], 1
				    );
				if ( D != 0 ) {
					for ( int i = 0; i < 2; ++i )
					{
						D0 = Det3x3(
						         stI[i], verts[0][tv], 1,
						         stJ[i], verts[1][tv], 1,
						         stK[i], verts[2][tv], 1
						     );
						D1 = Det3x3(
						         verts[0][sv], stI[i], 1,
						         verts[1][sv], stJ[i], 1,
						         verts[2][sv], stK[i], 1
						     );
						D2 = Det3x3(
						         verts[0][sv], verts[0][tv], stI[i],
						         verts[1][sv], verts[1][tv], stJ[i],
						         verts[2][sv], verts[2][tv], stK[i]
						     );
						texMat[i] = { D0 / D, D1 / D, D2 / D };
						//Sys_Printf( "%.3f %.3f %.3f \n", texMat[i][0], texMat[i][1], texMat[i][2] );
					}
				}
				else{
					fprintf( stderr, "degenerate triangle found when solving texDef equations\n" ); // FIXME add stuff here
					texMat[0] = { 2.0, 0.0, 0.0 };
					texMat[1] = { 0.0, -2.0, 0.0 };
				}
				// now we must solve:
				//	// now we must invert:
				//	ang = degrees_to_radians( rotate );
				//	sinv = sin( ang );
				//	cosv = cos( ang );
				//	ns = cosv * vecs[0][sv];
				//	nt = sinv * vecs[0][sv];
				//	vecsrotscaled[0][sv] = ns / scale[0];
				//	vecsrotscaled[0][tv] = nt / scale[0];
				//	ns = -sinv * vecs[1][tv];
				//	nt =  cosv * vecs[1][tv];
				//	vecsrotscaled[1][sv] = ns / scale[1];
				//	vecsrotscaled[1][tv] = nt / scale[1];
#if 0
				scale[0] = 1.0 / vector2_length( texMat[0].vec2() );
				scale[1] = 1.0 / vector2_length( texMat[1].vec2() );
				rotate = radians_to_degrees( atan2( texMat[0][1] * vecs[0][sv] - texMat[1][0] * vecs[1][tv], texMat[0][0] * vecs[0][sv] + texMat[1][1] * vecs[1][tv] ) );
				shift[0] = buildSide.shaderInfo->shaderWidth * FRAC( texMat[0][2] / buildSide.shaderInfo->shaderWidth );
				shift[1] = buildSide.shaderInfo->shaderHeight * FRAC( texMat[1][2] / buildSide.shaderInfo->shaderHeight );
#else			// Texdef_fromTransform() from brush_primit.cpp, flawless unlike upper
				scale[0] = 1.0 / vector2_length( texMat[0].vec2() );
				scale[1] = 1.0 / vector2_length( texMat[1].vec2() );

				rotate = -radians_to_degrees( atan2( -texMat[0][1], texMat[0][0] ) );

				if ( rotate == -180.0f ) {
					rotate = 180.0f;
				}

				shift[0] = buildSide.shaderInfo->shaderWidth * FRAC( texMat[0][2] / buildSide.shaderInfo->shaderWidth );
				shift[1] = buildSide.shaderInfo->shaderHeight * FRAC( texMat[1][2] / buildSide.shaderInfo->shaderHeight );

				// If the 2d cross-product of the x and y axes is positive, one of the axes has a negative scale.
				if ( vector2_cross( Vector2( texMat[0][0], texMat[0][1] ), Vector2( texMat[1][0], texMat[1][1] ) ) > 0 ) {
					if ( rotate >= 180.0f ) {
						rotate -= 180.0f;
						scale[0] = -scale[0];
					}
					else
					{
						scale[1] = -scale[1];
					}
				}
#endif
				/* print brush side */
				/* ( 640 24 -224 ) ( 448 24 -224 ) ( 448 -232 -224 ) common/caulk 0 48 0 0.500000 0.500000 0 0 0 */
				fprintf( f, "%s %.8f %.8f %.8f %.8f %.8f %d 0 0\n",
				         texture,
				         shift[0], shift[1], rotate, scale[0], scale[1],
				         contentFlag
				       );
			}
		}
		else
		{
			if ( g_decompile_wtf
			  && !striEqualPrefix( buildSide.shaderInfo->shader, "textures/common/" )
			  && !striEqualPrefix( buildSide.shaderInfo->shader, "textures/system/" )
			  &&        !strEqual( buildSide.shaderInfo->shader, "noshader" )
			  &&        !strEqual( buildSide.shaderInfo->shader, "default" ) ) {
				//fprintf( stderr, "no matching triangle for brushside using %s (hopefully nobody can see this side anyway)\n", buildSide.shaderInfo->shader );
				texture = "common/WTF";
			}

			if ( brushType == EBrushType::Quake ) {
				fprintf( f, "%s %.8f %.8f %.8f %.8f %.8f %d 0 0\n",
				         texture,
				         0.0f, 0.0f, 0.0f, 0.25f, 0.25f,
				         contentFlag
				       );
			}
			else if ( brushType == EBrushType::Valve220 ) {
				Vector3 texX, texY;
				ComputeAxisBase( buildPlane.normal(), texX, texY );
				fprintf( f, "%s [ %.8f %.8f %.8f %.8f ] [ %.8f %.8f %.8f %.8f ] 0 0.5 0.5 %d 0 0\n",
				         texture,
				         texX.x(), texX.y(), texX.z(), 0.f,
				         texY.x(), texY.y(), texY.z(), 0.f,
				         contentFlag
				       );
			}
			else if ( brushType == EBrushType::Bp ) {
				fprintf( f, "( ( %.8f %.8f %.8f ) ( %.8f %.8f %.8f ) ) %s %d 0 0\n",
				         1.0f / 16.0f, 0.0f, 0.0f,
				         0.0f, 1.0f / 16.0f, 0.0f,
				         texture,
				         contentFlag
				       );
			}
		}
	}

	/* end brush */
	if ( brushType == EBrushType::Bp ) {
		fprintf( f, "\t}\n" );
	}
	fprintf( f, "\t}\n\n" );
}
#undef FRAC

#if 0
/* iterate through the brush sides (ignore the first 6 bevel planes) */
for ( i = 0; i < brush->numSides; ++i )
{
	/* get side */
	side = &bspBrushSides[ brush->firstSide + i ];

	/* get shader */
	if ( side->shaderNum < 0 || side->shaderNum >= int( bspShaders.size() ) ) {
		continue;
	}
	shader = &bspShaders[ side->shaderNum ];
	if ( striEqual( shader->shader, "default" ) || striEqual( shader->shader, "noshader" ) ) {
		continue;
	}

	/* get texture name */
	if ( striEqualPrefix( shader->shader, "textures/" ) ) {
		texture = shader->shader + 9;
	}
	else{
		texture = shader->shader;
	}

	/* get plane */
	plane = &bspPlanes[ side->planeNum ];

	/* make plane points */
	{
		vec3_t vecs[ 2 ];


		MakeNormalVectors( plane->normal, vecs[ 0 ], vecs[ 1 ] );
		VectorMA( vec3_origin, plane->dist, plane->normal, pts[ 0 ] );
		VectorMA( pts[ 0 ], 256.0f, vecs[ 0 ], pts[ 1 ] );
		VectorMA( pts[ 0 ], 256.0f, vecs[ 1 ], pts[ 2 ] );
	}

	/* offset by origin */
	for ( j = 0; j < 3; ++j )
		VectorAdd( pts[ j ], origin, pts[ j ] );

	/* print brush side */
	/* ( 640 24 -224 ) ( 448 24 -224 ) ( 448 -232 -224 ) common/caulk 0 48 0 0.500000 0.500000 0 0 0 */
	fprintf( f, "\t\t( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 0 0 0\n",
	         pts[ 0 ][ 0 ], pts[ 0 ][ 1 ], pts[ 0 ][ 2 ],
	         pts[ 1 ][ 0 ], pts[ 1 ][ 1 ], pts[ 1 ][ 2 ],
	         pts[ 2 ][ 0 ], pts[ 2 ][ 1 ], pts[ 2 ][ 2 ],
	         texture );
}
#endif



/*
   ConvertPatch()
   converts a bsp patch to a map patch

    {
        patchDef2
        {
            base_wall/concrete
            ( 9 3 0 0 0 )
            (
                ( ( 168 168 -192 0 2 ) ( 168 168 -64 0 1 ) ( 168 168 64 0 0 ) ... )
                ...
            )
        }
    }

 */

static void ConvertPatch( FILE *f, int num, const bspDrawSurface_t& ds, const Vector3& origin ){
	/* only patches */
	if ( ds.surfaceType != MST_PATCH ) {
		return;
	}

	/* get shader */
	if ( ds.shaderNum < 0 || ds.shaderNum >= int( bspShaders.size() ) ) {
		return;
	}

	/* get texture name */
	const char      *texture;
	if ( const bspShader_t& shader = bspShaders[ ds.shaderNum ];
		striEqualPrefix( shader.shader, "textures/" ) ) {
		texture = shader.shader + 9;
	}
	else{
		texture = shader.shader;
	}

	/* start patch */
	fprintf( f, "\t// patch %d\n", num );
	fprintf( f, "\t{\n" );
	fprintf( f, "\t\tpatchDef2\n" );
	fprintf( f, "\t\t{\n" );
	fprintf( f, "\t\t\t%s\n", texture );
	fprintf( f, "\t\t\t( %d %d 0 0 0 )\n", ds.patchWidth, ds.patchHeight );
	fprintf( f, "\t\t\t(\n" );

	/* iterate through the verts */
	for ( int x = 0; x < ds.patchWidth; ++x )
	{
		/* start row */
		fprintf( f, "\t\t\t\t(" );

		/* iterate through the row */
		for ( int y = 0; y < ds.patchHeight; ++y )
		{
			/* get vert */
			const bspDrawVert_t& dv = bspDrawVerts[ ds.firstVert + ( y * ds.patchWidth ) + x ];

			/* offset it */
			const Vector3 xyz = dv.xyz + origin;

			/* print vertex */
			fprintf( f, " ( %f %f %f %f %f )", xyz[ 0 ], xyz[ 1 ], xyz[ 2 ], dv.st[ 0 ], dv.st[ 1 ] );
		}

		/* end row */
		fprintf( f, " )\n" );
	}

	/* end patch */
	fprintf( f, "\t\t\t)\n" );
	fprintf( f, "\t\t}\n" );
	fprintf( f, "\t}\n\n" );
}



/*
   ConvertModel()
   exports a bsp model to a map file
 */

static void ConvertModel( FILE *f, const bspModel_t& model, const Vector3& origin, EBrushType brushType ){
	if ( origin != g_vector3_identity ) {
		ConvertOriginBrush( f, -1, origin, brushType );
	}

	/* go through each brush in the model */
	if( fast ){
		for ( int i = 0; i < model.numBSPBrushes; ++i )
			ConvertBrushFast( f, model.firstBSPBrush + i, origin, brushType );
	}
	else{
		/* RBSP brush sides link to their surface; when the bsp has such links, only unlinked planar surfaces are search candidates */
		static std::vector<char> linkedSurfs;
		static bool hasLinks = false;
		if ( linkedSurfs.size() != bspDrawSurfaces.size() ) {
			linkedSurfs.assign( bspDrawSurfaces.size(), 0 );
			for ( const bspBrushSide_t& bs : bspBrushSides )
				if ( bs.surfaceNum >= 0 && bs.surfaceNum < int( linkedSurfs.size() ) ) { linkedSurfs[ bs.surfaceNum ] = 1; hasLinks = true; }
		}
		const bool exact = brushType == EBrushType::Quake && !g_decompile_legacy;
		ModelTriangles modelTriangles( model, exact && hasLinks ? &linkedSurfs : nullptr );
		for ( int i = 0; i < model.numBSPBrushes; ++i ){
			if ( &model == &bspModels[ 0 ] && terraindec::g_enabled && terraindec::g_brushFamily[ model.firstBSPBrush + i ] >= 0 )
				continue; // written in its own terrain func_group
			if ( exact )
				ConvertBrushExact( f, model.firstBSPBrush + i, origin, modelTriangles, BrushDetailFlag( model.firstBSPBrush + i ) );
			else
				ConvertBrush( f, model.firstBSPBrush + i, origin, brushType, modelTriangles );
		}
	}

	/* go through each drawsurf in the model */
	for ( int i = 0; i < model.numBSPSurfaces; ++i )
	{
		const int num = i + model.firstBSPSurface;
		const bspDrawSurface_t& ds = bspDrawSurfaces[ num ];

		/* we only love patches */
		if ( ds.surfaceType == MST_PATCH ) {
			ConvertPatch( f, num, ds, origin );
		}
	}
}



/*
   ConvertEPairs()
   exports entity key/value pairs to a map file
 */

static void ConvertEPairs( FILE *f, const entity_t& e, bool skip_origin ){
	/* walk epairs */
	for ( const auto& ep : e.epairs )
	{
		/* ignore empty keys/values */
		if ( ep.key.empty() || ep.value.empty() ) {
			continue;
		}

		/* ignore model keys with * prefixed values */
		if ( striEqual( ep.key.c_str(), "model" ) && ep.value.c_str()[ 0 ] == '*' ) {
			continue;
		}

		/* ignore origin keys if skip_origin is set */
		if ( skip_origin && striEqual( ep.key.c_str(), "origin" ) ) {
			continue;
		}

		/* emit the epair */
		fprintf( f, "\t\"%s\" \"%s\"\n", ep.key.c_str(), ep.value.c_str() );
	}
}



/*
   ConvertBSPToMap()
   exports an quake map file from the bsp
 */


static int ConvertBSPToMap_Ext( char *bspName, EBrushType brushType ){
	/* setup brush conversion prerequisites */
	{
		/* convert bsp planes to map planes */
		mapplanes.resize( bspPlanes.size() );
		for ( size_t i = 0; i < bspPlanes.size(); ++i )
		{
			plane_t& plane = mapplanes[i];
			plane.plane = bspPlanes[ i ];
			plane.type = PlaneTypeForNormal( plane.normal() );
			plane.hash_chain = 0;
		}

		/* allocate a build brush */
		buildBrush.sides.reserve( MAX_BUILD_SIDES );
		buildBrush.entityNum = 0;
		buildBrush.original = &buildBrush;
	}

	if( g_game->load == LoadRBSPFile )
		UnSetLightStyles();

	/* note it */
	Sys_Printf( "--- Convert BSP to MAP ---\n" );
	if ( brushType == EBrushType::Quake && !g_decompile_legacy && !fast )
		ExactComputeDefaults();

	terraindec::g_enabled = false;
	if ( brushType == EBrushType::Quake && !g_decompile_legacy && !fast )
		terraindec::setup( bspName );

	/* create map filename from the bsp name */
	const auto name = StringStream( PathExtensionless( bspName ), "_converted.map" );
	Sys_Printf( "writing %s\n", name.c_str() );

	/* open it */
	FILE *f = SafeOpenWrite( name );

	/* print header */
	fprintf( f, "// Generated by Q3Map2 (ydnar) -convert -format map\n" );

	/* walk entity list */
	for ( std::size_t i = 0; i < entities.size(); ++i )
	{
		/* get entity */
		const entity_t& e = entities[ i ];

		/* start entity */
		fprintf( f, "// entity %zu\n", i );
		fprintf( f, "{\n" );

		/* get model num */
		int modelNum;
		if ( i == 0 ) {
			modelNum = 0;
		}
		else
		{
			const char *value = e.valueForKey( "model" );
			if ( value[ 0 ] == '*' ) {
				modelNum = atoi( value + 1 );
			}
			else{
				modelNum = -1;
			}
		}

		/* export keys */
		ConvertEPairs( f, e, modelNum >= 0 );
		fprintf( f, "\n" );

		/* only handle bsp models */
		if ( modelNum >= 0 ) {
			/* convert model */
			ConvertModel( f, bspModels[ modelNum ], e.vectorForKey( "origin" ), brushType );
		}

		/* end entity */
		fprintf( f, "}\n\n" );

		/* KenRadiant: the world's index-map terrains, each in its func_group */
		if ( i == 0 && terraindec::g_enabled ) {
			std::vector<char> linked( bspDrawSurfaces.size(), 0 );
			bool hasLinks = false;
			for ( const bspBrushSide_t& bs : bspBrushSides )
				if ( bs.surfaceNum >= 0 && bs.surfaceNum < int( linked.size() ) ) { linked[ bs.surfaceNum ] = 1; hasLinks = true; }
			const ModelTriangles mt( bspModels[ 0 ], hasLinks ? &linked : nullptr );
			for ( const terraindec::Family& fam : terraindec::g_families ) {
				if ( fam.alphamap.empty() )
					continue;
				fprintf( f, "// terrain %s\n{\n\t\"classname\" \"func_group\"\n", fam.base.c_str() );
				fprintf( f, "\t\"alphamap\" \"%s\"\n\t\"layers\" \"%d\"\n\t\"shader\" \"%s\"\n", fam.alphamap.c_str(), fam.layers, fam.base.c_str() );
				fprintf( f, "\t\"min\" \"%g %g %g\"\n\t\"max\" \"%g %g %g\"\n\n", fam.bounds.mins[0], fam.bounds.mins[1], fam.bounds.mins[2],
				         fam.bounds.maxs[0], fam.bounds.maxs[1], fam.bounds.maxs[2] );
				for ( int bn : fam.brushes )
					ConvertBrushExact( f, bn, g_vector3_identity, mt, BrushDetailFlag( bn ), true );
				fprintf( f, "}\n\n" );
			}
		}
	}

	/* close the file and return */
	fclose( f );
	if ( brushType == EBrushType::Quake && !g_decompile_legacy && !fast ) {
		const auto& st = exactdec::g_stats;
		Sys_Printf( "exact decompile: %d sides from linked surface, %d from triangle search\n"
		            "                 %d exact (<= %g texel), %d inexact (worst %.3f texel)\n"
		            "                 %d hidden sides inherited from brush, %d used map default\n"
		            "                 %d bevel/degenerate sides dropped, %d sides with missing texture image\n",
		            st.linked, st.searched, st.exact, exactdec::g_tolTexel, st.inexact, st.worstErr,
		            st.hiddenBrush, st.hiddenGlobal, st.bevelsDropped, st.missingImage );
	}

	/* return to sender */
	return 0;
}

int ConvertBSPToMap( char *bspName ){
	return ConvertBSPToMap_Ext( bspName, EBrushType::Quake );
}

int ConvertBSPToMap_BP( char *bspName ){
	return ConvertBSPToMap_Ext( bspName, EBrushType::Bp );
}

int ConvertBSPToMap_220( char *bspName ){
	return ConvertBSPToMap_Ext( bspName, EBrushType::Valve220 );
}
