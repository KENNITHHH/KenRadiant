# KenRadiant changelog

Builds are published as **KenRadiant Latest Build** on the Releases page; the q3map2 version string carries the commit
(`q3map2 -v` → `2.5.17n-sof2-git-<commit>`).

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
