/* SPDX-License-Identifier: MIT
 * Freedesktop icon theme lookup.
 *
 * Theme directories are scanned once into an index, so a lookup is a hash
 * probe instead of thousands of stat() calls. Loaded icons are cached per
 * pixel size. Icons come from installed system/user theme directories.
 */
#ifndef XD_UI_ICONS_H
#define XD_UI_ICONS_H

#include <cairo.h>

#include "../core/common.h"

typedef struct xd_icons xd_icons;

/* `theme` is tried first, then its usual fallbacks, then hicolor. */
[[nodiscard]] xd_icons *xd_icons_create(const char *theme);
void xd_icons_destroy(xd_icons *icons);

/* Icon `name` (or an absolute path) rendered at px x px. Owned by the
 * cache; nullptr if not found. */
cairo_surface_t *xd_icons_get(xd_icons *icons, const char *name, int px);

/* Tries each name in turn. */
cairo_surface_t *xd_icons_get_any(xd_icons *icons, const char *const *names, size_t count,
                                  int px);

#endif
