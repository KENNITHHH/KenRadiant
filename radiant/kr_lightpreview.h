#pragma once
// KenRadiant: live light preview in the 3D view (lights + sky shader sun, no shadows)
#include "math/vector.h"
#include "math/matrix.h"
#include <vector>

struct KrPreviewLight
{
	Vector3 origin;
	Vector3 colour;
	float radius;
};

extern bool g_krLightPreview;          // user toggle (Ctrl+L / toolbar)
extern bool g_krLightPreviewDrawing;   // true while the 3D view renders (the model/texture browsers stay unlit)
extern Matrix4 g_krLightPreviewView;   // camera world->eye matrix of that render
extern std::vector<KrPreviewLight> g_krLightPreviewLights;
extern Vector3 g_krSunDirection;       // towards the sun, world space
extern Vector3 g_krSunColour;          // 0 = no sun
extern Vector3 g_krAmbient;
constexpr std::size_t c_krMaxPreviewLights = 24;

// sun shadow map (rendered by the 3D view, read by the light preview program)
extern bool g_krShadows;               // user toggle (on by default with the preview)
extern unsigned int g_krShadowTex;     // GL depth texture, 0 = none
extern Matrix4 g_krEye2Shadow;         // camera eye space -> shadow map [0,1]
extern float g_krShadowTexel;          // 1 / shadow map size
extern float g_krShadowBiasUnit;       // one shadow texel in shadow depth units
class Shader;
bool Shader_castsShadow( Shader* shader ); // textured, opaque, not sky

// animated shaders (tcMod scroll / rotate)
extern bool g_krAnimateShaders;
extern float g_krShaderTime;       // seconds
extern bool g_krAnimatedDrawn;     // set by the renderer when an animated surface was drawn
