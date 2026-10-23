// KenRadiant live light preview: texture * ( ambient + shadowed sun + point lights ), lights in eye space
uniform sampler2D u_tex;
uniform int u_count;
uniform vec3 u_pos[24];
uniform vec3 u_col[24];
uniform float u_rad[24];
uniform vec3 u_sun_dir;
uniform vec3 u_sun_col;
uniform vec3 u_ambient;    // worldspawn _ambient
uniform vec3 u_sky;        // q3map_skyLight: light from the whole sky, more on surfaces facing up
uniform float u_minlight;  // worldspawn _minlight
uniform sampler2DShadow u_shadow;
uniform mat4 u_eye2shadow;
uniform int u_has_shadow;
uniform float u_texel;
uniform float u_bias_unit; // one shadow texel in depth units
uniform float u_soft;      // PCF spread in texels (q3map_sunExt deviance)

// point light shadows: depth cube maps of the first lights of the list
uniform samplerCube u_cube0;
uniform samplerCube u_cube1;
uniform samplerCube u_cube2;
uniform samplerCube u_cube3;
uniform samplerCube u_cube4;
uniform samplerCube u_cube5;
uniform samplerCube u_cube6;
uniform samplerCube u_cube7;
uniform vec4 u_cube_far;   // lights 0-3, 0 = no shadow for that light
uniform vec4 u_cube_far2;  // lights 4-7
uniform float u_cube_near;
uniform mat3 u_eye2world;

// compile settings preview (q3map2 -gamma -compensate -saturation, bounce approximation)
uniform float u_gamma;
uniform float u_compensate;
uniform float u_saturation;
uniform vec3 u_bounce;
uniform int u_mode; // 0 = unlit (fixed-function look), 1 = lit preview, 2 = fullbright glow

varying vec3 v_pos;
varying vec3 v_normal;

float cubeDepthToDistance( float d, float n, float f ){
	return 2.0 * f * n / ( ( f + n ) - ( 2.0 * d - 1.0 ) * ( f - n ) );
}

// 1 = lit, 0 = shadowed; L = world-space vector light -> fragment
float cubeShadow( samplerCube cube, vec3 L, float f, float ndl ){
	vec3 a = abs( L );
	float ma = max( a.x, max( a.y, a.z ) );
	if( ma >= f )
		return 1.0;
	float bias = 2.0 + ma * ( 0.01 + 0.04 * ( 1.0 - ndl ) );
	// 4 taps around the direction for a slightly soft edge
	vec3 t1 = normalize( cross( L, abs( L.z ) < 0.9 * length( L ) ? vec3( 0.0, 0.0, 1.0 ) : vec3( 1.0, 0.0, 0.0 ) ) );
	vec3 t2 = normalize( cross( L, t1 ) );
	float r = ma * 0.006;
	float lit = 0.0;
	lit += step( ma - bias, cubeDepthToDistance( textureCube( cube, L + t1 * r ).r, u_cube_near, f ) );
	lit += step( ma - bias, cubeDepthToDistance( textureCube( cube, L - t1 * r ).r, u_cube_near, f ) );
	lit += step( ma - bias, cubeDepthToDistance( textureCube( cube, L + t2 * r ).r, u_cube_near, f ) );
	lit += step( ma - bias, cubeDepthToDistance( textureCube( cube, L - t2 * r ).r, u_cube_near, f ) );
	return lit * 0.25;
}

void main()
{
	vec4 tex = texture2D( u_tex, gl_TexCoord[0].st );
	if( u_mode == 0 ){
		gl_FragColor = tex * gl_Color;
		return;
	}
	if( u_mode == 2 ){
		gl_FragColor = vec4( tex.rgb, tex.a * gl_Color.a );
		return;
	}
	vec3 n = normalize( v_normal );
	if( !gl_FrontFacing )
		n = -n;

	float ndl = max( dot( n, u_sun_dir ), 0.0 );
	float sun = ndl;
	if( u_has_shadow != 0 && ndl > 0.0 ){
		vec4 sc = u_eye2shadow * vec4( v_pos, 1.0 );
		if( sc.x > 0.0 && sc.x < 1.0 && sc.y > 0.0 && sc.y < 1.0 && sc.z < 1.0 ){
			// slope-scaled bias, 5x5 PCF; the spread grows with the sun's deviance (q3map_sunExt)
			float bias = u_bias_unit * ( 1.0 + 2.0 * min( sqrt( 1.0 - ndl * ndl ) / max( ndl, 0.05 ), 8.0 ) ) * max( 1.0, u_soft );
			float lit = 0.0;
			for( int y = -2; y <= 2; ++y )
				for( int x = -2; x <= 2; ++x )
					lit += shadow2D( u_shadow, vec3( sc.xy + vec2( float( x ), float( y ) ) * ( u_texel * 0.5 * max( 1.0, u_soft ) ), sc.z - bias ) ).r;
			sun *= lit / 25.0;
		}
	}
	float up = ( u_eye2world * n ).z;
	vec3 light = u_ambient + u_sky * ( 0.55 + 0.45 * up ) + u_sun_col * sun + u_bounce;
	for( int i = 0; i < 24; ++i ){
		if( i >= u_count )
			break;
		vec3 d = u_pos[i] - v_pos;
		float dist = length( d );
		// q3map2 point light, in lightmap units (1 = 255): inverse square ( light * 7500 / dist² ),
		// or linear ( spawnflags 1: light * 7500 / 8000 - dist ), dist never below 16
		float lndl = max( dot( n, d / max( dist, 0.001 ) ), 0.0 );
		float att = u_rad[i] > 0.0 ? u_rad[i] / max( dist * dist, 256.0 ) : max( -u_rad[i] - dist, 0.0 ) / 255.0;
		float c = att * lndl;
		if( c > 0.0 && i < 8 ){
			vec3 L = u_eye2world * ( -d );
			if( i == 0 && u_cube_far.x > 0.0 ) c *= cubeShadow( u_cube0, L, u_cube_far.x, lndl );
			else if( i == 1 && u_cube_far.y > 0.0 ) c *= cubeShadow( u_cube1, L, u_cube_far.y, lndl );
			else if( i == 2 && u_cube_far.z > 0.0 ) c *= cubeShadow( u_cube2, L, u_cube_far.z, lndl );
			else if( i == 3 && u_cube_far.w > 0.0 ) c *= cubeShadow( u_cube3, L, u_cube_far.w, lndl );
			else if( i == 4 && u_cube_far2.x > 0.0 ) c *= cubeShadow( u_cube4, L, u_cube_far2.x, lndl );
			else if( i == 5 && u_cube_far2.y > 0.0 ) c *= cubeShadow( u_cube5, L, u_cube_far2.y, lndl );
			else if( i == 6 && u_cube_far2.z > 0.0 ) c *= cubeShadow( u_cube6, L, u_cube_far2.z, lndl );
			else if( i == 7 && u_cube_far2.w > 0.0 ) c *= cubeShadow( u_cube7, L, u_cube_far2.w, lndl );
		}
		light += u_col[i] * c;
	}
	light = max( light, vec3( u_minlight ) );
	// what q3map2 does to the lightmap: compensate, gamma, saturation
	light = max( light / u_compensate, vec3( 0.0 ) );
	light = pow( light, vec3( 1.0 / u_gamma ) );
	float grey = dot( light, vec3( 0.299, 0.587, 0.114 ) );
	light = max( mix( vec3( grey ), light, u_saturation ), vec3( 0.0 ) );
	// the engine doubles the lightmap (overbright shift) and clamps it keeping the colour: a fully lit surface
	// shows its texture as it is, never washed out to white
	vec3 shown = light * 2.0;
	float peak = max( shown.r, max( shown.g, shown.b ) );
	if( peak > 1.0 )
		shown /= peak;
	gl_FragColor = vec4( tex.rgb * shown, tex.a * gl_Color.a );
}
