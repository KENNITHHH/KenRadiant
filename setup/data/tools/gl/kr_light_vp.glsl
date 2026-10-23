// KenRadiant live light preview - eye-space position and normal for the fragment shader
varying vec3 v_pos;
varying vec3 v_normal;

void main()
{
	v_pos = vec3( gl_ModelViewMatrix * gl_Vertex );
	v_normal = gl_NormalMatrix * gl_Normal;
	gl_TexCoord[0] = gl_TextureMatrix[0] * gl_MultiTexCoord0; // tcMod scroll/rotate
	gl_FrontColor = gl_Color;
	gl_Position = ftransform();
}
