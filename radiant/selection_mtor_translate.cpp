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

/// KenRadiant: line drawn thicker (Unreal-style gizmo)
struct RenderableThickLine : public RenderableLine
{
	float m_width = 3;
	void render( RenderStateFlags state ) const override {
		gl().glLineWidth( m_width );
		RenderableLine::render( state );
		gl().glLineWidth( 1 );
	}
};

/// Unreal Engine style move gizmo:
/// - thick red/green/blue shafts with cone heads, the hovered one turns yellow
/// - plane handles: an L of two short lines (in the colours of the two axes it moves along) closing a square between the arrows,
///   with a faint fill that turns yellow on hover; dragging it moves only in that plane
/// - a white ball in the middle: drag to move in the screen plane
class TranslateManipulatorImpl final : public TranslateManipulator, public ManipulatorSelectionChangeable
{
	TranslateFree m_free;
	TranslateAxis m_axis;
	TranslatePlane m_plane;
	RenderableThickLine m_arrow_x;
	RenderableThickLine m_arrow_y;
	RenderableThickLine m_arrow_z;
	RenderableArrowHead m_arrow_head_x;
	RenderableArrowHead m_arrow_head_y;
	RenderableArrowHead m_arrow_head_z;
	RenderableQuad m_quad_screen;          // hit area of the centre ball (not drawn)
	RenderableArrowHead m_ball{ 2 * 3 * 24 }; // centre ball: disc facing the viewer, both windings
	SelectableBool m_selectable_x;
	SelectableBool m_selectable_y;
	SelectableBool m_selectable_z;
	SelectableBool m_selectable_screen;
	// plane handles; index = plane normal axis (0: YZ, 1: XZ, 2: XY)
	RenderableQuad m_quad_plane[3];        // hit area (not drawn)
	RenderableThickLine m_plane_line[3][2];
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

		// centre ball, ~7 px radius, in view-plane space; hit area slightly bigger
		draw_quad( 20, m_quad_screen.m_quad );
		{
			const float r = 7;
			for( int i = 0; i < 24; ++i ){
				const double a0 = c_2pi * i / 24, a1 = c_2pi * ( i + 1 ) / 24;
				const Vector3 p[3] = { Vector3( 0, 0, 0 ), Vector3( r * cos( a0 ), r * sin( a0 ), 0 ), Vector3( r * cos( a1 ), r * sin( a1 ), 0 ) };
				for( int k = 0; k < 3; ++k ){
					m_ball.m_vertices[i * 3 + k].vertex = vertex3f_for_vector3( p[k] );
					m_ball.m_vertices[i * 3 + k].normal = normal3f_for_vector3( Vector3( 0, 0, 1 ) );
					m_ball.m_vertices[72 + i * 3 + k].vertex = vertex3f_for_vector3( p[2 - k] );
					m_ball.m_vertices[72 + i * 3 + k].normal = normal3f_for_vector3( Vector3( 0, 0, -1 ) );
				}
			}
		}

		// plane handles: square from (a,a) to (b,b) in the plane's two axes
		const float a = length * 0.15f, b = length * 0.5f;
		for( int n = 0; n < 3; ++n ){
			const int u = ( n + 1 ) % 3, v = ( n + 2 ) % 3;
			const auto point = [&]( float pu, float pv ){ Vector3 p( 0, 0, 0 ); p[u] = pu; p[v] = pv; return p; };
			const Vector3 quad[4] = { point( a, a ), point( b, a ), point( b, b ), point( a, b ) };
			for( int i = 0; i < 4; ++i )
				m_quad_plane[n].m_quad[i].vertex = vertex3f_for_vector3( quad[i] );
			// L: from the u axis up to the corner, and from the corner back to the v axis
			m_plane_line[n][0].m_line[0].vertex = vertex3f_for_vector3( point( b, 0 ) );
			m_plane_line[n][0].m_line[1].vertex = vertex3f_for_vector3( point( b, b ) );
			m_plane_line[n][1].m_line[0].vertex = vertex3f_for_vector3( point( b, b ) );
			m_plane_line[n][1].m_line[1].vertex = vertex3f_for_vector3( point( 0, b ) );
			m_plane_line[n][0].m_width = m_plane_line[n][1].m_width = 2;
			// filled square from the pivot to the corner, two triangles per side
			const Vector3 fill[4] = { point( 0, 0 ), point( b, 0 ), point( b, b ), point( 0, b ) };
			const int order[12] = { 0, 1, 2, 0, 2, 3,   0, 2, 1, 0, 3, 2 };
			for( int i = 0; i < 12; ++i ){
				m_fill_plane[n].m_vertices[i].vertex = vertex3f_for_vector3( fill[order[i]] );
				Vector3 normal( 0, 0, 0 );
				normal[n] = i < 6 ? 1 : -1;
				m_fill_plane[n].m_vertices[i].normal = normal3f_for_vector3( normal );
			}
		}
	}

	void UpdateColours(){
		const Colour4b* axis_colours[3] = { &g_colour_x, &g_colour_y, &g_colour_z };
		const bool axis_selected[3] = { m_selectable_x.isSelected(), m_selectable_y.isSelected(), m_selectable_z.isSelected() };
		m_arrow_x.setColour( colourSelected( g_colour_x, axis_selected[0] ) );
		m_arrow_head_x.setColour( colourSelected( g_colour_x, axis_selected[0] ) );
		m_arrow_y.setColour( colourSelected( g_colour_y, axis_selected[1] ) );
		m_arrow_head_y.setColour( colourSelected( g_colour_y, axis_selected[1] ) );
		m_arrow_z.setColour( colourSelected( g_colour_z, axis_selected[2] ) );
		m_arrow_head_z.setColour( colourSelected( g_colour_z, axis_selected[2] ) );
		m_ball.setColour( m_selectable_screen.isSelected() ? g_colour_selected : Colour4b( 230, 230, 230, 255 ) );
		for( int n = 0; n < 3; ++n ){
			const int u = ( n + 1 ) % 3, v = ( n + 2 ) % 3;
			const bool selected = m_selectable_plane[n].isSelected();
			m_plane_line[n][0].setColour( colourSelected( *axis_colours[v], selected ) ); // runs along v
			m_plane_line[n][1].setColour( colourSelected( *axis_colours[u], selected ) ); // runs along u
			m_fill_plane[n].setColour( selected ? Colour4b( 255, 255, 0, 110 ) : Colour4b( 255, 255, 255, 28 ) );
		}
	}

	bool manipulator_show_axis( const Pivot2World& pivot, const Vector3& axis ){
		return std::fabs( vector3_dot( pivot.m_axis_screen, axis ) ) < 0.95;
	}
	/// plane handle is usable unless the plane is seen (nearly) edge-on
	bool manipulator_show_plane( const Pivot2World& pivot, const Vector3& normal ){
		return std::fabs( vector3_dot( pivot.m_axis_screen, normal ) ) > 0.2;
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		m_pivot.update( pivot2world, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );

		// temp hack
		UpdateColours();

		const Vector3 axes[3] = { vector3_normalised( m_pivot.m_worldSpace.x().vec3() ),
		                          vector3_normalised( m_pivot.m_worldSpace.y().vec3() ),
		                          vector3_normalised( m_pivot.m_worldSpace.z().vec3() ) };
		const bool show[3] = { manipulator_show_axis( m_pivot, axes[0] ), manipulator_show_axis( m_pivot, axes[1] ), manipulator_show_axis( m_pivot, axes[2] ) };

		renderer.SetState( m_state_wire, Renderer::eWireframeOnly );
		renderer.SetState( m_state_wire, Renderer::eFullMaterials );

		if ( show[0] ) renderer.addRenderable( m_arrow_x, m_pivot.m_worldSpace );
		if ( show[1] ) renderer.addRenderable( m_arrow_y, m_pivot.m_worldSpace );
		if ( show[2] ) renderer.addRenderable( m_arrow_z, m_pivot.m_worldSpace );
		for( int n = 0; n < 3; ++n )
			if( manipulator_show_plane( m_pivot, axes[n] ) ){
				renderer.addRenderable( m_plane_line[n][0], m_pivot.m_worldSpace );
				renderer.addRenderable( m_plane_line[n][1], m_pivot.m_worldSpace );
			}

		renderer.SetState( m_state_fill, Renderer::eWireframeOnly );
		renderer.SetState( m_state_fill, Renderer::eFullMaterials );

		if ( show[0] ) renderer.addRenderable( m_arrow_head_x, m_pivot.m_worldSpace );
		if ( show[1] ) renderer.addRenderable( m_arrow_head_y, m_pivot.m_worldSpace );
		if ( show[2] ) renderer.addRenderable( m_arrow_head_z, m_pivot.m_worldSpace );
		for( int n = 0; n < 3; ++n )
			if( manipulator_show_plane( m_pivot, axes[n] ) )
				renderer.addRenderable( m_fill_plane[n], m_pivot.m_worldSpace );
		renderer.addRenderable( m_ball, m_pivot.m_viewplaneSpace );
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		if( g_modifiers != c_modifierNone && g_modifiers != c_modifierAlt ) // Alt+drag duplicates
			return selectionChange( nullptr );

		m_pivot.update( pivot2world, view.GetModelview(), view.GetProjection(), view.GetViewport() );

		SelectionPool selector;

		const Vector3 x = vector3_normalised( m_pivot.m_worldSpace.x().vec3() );
		const Vector3 y = vector3_normalised( m_pivot.m_worldSpace.y().vec3() );
		const Vector3 z = vector3_normalised( m_pivot.m_worldSpace.z().vec3() );
		const bool show_x = manipulator_show_axis( m_pivot, x );
		const bool show_y = manipulator_show_axis( m_pivot, y );
		const bool show_z = manipulator_show_axis( m_pivot, z );

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_viewpointSpace ) );
			SelectionIntersection best;
			Quad_BestPoint( local2view, EClipCull::CW, m_quad_screen.m_quad, best );
			if ( best.valid() ) {
				best = SelectionIntersection( 0, 0 );
				selector.addSelectable( best, &m_selectable_screen );
			}
		}

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_worldSpace ) );

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
					Line_BestPoint( local2view, m_plane_line[n][0].m_line, best );
					Line_BestPoint( local2view, m_plane_line[n][1].m_line, best );
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
