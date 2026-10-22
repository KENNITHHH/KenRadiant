#!/bin/sh
# KenRadiant: tweak the downloaded gamepacks in place. Usage: apply_gamepack_tweaks.sh <install>/gamepacks
# - SOF2's tool shaders are textures/tools/_caulk etc.; the pack's caulk_shader pointed at a non-existent textures/tools/caulk
# - insert the KenRadiant compile presets at the top of the SOF2 Build menu (once)
G="$1"
HERE=$(dirname "$0")
[ -d "$G" ] || exit 0
if [ -f "$G/games/sof2.game" ]; then
	sed -i 's|caulk_shader="textures/tools/caulk"|caulk_shader="textures/tools/_caulk"|' "$G/games/sof2.game"
	# the editor's caulk/clip/hint/nodraw/trigger buttons, AutoCaulk and Hollow look up shader_<name>, else textures/tools/<name>
	if ! grep -q 'shader_caulk=' "$G/games/sof2.game"; then
		sed -i 's|^\(\s*\)caulk_shader=\(.*\)$|\1caulk_shader=\2\
\1shader_caulk="textures/tools/_caulk"\
\1shader_clip="textures/tools/_clip"\
\1shader_weapclip="textures/tools/_shoot"\
\1shader_hint="textures/tools/_hint"\
\1shader_hintlocal="textures/tools/_hint"\
\1shader_hintskip="textures/tools/_skip"\
\1shader_trigger="textures/tools/_trigger"\
\1shader_nodraw="textures/tools/_nodraw_solid"\
\1shader_nodrawnonsolid="textures/tools/_nodraw"\
\1shader_watercaulk="textures/tools/_nodraw_water"\
\1shader_lavacaulk="textures/tools/_nodraw_water"\
\1shader_slimecaulk="textures/tools/_nodraw_water"|' "$G/games/sof2.game"
	fi
fi
M="$G/sof2.game/default_build_menu.xml"
if [ -f "$M" ] && ! grep -q "KenRadiant presets" "$M"; then
	# after the first line that defines the q3map2 variable
	awk -v presets="$HERE/sof2_build_presets.xml" '
		{ print }
		!done && /<var name="q3map2">/ { print ""; while ((getline line < presets) > 0) print line; done = 1 }
	' "$M" > "$M.tmp" && mv "$M.tmp" "$M"
fi
exit 0
