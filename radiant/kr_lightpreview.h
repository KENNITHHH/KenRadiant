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
	float falloff;     // shader: > 0 inverse square ( light * 7500 / 255 ), < 0 linear ( -light * 7500 / 8000 / fade, colour * fade )
	Vector3 spotDir{ 0, 0, 0 };   // spotlight (light with a target): towards the target
	float spotRadiusByDist = 0;   // q3map2 ( radius + 16 ) / distance to the target; 0 = point light
	bool noAngle = false;         // spawnflags 1 (linear) / 2: no angle attenuation
	float angleScale = 0;         // _anglescale
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
constexpr std::size_t c_krMaxPreviewLights = 2048;
extern int g_krMaxPreviewLightsGPU; // = c_krMaxPreviewLights (lights live in a float texture)
// lights in a float texture, found per fragment through a world grid around the camera (clustered lighting)
constexpr int c_krLightTexW = 1024;          // texels per row
constexpr int c_krLightTexels = 4;           // per light
constexpr int c_krGridX = 20, c_krGridY = 20, c_krGridZ = 10, c_krCellSize = 256, c_krCellSlots = 48;
static_assert( ( 2048 * 4 + 20 * 20 * 10 * 48 / 4 + 1023 ) / 1024 == 55, "kr_light_fp.glsl KR_TH" );
constexpr int c_krLightTexRows = ( int( c_krMaxPreviewLights ) * c_krLightTexels + c_krGridX * c_krGridY * c_krGridZ * c_krCellSlots / 4 + c_krLightTexW - 1 ) / c_krLightTexW;
extern unsigned int g_krLightTex;            // 0 = none
extern Vector3 g_krGridMin;
extern int g_krLightUnit;                    // texture unit of g_krLightTex (16, or 15 on cards with 16 units)
extern std::size_t g_krMaxCubes;             // 8, or 7 when unit 15 holds the lights

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
extern Vector3 g_krBounceFill;
// how the game shows the lightmap: overbright shift (r_mapOverBrightBits - r_overBrightBits, SOF2 fullscreen = 1, windowed / no hardware gamma = 0) and r_gamma
extern int g_krEngineShift;
extern float g_krEngineGamma;   // approximate indirect light added by the bounce slider

// point light shadows (cube maps of the nearest lights)
constexpr std::size_t c_krMaxCubeShadows = 8;
extern int g_krShadowedLights;   // how many of the nearest lights get a shadow cube (0-8, setting)
extern int g_krPreviewLightCount; // how many lights light the preview (1-1024, setting)
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
constexpr int c_krSkyDirs = 16, c_krSkyGrid = 4; // sky light directions, tiles per atlas row
extern Matrix4 g_krEye2Sky[c_krSkyDirs];
extern Vector3 g_krSkyDirs[c_krSkyDirs];         // world, towards the sky
extern float g_krSkyBias;
