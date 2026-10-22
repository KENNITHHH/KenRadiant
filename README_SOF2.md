# q3map2 — SOF2 branch

A q3map2 build focused on **Soldier of Fortune II: Double Helix**, based on
[Garux/netradiant-custom](https://github.com/Garux/netradiant-custom) (64-bit, multithreaded, the most advanced q3map2 fork).

## What's different from netradiant-custom

### Decompile (`-convert -format map`)
Standard (Q3 / SOF2Radiant) brush format output is rewritten to recover texture alignment exactly.

| kam6.bsp vs Raven's kam6.map | visible faces identical | hidden faces identical | hidden: same rotate+scale |
|---|---|---|---|
| netradiant-custom | 97.3% | 7.4% | 7.4% |
| this branch | **99.4%** | **54.1%** | **85.5%** |
| Raven's .map vs its own .bsp (ceiling) | 99.5% | – | – |

How:
- **Direct side→surface links.** RBSP brush sides store the drawsurface they produced. Used directly instead of searching for a matching triangle.
- **Least-squares fit** over all of the face's vertices (restricted to the side's own polygon, so it's also safe for q3map2 meta-merged surfaces), instead of 3 vertices.
- **Exact inverse of the Q3 projection**, choosing the representation editors write (positive X scale, symmetric shift) and snapping to "hand-made" values (integer shifts, 0.125/0.25/0.5 scales, whole-degree rotations) only when the snapped texdef still reproduces every UV.
- **Hidden faces** (no surface in the BSP) inherit rotate/scale/shift from the visible faces of the same brush. Fully hidden brushes get the map-wide most common texdef (SOF2: `0 0 0 0.125 0.125`), not `0.25`.
- **Integer plane points** whenever that reproduces the plane exactly; compact numbers (`128 0 0 0.125 0.125`, not `128.00000000 …`).
- The fallback triangle search never matches against misc_model soups or faces already owned by another side.
- Prints a summary: how many sides were solved, how many are exact, worst error, hidden-side handling.
- `-legacydecompile` restores the old netradiant behaviour; `-format map_bp` / `map_220` are unchanged.

### SOF2 compile changes
- Decompiled SOF2 maps write origin brushes with `tools/_origin` (netradiant wrote JK2's `system/origin`, which SOF2 has no shader for), so doors and rotating entities keep their pivot when recompiled.
- `-game sof2` now matches Raven's sof2map defaults: `misc_model` entities are kept in the BSP (so `-convert` restores them 1:1), and baked models use the `_bsp` shader variants, **but only when that shader is actually defined**, so custom/converted props never lose their texture. Opt out with `-nokeepmodels` / `-norename`.
- `q3map_lightColor ( r g b )` (Raven syntax) is parsed. Other Raven `q3map_*` keywords (`q3map_hint`, `q3map_areaportal`, `q3map_origin`, `q3map_nolightmap`, `q3map_alphashadow` …) already worked through ydnar's generic `q3map_<surfaceparm>` fallback.

### Lightmap packing for MAX_SHADERS (light stage)
SOF2/JK2/JA create one renderer shader per (shader, lightmap page) pair and stop at `MAX_SHADERS` (2048 in SOF2).
Stock q3map2 scatters every shader over most lightmap pages, for three reasons:
1. first-fit placement drops each lightmap into the first hole on *any* page;
2. a lightmap block can hold several coplanar shaders, so secondary shaders ride along onto other shaders' pages;
3. identical lightmaps are shared across shaders, which points surfaces at pages anywhere in the map (2/3 of mp_strike's lightmapped surfaces).

With `-game sof2|jk2|ja` the light stage now packs per shader: one shader per lightmap block, pages that already hold the shader first,
a shader only moves to a partly used page if all of its remaining lightmaps fit there, and identical-lightmap sharing only between blocks
whose shaders are a subset of the target's. The light log prints the real pair count:
`(shader, lightmap) pairs in the bsp`.

mp_strike at `_lightmapscale 1`: **1938 → 409 pairs**, 100 → 119 lightmap pages, 98.5% of lightmapped vertices identical (≤1/255).
`-noshaderpack` restores the stock behaviour; `-shaderpack` enables it for other games. `Q3MAP2_DEBUGPACK=1` dumps every placement to stderr.

### `-mergesurfaces` (post-light, fewer surfaces per frame)
SOF2's frame cost scales with drawsurfaces in view (mp_strike: 27k surfaces = 67 fps, 10k = 200 fps), and on open
CoD-style maps VIS can't help much (mp_strike: the average cluster sees 65% of the map). Run on the finished bsp:

    q3map2 -game sof2 -fs_basepath <SoF2> -fs_game base -mergesurfaces maps\<map>.bsp

- flat faces with the same shader, fog, lightmap page(s), light styles and normal, plane distances within
  `-planerange` (64) and inside one `-cell` (2048 units; in-game test on mp_strike: 1024 → ~12.2k srfs in view, 2048 → ~10.4k, 4096 → ~10.2k), become one face of at most 64 verts (SOF2 MAX_FACE_POINTS).
  The rearmost piece goes first, so the renderer's back-face cull (plane from the first vertex) never hides a visible piece.
- vertex-lit triangle soups (baked misc_models) and vertex-lit faces whose shader has `surfaceparm nomarks`
  merge into soups (999 verts / 6000 indexes). Soups get no bullet marks, hence the nomarks rule. `-nosoups` disables.
- patches, flares, autosprite, sky and lightmapped soups are untouched; leaf and brushside links are remapped.
- surfaces only merge on the same lightmap page: no luxel moves, (shader, lightmap) pairs can't grow.

Per map, worldspawn keys `"_mergecell" "<units>"` and `"_mergeplanerange" "<units>"` override the defaults
(mp_strike: cell 1024 → 27,010 surfaces, 2048 → 23,607, 4096 → 21,635; bigger cells draw more off-screen triangles).

**Automatic:** `-light -game sof2` runs this merge itself when lighting is done, so front-ends that can't add a
step (CoD4toSOF2 GUI, SourceToSoF2) get it for free. `-nomerge` skips it, `-merge` enables it for other games.

mp_strike: 72,206 → 27,010 drawsurfaces (median potentially visible per cluster 66,677 → 24,986), 2 seconds,
the triangle set (positions, UVs, lightmap UVs, colours, pages) bit-for-bit identical. Must be the last step:
`-light` on a merged bsp warns.

### Decompile on q3map2-compiled maps
The linked-surface fit takes whole triangles of the side (all corners on its plane, centroid inside the brush), so faces of
neighbouring brushes that q3map2's meta merge (or `-mergesurfaces`) put in the same surface no longer pollute the fit.
mp_strike (q3map2 bsp): inexact sides 9,788 → 518. Raven bsps unchanged.

### Crash fixes
- assimp LWO loader: out-of-bounds read on `.lwo` files with vertex colour maps (it indexed surfaces with a stale face counter). This crashed `-bsp` on CoD4toSOF2 maps, which ship their vertex-alpha blend layers as `models/cod4/blend/*_layers.lwo`. Found with AddressSanitizer on mp_strike.
- A model that fails to load is now named in the warning (assimp's own message, e.g. "No meshes remaining", doesn't say which file), and is only tried once instead of once per placement.

### Detecting this build from a front-end
- the version string carries `-sof2`: `Q3Map (ydnar) - v2.5.17n-sof2-git-<commit>`
- `q3map2 -sof2caps` prints `sof2caps 1` and one feature per line
  (`exactdecompile`, `shaderpack`, `mergesurfaces`, `automerge`, `keepmodels-default`, `lwofix`).
  A tool can run it once, and offer options only for the features listed. Stock q3map2 prints its usage error instead.

### Build
- No glib dependency for q3map2 (directory listing uses `std::filesystem`).
- GCC 13 build fix.

## Usage

```
q3map2 -game sof2 -fs_basepath "C:\Games\SoF2" -fs_game base -convert -format map maps\mp_shop.bsp
```

writes `maps\mp_shop_converted.map`. Textures and shaders must be findable under `base` (in pk3s or loose), because texture sizes are needed to recover scale and shift.

## misc_models
Stock SOF2 BSPs (compiled by Raven's tool) keep every `misc_model` entity in the entity lump, so decompiling restores them as-is. BSPs compiled by stock q3map2 strip them; recovery for those is planned (model-library matching).
