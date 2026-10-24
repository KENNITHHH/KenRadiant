// KenRadiant 3D view program: live light preview (eye-space position + normal for the fragment shader),
// tcMod turb and deformVertexes wave / move / bulge, fixed-function look when the preview is off
uniform int u_mode;      // 0 = unlit (fixed-function look), 1 = lit preview, 2 = fullbright glow
uniform int u_fflight;   // fixed-function lighting on (u_mode 0)
uniform float u_time;
uniform int u_turb_on;
uniform vec4 u_turb;     // base amp phase freq
uniform int u_deform;    // 0 none, 1 wave, 2 move, 3 bulge
uniform int u_dfunc;     // 1 sin, 2 triangle, 3 square, 4 sawtooth, 5 inversesawtooth
uniform float u_ddiv;
uniform vec4 u_dwave;    // base amp phase freq
uniform vec3 u_dmove;
uniform vec3 u_bulge;    // width height speed
uniform int u_env;       // tcGen environment
uniform vec3 u_cam_vs;   // camera position (world)

varying vec3 v_pos;
varying vec3 v_normal;
varying vec3 v_world; // terrain index map lookup (brushes and patches are in world space)

float wave( int func, vec4 w, float offset ){
	float x = w.z + offset + u_time * w.w;
	float f = fract( x );
	float v = 0.0;
	if( func == 1 ) v = sin( x * 6.2831853 );
	else if( func == 2 ) v = 4.0 * abs( fract( x - 0.25 ) - 0.5 ) - 1.0;
	else if( func == 3 ) v = f < 0.5 ? 1.0 : -1.0;
	else if( func == 4 ) v = f;
	else if( func == 5 ) v = 1.0 - f;
	return w.x + w.y * v;
}

void main()
{
	vec4 vertex = gl_Vertex;
	vec2 st = gl_MultiTexCoord0.st;
	if( u_deform == 1 ){
		float off = ( vertex.x + vertex.y + vertex.z ) * u_ddiv;
		vertex.xyz += gl_Normal * wave( u_dfunc, u_dwave, off );
	}
	else if( u_deform == 2 ){
		vertex.xyz += u_dmove * wave( u_dfunc, u_dwave, 0.0 );
	}
	else if( u_deform == 3 ){
		vertex.xyz += gl_Normal * sin( st.s * u_bulge.x + u_time * u_bulge.z ) * u_bulge.y;
	}
	if( u_turb_on != 0 ){ // Quake 3 tcMod turb
		float now = u_turb.z + u_time * u_turb.w;
		st.s += sin( ( ( gl_Vertex.x + gl_Vertex.z ) * 1.0 / 128.0 * 0.125 + now ) * 6.2831853 ) * u_turb.y;
		st.t += sin( ( gl_Vertex.y * 1.0 / 128.0 * 0.125 + now ) * 6.2831853 ) * u_turb.y;
	}

	if( u_env != 0 ){ // Quake 3 tcGen environment
		vec3 viewer = normalize( u_cam_vs - vertex.xyz );
		vec3 n = normalize( gl_Normal );
		vec3 reflected = n * 2.0 * dot( n, viewer ) - viewer;
		st = vec2( 0.5 + reflected.y * 0.5, 0.5 - reflected.z * 0.5 );
	}
	v_world = vertex.xyz;
	v_pos = vec3( gl_ModelViewMatrix * vertex );
	v_normal = gl_NormalMatrix * gl_Normal;
	gl_TexCoord[0] = gl_TextureMatrix[0] * vec4( st, gl_MultiTexCoord0.zw ); // tcMod scroll / rotate / scale / stretch
	if( u_mode == 0 && u_fflight != 0 ){
		// what GL_LIGHTING + GL_COLOR_MATERIAL with the 3D view's light 0 gives
		vec3 n = normalize( v_normal );
		float d = max( dot( n, normalize( gl_LightSource[0].position.xyz ) ), 0.0 );
		vec3 c = gl_Color.rgb * ( gl_LightModel.ambient.rgb + gl_LightSource[0].ambient.rgb + gl_LightSource[0].diffuse.rgb * d );
		gl_FrontColor = vec4( c, gl_Color.a );
	}
	else
		gl_FrontColor = gl_Color;
	// ftransform() gives exactly the fixed-function depth, so selection tint / wires drawn over it don't z-fight
	if( u_deform == 0 )
		gl_Position = ftransform();
	else
		gl_Position = gl_ModelViewProjectionMatrix * vertex;
}
