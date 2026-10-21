/* SPDX-License-Identifier: MIT
 * Theme packages (.xdtheme, docs/THEMES.md): loading one, looking up its
 * properties in uxtheme's vocabulary, and drawing its parts as uxtheme's
 * DrawThemeBackground does. The theme engine's draw functions use the
 * active package where it styles what they draw and fall back to the
 * package's base style where it doesn't.
 */
#ifndef XD_THEME_PKG_H
#define XD_THEME_PKG_H

#include <glib.h>

#include "theme.h"

typedef struct xd_pkg xd_pkg;

/* Loads the package at `path` (a .xdtheme zip, or a folder holding
 * theme.xml) in variant `variant` (its id; nullptr or unknown: the first),
 * with uxtheme's names from `names_path` (data/theme/uxtheme.ini).
 * nullptr with `error` set when it can't be read. */
xd_pkg *xd_pkg_load(const char *path, const char *variant, const char *names_path,
                    GError **error);
void xd_pkg_free(xd_pkg *pkg);

const char *xd_pkg_name(const xd_pkg *pkg);
const char *xd_pkg_variant(const xd_pkg *pkg);
/* The built-in style drawing what the package doesn't. */
xd_visual_style xd_pkg_base(const xd_pkg *pkg);
/* The package's preferred image filter, if it names one. */
bool xd_pkg_filter(const xd_pkg *pkg, xd_image_filter *out);

/* ---- properties ----
 * `cls` may be "App::Class"; `part` and `state` are names (as Microsoft's
 * "Parts and States" spell them) or numbers, nullptr for none. A property
 * missing for the state is looked up for the part, then the class, then
 * in Class (for App::Class), ClassStyle and globals, as uxtheme does. */
const char *xd_pkg_prop(const xd_pkg *pkg, const char *cls, const char *part,
                        const char *state, const char *prop);
/* Whether the package styles this part at all (any property). */
bool xd_pkg_has(const xd_pkg *pkg, const char *cls, const char *part);
bool xd_pkg_int(const xd_pkg *pkg, const char *cls, const char *part, const char *state,
                const char *prop, int *out);
bool xd_pkg_bool(const xd_pkg *pkg, const char *cls, const char *part, const char *state,
                 const char *prop, bool *out);
bool xd_pkg_color(const xd_pkg *pkg, const char *cls, const char *part, const char *state,
                  const char *prop, xd_rgba *out);
/* MARGINS: left, right, top, bottom (as uxtheme orders them). */
bool xd_pkg_margins(const xd_pkg *pkg, const char *cls, const char *part, const char *state,
                    const char *prop, xd_insets *out);

/* [SysMetrics]: colours by registry name ("ActiveCaption"), sizes
 * ("CaptionBarHeight"), fonts ("CaptionFont": face, points, bold). */
bool xd_pkg_sys_color(const xd_pkg *pkg, const char *name, xd_rgba *out);
bool xd_pkg_sys_int(const xd_pkg *pkg, const char *name, int *out);
bool xd_pkg_sys_bool(const xd_pkg *pkg, const char *name, bool *out); /* "FlatMenus" */
bool xd_pkg_sys_font(const xd_pkg *pkg, const char *name, const char **face, int *points,
                     bool *bold, bool *italic);

/* ---- drawing, at the theme's scale with its image filter ---- */

/* Draws the part's background (an image, or a border and fill) into `r`,
 * and its glyph if it has one. False (nothing drawn) when the package
 * doesn't style the part. */
bool xd_pkg_draw(const xd_theme *t, cairo_t *cr, const char *cls, const char *part,
                 const char *state, xd_rect r);
/* Its two halves: the background alone, and the image glyph alone. */
bool xd_pkg_draw_background(const xd_theme *t, cairo_t *cr, const char *cls, const char *part,
                            const char *state, xd_rect r);
bool xd_pkg_draw_glyph(const xd_theme *t, cairo_t *cr, const char *cls, const char *part,
                       const char *state, xd_rect r);
/* The size of the part's largest image glyph at the theme's scale. */
bool xd_pkg_glyph_size(const xd_theme *t, const char *cls, const char *part,
                       const char *state, int *w, int *h);
/* The size of one state's image at the theme's scale (TrueSize parts:
 * check boxes, caption buttons): false when the part has no image. */
bool xd_pkg_part_size(const xd_theme *t, const char *cls, const char *part,
                      const char *state, int *w, int *h);
/* `r` less the part's ContentMargins, scaled. */
xd_rect xd_pkg_content_rect(const xd_theme *t, const char *cls, const char *part,
                            const char *state, xd_rect r);

/* The package every xd_theme_init() attaches (nullptr: none), and how
 * its images are scaled: `filter` as chosen (the caller resolves
 * "Automatic" to the package's own, else Smooth), whole-number scales
 * exactly with Smooth when `integer_scaling`. The caller keeps `pkg` alive
 * while it's set, and while any theme initialized with it is in use. Once
 * called, $XD_THEME_PACKAGE is no longer looked at: the caller chose. */
void xd_theme_use_package(const xd_pkg *pkg, xd_image_filter filter, bool integer_scaling);
/* xd_theme_init with `pkg` (nullptr: none) instead of the active package:
 * Display Properties' preview of a package before it's applied. */
void xd_theme_init_package(xd_theme *t, xd_visual_style style, const xd_scheme *scheme,
                           double scale, const xd_pkg *pkg, xd_image_filter filter,
                           bool integer_scaling);

/* ---- what a package offers (pkginfo.c) ---- */

typedef struct xd_pkg_variant_info {
    char *id;
    char *name; /* its colours, as "Color scheme" lists it */
    char *size; /* as "Font size" lists it */
} xd_pkg_variant_info;

typedef struct xd_pkg_info {
    char *name; /* theme.xml's; "" if it has none */
    xd_visual_style base;
    bool has_filter;
    xd_image_filter filter;
    size_t variant_count;
    xd_pkg_variant_info *variants; /* in theme.xml's order: the first is the default */
} xd_pkg_info;

/* Reads only theme.xml of the package at `path` (a folder or .xdtheme),
 * for lists of packages. False with `error` set if it isn't a package or
 * has no variant. */
bool xd_pkg_read_info(const char *path, xd_pkg_info *out, GError **error);
void xd_pkg_info_clear(xd_pkg_info *info);

#endif
