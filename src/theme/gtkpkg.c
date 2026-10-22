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

static int lpx(const ctx *generator, int device) {
    return (int)lround(device / generator->t->scale);
}

static void color(xd_strbuf *buffer, xd_rgba rgba) {
    unsigned red = (unsigned)lround(CLAMP(rgba.r, 0.0, 1.0) * 255);
    unsigned green = (unsigned)lround(CLAMP(rgba.g, 0.0, 1.0) * 255);
    unsigned blue = (unsigned)lround(CLAMP(rgba.b, 0.0, 1.0) * 255);
    xd_strbuf_appendf(buffer, "#%02x%02x%02x", red, green, blue);
}

static bool enum_is(const ctx *generator, const char *cls, const char *part, const char *state,
                    const char *prop, const char *value) {
    const char *prop_value = xd_pkg_prop(generator->t->pkg, cls, part, state, prop);
    return prop_value != nullptr && g_ascii_strcasecmp(prop_value, value) == 0;
}

static bool border_fill(const ctx *generator, const char *cls, const char *part,
                        const char *state) {
    return enum_is(generator, cls, part, state, "BgType", "BorderFill");
}

/* A part's frame at its natural size, in logical px. */
static bool natural(const ctx *generator, const char *cls, const char *part, const char *state,
                    int *width, int *height) {
    int fw, fh;
    if (!xd_pkg_part_size(generator->t, cls, part, state, &fw, &fh))
        return false;
    *width = MAX(1, lpx(generator, fw));
    *height = MAX(1, lpx(generator, fh));
    return true;
}

static bool glyph_size(const ctx *generator, const char *cls, const char *part, const char *state,
                       int *width, int *height) {
    int gw, gh;
    if (!xd_pkg_glyph_size(generator->t, cls, part, state, &gw, &gh))
        return false;
    *width = MAX(1, lpx(generator, gw));
    *height = MAX(1, lpx(generator, gh));
    return true;
}

/* Whether the package draws the part: an image that loads, or a border
 * and fill. */
static bool styles(const ctx *generator, const char *cls, const char *part, const char *state) {
    if (generator->t->pkg == nullptr || !xd_pkg_has(generator->t->pkg, cls, part) ||
        enum_is(generator, cls, part, state, "BgType", "None"))
        return false;
    int width, height;
    return border_fill(generator, cls, part, state) ||
           natural(generator, cls, part, state, &width, &height);
}

/* The part's SizingMargins, within its frame. */
static xd_insets sizing(const ctx *generator, const char *cls, const char *part, const char *state,
                        int width, int height) {
    xd_insets insets = {0};
    (void)xd_pkg_margins(generator->t->pkg, cls, part, state, "SizingMargins", &insets);
    insets.left = CLAMP(insets.left, 0, width);
    insets.right = CLAMP(insets.right, 0, width - insets.left);
    insets.top = CLAMP(insets.top, 0, height);
    insets.bottom = CLAMP(insets.bottom, 0, height - insets.top);
    return insets;
}

static xd_insets content(const ctx *generator, const char *cls, const char *part, const char *state,
                         int fallback) {
    xd_insets insets = {fallback, fallback, fallback, fallback};
    (void)xd_pkg_margins(generator->t->pkg, cls, part, state, "ContentMargins", &insets);
    insets.left = MAX(insets.left, 0);
    insets.right = MAX(insets.right, 0);
    insets.top = MAX(insets.top, 0);
    insets.bottom = MAX(insets.bottom, 0);
    return insets;
}

/* ---- the images -------------------------------------------------------------------- */

typedef struct src {
    char s[320];
} src;

/* The CSS image for one of the images, which is listed for writing. */
static src image(ctx *generator, xd_gtk_pkg_layer layer, const char *cls, const char *part,
                 const char *state, const char *suffix, int width, int height, xd_rect part_rect) {
    xd_gtk_pkg_image img = {.layer = layer, .w = width, .h = height, .part_rect = part_rect};
    g_strlcpy(img.cls, cls, sizeof(img.cls));
    g_strlcpy(img.part, part != nullptr ? part : "", sizeof(img.part));
    g_strlcpy(img.state, state != nullptr ? state : "", sizeof(img.state));
    static const char *const layers[] = {"w", "b", "g"};
    char *raw = g_strdup_printf("%s-%s-%s-%s%s%s", cls, img.part, img.state, layers[layer],
                                suffix != nullptr ? "-" : "", suffix != nullptr ? suffix : "");
    /* Lowercase letters, digits and dashes ("TaskBand::Toolbar"). */
    size_t length = 0;
    for (const char *cursor = raw; *cursor != '\0' && length + 1 < sizeof(img.name); cursor++) {
        char lower = g_ascii_tolower(*cursor);
        if (!g_ascii_isalnum(lower))
            lower = '-';
        if (lower == '-' && length > 0 && img.name[length - 1] == '-')
            continue;
        img.name[length++] = lower;
    }
    img.name[length] = '\0';
    g_free(raw);
    if (generator->images != nullptr) {
        bool have = false;
        for (guint i = 0; i < generator->images->len && !have; i++)
            have =
                strcmp(g_array_index(generator->images, xd_gtk_pkg_image, i).name, img.name) == 0;
        if (!have)
            g_array_append_val(generator->images, img);
    }
    src out;
    g_snprintf(out.s, sizeof(out.s),
               "-gtk-scaled(url(\"" XD_GTK_PKG_ASSETS "/%s.png\"), url(\"" XD_GTK_PKG_ASSETS
               "/%s@2.png\"))",
               img.name, img.name);
    return out;
}

/* The part's frame in the state, as it is. */
static src frame_image(ctx *generator, const char *cls, const char *part, const char *state,
                       xd_gtk_pkg_layer layer, int width, int height) {
    return image(generator, layer, cls, part, state, nullptr, width, height,
                 (xd_rect){0, 0, width, height});
}

cairo_surface_t *xd_gtk_pkg_image_draw(const xd_theme *theme, const xd_gtk_pkg_image *img) {
    if (theme->pkg == nullptr || img->w <= 0 || img->h <= 0)
        return nullptr;
    double scale = theme->scale;
    int width = (int)lround(img->w * scale), height = (int)lround(img->h * scale);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cairo = cairo_create(surf);
    xd_rect rect = {(int)lround(img->part_rect.x * scale), (int)lround(img->part_rect.y * scale),
                    (int)lround(img->part_rect.w * scale), (int)lround(img->part_rect.h * scale)};
    const char *part = img->part[0] != '\0' ? img->part : nullptr;
    const char *state = img->state[0] != '\0' ? img->state : nullptr;
    bool drew = false;
    switch (img->layer) {
    case XD_GTK_PKG_WHOLE:
        drew = xd_pkg_draw(theme, cairo, img->cls, part, state, rect);
        break;
    case XD_GTK_PKG_BACKGROUND:
        drew = xd_pkg_draw_background(theme, cairo, img->cls, part, state, rect);
        break;
    case XD_GTK_PKG_GLYPH:
        drew = xd_pkg_draw_glyph(theme, cairo, img->cls, part, state, rect);
        break;
    }
    cairo_destroy(cairo);
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
static void cross(xd_strbuf *buffer, const char *bases, const char *suffixes) {
    char **base_list = g_strsplit(bases, "|", -1);
    /* g_strsplit gives "" no pieces; it's one empty suffix here. */
    char **suffix_list = g_strsplit(suffixes, "|", -1);
    if (suffix_list[0] == nullptr) {
        g_free(suffix_list);
        suffix_list = g_new0(char *, 2);
        suffix_list[0] = g_strdup("");
    }
    bool first = true;
    for (char **bi = base_list; *bi != nullptr; bi++)
        for (char **si = suffix_list; *si != nullptr; si++) {
            if (!first)
                xd_strbuf_append(buffer, XD_STR(",\n"));
            first = false;
            const char *pct = strchr(*bi, '%');
            if (pct != nullptr)
                xd_strbuf_appendf(buffer, "%.*s%s%s", (int)(pct - *bi), *bi, *si, pct + 1);
            else
                xd_strbuf_appendf(buffer, "%s%s", *bi, *si);
        }
    g_strfreev(suffix_list);
    g_strfreev(base_list);
}

static void text_color(ctx *generator, const char *cls, const char *part, const char *state) {
    xd_rgba text_rgba;
    if (xd_pkg_color(generator->t->pkg, cls, part, state, "TextColor", &text_rgba)) {
        xd_strbuf_append(generator->o, XD_STR("  color: "));
        color(generator->o, text_rgba);
        xd_strbuf_append(generator->o, XD_STR(";\n"));
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
static xd_insets borders(const ctx *generator, const char *cls, const char *part, const char *state,
                         xd_insets margins, unsigned flags) {
    xd_insets content_margins = content(generator, cls, part, state, 3);
    xd_insets border_widths = margins;
    if (!(flags & EXACT_H)) {
        border_widths.left = MIN(content_margins.left, margins.left);
        border_widths.right = MIN(content_margins.right, margins.right);
    }
    if (!(flags & EXACT_V)) {
        border_widths.top = MIN(content_margins.top, margins.top);
        border_widths.bottom = MIN(content_margins.bottom, margins.bottom);
    }
    return border_widths;
}

/* The part's look in the state, as declarations: a border and fill, or
 * its image as a border image cut at its SizingMargins, the middle (which
 * GTK leaves empty) the image's middle as the background inside the CSS
 * borders. Where those are the margins, that's exactly uxtheme's 9-grid;
 * elsewhere it's exact at the image's own size and close to it around
 * that. A corner is never under the middle: one of its sides is exact. */
static bool decls(ctx *generator, const char *cls, const char *part, const char *state,
                  unsigned flags) {
    const xd_pkg *package = generator->t->pkg;
    xd_strbuf *buffer = generator->o;
    if (border_fill(generator, cls, part, state)) {
        int border = 1;
        (void)xd_pkg_int(package, cls, part, state, "BorderSize", &border);
        xd_rgba bc, fc, g1, g2;
        xd_strbuf_append(buffer, XD_STR("  border-image: none;\n  box-shadow: none;\n"
                                        "  border-radius: 0;\n"));
        if (xd_pkg_color(package, cls, part, state, "BorderColor", &bc) && border > 0) {
            xd_strbuf_appendf(
                buffer, "  border-style: solid;\n  border-width: %dpx;\n  border-color: ", border);
            color(buffer, bc);
            xd_strbuf_append(buffer, XD_STR(";\n"));
        } else {
            xd_strbuf_append(buffer, XD_STR("  border-style: none;\n"));
        }
        if (!enum_is(generator, cls, part, state, "FillType", "Solid") &&
            xd_pkg_color(package, cls, part, state, "GradientColor1", &g1) &&
            xd_pkg_color(package, cls, part, state, "GradientColor2", &g2)) {
            xd_strbuf_appendf(buffer, "  background-image: linear-gradient(%s",
                              enum_is(generator, cls, part, state, "FillType", "HorzGradient")
                                  ? "to right, "
                                  : "");
            color(buffer, g1);
            xd_strbuf_append(buffer, XD_STR(", "));
            color(buffer, g2);
            xd_strbuf_append(buffer, XD_STR(");\n"));
        } else {
            xd_strbuf_append(buffer, XD_STR("  background-image: none;\n"));
        }
        if (xd_pkg_color(package, cls, part, state, "FillColor", &fc)) {
            xd_strbuf_append(buffer, XD_STR("  background-color: "));
            color(buffer, fc);
            xd_strbuf_append(buffer, XD_STR(";\n"));
        }
        if (!(flags & NO_TEXT))
            text_color(generator, cls, part, state);
        return true;
    }
    int width, height;
    if (!natural(generator, cls, part, state, &width, &height))
        return false;
    xd_insets margins = sizing(generator, cls, part, state, width, height);
    xd_insets border_widths = borders(generator, cls, part, state, margins, flags);
    bool tile = enum_is(generator, cls, part, state, "SizingType", "Tile");
    bool border_only = false;
    (void)xd_pkg_bool(package, cls, part, state, "BorderOnly", &border_only);
    xd_strbuf_appendf(buffer, "  border-style: solid;\n  border-width: %dpx %dpx %dpx %dpx;\n",
                      border_widths.top, border_widths.right, border_widths.bottom,
                      border_widths.left);
    if (margins.left + margins.right + margins.top + margins.bottom > 0) {
        src img = frame_image(generator, cls, part, state, XD_GTK_PKG_BACKGROUND, width, height);
        xd_strbuf_appendf(buffer,
                          "  border-image: %s %d %d %d %d %s;\n"
                          "  border-image-width: %dpx %dpx %dpx %dpx;\n",
                          img.s, margins.top, margins.right, margins.bottom, margins.left,
                          tile ? "repeat" : "stretch", margins.top, margins.right, margins.bottom,
                          margins.left);
    } else {
        xd_strbuf_append(buffer, XD_STR("  border-image: none;\n"));
    }
    /* The image inside the CSS borders, stretched; for Tile, the middle
     * alone, tiled. */
    xd_insets cut = tile ? margins : border_widths;
    /* GTK scales an image with its edges fading out over half a source
     * pixel, which across a 4-pixel middle stretched to 70 is a visible
     * band: along an exact axis (only the part's own middle is there) it's
     * drawn larger to begin with, as the part would be. */
    int pw = width, ph = height;
    if (!tile && (flags & EXACT_H))
        pw = MAX(width, cut.left + cut.right + 64);
    if (!tile && (flags & EXACT_V))
        ph = MAX(height, cut.top + cut.bottom + 64);
    int mw = pw - cut.left - cut.right, mh = ph - cut.top - cut.bottom;
    bool middle =
        !border_only && width - cut.left - cut.right > 0 && height - cut.top - cut.bottom > 0;
    src mid = {0};
    if (middle) {
        char suffix[64];
        g_snprintf(suffix, sizeof(suffix), "m%d-%d-%d-%d-%dx%d", cut.top, cut.right, cut.bottom,
                   cut.left, mw, mh);
        mid = image(generator, XD_GTK_PKG_BACKGROUND, cls, part, state, suffix, mw, mh,
                    (xd_rect){-cut.left, -cut.top, pw, ph});
    }
    const char *mid_size = tile ? "auto" : "100% 100%";
    const char *mid_repeat = tile ? "repeat" : "no-repeat";
    int gw = generator->overlay_w, gh = generator->overlay_h;
    bool glyph = generator->overlay != nullptr ||
                 ((flags & GLYPH_LAYER) && glyph_size(generator, cls, part, state, &gw, &gh));
    if (glyph) {
        /* Drawn at its size: the content area around it is the part's. */
        src glyph_image;
        if (generator->overlay != nullptr) {
            g_strlcpy(glyph_image.s, generator->overlay, sizeof(glyph_image.s));
        } else {
            xd_insets content_margins = content(generator, cls, part, state, 0);
            glyph_image = image(generator, XD_GTK_PKG_GLYPH, cls, part, state, nullptr, gw, gh,
                                (xd_rect){-content_margins.left, -content_margins.top,
                                          gw + content_margins.left + content_margins.right,
                                          gh + content_margins.top + content_margins.bottom});
        }
        if (middle)
            xd_strbuf_appendf(buffer,
                              "  background-image: %s, %s;\n"
                              "  background-size: %dpx %dpx, %s;\n"
                              "  background-position: center, left top;\n"
                              "  background-repeat: no-repeat, %s;\n",
                              glyph_image.s, mid.s, gw, gh, mid_size, mid_repeat);
        else
            xd_strbuf_appendf(buffer,
                              "  background-image: %s;\n  background-size: %dpx %dpx;\n"
                              "  background-position: center;\n"
                              "  background-repeat: no-repeat;\n",
                              glyph_image.s, gw, gh);
    } else if (middle) {
        xd_strbuf_appendf(buffer,
                          "  background-image: %s;\n  background-size: %s;\n"
                          "  background-position: left top;\n  background-repeat: %s;\n",
                          mid.s, mid_size, mid_repeat);
    } else {
        xd_strbuf_append(buffer, XD_STR("  background-image: none;\n"));
    }
    xd_strbuf_append(buffer,
                     XD_STR("  background-origin: padding-box;\n"
                            "  background-clip: padding-box;\n  background-color: transparent;\n"
                            "  box-shadow: none;\n  border-radius: 0;\n  text-shadow: none;\n"));
    if (!(flags & NO_TEXT))
        text_color(generator, cls, part, state);
    return true;
}

/* A rule for the selectors: the part's look in the state, then `extra`
 * (declarations, may be nullptr). False if the package doesn't draw it. */
static bool rule(ctx *generator, const char *bases, const char *suffixes, const char *cls,
                 const char *part, const char *state, unsigned flags, const char *extra) {
    if (!styles(generator, cls, part, state))
        return false;
    cross(generator->o, bases, suffixes);
    xd_strbuf_append(generator->o, XD_STR(" {\n"));
    bool ok = decls(generator, cls, part, state, flags);
    if (extra != nullptr)
        xd_strbuf_append(generator->o, xd_str_from_cstr(extra));
    xd_strbuf_append(generator->o, XD_STR("}\n"));
    return ok;
}

/* A TrueSize part (or one shown whole, glyph and all) at `width` x `height` as the
 * background, nothing else. */
static bool whole_rule(ctx *generator, const char *bases, const char *suffixes, const char *cls,
                       const char *part, const char *state, int width, int height,
                       const char *extra) {
    if (!styles(generator, cls, part, state))
        return false;
    src img = frame_image(generator, cls, part, state, XD_GTK_PKG_WHOLE, width, height);
    cross(generator->o, bases, suffixes);
    xd_strbuf_appendf(generator->o,
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

static void states(ctx *generator, const char *bases, const state_map *map, size_t count,
                   const char *cls, const char *part, unsigned flags, const char *extra) {
    for (size_t i = 0; i < count; i++)
        (void)rule(generator, bases, map[i].suffixes, cls, part, map[i].state, flags, extra);
}

/* ---- widgets ----------------------------------------------------------------------- */

/* Push buttons (Button/PushButton): 75x23 at least for text, as Windows'
 * dialog buttons; the label inset by the ContentMargins. The default (and
 * focused) one is drawn Defaulted, as XP does. */
#define PUSH                                                                                   \
    "button.text-button:not(.flat):not(.combo)|button.image-button:not(.flat):not(.combo)"
static void push_buttons(ctx *generator) {
    int width, height;
    if (!styles(generator, "Button", "PushButton", "1") ||
        border_fill(generator, "Button", "PushButton", "1") ||
        !natural(generator, "Button", "PushButton", "1", &width, &height))
        return;
    xd_insets insets =
        borders(generator, "Button", "PushButton", "1",
                sizing(generator, "Button", "PushButton", "1", width, height), EXACT_H);
    int min_h = MAX(23 - insets.top - insets.bottom, 0);
    xd_strbuf_appendf(generator->o,
                      "%s {\n  padding: 0 1px;\n  min-height: %dpx;\n  min-width: 0;\n}\n"
                      "button.text-button:not(.flat):not(.combo) { min-width: %dpx; }\n",
                      "button.text-button:not(.flat):not(.combo),\n"
                      "button.image-button:not(.flat):not(.combo)",
                      min_h, MAX(75 - insets.left - insets.right - 2, 0));
    static const state_map map[] = {
        {"", "1"},
        {":focus|.default", "5"},
        {":hover", "2"},
        {":active|:checked", "3"},
        {":disabled", "4"},
    };
    states(generator, PUSH, map, G_N_ELEMENTS(map), "Button", "PushButton", EXACT_H, nullptr);
    /* Pressed: the label one pixel down and right, as in Windows. */
    cross(generator->o, PUSH, ":active");
    xd_strbuf_appendf(generator->o, " {\n  padding: 1px 0 0 2px;\n  min-height: %dpx;\n}\n",
                      MAX(min_h - 1, 0));
    xd_rgba disabled_rgba;
    if (xd_pkg_color(generator->t->pkg, "Button", "PushButton", "4", "TextColor", &disabled_rgba)) {
        cross(generator->o, PUSH, ":disabled label");
        xd_strbuf_append(generator->o, XD_STR(" { color: "));
        color(generator->o, disabled_rgba);
        xd_strbuf_append(generator->o, XD_STR("; }\n"));
    }
    /* Toolbars keep flat buttons. */
    xd_strbuf_append(generator->o, XD_STR("toolbar button.text-button:not(.flat):not(.combo),\n"
                                          "toolbar button.image-button:not(.flat):not(.combo) {\n"
                                          "  border-image: none;\n  background-image: none;\n"
                                          "  border-width: 1px;\n  padding: 2px 4px;\n"
                                          "  min-height: 0;\n  min-width: 0;\n}\n"));
}
#undef PUSH

/* Check boxes and radio buttons: TrueSize images, shown as they are. Not
 * in menus, which show only the mark (the base style's). */
static void indicators(ctx *generator, const char *part, const char *bases, bool mixed) {
    int width, height;
    if (!styles(generator, "Button", part, "1") ||
        !natural(generator, "Button", part, "1", &width, &height))
        return;
    static const char *const kinds[] = {"", ":checked", ":indeterminate"};
    static const char *const moods[] = {"", ":hover", ":active", ":disabled"};
    for (int k = 0; k < (mixed ? 3 : 2); k++)
        for (int mood = 0; mood < 4; mood++) {
            char state[8];
            g_snprintf(state, sizeof(state), "%d", k * 4 + mood + 1);
            char suffix[48];
            g_snprintf(suffix, sizeof(suffix), "%s%s", kinds[k], moods[mood]);
            if (!styles(generator, "Button", part, state))
                continue;
            src img =
                frame_image(generator, "Button", part, state, XD_GTK_PKG_WHOLE, width, height);
            cross(generator->o, bases, suffix);
            xd_strbuf_appendf(generator->o,
                              " {\n  -gtk-icon-source: %s;\n  min-width: %dpx;\n"
                              "  min-height: %dpx;\n  background-image: none;\n"
                              "  background-color: transparent;\n  border: none;\n"
                              "  border-image: none;\n  border-radius: 0;\n  box-shadow: none;\n"
                              "  padding: 0;\n}\n",
                              img.s, width, height);
        }
}

/* Text fields (Edit/EditText). */
static void entries(ctx *generator) {
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":focus", "5"}, {":disabled", "4"},
    };
    states(generator, "entry|spinbutton", map, G_N_ELEMENTS(map), "Edit", "EditText", EXACT_H,
           nullptr);
}

/* Combo boxes: the field (ComboBox) and its drop-down button
 * (DropDownButton) on GTK's arrow, with the glyph as the arrow's icon. */
static void combos(ctx *generator) {
    static const state_map field[] = {
        {"", "1"}, {":hover", "2"}, {":disabled", "4"},
    };
    states(generator, "combobox button.combo", field, G_N_ELEMENTS(field), "ComboBox", nullptr,
           EXACT_H, "  padding: 0 0 0 3px;\n");
    int width, height, gw = 0, gh = 0;
    const char *const DB = "DropDownButton";
    if (!styles(generator, "ComboBox", DB, "1") ||
        !natural(generator, "ComboBox", DB, "1", &width, &height))
        return;
    (void)glyph_size(generator, "ComboBox", DB, "1", &gw, &gh);
    xd_insets insets = sizing(generator, "ComboBox", DB, "1", width, height);
    /* As wide as the image (Windows' is the scroll bar's width, near it),
     * the glyph a background over it: GTK draws an arrow's icon square. */
    char extra[256];
    g_snprintf(extra, sizeof(extra),
               "  -gtk-icon-source: none;\n  min-width: %dpx;\n  min-height: %dpx;\n"
               "  padding: 0;\n  margin: 0;\n",
               MAX(width - insets.left - insets.right, gw), gh);
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":active|:checked", "3"}, {":disabled", "4"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(map); i++)
        (void)rule(generator, "combobox button.combo% arrow", map[i].suffixes, "ComboBox", DB,
                   map[i].state, GLYPH_LAYER | NO_TEXT | EXACT, extra);
}

/* Scroll bars: the arrow buttons whole, the thumb with its gripper, the
 * track, in both orientations, the scheme's scroll bar width across. */
static void scrollbars(ctx *generator) {
    const char *const SB = "ScrollBar";
    if (!styles(generator, SB, "ArrowBtn", "1"))
        return;
    int width = xd_metric_px(generator->t, XD_METRIC_SCROLL_WIDTH);
    width = MAX(lpx(generator, width), 8);
    xd_strbuf_appendf(generator->o,
                      "scrollbar { background-color: transparent; background-image: none; }\n"
                      "scrollbar.vertical > contents > trough { min-width: %dpx; }\n"
                      "scrollbar.horizontal > contents > trough { min-height: %dpx; }\n",
                      width, width);
    static const struct {
        const char *orient, *thumb, *gripper, *track;
        const char *arrow_up, *arrow_down; /* first state of four */
    } bars[] = {
        {"vertical", "ThumbBtnVert", "GripperVert", "UpperTrackVert", "1", "5"},
        {"horizontal", "ThumbBtnHorz", "GripperHorz", "UpperTrackHorz", "9", "13"},
    };
    /* Windows keeps an arrow enabled at the end of its track (GTK disables
     * it): only a disabled scroll bar's are. */
    static const char *const moods[] = {"", ":hover", ":active", ""};
    for (size_t i = 0; i < G_N_ELEMENTS(bars); i++) {
        bool vertical = i == 0;
        for (int button = 0; button < 2; button++) {
            int first = atoi(button == 0 ? bars[i].arrow_up : bars[i].arrow_down);
            for (int mood = 0; mood < 4; mood++) {
                char state[8], base[96], extra[160];
                g_snprintf(state, sizeof(state), "%d", first + mood);
                g_snprintf(base, sizeof(base), "scrollbar.%s%s button.%s", bars[i].orient,
                           mood == 3 ? ":disabled" : "", button == 0 ? "up" : "down");
                g_snprintf(extra, sizeof(extra),
                           "  min-width: %dpx;\n  min-height: %dpx;\n  padding: 0;\n"
                           "  margin: 0;\n  -gtk-icon-source: none;\n",
                           width, width);
                (void)whole_rule(generator, base, moods[mood], SB, "ArrowBtn", state, width, width,
                                 extra);
            }
        }
        char trough[96];
        g_snprintf(trough, sizeof(trough), "scrollbar.%s trough", bars[i].orient);
        (void)rule(generator, trough, "", SB, bars[i].track, "1", NO_TEXT | EXACT,
                   "  padding: 0;\n");
        g_snprintf(trough, sizeof(trough), "scrollbar.%s:disabled trough", bars[i].orient);
        (void)rule(generator, trough, "", SB, bars[i].track, "4", NO_TEXT | EXACT, nullptr);
        for (int mood = 0; mood < 4; mood++) {
            char state[8], base[96], extra[256];
            g_snprintf(state, sizeof(state), "%d", mood + 1);
            g_snprintf(base, sizeof(base), "scrollbar.%s%s slider", bars[i].orient,
                       mood == 3 ? ":disabled" : "");
            int tw, th;
            if (!natural(generator, SB, bars[i].thumb, state, &tw, &th))
                continue;
            xd_insets tm = sizing(generator, SB, bars[i].thumb, state, tw, th);
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
            src gripper_image = {0};
            if (styles(generator, SB, bars[i].gripper, state) &&
                natural(generator, SB, bars[i].gripper, state, &gw, &gh)) {
                int space = vertical ? width - tm.left - tm.right : width - tm.top - tm.bottom;
                int size = vertical ? gw : gh;
                int canvas = size + ((space - size) & 1);
                int want = (width - size) / 2 - (vertical ? tm.left : tm.top);
                int at = want - (space - canvas) / 2;
                gripper_image = vertical ? image(generator, XD_GTK_PKG_WHOLE, SB, bars[i].gripper,
                                                 state, "c", canvas, gh, (xd_rect){at, 0, gw, gh})
                                         : image(generator, XD_GTK_PKG_WHOLE, SB, bars[i].gripper,
                                                 state, "c", gw, canvas, (xd_rect){0, at, gw, gh});
                generator->overlay = gripper_image.s;
                generator->overlay_w = vertical ? canvas : gw;
                generator->overlay_h = vertical ? gh : canvas;
            }
            (void)rule(generator, base, moods[mood], SB, bars[i].thumb, state, NO_TEXT | EXACT,
                       extra);
            generator->overlay = nullptr;
        }
    }
}

/* Tabs on top (Tab/TabItem, TopTabItem for the selected one, the
 * LeftEdge variants for the first) and the page (Pane's edges, Body's
 * fill). The page's top edge is the header's bottom border, so the
 * selected tab, 2px taller, can cover it and join the page. */
static void tabs(ctx *generator) {
    const char *const TAB = "Tab";
    int pw, ph;
    bool pane = styles(generator, TAB, "Pane", "1") &&
                natural(generator, TAB, "Pane", "1", &pw, &ph) &&
                !border_fill(generator, TAB, "Pane", "1");
    xd_insets pm = pane ? sizing(generator, TAB, "Pane", "1", pw, ph) : (xd_insets){0};
    if (pane) {
        /* The pane's top edge, without its corners, along the header. */
        int ew = MAX(pw - pm.left - pm.right, 1);
        if (pm.top > 0) {
            src top = image(generator, XD_GTK_PKG_BACKGROUND, TAB, "Pane", "1", "top", ew, pm.top,
                            (xd_rect){-pm.left, 0, pw, ph});
            xd_strbuf_appendf(generator->o,
                              "notebook > header.top {\n  border-style: solid;\n"
                              "  border-width: 0 0 %dpx 0;\n"
                              "  border-image: %s 0 0 %d 0 stretch;\n"
                              "  border-image-width: 0 0 %dpx 0;\n  box-shadow: none;\n"
                              "  background-image: none;\n  padding: 0 2px;\n}\n",
                              pm.top, top.s, pm.top, pm.top);
        }
        src edges = frame_image(generator, TAB, "Pane", "1", XD_GTK_PKG_BACKGROUND, pw, ph);
        xd_strbuf_appendf(generator->o,
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
        if (styles(generator, TAB, "Body", "1") && natural(generator, TAB, "Body", "1", &bw, &bh)) {
            /* Wide to begin with (XP's is 10 pixels): see decls(). */
            bw = MAX(bw, 256);
            src body = image(generator, XD_GTK_PKG_BACKGROUND, TAB, "Body", "1", "wide", bw, bh,
                             (xd_rect){0, 0, bw, bh});
            xd_strbuf_appendf(generator->o,
                              "  background-image: %s;\n  background-size: 100%% 100%%;\n"
                              "  background-repeat: no-repeat;\n",
                              body.s);
        } else {
            xd_strbuf_appendf(generator->o,
                              "  background-image: %s;\n  background-size: 100%% 100%%;\n",
                              edges.s);
        }
        if (xd_pkg_color(generator->t->pkg, TAB, "Body", nullptr, "FillColorHint", &hint) ||
            xd_pkg_color(generator->t->pkg, TAB, "Pane", nullptr, "FillColorHint", &hint)) {
            xd_strbuf_append(generator->o, XD_STR("  background-color: "));
            color(generator->o, hint);
            xd_strbuf_append(generator->o, XD_STR(";\n"));
        }
        xd_strbuf_append(generator->o, XD_STR("}\n"));
    }
    int width, height;
    if (!styles(generator, TAB, "TabItem", "1") || border_fill(generator, TAB, "TabItem", "1") ||
        !natural(generator, TAB, "TabItem", "1", &width, &height))
        return;
    /* The tabs as tall as the image (18 in XP), the selected one 2px
     * taller and over the page's edge. */
    xd_insets insets = borders(generator, TAB, "TabItem", "1",
                               sizing(generator, TAB, "TabItem", "1", width, height), EXACT_H);
    int inner = MAX(height - insets.top - insets.bottom - 4, 0);
    const char *const TAB_SELECTOR = "notebook > header.top > tabs > tab";
    xd_strbuf_appendf(generator->o,
                      "%s {\n  margin: 2px 0 0 0;\n  padding: 2px 1px;\n  min-height: %dpx;\n"
                      "  outline-offset: -3px;\n}\n"
                      "%s:checked {\n  margin: 0 0 -%dpx 0;\n  padding: 2px 1px %dpx 1px;\n}\n",
                      TAB_SELECTOR, inner, TAB_SELECTOR, pm.top, 4 + pm.top);
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":disabled", "4"},
    };
    bool top = styles(generator, TAB, "TopTabItem", "3");
    bool left = styles(generator, TAB, "TabItemLeftEdge", "1");
    bool top_left = styles(generator, TAB, "TopTabItemLeftEdge", "3");
    states(generator, TAB_SELECTOR, map, G_N_ELEMENTS(map), TAB, "TabItem", EXACT_H, nullptr);
    if (left)
        states(generator, "notebook > header.top > tabs > tab:first-child", map, G_N_ELEMENTS(map),
               TAB, "TabItemLeftEdge", EXACT_H, nullptr);
    (void)rule(generator, TAB_SELECTOR, ":checked", TAB, top ? "TopTabItem" : "TabItem", "3",
               EXACT_H, nullptr);
    if (top_left || left)
        (void)rule(generator, "notebook > header.top > tabs > tab:first-child", ":checked", TAB,
                   top_left ? "TopTabItemLeftEdge" : "TabItemLeftEdge", "3", EXACT_H, nullptr);
}

/* Group boxes (Button/GroupBox): the border alone, the caption in the
 * part's text colour. Status bar panes (.xd-sunken) keep theirs. */
static void group_boxes(ctx *generator) {
    if (!styles(generator, "Button", "GroupBox", "1") ||
        border_fill(generator, "Button", "GroupBox", "1"))
        return;
    (void)rule(generator, "frame:not(.xd-sunken) > border", "", "Button", "GroupBox", "1",
               NO_TEXT | EXACT, nullptr);
    xd_rgba text_rgba;
    if (xd_pkg_color(generator->t->pkg, "Button", "GroupBox", "1", "TextColor", &text_rgba)) {
        xd_strbuf_append(generator->o, XD_STR("frame:not(.xd-sunken) > border + label { color: "));
        color(generator->o, text_rgba);
        xd_strbuf_append(generator->o, XD_STR("; }\n"));
    }
}

/* Progress bars: the track (Bar) and its chunks (Chunk), ProgressChunkSize
 * wide with ProgressSpaceSize between, as Windows draws them; inside the
 * track's sizing margins (it has no content margins in XP's). */
static void progress_bars(ctx *generator) {
    static const struct {
        const char *orient, *bar, *chunk;
        bool vertical;
    } bars[] = {{"horizontal", "Bar", "Chunk", false}, {"vertical", "BarVert", "ChunkVert", true}};
    for (size_t i = 0; i < G_N_ELEMENTS(bars); i++) {
        int width, height;
        if (!styles(generator, "Progress", bars[i].bar, "1") ||
            !natural(generator, "Progress", bars[i].bar, "1", &width, &height))
            continue;
        xd_insets insets = border_fill(generator, "Progress", bars[i].bar, "1")
                               ? (xd_insets){1, 1, 1, 1}
                               : sizing(generator, "Progress", bars[i].bar, "1", width, height);
        char sel[96], extra[256];
        g_snprintf(sel, sizeof(sel), "progressbar.%s > trough", bars[i].orient);
        int across = bars[i].vertical ? MAX(width - insets.left - insets.right, 1)
                                      : MAX(height - insets.top - insets.bottom, 1);
        g_snprintf(extra, sizeof(extra), "  padding: 0;\n  %s: %dpx;\n",
                   bars[i].vertical ? "min-width" : "min-height", across);
        (void)rule(generator, sel, "", "Progress", bars[i].bar, "1", NO_TEXT | EXACT, extra);
        int cw, ch;
        if (!styles(generator, "Progress", bars[i].chunk, "1") ||
            !natural(generator, "Progress", bars[i].chunk, "1", &cw, &ch))
            continue;
        int size = bars[i].vertical ? ch : cw, space = 0;
        (void)xd_pkg_int(generator->t->pkg, "Progress", bars[i].chunk, "1", "ProgressChunkSize",
                         &size);
        (void)xd_pkg_int(generator->t->pkg, "Progress", bars[i].chunk, "1", "ProgressSpaceSize",
                         &space);
        size = CLAMP(size, 1, 256);
        space = CLAMP(space, 0, 256);
        /* One chunk and the space after it (before it, upwards). */
        src img = bars[i].vertical
                      ? image(generator, XD_GTK_PKG_BACKGROUND, "Progress", bars[i].chunk, "1",
                              "step", cw, size + space, (xd_rect){0, space, cw, size})
                      : image(generator, XD_GTK_PKG_BACKGROUND, "Progress", bars[i].chunk, "1",
                              "step", size + space, ch, (xd_rect){0, 0, size, ch});
        xd_strbuf_appendf(generator->o,
                          "progressbar.%s > trough > progress {\n  background-image: %s;\n"
                          "  background-repeat: %s;\n  background-size: %s;\n"
                          "  background-position: %s;\n  background-color: transparent;\n"
                          "  border: none;\n  border-image: none;\n  border-radius: 0;\n"
                          "  box-shadow: none;\n  %s: %dpx;\n}\n",
                          bars[i].orient, img.s, bars[i].vertical ? "repeat-y" : "repeat-x",
                          bars[i].vertical ? "100% auto" : "auto 100%",
                          bars[i].vertical ? "left bottom" : "left top",
                          bars[i].vertical ? "min-width" : "min-height", across);
    }
}

/* Sliders (TrackBar): the track, and the thumb whole: rectangular, or
 * pointed towards the marks when the scale has them on one side. */
static void scales(ctx *generator) {
    const char *const TB = "TrackBar";
    static const struct {
        const char *orient, *track, *thumb, *before, *after;
        bool vertical;
    } bars[] = {
        {"horizontal", "Track", "Thumb", "ThumbTop", "ThumbBottom", false},
        {"vertical", "TrackVert", "ThumbVert", "ThumbLeft", "ThumbRight", true},
    };
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":active", "3"}, {":disabled", "5"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bars); i++) {
        int tw, th, across = 0;
        if (styles(generator, TB, bars[i].track, "1") &&
            natural(generator, TB, bars[i].track, "1", &tw, &th)) {
            xd_insets insets = border_fill(generator, TB, bars[i].track, "1")
                                   ? (xd_insets){1, 1, 1, 1}
                                   : sizing(generator, TB, bars[i].track, "1", tw, th);
            char sel[64], extra[256];
            g_snprintf(sel, sizeof(sel), "scale.%s trough", bars[i].orient);
            int inner = bars[i].vertical ? MAX(tw - insets.left - insets.right, 0)
                                         : MAX(th - insets.top - insets.bottom, 0);
            g_snprintf(extra, sizeof(extra),
                       "  padding: 0;\n  min-height: %dpx;\n  min-width: %dpx;\n",
                       bars[i].vertical ? 0 : inner, bars[i].vertical ? inner : 0);
            (void)rule(generator, sel, "", TB, bars[i].track, "1", NO_TEXT | EXACT, extra);
            across = inner;
        }
        const char *const thumbs[][2] = {
            {bars[i].thumb, ""},
            {bars[i].before, ".marks-before:not(.marks-after)"},
            {bars[i].after, ".marks-after:not(.marks-before)"},
        };
        for (size_t k = 0; k < G_N_ELEMENTS(thumbs); k++) {
            int width, height;
            if (!styles(generator, TB, thumbs[k][0], "1") ||
                !natural(generator, TB, thumbs[k][0], "1", &width, &height))
                continue;
            char base[128], extra[256];
            g_snprintf(base, sizeof(base), "scale.%s%s slider", bars[i].orient, thumbs[k][1]);
            /* Centred across the track, overhanging it. */
            int over = bars[i].vertical ? (width - across) / 2 : (height - across) / 2;
            g_snprintf(extra, sizeof(extra),
                       "  min-width: %dpx;\n  min-height: %dpx;\n  padding: 0;\n"
                       "  margin: %s%dpx%s;\n",
                       width, height, bars[i].vertical ? "0 " : "", -MAX(over, 0),
                       bars[i].vertical ? "" : " 0");
            for (size_t state_index = 0; state_index < G_N_ELEMENTS(map); state_index++)
                (void)whole_rule(generator, base, map[state_index].suffixes, TB, thumbs[k][0],
                                 map[state_index].state, width, height, extra);
        }
    }
}

/* Spin buttons: Spin's Up and Down, each with its glyph. GTK puts them
 * side by side; Windows stacks them, which CSS can't. */
static void spin_buttons(ctx *generator) {
    static const state_map map[] = {
        {"", "1"}, {":hover", "2"}, {":active", "3"}, {":disabled", "4"},
    };
    static const char *const parts[][2] = {{"Up", "up"}, {"Down", "down"}};
    for (size_t i = 0; i < G_N_ELEMENTS(parts); i++) {
        int width, height;
        if (!styles(generator, "Spin", parts[i][0], "1") ||
            !natural(generator, "Spin", parts[i][0], "1", &width, &height))
            continue;
        int gw = 0, gh = 0;
        (void)glyph_size(generator, "Spin", parts[i][0], "1", &gw, &gh);
        xd_insets insets = sizing(generator, "Spin", parts[i][0], "1", width, height);
        char base[64], extra[192];
        g_snprintf(base, sizeof(base), "spinbutton:not(.vertical) button.%s", parts[i][1]);
        g_snprintf(extra, sizeof(extra),
                   "  padding: 0;\n  margin: 0;\n  min-width: %dpx;\n  min-height: 0;\n"
                   "  -gtk-icon-transform: scale(0);\n",
                   MAX(width - insets.left - insets.right, gw));
        states(generator, base, map, G_N_ELEMENTS(map), "Spin", parts[i][0],
               GLYPH_LAYER | NO_TEXT | EXACT, extra);
    }
}

/* Column headers (Header/HeaderItem), as tall as the image. */
static void headers(ctx *generator) {
    int width, height;
    if (!styles(generator, "Header", "HeaderItem", "1") ||
        border_fill(generator, "Header", "HeaderItem", "1") ||
        !natural(generator, "Header", "HeaderItem", "1", &width, &height))
        return;
    xd_insets insets =
        borders(generator, "Header", "HeaderItem", "1",
                sizing(generator, "Header", "HeaderItem", "1", width, height), EXACT_H);
    char extra[192];
    g_snprintf(extra, sizeof(extra), "  padding: 1px 0;\n  min-height: %dpx;\n",
               MAX(height - insets.top - insets.bottom - 2, 0));
    static const state_map map[] = {{"", "1"}, {":hover", "2"}, {":active", "3"}};
    states(generator, "treeview.view header button", map, G_N_ELEMENTS(map), "Header", "HeaderItem",
           EXACT_H, extra);
}

/* Tooltips (Tooltip/Standard), where the package draws them. */
static void tooltips(ctx *generator) {
    (void)rule(generator, "tooltip.background|tooltip", "", "Tooltip", "Standard", "1", EXACT_H,
               "  padding: 1px 3px;\n");
    xd_rgba text_rgba;
    if (xd_pkg_color(generator->t->pkg, "Tooltip", "Standard", "1", "TextColor", &text_rgba)) {
        xd_strbuf_append(generator->o, XD_STR("tooltip * { color: "));
        color(generator->o, text_rgba);
        xd_strbuf_append(generator->o, XD_STR("; }\n"));
    }
}

/* Menus in the package's menu font (its colours come with the toolkit's). */
static void menu_font(ctx *generator) {
    const xd_font_spec *menu_spec = &generator->t->scheme.fonts[XD_FONT_MENU];
    const xd_font_spec *message_spec = &generator->t->scheme.fonts[XD_FONT_MESSAGE];
    if (strcmp(menu_spec->family, message_spec->family) == 0 &&
        menu_spec->size_pt == message_spec->size_pt && menu_spec->bold == message_spec->bold)
        return;
    xd_strbuf_append(generator->o,
                     XD_STR("menubar, menu, .menu, .context-menu {\n  font-family: "));
    /* Names without quotes or backslashes, so they can't leave the string. */
    char **names = g_strsplit(menu_spec->family, ",", -1);
    for (char **family_part = names; *family_part != nullptr; family_part++) {
        char *name = g_strstrip(*family_part);
        if (family_part != names)
            xd_strbuf_append(generator->o, XD_STR(", "));
        bool generic = strcmp(name, "sans-serif") == 0 || strcmp(name, "serif") == 0 ||
                       strcmp(name, "monospace") == 0;
        if (!generic)
            xd_strbuf_append_char(generator->o, '"');
        for (const char *cursor = name; *cursor != '\0'; cursor++)
            if (*cursor != '"' && *cursor != '\\')
                xd_strbuf_append_char(generator->o, *cursor);
        if (!generic)
            xd_strbuf_append_char(generator->o, '"');
    }
    g_strfreev(names);
    xd_strbuf_appendf(generator->o, ";\n  font-size: %dpx;\n  font-weight: %s;\n}\n",
                      (int)lround(menu_spec->size_pt * 96.0 / 72.0),
                      menu_spec->bold ? "bold" : "normal");
}

static void generate(ctx *generator) {
    if (generator->t->pkg == nullptr)
        return;
    xd_strbuf_appendf(generator->o, "\n/* Theme package \"%s\" (%s), its images scaled %s. */\n",
                      xd_pkg_name(generator->t->pkg), xd_pkg_variant(generator->t->pkg),
                      xd_image_filter_name(generator->t->image_filter));
    push_buttons(generator);
    indicators(generator, "CheckBox", "checkbutton check", true);
    indicators(generator, "RadioButton", "radiobutton radio", false);
    entries(generator);
    combos(generator);
    scrollbars(generator);
    tabs(generator);
    group_boxes(generator);
    progress_bars(generator);
    scales(generator);
    spin_buttons(generator);
    headers(generator);
    tooltips(generator);
    menu_font(generator);
}

void xd_gtk_pkg_css(const xd_theme *theme, xd_strbuf *out) {
    ctx generator = {.t = theme, .o = out};
    generate(&generator);
}

GArray *xd_gtk_pkg_images(const xd_theme *theme) {
    xd_strbuf scratch = {0};
    ctx generator = {
        .t = theme, .o = &scratch, .images = g_array_new(false, true, sizeof(xd_gtk_pkg_image))};
    generate(&generator);
    xd_strbuf_free(&scratch);
    return generator.images;
}

void xd_gtk_pkg_toolkit_colors(const xd_theme *theme, xd_toolkit_colors *toolkit_colors) {
    if (theme->pkg == nullptr)
        return;
    bool flat = theme->scheme.flat_menus;
    (void)xd_pkg_sys_bool(theme->pkg, "FlatMenus", &flat);
    toolkit_colors->bg = toolkit_colors->face = xd_sys(theme, XD_COLOR_BUTTON_FACE);
    toolkit_colors->fg = xd_sys(theme, XD_COLOR_BUTTON_TEXT);
    toolkit_colors->base = xd_sys(theme, XD_COLOR_WINDOW);
    toolkit_colors->text = xd_sys(theme, XD_COLOR_WINDOW_TEXT);
    toolkit_colors->disabled = xd_sys(theme, XD_COLOR_GRAY_TEXT);
    toolkit_colors->selected_bg = xd_sys(theme, XD_COLOR_HILIGHT);
    toolkit_colors->selected_fg = xd_sys(theme, XD_COLOR_HILIGHT_TEXT);
    toolkit_colors->tooltip_bg = xd_sys(theme, XD_COLOR_INFO_WINDOW);
    toolkit_colors->tooltip_fg = xd_sys(theme, XD_COLOR_INFO_TEXT);
    toolkit_colors->menu_bg = xd_sys(theme, XD_COLOR_MENU);
    toolkit_colors->menu_fg = xd_sys(theme, XD_COLOR_MENU_TEXT);
    /* XP's flat menus: MenuHilight behind the item, MenuBar behind the bar. */
    toolkit_colors->menu_hot = xd_sys(theme, flat ? XD_COLOR_MENU_HILIGHT : XD_COLOR_HILIGHT);
    toolkit_colors->menu_hot_fg = xd_sys(theme, XD_COLOR_HILIGHT_TEXT);
    toolkit_colors->menubar_bg = xd_sys(theme, flat ? XD_COLOR_MENU_BAR : XD_COLOR_BUTTON_FACE);
    toolkit_colors->menu_border = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
    toolkit_colors->title_bg = xd_sys(theme, XD_COLOR_ACTIVE_TITLE);
    toolkit_colors->title_bg2 = xd_sys(theme, XD_COLOR_GRADIENT_ACTIVE_TITLE);
    toolkit_colors->title_fg = xd_sys(theme, XD_COLOR_TITLE_TEXT);
    toolkit_colors->hilight = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT);
    toolkit_colors->light = xd_sys(theme, XD_COLOR_BUTTON_LIGHT);
    toolkit_colors->shadow = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
    toolkit_colors->dkshadow = xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW);
    toolkit_colors->scrollbar = xd_sys(theme, XD_COLOR_SCROLLBAR);
    /* Fields' and lists' borders, where the package gives them. */
    xd_rgba border;
    if (xd_pkg_color(theme->pkg, "Edit", "EditText", "1", "BorderColor", &border))
        toolkit_colors->field_border = border;
}
