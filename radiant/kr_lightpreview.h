#pragma once
// KenRadiant: live light preview in the 3D view (lights + sky shader sun, shadows)
#include "math/vector.h"
#include "math/matrix.h"
#include <vector>
#include "string/string.h"

struct KrPreviewLight
{
	Vector3 origin;
	Vector3 colour;    // normalised _color
	float radius;      // reach (where it drops below 1/255), for shadows and picking the nearest lights
	float falloff;     // shader: > 0 inverse square ( light * 7500 / 255 ), < 0 linear ( -light * 7500 / 8000 )
};

extern bool g_krLightPreview;          // user toggle (Ctrl+L / toolbar)
extern bool g_krLightPreviewDrawing;   // true while the 3D view renders (the model/texture browsers stay unlit)
extern Matrix4 g_krLightPreviewView;   // camera world->eye matrix of that render
extern std::vector<KrPreviewLight> g_krLightPreviewLights;
extern Vector3 g_krSunDirection;       // towards the sun, world space
extern Vector3 g_krSunColour;          // 0 = no sun
extern Vector3 g_krAmbient;            // worldspawn _ambient
extern Vector3 g_krSkyAmbient;         // q3map_skyLight share (hemisphere)
extern float g_krMinLight;             // worldspawn _minlight
constexpr std::size_t c_krMaxPreviewLights = 48;

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

/// the sun the preview uses: read from the map's sky shader, or overridden live by the Sun Calibrator
struct KrSun
{
	bool valid = false;
	CopiedString shader;     // sky shader it came from ("" = none in the map)
	Vector3 colour{ 1, 1, 1 };
	float intensity = 0, degrees = 0, elevation = 45, skyLight = 0, skyIterations = 3;
	bool ext = false;        // q3map_sunExt
	float deviance = 0, samples = 16;
	bool overridden = false; // calibrator values in use
};
extern KrSun g_krSun;
void KrSun_changed();          // sun re-read from the map: calibrator window refresh
void KrSun_rescan();           // forget calibrator values, read the sky shader again
void KrLightPreview_enable();  // turns the live preview on (if off)

/// compile settings previewed on the lighting (q3map2 -gamma, -compensate, -saturation, -bounce approximation)
struct KrCompilePreview
{
	bool enabled = false;
	float gamma = 1, compensate = 1, saturation = 1;
	int bounce = 0;
};
extern KrCompilePreview g_krCompile;
extern Vector3 g_krBounceFill;   // approximate indirect light added by the bounce slider

// point light shadows (cube maps of the nearest lights)
constexpr std::size_t c_krMaxCubeShadows = 8;
extern int g_krShadowedLights;   // how many of the nearest lights get a shadow cube (0-8, setting)
extern int g_krPreviewLightCount; // how many lights light the preview (1-24, setting)
extern bool g_krLightShadows;
extern unsigned int g_krCubeTex[c_krMaxCubeShadows]; // 0 = that light casts no shadow
extern float g_krCubeNear, g_krCubeFar[c_krMaxCubeShadows];
extern Matrix4 g_krEye2World;    // camera eye -> world (for cube map lookups)
extern float g_krSunSoftness;    // shadow PCF spread in texels (q3map_sunExt deviance)

// shader effect toggles (View -> Shader Effects)
extern bool g_krDeformVertexes;
extern bool g_krPolygonOffset;
extern bool g_krGlow;

// sky light occlusion: 9 directional depth maps in one atlas (rendered by the 3D view)
extern unsigned int g_krSkyShadowTex;  // 0 = none (sky light unoccluded)
extern Matrix4 g_krEye2Sky[9];
extern Vector3 g_krSkyDirs[9];         // world, towards the sky
extern float g_krSkyBias;
