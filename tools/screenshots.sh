#!/bin/sh
# SPDX-License-Identifier: MIT
# Takes the README's screenshots: tests/ui/run.py's showcase scenarios,
# each in a headless session of its own (a stand-in user and computer
# name, a fixed clock), copied into docs/screenshots as NAME.png. The Aero
# ones run on the GPU when there's one, for blurred glass:
#
#   tools/screenshots.sh [BUILD_DIR]
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
out=$root/docs/screenshots
keep=$(mktemp -d "$build/screenshots.XXXXXX")
trap 'rm -rf "$keep"' EXIT

software="showcase_classic showcase_luna showcase_olive showcase_silver showcase_dark
  showcase_watercolor showcase_date_time showcase_minefield showcase_shut_down
  showcase_security"
glass="showcase_aero showcase_aero_snap showcase_aero_thumbnails showcase_alt_tab"

# shellcheck disable=SC2086 # the lists are word-split on purpose
python3 "$root/tests/ui/run.py" --build-dir "$build" --keep "$keep" --keep-all $software
node=$(ls /dev/dri/renderD* 2>/dev/null | head -n 1 || true)
if [ -n "$node" ]; then
    # shellcheck disable=SC2086
    XD_UI_RENDERER=gles2 WLR_RENDER_DRM_DEVICE=${WLR_RENDER_DRM_DEVICE:-$node} \
        python3 "$root/tests/ui/run.py" --build-dir "$build" --keep "$keep" --keep-all $glass
else
    echo "screenshots.sh: no GPU; Aero's glass is drawn unblurred" >&2
    # shellcheck disable=SC2086
    python3 "$root/tests/ui/run.py" --build-dir "$build" --keep "$keep" --keep-all $glass
fi

for name in $software $glass; do
    shot=$(ls "$keep/$name"-*/*-showcase.png | tail -n 1)
    file=$(echo "${name#showcase_}" | tr _ -).png
    cp "$shot" "$out/$file"
    echo "$out/$file"
done
