# KenRadiant

SOF2-focused Radiant, based on [Garux/netradiant-custom](https://github.com/Garux/netradiant-custom), bundled with the SOF2 q3map2 (see README_SOF2.md).

## Editor changes
| What | How |
|---|---|
| Shaders whose name contains `{` (e.g. `textures/cstrike/{tk_grate`) no longer break the rest of their .shader file | shader files are tokenised like the game does: `{`/`}` inside a word belong to it |
| Shaders without a usable editor image still show a picture | first stage image, image named like the shader, sky `_ft`, lowercase variants |
| SOF2 tool shaders | caulk/clip/hint/skip/trigger/nodraw buttons, AutoCaulk (F4), Hollow and clipper cuts use `textures/tools/_caulk` etc. |
| AutoCaulk uses the game's own shader rules | runs q3map2 with `-game sof2` instead of `-game quake3` |
| Gizmo plane handles (Unreal) | the coloured squares between two arrows move only in that plane |
| Alt + drag the gizmo (Unreal) | duplicates the selection and moves/rotates/scales the copy, one undo step |
| End: drop to floor (Unreal) | selection moves down onto the highest brush below it; Center View moved to Shift+End |
| Mouse wheel while holding right mouse in fly mode (Unreal) | camera fly speed |
| Find (magnifier button / Tab) | flies the camera to the selection |
| Ctrl+B (Unreal "browse to asset") | shows the selected face's/brush's/patch's texture in the texture browser |
| FOV slider | right end of the main toolbar |
| Ctrl + wheel in the texture browser | thumbnail size 10/25/50/100/200 % |
| Edit > AZERTY Keyboard (ZQSD) | swaps W/Z and A/Q in every shortcut |
| Defaults | 3D workzone off, texture lock on |
| Build menu | KenRadiant SOF2 presets (final, HQ, quick, no-merge, decompile) |
