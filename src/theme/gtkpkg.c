/* SPDX-License-Identifier: MIT
 * GTK3 widgets drawn with a theme package. See gtkpkg.h.
 *
 * Each widget's part is drawn once per state by the package's renderer
 * at its natural size (the image's frame), then given to GTK as a border
 * image cut at the part's SizingMargins, so GTK stretches the edges and
 * keeps the corners as uxtheme does. GTK 3 never fills a border image's
 * middle: the image's inside is the background within the CSS border (see
 * decls()). TrueSize parts (check boxes, radio buttons, slider thumbs,
 * scroll arrows) are drawn at their size and shown as they are. Where the
 * layout differs from the image (a 23-pixel button around 9-pixel
 * margins), the border image is wider than the CSS border, which then
 * only sets how far the content is inset (the part's ContentMargins).
 */
#include "gtkpkg.h"

#include <math.h>
#include <string.h>

#include "pkg.h"

typedef struct ctx {
    const xd_theme *t;
    xd_strbuf *o;
    GArray *images;
    /* An image centred over the next part instead of its glyph (a scroll
     * thumb's gripper), with its size. */
    const char *overlay;
    int overlay_w, overlay_h;
} ctx;

static int lpx(const ctx *x, int device) {
    return (int)lround(device / x->t->scale);
}

static void color(xd_strbuf *o, xd_rgba c) {
    unsigned r = (unsigned)lround(CLAMP(c.r, 0.0, 1.0) * 255);
    unsigned g = (unsigned)lround(CLAMP(c.g, 0.0, 1.0) * 255);
    unsigned b = (unsigned)lround(CLAMP(c.b, 0.0, 1.0) * 255);
    xd_strbuf_appendf(o, "#%02x%02x%02x", r, g, b);
}

static bool enum_is(const ctx *x, const char *cls, const char *part, const char *state,
                    const char *prop, const char *value) {
    const char *v = xd_pkg_prop(x->t->pkg, cls, part, state, prop);
    return v != nullptr && g_ascii_strcasecmp(v, value) == 0;
}

static bool border_fill(const ctx *x, const char *cls, const char *part, const char *state) {
    return enum_is(x, cls, part, state, "BgType", "BorderFill");
}

/* A part's frame at its natural size, in logical px. */
static bool natural(const ctx *x, const char *cls, const char *part, const char *state, int *w,
                    int *h) {
    int fw, fh;
    if (!xd_pkg_part_size(x->t, cls, part, state, &fw, &fh))
        return false;
    *w = MAX(1, lpx(x, fw));
    *h = MAX(1, lpx(x, fh));
    return true;
}

static bool glyph_size(const ctx *x, const char *cls, const char *part, const char *state,
                       int *w, int *h) {
    int gw, gh;
    if (!xd_pkg_glyph_size(x->t, cls, part, state, &gw, &gh))
        return false;
    *w = MAX(1, lpx(x, gw));
    *h = MAX(1, lpx(x, gh));
    return true;
}

/* Whether the package draws the part: an image that loads, or a border
 * and fill. */
static bool styles(const ctx *x, const char *cls, const char *part, const char *state) {
    if (x->t->pkg == nullptr || !xd_pkg_has(x->t->pkg, cls, part) ||
        enum_is(x, cls, part, state, "BgType", "None"))
        return false;
    int w, h;
    return border_fill(x, cls, part, state) || natural(x, cls, part, state, &w, &h);
}

/* The part's SizingMargins, within its frame. */
static xd_insets sizing(const ctx *x, const char *cls, const char *part, const char *state,
                        int w, int h) {
    xd_insets m = {0};
    (void)xd_pkg_margins(x->t->pkg, cls, part, state, "SizingMargins", &m);
    m.left = CLAMP(m.left, 0, w);
    m.right = CLAMP(m.right, 0, w - m.left);
    m.top = CLAMP(m.top, 0, h);
    m.bottom = CLAMP(m.bottom, 0, h - m.top);
    return m;
}

static xd_insets content(const ctx *x, const char *cls, const char *part, const char *state,
                         int fallback) {
    xd_insets m = {fallback, fallback, fallback, fallback};
    (void)xd_pkg_margins(x->t->pkg, cls, part, state, "ContentMargins", &m);
    m.left = MAX(m.left, 0);
    m.right = MAX(m.right, 0);
    m.top = MAX(m.top, 0);
    m.bottom = MAX(m.bottom, 0);
    return m;
}

/* ---- the images -------------------------------------------------------------------- */

typedef struct src {
    char s[320];
} src;

/* The CSS image for one of the images, which is listed for writing. */
static src image(ctx *x, xd_gtk_pkg_layer layer, const char *cls, const char *part,
                 const char *state, const char *suffix, int w, int h, xd_rect part_rect) {
    xd_gtk_pkg_image img = {.layer = layer, .w = w, .h = h, .part_rect = part_rect};
    g_strlcpy(img.cls, cls, sizeof(img.cls));
    g_strlcpy(img.part, part != nullptr ? part : "", sizeof(img.part));
    g_strlcpy(img.state, state != nullptr ? state : "", sizeof(img.state));
    static const char *const layers[] = {"w", "b", "g"};
    char *raw = g_strdup_printf("%s-%s-%s-%s%s%s", cls, img.part, img.state, layers[layer],
                                suffix != nullptr ? "-" : "", suffix != nullptr ? suffix : "");
    /* Lowercase letters, digits and dashes ("TaskBand::Toolbar"). */
    size_t n = 0;
    for (const char *p = raw; *p != '\0' && n + 1 < sizeof(img.name); p++) {
        char c = g_ascii_tolower(*p);
        if (!g_ascii_isalnum(c))
            c = '-';
        if (c == '-' && n > 0 && img.name[n - 1] == '-')
            continue;
        img.name[n++] = c;
    }
    img.name[n] = '\0';
    g_free(raw);
    if (x->images != nullptr) {
        bool have = false;
        for (guint i = 0; i < x->images->len && !have; i++)
            have = strcmp(g_array_index(x->images, xd_gtk_pkg_image, i).name, img.name) == 0;
        if (!have)
            g_array_append_val(x->images, img);
    }
    src out;
    g_snprintf(out.s, sizeof(out.s),
               "-gtk-scaled(url(\"" XD_GTK_PKG_ASSETS "/%s.png\"), url(\"" XD_GTK_PKG_ASSETS
               "/%s@2.png\"))",
               img.name, img.name);
    return out;
}

/* The part's frame in the state, as it is. */
static src frame_image(ctx *x, const char *cls, const char *part, const char *state,
                       xd_gtk_pkg_layer layer, int w, int h) {
    return image(x, layer, cls, part, state, nullptr, w, h, (xd_rect){0, 0, w, h});
}

cairo_surface_t *xd_gtk_pkg_image_draw(const xd_theme *t, const xd_gtk_pkg_image *img) {
    if (t->pkg == nullptr || img->w <= 0 || img->h <= 0)
        return nullptr;
    double s = t->scale;
    int w = (int)lround(img->w * s), h = (int)lround(img->h * s);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    xd_rect r = {(int)lround(img->part_rect.x * s), (int)lround(img->part_rect.y * s),
                 (int)lround(img->part_rect.w * s), (int)lround(img->part_rect.h * s)};
    const char *part = img->part[0] != '\0' ? img->part : nullptr;
    const char *state = img->state[0] != '\0' ? img->state : nullptr;
    bool drew = false;
    switch (img->layer) {
    case XD_GTK_PKG_WHOLE:
        drew = xd_pkg_draw(t, cr, img->cls, part, state, r);
        break;
    case XD_GTK_PKG_BACKGROUND:
        drew = xd_pkg_draw_background(t, cr, img->cls, part, state, r);
        break;
    case XD_GTK_PKG_GLYPH:
        drew = xd_pkg_draw_glyph(t, cr, img->cls, part, state, r);
        break;
    }
    cairo_destroy(cr);
    if (!drew) {
        cairo_surface_destroy(surf);
        return nullptr;
    }
    cairo_surface_flush(surf);
    return surf;
}

/* ---- rules ------------------------------------------------------------------------- */

/* Every base in `bases` with every suffix in `suffixes` ('|' between
 * them): a '%' in a base is where the suffix goes, else its end. */
static void cross(xd_strbuf *o, const char *bases, const char *suffixes) {
    char **b = g_strsplit(bases, "|", -1);
    /* g_strsplit gives "" no pieces; it's one empty suffix here. */
    char **s = g_strsplit(suffixes, "|", -1);
    if (s[0] == nullptr) {
        g_free(s);
        s = g_new0(char *, 2);
        s[0] = g_strdup("");
    }
    bool first = true;
    for (char **bi = b; *bi != nullptr; bi++)
        for (char **si = s; *si != nullptr; si++) {
            if (!first)
                xd_strbuf_append(o, XD_STR(",\n"));
            first = false;
            const char *pct = strchr(*bi, '%');
            if (pct != nullptr)
                xd_strbuf_appendf(o, "%.*s%s%s", (int)(pct - *bi), *bi, *si, pct + 1);
            else
                xd_strbuf_appendf(o, "%s%s", *bi, *si);
        }
    g_strfreev(s);
    g_strfreev(b);
}

static void text_color(ctx *x, const char *cls, const char *part, const char *state) {
    xd_rgba c;
    if (xd_pkg_color(x->t->pkg, cls, part, state, "TextColor", &c)) {
        xd_strbuf_append(x->o, XD_STR("  color: "));
        color(x->o, c);
        xd_strbuf_append(x->o, XD_STR(";\n"));
    }
}

enum {
    GLYPH_LAYER = 1u << 0, /* the image glyph centred over the background */
    NO_TEXT = 1u << 1,     /* not the part's TextColor */
    EXACT_H = 1u << 2,     /* CSS borders left and right: the SizingMargins */
    EXACT_V = 1u << 3,     /* top and bottom */
    EXACT = EXACT_H | EXACT_V,
};

/* The CSS border widths for an image part: its SizingMargins on the sides
 * `flags` makes exact, else its ContentMargins (3 without, never more than
 * the SizingMargins), so the content goes where Windows puts it. */
static xd_insets borders(const ctx *x, const char *cls, const char *part, const char *state,
                         xd_insets m, unsigned flags) {
    xd_insets c = content(x, cls, part, state, 3);
    xd_insets b = m;
    if (!(flags & EXACT_H)) {
        b.left = MIN(c.left, m.left);
        b.right = MIN(c.right, m.right);
    }
    if (!(flags & EXACT_V)) {
        b.top = MIN(c.top, m.top);
        b.bottom = MIN(c.bottom, m.bottom);
    }
    return b;
}

/* The part's look in the state, as declarations: a border and fill, or
 * its image as a border image cut at its SizingMargins, the middle (which
 * GTK leaves empty) the image's middle as the background inside the CSS
 * borders. Where those are the margins, that's exactly uxtheme's 9-grid;
 * elsewhere it's exact at the image's own size and close to it around
 * that. A corner is never under the middle: one of its sides is exact. */
static bool decls(ctx *x, const char *cls, const char *part, const char *state, unsigned flags) {
    const xd_pkg *p = x->t->pkg;
    xd_strbuf *o = x->o;
    if (border_fill(x, cls, part, state)) {
        int border = 1;
        (void)xd_pkg_int(p, cls, part, state, "BorderSize", &border);
        xd_rgba bc, fc, g1, g2;
        xd_strbuf_append(o, XD_STR("  border-image: none;\n  box-shadow: none;\n"
                                   "  border-radius: 0;\n"));
        if (xd_pkg_color(p, cls, part, state, "BorderColor", &bc) && border > 0) {
            xd_strbuf_appendf(o, "  border-style: solid;\n  border-width: %dpx;\n  border-color: ",
                              border);
            color(o, bc);
            xd_strbuf_append(o, XD_STR(";\n"));
        } else {
            xd_strbuf_append(o, XD_STR("  border-style: none;\n"));
        }
        if (!enum_is(x, cls, part, state, "FillType", "Solid") &&
            xd_pkg_color(p, cls, part, state, "GradientColor1", &g1) &&
            xd_pkg_color(p, cls, part, state, "GradientColor2", &g2)) {
            xd_strbuf_appendf(o, "  background-image: linear-gradient(%s",
                              enum_is(x, cls, part, state, "FillType", "HorzGradient")
                                  ? "to right, "
                                  : "");
            color(o, g1);
            xd_strbuf_append(o, XD_STR(", "));
            color(o, g2);
            xd_strbuf_append(o, XD_STR(");\n"));
        } else {
            xd_strbuf_append(o, XD_STR("  background-image: none;\n"));
        }
        if (xd_pkg_color(p, cls, part, state, "FillColor", &fc)) {
            xd_strbuf_append(o, XD_STR("  background-color: "));
            color(o, fc);
            xd_strbuf_append(o, XD_STR(";\n"));
        }
        if (!(flags & NO_TEXT))
            text_color(x, cls, part, state);
        return true;
    }
    int w, h;
    if (!natural(x, cls, part, state, &w, &h))
        return false;
    xd_insets m = sizing(x, cls, part, state, w, h);
    xd_insets b = borders(x, cls, part, state, m, flags);
    bool tile = enum_is(x, cls, part, state, "SizingType", "Tile");
    bool border_only = false;
    (void)xd_pkg_bool(p, cls, part, state, "BorderOnly", &border_only);
    xd_strbuf_appendf(o, "  border-style: solid;\n  border-width: %dpx %dpx %dpx %dpx;\n",
                      b.top, b.right, b.bottom, b.left);
    if (m.left + m.right + m.top + m.bottom > 0) {
        src img = frame_image(x, cls, part, state, XD_GTK_PKG_BACKGROUND, w, h);
        xd_strbuf_appendf(o,
                          "  border-image: %s %d %d %d %d %s;\n"
                          "  border-image-width: %dpx %dpx %dpx %dpx;\n",
                          img.s, m.top, m.right, m.bottom, m.left, tile ? "repeat" : "stretch",
                          m.top, m.right, m.bottom, m.left);
    } else {
        xd_strbuf_append(o, XD_STR("  border-image: none;\n"));
    }
    /* The image inside the CSS borders, stretched; for Tile, the middle
     * alone, tiled. */
    xd_insets cut = tile ? m : b;
    /* GTK scales an image with its edges fading out over half a source
     * pixel, which across a 4-pixel middle stretched to 70 is a visible
     * band: along an exact axis (only the part's own middle is there) it's
     * drawn larger to begin with, as the part would be. */
    int pw = w, ph = h;
    if (!tile && (flags & EXACT_H))
        pw = MAX(w, cut.left + cut.right + 64);
    if (!tile && (flags & EXACT_V))
        ph = MAX(h, cut.top + cut.bottom + 64);
    int mw = pw - cut.left - cut.right, mh = ph - cut.top - cut.bottom;
    bool middle = !border_only && w - cut.left - cut.right > 0 && h - cut.top - cut.bottom > 0;
    src mid = {0};
    if (middle) {
        char suffix[64];
        g_snprintf(suffix, sizeof(suffix), "m%d-%d-%d-%d-%dx%d", cut.top, cut.right, cut.bottom,
                   cut.left, mw, mh);
        mid = image(x, XD_GTK_PKG_BACKGROUND, cls, part, state, suffix, mw, mh,
                    (xd_rect){-cut.left, -cut.top, pw, ph});
    }
    const char *mid_size = tile ? "auto" : "100% 100%";
    const char *mid_repeat = tile ? "repeat" : "no-repeat";
    int gw = x->overlay_w, gh = x->overlay_h;
    bool glyph = x->overlay != nullptr ||
                 ((flags & GLYPH_LAYER) && glyph_size(x, cls, part, state, &gw, &gh));
    if (glyph) {
        /* Drawn at its size: the content area around it is the part's. */
        src g;
        if (x->overlay != nullptr) {
            g_strlcpy(g.s, x->overlay, sizeof(g.s));
        } else {
            xd_insets c = content(x, cls, part, state, 0);
            g = image(x, XD_GTK_PKG_GLYPH, cls, part, state, nullptr, gw, gh,
                      (xd_rect){-c.left, -c.top, gw + c.left + c.right, gh + c.top + c.bottom});
        }
        if (middle)
            xd_strbuf_appendf(o, "  background-image: %s, %s;\n"
                                 "  background-size: %dpx %dpx, %s;\n"
                                 "  background-position: center, left top;\n"
                                 "  background-repeat: no-repeat, %s;\n",
                              g.s, mid.s, gw, gh, mid_size, mid_repeat);
        else
            xd_strbuf_appendf(o, "  background-image: %s;\n  background-size: %dpx %dpx;\n"
                                 "  background-position: center;\n"
                                 "  background-repeat: no-repeat;\n",
                              g.s, gw, gh);
    } else if (middle) {
        xd_strbuf_appendf(o, "  background-image: %s;\n  background-size: %s;\n"
                             "  background-position: left top;\n  background-repeat: %s;\n",
                          mid.s, mid_size, mid_repeat);
    } else {
        xd_strbuf_append(o, XD_STR("  background-image: none;\n"));
    }
    xd_strbuf_append(o, XD_STR("  background-origin: padding-box;\n"
                               "  background-clip: padding-box;\n  background-color: transparent;\n"
                               "  box-shadow: none;\n  border-radius: 0;\n  text-shadow: none;\n"));
    if (!(flags & NO_TEXT))
        text_color(x, cls, part, state);
    return true;
}

/* A rule for the selectors: the part's look in the state, then `extra`
 * (declarations, may be nullptr). False if the package doesn't draw it. */
static bool rule(ctx *x, const char *bases, const char *suffixes, const char *cls,
                 const char *part, const char *state, unsigned flags, const char *extra) {
    if (!styles(x, cls, part, state))
        return false;
    cross(x->o, bases, suffixes);
    xd_strbuf_append(x->o, XD_STR(" {\n"));
    bool ok = decls(x, cls, part, state, flags);
    if (extra != nullptr)
        xd_strbuf_append(x->o, xd_str_from_cstr(extra));
    xd_strbuf_append(x->o, XD_STR("}\n"));
    return ok;
}

/* A TrueSize part (or one shown whole, glyph and all) at `w` x `h` as the
 * background, nothing else. */
static bool whole_rule(ctx *x, const char *bases, const char *suffixes, const char *cls,
                       const char *part, const char *state, int w, int h, const char *extra) {
    if (!styles(x, cls, part, state))
        return false;
    src img = frame_image(x, cls, part, state, XD_GTK_PKG_WHOLE, w, h);
    cross(x->o, bases, suffixes);
    xd_strbuf_appendf(x->o,
                      " {\n  background-image: %s;\n  background-size: 100%% 100%%;\n"
                      "  background-repeat: no-repeat;\n  background-color: transparent;\n"
                      "  border-image: none;\n  border: none;\n  box-shadow: none;\n"
                      "  border-radius: 0;\n%s}\n",
                      img.s, extra != nullptr ? extra : "");
    return true;
}

/* One rule per state: {suffixes, state number}. */
typedef struct state_map {
    const char *suffixes;
    const char *state;
} state_map;

static void states(ctx *x, const char *bases, const state_map *map, size_t n, const char *cls,
                   const char *part, unsigned flags, const char *extra) {
    for (size_t i = 0; i < n; i++)
        (void)rule(x, bases, map[i].suffixes, cls, part, map[i].state, flags, extra);
}

/* ---- widgets ----------------------------------------------------------------------- */

/* Push buttons (Button/PushButton): 75x23 at least for text, as Windows'
 * dialog buttons; the label inset by the ContentMargins. The default (and
 * focused) one is drawn Defaulted, as XP does. */
#define PUSH                                                                                   \
    "button.text-button:not(.flat):not(.combo)|button.image-button:not(.flat):not(.combo)"
static void push_buttons(ctx *x) {
    int w, h;
    if (!styles(x, "Button", "PushButton", "1") || border_fill(x, "Button", "PushButton", "1") ||
        !natural(x, "Button", "PushButton", "1", &w, &h))
        return;
    xd_insets b = borders(x, "Button", "PushButton", "1", sizing(x, "Button", "PushButton", "1", w, h),
                          EXACT_H);
    int min_h = MAX(23 - b.top - b.bottom, 0);
    xd_strbuf_appendf(x->o,
                      "%s {\n  padding: 0 1px;\n  min-height: %dpx;\n  min-width: 0;\n}\n"
                      "button.text-button:not(.flat):not(.combo) { min-width: %dpx; }\n",
                      "button.text-button:not(.flat):not(.combo),\n"
                      "button.image-button:not(.flat):not(.combo)",
                      min_h, MAX(75 - b.left - b.right - 2, 0));
    static const state_map map[] = {
        {"", "1"},
        {":focus|.default", "5"},
        {":hover", "2"},
        {":active|:checked", "3"},
        {":disabled", "4"},
    };
    states(x, PUSH, map, G_N_ELEMENTS(map), "Button", "PushButton", EXACT_H, nullptr);
    /* Pressed: the label one pixel down and right, as in Windows. */
    cross(x->o, PUSH, ":active");
    xd_strbuf_appendf(x->o, " {\n  padding: 1px 0 0 2px;\n  min-height: %dpx;\n}\n",
                      MAX(min_h - 1, 0));
    xd_rgba d;
    if (xd_pkg_color(x->t->pkg, "Button", "PushButton", "4", "TextColor", &d)) {
        cross(x->o, PUSH, ":disabled label");
        xd_strbuf_append(x->o, XD_STR(" { color: "));
        color(x->o, d);
        xd_strbuf_append(x->o, XD_STR("; }\n"));
    }
    /* Toolbars keep flat buttons. */
    xd_strbuf_append(x->o, XD_STR("toolbar button.text-button:not(.flat):not(.combo),\n"
                                  "toolbar button.image-button:not(.flat):not(.combo) {\n"
                                  "  border-image: none;\n  background-image: none;\n"
                                  "  border-width: 1px;\n  padding: 2px 4px;\n"
                                  "  min-height: 0;\n  min-width: 0;\n}\n"));
}
#undef PUSH

/* Check boxes and radio buttons: TrueSize images, shown as they are. Not
 * in menus, which show only the mark (the base style's). */
static void indicators(ctx *x, const char *part, const char *bases, bool mixed) {
    int w, h;
    if (!styles(x, "Button", part, "1") || !natural(x, "Button", part, "1", &w, &h))
        return;
    static const char *const kinds[] = {"", ":checked", ":indeterminate"};
    static const char *const moods[] = {"", ":hover", ":active", ":disabled"};
    for (int k = 0; k < (mixed ? 3 : 2); k++)
        for (int m = 0; m < 4; m++) {
            char state[8];
            g_snprintf(state, sizeof(state), "%d", k * 4 + m + 1);
            char suffix[48];
            g_snprintf(suffix, sizeof(suffix), "%s%s", kinds[k], moods[m]);
            if (!styles(x, "Button", part, state))
                continue;
            src img = frame_image(x, "Button", part, state, XD_GTK_PKG_WHOLE, w, h);
            cross(x->o, bases, suffix);
            xd_strbuf_appendf(x->o,
                              " {\n  -gtk-icon-source: %s;\n  min-width: %dpx;\n"
                              "  min-height: %dpx;\n  background-image: none;\n"
                              "  background-color: transparent;\n  border: none;\n"
                              "  border-image: none;\n  border-radius: 0;\n  box-shadow: none;\n"
                              "  padding: 0;\n}\n",
                              img.s, w, h);
        }
}

/* Text fields (Edit/EditText). */
static void entries(ctx *x) {
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":focus", "5"}, {":disabled", "4"},
    };
    states(x, "entry|spinbutton", map, G_N_ELEMENTS(map), "Edit", "EditText", EXACT_H, nullptr);
}

/* Combo boxes: the field (ComboBox) and its drop-down button
 * (DropDownButton) on GTK's arrow, with the glyph as the arrow's icon. */
static void combos(ctx *x) {
    static const state_map field[] = {
        {"", "1"}, {":hover", "2"}, {":disabled", "4"},
    };
    states(x, "combobox button.combo", field, G_N_ELEMENTS(field), "ComboBox", nullptr, EXACT_H,
           "  padding: 0 0 0 3px;\n");
    int w, h, gw = 0, gh = 0;
    const char *const DB = "DropDownButton";
    if (!styles(x, "ComboBox", DB, "1") || !natural(x, "ComboBox", DB, "1", &w, &h))
        return;
    (void)glyph_size(x, "ComboBox", DB, "1", &gw, &gh);
    xd_insets m = sizing(x, "ComboBox", DB, "1", w, h);
    /* As wide as the image (Windows' is the scroll bar's width, near it),
     * the glyph a background over it: GTK draws an arrow's icon square. */
    char extra[256];
    g_snprintf(extra, sizeof(extra),
               "  -gtk-icon-source: none;\n  min-width: %dpx;\n  min-height: %dpx;\n"
               "  padding: 0;\n  margin: 0;\n",
               MAX(w - m.left - m.right, gw), gh);
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":active|:checked", "3"}, {":disabled", "4"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(map); i++)
        (void)rule(x, "combobox button.combo% arrow", map[i].suffixes, "ComboBox", DB,
                   map[i].state, GLYPH_LAYER | NO_TEXT | EXACT, extra);
}

/* Scroll bars: the arrow buttons whole, the thumb with its gripper, the
 * track, in both orientations, the scheme's scroll bar width across. */
static void scrollbars(ctx *x) {
    const char *const SB = "ScrollBar";
    if (!styles(x, SB, "ArrowBtn", "1"))
        return;
    int width = xd_metric_px(x->t, XD_METRIC_SCROLL_WIDTH);
    width = MAX(lpx(x, width), 8);
    xd_strbuf_appendf(x->o, "scrollbar { background-color: transparent; background-image: none; }\n"
                            "scrollbar.vertical > contents > trough { min-width: %dpx; }\n"
                            "scrollbar.horizontal > contents > trough { min-height: %dpx; }\n",
                      width, width);
    static const struct {
        const char *orient, *thumb, *gripper, *track;
        const char *arrow_up, *arrow_down; /* first state of four */
    } o[] = {
        {"vertical", "ThumbBtnVert", "GripperVert", "UpperTrackVert", "1", "5"},
        {"horizontal", "ThumbBtnHorz", "GripperHorz", "UpperTrackHorz", "9", "13"},
    };
    /* Windows keeps an arrow enabled at the end of its track (GTK disables
     * it): only a disabled scroll bar's are. */
    static const char *const moods[] = {"", ":hover", ":active", ""};
    for (size_t i = 0; i < G_N_ELEMENTS(o); i++) {
        bool vertical = i == 0;
        for (int b = 0; b < 2; b++) {
            int first = atoi(b == 0 ? o[i].arrow_up : o[i].arrow_down);
            for (int m = 0; m < 4; m++) {
                char state[8], base[96], extra[160];
                g_snprintf(state, sizeof(state), "%d", first + m);
                g_snprintf(base, sizeof(base), "scrollbar.%s%s button.%s", o[i].orient,
                           m == 3 ? ":disabled" : "", b == 0 ? "up" : "down");
                g_snprintf(extra, sizeof(extra),
                           "  min-width: %dpx;\n  min-height: %dpx;\n  padding: 0;\n"
                           "  margin: 0;\n  -gtk-icon-source: none;\n",
                           width, width);
                (void)whole_rule(x, base, moods[m], SB, "ArrowBtn", state, width, width, extra);
            }
        }
        char trough[96];
        g_snprintf(trough, sizeof(trough), "scrollbar.%s trough", o[i].orient);
        (void)rule(x, trough, "", SB, o[i].track, "1", NO_TEXT | EXACT, "  padding: 0;\n");
        g_snprintf(trough, sizeof(trough), "scrollbar.%s:disabled trough", o[i].orient);
        (void)rule(x, trough, "", SB, o[i].track, "4", NO_TEXT | EXACT, nullptr);
        for (int m = 0; m < 4; m++) {
            char state[8], base[96], extra[256];
            g_snprintf(state, sizeof(state), "%d", m + 1);
            g_snprintf(base, sizeof(base), "scrollbar.%s%s slider", o[i].orient,
                       m == 3 ? ":disabled" : "");
            int tw, th;
            if (!natural(x, SB, o[i].thumb, state, &tw, &th))
                continue;
            xd_insets tm = sizing(x, SB, o[i].thumb, state, tw, th);
            int len = 8; /* Windows' smallest thumb */
            int across = width - (vertical ? tm.left + tm.right : tm.top + tm.bottom);
            int along = len - (vertical ? tm.top + tm.bottom : tm.left + tm.right);
            g_snprintf(extra, sizeof(extra),
                       "  margin: 0;\n  padding: 0;\n  min-width: %dpx;\n  min-height: %dpx;\n",
                       MAX(vertical ? across : along, 0), MAX(vertical ? along : across, 0));
            /* The gripper over the thumb, centred, as it is. GTK centres
             * it in the thumb's middle, at half a pixel when their sizes
             * differ by an odd number, which blurs it: across the bar, a
             * pixel more of canvas puts it where Windows does. */
            int gw, gh;
            src g = {0};
            if (styles(x, SB, o[i].gripper, state) && natural(x, SB, o[i].gripper, state, &gw, &gh)) {
                int space = vertical ? width - tm.left - tm.right : width - tm.top - tm.bottom;
                int size = vertical ? gw : gh;
                int canvas = size + ((space - size) & 1);
                int want = (width - size) / 2 - (vertical ? tm.left : tm.top);
                int at = want - (space - canvas) / 2;
                g = vertical ? image(x, XD_GTK_PKG_WHOLE, SB, o[i].gripper, state, "c", canvas, gh,
                                     (xd_rect){at, 0, gw, gh})
                             : image(x, XD_GTK_PKG_WHOLE, SB, o[i].gripper, state, "c", gw, canvas,
                                     (xd_rect){0, at, gw, gh});
                x->overlay = g.s;
                x->overlay_w = vertical ? canvas : gw;
                x->overlay_h = vertical ? gh : canvas;
            }
            (void)rule(x, base, moods[m], SB, o[i].thumb, state, NO_TEXT | EXACT, extra);
            x->overlay = nullptr;
        }
    }
}

/* Tabs on top (Tab/TabItem, TopTabItem for the selected one, the
 * LeftEdge variants for the first) and the page (Pane's edges, Body's
 * fill). The page's top edge is the header's bottom border, so the
 * selected tab, 2px taller, can cover it and join the page. */
static void tabs(ctx *x) {
    const char *const TAB = "Tab";
    int pw, ph;
    bool pane = styles(x, TAB, "Pane", "1") && natural(x, TAB, "Pane", "1", &pw, &ph) &&
                !border_fill(x, TAB, "Pane", "1");
    xd_insets pm = pane ? sizing(x, TAB, "Pane", "1", pw, ph) : (xd_insets){0};
    if (pane) {
        /* The pane's top edge, without its corners, along the header. */
        int ew = MAX(pw - pm.left - pm.right, 1);
        if (pm.top > 0) {
            src top = image(x, XD_GTK_PKG_BACKGROUND, TAB, "Pane", "1", "top", ew, pm.top,
                            (xd_rect){-pm.left, 0, pw, ph});
            xd_strbuf_appendf(x->o,
                              "notebook > header.top {\n  border-style: solid;\n"
                              "  border-width: 0 0 %dpx 0;\n"
                              "  border-image: %s 0 0 %d 0 stretch;\n"
                              "  border-image-width: 0 0 %dpx 0;\n  box-shadow: none;\n"
                              "  background-image: none;\n  padding: 0 2px;\n}\n",
                              pm.top, top.s, pm.top, pm.top);
        }
        src edges = frame_image(x, TAB, "Pane", "1", XD_GTK_PKG_BACKGROUND, pw, ph);
        xd_strbuf_appendf(x->o,
                          "notebook > header.top + stack {\n  border-style: solid;\n"
                          "  border-width: 0 %dpx %dpx %dpx;\n"
                          "  border-image: %s %d %d %d %d stretch;\n"
                          "  border-image-width: 0 %dpx %dpx %dpx;\n  box-shadow: none;\n"
                          "  border-radius: 0;\n  background-origin: padding-box;\n"
                          "  background-clip: padding-box;\n",
                          pm.right, pm.bottom, pm.left, edges.s, pm.top, pm.right, pm.bottom,
                          pm.left, pm.right, pm.bottom, pm.left);
        int bw, bh;
        xd_rgba hint;
        if (styles(x, TAB, "Body", "1") && natural(x, TAB, "Body", "1", &bw, &bh)) {
            /* Wide to begin with (XP's is 10 pixels): see decls(). */
            bw = MAX(bw, 256);
            src body = image(x, XD_GTK_PKG_BACKGROUND, TAB, "Body", "1", "wide", bw, bh,
                             (xd_rect){0, 0, bw, bh});
            xd_strbuf_appendf(x->o, "  background-image: %s;\n  background-size: 100%% 100%%;\n"
                                    "  background-repeat: no-repeat;\n",
                              body.s);
        } else {
            xd_strbuf_appendf(x->o, "  background-image: %s;\n  background-size: 100%% 100%%;\n",
                              edges.s);
        }
        if (xd_pkg_color(x->t->pkg, TAB, "Body", nullptr, "FillColorHint", &hint) ||
            xd_pkg_color(x->t->pkg, TAB, "Pane", nullptr, "FillColorHint", &hint)) {
            xd_strbuf_append(x->o, XD_STR("  background-color: "));
            color(x->o, hint);
            xd_strbuf_append(x->o, XD_STR(";\n"));
        }
        xd_strbuf_append(x->o, XD_STR("}\n"));
    }
    int w, h;
    if (!styles(x, TAB, "TabItem", "1") || border_fill(x, TAB, "TabItem", "1") ||
        !natural(x, TAB, "TabItem", "1", &w, &h))
        return;
    /* The tabs as tall as the image (18 in XP), the selected one 2px
     * taller and over the page's edge. */
    xd_insets b = borders(x, TAB, "TabItem", "1", sizing(x, TAB, "TabItem", "1", w, h), EXACT_H);
    int inner = MAX(h - b.top - b.bottom - 4, 0);
    const char *const T = "notebook > header.top > tabs > tab";
    xd_strbuf_appendf(x->o,
                      "%s {\n  margin: 2px 0 0 0;\n  padding: 2px 1px;\n  min-height: %dpx;\n"
                      "  outline-offset: -3px;\n}\n"
                      "%s:checked {\n  margin: 0 0 -%dpx 0;\n  padding: 2px 1px %dpx 1px;\n}\n",
                      T, inner, T, pm.top, 4 + pm.top);
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":disabled", "4"},
    };
    bool top = styles(x, TAB, "TopTabItem", "3");
    bool left = styles(x, TAB, "TabItemLeftEdge", "1");
    bool top_left = styles(x, TAB, "TopTabItemLeftEdge", "3");
    states(x, T, map, G_N_ELEMENTS(map), TAB, "TabItem", EXACT_H, nullptr);
    if (left)
        states(x, "notebook > header.top > tabs > tab:first-child", map, G_N_ELEMENTS(map), TAB,
               "TabItemLeftEdge", EXACT_H, nullptr);
    (void)rule(x, T, ":checked", TAB, top ? "TopTabItem" : "TabItem", "3", EXACT_H, nullptr);
    if (top_left || left)
        (void)rule(x, "notebook > header.top > tabs > tab:first-child", ":checked", TAB,
                   top_left ? "TopTabItemLeftEdge" : "TabItemLeftEdge", "3", EXACT_H, nullptr);
}

/* Group boxes (Button/GroupBox): the border alone, the caption in the
 * part's text colour. Status bar panes (.xd-sunken) keep theirs. */
static void group_boxes(ctx *x) {
    if (!styles(x, "Button", "GroupBox", "1") || border_fill(x, "Button", "GroupBox", "1"))
        return;
    (void)rule(x, "frame:not(.xd-sunken) > border", "", "Button", "GroupBox", "1", NO_TEXT | EXACT,
               nullptr);
    xd_rgba c;
    if (xd_pkg_color(x->t->pkg, "Button", "GroupBox", "1", "TextColor", &c)) {
        xd_strbuf_append(x->o, XD_STR("frame:not(.xd-sunken) > border + label { color: "));
        color(x->o, c);
        xd_strbuf_append(x->o, XD_STR("; }\n"));
    }
}

/* Progress bars: the track (Bar) and its chunks (Chunk), ProgressChunkSize
 * wide with ProgressSpaceSize between, as Windows draws them; inside the
 * track's sizing margins (it has no content margins in XP's). */
static void progress_bars(ctx *x) {
    static const struct {
        const char *orient, *bar, *chunk;
        bool vertical;
    } o[] = {{"horizontal", "Bar", "Chunk", false}, {"vertical", "BarVert", "ChunkVert", true}};
    for (size_t i = 0; i < G_N_ELEMENTS(o); i++) {
        int w, h;
        if (!styles(x, "Progress", o[i].bar, "1") || !natural(x, "Progress", o[i].bar, "1", &w, &h))
            continue;
        xd_insets m = border_fill(x, "Progress", o[i].bar, "1")
                          ? (xd_insets){1, 1, 1, 1}
                          : sizing(x, "Progress", o[i].bar, "1", w, h);
        char sel[96], extra[256];
        g_snprintf(sel, sizeof(sel), "progressbar.%s > trough", o[i].orient);
        int across = o[i].vertical ? MAX(w - m.left - m.right, 1) : MAX(h - m.top - m.bottom, 1);
        g_snprintf(extra, sizeof(extra), "  padding: 0;\n  %s: %dpx;\n",
                   o[i].vertical ? "min-width" : "min-height", across);
        (void)rule(x, sel, "", "Progress", o[i].bar, "1", NO_TEXT | EXACT, extra);
        int cw, ch;
        if (!styles(x, "Progress", o[i].chunk, "1") ||
            !natural(x, "Progress", o[i].chunk, "1", &cw, &ch))
            continue;
        int size = o[i].vertical ? ch : cw, space = 0;
        (void)xd_pkg_int(x->t->pkg, "Progress", o[i].chunk, "1", "ProgressChunkSize", &size);
        (void)xd_pkg_int(x->t->pkg, "Progress", o[i].chunk, "1", "ProgressSpaceSize", &space);
        size = CLAMP(size, 1, 256);
        space = CLAMP(space, 0, 256);
        /* One chunk and the space after it (before it, upwards). */
        src img = o[i].vertical
                      ? image(x, XD_GTK_PKG_BACKGROUND, "Progress", o[i].chunk, "1", "step", cw,
                              size + space, (xd_rect){0, space, cw, size})
                      : image(x, XD_GTK_PKG_BACKGROUND, "Progress", o[i].chunk, "1", "step",
                              size + space, ch, (xd_rect){0, 0, size, ch});
        xd_strbuf_appendf(x->o,
                          "progressbar.%s > trough > progress {\n  background-image: %s;\n"
                          "  background-repeat: %s;\n  background-size: %s;\n"
                          "  background-position: %s;\n  background-color: transparent;\n"
                          "  border: none;\n  border-image: none;\n  border-radius: 0;\n"
                          "  box-shadow: none;\n  %s: %dpx;\n}\n",
                          o[i].orient, img.s, o[i].vertical ? "repeat-y" : "repeat-x",
                          o[i].vertical ? "100% auto" : "auto 100%",
                          o[i].vertical ? "left bottom" : "left top",
                          o[i].vertical ? "min-width" : "min-height", across);
    }
}

/* Sliders (TrackBar): the track, and the thumb whole: rectangular, or
 * pointed towards the marks when the scale has them on one side. */
static void scales(ctx *x) {
    const char *const TB = "TrackBar";
    static const struct {
        const char *orient, *track, *thumb, *before, *after;
        bool vertical;
    } o[] = {
        {"horizontal", "Track", "Thumb", "ThumbTop", "ThumbBottom", false},
        {"vertical", "TrackVert", "ThumbVert", "ThumbLeft", "ThumbRight", true},
    };
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":active", "3"}, {":disabled", "5"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(o); i++) {
        int tw, th, across = 0;
        if (styles(x, TB, o[i].track, "1") && natural(x, TB, o[i].track, "1", &tw, &th)) {
            xd_insets m = border_fill(x, TB, o[i].track, "1")
                              ? (xd_insets){1, 1, 1, 1}
                              : sizing(x, TB, o[i].track, "1", tw, th);
            char sel[64], extra[256];
            g_snprintf(sel, sizeof(sel), "scale.%s trough", o[i].orient);
            int inner = o[i].vertical ? MAX(tw - m.left - m.right, 0) : MAX(th - m.top - m.bottom, 0);
            g_snprintf(extra, sizeof(extra), "  padding: 0;\n  min-height: %dpx;\n  min-width: %dpx;\n",
                       o[i].vertical ? 0 : inner, o[i].vertical ? inner : 0);
            (void)rule(x, sel, "", TB, o[i].track, "1", NO_TEXT | EXACT, extra);
            across = inner;
        }
        const char *const thumbs[][2] = {
            {o[i].thumb, ""}, {o[i].before, ".marks-before:not(.marks-after)"},
            {o[i].after, ".marks-after:not(.marks-before)"},
        };
        for (size_t k = 0; k < G_N_ELEMENTS(thumbs); k++) {
            int w, h;
            if (!styles(x, TB, thumbs[k][0], "1") || !natural(x, TB, thumbs[k][0], "1", &w, &h))
                continue;
            char base[128], extra[256];
            g_snprintf(base, sizeof(base), "scale.%s%s slider", o[i].orient, thumbs[k][1]);
            /* Centred across the track, overhanging it. */
            int over = o[i].vertical ? (w - across) / 2 : (h - across) / 2;
            g_snprintf(extra, sizeof(extra),
                       "  min-width: %dpx;\n  min-height: %dpx;\n  padding: 0;\n"
                       "  margin: %s%dpx%s;\n",
                       w, h, o[i].vertical ? "0 " : "", -MAX(over, 0), o[i].vertical ? "" : " 0");
            for (size_t s = 0; s < G_N_ELEMENTS(map); s++)
                (void)whole_rule(x, base, map[s].suffixes, TB, thumbs[k][0], map[s].state, w, h,
                                 extra);
        }
    }
}

/* Spin buttons: Spin's Up and Down, each with its glyph. GTK puts them
 * side by side; Windows stacks them, which CSS can't. */
static void spin_buttons(ctx *x) {
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":active", "3"}, {":disabled", "4"},
    };
    static const char *const parts[][2] = {{"Up", "up"}, {"Down", "down"}};
    for (size_t i = 0; i < G_N_ELEMENTS(parts); i++) {
        int w, h;
        if (!styles(x, "Spin", parts[i][0], "1") || !natural(x, "Spin", parts[i][0], "1", &w, &h))
            continue;
        int gw = 0, gh = 0;
        (void)glyph_size(x, "Spin", parts[i][0], "1", &gw, &gh);
        xd_insets m = sizing(x, "Spin", parts[i][0], "1", w, h);
        char base[64], extra[192];
        g_snprintf(base, sizeof(base), "spinbutton:not(.vertical) button.%s", parts[i][1]);
        g_snprintf(extra, sizeof(extra),
                   "  padding: 0;\n  margin: 0;\n  min-width: %dpx;\n  min-height: 0;\n"
                   "  -gtk-icon-transform: scale(0);\n",
                   MAX(w - m.left - m.right, gw));
        states(x, base, map, G_N_ELEMENTS(map), "Spin", parts[i][0], GLYPH_LAYER | NO_TEXT | EXACT,
               extra);
    }
}

/* Column headers (Header/HeaderItem), as tall as the image. */
static void headers(ctx *x) {
    int w, h;
    if (!styles(x, "Header", "HeaderItem", "1") || border_fill(x, "Header", "HeaderItem", "1") ||
        !natural(x, "Header", "HeaderItem", "1", &w, &h))
        return;
    xd_insets b = borders(x, "Header", "HeaderItem", "1",
                          sizing(x, "Header", "HeaderItem", "1", w, h), EXACT_H);
    char extra[192];
    g_snprintf(extra, sizeof(extra), "  padding: 1px 0;\n  min-height: %dpx;\n",
               MAX(h - b.top - b.bottom - 2, 0));
    static const state_map map[] = {{"", "1"}, {":hover", "2"}, {":active", "3"}};
    states(x, "treeview.view header button", map, G_N_ELEMENTS(map), "Header", "HeaderItem",
           EXACT_H, extra);
}

/* Tooltips (Tooltip/Standard), where the package draws them. */
static void tooltips(ctx *x) {
    (void)rule(x, "tooltip.background|tooltip", "", "Tooltip", "Standard", "1", EXACT_H,
               "  padding: 1px 3px;\n");
    xd_rgba c;
    if (xd_pkg_color(x->t->pkg, "Tooltip", "Standard", "1", "TextColor", &c)) {
        xd_strbuf_append(x->o, XD_STR("tooltip * { color: "));
        color(x->o, c);
        xd_strbuf_append(x->o, XD_STR("; }\n"));
    }
}

/* Menus in the package's menu font (its colours come with the toolkit's). */
static void menu_font(ctx *x) {
    const xd_font_spec *f = &x->t->scheme.fonts[XD_FONT_MENU];
    const xd_font_spec *m = &x->t->scheme.fonts[XD_FONT_MESSAGE];
    if (strcmp(f->family, m->family) == 0 && f->size_pt == m->size_pt && f->bold == m->bold)
        return;
    xd_strbuf_append(x->o, XD_STR("menubar, menu, .menu, .context-menu {\n  font-family: "));
    /* Names without quotes or backslashes, so they can't leave the string. */
    char **names = g_strsplit(f->family, ",", -1);
    for (char **n = names; *n != nullptr; n++) {
        char *name = g_strstrip(*n);
        if (n != names)
            xd_strbuf_append(x->o, XD_STR(", "));
        bool generic = strcmp(name, "sans-serif") == 0 || strcmp(name, "serif") == 0 ||
                       strcmp(name, "monospace") == 0;
        if (!generic)
            xd_strbuf_append_char(x->o, '"');
        for (const char *p = name; *p != '\0'; p++)
            if (*p != '"' && *p != '\\')
                xd_strbuf_append_char(x->o, *p);
        if (!generic)
            xd_strbuf_append_char(x->o, '"');
    }
    g_strfreev(names);
    xd_strbuf_appendf(x->o, ";\n  font-size: %dpx;\n  font-weight: %s;\n}\n",
                      (int)lround(f->size_pt * 96.0 / 72.0), f->bold ? "bold" : "normal");
}

static void generate(ctx *x) {
    if (x->t->pkg == nullptr)
        return;
    xd_strbuf_appendf(x->o, "\n/* Theme package \"%s\" (%s), its images scaled %s. */\n",
                      xd_pkg_name(x->t->pkg), xd_pkg_variant(x->t->pkg),
                      xd_image_filter_name(x->t->image_filter));
    push_buttons(x);
    indicators(x, "CheckBox", "checkbutton check", true);
    indicators(x, "RadioButton", "radiobutton radio", false);
    entries(x);
    combos(x);
    scrollbars(x);
    tabs(x);
    group_boxes(x);
    progress_bars(x);
    scales(x);
    spin_buttons(x);
    headers(x);
    tooltips(x);
    menu_font(x);
}

void xd_gtk_pkg_css(const xd_theme *t, xd_strbuf *out) {
    ctx x = {.t = t, .o = out};
    generate(&x);
}

GArray *xd_gtk_pkg_images(const xd_theme *t) {
    xd_strbuf scratch = {0};
    ctx x = {.t = t, .o = &scratch, .images = g_array_new(false, true, sizeof(xd_gtk_pkg_image))};
    generate(&x);
    xd_strbuf_free(&scratch);
    return x.images;
}

void xd_gtk_pkg_toolkit_colors(const xd_theme *t, xd_toolkit_colors *c) {
    if (t->pkg == nullptr)
        return;
    bool flat = t->scheme.flat_menus;
    (void)xd_pkg_sys_bool(t->pkg, "FlatMenus", &flat);
    c->bg = c->face = xd_sys(t, XD_COLOR_BUTTON_FACE);
    c->fg = xd_sys(t, XD_COLOR_BUTTON_TEXT);
    c->base = xd_sys(t, XD_COLOR_WINDOW);
    c->text = xd_sys(t, XD_COLOR_WINDOW_TEXT);
    c->disabled = xd_sys(t, XD_COLOR_GRAY_TEXT);
    c->selected_bg = xd_sys(t, XD_COLOR_HILIGHT);
    c->selected_fg = xd_sys(t, XD_COLOR_HILIGHT_TEXT);
    c->tooltip_bg = xd_sys(t, XD_COLOR_INFO_WINDOW);
    c->tooltip_fg = xd_sys(t, XD_COLOR_INFO_TEXT);
    c->menu_bg = xd_sys(t, XD_COLOR_MENU);
    c->menu_fg = xd_sys(t, XD_COLOR_MENU_TEXT);
    /* XP's flat menus: MenuHilight behind the item, MenuBar behind the bar. */
    c->menu_hot = xd_sys(t, flat ? XD_COLOR_MENU_HILIGHT : XD_COLOR_HILIGHT);
    c->menu_hot_fg = xd_sys(t, XD_COLOR_HILIGHT_TEXT);
    c->menubar_bg = xd_sys(t, flat ? XD_COLOR_MENU_BAR : XD_COLOR_BUTTON_FACE);
    c->menu_border = xd_sys(t, XD_COLOR_BUTTON_SHADOW);
    c->title_bg = xd_sys(t, XD_COLOR_ACTIVE_TITLE);
    c->title_bg2 = xd_sys(t, XD_COLOR_GRADIENT_ACTIVE_TITLE);
    c->title_fg = xd_sys(t, XD_COLOR_TITLE_TEXT);
    c->hilight = xd_sys(t, XD_COLOR_BUTTON_HILIGHT);
    c->light = xd_sys(t, XD_COLOR_BUTTON_LIGHT);
    c->shadow = xd_sys(t, XD_COLOR_BUTTON_SHADOW);
    c->dkshadow = xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW);
    c->scrollbar = xd_sys(t, XD_COLOR_SCROLLBAR);
    /* Fields' and lists' borders, where the package gives them. */
    xd_rgba border;
    if (xd_pkg_color(t->pkg, "Edit", "EditText", "1", "BorderColor", &border))
        c->field_border = border;
}
