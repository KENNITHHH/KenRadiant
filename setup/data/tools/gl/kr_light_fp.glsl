// KenRadiant live light preview: texture * ( ambient + sun + point lights ), lights in eye space, no shadows
uniform sampler2D u_tex;
uniform int u_count;
uniform vec3 u_pos[24];
uniform vec3 u_col[24];
uniform float u_rad[24];
uniform vec3 u_sun_dir;
uniform vec3 u_sun_col;
uniform vec3 u_ambient;

varying vec3 v_pos;
varying vec3 v_normal;

void main()
{
	vec4 tex = texture2D( u_tex, gl_TexCoord[0].st );
	vec3 n = normalize( v_normal );
	if( !gl_FrontFacing )
		n = -n;

	vec3 light = u_ambient + u_sun_col * max( dot( n, u_sun_dir ), 0.0 );
	for( int i = 0; i < 24; ++i ){
		if( i >= u_count )
			break;
		vec3 d = u_pos[i] - v_pos;
		float dist = length( d );
		float a = clamp( 1.0 - dist / u_rad[i], 0.0, 1.0 );
		light += u_col[i] * ( a * a ) * max( dot( n, d / max( dist, 0.001 ) ), 0.0 );
	}
	gl_FragColor = vec4( tex.rgb * min( light, vec3( 2.0 ) ), tex.a * gl_Color.a );
}
