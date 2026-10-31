// KenRadiant live light preview: texture * ( ambient + shadowed sun + point lights ), lights in eye space
uniform sampler2D u_tex;
// lights (up to 8192) in a float texture: 4 texels per light ( world position, falloff ), ( colour, reach² ),
// ( spot direction, spot radius by distance ), ( no angle attenuation, _anglescale ); then a grid of
// 256-unit cells around the camera, each with the 48 strongest lights that reach it (-1 = end of the list)
uniform sampler2D u_lights;
uniform int u_has_lights;
uniform vec3 u_grid_min;
uniform vec3 u_cam_world; // camera position: world = u_eye2world * eye + u_cam_world (models are drawn in their own space)
vec3 g_wp;
#define KR_TW 1024.0
// KR_LIGHTS_TEXELS, KR_TH, KR_GRID, KR_CELL, KR_CELL_TEXELS: defined by the editor (kr_lightpreview.h)
uniform vec3 u_sun_dir;
uniform vec3 u_sun_col;
uniform vec3 u_ambient;    // worldspawn _ambient
uniform vec3 u_sky;        // q3map_skyLight: light from the whole sky, more on surfaces facing up
uniform vec3 u_minlight;   // worldspawn _minlight, in its _color
uniform vec3 u_flood;      // worldspawn _floodlight colour * intensity (q3map2 fill light), weighed by the sky seen
// sky light occlusion: 16 directional depth maps (zenith + 2 rings) in a 4 x 4 atlas
uniform sampler2DShadow u_skyshadow;
uniform int u_has_sky;
#define KR_SKY 16
#define KR_SKY_GRID 4
uniform mat4 u_eye2sky[KR_SKY];
uniform vec3 u_sky_dir[KR_SKY];   // world space, towards the sky
uniform float u_sky_bias;
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
uniform float u_overbright; // 2 ^ ( r_mapOverBrightBits - r_overBrightBits ): 2 in SOF2 fullscreen
uniform float u_rgamma;     // r_gamma
uniform float u_brightness; // q3map2 -brightness / -vertexscale of the LIGHT stage
uniform float u_vscale;
uniform int u_vertexlit;   // rgbGen vertex stage: vertex light (-vertexscale), used by the engine as stored
uniform float u_lmscale; // game look: 2 when the shader multiplies its lightmap in twice (GL_DST_COLOR GL_SRC_COLOR)
uniform int u_debug;   // Lighting Probe: 1 = output what q3map2 would store (lightmap / vertex light bytes), 0 = normal
uniform int u_dbgcut;  // Lighting Probe: blended stage, leave out its see-through texels
uniform vec4 u_stage;   // game look: the stage's rgbGen / alphaGen
uniform int u_mode; // 0 = unlit (fixed-function look), 1 = lit preview, 2 = fullbright glow

varying vec3 v_pos;
varying vec3 v_normal;
varying vec3 v_world;

// KenRadiant: the map's fog, as the engine draws it (fogparms). 0 = off, 1 = fade into it, 2 = flat fog colour (sky)
uniform int u_fog_on;
uniform vec3 u_fog_col;
uniform float u_fog_depth; // fogparms' distance: completely fogged this far away

/* the engine's fog curve: alpha = sqrt( distance / depthForOpaque ), clamped (tr_shade fogTable = pow( x, 0.5 ) ).
   The distance is the one the engine uses in RB_CalcFogTexCoords: along the view axis (dot( vertex - eye, forward )),
   which is -z in eye space - not the distance to the eye, or the screen edges would fog far too early. */
vec3 krFog( vec3 colour ){
	if( u_fog_on == 0 )
		return colour;
	return mix( colour, u_fog_col, sqrt( clamp( -v_pos.z / u_fog_depth, 0.0, 1.0 ) ) );
}

// q3map2 index-map terrain: layer weights from the index map, layer textures with their tcMod scale
uniform int u_terrain;      // number of layers, 0 = not terrain
uniform sampler2D u_tidx;   // RGBA = layers 0-3
uniform sampler2D u_tl0;
uniform sampler2D u_tl1;
uniform sampler2D u_tl2;
uniform sampler2D u_tl3;
uniform vec2 u_tmins;
uniform vec2 u_tsize;
uniform vec2 u_tscale[4];

vec4 terrainColour(){
	vec2 uv = vec2( ( v_world.x - u_tmins.x ) / u_tsize.x, 1.0 - ( v_world.y - u_tmins.y ) / u_tsize.y );
	vec4 w = texture2D( u_tidx, uv );
	vec2 st = gl_TexCoord[0].st;
	vec4 c = texture2D( u_tl0, st * u_tscale[0] ) * w.r;
	if( u_terrain > 1 ) c += texture2D( u_tl1, st * u_tscale[1] ) * w.g;
	if( u_terrain > 2 ) c += texture2D( u_tl2, st * u_tscale[2] ) * w.b;
	if( u_terrain > 3 ) c += texture2D( u_tl3, st * u_tscale[3] ) * w.a;
	return vec4( c.rgb / max( w.r + w.g + w.b + w.a, 0.001 ), 1.0 );
}

vec3 g_offw = vec3( 0.0 ); // terrain: world-space offset off the face itself (see main)
vec4 lightTexel( float t ){
	float y = floor( t / KR_TW );
	return texture2D( u_lights, vec2( ( t - y * KR_TW + 0.5 ) / KR_TW, ( y + 0.5 ) / KR_TH ) );
}

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

// q3map2 point / spot light at this fragment, in lightmap units (1 = 255)
vec3 pointLight( float id, vec3 nw ){
	vec3 wp = g_wp;
	vec4 a = lightTexel( id * 4.0 );
	vec4 b = lightTexel( id * 4.0 + 1.0 );
	vec3 d = a.xyz - wp;
	float dd = dot( d, d );
	if( dd > b.w )
		return vec3( 0.0 );
	vec4 sp = lightTexel( id * 4.0 + 2.0 );
	vec4 fl = lightTexel( id * 4.0 + 3.0 );
	float dist = sqrt( dd );
	float ndl = max( dot( nw, d / max( dist, 0.001 ) ), 0.0 );
	float angle = fl.x > 0.5 ? 1.0 : ndl;
	if( fl.y != 0.0 )
		angle = min( angle / fl.y, 1.0 );
	// inverse square ( light * 7500 / dist² ) or linear ( light * 7500 / 8000 - dist * fade ), dist never below 16
	float c = angle * ( a.w > 0.0 ? a.w / max( dd, 256.0 ) : max( -a.w - max( dist, 16.0 ), 0.0 ) / 255.0 );
	if( sp.w > 0.0 ){ // spotlight: cone towards the target, 32 units soft edge
		float distByNormal = dot( -d, sp.xyz );
		if( distByNormal < 0.0 )
			return vec3( 0.0 );
		float radiusAtDist = sp.w * distByNormal;
		float sampleRadius = length( wp - ( a.xyz + sp.xyz * distByNormal ) );
		if( sampleRadius >= radiusAtDist )
			return vec3( 0.0 );
		if( sampleRadius > radiusAtDist - 32.0 )
			c *= ( radiusAtDist - sampleRadius ) / 32.0;
	}
	if( c > 0.0 && id < 7.5 ){
		vec3 L = -d + nw * ( 3.0 + dist * ( 0.02 + 0.04 * ( 1.0 - ndl ) ) ) + g_offw * ( 1.0 + dist * 0.01 ); // (normal offset against acne, more at grazing angles; terrain: off the face too)
		int i = int( id + 0.5 );
		if( i == 0 && u_cube_far.x > 0.0 ) c *= cubeShadow( u_cube0, L, u_cube_far.x, ndl );
		else if( i == 1 && u_cube_far.y > 0.0 ) c *= cubeShadow( u_cube1, L, u_cube_far.y, ndl );
		else if( i == 2 && u_cube_far.z > 0.0 ) c *= cubeShadow( u_cube2, L, u_cube_far.z, ndl );
		else if( i == 3 && u_cube_far.w > 0.0 ) c *= cubeShadow( u_cube3, L, u_cube_far.w, ndl );
		else if( i == 4 && u_cube_far2.x > 0.0 ) c *= cubeShadow( u_cube4, L, u_cube_far2.x, ndl );
		else if( i == 5 && u_cube_far2.y > 0.0 ) c *= cubeShadow( u_cube5, L, u_cube_far2.y, ndl );
		else if( i == 6 && u_cube_far2.z > 0.0 ) c *= cubeShadow( u_cube6, L, u_cube_far2.z, ndl );
		else if( i == 7 && u_cube_far2.w > 0.0 ) c *= cubeShadow( u_cube7, L, u_cube_far2.w, ndl );
	}
	return b.rgb * c;
}

void main()
{
	if( u_fog_on == 2 ){ // the sky: the game shows the flat fog colour where it would be
		gl_FragColor = vec4( u_fog_col, 1.0 );
		return;
	}
	vec4 tex = u_terrain > 0 ? terrainColour() : texture2D( u_tex, gl_TexCoord[0].st );
	if( u_debug != 0 ){
		if( u_dbgcut != 0 && tex.a * gl_Color.a * u_stage.a < 0.5 )
			discard;
		if( u_mode != 1 ){
			gl_FragColor = vec4( 0.0, 0.0, 0.0, 0.0 ); // unlit / glowing: no stored light (alpha 0 marks it)
			return;
		}
	}
	if( u_mode == 0 ){
		gl_FragColor = tex * gl_Color * u_stage;
		gl_FragColor.rgb = krFog( gl_FragColor.rgb );
		return;
	}
	if( u_mode == 2 ){
		gl_FragColor = vec4( krFog( tex.rgb * u_stage.rgb ), tex.a * gl_Color.a * u_stage.a );
		return;
	}
	vec3 n = normalize( v_normal );
	// the side the camera sees (two-sided "cull none" surfaces: the editor draws those with the other front-face
	// winding, so gl_FrontFacing can't be trusted)
	if( dot( n, v_pos ) > 0.0 )
		n = -n;
	// terrain: smoothed normals can point to the light while the face itself turns away (ridges, far slopes) -
	// look the shadows up off the face too, or the face shadows itself (dark rims)
	vec3 g_off = vec3( 0.0 );
	if( u_terrain > 0 ){
		vec3 cr = cross( dFdx( v_pos ), dFdy( v_pos ) );
		float len = length( cr );
		vec3 gn = len > 1e-6 ? cr / len : n; // (tiny / far pixels: no derivative to trust - normalize( 0 ) would be NaN, black)
		if( dot( gn, n ) < 0.0 )
			gn = -gn;
		g_off = gn * ( 4.0 + 0.004 * length( v_pos ) );
		g_offw = u_eye2world * gn * 4.0;
	}

	float ndl = max( dot( n, u_sun_dir ), 0.0 );
	float sun = ndl;
	if( u_has_shadow != 0 && ndl > 0.0 ){
		// normal offset: look the shadow up a little off the surface, no self-shadow stripes (acne) on walls
		vec4 sc = u_eye2shadow * vec4( v_pos + n * ( 2.0 + 4.0 * ( 1.0 - ndl ) ) + g_off, 1.0 );
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
	vec3 nw = u_eye2world * n;
	// q3map2 sky light: light from every direction of the sky the surface can see (cosine weighted)
	float skyVis = 0.0, skyAll = 0.0;
	for( int i = 0; i < KR_SKY; ++i )
		skyAll += max( u_sky_dir[i].z, 0.0 ); // what an open floor gets
	float tileSize = 1.0 / float( KR_SKY_GRID );
	for( int i = 0; i < KR_SKY; ++i ){
		float c = max( dot( nw, u_sky_dir[i] ), 0.0 );
		if( c <= 0.0 )
			continue;
		float vis = 1.0;
		if( u_has_sky != 0 ){
			vec4 sc = u_eye2sky[i] * vec4( v_pos + n * ( 6.0 + 12.0 * ( 1.0 - c ) ) + g_off * 2.0, 1.0 );
			vec2 t0 = vec2( float( i - KR_SKY_GRID * ( i / KR_SKY_GRID ) ), float( i / KR_SKY_GRID ) ) * tileSize; // this direction's tile of the atlas
			// a wide soft filter: q3map2's sky light comes from the whole dome, not from a few sharp directions
			float r = 3.0 * tileSize / 1024.0;
			if( sc.x > t0.x + r && sc.x < t0.x + tileSize - r && sc.y > t0.y + r && sc.y < t0.y + tileSize - r && sc.z < 1.0 ){
				float bias = u_sky_bias * ( 2.0 + 3.0 * min( sqrt( 1.0 - c * c ) / max( c, 0.05 ), 8.0 ) );
				float z = sc.z - bias;
				vis = 0.2 * ( shadow2D( u_skyshadow, vec3( sc.xy, z ) ).r
				            + shadow2D( u_skyshadow, vec3( sc.xy + vec2( r, 0.3 * r ), z ) ).r
				            + shadow2D( u_skyshadow, vec3( sc.xy + vec2( -0.3 * r, r ), z ) ).r
				            + shadow2D( u_skyshadow, vec3( sc.xy + vec2( -r, -0.3 * r ), z ) ).r
				            + shadow2D( u_skyshadow, vec3( sc.xy + vec2( 0.3 * r, -r ), z ) ).r );
			}
		}
		skyVis += c * vis;
	}
	vec3 light = u_ambient + u_sky * ( skyVis / max( skyAll, 0.001 ) ) + u_sun_col * sun + u_bounce;
	// q3map2 _floodlight: light from the directions open within its distance - the share of sky this spot sees
	// stands in for that (right outdoors, a bit dark under roofs where q3map2 also counts far walls as open)
	light += u_flood * clamp( skyVis / max( skyAll, 0.001 ), 0.0, 1.0 );
	if( u_has_lights != 0 ){
		g_wp = u_eye2world * v_pos + u_cam_world;
		vec3 cellf = clamp( floor( ( g_wp - u_grid_min ) / KR_CELL ), vec3( 0.0 ), KR_GRID - vec3( 1.0 ) );
		float base = KR_LIGHTS_TEXELS + ( cellf.x + KR_GRID.x * ( cellf.y + KR_GRID.y * cellf.z ) ) * float( KR_CELL_TEXELS );
		bool done = false;
		for( int k = 0; k < KR_CELL_TEXELS && !done; ++k ){
			vec4 ids = lightTexel( base + float( k ) );
			for( int j = 0; j < 4; ++j ){
				float id = j == 0 ? ids.x : j == 1 ? ids.y : j == 2 ? ids.z : ids.w;
				if( id < 0.0 ){
					done = true;
					break;
				}
				light += pointLight( id, nw );
			}
		}
	}
	light = max( light, u_minlight );
	// what q3map2 does (ColorToBytes): vertex light is scaled by -vertexscale before the gamma, a lightmap by
	// -brightness after it (it goes in as lmscale), then both by 1 / -compensate
	if( u_vertexlit != 0 )
		light *= u_vscale;
	light = pow( max( light, vec3( 0.0 ) ), vec3( 1.0 / u_gamma ) );
	if( u_vertexlit == 0 )
		light *= u_brightness;
	light /= u_compensate;
	float grey = dot( light, vec3( 0.299, 0.587, 0.114 ) );
	light = max( mix( vec3( grey ), light, u_saturation ), vec3( 0.0 ) );
	// the engine doubles the lightmap (overbright shift) and clamps it keeping the colour: a fully lit surface
	// shows its texture as it is, never washed out to white
	if( u_debug != 0 ){ // q3map2 clamps keeping the colour and stores bytes
		float top = max( light.r, max( light.g, light.b ) );
		gl_FragColor = vec4( top > 1.0 ? light / top : light, 1.0 );
		return;
	}
	vec3 shown = light * u_overbright;
	float peak = max( shown.r, max( shown.g, shown.b ) );
	if( peak > 1.0 )
		shown /= peak;
	shown *= u_lmscale;
	gl_FragColor = vec4( krFog( pow( tex.rgb * shown, vec3( 1.0 / u_rgamma ) ) * u_stage.rgb ), tex.a * gl_Color.a * u_stage.a );
}
