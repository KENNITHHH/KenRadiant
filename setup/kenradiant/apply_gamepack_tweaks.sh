#!/bin/sh
# KenRadiant: tweak the downloaded gamepacks in place. Usage: apply_gamepack_tweaks.sh <install>/gamepacks
# - SOF2's tool shaders are textures/tools/_caulk etc.; the pack's caulk_shader pointed at a non-existent textures/tools/caulk
# - insert the KenRadiant compile presets at the top of the SOF2 Build menu (once)
G="$1"
HERE=$(dirname "$0")
[ -d "$G" ] || exit 0
if [ -f "$G/games/sof2.game" ]; then
	sed -i 's|caulk_shader="textures/tools/caulk"|caulk_shader="textures/tools/_caulk"|' "$G/games/sof2.game"
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
