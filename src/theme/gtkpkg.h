/* SPDX-License-Identifier: MIT
 * GTK3 and Qt programs drawn with a theme package (docs/THEMES.md), as the
 * XP-styled GTK themes of old were: each widget's part and state rendered
 * once with the package's renderer into a PNG beside gtk.css, then
 * stretched by GTK as uxtheme stretches it (a CSS border image cut at the
 * part's SizingMargins). What the package doesn't style keeps the
 * generated CSS of its base style.
 */
#ifndef XD_THEME_GTKPKG_H
#define XD_THEME_GTKPKG_H

#include <glib.h>

#include "theme.h"

/* Where the images go, relative to gtk.css. */
#define XD_GTK_PKG_ASSETS "assets/pkg"

/* What of a part an image holds. */
typedef enum xd_gtk_pkg_layer : uint8_t {
    XD_GTK_PKG_WHOLE,      /* background and glyph, as drawn in a control */
    XD_GTK_PKG_BACKGROUND, /* the background alone, for a border image */
    XD_GTK_PKG_GLYPH,      /* the image glyph alone */
} xd_gtk_pkg_layer;

/* One image the CSS names: NAME.png at 1x and NAME@2.png at 2x in
 * XD_GTK_PKG_ASSETS. The part is drawn at `part` (logical px) in a
 * `w` x `h` canvas; a part larger than the canvas is cropped. */
typedef struct xd_gtk_pkg_image {
    char name[96];
    char cls[48], part[48], state[8];
    xd_gtk_pkg_layer layer;
    int w, h;
    xd_rect part_rect;
} xd_gtk_pkg_image;

/* The toolkit colours with the package's [SysMetrics] colours (already
 * over t->scheme) in place of the base style's own; nothing without a
 * package. For the GTK CSS and the Qt palette alike. */
void xd_gtk_pkg_toolkit_colors(const xd_theme *t, xd_toolkit_colors *c);

/* The rules drawing GTK widgets with t's package, to go after the base
 * style's (they override it where the package styles a widget); nothing
 * without a package. `t` at scale 1. */
void xd_gtk_pkg_css(const xd_theme *t, xd_strbuf *out);

/* The images those rules name (an array of xd_gtk_pkg_image; empty
 * without a package). g_array_unref it. */
GArray *xd_gtk_pkg_images(const xd_theme *t);

/* One of them drawn at t's scale (an ARGB32 surface of w x h times the
 * scale), or nullptr if the package can't draw it. */
cairo_surface_t *xd_gtk_pkg_image_draw(const xd_theme *t, const xd_gtk_pkg_image *img);

#endif
