uniform samplerCube skybox;
// KenRadiant: with the map's fog on, the game shows the sky as the flat fog colour
uniform int u_fog_on;
uniform vec3 u_fog_col;

void main()
{
	if( u_fog_on != 0 ){
		gl_FragColor = vec4( u_fog_col, 1.0 );
		return;
	}
	//doing rotation/flip to fix skybox orientation
	gl_FragColor = textureCube( skybox, vec3( -gl_TexCoord[0].y, gl_TexCoord[0].z, gl_TexCoord[0].x ) );
}
