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
