# KenRadiant changelog

Builds are published as **KenRadiant Latest Build** on the Releases page; the q3map2 version string carries the commit
(`q3map2 -v` → `2.5.17n-sof2-git-<commit>`).

## Build 2026-10-24 (26)
### q3map2
- **Decompile brings index-map terrain back as terrain.** The .bsp keeps the layer shaders (`<base>_0`, `_0to1` ...) on the brush sides and every vertex's layer in its alpha; the decompile now puts those brushes in a func_group with `alphamap` / `layers` / `shader` / `min` / `max`, gives their layer sides `tools/_terrain` again (same texture coordinates) and writes `<map>_terrainN.pcx` so q3map2 reads the same layer at every vertex. mp_raven's snow landscape (metashader/kam4): 780 of 780 matched vertices get the same layer after decompile + recompile (before: every face showed the top layer - the "way off" textures). Decompile again to get it.
### Editor
- **Terrain Paint paints where the cursor is, also on selected terrain.** The paint ray skipped selected brushes (they stay selected after Create terrain) and hit what was behind; it now hits the terrain's own faces (unless something unselected is in front).
- **Dots show what the compiled map gets**: the brush corners near the cursor, coloured by the layer q3map2 will read there (it only reads the index map at the corners - paint across them; smaller brushes = finer painting).
- **Hardness** slider: below 100% the edge of the brush is painted thinner and thinner (dithered).

## Build 2026-10-24 (25)
### Editor
- **Terrain Paint writes into the game folder** (engine path + base), not next to the map: with a map kept elsewhere (your Extracted copy) the layer shaders and the .pcx were written where neither the editor nor q3map2 looks ("Texture load failed: textures/<map>/terrain1_0"). The new shaders are also loaded before the terrain asks for them.
- New Build menu preset **"KenRadiant: (like Raven's own maps)"** = `-light -fast -super 2 -filter`, no bounce, no -brightness / -gamma: measured against the original mp_shop.bsp, this q3map2 with these settings gives the same lightmaps (1.00x at every brightness). (Shows in new installs' Build menus; or set it in the Compile window.)

## Build 2026-10-24 (24)
### Editor
- **Terrain Paint: your own layers.** Pick 2-4 textures (click one in the Textures tab, **+ Texture browser's texture**; Remove / Up / Down), select the ground brushes and **Create terrain from the selected brushes**: they go into a func_group (if they are not in one), KenRadiant writes the layer shaders (`textures/<map>/terrainN_0`, `_1`, `_0to1` ...) to `shaders/<map>_terrain.shader` (listed in shaderlist.txt when there is one, .bak kept), makes the .pcx index map, gives the upward faces `tools/_terrain`, and you paint. The lightmap stage uses the same blend as your map's own shaders (2x `GL_DST_COLOR GL_SRC_COLOR` or 1x). Texture scale (tcMod scale) and index map size are set there too.
- Move the mouse over a terrain made this way: its layers show in the list - add, remove or reorder them and **Apply**: the shaders are rewritten and the painted layers follow (a removed layer becomes layer 0). Checked with q3map2: the compiled .bsp uses the new layer shaders.
- **Performance view counts like the engine**: each surface is culled against the view and one-sided faces seen from behind are left out (SOF2's R_CullSurface), so "in view" is closer to r_speeds; shows the leaf count too.

## Build 2026-10-24 (23)
### Editor
- **Performance view** (Build → Performance): what the game draws from the camera, from the compiled .bsp the way the engine picks it (vis cluster → PVS → leafs in view): triangles, surfaces (draw calls), passes (surfaces x shader stages), shaders, blended surfaces - colour bars against rough SOF2 budgets, updating as you fly - and the heaviest shaders in view. **Find worst spots + suggestions** ranks all vis clusters (Go flies there) and suggests fixes: run VIS, make small structural brushes detail (button, undoable), lightmap count, multi-stage shaders used a lot in heavy spots, the share of baked models. On your de_dust2.bsp: the worst spots see the whole map (~131k triangles), 82% of it baked models.
- Live light: point-light shadows get more normal offset at grazing angles (less acne on walls lit from the side, e.g. mp_shop).

## Build 2026-10-24 (22)
### Editor
- **Models in the live light preview at the game's brightness** (your third probe: live light itself was right - walls 1.04 x, decals 1.02 x, models 1.06 x what q3map2 stored - but models showed 0.54 x the game). The compiler bakes misc_models with the shader's `_bsp` variant when one exists (two `rgbGen vertex` stages, the 2x), the editor drew them with the plain one (`rgbGen lightingDiffuse`, one stage). The editor now uses the `_bsp` variant too, like q3map2 (after `_remap`).
- Without light (preview off), the "same image added again with `rgbGen vertex`" stage of those shaders and decals adds nothing, so full-bright views don't show them doubled.

## Build 2026-10-24 (21)
### Editor
- **Your second dust2 probe (Show Compiled Lighting) now matches SOF2**: walls 1.00 x, decals 1.00 x, models 1.02 x the game.
- **Fixed (found with the probe): decals, posters and other `cull none` surfaces lit from behind in the live light preview** (they got about a third of the light q3map2 gives them - the dark poster and B arrow). The editor draws two-sided surfaces with the other winding, so the preview took their back for their front; it now uses the side the camera sees.

## Build 2026-10-24 (20)
### Editor
- **Brightness set from your dust2 measurement** (Lighting Probe, editor vs SOF2 at the same spot, ~2900 points). The game does: lightmapped walls = 1.97 x texture x stored lightmap (the shader's `GL_DST_COLOR GL_SRC_COLOR` doubling, nothing else), vertex lit = the stored vertex colour as it is per stage (models 2.00 x, decals 2.07 x with their two stages). The editor halved vertex light (an `identityLight` 0.5 guess); that's gone, in Show Compiled Lighting and in the live preview. Domes, palm trunks, crates and decals were 2x too dark; walls already matched (1.01 x). `rgbGen identityLighting` is 1 (r_overBrightBits 0).

## Build 2026-10-24 (19)
### Editor
- **Lighting Probe** (Build menu): measures instead of guessing. *Record* takes ~4000 points of the 3D view and writes, per point, the compiled surface (shader, lightmapped or vertex lit), its texture colour, the light q3map2 stored there, what the editor shows, and - with the live light on - what the live preview would have stored. *Compare with game screenshot* lines a SOF2 screenshot up with those points by their world positions (eye height, field of view and small turns are found by itself) and reports the game's brightness as "x texture x stored light" per kind of surface (walls, decals, models), next to the editor's, with a brightness curve and a per-shader list. Report files: `<map>_lightprobe.txt`, `<map>_lightprobe_compare.txt`, `<map>_lightprobe_editor.png` next to the map.
- **Fixed (found with the probe): live preview lightmaps 2.2x too bright with `-brightness`.** q3map2 applies `-brightness` to lightmaps *after* the gamma (and `-vertexscale` to vertex light before it); the preview did both before. Live lightmaps now land within ~10% of what q3map2 stores, vertex light within 3%.
- **Fixed (found with the probe): decals far too bright in Show Compiled Lighting.** The editor's own decal faces (`polygonOffset` shaders) were drawn over the compiled decals, unlit and doubled by the additive stage (about 6x). The compiled decals now cover them.

## Build 2026-10-24 (18)
### Editor
- **Live light preview uses your compile settings**: the Compile window's LIGHT `-brightness`, `-gamma`, `-compensate` and `-vertexscale` (from the fields, Extra options or All options) now change the preview the same way q3map2 changes the lightmaps. Your dust2 compile (`-gamma 2.2 -brightness 0.28358 -vertexscale 0.0625`) no longer shows super bright - put those flags in the Compile window (easiest: **Load...** your preset).
- **Vertex-lit stages at the engine's real brightness** (`rgbGen vertex`, `identityLighting`): halved like SOF2's identityLight, in the live preview and in Show Compiled Lighting. Decals (posters, bullet holes) were about 2x too bright.
- **Compile window: Load...** next to Preset fills every tab from one of your saved Build menu entries (BSP / VIS / LIGHT flags, values, extra options).
- Glowing surfaces only come from shaders with `q3map_surfacelight` (sky shaders excluded), same as q3map2.

## Build 2026-10-24 (17)
### Editor
- **Fixed: "scenegraph root already exists"** (and the following "destroying a referenced object" / runtime error on close) when the Decompile tab's *Open the result* opened the map - it now closes the old map first like File > Open.
- **Show Compiled Lighting draws shaders like the game**: every stage with its blend, `rgbGen vertex` / `alphaGen vertex` from the .bsp's vertex light and alpha (terrain blends, the road overlay), the `$lightmap` stage with its own blendFunc (so `GL_DST_COLOR GL_SRC_COLOR` doubles like in game), alphaFunc, tcMod scale / scroll, animMap. Your converted decals (blend + additive copy, `rgbGen vertex`) come out like in game instead of very bright.
- **Brightness model fixed to match SOF2 as you play it** (r_mapOverBrightBits 1, r_overBrightBits 0): lightmaps and vertex light are used as stored; the shader's own lightmap blend does the brightening (2x for `GL_DST_COLOR GL_SRC_COLOR`). *Overbright* in Game look is now an extra brightening and starts at 0. Vertex-lit stages are clamped at 1 like the engine does.
- **Glowing surfaces light the preview**: faces with `q3map_surfacelight` become small lights (one per ~128 x 128 piece, only in front of the face, as bright as q3map2's area lights), coloured by their texture. The console says how many.
- **Fewer stripes (acne)**: bigger normal offsets for sun, sky and light shadows.
- **Models tab search**: type part of a path (e.g. `tree_lrg`) and press Enter - every model from all folders that matches is shown.
- **Entities tab**: spawnflags explain themselves on hover (from the definition's comment) and show their value.
- **Entity angle arrows / names come back**: closing the editor in Game View saved Game View's hidden settings (angles, names, filters). Now the real settings are saved. If your arrows are still off: View > Show > Show Entity Angles once.

## Build 2026-10-24 (16)
### Editor
- **Entities tab** (next to Textures / Models): every point entity of the game by group (pickup, info, target ...), searchable. Pick one, set its keys (the definition's keys + angle), spawnflags and - for SOF2 - the gametypes it appears in (dm tdm ctf inf elim dem, written as `"gametype" "dm tdm"`, none ticked = all), then **drag it into the 2D or 3D view**: in 3D it lands on the surface under the cursor (standing on floors, in front of walls, under ceilings), in 2D where you drop it. Double-click places it in the middle of the 3D view. Your settings per entity are kept while the editor runs.
- **SOF2 sky shaders light the preview**: SOF2's own `sun r g b intensity degrees elevation` keyword is read like q3map_sun (mp_finca's `textures/skies/col2_jersey`), and `q3map_surfacelight` on a sky shader counts as sky light.

## Build 2026-10-24 (15)
### Editor
- **Fixed: "orphan shader still referenced"** when shaders reload (skybox / game shader toggles, Flush & Reload) while Show Compiled Lighting is on.
- **All lights**: up to 8192 preview lights, default all of them; the Sun Calibrator says how many light entities the map has. The light grid now covers 8192 x 8192 x 3072 units around the camera and only moves in 1024-unit steps.
- **No more stripes on walls** (shadow acne): sun, sky and light shadows are looked up slightly off the surface.
- **Sun found more often**: any shader on the map's brushes with q3map_sun / q3map_sunExt / q3map_skyLight (not only surfaceparm sky ones), and q3map2's `_sun` light entities (a light with `_sun 1` and a target). The console says when there is no sun and why.
- **Shaders like in game: toolbar button** (stacked layers icon, next to Game View). More rgbGens count as lit (lightingDiffuseEntity), 2x-modulate stages are treated as the lightmapped base.
- **Compile window**: options are labelled and sorted by their q3map2 name (`-bounce (light bounces)`, `-dirty (corner dirt)` ...), the *All q3map2 options* lists too; *Add to Build menu* asks for a name; new **Decompile** tab - browse to any .bsp, -format map / map_bp, -fast, -legacydecompile, -modelclip, -wtf; *Decompile* runs without touching the open map, *Open the result* opens `<name>_converted.map`.

## Build 2026-10-24 (14)
### Editor
- **Shaders like in game** (View → Shader Effects, on by default): every stage of a shader is drawn the way the game draws it - blendFunc (add, filter, blend ...), alphaFunc, rgbGen (lightingDiffuse / vertex lit, wave, const), alphaGen, tcGen environment, animMap, per-stage tcMods. Glow stages (an extra `blendFunc GL_ONE GL_ONE` stage like `helipad_glass_glow`) now show. `$lightmap` stages are left out and the stage multiplied onto them becomes the lit base, so the live light preview stands in for the lightmap.
- **Show Compiled Lighting** (Build menu): draws the map's compiled .bsp in the 3D view with its real baked lightmaps (and vertex light), brightened like SOF2 (Game look settings). It reloads by itself when a compile writes a new .bsp, so: compile, look, change, compile again. Patches are drawn too.
- **Engine limits** in Check for Problems (Ctrl+Alt+P): game entities (1024 slots, ~64 for players), brush entities (256 inline models), different models / sounds on entities (256 each, shared with weapons and players), different shaders (1024 in a .bsp), brushes. Warns well before the limit; the counts are printed in the console.
- **Compile window**: options that don't work in SOF2 are greyed out with the reason (-deluxe / -deluxemode: no deluxemaps in SOF2; -external / -lightmapsize / -extlmhacksize / -lightmapdir: SOF2MP only reads 128 x 128 lightmaps in the .bsp; -wolf, -cpma) and are never added to the command.
- **Update notice** at the top right is no longer cut off: "Update available - click to download (you have ...)".

## Build 2026-10-24 (13)
### Editor
- **Compile window: every q3map2 option**: each tab (BSP, VIS, LIGHT) has *All N q3map2 options* - every other option of that stage as a compact grid of ticks with value boxes (typing a value ticks it), described by q3map2's own help text when you point at it (95 for LIGHT, 43 for BSP).
- **Game look** (Sun Calibrator / Light Preview Settings): *Overbright* (r_mapOverBrightBits - r_overBrightBits: 1 = SOF2 fullscreen, the default; 0 = windowed / no hardware gamma, darker) and *r_gamma*, so the preview can match how you play.
- **Up to 2048 preview lights**, each spot adds up the 48 strongest.
- New claw-hammer icon for the Compile window.

## Build 2026-10-24 (12)
### Editor
- **Up to 1024 preview lights** (default 1024, Sun Calibrator → Lights): the lights now live in a texture and each spot of the screen only adds up the 32 strongest lights that reach it (a 256-unit grid around the camera), so many lights stay fast. The 8 nearest still cast shadows.
- **Lights match q3map2 more closely**: spotlights (a light with a `target`) now light only their cone (`radius` key, default 64, soft 32-unit edge) instead of every direction; linear lights (spawnflags 1) have no angle falloff and use the `fade` key; spawnflags 2 (no angle falloff) and `_anglescale` work; `_light` is read before `light`, `light 0` means 300 like q3map2. Lights on misc_models are placed correctly.
- **Softer sky light**: 16 sky directions instead of 9, each with a wide soft filter: doorways no longer get 2-3 separate light patches from the sky.
- **Compile window, simpler**: one row with a preset menu, BSP / VIS / LIGHT switches, *Start SOF2 after* and a green *Compile* button; the options are compact tabs (BSP, VIS, LIGHT, Game) and explain themselves when you point at them. More options (-nocurves, -nodetail, -notjunc, -nopassage, -samples, -shade, -nocollapse).
- **Problems tab**: the compiler's warnings and errors while it runs, grouped when the same message repeats. Click one to select the entity / brush it names (or fly to the coordinates in it); Previous / Next step through them.
- **Start SOF2 after compiling**: Game tab - program (SoF2MP.exe in your engine folder) and arguments (`+set sv_pure 0 +set developer 1 +devmap %mapname%`); *Start SOF2 now* runs it without compiling.
- **Map Text find works**: Enter used to be taken by the clipper's shortcut. Typing lists every brush / entity that contains the text (a texture, a model, a key) - click one to go there; **Select all** selects all of them (e.g. every brush with a texture); `<` / `>` step through the hits.
- **Go To**: *Current* fills in the camera position, *Copy* copies it. The camera position is always shown at the bottom right (click it to copy).
- New hammer icon; the version text at the top right is bigger.

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
