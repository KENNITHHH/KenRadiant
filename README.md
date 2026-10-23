# KenRadiant

**A Soldier of Fortune II: Double Helix level editor**, built on
[Garux/netradiant-custom](https://github.com/Garux/netradiant-custom) and bundled with a SOF2-tuned **q3map2**
(faster compiles that stay under SOF2's engine limits, and near 1:1 BSP → .map decompiling).

Made by **KENNITHH**. See [CHANGELOG.md](CHANGELOG.md) for what changed in each build.

---

## Download & install

1. Download the latest **KenRadiant Latest Build** from the [Releases page](../../releases)
   (`kenradiant-<date>-windows-x86_64.zip` or `-linux-x86_64.7z`).
2. Unzip it into its own folder, e.g. `C:\Games\SoF2\KenRadiant\`. To update, unzip a newer build over it — your settings stay.
3. Start `radiant.exe`, pick **Soldier of Fortune II - Double Helix**, and point the engine path at your SoF2 folder
   (the one that contains `base`).
4. Open a map from `base/maps`. Compile it with **Build → KenRadiant: (final) …**.

> If a texture shows **SHADER NOT FOUND**, check that its image exists under `base/textures` (loose or inside a `.pk3`)
> and that its `.shader` file is listed in `base/shaders/shaderlist.txt`.

## What KenRadiant adds

### Editing (Unreal Engine style)
| Feature | How to use it |
|---|---|
| **Unreal-style move gizmo** (W) | Thick X/Y/Z arrows; the L-shaped handles between two arrows move only in that plane (XY / XZ / YZ); the white ball moves in the screen plane. Hovered parts turn yellow. |
| **Alt + drag to duplicate** | Hold **Alt** and drag any part of the move / rotate / scale gizmo: a copy stays behind, the selection moves. One **Ctrl+Z** undoes both. |
| **World / Local gizmo** | Cube button on the toolbar: off = world axes, on = the object's own rotation (models / entities with `angles`; brushes are always axis-aligned). |
| **Drop to floor** | **End** — the selection drops onto the highest brush below it (Edit → Drop To Floor). *Center View moved to Shift+End.* |
| **Find** | Magnifier button (or **Tab**) — flies the camera to the selection. |
| **Fly speed / up / down** | While flying (hold right mouse): **mouse wheel** = speed, **Space** = up, **C** = down. |
| **Game view** | **Shift+G** hides editor-only stuff (all nodraw/tool surfaces, triggers, lights, paths, portals, sounds and other point entities); again to restore. |
| **Scale gizmo** | Cubes on the axes scale one axis, triangles between two axes scale both by the same amount, the white centre cube scales uniformly (snapped to the grid). |
| **Textures / Models tabs** | Under the camera: switch between the texture and model browser; Ctrl + wheel resizes both (textures up to 400 %). |
| **Drag & drop models** | Drag a model from the Models tab into the 3D view (lands on the surface under the cursor) or a 2D view (top view: onto the floor). |
| **Live light preview** | **Ctrl+L** or the bulb button: lights and the sky shader's sun light the 3D view in real time, with **sun shadows** and **shadows from the 4 nearest lights** (View → Light Preview). Game View keeps the light, only the light icons hide. |
| **Sun Calibrator** | Sun button: sliders for the sky shader's `q3map_sun` / `q3map_sunExt` / `q3map_skyLight` (colour, intensity, degrees, elevation, deviance …) with live shadows; **Write to shader file** puts them in the .shader (keeps a .bak). Compile preview: `-gamma`, `-compensate`, `-saturation`, `-bounce` (approximation) on the preview, **Make build preset** adds them to the Build menu. |
| **Surface snapping** | Magnet button (or hold **Ctrl**): dragging the white ball sticks the selection to the surface under the cursor; Alt turns it to the surface, Shift slides along it. |
| **Measure tool** | Ruler button: click two points in the 3D view to see the distance (Shift snaps to the grid). |
| **Double-click** | Double-click an entity in the 3D view to open the Entity window. |
| **Selection styles** | **J** cycles: models / entities get an orange outline and brushes keep edges + tint (default), everything outlined, or classic edges + tint everywhere. |
| **Snapping** | Toolbar combos: **Grid** (0.125–256), **Rot** (off, 1°, 5°, 10°, 15°, 22.5°, 45°, 90° for the rotate gizmo; hold Shift for free), **Scale** (grid sizes or fixed steps x0.0625…x1). |
| **Shader effects** | View → Shader Effects: `tcMod scroll / rotate / scale / stretch / turb` and `deformVertexes wave / move / bulge` animate, `polygonOffset` decals sit in front, glow / additive / `q3map_surfacelight` shaders are fullbright in the light preview. |
| **KenRadiant toolbar** | Second toolbar row: Find, Local gizmo, Drop to floor, Bevel, Skybox, Game View, Live light preview, Sun Calibrator, Grid / Rot / Scale snap, FOV slider. |
| **Face select** | Ctrl+click or **Ctrl+Shift+click** (GtkRadiant style). |
| **Rotate gizmo** (R) | Unreal-style thick rings per axis, white outer ring rotates around the view direction. |
| **Sky brushes** | Shown with the sky shader's editor image so you can edit them; the cloud button (or View → Show → *Sky Brushes Show Skybox*) previews the real sky. Game View turns it on automatically. |
| **Bevel** | **Shift+B** (or the toolbar button / Brush → Bevel) chamfers edges by the grid size: selected edges (E), all edges of selected faces (F), or whole brushes. |
| **Browse to texture** | **Ctrl+B** — opens the texture browser on the texture of the selected face / brush / patch. |
| **FOV slider** | Its own small toolbar (drag the dotted handle to place it). |
| **Thumbnail size** | **Ctrl + mouse wheel** in the texture browser (10–400 %). |
| **AZERTY keyboard** | Edit → **AZERTY Keyboard (ZQSD)**: flying uses ZQSD, number-row shortcuts (1–0, Alt+1…Alt+9) work on the AZERTY keys (&, é, ", ', (, §, è, !, ç, à). Tool keys keep their letters (W move, E edges, R rotate, Z delete, Ctrl+Z undo). |

### SOF2 support
- **Shaders load like in the game**: names containing `{` (e.g. `textures/cstrike/{tk_grate`) no longer break the rest of the `.shader` file.
- **Shader preview**: textures with `alphaFunc` show as cut-out (masked), `blendFunc` stages show translucent / additive (glows) / multiply, like in game.
- Shaders without an editor image still show a picture: first stage image, sky `_ft` side, or an image with the shader's name.
- **Tool textures** use SOF2's own shaders: the caulk / clip / hint / nodraw / trigger filter buttons (right-click = apply to selection),
  **AutoCaulk (F4)**, Hollow and clipper cuts all use `textures/tools/_caulk` and friends.
- **Build menu presets** for this q3map2: final, final high quality (bounce + ambient occlusion), best quality (slow: supersampling, 8 bounces, dirt, patch shadows), quick test, final without surface merge, and **decompile .bsp → .map**.
- Defaults: 3D workzone off, texture lock on.

### Bundled q3map2 (SOF2 edition)
Details in [README_SOF2.md](README_SOF2.md). Highlights:
- **Decompile** (`-convert -format map`) with exact texture alignment recovery — Raven's kam6: 99.4 % of visible faces identical to the original .map.
- **Lightmap packing per shader** so maps stay under SOF2's `MAX_SHADERS` (mp_strike: 1938 → 409 shader/lightmap pairs).
- **Automatic surface merge** after `-light`: far fewer draw surfaces per frame (mp_strike busy view: ~27k → ~10k surfaces, 67 fps → 140+ fps).
- SOF2 defaults that match Raven's compiler (misc_models kept, `_bsp` model shaders), crash fixes for CoD4-converted `.lwo` models.

## Shortcuts that differ from NetRadiant
| Key | Action |
|---|---|
| End | Drop selection to floor |
| Shift+End | Center view (was End) |
| Ctrl+B | Show selected texture in the texture browser |
| Alt + drag gizmo | Duplicate and move/rotate/scale |
| Wheel while right mouse held | Camera fly speed |
| Space / C while flying | Fly up / down |
| Shift+G | Game view |
| Shift+B | Bevel |
| Ctrl+L | Live light preview |
| J | Cycle selection style (Show size in 2D views moved to the View menu only) |
| Ctrl+Shift+click | Select faces (also Ctrl+click) |

All shortcuts can be changed in Edit → Shortcuts.

## Building from source
Windows builds are made by GitHub Actions (MSYS2 / MinGW, `.github/workflows/build.yml`); run the workflow by hand to publish a new **KenRadiant Latest Build**.
On Linux:
```
make -j8 DEPENDENCIES_CHECK=off ASSIMP_INTERNAL=yes BUILD=release
```
The result is in `install/`. The SOF2 gamepack tweaks are applied by `setup/kenradiant/apply_gamepack_tweaks.sh` during `make`.

## Credits
- [Garux](https://github.com/Garux/netradiant-custom) — netradiant-custom, the base of this editor and of q3map2-custom
- ydnar, TTimo, divVerent and everyone behind GtkRadiant / NetRadiant / q3map2
- Raven Software — Soldier of Fortune II and the original kam6.map used to verify decompiling

The original netradiant-custom readme is kept in [README_NETRADIANT.md](README_NETRADIANT.md). License: GPL v2 (see `LICENSE`, `GPL`).
