#pragma once
// KenRadiant: q3map2 index-map terrain (func_group with "alphamap" / "_indexmap", "layers", "shader")
// - the 3D view blends the layer shaders (textures/<shader>_0 ... _N) from the .pcx index map, like q3map2 + the game
// - Terrain Paint tool: paint layers into the index map in the 3D view, saved with the map

#include "math/vector.h"
#include "math/matrix.h"

class Shader;
class View;
class Entity;
namespace scene { class Node; }

/// the render state that replaces q3map_terrain faces inside this entity (nullptr: not a terrain entity)
Shader* KrTerrain_state( scene::Node& node, const Entity& entity );

/// GL data of a terrain render state, used by the 3D view program
struct KrTerrainGL
{
	unsigned int indexTex = 0;       // RGBA: channel k = 1 where the index map says layer k (layers 0-3)
	unsigned int indexTex2 = 0;      // layers 4-7
	unsigned int layerTex[8] = {};
	float layerScale[8][2] = {};
	int layers = 0;
	Vector3 mins{ 0, 0, 0 }, maxs{ 1, 1, 1 };
};
const KrTerrainGL* KrTerrain_gl( int id );

// paint tool (3D view)
bool KrTerrainPaint_active();
/// mouse in the 3D view while the tool is on: phase 0 = press, 1 = move (button held), 2 = release, 3 = hover
void KrTerrainPaint_mouse( int phase, const View& view, float deviceX, float deviceY, float epsX, float epsY );
void KrTerrainPaint_drawOverlay( const Matrix4& world2screen, int width, int height );
void KrTerrain_shutdown(); // called once when the editor closes
void KrTerrain_saveAll(); // write changed index maps (called when the map is saved)
