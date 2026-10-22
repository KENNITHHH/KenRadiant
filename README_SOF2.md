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

### SOF2 compile fixes
- `q3map_hint`, `q3map_areaportal`, `q3map_origin`, `q3map_structural` and `q3map_clusterportal` (Raven keywords used by SOF2's stock `tools.shader`) are honoured. Before this, `tools/_hint` did nothing and `tools/_origin` was not an origin brush.
- Decompiled SOF2 maps write origin brushes with `tools/_origin` (was JK2's `system/origin`), so doors and rotating entities keep their pivot when recompiled.

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
