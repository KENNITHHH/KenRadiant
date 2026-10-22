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

#include "selection_mtor_translate.h"

#include "selection_.h"
#include "selection_render.h"
#include "selection_volume.h"
#include "selection_selector.h"
#include "selection_mtable_translate.h"
#include "selectionlib.h"

class TranslateManipulatorImpl final : public TranslateManipulator, public ManipulatorSelectionChangeable
{
	TranslateFree m_free;
	TranslateAxis m_axis;
	TranslatePlane m_plane;
	RenderableLine m_arrow_x;
	RenderableLine m_arrow_y;
	RenderableLine m_arrow_z;
	RenderableArrowHead m_arrow_head_x;
	RenderableArrowHead m_arrow_head_y;
	RenderableArrowHead m_arrow_head_z;
	RenderableQuad m_quad_screen;
	SelectableBool m_selectable_x;
	SelectableBool m_selectable_y;
	SelectableBool m_selectable_z;
	SelectableBool m_selectable_screen;
	// KenRadiant: Unreal-style plane handles; index = plane normal axis (0: YZ, 1: XZ, 2: XY)
	RenderableQuad m_quad_plane[3];
	RenderableArrowHead m_fill_plane[3]{ RenderableArrowHead( 12 ), RenderableArrowHead( 12 ), RenderableArrowHead( 12 ) }; // both windings
	SelectableBool m_selectable_plane[3];
	Pivot2World m_pivot;
public:
	TranslateManipulatorImpl( Translatable& translatable, std::size_t segments, float length ) :
		m_free( translatable ),
		m_axis( translatable ),
		m_plane( translatable ),
		m_arrow_head_x( 3 * 2 * ( segments << 3 ) ),
		m_arrow_head_y( 3 * 2 * ( segments << 3 ) ),
		m_arrow_head_z( 3 * 2 * ( segments << 3 ) ){
		draw_arrowline( length, m_arrow_x.m_line, 0 );
		draw_arrowhead( segments, length, m_arrow_head_x.m_vertices.data(), TripleRemapXYZ<Vertex3f>(), TripleRemapXYZ<Normal3f>() );
		draw_arrowline( length, m_arrow_y.m_line, 1 );
		draw_arrowhead( segments, length, m_arrow_head_y.m_vertices.data(), TripleRemapYZX<Vertex3f>(), TripleRemapYZX<Normal3f>() );
		draw_arrowline( length, m_arrow_z.m_line, 2 );
		draw_arrowhead( segments, length, m_arrow_head_z.m_vertices.data(), TripleRemapZXY<Vertex3f>(), TripleRemapZXY<Normal3f>() );

		draw_quad( 16, m_quad_screen.m_quad );

		const float a = length * 0.2f, b = length * 0.6f; // Unreal-sized: ~40% of the arrow
		for( int n = 0; n < 3; ++n ){
			const int u = ( n + 1 ) % 3, v = ( n + 2 ) % 3;
			const float uv[4][2] = { { a, a }, { b, a }, { b, b }, { a, b } };
			for( int i = 0; i < 4; ++i ){
				Vector3 p( 0, 0, 0 );
				p[u] = uv[i][0];
				p[v] = uv[i][1];
				m_quad_plane[n].m_quad[i].vertex = vertex3f_for_vector3( p );
			}
			// filled square, two triangles per side
			const int order[12] = { 0, 1, 2, 0, 2, 3,   0, 2, 1, 0, 3, 2 };
			for( int i = 0; i < 12; ++i ){
				m_fill_plane[n].m_vertices[i].vertex = m_quad_plane[n].m_quad[order[i]].vertex;
				Vector3 normal( 0, 0, 0 );
				normal[n] = i < 6 ? 1 : -1;
				m_fill_plane[n].m_vertices[i].normal = normal3f_for_vector3( normal );
			}
		}
	}

	void UpdateColours(){
		m_arrow_x.setColour( colourSelected( g_colour_x, m_selectable_x.isSelected() ) );
		m_arrow_head_x.setColour( colourSelected( g_colour_x, m_selectable_x.isSelected() ) );
		m_arrow_y.setColour( colourSelected( g_colour_y, m_selectable_y.isSelected() ) );
		m_arrow_head_y.setColour( colourSelected( g_colour_y, m_selectable_y.isSelected() ) );
		m_arrow_z.setColour( colourSelected( g_colour_z, m_selectable_z.isSelected() ) );
		m_arrow_head_z.setColour( colourSelected( g_colour_z, m_selectable_z.isSelected() ) );
		m_quad_screen.setColour( colourSelected( g_colour_screen, m_selectable_screen.isSelected() ) );
		const Colour4b* plane_colours[3] = { &g_colour_x, &g_colour_y, &g_colour_z };
		for( int n = 0; n < 3; ++n ){
			const Colour4b colour = colourSelected( *plane_colours[n], m_selectable_plane[n].isSelected() );
			m_quad_plane[n].setColour( colour );
			m_fill_plane[n].setColour( Colour4b( colour.r, colour.g, colour.b, 96 ) );
		}
	}

	/// plane handle is usable unless the plane is seen (nearly) edge-on
	bool manipulator_show_plane( const Pivot2World& pivot, const Vector3& normal ){
		return std::fabs( vector3_dot( pivot.m_axis_screen, normal ) ) > 0.2;
	}

	bool manipulator_show_axis( const Pivot2World& pivot, const Vector3& axis ){
		return std::fabs( vector3_dot( pivot.m_axis_screen, axis ) ) < 0.95;
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		m_pivot.update( pivot2world, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );

		// temp hack
		UpdateColours();

		Vector3 x = vector3_normalised( m_pivot.m_worldSpace.x().vec3() );
		bool show_x = manipulator_show_axis( m_pivot, x );

		Vector3 y = vector3_normalised( m_pivot.m_worldSpace.y().vec3() );
		bool show_y = manipulator_show_axis( m_pivot, y );

		Vector3 z = vector3_normalised( m_pivot.m_worldSpace.z().vec3() );
		bool show_z = manipulator_show_axis( m_pivot, z );

		renderer.SetState( m_state_wire, Renderer::eWireframeOnly );
		renderer.SetState( m_state_wire, Renderer::eFullMaterials );

		if ( show_x ) {
			renderer.addRenderable( m_arrow_x, m_pivot.m_worldSpace );
		}
		if ( show_y ) {
			renderer.addRenderable( m_arrow_y, m_pivot.m_worldSpace );
		}
		if ( show_z ) {
			renderer.addRenderable( m_arrow_z, m_pivot.m_worldSpace );
		}

		renderer.addRenderable( m_quad_screen, m_pivot.m_viewplaneSpace );

		if( manipulator_show_plane( m_pivot, x ) )
			renderer.addRenderable( m_quad_plane[0], m_pivot.m_worldSpace );
		if( manipulator_show_plane( m_pivot, y ) )
			renderer.addRenderable( m_quad_plane[1], m_pivot.m_worldSpace );
		if( manipulator_show_plane( m_pivot, z ) )
			renderer.addRenderable( m_quad_plane[2], m_pivot.m_worldSpace );

		renderer.SetState( m_state_fill, Renderer::eWireframeOnly );
		renderer.SetState( m_state_fill, Renderer::eFullMaterials );

		if ( show_x ) {
			renderer.addRenderable( m_arrow_head_x, m_pivot.m_worldSpace );
		}
		if ( show_y ) {
			renderer.addRenderable( m_arrow_head_y, m_pivot.m_worldSpace );
		}
		if ( show_z ) {
			renderer.addRenderable( m_arrow_head_z, m_pivot.m_worldSpace );
		}
		for( int n = 0; n < 3; ++n )
			if( manipulator_show_plane( m_pivot, n == 0 ? x : n == 1 ? y : z ) )
				renderer.addRenderable( m_fill_plane[n], m_pivot.m_worldSpace );
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		if( g_modifiers != c_modifierNone && g_modifiers != c_modifierAlt ) // KenRadiant: Alt+drag duplicates
			return selectionChange( nullptr );

		m_pivot.update( pivot2world, view.GetModelview(), view.GetProjection(), view.GetViewport() );

		SelectionPool selector;

		Vector3 x = vector3_normalised( m_pivot.m_worldSpace.x().vec3() );
		bool show_x = manipulator_show_axis( m_pivot, x );

		Vector3 y = vector3_normalised( m_pivot.m_worldSpace.y().vec3() );
		bool show_y = manipulator_show_axis( m_pivot, y );

		Vector3 z = vector3_normalised( m_pivot.m_worldSpace.z().vec3() );
		bool show_z = manipulator_show_axis( m_pivot, z );

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_viewpointSpace ) );

			{
				SelectionIntersection best;
				Quad_BestPoint( local2view, EClipCull::CW, m_quad_screen.m_quad, best );
				if ( best.valid() ) {
					best = SelectionIntersection( 0, 0 );
					selector.addSelectable( best, &m_selectable_screen );
				}
			}
		}

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_worldSpace ) );

#if defined( DEBUG_SELECTION )
			g_render_clipped.construct( view.GetViewMatrix() );
#endif

			if ( show_x ) {
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_x.m_line, best );
				Triangles_BestPoint( local2view, EClipCull::CW, m_arrow_head_x.m_vertices.begin(), m_arrow_head_x.m_vertices.end(), best );
				selector.addSelectable( best, &m_selectable_x );
			}

			if ( show_y ) {
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_y.m_line, best );
				Triangles_BestPoint( local2view, EClipCull::CW, m_arrow_head_y.m_vertices.begin(), m_arrow_head_y.m_vertices.end(), best );
				selector.addSelectable( best, &m_selectable_y );
			}

			if ( show_z ) {
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_z.m_line, best );
				Triangles_BestPoint( local2view, EClipCull::CW, m_arrow_head_z.m_vertices.begin(), m_arrow_head_z.m_vertices.end(), best );
				selector.addSelectable( best, &m_selectable_z );
			}

			const Vector3 normals[3] = { x, y, z };
			for( int n = 0; n < 3; ++n ){
				if( manipulator_show_plane( m_pivot, normals[n] ) ){
					SelectionIntersection best;
					Quad_BestPoint( local2view, EClipCull::None, m_quad_plane[n].m_quad, best );
					selector.addSelectable( best, &m_selectable_plane[n] );
				}
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
			if ( m_selectable_plane[n].isSelected() ) {
				m_plane.SetNormal( g_vector3_axes[n] );
				return &m_plane;
			}
		}
		return &m_free;
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


TranslateManipulator* New_TranslateManipulator( Translatable& translatable, std::size_t segments, float length ){
	return new TranslateManipulatorImpl( translatable, segments, length );
}
