// KenRadiant live light preview: texture * ( ambient + shadowed sun + point lights ), lights in eye space
uniform sampler2D u_tex;
uniform int u_count;
uniform vec3 u_pos[24];
uniform vec3 u_col[24];
uniform float u_rad[24];
uniform vec3 u_sun_dir;
uniform vec3 u_sun_col;
uniform vec3 u_ambient;
uniform sampler2DShadow u_shadow;
uniform mat4 u_eye2shadow;
uniform int u_has_shadow;
uniform float u_texel;
uniform float u_bias_unit; // one shadow texel in depth units

varying vec3 v_pos;
varying vec3 v_normal;

void main()
{
	vec4 tex = texture2D( u_tex, gl_TexCoord[0].st );
	vec3 n = normalize( v_normal );
	if( !gl_FrontFacing )
		n = -n;

	float ndl = max( dot( n, u_sun_dir ), 0.0 );
	float sun = ndl;
	if( u_has_shadow != 0 && ndl > 0.0 ){
		vec4 sc = u_eye2shadow * vec4( v_pos, 1.0 );
		if( sc.x > 0.0 && sc.x < 1.0 && sc.y > 0.0 && sc.y < 1.0 && sc.z < 1.0 ){
			// slope-scaled bias, 3x3 PCF for soft-ish edges
			float bias = u_bias_unit * ( 1.0 + 2.0 * min( sqrt( 1.0 - ndl * ndl ) / max( ndl, 0.05 ), 8.0 ) );
			float lit = 0.0;
			for( int y = -1; y <= 1; ++y )
				for( int x = -1; x <= 1; ++x )
					lit += shadow2D( u_shadow, vec3( sc.xy + vec2( float( x ), float( y ) ) * u_texel, sc.z - bias ) ).r;
			sun *= lit / 9.0;
		}
	}
	vec3 light = u_ambient + u_sun_col * sun;
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
