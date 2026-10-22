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

#include "selection_mtor_scale.h"

#include "selection_.h"
#include "selection_render.h"
#include "selection_volume.h"
#include "selection_selector.h"
#include "selection_mtable_scale.h"
#include "selectionlib.h"

struct ScaleThickLine : public RenderableLine
{
	float m_width = 3;
	void render( RenderStateFlags state ) const override {
		gl().glLineWidth( m_width );
		RenderableLine::render( state );
		gl().glLineWidth( 1 );
	}
};

/// filled cube centred on \p c with half size \p h, 36 flat-shaded vertices
inline void draw_cube( FlatShadedVertex* v, const Vector3& c, float h ){
	static const int faces[6][4] = { {0,2,3,1}, {4,5,7,6}, {0,1,5,4}, {2,6,7,3}, {0,4,6,2}, {1,3,7,5} };
	static const float normals[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };
	const auto corner = [&]( int i ){ return Vector3( c[0] + ( ( i & 4 ) ? h : -h ), c[1] + ( ( i & 2 ) ? h : -h ), c[2] + ( ( i & 1 ) ? h : -h ) ); };
	int k = 0;
	for( int f = 0; f < 6; ++f ){
		const int tri[6] = { faces[f][0], faces[f][1], faces[f][2], faces[f][0], faces[f][2], faces[f][3] };
		for( int t = 0; t < 6; ++t ){
			v[k].vertex = vertex3f_for_vector3( corner( tri[t] ) );
			v[k].normal = normal3f_for_vector3( Vector3( normals[f][0], normals[f][1], normals[f][2] ) );
			++k;
		}
	}
}

/// KenRadiant: Unreal Engine style scale gizmo
/// - thick axis lines ending in cubes: scale along that axis
/// - triangles between two axes: scale those two axes
/// - white centre cube: uniform scale (drag away from / towards the centre)
class ScaleManipulatorImpl final : public ScaleManipulator, public ManipulatorSelectionChangeable
{
	ScaleFree m_free;
	ScaleAxis m_axis;
	ScaleUniform m_uniform;
	ScaleThickLine m_arrow_x;
	ScaleThickLine m_arrow_y;
	ScaleThickLine m_arrow_z;
	RenderableArrowHead m_cube_x{ 36 }, m_cube_y{ 36 }, m_cube_z{ 36 }, m_cube_centre{ 36 };
	RenderableQuad m_quad_screen; // hit area of the centre cube (not drawn)
	SelectableBool m_selectable_x;
	SelectableBool m_selectable_y;
	SelectableBool m_selectable_z;
	SelectableBool m_selectable_screen;
	// plane triangles; index = normal axis (0: YZ, 1: XZ, 2: XY)
	ScaleThickLine m_plane_line[3];
	RenderableArrowHead m_plane_fill[3]{ RenderableArrowHead( 6 ), RenderableArrowHead( 6 ), RenderableArrowHead( 6 ) };
	SelectableBool m_selectable_plane[3];
	Pivot2World m_pivot;
public:
	ScaleManipulatorImpl( Scalable& scalable, std::size_t segments, float length ) :
		m_free( scalable ),
		m_axis( scalable ),
		m_uniform( scalable ){
		const float cube = 5;
		const float shaft = length - cube;
		for( int axis = 0; axis < 3; ++axis ){
			ScaleThickLine& line = axis == 0 ? m_arrow_x : axis == 1 ? m_arrow_y : m_arrow_z;
			Vector3 end( 0, 0, 0 );
			end[axis] = shaft;
			line.m_line[0].vertex = vertex3f_identity;
			line.m_line[1].vertex = vertex3f_for_vector3( end );
			end[axis] = shaft + cube;
			draw_cube( ( axis == 0 ? m_cube_x : axis == 1 ? m_cube_y : m_cube_z ).m_vertices.data(), end, cube );
		}
		draw_cube( m_cube_centre.m_vertices.data(), Vector3( 0, 0, 0 ), 6 );
		draw_quad( 16, m_quad_screen.m_quad );

		const float a = length * 0.45f, b = length * 0.6f;
		for( int n = 0; n < 3; ++n ){
			const int u = ( n + 1 ) % 3, v = ( n + 2 ) % 3;
			const auto point = [&]( float pu, float pv ){ Vector3 p( 0, 0, 0 ); p[u] = pu; p[v] = pv; return p; };
			m_plane_line[n].m_line[0].vertex = vertex3f_for_vector3( point( b, 0 ) );
			m_plane_line[n].m_line[1].vertex = vertex3f_for_vector3( point( 0, b ) );
			m_plane_line[n].m_width = 2;
			// band between the two diagonals at a and b, both windings
			const Vector3 q[4] = { point( a, 0 ), point( b, 0 ), point( 0, b ), point( 0, a ) };
			(void)q;
			const Vector3 tri[3] = { point( a, 0 ), point( b, 0 ), point( 0, b ) };
			const Vector3 tri2[3] = { point( a, 0 ), point( 0, b ), point( 0, a ) };
			// 6 vertices: one band of two triangles; drawn with culling off by the overlay state, hit test without culling
			const Vector3 all[6] = { tri[0], tri[1], tri[2], tri2[0], tri2[1], tri2[2] };
			for( int i = 0; i < 6; ++i ){
				m_plane_fill[n].m_vertices[i].vertex = vertex3f_for_vector3( all[i] );
				Vector3 normal( 0, 0, 0 );
				normal[n] = 1;
				m_plane_fill[n].m_vertices[i].normal = normal3f_for_vector3( normal );
			}
		}
	}

	void UpdateColours(){
		const Colour4b* axis_colours[3] = { &g_colour_x, &g_colour_y, &g_colour_z };
		m_arrow_x.setColour( colourSelected( g_colour_x, m_selectable_x.isSelected() ) );
		m_arrow_y.setColour( colourSelected( g_colour_y, m_selectable_y.isSelected() ) );
		m_arrow_z.setColour( colourSelected( g_colour_z, m_selectable_z.isSelected() ) );
		m_cube_x.setColour( colourSelected( g_colour_x, m_selectable_x.isSelected() ) );
		m_cube_y.setColour( colourSelected( g_colour_y, m_selectable_y.isSelected() ) );
		m_cube_z.setColour( colourSelected( g_colour_z, m_selectable_z.isSelected() ) );
		m_cube_centre.setColour( m_selectable_screen.isSelected() ? g_colour_selected : Colour4b( 230, 230, 230, 255 ) );
		for( int n = 0; n < 3; ++n ){
			const bool selected = m_selectable_plane[n].isSelected();
			// Unreal: the diagonal takes the colour of the plane's normal axis
			m_plane_line[n].setColour( colourSelected( *axis_colours[n], selected ) );
			m_plane_fill[n].setColour( selected ? Colour4b( 255, 255, 0, 110 ) : Colour4b( 255, 255, 255, 28 ) );
		}
	}

	bool show_plane( const Vector3& normal ){
		return std::fabs( vector3_dot( m_pivot.m_axis_screen, normal ) ) > 0.2;
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		m_pivot.update( pivot2world, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );

		// temp hack
		UpdateColours();

		const Vector3 axes[3] = { vector3_normalised( m_pivot.m_worldSpace.x().vec3() ),
		                          vector3_normalised( m_pivot.m_worldSpace.y().vec3() ),
		                          vector3_normalised( m_pivot.m_worldSpace.z().vec3() ) };

		renderer.SetState( m_state_wire, Renderer::eWireframeOnly );
		renderer.SetState( m_state_wire, Renderer::eFullMaterials );
		renderer.addRenderable( m_arrow_x, m_pivot.m_worldSpace );
		renderer.addRenderable( m_arrow_y, m_pivot.m_worldSpace );
		renderer.addRenderable( m_arrow_z, m_pivot.m_worldSpace );
		for( int n = 0; n < 3; ++n )
			if( show_plane( axes[n] ) )
				renderer.addRenderable( m_plane_line[n], m_pivot.m_worldSpace );

		renderer.SetState( m_state_fill, Renderer::eWireframeOnly );
		renderer.SetState( m_state_fill, Renderer::eFullMaterials );
		renderer.addRenderable( m_cube_x, m_pivot.m_worldSpace );
		renderer.addRenderable( m_cube_y, m_pivot.m_worldSpace );
		renderer.addRenderable( m_cube_z, m_pivot.m_worldSpace );
		renderer.addRenderable( m_cube_centre, m_pivot.m_worldSpace );
		for( int n = 0; n < 3; ++n )
			if( show_plane( axes[n] ) )
				renderer.addRenderable( m_plane_fill[n], m_pivot.m_worldSpace );
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		if( g_modifiers != c_modifierNone && g_modifiers != c_modifierAlt ) // Alt+drag duplicates
			return selectionChange( nullptr );

		m_pivot.update( pivot2world, view.GetModelview(), view.GetProjection(), view.GetViewport() );

		SelectionPool selector;

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_viewpointSpace ) );
			SelectionIntersection best;
			Quad_BestPoint( local2view, EClipCull::CW, m_quad_screen.m_quad, best );
			if ( best.valid() ) {
				best = SelectionIntersection( 0, 0 ); // centre cube wins
				selector.addSelectable( best, &m_selectable_screen );
			}
		}
		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_worldSpace ) );
			const auto axisTest = [&]( ScaleThickLine& line, RenderableArrowHead& cube, SelectableBool& selectable ){
				SelectionIntersection best;
				Line_BestPoint( local2view, line.m_line, best );
				Triangles_BestPoint( local2view, EClipCull::None, cube.m_vertices.begin(), cube.m_vertices.end(), best );
				selector.addSelectable( best, &selectable );
			};
			axisTest( m_arrow_x, m_cube_x, m_selectable_x );
			axisTest( m_arrow_y, m_cube_y, m_selectable_y );
			axisTest( m_arrow_z, m_cube_z, m_selectable_z );

			const Vector3 axes[3] = { vector3_normalised( m_pivot.m_worldSpace.x().vec3() ),
			                          vector3_normalised( m_pivot.m_worldSpace.y().vec3() ),
			                          vector3_normalised( m_pivot.m_worldSpace.z().vec3() ) };
			for( int n = 0; n < 3; ++n ){
				if( !show_plane( axes[n] ) )
					continue;
				SelectionIntersection best;
				Triangles_BestPoint( local2view, EClipCull::None, m_plane_fill[n].m_vertices.begin(), m_plane_fill[n].m_vertices.end(), best );
				Line_BestPoint( local2view, m_plane_line[n].m_line, best );
				selector.addSelectable( best, &m_selectable_plane[n] );
			}
		}

		selectionChange( selector );
	}

	Manipulatable* GetManipulatable() override {
		if ( m_selectable_x.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_x );
			return &m_axis;
		}
		else if ( m_selectable_y.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_y );
			return &m_axis;
		}
		else if ( m_selectable_z.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_z );
			return &m_axis;
		}
		for( int n = 0; n < 3; ++n ){
			if( m_selectable_plane[n].isSelected() ){
				m_free.SetAxes( g_vector3_axes[( n + 1 ) % 3], g_vector3_axes[( n + 2 ) % 3] );
				return &m_free;
			}
		}
		return &m_uniform;
	}

	void setSelected( bool select ) override {
		m_selectable_x.setSelected( select );
		m_selectable_y.setSelected( select );
		m_selectable_z.setSelected( select );
		m_selectable_screen.setSelected( select );
		for( auto& p : m_selectable_plane )
			p.setSelected( select );
	}
	bool isSelected() const override {
		return m_selectable_x.isSelected()
		    || m_selectable_y.isSelected()
		    || m_selectable_z.isSelected()
		    || m_selectable_screen.isSelected()
		    || m_selectable_plane[0].isSelected()
		    || m_selectable_plane[1].isSelected()
		    || m_selectable_plane[2].isSelected();
	}
};


ScaleManipulator* New_ScaleManipulator( Scalable& scalable, std::size_t segments, float length ){
	return new ScaleManipulatorImpl( scalable, segments, length );
}
