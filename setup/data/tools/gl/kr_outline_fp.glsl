// KenRadiant selection outline (Unreal style): pixels just outside the selected silhouette
uniform sampler2D u_mask;
uniform vec2 u_texel;
uniform vec4 u_colour;

void main()
{
	vec2 uv = gl_TexCoord[0].st;
	if( texture2D( u_mask, uv ).r > 0.5 )
		discard;
	float edge = 0.0;
	for( int y = -2; y <= 2; ++y )
		for( int x = -2; x <= 2; ++x )
			edge = max( edge, texture2D( u_mask, uv + vec2( float( x ), float( y ) ) * u_texel ).r );
	if( edge < 0.5 )
		discard;
	gl_FragColor = u_colour;
}
