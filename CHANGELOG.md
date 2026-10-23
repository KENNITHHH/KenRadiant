# KenRadiant changelog

Builds are published as **KenRadiant Latest Build** on the Releases page; the q3map2 version string carries the commit
(`q3map2 -v` → `2.5.17n-sof2-git-<commit>`).

## Build 2026-10-23 (11)
### Editor
- **Compile window** (hammer button on the toolbar, Build → Compile..., F9): tick BSP / VIS / LIGHT and their options, each with a plain explanation of what it does and what it costs; presets (Quick test, Final, High quality, Re-light only, Entities only); shows the exact q3map2 commands. *Compile* saves the map and runs them (output in the console / build monitor as usual), *Add to Build menu* keeps them as "KenRadiant: (Compile window)". Settings are remembered.
- **Hidden caulk blocks light**: faces that aren't drawn right now (caulk / nodraw in Game View, filtered or hidden brushes, hidden layers) still cast sun, sky and light shadows in the live light preview, like in the compiled map. Tunnels with a caulk roof are no longer sunlit.
- **Up to 256 preview lights** (default 128, was 48): the limit is what your graphics card's shader can hold (the console says "light preview: up to N lights" at startup). Lights only cost time where they reach.
- **Map Text**: toolbar button; selecting in the views now always jumps to the line (it didn't when the selection also refreshed the text); the Find box works (Enter = next, Shift+Enter = previous, it used to press Refresh) and selects what it finds in the views, e.g. type `tree_lrg.md3`.
- **Go To** toolbar button (crosshair).
- **Fixed: runtime error when closing the editor** (an unreleased selection-outline state and the terrain shader observer made the shutdown assert).

## Build 2026-10-23 (10)
### Editor
- **Sky light now sees roofs**: `q3map_skyLight` in the light preview is occluded like q3map2 (9 sky directions with shadow maps), so rooms, tunnels and covered areas are dark again instead of lit like outside. Corners and walls get less sky than open floors. (View → Light Preview → Sun Shadows also turns the sky shadows on / off.)
- **Up to 48 lights** in the preview (was 24), so fewer small lights pop in and out while you fly. Set it in the Sun Calibrator window.
- **Map Text window** (View → Map Text Window): the .map as text. Select something in the views and the text jumps to its entity / brush (highlighted); click a line to select that brush / entity, double-click to fly there. Find box, follows your edits.
- **Go To Coordinates** (View menu, Ctrl+Shift+G): type or paste `x y z` (an origin, a console line ...) and the camera flies there.
- **Version and update check** at the right of the menu bar: shows this build's commit and asks GitHub at startup whether a newer build exists (click to open the download page). Help → *Check for Updates at Startup* turns it off, *Check for KenRadiant Update Now* checks on demand.
- **Fixed: assertion "orphan shader still referenced"** when the skybox button (or Flush & Reload Shaders) reloaded shaders with a terrain in the map; the terrain look comes back right after the reload.
- **Fixed: assertion "pointer m_shader is null"** when selecting terrain.
- Terrain Paint: the layers are a dropdown (the window no longer grows and spams geometry warnings).

## Build 2026-10-23 (9)
### Editor
- **Terrain blending in the 3D view**: q3map2 index-map terrain (a func_group with `alphamap` / `_indexmap` .pcx, `layers` and `shader`, faces with a `q3map_terrain` shader such as `textures/tools/_terrain`) shows its layers `textures/<shader>_0 ... _3` blended by the index map, with each layer's `tcMod scale`, lit by the live light preview too. The terrain stays visible in Game View.
- **Terrain Paint** (toolbar button): left-drag on the terrain in the 3D view paints the chosen layer into the index map (radius slider, green circle = on a terrain). Ctrl+Z / Ctrl+Y per stroke. The .pcx is saved when you save the map (or with *Save index maps now*), q3map2 reads it as before.
- **Make terrain**: in the Terrain Paint window, select brushes in a func_group, pick a layer shader set (found automatically: shaders with `_0`, `_1` ...) and an index map size; KenRadiant sets the keys, gives the faces `textures/tools/_terrain` and starts a new index map (`maps/<map>_terrainN.pcx`).

## Build 2026-10-23 (8)
### Editor
- **Brush Shapes** (toolbar button / Misc → Brush Shapes): Unreal-style geometry presets - Box (hollow), Cone (hollow with cap Z), Cylinder (hollow), Curved Stair, Linear Stair, Spiral Stair (sloped floor / ceiling), Sphere (tessellation 0-3). Created on the surface in the middle of the 3D view with the selected texture (caulk if none); sliders update the last created shape live until you edit something else. One Ctrl+Z removes it.
- **Check for Problems** (Misc menu, Ctrl+Alt+P - Hammer's Alt+P opens the Plugins menu here): broken targets, missing models and textures, unknown entities, empty brush entities, degenerate and duplicate brushes, duplicate entities, no player start. Click a line to select it, double-click to fly there.
- **Light Preview Settings** (in the Sun Calibrator window): how many lights light the preview (1-24) and how many of the nearest cast shadows (0-8, default 4).
- **Fixed: Ctrl+Z in edge mode** lost the selected edges (vertices and edges are now remembered by the faces that meet there).
- **Fixed: double-click** picked another object; it now keeps what you selected and opens the Entity window.
- **Fixed: visible playerclip geometry** (trash cans, nets: `surfaceparm playerclip` without `nodraw`) was hidden by the Clip filter and Game View.
- **Fixed: shaders like `blendFunc GL_SRC_ALPHA GL_ZERO` + alphaFunc** (airport trash can) were drawn see-through; a `GL_ZERO` destination covers what is behind it.

## Build 2026-10-23 (7)
### Editor
- **Light preview brightness like q3map2 + the game**: point lights use q3map2's falloff (`light × 7500 / distance²`, `spawnflags 1` = linear), `_color` is normalised, `scale` is used; the sun and `_ambient` are in lightmap units. The result is doubled and clamped keeping its colour like the engine does, so indoor maps with many lights no longer wash out to white.
- **Surface snapping** (magnet button): drag the white ball and the selection sticks to the surface under the cursor; **Alt** also turns it to the surface, **Shift** slides along it. Without the button, hold **Ctrl** while dragging.
- **The white ball always moves the selection** (it used to move the pivot when you grabbed its exact centre). Move the pivot in rotate mode (R).
- **Measure tool** (ruler button): click two points on surfaces in the 3D view; the line shows the distance and the X / Y / Z parts (also printed in the console). **Shift** snaps the points to the grid.
- **Double-click an entity** in the 3D view (model, light, door brush ...) to select it and open the Entity window.
- **Vertex / edge selection survives Ctrl+Z / Ctrl+Y**: only the movement is undone.
- **Game View** also hides entity names, target names, angle arrows, target lines and light radii.
- **Menus show real check boxes** (Filter menu: "Checked = hidden" at the top).
- Gizmo handles are easier to grab: the scale gizmo's triangles have a much wider hit area, and plane handles win over the arrows behind them.
- Translucent shaders whose first stage has `depthWrite` no longer show their own back side through themselves.

## Build 2026-10-23 (6)
### Editor
- **Light preview follows q3map2's shadow rules**: entities with `_castShadows 0` / `_cs 0` (e.g. a lantern model around its light) don't block light; doors and other brush entities only cast with `_castShadows 1`; worldspawn, func_group and misc_model cast by default.
- **Tool surfaces let light through** like in q3map2, read from the shader's surfaceparms: `trans`, `nonsolid`, `playerclip`, `monsterclip`, `botclip`, `water` / `slime` / `lava` / `fog`, `areaportal`, `clusterportal`, `donotenter`, `origin`, `trigger`, `ladder`, `shotclip`, `hint`, `skip`. Foliage with `alphashadow` / `lightfilter` still casts its cut-out shadow.
- **Sky light** (`q3map_skyLight`) now lights the preview like a sky dome: faces turned away from the sun are no longer near-black (terrain such as mp_col1 looks far more like in game). Worldspawn `_minlight` is used too.
- **Selection tint no longer z-fights** with the face under it.

## Build 2026-10-23 (5)
### Editor
- **Fixed: sun shadows stopped after opening another map** (also when you came back to the first map). The sun is now read from the sky shader on the map's own sky brushes.
- **Shadows from lights**: the 4 lights nearest to the camera cast real-time shadows (cube shadow maps) in the live light preview. View → Light Preview → Light Shadows.
- **Sun Calibrator** (sun button / View → Light Preview): sliders for colour, intensity, degrees, elevation, `q3map_sunExt` deviance (softer shadows) and samples, `q3map_skyLight`. You see the change live; **Write to shader file** updates the sky shader (unchanged numbers keep their exact text, a `.bak` is kept) and reloads shaders. **Copy lines** for shaders inside a pk3.
- **Compile preview** in the Sun Calibrator: `-gamma`, `-compensate`, `-saturation` on the light preview, `-bounce` as an approximated fill light. **Make build preset** adds *KenRadiant: (Sun Calibrator settings)* to the Build menu.
- **Shader effects**: `tcMod turb`, `tcMod stretch`, `tcMod scale`, `deformVertexes wave / move / bulge` animate, `polygonOffset` decals draw in front of their wall. View → Shader Effects has a toggle for each (and for glow fullbright).
- **Selection styles (J)**, like GtkRadiant: default = models / entities orange outline, brushes edges + tint; then everything outlined; then classic. (*Show size in 2D views* no longer uses J.)
- **Scale gizmo**: the triangle between two axes always scales both axes by the same amount.

## Build 2026-10-23 (4)
### Editor
- **Sun shadows in the live light preview**: the sky shader's sun casts real-time shadows (shadow map, soft edges). Translucent, sky and fog surfaces don't cast; masked (alphaFunc) ones do. Toggle: View → Sun Shadows.
- **Game View keeps the light preview**: lights still shine, only their icons are hidden.
- **Glow shaders**: `glow`, additive extra stages and `q3map_surfacelight` shaders show fullbright in the light preview.
- **Moving shaders**: `tcMod scroll` and `tcMod rotate` animate in the 3D view (water, lava, conveyors). View → Animate Shaders turns it off.
- **Unreal-style snapping**: toolbar combos for **Grid**, **Rot** (1°, 5°, 10°, 15°, 22.5°, 45°, 90°; hold Shift for free rotation) and **Scale** (grid or fixed steps).
- **Selection outline**: selected objects keep their textures with an orange outline (Unreal style). View → Selection Outline switches back to the red tint.
### Build menu
- New preset **KenRadiant: (final, best quality - slow)**: `-super 4 -samples 3 -filter -bounce 8 -dirty -patchshadows`.
  It shows automatically unless you once edited the build menu (then `settings/1.6.0/sof2.game/build_menu.xml` exists; delete it to get the new defaults).

## Build 2026-10-23 (3)
### Editor
- **Live light preview** (Ctrl+L, bulb button, View → Live Light Preview): light entities (`light`, `_color`) and the sky shader's sun
  (`q3map_sun` / `q3map_sunExt`, `q3map_skyLight`) light the 3D view in real time, with worldspawn `_ambient`. No shadows or bounce - use it for placement and colour, compile for the final look.
- **Drag & drop models**: drag a model from the Models tab into the 3D view (it lands on the surface under the cursor and follows the mouse) or into a 2D view (top view drops it onto the floor). One Ctrl+Z removes it. Ctrl + left drag still rotates the preview.
- **KenRadiant toolbar** on its own row, so nothing is hidden: Find, Local gizmo, Drop to floor, Bevel, Skybox, Game View, Live light preview, FOV.
- Texture browser zoom goes up to 400 % (Ctrl + wheel: 10, 25, 50, 100, 150, 200, 300, 400 %).

## Build 2026-10-23 (2)
### Editor
- **Textures / Models tabs** under the camera (regular layouts): the model browser now sits next to the texture browser. **Ctrl + wheel** resizes model cells too.
- **Unreal-style scale gizmo**: axis lines with cubes (one axis), triangles between two axes (two axes), white centre cube = uniform scale (drag right/up to grow), sizes snap to the grid.
- **Ctrl+Shift+click selects faces** again, as in GtkRadiant (Ctrl+click still works too).
- **Ctrl+B** works (it had no menu entry, so the shortcut was never connected); also View → Browse To Selected Texture.
- **Skybox button** and **Game View button** on the FOV toolbar. Game View turns the skybox preview on and puts it back as it was.
- Bevel (Shift+B) also works on faces picked with Ctrl / Ctrl+Shift+click.

## Build 2026-10-23
### Editor
- **Sky brushes** show as brushes with the sky shader's editor image again (like GtkRadiant). View → Show → *Sky Brushes Show Skybox* turns the in-game sky preview back on.
- **Game view** (Shift+G) now also hides every `surfaceparm nodraw` surface (caulk, nodraw, hint, skip, clip, shoot, trigger, vmf2map tools …) and point entities (sounds, info_null, targets …). Models and world geometry stay.
- New filter **Nodraw (all tool surfaces)**; the Caulk / Hint / Areaportal / Clusterportal filters know SOF2's `textures/tools/_*` names.
- **Local gizmo** stays on the object while dragging.
- **Space / C fly up/down** now also works with shortcuts saved by older builds (the saved empty binding overrode the new default). Same for End (drop to floor) vs Center View.
- **Rotate gizmo** Unreal-style: thick red/green/blue rings, white outer ring for view-axis rotation, faint guide circle; same size as the move gizmo.

## Build 2026-10-22 (night)
### Editor
- **Bevel** (Shift+B, toolbar button, Brush → Bevel): chamfers edges by the current grid size, like Blender's bevel.
  Edge mode (E): the selected edges; face mode (F): every edge of the selected faces; whole brushes: all their edges.
- **Unreal-style move gizmo**: thick axis lines, plane handles drawn as an L in the two axis colours with a fill that lights up yellow, white ball in the middle for screen-plane moves, 25 % bigger.
- **Local gizmo fixed**: moving along the object's own X/Y axes went the wrong way (only Z was right). Grid snapping now follows the gizmo's axes.
- **Game view** (Shift+G, Filter menu): hides caulk, clip, hint/skip, triggers, lights, paths, portals, botclip, lightgrid; Shift+G again restores your filters.
- **Fly up / down**: Space and C while flying (right mouse held). Outside flying they keep Duplicate / Deselect.
- **FOV slider** is its own toolbar (grab the dotted handle to move it).
- **AZERTY**: only the camera keys follow the layout (fly with ZQSD); every tool keeps its Unreal/Radiant letter (W move, E edges, R rotate, Z delete).
  Shortcuts changed by earlier versions are put back automatically.

## Build 2026-10-22 (evening)
### Editor
- **Shader preview**: `alphaFunc` stages render cut-out (masked), `blendFunc` stages render translucent, additive (glows) or multiply.
- **World / Local gizmo** toggle (cube button on the toolbar). Local uses the object's own rotation (models, entities with `angles`).
- **Alt + drag** now keeps moving after the copy is made (the copy stays behind, the selection keeps following the mouse); one Ctrl+Z undoes it.
- Gizmo plane handles are bigger and filled.
- **AZERTY**: Ctrl/Alt shortcuts keep their letters (Ctrl+Z stays undo); only plain letter keys swap (WASD → ZQSD).
  Number-row shortcuts (1–0, Alt+6 …) also work on the AZERTY characters (&, é, ", ', (, §, è, !, ç, à; French layout: - and _ for 6 and 8).
  Shortcuts saved by the first AZERTY version (Ctrl+W for undo etc.) are corrected automatically.
- More room left of the FOV label.

## Build 2026-10-22 (afternoon)
### Editor
- **Fixed "SHADER NOT FOUND"** for shaders listed after a shader with `{` in its name (`textures/cstrike/{tk_grate`): sky, `_nonsolid`, emitters, model shaders.
- SOF2 tool shaders for the caulk/clip/hint/nodraw/trigger buttons, AutoCaulk, Hollow and clipper (`textures/tools/_caulk` …).
- AutoCaulk (F4) compiles with `-game sof2` so translucent/sky/liquid rules come from SOF2 shaders.
- Unreal-style **gizmo plane handles** and **Alt + drag duplicate**.
- **End** = drop to floor (Center View → Shift+End), **Ctrl+B** = show the selected texture in the texture browser.
- **Find** (magnifier) button, **FOV slider**, mouse wheel = fly speed while holding right mouse.
### Build
- The editor's q3map2 uses the bundled, patched model loader (fixes the `.lwo` crash on CoD4-converted maps such as mp_strike).

## Build 2026-10-22 (morning)
### Editor
- Shaders without a working editor image fall back to the first stage image, the shader name, or the sky's `_ft` image.
- Ctrl + mouse wheel resizes texture browser thumbnails.
- Edit → **AZERTY Keyboard (ZQSD)**.
- 3D workzone off and texture lock on by default.
- **Build menu**: KenRadiant SOF2 presets (final, high quality, quick test, no merge, decompile .bsp → .map).

## Initial release (based on netradiant-custom 8216133)
### q3map2 SOF2 edition
- Exact texture alignment when decompiling (`-convert -format map`), SOF2 origin brushes, Raven `q3map_*` keywords.
- Per-shader lightmap packing for SOF2's `MAX_SHADERS` limit.
- `-mergesurfaces`, automatic after `-light -game sof2` (`-nomerge` to skip); `_mergecell` / `_mergeplanerange` worldspawn keys.
- misc_models kept and `_bsp` model shaders used by default for SOF2, like Raven's compiler.
- Crash fix for `.lwo` models with vertex colours; failed models are named once instead of per placement.
- `-sof2caps` feature probe for front-ends (CoD4toSOF2, SourceToSoF2).
