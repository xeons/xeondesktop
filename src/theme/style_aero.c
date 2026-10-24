/* SPDX-License-Identifier: MIT
 * Aero: the Windows 7 look. Translucent glass window frames (the
 * compositor blurs what's behind them), a grouped caption button strip
 * with a red Close, the title on a soft white glow, a glass taskbar with
 * a round Start orb, and Windows 7's light dialogs and menus. All drawn
 * procedurally.
 */
#include <math.h>
#include <pango/pangocairo.h>

#include "aero_controls.h"
#include "style.h"

/* Logical px at 96 dpi. */
enum {
    BORDER = 8,        /* a resizable window's glass border */
    DIALOG_BORDER = 6, /* a fixed-size window's */
    CAPTION = 22,      /* the caption band below the top border */
    RADIUS = 6,        /* frame corners */
    BTN_H = 20,
    BTN_MIN_W = 26,
    BTN_MAX_W = 27,
    BTN_CLOSE_W = 47,
    BTN_CLOSE_ONLY_W = 43,
    TASKBAR_H = 40,
    ORB_W = 54,
};

static const xd_font_spec title_font = {.family = XD_FONT_FAMILY_SEGOE, .size_pt = 9};

/* Windows 7's Window Color and Appearance swatches, as DWM stores them in
 * ColorizationColor (0xAARRGGBB, the alpha is the colour intensity). Read
 * off Windows 7's registry by others: AeroThemePlasma's colour mixer
 * documentation lists all sixteen, and Mozilla bug 578780 (comments 27,
 * 29 and 31) gives Sky, Sea, Lime, Sun, Pumpkin, Ruby, Violet and
 * Chocolate with the same values. */
const xd_glass_swatch xd_glass_swatches[XD_GLASS_SWATCH_COUNT] = {
    {"Sky", 0x6B74B8FC},     {"Twilight", 0xA80046AD},  {"Sea", 0x8032CDCD},
    {"Leaf", 0x6614A600},    {"Lime", 0x6697D937},      {"Sun", 0x54FADC0E},
    {"Pumpkin", 0x80FF9C00}, {"Ruby", 0xA8CE0F0F},      {"Fuchsia", 0x66FF0099},
    {"Blush", 0x70FCC7F8},   {"Violet", 0x856E3BA1},    {"Lavender", 0x528D5A94},
    {"Taupe", 0x6698844C},   {"Chocolate", 0xA84F1B1B}, {"Slate", 0x80555555},
    {"Frost", 0x54FCFCFC},
};

/* The colour's hue, deeper and darker: the taskbar's and Start menu's
 * glass, darker than the frames' in Windows 7. */
static xd_rgba deepen(xd_hsv hsv, double value) {
    return xd_rgba_from_hsv((xd_hsv){hsv.h, fmin(1.0, hsv.s * 1.3), hsv.v * value}, 1.0);
}

void xd_theme_glass_colors(const xd_theme *theme, xd_glass_colors *out) {
    uint32_t argb = theme->scheme.glass_color;
    xd_rgba color = xd_rgba_hex(argb & 0xFFFFFFu);
    double intensity = ((argb >> 24) & 0xFFu) / 255.0;
    xd_hsv hsv = xd_rgba_to_hsv(color);
    out->translucent = theme->glass && !theme->scheme.glass_opaque;
    if (out->translucent) {
        /* Over the blur: the intensity is how much of the colour shows.
         * Sky's comes to Windows 7's default look. */
        double opacity = fmin(0.2 + 0.85 * intensity, 0.95);
        out->frame = xd_rgba_alpha(color, opacity);
        /* Inactive glass is paler and clearer. */
        out->frame_inactive =
            xd_rgba_alpha(xd_rgba_mix(color, xd_rgba_hex(0xD9D9D9), 0.7), 0.08 + 0.6 * opacity);
        out->taskbar = xd_rgba_alpha(deepen(hsv, 0.26), 0.45 + 0.4 * intensity);
        out->menu = xd_rgba_alpha(deepen(hsv, 0.36), 0.3 + 0.35 * intensity);
        out->menu_frame = xd_rgba_alpha(deepen(hsv, 0.25), 0.55 + 0.35 * intensity);
        return;
    }
    /* Opaque (transparency off, or no blur: Windows 7 Basic): the colour
     * blended into light grey by its intensity, as Windows 7 does with
     * transparency off. */
    xd_rgba grey = xd_rgba_hex(0xEBEBEB);
    out->frame = xd_rgba_mix(grey, color, intensity);
    out->frame_inactive = xd_rgba_mix(grey, color, intensity * 0.5);
    out->taskbar = deepen(hsv, 0.42);
    out->menu = deepen(hsv, 0.36);
    out->menu_frame = deepen(hsv, 0.25);
}

static xd_rgba glass(const xd_theme *theme, bool active) {
    xd_glass_colors glass_colors;
    xd_theme_glass_colors(theme, &glass_colors);
    return active ? glass_colors.frame : glass_colors.frame_inactive;
}

static int border_of(const xd_theme *theme, const xd_frame *frame) {
    if (frame->maximized)
        return 0;
    return xd_tpx(theme, frame->dialog ? DIALOG_BORDER : BORDER);
}

/* ---- frame ---------------------------------------------------------------------- */

static xd_insets aero_frame_insets(const xd_theme *theme, const xd_frame *frame) {
    int border = border_of(theme, frame);
    return (xd_insets){border, border + xd_tpx(theme, CAPTION), border, border};
}

static void aero_frame_layout(const xd_theme *theme, xd_rect outer, const xd_frame *frame,
                              xd_frame_layout *frame_layout) {
    xd_insets in = aero_frame_insets(theme, frame);
    int border = border_of(theme, frame);
    frame_layout->caption = (xd_rect){outer.x, outer.y, outer.w, in.top};
    /* The strip hangs from the top edge, inside the right border. */
    int right =
        outer.x + outer.w - (frame->maximized ? xd_tpx(theme, 2) : border - xd_tpx(theme, 2));
    int by = outer.y + (frame->maximized ? 0 : xd_tpx(theme, 1));
    int bh = xd_tpx(theme, BTN_H);
    bool close_only = (frame->buttons & ~XD_CAPBTN_CLOSE) == 0;
    static const struct {
        uint8_t flag;
        xd_frame_part part;
        int w;
    } order[] = {
        {XD_CAPBTN_CLOSE, XD_PART_CLOSE, BTN_CLOSE_W},
        {XD_CAPBTN_MAXIMIZE, XD_PART_MAXIMIZE, BTN_MAX_W},
        {XD_CAPBTN_MINIMIZE, XD_PART_MINIMIZE, BTN_MIN_W},
        {XD_CAPBTN_HELP, XD_PART_HELP, BTN_MIN_W},
    };
    int bx = right;
    for (size_t i = 0; i < XD_ARRAY_LEN(order); i++) {
        if (!(frame->buttons & order[i].flag))
            continue;
        int width = xd_tpx(theme, order[i].part == XD_PART_CLOSE && close_only ? BTN_CLOSE_ONLY_W
                                                                               : order[i].w);
        bx -= width;
        xd_layout_add_button(frame_layout, (xd_rect){bx, by, width, bh}, order[i].part);
    }
    /* As in Windows 7, the icon and title sit in the middle of all the glass
     * above the client area, top border included: the caption buttons hang
     * from the top edge, and centred below the border they look low. */
    int icon = xd_tpx(theme, 16);
    int cy = outer.y + (in.top - icon) / 2;
    frame_layout->icon = (xd_rect){outer.x + border + xd_tpx(theme, 2), cy, icon, icon};
    int tx = frame_layout->icon.x + icon + xd_tpx(theme, 5);
    frame_layout->title = (xd_rect){tx, outer.y, bx - xd_tpx(theme, 8) - tx, in.top};
}

/* The glass: the colour, a sheen fading down from the top, and faint
 * diagonal streaks, as light catches Windows 7's frames. */
static void paint_glass(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool active,
                        int sheen_h) {
    xd_rgba color = glass(theme, active);
    cairo_set_source_rgba(cairo, color.r, color.g, color.b, color.a);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_fill(cairo);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, rect.y, 0, rect.y + sheen_h);
    cairo_pattern_add_color_stop_rgba(pattern, 0, 1, 1, 1, active ? 0.42 : 0.35);
    cairo_pattern_add_color_stop_rgba(pattern, 0.5, 1, 1, 1, 0.14);
    cairo_pattern_add_color_stop_rgba(pattern, 1, 1, 1, 1, 0.0);
    cairo_set_source(cairo, pattern);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, sheen_h);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
    if (!theme->glass || xd_pkgstyle_dwm_has_reflections(theme))
        return; /* Basic: no reflections; a package's are its own */
    /* Streaks every ~180px, leaning right. */
    double step = xd_tpx(theme, 180), wide = xd_tpx(theme, 36);
    for (double x = rect.x - rect.h; x < rect.x + rect.w; x += step) {
        cairo_move_to(cairo, x, rect.y + rect.h);
        cairo_line_to(cairo, x + rect.h * 0.6, rect.y);
        cairo_line_to(cairo, x + rect.h * 0.6 + wide, rect.y);
        cairo_line_to(cairo, x + wide, rect.y + rect.h);
        cairo_close_path(cairo);
        cairo_set_source_rgba(cairo, 1, 1, 1, active ? 0.07 : 0.05);
        cairo_fill(cairo);
    }
}

/* The title on Windows 7's soft white glow. */
static void draw_title(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *title,
                       bool active) {
    if (title == nullptr || *title == '\0' || rect.w <= 0)
        return;
    xd_text_style st = {
        .font = &title_font,
        .scale = theme->scale,
        .color = active ? xd_rgba_hex(0x000000) : xd_rgba_hex(0x4A4A4A),
        .align = XD_ALIGN_START,
        .ellipsize = true,
        .smoothing = theme->scheme.font_smoothing,
    };
    PangoLayout *title_layout = xd_text_layout(cairo, title, &st);
    pango_layout_set_width(title_layout, rect.w * PANGO_SCALE);
    pango_layout_set_ellipsize(title_layout, PANGO_ELLIPSIZE_END);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(title_layout, &tw, &th);
    /* Centre the capitals, as Windows does, not the line with its room for
     * descenders (that sits the text high); an "H" gives their height
     * whatever the title's letters. Whole pixels keep the text crisp. */
    PangoLayout *caps_layout = xd_text_layout(cairo, "H", &st);
    PangoRectangle caps;
    pango_layout_get_pixel_extents(caps_layout, &caps, nullptr);
    g_object_unref(caps_layout);
    double x = rect.x, y = rect.y + (rect.h - caps.height) / 2 - caps.y;
    /* The glow: a soft white pill under the text. */
    double pad = xd_tpx(theme, 10);
    for (int i = 0; i < 6; i++) {
        double grow = pad * (1.0 - i / 6.0);
        xd_path_rounded(cairo, x - grow, y + th * 0.15 - grow / 2, tw + 2 * grow, th * 0.7 + grow,
                        (th * 0.7 + grow) / 2, (th * 0.7 + grow) / 2, (th * 0.7 + grow) / 2,
                        (th * 0.7 + grow) / 2);
        cairo_set_source_rgba(cairo, 1, 1, 1, active ? 0.10 : 0.07);
        cairo_fill(cairo);
    }
    xd_set_color(cairo, st.color);
    cairo_move_to(cairo, x, y);
    pango_cairo_show_layout(cairo, title_layout);
    g_object_unref(title_layout);
}

/* A glossy glass button face: a brighter top half over a softer bottom. */
static void gloss(cairo_t *cairo, xd_rect rect, xd_rgba top0, xd_rgba top1, xd_rgba bot0,
                  xd_rgba bot1) {
    double mid = rect.y + rect.h * 0.48;
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    double split = (mid - rect.y) / rect.h;
    cairo_pattern_add_color_stop_rgba(pattern, 0, top0.r, top0.g, top0.b, top0.a);
    cairo_pattern_add_color_stop_rgba(pattern, split, top1.r, top1.g, top1.b, top1.a);
    cairo_pattern_add_color_stop_rgba(pattern, split, bot0.r, bot0.g, bot0.b, bot0.a);
    cairo_pattern_add_color_stop_rgba(pattern, 1, bot1.r, bot1.g, bot1.b, bot1.a);
    cairo_set_source(cairo, pattern);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(pattern);
}

/* ---- controls, from aero.msstyles' measured artwork --------------------------------- */

/* Normal 0, hot 1, pressed 2, disabled 3: the images' first four states. */
static int aero_state(uint8_t state) {
    return (state & XD_STATE_DISABLED) ? 3
           : (state & XD_STATE_PRESSED) ? 2
           : (state & XD_STATE_HOT)     ? 1
                                        : 0;
}

static void set_rgb(cairo_t *cairo, uint32_t rgb, double alpha) {
    cairo_set_source_rgba(cairo, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                          (rgb & 0xFF) / 255.0, alpha);
}

/* Windows 7's glyphs (caption buttons, arrows) are white or dark shapes
 * with a soft outline: a shape of rectangles in a w x h box designed for
 * 96 DPI, drawn at the monitor's resolution, centred in r. The outline
 * reaches `ox` px to each side and `oy` above and below, its corners
 * half covered. */
typedef struct aero_rect {
    int8_t x0, y0, x1, y1; /* inclusive */
} aero_rect;

typedef struct aero_glyph {
    int w, h;
    aero_rect r[12];
    int count;
} aero_glyph;

static void draw_aero_glyph(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                            const aero_glyph *glyph, uint32_t fill, uint32_t outline, double alpha,
                            int ox, int oy) {
    double k = theme->scale;
    int width = (int)lround(glyph->w * k), height = (int)lround(glyph->h * k);
    int x0 = rect.x + (rect.w - width) / 2, y0 = rect.y + (rect.h - height) / 2;
    int dx = (int)lround(ox * k), dy = (int)lround(oy * k);
    cairo_surface_t *a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    cairo_t *mask_cairo = cairo_create(a8);
    for (int i = 0; i < glyph->count; i++) {
        const aero_rect *part_rect = &glyph->r[i];
        int ax = (int)lround(part_rect->x0 * k), ay = (int)lround(part_rect->y0 * k);
        int bx = (int)lround((part_rect->x1 + 1) * k), by = (int)lround((part_rect->y1 + 1) * k);
        cairo_rectangle(mask_cairo, ax, ay, bx - ax, by - ay);
    }
    cairo_fill(mask_cairo);
    cairo_destroy(mask_cairo);
    cairo_surface_flush(a8);
    /* The outline: the shape spread out, the shape itself left clear. */
    const uint8_t *mask_data = cairo_image_surface_get_data(a8);
    int stride = cairo_image_surface_get_stride(a8);
    cairo_surface_t *ring = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    cairo_surface_flush(ring);
    uint8_t *ring_data = cairo_image_surface_get_data(ring);
    int ostride = cairo_image_surface_get_stride(ring);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (mask_data[y * stride + x])
                continue;
            double cover = 0;
            for (int j = -dy; j <= dy && cover < 1; j++) {
                for (int i = -dx; i <= dx; i++) {
                    int sx = x + i, sy = y + j;
                    if (sx < 0 || sy < 0 || sx >= width || sy >= height ||
                        !mask_data[sy * stride + sx])
                        continue;
                    bool corner = (i == -dx || i == dx) && (j == -dy || j == dy) && dy > 0;
                    cover = fmax(cover, corner ? 0.5 : 1);
                }
            }
            ring_data[y * ostride + x] = (uint8_t)lround(cover * 255);
        }
    }
    cairo_surface_mark_dirty(ring);
    set_rgb(cairo, outline, alpha);
    cairo_mask_surface(cairo, ring, x0, y0);
    set_rgb(cairo, fill, alpha);
    cairo_mask_surface(cairo, a8, x0, y0);
    cairo_surface_destroy(ring);
    cairo_surface_destroy(a8);
}

/* The caption glyphs, from Windows 7's (96 DPI): close, maximize,
 * minimize, restore, help. */
static const aero_glyph caption_glyphs[5] = {
    {13, 14,
     {{2, 3, 4, 3}, {8, 3, 10, 3}, {3, 4, 5, 4}, {7, 4, 9, 4}, {4, 5, 8, 5}, {5, 6, 7, 7},
      {4, 8, 8, 8}, {3, 9, 5, 9}, {7, 9, 9, 9}, {2, 10, 4, 10}, {8, 10, 10, 10}},
     11},
    {16, 14, {{4, 3, 13, 4}, {4, 5, 5, 8}, {12, 5, 13, 8}, {4, 9, 13, 10}}, 4},
    {14, 14, {{2, 8, 11, 10}}, 1},
    {14, 14,
     {{4, 2, 11, 2}, {11, 3, 11, 9}, {2, 4, 9, 5}, {2, 6, 3, 9}, {8, 6, 9, 9}, {2, 10, 9, 11}},
     6},
    {10, 14,
     {{3, 2, 6, 2}, {2, 3, 7, 3}, {2, 4, 3, 4}, {6, 4, 7, 4}, {6, 5, 7, 5}, {5, 6, 7, 6},
      {4, 7, 6, 7}, {4, 8, 5, 8}, {4, 10, 5, 11}},
     9},
};

static int caption_glyph_index(xd_frame_part part, bool maximized) {
    return part == XD_PART_CLOSE      ? 0
           : part == XD_PART_MINIMIZE ? 2
           : part == XD_PART_HELP     ? 4
           : maximized                ? 3
                                      : 1;
}

/* ---- glass caption buttons ---------------------------------------------------------- */

/* Windows 7's glass caption buttons, drawn: a strip hanging from the top
 * edge, outlined dark with its bottom outer corners rounded, a dark line
 * between buttons with a light one either side of it. A face is split
 * a little above its middle, light glass above and darker below, which
 * lightens towards a light line at the bottom; hot and pressed faces are
 * nearly opaque (blue, cyan-lit; Close's red, yellow-lit), a normal one
 * translucent, and an inactive window's clear. Colours and alphas are
 * read off Windows 7's images at their middle column (deduced, no
 * pixels), down the face: upper top and bottom, lower top, two below it
 * and the bottom line. */
typedef struct glass_face {
    uint32_t upper[2], lower[3], line;
    double alpha, line_alpha;
} glass_face;

static const glass_face GLASS_NORMAL = {
    {0xF4F4F4, 0xD3D3D3}, {0x000000, 0x21212B, 0x8F96AD}, 0xEAECF1, 0.50, 0.58};
static const glass_face GLASS_HOT = {
    {0xB7DBF2, 0x97C8E4}, {0x2881BB, 0x18AAC9, 0x2ED8E5}, 0x0AFEFB, 0.96, 1.0};
static const glass_face GLASS_PRESSED = {
    {0xA2BBCD, 0x5FAAB9}, {0x0D6F80, 0x099997, 0x16D6C5}, 0x0AFDFA, 1.0, 1.0};
static const glass_face GLASS_CLOSE = {
    {0xFCA898, 0xEB7561}, {0xD02102, 0xD02E0F, 0xDE704E}, 0xF2B29E, 0.80, 0.80};
static const glass_face GLASS_CLOSE_HOT = {
    {0xFB9D8B, 0xF1745E}, {0xD22202, 0xD63209, 0xE6942F}, 0xF5E477, 1.0, 1.0};
static const glass_face GLASS_CLOSE_PRESSED = {
    {0xD2AA93, 0xBE7F64}, {0x871700, 0x872A01, 0x8F6C20}, 0xEDC72C, 1.0, 1.0};

static void add_stop(cairo_pattern_t *pattern, double at, uint32_t rgb, double alpha) {
    cairo_pattern_add_color_stop_rgba(pattern, at, ((rgb >> 16) & 0xFF) / 255.0,
                                      ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

/* The button's outline as a path: down its left side if it begins the
 * strip, along the bottom, up its right side, corners rounded where it
 * ends the strip. Inset half a line so strokes land on whole pixels. */
static void glass_outline_path(cairo_t *cairo, xd_rect rect, double line, double radius,
                               bool leftmost, bool rightmost) {
    double half = line / 2;
    double left = rect.x + half, right = rect.x + rect.w - half;
    double top = rect.y, bottom = rect.y + rect.h - half;
    double rl = leftmost ? radius : 0, rr = rightmost ? radius : 0;
    cairo_new_path(cairo);
    if (leftmost)
        cairo_move_to(cairo, left, top);
    else
        cairo_move_to(cairo, rect.x, bottom);
    if (leftmost) {
        cairo_line_to(cairo, left, bottom - rl);
        if (rl > 0)
            cairo_arc_negative(cairo, left + rl, bottom - rl, rl, XD_PI, XD_PI / 2);
    }
    cairo_line_to(cairo, right - rr, bottom);
    if (rr > 0)
        cairo_arc_negative(cairo, right - rr, bottom - rr, rr, XD_PI / 2, 0);
    cairo_line_to(cairo, right, top);
}

static void draw_glass_caption_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                      bool close, int state_index, bool active, bool leftmost,
                                      bool rightmost) {
    double line = XD_MAX(xd_tpx(theme, 1), 1);
    double radius = xd_tpx(theme, 3);
    bool hot = state_index == 1, pressed = state_index == 2;
    /* A hot button glows round its edge, behind the strip. */
    if (hot) {
        double spread = xd_tpx(theme, 9);
        double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h * 0.6;
        cairo_pattern_t *glow = cairo_pattern_create_radial(cx, cy, rect.h * 0.3, cx, cy,
                                                            rect.w / 2.0 + spread);
        uint32_t tint = close ? 0xF0602E : 0x4ADFF0;
        add_stop(glow, 0, tint, 0.55);
        add_stop(glow, 1, tint, 0);
        cairo_save(cairo);
        cairo_translate(cairo, cx, cy);
        cairo_scale(cairo, 1, (rect.h / 2.0 + spread / 2) / (rect.w / 2.0 + spread));
        cairo_translate(cairo, -cx, -cy);
        cairo_set_source(cairo, glow);
        cairo_arc(cairo, cx, cy, rect.w / 2.0 + spread, 0, 2 * XD_PI);
        cairo_fill(cairo);
        cairo_restore(cairo);
        cairo_pattern_destroy(glow);
    }

    cairo_save(cairo);
    xd_path_rounded(cairo, rect.x, rect.y, rect.w, rect.h, 0, 0, rightmost ? radius : 0,
                    leftmost ? radius : 0);
    cairo_clip(cairo);
    /* The face: an inactive window's is clear unless hot or pressed. */
    const glass_face *face = close ? (hot       ? &GLASS_CLOSE_HOT
                                      : pressed ? &GLASS_CLOSE_PRESSED
                                                : &GLASS_CLOSE)
                             : hot     ? &GLASS_HOT
                             : pressed ? &GLASS_PRESSED
                                       : &GLASS_NORMAL;
    bool clear = !active && !hot && !pressed;
    double split = rect.y + round(rect.h * 0.45);
    double bottom = rect.y + rect.h - line; /* the outline's row */
    if (!clear) {
        cairo_pattern_t *upper = cairo_pattern_create_linear(0, rect.y, 0, split);
        add_stop(upper, 0, face->upper[0], face->alpha * (close ? 1 : 1.14));
        add_stop(upper, 1, face->upper[1], face->alpha * (close ? 1 : 0.92));
        cairo_set_source(cairo, upper);
        cairo_rectangle(cairo, rect.x, rect.y, rect.w, split - rect.y);
        cairo_fill(cairo);
        cairo_pattern_destroy(upper);
        cairo_pattern_t *lower = cairo_pattern_create_linear(0, split, 0, bottom - line);
        /* A normal min/max face is barely tinted at the split, more so at
         * the bottom (Windows 7's alphas: 46 to 78). */
        bool tinted = face == &GLASS_NORMAL;
        add_stop(lower, 0, face->lower[0], tinted ? 0.18 : face->alpha);
        add_stop(lower, 0.4, face->lower[1], tinted ? 0.21 : face->alpha);
        add_stop(lower, 1, face->lower[2], tinted ? 0.31 : face->alpha);
        cairo_set_source(cairo, lower);
        cairo_rectangle(cairo, rect.x, split, rect.w, bottom - split);
        cairo_fill(cairo);
        cairo_pattern_destroy(lower);
        if (hot || pressed) {
            /* Lit from the bottom middle: its sides darker. */
            cairo_pattern_t *sides = cairo_pattern_create_linear(rect.x, 0, rect.x + rect.w, 0);
            add_stop(sides, 0, 0x000000, 0.28);
            add_stop(sides, 0.3, 0x000000, 0);
            add_stop(sides, 0.7, 0x000000, 0);
            add_stop(sides, 1, 0x000000, 0.28);
            cairo_set_source(cairo, sides);
            cairo_rectangle(cairo, rect.x, split, rect.w, bottom - split);
            cairo_fill(cairo);
            cairo_pattern_destroy(sides);
        }
    }
    /* The light line along the bottom, and up either side inside the
     * outline (fainter on an inactive window's clear glass). */
    set_rgb(cairo, clear ? 0xFEFEFE : face->line, clear ? 0.23 : face->line_alpha);
    cairo_rectangle(cairo, rect.x, bottom - line, rect.w, line);
    cairo_fill(cairo);
    double inner_alpha = clear ? 0.35 : 0.7;
    cairo_pattern_t *inner = cairo_pattern_create_linear(0, rect.y, 0, bottom);
    add_stop(inner, 0, 0xFCFCFC, inner_alpha);
    add_stop(inner, 1, 0xDADADA, inner_alpha * 0.75);
    cairo_set_source(cairo, inner);
    cairo_rectangle(cairo, rect.x + (leftmost ? line : 0), rect.y, line, bottom - rect.y);
    cairo_rectangle(cairo, rect.x + rect.w - 2 * line, rect.y, line, bottom - rect.y);
    cairo_fill(cairo);
    cairo_pattern_destroy(inner);
    /* The top rim, light across the strip's top. */
    set_rgb(cairo, 0xFCFCFC, active ? 0.85 : 0.6);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, line);
    cairo_fill(cairo);
    cairo_restore(cairo);

    /* The outline: grey above, darker below, as Windows 7's. */
    double outline_alpha = active ? 1.0 : 0.7;
    cairo_pattern_t *outline = cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    add_stop(outline, 0, 0x797979, 0.78 * outline_alpha);
    add_stop(outline, 0.45, 0x545454, 0.70 * outline_alpha);
    add_stop(outline, 0.5, 0x000000, 0.52 * outline_alpha);
    add_stop(outline, 1, 0x1B1B1B, 0.69 * outline_alpha);
    cairo_set_source(cairo, outline);
    cairo_set_line_width(cairo, line);
    glass_outline_path(cairo, rect, line, radius, leftmost, rightmost);
    cairo_stroke(cairo);
    cairo_pattern_destroy(outline);
    /* A faint light edge just below the strip. */
    set_rgb(cairo, 0xFEFEFE, active ? 0.45 : 0.3);
    cairo_rectangle(cairo, rect.x + (leftmost ? radius : 0), rect.y + rect.h,
                    rect.w - (leftmost ? radius : 0) - (rightmost ? radius : 0), line);
    cairo_fill(cairo);
}

/* Windows 7's caption buttons: on glass, the strip drawn above; in Basic,
 * the Window class's measured images. The glyph is white, outlined. */
static void aero_caption_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                xd_frame_part part, const xd_frame *frame, bool leftmost,
                                bool rightmost) {
    bool pressed = frame->pressed == part && frame->hot == part;
    bool hot = frame->hot == part && !pressed;
    int state_index = pressed ? 2 : hot ? 1 : 0;
    int glyph_index = caption_glyph_index(part, frame->maximized);
    bool close = part == XD_PART_CLOSE;
    if (theme->glass) {
        draw_glass_caption_button(theme, cairo, rect, close, state_index, frame->active, leftmost,
                                  rightmost);
        /* DWM's glyphs are outlined 2 px at the sides, but Maximize's
         * box 1 px, which leaves it hollow. */
        draw_aero_glyph(theme, cairo, rect, &caption_glyphs[glyph_index],
                        aero_dwm_glyph[glyph_index][state_index][0],
                        aero_dwm_glyph[glyph_index][state_index][1], 1, glyph_index == 1 ? 1 : 2,
                        1);
        return;
    }
    int k = (frame->active ? 0 : 4) + state_index;
    xd_shape_draw(theme, cairo, close ? &aero_basic_close[k] : &aero_basic_button[k], rect);
    draw_aero_glyph(theme, cairo, rect, &caption_glyphs[glyph_index],
                    aero_basic_glyph[glyph_index][k][0], aero_basic_glyph[glyph_index][k][1], 1, 1,
                    1);
}

static void caption_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_frame_part part,
                           const xd_frame *frame, bool first, bool last) {
    bool alone = part == XD_PART_CLOSE && (frame->buttons & ~XD_CAPBTN_CLOSE) == 0;
    if (xd_pkgstyle_dwm_button(theme, cairo, rect, part, frame, first, alone))
        return;
    aero_caption_button(theme, cairo, rect, part, frame, first, last);
}

static void aero_draw_frame(const xd_theme *theme, cairo_t *cairo, xd_rect outer,
                            const xd_frame *frame) {
    xd_insets in = aero_frame_insets(theme, frame);
    double rad = frame->maximized ? 0 : xd_tpx(theme, RADIUS);
    cairo_save(cairo);
    xd_path_rounded(cairo, outer.x, outer.y, outer.w, outer.h, rad, rad, rad, rad);
    cairo_clip(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    paint_glass(theme, cairo, outer, frame->active, in.top + xd_tpx(theme, 14));
    (void)xd_pkgstyle_dwm_reflections(theme, cairo, outer, frame);
    /* Around the client area: a dark line, and a light one outside it. */
    xd_rect client = {outer.x + in.left, outer.y + in.top, outer.w - in.left - in.right,
                      outer.h - in.top - in.bottom};
    /* Nothing under the client area: the compositor blurs only where the
     * frame is drawn. */
    if (client.w > 0 && client.h > 0) {
        cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
        cairo_rectangle(cairo, client.x, client.y, client.w, client.h);
        cairo_fill(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    }
    bool pkg_edges = xd_pkgstyle_dwm_edges(theme, cairo, outer, client, frame);
    /* Lines a logical pixel wide, so they scale with the frame. */
    double px = xd_tpx(theme, 1), half = px / 2;
    if (!pkg_edges && !frame->maximized && client.w > 0 && client.h > 0) {
        cairo_set_line_width(cairo, px);
        cairo_rectangle(cairo, client.x - px - half, client.y - px - half, client.w + 3 * px,
                        client.h + 3 * px);
        cairo_set_source_rgba(cairo, 1, 1, 1, 0.45);
        cairo_stroke(cairo);
        cairo_rectangle(cairo, client.x - half, client.y - half, client.w + px, client.h + px);
        cairo_set_source_rgba(cairo, 0, 0, 0, 0.45);
        cairo_stroke(cairo);
    }
    cairo_restore(cairo);
    if (!pkg_edges && !frame->maximized) {
        /* The outline, and a highlight just inside it. */
        cairo_set_line_width(cairo, px);
        xd_path_rounded(cairo, outer.x + half, outer.y + half, outer.w - px, outer.h - px, rad, rad,
                        rad, rad);
        cairo_set_source_rgba(cairo, 0, 0, 0, frame->active ? 0.65 : 0.45);
        cairo_stroke(cairo);
        xd_path_rounded(cairo, outer.x + px + half, outer.y + px + half, outer.w - 3 * px,
                        outer.h - 3 * px, rad - px, rad - px, rad - px, rad - px);
        cairo_set_source_rgba(cairo, 1, 1, 1, frame->active ? 0.55 : 0.4);
        cairo_stroke(cairo);
    }
    xd_frame_layout frame_layout = {0};
    aero_frame_layout(theme, outer, frame, &frame_layout);
    /* Laid out right to left (the last is leftmost), drawn left to right:
     * on glass each button overlaps the one before. */
    for (size_t i = frame_layout.button_count; i-- > 0;) {
        caption_button(theme, cairo, frame_layout.button_rects[i], frame_layout.button_parts[i],
                       frame, i + 1 == frame_layout.button_count, i == 0);
    }
    if (frame->icon != nullptr)
        xd_style_task_icon(theme, cairo, frame->icon, frame_layout.icon.x, frame_layout.icon.y);
    draw_title(theme, cairo, frame_layout.title, frame->title, frame->active);
}

static xd_rgba aero_dialog_color([[maybe_unused]] const xd_theme *theme) {
    return xd_rgba_hex(0xF0F0F0);
}

/* ---- push buttons and check boxes ------------------------------------------------- */

static void aero_draw_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                             const char *label) {
    bool disabled = state & XD_STATE_DISABLED;
    /* Normal, hot, pressed, disabled, then a default (or focused) one's. */
    int state_index = aero_state(state);
    if (state_index == 0 && (state & (XD_STATE_DEFAULT | XD_STATE_FOCUSED)))
        state_index = 4;
    xd_shape_draw(theme, cairo, &aero_pushbutton[state_index], rect);
    if (label != nullptr) {
        xd_text_style st = xd_text(theme, XD_FONT_MESSAGE,
                                   xd_rgba_hex(disabled ? 0x838383 : 0x000000), XD_ALIGN_CENTER);
        xd_draw_text(cairo, rect, label, &st);
    }
    if (state & XD_STATE_FOCUSED)
        xd_theme_draw_focus_rect(theme, cairo,
                                 xd_rect_inset(rect, xd_tpx(theme, 3), xd_tpx(theme, 3)));
}

/* The tick: a dark stroke on a soft white halo. */
static void draw_tick(cairo_t *cairo, xd_rect rect, uint32_t rgb) {
    double k = rect.w / 13.0;
    cairo_save(cairo);
    cairo_translate(cairo, rect.x, rect.y);
    cairo_scale(cairo, k, k);
    cairo_move_to(cairo, 3.0, 7.2);
    cairo_line_to(cairo, 5.3, 9.3);
    cairo_line_to(cairo, 9.4, 3.3);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cairo, 3.6);
    cairo_set_source_rgba(cairo, 1, 1, 1, 0.75);
    cairo_stroke_preserve(cairo);
    cairo_set_line_width(cairo, 1.7);
    set_rgb(cairo, rgb, 1);
    cairo_stroke(cairo);
    cairo_restore(cairo);
}

static void aero_draw_checkbox(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                               const char *label) {
    bool disabled = state & XD_STATE_DISABLED;
    int state_index = aero_state(state);
    int box = xd_tpx(theme, 13);
    xd_rect box_rect = {rect.x, rect.y + (rect.h - box) / 2, box, box};
    xd_shape_draw(theme, cairo, &aero_check[state_index], box_rect);
    if (state & XD_STATE_CHECKED)
        draw_tick(cairo, box_rect, aero_tick[state_index]);
    if (label != nullptr) {
        xd_text_style st = xd_text(theme, XD_FONT_MESSAGE,
                                   xd_rgba_hex(disabled ? 0x6D6D6D : 0x000000), XD_ALIGN_START);
        int tx = box_rect.x + box + xd_tpx(theme, 5);
        xd_rect text = {tx, rect.y, rect.x + rect.w - tx, rect.h};
        xd_draw_text(cairo, text, label, &st);
        if (state & XD_STATE_FOCUSED) {
            int width = XD_MIN(xd_text_width(cairo, label, &st), text.w);
            xd_theme_draw_focus_rect(theme, cairo,
                                     (xd_rect){text.x - xd_tpx(theme, 1), rect.y + xd_tpx(theme, 2),
                                               width + xd_tpx(theme, 3),
                                               rect.h - xd_tpx(theme, 4)});
        }
    }
}

void xd_aero_draw_radio([[maybe_unused]] const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                        uint8_t state) {
    int state_index = aero_state(state) + ((state & XD_STATE_CHECKED) ? 4 : 0);
    const uint32_t *palette_row = aero_radio[state_index];
    double unit = rect.w / 13.0;
    cairo_save(cairo);
    cairo_translate(cairo, rect.x, rect.y);
    cairo_scale(cairo, unit, unit);
    double cx = 6.0, cy = 6.5;
    /* The face: lit from the bottom right inside a light ring. */
    cairo_pattern_t *face = cairo_pattern_create_linear(3, 3.5, 9, 9.5);
    for (int i = 0; i < 2; i++)
        cairo_pattern_add_color_stop_rgb(face, i, ((palette_row[2 + i] >> 16) & 0xFF) / 255.0,
                                         ((palette_row[2 + i] >> 8) & 0xFF) / 255.0,
                                         (palette_row[2 + i] & 0xFF) / 255.0);
    cairo_arc(cairo, cx, cy, 5.5, 0, 2 * G_PI);
    set_rgb(cairo, palette_row[1], 1);
    cairo_fill(cairo);
    cairo_arc(cairo, cx, cy, 4.0, 0, 2 * G_PI);
    cairo_set_source(cairo, face);
    cairo_fill(cairo);
    cairo_pattern_destroy(face);
    cairo_set_line_width(cairo, 1);
    cairo_arc(cairo, cx, cy, 5.5, 0, 2 * G_PI);
    set_rgb(cairo, palette_row[0], 1);
    cairo_stroke(cairo);
    if (state & XD_STATE_CHECKED) {
        /* A glassy ball in a dark rim, lit from the top left. */
        cairo_arc(cairo, cx, cy, 3.4, 0, 2 * G_PI);
        set_rgb(cairo, palette_row[4], 1);
        cairo_fill(cairo);
        cairo_pattern_t *ball = cairo_pattern_create_radial(cx - 0.9, cy - 1.3, 0, cx, cy, 2.6);
        for (int i = 0; i < 3; i++)
            cairo_pattern_add_color_stop_rgb(
                ball, i / 2.0, ((palette_row[5 + i] >> 16) & 0xFF) / 255.0,
                ((palette_row[5 + i] >> 8) & 0xFF) / 255.0, (palette_row[5 + i] & 0xFF) / 255.0);
        cairo_arc(cairo, cx, cy, 2.6, 0, 2 * G_PI);
        cairo_set_source(cairo, ball);
        cairo_fill(cairo);
        cairo_pattern_destroy(ball);
    }
    cairo_restore(cairo);
}

xd_rect xd_aero_draw_field(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state) {
    bool disabled = state & XD_STATE_DISABLED;
    /* Normal, Hot (light blue), Focused (blue), Disabled. */
    int state_index = disabled                      ? 3
                      : (state & XD_STATE_FOCUSED) ? 2
                      : (state & XD_STATE_HOT)     ? 1
                                                   : 0;
    xd_fill_rect(cairo, rect, xd_rgba_hex(disabled ? 0xF0F0F0 : 0xFFFFFF));
    xd_shape_draw(theme, cairo, &aero_field[state_index], rect);
    return xd_rect_inset(rect, xd_tpx(theme, 1), xd_tpx(theme, 1));
}

/* A small triangle pointing down (or up), centred in r. */
static void aero_triangle(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool up,
                          uint32_t rgb, int width) {
    double triangle_width = xd_tpx(theme, width), height = xd_tpx(theme, (width + 1) / 2);
    double x = rect.x + (rect.w - triangle_width) / 2.0, y = rect.y + (rect.h - height) / 2.0;
    double py = up ? y + height : y;
    double ty = up ? y : y + height;
    /* A faint white halo first, as Windows 7's arrows have. */
    cairo_move_to(cairo, x - xd_tpx(theme, 1), py);
    cairo_line_to(cairo, x + triangle_width + xd_tpx(theme, 1), py);
    cairo_line_to(cairo, x + triangle_width / 2, ty + (up ? -xd_tpx(theme, 1) : xd_tpx(theme, 1)));
    cairo_close_path(cairo);
    cairo_set_source_rgba(cairo, 1, 1, 1, 0.36);
    cairo_fill(cairo);
    cairo_move_to(cairo, x, py);
    cairo_line_to(cairo, x + triangle_width, py);
    cairo_line_to(cairo, x + triangle_width / 2, ty);
    cairo_close_path(cairo);
    set_rgb(cairo, rgb, 1);
    cairo_fill(cairo);
}

void xd_aero_draw_dropdown_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state) {
    int state_index = aero_state(state);
    xd_shape_draw(theme, cairo, &aero_dropdown[state_index], rect);
    aero_triangle(theme, cairo, rect, false, aero_dropdown_glyph[state_index], 7);
}

void xd_aero_draw_vscrollbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_rect thumb,
                             bool disabled) {
    int state_index = disabled ? 3 : 0;
    int arrow = XD_MIN(rect.w, rect.h / 2);
    xd_rect up = {rect.x, rect.y, rect.w, arrow};
    xd_rect down = {rect.x, rect.y + rect.h - arrow, rect.w, arrow};
    xd_shape_draw(theme, cairo, &aero_scrolltrack[state_index], rect);
    xd_shape_draw(theme, cairo, &aero_scrollarrow[state_index], up);
    xd_shape_draw(theme, cairo, &aero_scrollarrow[4 + state_index], down);
    aero_triangle(theme, cairo, up, true, aero_scrollarrow_glyph[state_index], 7);
    aero_triangle(theme, cairo, down, false, aero_scrollarrow_glyph[4 + state_index], 7);
    if (thumb.h <= 0 || disabled)
        return;
    xd_shape_draw(theme, cairo, &aero_scrollthumb[0], thumb);
    /* The gripper: four dark lines over light ones, where there's room. */
    int gw = xd_tpx(theme, 7), line = xd_tpx(theme, 1);
    if (thumb.h < xd_tpx(theme, 16))
        return;
    int gx = thumb.x + (thumb.w - gw) / 2, gy = thumb.y + (thumb.h - 7 * line) / 2;
    for (int i = 0; i < 4; i++) {
        xd_fill_rect(cairo, (xd_rect){gx, gy + 2 * i * line, gw, line},
                     xd_rgba_hex(aero_gripper[0]));
        xd_fill_rect(cairo, (xd_rect){gx + line, gy + (2 * i + 1) * line, gw, line},
                     xd_rgba_hex(aero_gripper[1]));
    }
}

void xd_aero_draw_tab(const xd_theme *theme, cairo_t *cairo, xd_rect tab, uint8_t state) {
    bool selected = state & XD_STATE_CHECKED;
    int state_index = selected                      ? 2
                      : (state & XD_STATE_DISABLED) ? 3
                      : (state & XD_STATE_HOT)      ? 1
                                                    : 0;
    xd_shape_draw(theme, cairo, selected ? &aero_tabtop[state_index] : &aero_tab[state_index], tab);
}

/* A white page in a grey line (aero.msstyles' Tab Pane: 137 140 149). */
void xd_aero_draw_tab_panel(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    xd_fill_rect(cairo, rect, xd_rgba_hex(0xFFFFFF));
    xd_rgba line = xd_rgba_hex(0x898C95);
    xd_draw_bevel(cairo, rect, xd_tpx(theme, 1), line, line);
}

void xd_aero_draw_groupbox_frame(const xd_theme *theme, cairo_t *cairo, xd_rect box) {
    xd_shape_draw(theme, cairo, aero_groupbox, box);
}

/* ---- taskbar ----------------------------------------------------------------------- */

static int aero_taskbar_height(const xd_theme *theme) { return xd_tpx(theme, TASKBAR_H); }

static void aero_draw_taskbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_edge edge) {
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    xd_glass_colors glass_colors;
    xd_theme_glass_colors(theme, &glass_colors);
    xd_set_color(cairo, glass_colors.taskbar);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_fill(cairo);
    cairo_restore(cairo);
    /* A sheen over the half nearest the desktop, and Windows 7's rim. */
    bool vertical = edge == XD_EDGE_LEFT || edge == XD_EDGE_RIGHT;
    cairo_pattern_t *pattern = vertical
                                   ? cairo_pattern_create_linear(rect.x, 0, rect.x + rect.w, 0)
                                   : cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    cairo_pattern_add_color_stop_rgba(pattern, 0, 1, 1, 1, 0.16);
    cairo_pattern_add_color_stop_rgba(pattern, 0.5, 1, 1, 1, 0.06);
    cairo_pattern_add_color_stop_rgba(pattern, 0.5, 1, 1, 1, 0.0);
    cairo_pattern_add_color_stop_rgba(pattern, 1, 1, 1, 1, 0.04);
    cairo_set_source(cairo, pattern);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
    int px = xd_tpx(theme, 1);
    xd_fill_rect(cairo, xd_style_rim_line(rect, edge, 0, px),
                 xd_rgba_alpha(xd_rgba_hex(0x000000), 0.5));
    xd_fill_rect(cairo, xd_style_rim_line(rect, edge, px, px),
                 xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.38));
}

static int aero_start_button_width(const xd_theme *theme, [[maybe_unused]] cairo_t *cairo) {
    return xd_tpx(theme, ORB_W);
}

/* The Start orb: a glossy dark-blue sphere with the logo. */
static void aero_draw_start_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                   uint8_t state) {
    double diameter = XD_MIN(rect.w, rect.h) - xd_tpx(theme, 2);
    double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h / 2.0, rad = diameter / 2;
    bool hot = state & XD_STATE_HOT, pressed = state & XD_STATE_PRESSED;
    if (hot || pressed) {
        cairo_pattern_t *glow = cairo_pattern_create_radial(cx, cy, rad * 0.6, cx, cy, rad * 1.3);
        cairo_pattern_add_color_stop_rgba(glow, 0, 0.55, 0.85, 1.0, pressed ? 0.3 : 0.55);
        cairo_pattern_add_color_stop_rgba(glow, 1, 0.55, 0.85, 1.0, 0);
        cairo_set_source(cairo, glow);
        cairo_arc(cairo, cx, cy, rad * 1.3, 0, 2 * G_PI);
        cairo_fill(cairo);
        cairo_pattern_destroy(glow);
    }
    cairo_pattern_t *body = cairo_pattern_create_radial(cx, cy + rad * 0.45, rad * 0.1, cx, cy,
                                                        rad);
    double dim = pressed ? 0.75 : 1.0;
    cairo_pattern_add_color_stop_rgb(body, 0, 0.30 * dim, 0.62 * dim, 0.95 * dim);
    cairo_pattern_add_color_stop_rgb(body, 0.55, 0.10 * dim, 0.30 * dim, 0.60 * dim);
    cairo_pattern_add_color_stop_rgb(body, 1, 0.03 * dim, 0.10 * dim, 0.24 * dim);
    cairo_set_source(cairo, body);
    cairo_arc(cairo, cx, cy, rad, 0, 2 * G_PI);
    cairo_fill(cairo);
    cairo_pattern_destroy(body);
    /* The gloss over the top half. */
    cairo_save(cairo);
    cairo_translate(cairo, cx, cy - rad * 0.42);
    cairo_scale(cairo, rad * 0.78, rad * 0.5);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, -1, 0, 1);
    cairo_pattern_add_color_stop_rgba(pattern, 0, 1, 1, 1, 0.75);
    cairo_pattern_add_color_stop_rgba(pattern, 1, 1, 1, 1, 0.08);
    cairo_set_source(cairo, pattern);
    cairo_arc(cairo, 0, 0, 1, 0, 2 * G_PI);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
    cairo_restore(cairo);
    cairo_set_line_width(cairo, xd_tpx(theme, 1));
    cairo_arc(cairo, cx, cy, rad - 0.5, 0, 2 * G_PI);
    cairo_set_source_rgba(cairo, 0.75, 0.9, 1.0, 0.65);
    cairo_stroke(cairo);
    int logo = (int)(diameter * 0.55);
    xd_draw_start_logo(cairo,
                       (xd_rect){(int)(cx - logo / 2.0), (int)(cy - logo / 2.0), logo, logo});
}

static void aero_draw_task_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state, const char *label, cairo_surface_t *icon) {
    bool active = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    bool hot = state & XD_STATE_HOT;
    bool flash = (state & XD_STATE_FLASH) && !active;
    xd_rect button = xd_rect_inset(rect, xd_tpx(theme, 1), xd_tpx(theme, 2));
    double rad = xd_tpx(theme, 3);
    xd_path_rounded(cairo, button.x + 0.5, button.y + 0.5, button.w - 1, button.h - 1, rad, rad,
                    rad, rad);
    if (flash)
        gloss(cairo, button, (xd_rgba){1, 0.80, 0.45, 0.85}, (xd_rgba){1, 0.62, 0.20, 0.80},
              (xd_rgba){0.95, 0.48, 0.08, 0.80}, (xd_rgba){1, 0.62, 0.20, 0.85});
    else
        gloss(cairo, button,
              (xd_rgba){1, 1, 1,
                        active ? 0.34
                        : hot  ? 0.28
                               : 0.16},
              (xd_rgba){1, 1, 1,
                        active ? 0.18
                        : hot  ? 0.14
                               : 0.06},
              (xd_rgba){1, 1, 1,
                        active ? 0.08
                        : hot  ? 0.06
                               : 0.0},
              (xd_rgba){1, 1, 1,
                        active ? 0.20
                        : hot  ? 0.18
                               : 0.06});
    cairo_set_line_width(cairo, 1);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0.45);
    cairo_stroke(cairo);
    xd_path_rounded(cairo, button.x + 1.5, button.y + 1.5, button.w - 3, button.h - 3, rad - 1,
                    rad - 1, rad - 1, rad - 1);
    cairo_set_source_rgba(cairo, 1, 1, 1, active ? 0.55 : 0.30);
    cairo_stroke(cairo);
    int isz = xd_style_task_icon_size(theme, icon);
    int ix = label != nullptr ? button.x + xd_tpx(theme, 8) : button.x + (button.w - isz) / 2;
    xd_style_task_icon(theme, cairo, icon, ix, button.y + (button.h - isz) / 2);
    if (label == nullptr)
        return;
    xd_text_style st = xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_START);
    st.shadow = true;
    st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    int tx = ix + isz + xd_tpx(theme, 6);
    xd_draw_text(cairo,
                 (xd_rect){tx, button.y, button.x + button.w - tx - xd_tpx(theme, 6), button.h},
                 label, &st);
}

/* A glass button outline at b: a dark edge outside a light one. */
static void app_button_edges(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                             double inner_alpha) {
    double rad = xd_tpx(theme, 3);
    xd_path_rounded(cairo, rect.x + 0.5, rect.y + 0.5, rect.w - 1, rect.h - 1, rad, rad, rad, rad);
    cairo_set_line_width(cairo, 1);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0.45);
    cairo_stroke(cairo);
    xd_path_rounded(cairo, rect.x + 1.5, rect.y + 1.5, rect.w - 3, rect.h - 3, rad - 1, rad - 1,
                    rad - 1, rad - 1);
    cairo_set_source_rgba(cairo, 1, 1, 1, inner_alpha);
    cairo_stroke(cairo);
}

/* Windows 7's button for a program. A running program's has a glass
 * frame (lit while one of its windows is active), and more windows show
 * as edges stacked behind it; a pinned program that isn't running is its
 * icon alone. Hovering lights the glass with the icon's own colour,
 * glowing up from the bottom where the pointer is. */
static void aero_draw_app_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                 const xd_app_button *ab) {
    bool active = state & XD_STATE_CHECKED;
    bool pressed = state & XD_STATE_PRESSED;
    bool hot = state & XD_STATE_HOT;
    bool flash = (state & XD_STATE_FLASH) && !active;
    int stack = XD_CLAMP(ab->windows - 1, 0, 2); /* Windows 7 shows two at most */
    int step = xd_tpx(theme, 3);
    xd_rect button = xd_rect_inset(rect, xd_tpx(theme, 1), xd_tpx(theme, 1));
    button.w -= stack * step;
    double rad = xd_tpx(theme, 3);
    if (ab->windows > 0 || hot || pressed || flash) {
        /* The farthest edge first, each showing only past the one in front. */
        for (int k = stack; k >= 1; k--) {
            xd_rect edge = {button.x + k * step, button.y, button.w, button.h};
            cairo_save(cairo);
            int from = button.x + button.w + (k - 1) * step - xd_tpx(theme, 1);
            cairo_rectangle(cairo, from, edge.y, edge.x + edge.w - from + 1, edge.h);
            cairo_clip(cairo);
            xd_path_rounded(cairo, edge.x + 0.5, edge.y + 0.5, edge.w - 1, edge.h - 1, rad, rad,
                            rad, rad);
            cairo_set_source_rgba(cairo, 1, 1, 1, 0.10);
            cairo_fill(cairo);
            app_button_edges(theme, cairo, edge, 0.25);
            cairo_restore(cairo);
        }
        double lit = pressed ? 0.6 : active ? 1.0 : hot ? 0.8 : 0.45;
        xd_path_rounded(cairo, button.x + 0.5, button.y + 0.5, button.w - 1, button.h - 1, rad, rad,
                        rad, rad);
        if (flash)
            gloss(cairo, button, (xd_rgba){1, 0.80, 0.45, 0.85}, (xd_rgba){1, 0.62, 0.20, 0.80},
                  (xd_rgba){0.95, 0.48, 0.08, 0.80}, (xd_rgba){1, 0.62, 0.20, 0.85});
        else
            gloss(cairo, button, (xd_rgba){1, 1, 1, 0.36 * lit}, (xd_rgba){1, 1, 1, 0.18 * lit},
                  (xd_rgba){1, 1, 1, 0.08 * lit}, (xd_rgba){1, 1, 1, 0.20 * lit});
        cairo_new_path(cairo);
        if (hot && !flash && ab->glow_x >= 0) {
            cairo_save(cairo);
            xd_path_rounded(cairo, button.x + 0.5, button.y + 0.5, button.w - 1, button.h - 1, rad,
                            rad, rad, rad);
            cairo_clip(cairo);
            double gx = XD_CLAMP(ab->glow_x, button.x, button.x + button.w);
            double gy = button.y + button.h;
            cairo_pattern_t *pattern =
                cairo_pattern_create_radial(gx, gy, 0, gx, gy, button.w * 0.85);
            xd_rgba color = ab->tint;
            cairo_pattern_add_color_stop_rgba(pattern, 0, color.r, color.g, color.b,
                                              pressed ? 0.55 : 0.8);
            cairo_pattern_add_color_stop_rgba(pattern, 1, color.r, color.g, color.b, 0);
            cairo_set_source(cairo, pattern);
            cairo_paint(cairo);
            cairo_pattern_destroy(pattern);
            cairo_restore(cairo);
        }
        app_button_edges(theme, cairo, button, active ? 0.55 : hot ? 0.45 : 0.28);
    }
    if (ab->progress != XD_PROGRESS_NONE)
        xd_theme_draw_taskbar_progress(theme, cairo,
                                       xd_rect_inset(button, xd_tpx(theme, 1), xd_tpx(theme, 1)),
                                       ab->progress, ab->fraction);
    if (ab->icon != nullptr) {
        double sx = 1, sy = 1;
        cairo_surface_get_device_scale(ab->icon, &sx, &sy);
        int iw = (int)(cairo_image_surface_get_width(ab->icon) / sx);
        int ih = (int)(cairo_image_surface_get_height(ab->icon) / sy);
        int ix =
            ab->label != nullptr ? button.x + xd_tpx(theme, 6) : button.x + (button.w - iw) / 2;
        cairo_set_source_surface(cairo, ab->icon, ix, button.y + (button.h - ih) / 2);
        cairo_paint(cairo);
        if (ab->label != nullptr) {
            xd_text_style st = xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_START);
            st.shadow = true;
            st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
            int tx = ix + iw + xd_tpx(theme, 6);
            xd_draw_text(
                cairo,
                (xd_rect){tx, button.y, button.x + button.w - tx - xd_tpx(theme, 4), button.h},
                ab->label, &st);
        }
    }
}

static void aero_draw_tray(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *clock) {
    /* No band of its own in Windows 7: the clock sits on the glass. */
    xd_text_style st = xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_END);
    st.shadow = true;
    st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    st.lines = true; /* a tall taskbar's day and date under the time */
    xd_draw_text(cairo, xd_rect_inset4(rect, xd_tpx(theme, 8), 0, xd_tpx(theme, 8), 0), clock, &st);
}

/* ---- menus and the toolkit ----------------------------------------------------------- */

static void aero_menu_palette([[maybe_unused]] const xd_theme *theme,
                              xd_menu_palette *menu_palette) {
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    *menu_palette = (xd_menu_palette){
        .flat = true,
        .background = xd_rgba_hex(0xF0F0F0),
        .border = xd_rgba_hex(0x979797),
        .text = xd_rgba_hex(0x000000),
        .hilight = xd_rgba_hex(0xDCE9F7),
        .hilight_text = xd_rgba_hex(0x000000),
        .hilight_border = xd_rgba_hex(0xAECFF7),
        .disabled_text = xd_rgba_hex(0x6D6D6D),
        .disabled_emboss = none,
        .separator_dark = xd_rgba_hex(0xE0E0E0),
        .separator_light = xd_rgba_hex(0xFFFFFF),
        .banner_top = xd_rgba_hex(0x1C3D68),
        .banner_bottom = xd_rgba_hex(0x4C8BCF),
        .banner_text = xd_rgba_hex(0xFFFFFF),
    };
}

static void aero_toolkit_colors(const xd_theme *theme, xd_toolkit_colors *toolkit_colors) {
    xd_rgba white = xd_rgba_hex(0xFFFFFF);
    /* GTK's title bars (client-side decorations, message dialogs' frames)
     * in the glass colour, opaque: a little deeper, fading lighter. */
    xd_rgba hue = xd_rgba_hex(theme->scheme.glass_color & 0xFFFFFFu);
    xd_hsv hsv = xd_rgba_to_hsv(hue);
    hsv.v *= 0.9;
    *toolkit_colors = (xd_toolkit_colors){
        .bg = xd_rgba_hex(0xF0F0F0),
        .fg = xd_rgba_hex(0x000000),
        .base = white,
        .text = xd_rgba_hex(0x000000),
        .disabled = xd_rgba_hex(0x838383),
        .selected_bg = xd_rgba_hex(0x3399FF),
        .selected_fg = white,
        .tooltip_bg = xd_rgba_hex(0xFFFFFF),
        .tooltip_fg = xd_rgba_hex(0x575757),
        .menu_bg = xd_rgba_hex(0xF0F0F0),
        .menu_fg = xd_rgba_hex(0x000000),
        .menu_border = xd_rgba_hex(0x979797),
        .menu_hot = xd_rgba_hex(0xDCE9F7),
        .menu_hot_fg = xd_rgba_hex(0x000000),
        .menubar_bg = xd_rgba_hex(0xF0F0F0),
        .title_bg = xd_rgba_from_hsv(hsv, 1.0),
        .title_bg2 = xd_rgba_mix(hue, white, 0.35),
        .title_fg = xd_rgba_hex(0x000000),
        .face = xd_rgba_hex(0xF0F0F0),
        .field_border = xd_rgba_hex(0xABADB3),
        .button_top = xd_rgba_hex(0xF2F2F2),
        .button_bottom = xd_rgba_hex(0xCFCFCF),
        .button_border = xd_rgba_hex(0x707070),
        .button_hot = xd_rgba_hex(0x3C7FB1),
        .button_focus = xd_rgba_hex(0x3399FF),
        .check_border = xd_rgba_hex(0x8E8F8F),
        .check_mark = xd_rgba_hex(0x1F1F1F),
        .thumb_top = xd_rgba_hex(0xF0F0F0),
        .thumb_bottom = xd_rgba_hex(0xD6D6D6),
        .track = xd_rgba_hex(0xF0F0F0),
        .tab_border = xd_rgba_hex(0x898C95),
        .tab_hot = xd_rgba_hex(0xBEE6FD),
    };
}

/* ---- taskbar thumbnails and jump lists ------------------------------------------------ */

/* Drawn by the shell's taskbar alone, so not through the style table.
 * Aero's colours are from memory of Windows 7 with the default "Sky"
 * colourisation; the other styles draw the previews as their tooltips,
 * a hovered window's tile in the selection colour, as Vista's Basic
 * look did. */

void xd_theme_draw_thumb_panel(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (theme->style != XD_STYLE_AERO) {
        xd_theme_draw_tooltip(theme, cairo, rect, "");
        return;
    }
    double rad = xd_tpx(theme, 6);
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    xd_path_rounded(cairo, rect.x, rect.y, rect.w, rect.h, rad, rad, rad, rad);
    cairo_clip(cairo);
    /* The taskbar's glass, a little lighter, with its sheen. */
    if (theme->glass)
        cairo_set_source_rgba(cairo, 0.12, 0.22, 0.34, 0.66);
    else
        cairo_set_source_rgb(cairo, 0.20, 0.32, 0.47);
    cairo_paint(cairo);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    cairo_pattern_add_color_stop_rgba(pattern, 0, 1, 1, 1, 0.22);
    cairo_pattern_add_color_stop_rgba(pattern, 0.45, 1, 1, 1, 0.06);
    cairo_pattern_add_color_stop_rgba(pattern, 1, 1, 1, 1, 0.02);
    cairo_set_source(cairo, pattern);
    cairo_paint(cairo);
    cairo_pattern_destroy(pattern);
    cairo_restore(cairo);
    /* A package's frame over the glass (TaskbandExtendedUI's). */
    if (xd_pkgstyle_draw_thumb_panel(theme, cairo, rect))
        return;
    cairo_set_line_width(cairo, 1);
    xd_path_rounded(cairo, rect.x + 0.5, rect.y + 0.5, rect.w - 1, rect.h - 1, rad, rad, rad, rad);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0.6);
    cairo_stroke(cairo);
    xd_path_rounded(cairo, rect.x + 1.5, rect.y + 1.5, rect.w - 3, rect.h - 3, rad - 1, rad - 1,
                    rad - 1, rad - 1);
    cairo_set_source_rgba(cairo, 1, 1, 1, 0.35);
    cairo_stroke(cairo);
}

void xd_theme_draw_thumb_tile(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state) {
    if (xd_pkgstyle_draw_thumb_tile(theme, cairo, rect, state))
        return;
    bool hot = state & XD_STATE_HOT, pressed = state & XD_STATE_PRESSED;
    if (!hot && !pressed)
        return;
    if (theme->style != XD_STYLE_AERO) {
        xd_rgba selection = xd_sys(theme, XD_COLOR_HILIGHT);
        xd_fill_rect(cairo, rect, xd_rgba_alpha(selection, pressed ? 0.40 : 0.22));
        xd_draw_bevel(cairo, rect, xd_tpx(theme, 1), selection, selection);
        return;
    }
    double rad = xd_tpx(theme, 3);
    xd_path_rounded(cairo, rect.x + 0.5, rect.y + 0.5, rect.w - 1, rect.h - 1, rad, rad, rad, rad);
    double lit = pressed ? 0.7 : 1.0;
    gloss(cairo, rect, (xd_rgba){1, 1, 1, 0.34 * lit}, (xd_rgba){1, 1, 1, 0.18 * lit},
          (xd_rgba){1, 1, 1, 0.10 * lit}, (xd_rgba){1, 1, 1, 0.20 * lit});
    cairo_new_path(cairo);
    app_button_edges(theme, cairo, rect, 0.5);
}

void xd_theme_draw_thumb_title(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                               const char *title) {
    if (theme->style != XD_STYLE_AERO) {
        xd_text_style st =
            xd_text(theme, XD_FONT_TOOLTIP, xd_sys(theme, XD_COLOR_INFO_TEXT), XD_ALIGN_START);
        st.ellipsize = true;
        xd_draw_text(cairo, rect, title != nullptr ? title : "", &st);
        return;
    }
    xd_rgba fg = xd_rgba_hex(0xFFFFFF);
    (void)xd_pkgstyle_thumb_title_color(theme, &fg);
    xd_text_style st = xd_text(theme, XD_FONT_MENU, fg, XD_ALIGN_START);
    st.ellipsize = true;
    st.shadow = true;
    st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    xd_draw_text(cairo, rect, title != nullptr ? title : "", &st);
}

void xd_theme_draw_thumb_close(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state) {
    if (xd_pkgstyle_draw_thumb_close(theme, cairo, rect, state))
        return;
    bool hot = state & XD_STATE_HOT, pressed = state & XD_STATE_PRESSED;
    if (theme->style != XD_STYLE_AERO) {
        /* A push button with the close glyph, as the style's own. */
        xd_theme_draw_button(theme, cairo, rect, state, "");
        int size = XD_MIN(rect.w, rect.h) - xd_tpx(theme, 8);
        int offset = pressed ? xd_tpx(theme, 1) : 0;
        xd_draw_glyph(theme, cairo,
                      (xd_rect){rect.x + (rect.w - size) / 2 + offset,
                                rect.y + (rect.h - size) / 2 + offset, size, size},
                      XD_GLYPH_CLOSE, xd_sys(theme, XD_COLOR_BUTTON_TEXT), 1.2);
        return;
    }
    double rad = xd_tpx(theme, 3);
    xd_path_rounded(cairo, rect.x + 0.5, rect.y + 0.5, rect.w - 1, rect.h - 1, rad, rad, rad, rad);
    if (pressed)
        gloss(cairo, rect, xd_rgba_hex(0xC8705E), xd_rgba_hex(0xA83A28), xd_rgba_hex(0x8C1C10),
              xd_rgba_hex(0xB0402C));
    else if (hot)
        gloss(cairo, rect, xd_rgba_hex(0xF4B0A0), xd_rgba_hex(0xE8604A), xd_rgba_hex(0xD8301A),
              xd_rgba_hex(0xF07A50));
    else
        gloss(cairo, rect, xd_rgba_hex(0xE8A898), xd_rgba_hex(0xD06C5C), xd_rgba_hex(0xC04232),
              xd_rgba_hex(0xD06450));
    cairo_set_line_width(cairo, 1);
    cairo_set_source_rgba(cairo, 0.25, 0, 0, 0.7);
    cairo_stroke(cairo);
    int gs = XD_MIN(rect.w, rect.h) - xd_tpx(theme, 6);
    xd_rect gr = {rect.x + (rect.w - gs) / 2, rect.y + (rect.h - gs) / 2, gs, gs};
    xd_draw_glyph(theme, cairo, gr, XD_GLYPH_CLOSE, xd_rgba_hex(0xFFFFFF), 1.4);
}

void xd_theme_draw_jumplist_frame(const xd_theme *theme, cairo_t *cairo, xd_rect rect, int band_y) {
    if (xd_pkgstyle_draw_jumplist_frame(theme, cairo, rect, band_y))
        return;
    xd_fill_rect(cairo, rect, xd_rgba_hex(0xFFFFFF));
    if (band_y < rect.y + rect.h) {
        xd_rect band = {rect.x, band_y, rect.w, rect.y + rect.h - band_y};
        xd_fill_gradient(cairo, band, xd_rgba_hex(0xEEF3FA), xd_rgba_hex(0xDFE8F5), false);
        int px = xd_tpx(theme, 1);
        xd_fill_rect(cairo, (xd_rect){rect.x, band_y, rect.w, px}, xd_rgba_hex(0xCCD9EA));
        xd_fill_rect(cairo, (xd_rect){rect.x, band_y + px, rect.w, px}, xd_rgba_hex(0xF8FAFD));
    }
    cairo_set_line_width(cairo, xd_tpx(theme, 1));
    cairo_rectangle(cairo, rect.x + 0.5, rect.y + 0.5, rect.w - 1, rect.h - 1);
    xd_set_color(cairo, xd_rgba_hex(0x8C8C8C));
    cairo_stroke(cairo);
}

void xd_theme_draw_jumplist_header(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                   const char *label) {
    xd_rgba fg = xd_rgba_hex(0x6D6D6D);
    (void)xd_pkgstyle_jumplist_header_color(theme, &fg);
    xd_text_style st = xd_text(theme, XD_FONT_MENU, fg, XD_ALIGN_START);
    st.ellipsize = true;
    xd_draw_text(cairo, rect, label, &st);
}

/* Windows 7's hover: a rounded pale blue box with a blue edge. */
static void hover_box(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool strong) {
    double rad = xd_tpx(theme, 2);
    xd_path_rounded(cairo, rect.x + 0.5, rect.y + 0.5, rect.w - 1, rect.h - 1, rad, rad, rad, rad);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    xd_rgba top = xd_rgba_hex(strong ? 0xD5E6FA : 0xF2F7FE);
    xd_rgba bottom = xd_rgba_hex(strong ? 0xB8D6FB : 0xDDEBFC);
    cairo_pattern_add_color_stop_rgb(pattern, 0, top.r, top.g, top.b);
    cairo_pattern_add_color_stop_rgb(pattern, 1, bottom.r, bottom.g, bottom.b);
    cairo_set_source(cairo, pattern);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(pattern);
    cairo_set_line_width(cairo, 1);
    xd_set_color(cairo, xd_rgba_hex(strong ? 0x7DA2CE : 0x9DBBE1));
    cairo_stroke(cairo);
}

void xd_theme_draw_jumplist_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                 const char *label, int text_x) {
    bool disabled = state & XD_STATE_DISABLED;
    xd_rgba fg = xd_rgba_hex(disabled ? 0x6D6D6D : 0x1E1E1E);
    if (!xd_pkgstyle_draw_jumplist_item(theme, cairo, rect, state, &fg) && (state & XD_STATE_HOT) &&
        !disabled)
        hover_box(theme, cairo, rect, false);
    xd_text_style st = xd_text(theme, XD_FONT_MENU, fg, XD_ALIGN_START);
    st.ellipsize = true;
    xd_draw_text(cairo,
                 (xd_rect){rect.x + text_x, rect.y, rect.w - text_x - xd_tpx(theme, 4), rect.h},
                 label, &st);
}

void xd_theme_draw_pushpin([[maybe_unused]] const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                           bool pinned) {
    double size = XD_MIN(rect.w, rect.h);
    cairo_save(cairo);
    cairo_translate(cairo, rect.x + rect.w / 2.0, rect.y + rect.h / 2.0);
    /* Pushed in it stands upright, needle down; otherwise it lies on its
     * side, needle to the left. */
    if (!pinned)
        cairo_rotate(cairo, G_PI / 2);
    cairo_scale(cairo, size / 16.0, size / 16.0);
    cairo_set_line_width(cairo, 1.2);
    xd_set_color(cairo, xd_rgba_hex(0x5A5A5A));
    cairo_move_to(cairo, 0, 2);
    cairo_line_to(cairo, 0, 7.5);
    cairo_stroke(cairo);
    /* Head, then the wider collar. */
    cairo_rectangle(cairo, -2.5, -6.5, 5, 5.5);
    cairo_rectangle(cairo, -4.5, -1.5, 9, 2.5);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(-4.5, 0, 4.5, 0);
    cairo_pattern_add_color_stop_rgb(pattern, 0, 0.98, 0.98, 0.98);
    cairo_pattern_add_color_stop_rgb(pattern, 1, 0.70, 0.74, 0.80);
    cairo_set_source(cairo, pattern);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(pattern);
    xd_set_color(cairo, xd_rgba_hex(0x4E5B6E));
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
    cairo_restore(cairo);
}

void xd_theme_draw_jumplist_pin(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                bool pinned) {
    if ((state & (XD_STATE_HOT | XD_STATE_PRESSED)) &&
        !xd_pkgstyle_draw_jumplist_pin(theme, cairo, rect, state))
        hover_box(theme, cairo, rect, true);
    int pin_size = xd_tpx(theme, 16);
    xd_theme_draw_pushpin(theme, cairo,
                          (xd_rect){rect.x + (rect.w - pin_size) / 2,
                                    rect.y + (rect.h - pin_size) / 2, pin_size, pin_size},
                          pinned);
}

const xd_style_ops xd_style_aero = {
    .name = "Aero",
    .frame_insets = aero_frame_insets,
    .frame_layout = aero_frame_layout,
    .draw_frame = aero_draw_frame,
    .dialog_color = aero_dialog_color,
    .draw_button = aero_draw_button,
    .draw_checkbox = aero_draw_checkbox,
    .taskbar_height = aero_taskbar_height,
    .draw_taskbar = aero_draw_taskbar,
    .start_button_width = aero_start_button_width,
    .draw_start_button = aero_draw_start_button,
    .draw_task_button = aero_draw_task_button,
    .draw_app_button = aero_draw_app_button,
    .draw_tray = aero_draw_tray,
    .menu_palette = aero_menu_palette,
    .toolkit_colors = aero_toolkit_colors,
};
