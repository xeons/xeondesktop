#!/bin/sh
# SPDX-License-Identifier: MIT
# Fetches the pinned subprojects (SceneFX, for xd-compositor-fx; wlroots,
# for systems without 0.20) into subprojects/, so package builds (which
# don't download) include them. Meson reads the wraps and applies their
# patches; ones already there are left as they are.
set -eu
cd "$(dirname "$0")/.."
meson subprojects download
