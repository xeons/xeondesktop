#!/bin/sh
# SPDX-License-Identifier: MIT
# Fetches the pinned subprojects (SceneFX, for xd-compositor-fx) into
# subprojects/, so package builds (which don't download) include them.
set -eu
cd "$(dirname "$0")/.."
wrap=subprojects/scenefx.wrap
dir=subprojects/scenefx
[ -d "$dir" ] && exit 0
url=$(sed -n 's/^url = //p' "$wrap")
rev=$(sed -n 's/^revision = //p' "$wrap")
git init -q "$dir"
git -C "$dir" fetch -q --depth 1 "$url" "$rev"
git -C "$dir" checkout -q FETCH_HEAD
