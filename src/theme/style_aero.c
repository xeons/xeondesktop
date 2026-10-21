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
static xd_rgba deepen(xd_hsv c, double value) {
    return xd_rgba_from_hsv((xd_hsv){c.h, fmin(1.0, c.s * 1.3), c.v * value}, 1.0);
}

void xd_theme_glass_colors(const xd_theme *t, xd_glass_colors *out) {
    uint32_t argb = t->scheme.glass_color;
    xd_rgba c = xd_rgba_hex(argb & 0xFFFFFFu);
    double intensity = ((argb >> 24) & 0xFFu) / 255.0;
    xd_hsv hsv = xd_rgba_to_hsv(c);
    out->translucent = t->glass && !t->scheme.glass_opaque;
    if (out->translucent) {
        /* Over the blur: the intensity is how much of the colour shows.
         * Sky's comes to Windows 7's default look. */
        double a = fmin(0.2 + 0.85 * intensity, 0.95);
        out->frame = xd_rgba_alpha(c, a);
        /* Inactive glass is paler and clearer. */
        out->frame_inactive = xd_rgba_alpha(xd_rgba_mix(c, xd_rgba_hex(0xD9D9D9), 0.7),
                                            0.08 + 0.6 * a);
        out->taskbar = xd_rgba_alpha(deepen(hsv, 0.26), 0.45 + 0.4 * intensity);
        out->menu = xd_rgba_alpha(deepen(hsv, 0.36), 0.3 + 0.35 * intensity);
        out->menu_frame = xd_rgba_alpha(deepen(hsv, 0.25), 0.55 + 0.35 * intensity);
        return;
    }
    /* Opaque (transparency off, or no blur: Windows 7 Basic): the colour
     * blended into light grey by its intensity, as Windows 7 does with
     * transparency off. */
    xd_rgba grey = xd_rgba_hex(0xEBEBEB);
    out->frame = xd_rgba_mix(grey, c, intensity);
    out->frame_inactive = xd_rgba_mix(grey, c, intensity * 0.5);
    out->taskbar = deepen(hsv, 0.42);
    out->menu = deepen(hsv, 0.36);
    out->menu_frame = deepen(hsv, 0.25);
}

static xd_rgba glass(const xd_theme *t, bool active) {
    xd_glass_colors g;
    xd_theme_glass_colors(t, &g);
    return active ? g.frame : g.frame_inactive;
}

static int border_of(const xd_theme *t, const xd_frame *f) {
    if (f->maximized)
        return 0;
    return xd_tpx(t, f->dialog ? DIALOG_BORDER : BORDER);
}

/* ---- frame ---------------------------------------------------------------------- */

static xd_insets aero_frame_insets(const xd_theme *t, const xd_frame *f) {
    int b = border_of(t, f);
    return (xd_insets){b, b + xd_tpx(t, CAPTION), b, b};
}

static void aero_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                              xd_frame_layout *l) {
    xd_insets in = aero_frame_insets(t, f);
    int b = border_of(t, f);
    l->caption = (xd_rect){outer.x, outer.y, outer.w, in.top};
    /* The strip hangs from the top edge, inside the right border. */
    int right = outer.x + outer.w - (f->maximized ? xd_tpx(t, 2) : b - xd_tpx(t, 2));
    int by = outer.y + (f->maximized ? 0 : xd_tpx(t, 1));
    int bh = xd_tpx(t, BTN_H);
    bool close_only = (f->buttons & ~XD_CAPBTN_CLOSE) == 0;
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
        if (!(f->buttons & order[i].flag))
            continue;
        int w = xd_tpx(t, order[i].part == XD_PART_CLOSE && close_only ? BTN_CLOSE_ONLY_W
                                                                       : order[i].w);
        bx -= w;
        xd_layout_add_button(l, (xd_rect){bx, by, w, bh}, order[i].part);
    }
    /* As in Windows 7, the icon and title sit in the middle of all the glass
     * above the client area, top border included: the caption buttons hang
     * from the top edge, and centred below the border they look low. */
    int icon = xd_tpx(t, 16);
    int cy = outer.y + (in.top - icon) / 2;
    l->icon = (xd_rect){outer.x + b + xd_tpx(t, 2), cy, icon, icon};
    int tx = l->icon.x + icon + xd_tpx(t, 5);
    l->title = (xd_rect){tx, outer.y, bx - xd_tpx(t, 8) - tx, in.top};
}

/* The glass: the colour, a sheen fading down from the top, and faint
 * diagonal streaks, as light catches Windows 7's frames. */
static void paint_glass(const xd_theme *t, cairo_t *cr, xd_rect r, bool active, int sheen_h) {
    xd_rgba g = glass(t, active);
    cairo_set_source_rgba(cr, g.r, g.g, g.b, g.a);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_pattern_t *p = cairo_pattern_create_linear(0, r.y, 0, r.y + sheen_h);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, active ? 0.42 : 0.35);
    cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.14);
    cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0.0);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, r.x, r.y, r.w, sheen_h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    if (!t->glass)
        return; /* Basic: no reflections */
    /* Streaks every ~180px, leaning right. */
    double step = xd_tpx(t, 180), wide = xd_tpx(t, 36);
    for (double x = r.x - r.h; x < r.x + r.w; x += step) {
        cairo_move_to(cr, x, r.y + r.h);
        cairo_line_to(cr, x + r.h * 0.6, r.y);
        cairo_line_to(cr, x + r.h * 0.6 + wide, r.y);
        cairo_line_to(cr, x + wide, r.y + r.h);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.07 : 0.05);
        cairo_fill(cr);
    }
}

/* The title on Windows 7's soft white glow. */
static void draw_title(const xd_theme *t, cairo_t *cr, xd_rect r, const char *title,
                       bool active) {
    if (title == nullptr || *title == '\0' || r.w <= 0)
        return;
    xd_text_style st = {
        .font = &title_font,
        .scale = t->scale,
        .color = active ? xd_rgba_hex(0x000000) : xd_rgba_hex(0x4A4A4A),
        .align = XD_ALIGN_START,
        .ellipsize = true,
        .smoothing = t->scheme.font_smoothing,
    };
    PangoLayout *l = xd_text_layout(cr, title, &st);
    pango_layout_set_width(l, r.w * PANGO_SCALE);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(l, &tw, &th);
    /* Centre the capitals, as Windows does, not the line with its room for
     * descenders (that sits the text high); an "H" gives their height
     * whatever the title's letters. Whole pixels keep the text crisp. */
    PangoLayout *h = xd_text_layout(cr, "H", &st);
    PangoRectangle caps;
    pango_layout_get_pixel_extents(h, &caps, nullptr);
    g_object_unref(h);
    double x = r.x, y = r.y + (r.h - caps.height) / 2 - caps.y;
    /* The glow: a soft white pill under the text. */
    double pad = xd_tpx(t, 10);
    for (int i = 0; i < 6; i++) {
        double grow = pad * (1.0 - i / 6.0);
        xd_path_rounded(cr, x - grow, y + th * 0.15 - grow / 2, tw + 2 * grow,
                        th * 0.7 + grow, (th * 0.7 + grow) / 2, (th * 0.7 + grow) / 2,
                        (th * 0.7 + grow) / 2, (th * 0.7 + grow) / 2);
        cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.10 : 0.07);
        cairo_fill(cr);
    }
    xd_set_color(cr, st.color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

/* A glossy glass button face: a brighter top half over a softer bottom. */
static void gloss(cairo_t *cr, xd_rect r, xd_rgba top0, xd_rgba top1, xd_rgba bot0,
                  xd_rgba bot1) {
    double mid = r.y + r.h * 0.48;
    cairo_pattern_t *p = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    double split = (mid - r.y) / r.h;
    cairo_pattern_add_color_stop_rgba(p, 0, top0.r, top0.g, top0.b, top0.a);
    cairo_pattern_add_color_stop_rgba(p, split, top1.r, top1.g, top1.b, top1.a);
    cairo_pattern_add_color_stop_rgba(p, split, bot0.r, bot0.g, bot0.b, bot0.a);
    cairo_pattern_add_color_stop_rgba(p, 1, bot1.r, bot1.g, bot1.b, bot1.a);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
}

/* ---- controls, from aero.msstyles' measured artwork --------------------------------- */

/* Normal 0, hot 1, pressed 2, disabled 3: the images' first four states. */
static int aero_state(uint8_t state) {
    return (state & XD_STATE_DISABLED) ? 3
           : (state & XD_STATE_PRESSED) ? 2
           : (state & XD_STATE_HOT)     ? 1
                                        : 0;
}

static void set_rgb(cairo_t *cr, uint32_t rgb, double alpha) {
    cairo_set_source_rgba(cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
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

static void draw_aero_glyph(const xd_theme *t, cairo_t *cr, xd_rect r, const aero_glyph *g,
                            uint32_t fill, uint32_t outline, double alpha, int ox, int oy) {
    double k = t->scale;
    int w = (int)lround(g->w * k), h = (int)lround(g->h * k);
    int x0 = r.x + (r.w - w) / 2, y0 = r.y + (r.h - h) / 2;
    int dx = (int)lround(ox * k), dy = (int)lround(oy * k);
    cairo_surface_t *a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    cairo_t *m = cairo_create(a8);
    for (int i = 0; i < g->count; i++) {
        const aero_rect *q = &g->r[i];
        int ax = (int)lround(q->x0 * k), ay = (int)lround(q->y0 * k);
        int bx = (int)lround((q->x1 + 1) * k), by = (int)lround((q->y1 + 1) * k);
        cairo_rectangle(m, ax, ay, bx - ax, by - ay);
    }
    cairo_fill(m);
    cairo_destroy(m);
    cairo_surface_flush(a8);
    /* The outline: the shape spread out, the shape itself left clear. */
    const uint8_t *d = cairo_image_surface_get_data(a8);
    int stride = cairo_image_surface_get_stride(a8);
    cairo_surface_t *ring = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    cairo_surface_flush(ring);
    uint8_t *o = cairo_image_surface_get_data(ring);
    int ostride = cairo_image_surface_get_stride(ring);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (d[y * stride + x])
                continue;
            double cover = 0;
            for (int j = -dy; j <= dy && cover < 1; j++) {
                for (int i = -dx; i <= dx; i++) {
                    int sx = x + i, sy = y + j;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h || !d[sy * stride + sx])
                        continue;
                    bool corner = (i == -dx || i == dx) && (j == -dy || j == dy) && dy > 0;
                    cover = fmax(cover, corner ? 0.5 : 1);
                }
            }
            o[y * ostride + x] = (uint8_t)lround(cover * 255);
        }
    }
    cairo_surface_mark_dirty(ring);
    set_rgb(cr, outline, alpha);
    cairo_mask_surface(cr, ring, x0, y0);
    set_rgb(cr, fill, alpha);
    cairo_mask_surface(cr, a8, x0, y0);
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

/* A shape at its natural size and the theme's scale, centred in r. */
static xd_rect shape_centred(const xd_theme *t, const xd_shape *p, xd_rect r) {
    int w = xd_tpx(t, p->w), h = xd_tpx(t, p->h);
    return (xd_rect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
}

/* Windows 7's caption buttons: on glass, DWM's translucent ones (a glow
 * around a hot one); in Basic, the Window class's. The glyph is white,
 * outlined. */
static void aero_caption_button(const xd_theme *t, cairo_t *cr, xd_rect r, xd_frame_part part,
                                const xd_frame *f, bool leftmost, bool alone) {
    bool pressed = f->pressed == part && f->hot == part;
    bool hot = f->hot == part && !pressed;
    int n = pressed ? 2 : hot ? 1 : 0;
    int g = caption_glyph_index(part, f->maximized);
    bool close = part == XD_PART_CLOSE;
    if (t->glass) {
        const xd_shape *bg = close && alone ? (f->active ? aero_dwm_close_alone
                                                         : aero_dwm_close_alone_inactive)
                             : close        ? (f->active ? aero_dwm_close : aero_dwm_close_inactive)
                             : leftmost     ? (f->active ? aero_dwm_min : aero_dwm_min_inactive)
                                            : (f->active ? aero_dwm_max : aero_dwm_max_inactive);
        if (hot) {
            /* The glow around a hot button, at its own size. */
            const xd_shape *glow = close ? aero_dwm_close_glow : aero_dwm_glow;
            xd_shape_draw(t, cr, glow, shape_centred(t, glow, r));
        }
        xd_shape_draw(t, cr, &bg[n], r);
        draw_aero_glyph(t, cr, r, &caption_glyphs[g], aero_dwm_glyph[g][n][0],
                        aero_dwm_glyph[g][n][1], 1, 2, 1);
        return;
    }
    int k = (f->active ? 0 : 4) + n;
    xd_shape_draw(t, cr, close ? &aero_basic_close[k] : &aero_basic_button[k], r);
    draw_aero_glyph(t, cr, r, &caption_glyphs[g], aero_basic_glyph[g][k][0],
                    aero_basic_glyph[g][k][1], 1, 1, 1);
}

static void caption_button(const xd_theme *t, cairo_t *cr, xd_rect r, xd_frame_part part,
                           const xd_frame *f, bool first, [[maybe_unused]] bool last) {
    bool alone = part == XD_PART_CLOSE && (f->buttons & ~XD_CAPBTN_CLOSE) == 0;
    if (xd_pkgstyle_dwm_button(t, cr, r, part, f, first, alone))
        return;
    aero_caption_button(t, cr, r, part, f, first, alone);
}

static void aero_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f) {
    xd_insets in = aero_frame_insets(t, f);
    double rad = f->maximized ? 0 : xd_tpx(t, RADIUS);
    cairo_save(cr);
    xd_path_rounded(cr, outer.x, outer.y, outer.w, outer.h, rad, rad, rad, rad);
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    paint_glass(t, cr, outer, f->active, in.top + xd_tpx(t, 14));
    /* Around the client area: a dark line, and a light one outside it. */
    xd_rect client = {outer.x + in.left, outer.y + in.top, outer.w - in.left - in.right,
                      outer.h - in.top - in.bottom};
    /* Nothing under the client area: the compositor blurs only where the
     * frame is drawn. */
    if (client.w > 0 && client.h > 0) {
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_rectangle(cr, client.x, client.y, client.w, client.h);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    }
    /* Lines a logical pixel wide, so they scale with the frame. */
    double px = xd_tpx(t, 1), half = px / 2;
    if (!f->maximized && client.w > 0 && client.h > 0) {
        cairo_set_line_width(cr, px);
        cairo_rectangle(cr, client.x - px - half, client.y - px - half, client.w + 3 * px,
                        client.h + 3 * px);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
        cairo_stroke(cr);
        cairo_rectangle(cr, client.x - half, client.y - half, client.w + px, client.h + px);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    if (!f->maximized) {
        /* The outline, and a highlight just inside it. */
        cairo_set_line_width(cr, px);
        xd_path_rounded(cr, outer.x + half, outer.y + half, outer.w - px, outer.h - px, rad, rad,
                        rad, rad);
        cairo_set_source_rgba(cr, 0, 0, 0, f->active ? 0.65 : 0.45);
        cairo_stroke(cr);
        xd_path_rounded(cr, outer.x + px + half, outer.y + px + half, outer.w - 3 * px,
                        outer.h - 3 * px, rad - px, rad - px, rad - px, rad - px);
        cairo_set_source_rgba(cr, 1, 1, 1, f->active ? 0.55 : 0.4);
        cairo_stroke(cr);
    }
    xd_frame_layout l = {0};
    aero_frame_layout(t, outer, f, &l);
    for (size_t i = 0; i < l.button_count; i++) {
        /* Laid out right to left: the last is leftmost. */
        caption_button(t, cr, l.button_rects[i], l.button_parts[i], f, i + 1 == l.button_count,
                       i == 0);
    }
    if (f->icon != nullptr)
        xd_style_task_icon(t, cr, f->icon, l.icon.x, l.icon.y);
    draw_title(t, cr, l.title, f->title, f->active);
}

static xd_rgba aero_dialog_color([[maybe_unused]] const xd_theme *t) {
    return xd_rgba_hex(0xF0F0F0);
}

/* ---- push buttons and check boxes ------------------------------------------------- */

static void aero_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                             const char *label) {
    bool disabled = state & XD_STATE_DISABLED;
    /* Normal, hot, pressed, disabled, then a default (or focused) one's. */
    int n = aero_state(state);
    if (n == 0 && (state & (XD_STATE_DEFAULT | XD_STATE_FOCUSED)))
        n = 4;
    xd_shape_draw(t, cr, &aero_pushbutton[n], r);
    if (label != nullptr) {
        xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                                   xd_rgba_hex(disabled ? 0x838383 : 0x000000), XD_ALIGN_CENTER);
        xd_draw_text(cr, r, label, &st);
    }
    if (state & XD_STATE_FOCUSED)
        xd_theme_draw_focus_rect(t, cr, xd_rect_inset(r, xd_tpx(t, 3), xd_tpx(t, 3)));
}

/* The tick: a dark stroke on a soft white halo. */
static void draw_tick(cairo_t *cr, xd_rect b, uint32_t rgb) {
    double k = b.w / 13.0;
    cairo_save(cr);
    cairo_translate(cr, b.x, b.y);
    cairo_scale(cr, k, k);
    cairo_move_to(cr, 3.0, 7.2);
    cairo_line_to(cr, 5.3, 9.3);
    cairo_line_to(cr, 9.4, 3.3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 3.6);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, 1.7);
    set_rgb(cr, rgb, 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void aero_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label) {
    bool disabled = state & XD_STATE_DISABLED;
    int n = aero_state(state);
    int box = xd_tpx(t, 13);
    xd_rect b = {r.x, r.y + (r.h - box) / 2, box, box};
    xd_shape_draw(t, cr, &aero_check[n], b);
    if (state & XD_STATE_CHECKED)
        draw_tick(cr, b, aero_tick[n]);
    if (label != nullptr) {
        xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                                   xd_rgba_hex(disabled ? 0x6D6D6D : 0x000000), XD_ALIGN_START);
        int tx = b.x + box + xd_tpx(t, 5);
        xd_rect text = {tx, r.y, r.x + r.w - tx, r.h};
        xd_draw_text(cr, text, label, &st);
        if (state & XD_STATE_FOCUSED) {
            int w = XD_MIN(xd_text_width(cr, label, &st), text.w);
            xd_theme_draw_focus_rect(t, cr,
                                     (xd_rect){text.x - xd_tpx(t, 1), r.y + xd_tpx(t, 2),
                                               w + xd_tpx(t, 3), r.h - xd_tpx(t, 4)});
        }
    }
}

void xd_aero_draw_radio([[maybe_unused]] const xd_theme *t, cairo_t *cr, xd_rect b,
                        uint8_t state) {
    int n = aero_state(state) + ((state & XD_STATE_CHECKED) ? 4 : 0);
    const uint32_t *c = aero_radio[n];
    double s = b.w / 13.0;
    cairo_save(cr);
    cairo_translate(cr, b.x, b.y);
    cairo_scale(cr, s, s);
    double cx = 6.0, cy = 6.5;
    /* The face: lit from the bottom right inside a light ring. */
    cairo_pattern_t *face = cairo_pattern_create_linear(3, 3.5, 9, 9.5);
    for (int i = 0; i < 2; i++)
        cairo_pattern_add_color_stop_rgb(face, i, ((c[2 + i] >> 16) & 0xFF) / 255.0,
                                         ((c[2 + i] >> 8) & 0xFF) / 255.0,
                                         (c[2 + i] & 0xFF) / 255.0);
    cairo_arc(cr, cx, cy, 5.5, 0, 2 * G_PI);
    set_rgb(cr, c[1], 1);
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, 4.0, 0, 2 * G_PI);
    cairo_set_source(cr, face);
    cairo_fill(cr);
    cairo_pattern_destroy(face);
    cairo_set_line_width(cr, 1);
    cairo_arc(cr, cx, cy, 5.5, 0, 2 * G_PI);
    set_rgb(cr, c[0], 1);
    cairo_stroke(cr);
    if (state & XD_STATE_CHECKED) {
        /* A glassy ball in a dark rim, lit from the top left. */
        cairo_arc(cr, cx, cy, 3.4, 0, 2 * G_PI);
        set_rgb(cr, c[4], 1);
        cairo_fill(cr);
        cairo_pattern_t *ball = cairo_pattern_create_radial(cx - 0.9, cy - 1.3, 0, cx, cy, 2.6);
        for (int i = 0; i < 3; i++)
            cairo_pattern_add_color_stop_rgb(ball, i / 2.0, ((c[5 + i] >> 16) & 0xFF) / 255.0,
                                             ((c[5 + i] >> 8) & 0xFF) / 255.0,
                                             (c[5 + i] & 0xFF) / 255.0);
        cairo_arc(cr, cx, cy, 2.6, 0, 2 * G_PI);
        cairo_set_source(cr, ball);
        cairo_fill(cr);
        cairo_pattern_destroy(ball);
    }
    cairo_restore(cr);
}

xd_rect xd_aero_draw_field(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled) {
    xd_fill_rect(cr, r, xd_rgba_hex(disabled ? 0xF0F0F0 : 0xFFFFFF));
    xd_shape_draw(t, cr, &aero_field[disabled ? 3 : 0], r);
    return xd_rect_inset(r, xd_tpx(t, 1), xd_tpx(t, 1));
}

/* A small triangle pointing down (or up), centred in r. */
static void aero_triangle(const xd_theme *t, cairo_t *cr, xd_rect r, bool up, uint32_t rgb,
                          int width) {
    double w = xd_tpx(t, width), h = xd_tpx(t, (width + 1) / 2);
    double x = r.x + (r.w - w) / 2.0, y = r.y + (r.h - h) / 2.0;
    double py = up ? y + h : y;
    double ty = up ? y : y + h;
    /* A faint white halo first, as Windows 7's arrows have. */
    cairo_move_to(cr, x - xd_tpx(t, 1), py);
    cairo_line_to(cr, x + w + xd_tpx(t, 1), py);
    cairo_line_to(cr, x + w / 2, ty + (up ? -xd_tpx(t, 1) : xd_tpx(t, 1)));
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.36);
    cairo_fill(cr);
    cairo_move_to(cr, x, py);
    cairo_line_to(cr, x + w, py);
    cairo_line_to(cr, x + w / 2, ty);
    cairo_close_path(cr);
    set_rgb(cr, rgb, 1);
    cairo_fill(cr);
}

void xd_aero_draw_dropdown_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    int n = aero_state(state);
    xd_shape_draw(t, cr, &aero_dropdown[n], r);
    aero_triangle(t, cr, r, false, aero_dropdown_glyph[n], 7);
}

void xd_aero_draw_vscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rect thumb,
                             bool disabled) {
    int n = disabled ? 3 : 0;
    int arrow = XD_MIN(r.w, r.h / 2);
    xd_rect up = {r.x, r.y, r.w, arrow};
    xd_rect down = {r.x, r.y + r.h - arrow, r.w, arrow};
    xd_shape_draw(t, cr, &aero_scrolltrack[n], r);
    xd_shape_draw(t, cr, &aero_scrollarrow[n], up);
    xd_shape_draw(t, cr, &aero_scrollarrow[4 + n], down);
    aero_triangle(t, cr, up, true, aero_scrollarrow_glyph[n], 7);
    aero_triangle(t, cr, down, false, aero_scrollarrow_glyph[4 + n], 7);
    if (thumb.h <= 0 || disabled)
        return;
    xd_shape_draw(t, cr, &aero_scrollthumb[0], thumb);
    /* The gripper: four dark lines over light ones, where there's room. */
    int gw = xd_tpx(t, 7), line = xd_tpx(t, 1);
    if (thumb.h < xd_tpx(t, 16))
        return;
    int gx = thumb.x + (thumb.w - gw) / 2, gy = thumb.y + (thumb.h - 7 * line) / 2;
    for (int i = 0; i < 4; i++) {
        xd_fill_rect(cr, (xd_rect){gx, gy + 2 * i * line, gw, line},
                     xd_rgba_hex(aero_gripper[0]));
        xd_fill_rect(cr, (xd_rect){gx + line, gy + (2 * i + 1) * line, gw, line},
                     xd_rgba_hex(aero_gripper[1]));
    }
}

void xd_aero_draw_tab(const xd_theme *t, cairo_t *cr, xd_rect tab, uint8_t state) {
    bool selected = state & XD_STATE_CHECKED;
    int n = selected ? 2 : (state & XD_STATE_DISABLED) ? 3 : (state & XD_STATE_HOT) ? 1 : 0;
    xd_shape_draw(t, cr, selected ? &aero_tabtop[n] : &aero_tab[n], tab);
}

/* A white page in a grey line (aero.msstyles' Tab Pane: 137 140 149). */
void xd_aero_draw_tab_panel(const xd_theme *t, cairo_t *cr, xd_rect r) {
    xd_fill_rect(cr, r, xd_rgba_hex(0xFFFFFF));
    xd_rgba line = xd_rgba_hex(0x898C95);
    xd_draw_bevel(cr, r, xd_tpx(t, 1), line, line);
}

void xd_aero_draw_groupbox_frame(const xd_theme *t, cairo_t *cr, xd_rect box) {
    xd_shape_draw(t, cr, aero_groupbox, box);
}

/* ---- taskbar ----------------------------------------------------------------------- */

static int aero_taskbar_height(const xd_theme *t) { return xd_tpx(t, TASKBAR_H); }

static void aero_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    xd_glass_colors g;
    xd_theme_glass_colors(t, &g);
    xd_set_color(cr, g.taskbar);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_restore(cr);
    /* A sheen over the half nearest the desktop, and Windows 7's rim. */
    bool vertical = edge == XD_EDGE_LEFT || edge == XD_EDGE_RIGHT;
    cairo_pattern_t *p =
        vertical ? cairo_pattern_create_linear(r.x, 0, r.x + r.w, 0)
                 : cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.16);
    cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.06);
    cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.0);
    cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0.04);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    int px = xd_tpx(t, 1);
    xd_fill_rect(cr, xd_style_rim_line(r, edge, 0, px), xd_rgba_alpha(xd_rgba_hex(0x000000), 0.5));
    xd_fill_rect(cr, xd_style_rim_line(r, edge, px, px),
                 xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.38));
}

static int aero_start_button_width(const xd_theme *t, [[maybe_unused]] cairo_t *cr) {
    return xd_tpx(t, ORB_W);
}

/* The Start orb: a glossy dark-blue sphere with the logo. */
static void aero_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    double d = XD_MIN(r.w, r.h) - xd_tpx(t, 2);
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, rad = d / 2;
    bool hot = state & XD_STATE_HOT, pressed = state & XD_STATE_PRESSED;
    if (hot || pressed) {
        cairo_pattern_t *glow = cairo_pattern_create_radial(cx, cy, rad * 0.6, cx, cy, rad * 1.3);
        cairo_pattern_add_color_stop_rgba(glow, 0, 0.55, 0.85, 1.0, pressed ? 0.3 : 0.55);
        cairo_pattern_add_color_stop_rgba(glow, 1, 0.55, 0.85, 1.0, 0);
        cairo_set_source(cr, glow);
        cairo_arc(cr, cx, cy, rad * 1.3, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);
    }
    cairo_pattern_t *body = cairo_pattern_create_radial(cx, cy + rad * 0.45, rad * 0.1, cx, cy,
                                                        rad);
    double dim = pressed ? 0.75 : 1.0;
    cairo_pattern_add_color_stop_rgb(body, 0, 0.30 * dim, 0.62 * dim, 0.95 * dim);
    cairo_pattern_add_color_stop_rgb(body, 0.55, 0.10 * dim, 0.30 * dim, 0.60 * dim);
    cairo_pattern_add_color_stop_rgb(body, 1, 0.03 * dim, 0.10 * dim, 0.24 * dim);
    cairo_set_source(cr, body);
    cairo_arc(cr, cx, cy, rad, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(body);
    /* The gloss over the top half. */
    cairo_save(cr);
    cairo_translate(cr, cx, cy - rad * 0.42);
    cairo_scale(cr, rad * 0.78, rad * 0.5);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, -1, 0, 1);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.75);
    cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0.08);
    cairo_set_source(cr, g);
    cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_restore(cr);
    cairo_set_line_width(cr, xd_tpx(t, 1));
    cairo_arc(cr, cx, cy, rad - 0.5, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, 0.75, 0.9, 1.0, 0.65);
    cairo_stroke(cr);
    int logo = (int)(d * 0.55);
    xd_draw_start_logo(cr, (xd_rect){(int)(cx - logo / 2.0), (int)(cy - logo / 2.0), logo, logo});
}

static void aero_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label, cairo_surface_t *icon) {
    bool active = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    bool hot = state & XD_STATE_HOT;
    bool flash = (state & XD_STATE_FLASH) && !active;
    xd_rect b = xd_rect_inset(r, xd_tpx(t, 1), xd_tpx(t, 2));
    double rad = xd_tpx(t, 3);
    xd_path_rounded(cr, b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, rad, rad, rad, rad);
    if (flash)
        gloss(cr, b, (xd_rgba){1, 0.80, 0.45, 0.85}, (xd_rgba){1, 0.62, 0.20, 0.80},
              (xd_rgba){0.95, 0.48, 0.08, 0.80}, (xd_rgba){1, 0.62, 0.20, 0.85});
    else
        gloss(cr, b, (xd_rgba){1, 1, 1, active ? 0.34 : hot ? 0.28 : 0.16},
              (xd_rgba){1, 1, 1, active ? 0.18 : hot ? 0.14 : 0.06},
              (xd_rgba){1, 1, 1, active ? 0.08 : hot ? 0.06 : 0.0},
              (xd_rgba){1, 1, 1, active ? 0.20 : hot ? 0.18 : 0.06});
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_stroke(cr);
    xd_path_rounded(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, rad - 1, rad - 1, rad - 1,
                    rad - 1);
    cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.55 : 0.30);
    cairo_stroke(cr);
    int isz = xd_tpx(t, 16);
    int ix = b.x + xd_tpx(t, 8);
    xd_style_task_icon(t, cr, icon, ix, b.y + (b.h - isz) / 2);
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_START);
    st.shadow = true;
    st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    int tx = ix + isz + xd_tpx(t, 6);
    xd_draw_text(cr, (xd_rect){tx, b.y, b.x + b.w - tx - xd_tpx(t, 6), b.h}, label, &st);
}

/* A glass button outline at b: a dark edge outside a light one. */
static void app_button_edges(const xd_theme *t, cairo_t *cr, xd_rect b, double inner_alpha) {
    double rad = xd_tpx(t, 3);
    xd_path_rounded(cr, b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, rad, rad, rad, rad);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_stroke(cr);
    xd_path_rounded(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, rad - 1, rad - 1, rad - 1,
                    rad - 1);
    cairo_set_source_rgba(cr, 1, 1, 1, inner_alpha);
    cairo_stroke(cr);
}

/* Windows 7's button for a program. A running program's has a glass
 * frame (lit while one of its windows is active), and more windows show
 * as edges stacked behind it; a pinned program that isn't running is its
 * icon alone. Hovering lights the glass with the icon's own colour,
 * glowing up from the bottom where the pointer is. */
static void aero_draw_app_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                 const xd_app_button *ab) {
    bool active = state & XD_STATE_CHECKED;
    bool pressed = state & XD_STATE_PRESSED;
    bool hot = state & XD_STATE_HOT;
    bool flash = (state & XD_STATE_FLASH) && !active;
    int stack = XD_CLAMP(ab->windows - 1, 0, 2); /* Windows 7 shows two at most */
    int step = xd_tpx(t, 3);
    xd_rect b = xd_rect_inset(r, xd_tpx(t, 1), xd_tpx(t, 1));
    b.w -= stack * step;
    double rad = xd_tpx(t, 3);
    if (ab->windows > 0 || hot || pressed || flash) {
        /* The farthest edge first, each showing only past the one in front. */
        for (int k = stack; k >= 1; k--) {
            xd_rect e = {b.x + k * step, b.y, b.w, b.h};
            cairo_save(cr);
            int from = b.x + b.w + (k - 1) * step - xd_tpx(t, 1);
            cairo_rectangle(cr, from, e.y, e.x + e.w - from + 1, e.h);
            cairo_clip(cr);
            xd_path_rounded(cr, e.x + 0.5, e.y + 0.5, e.w - 1, e.h - 1, rad, rad, rad, rad);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
            cairo_fill(cr);
            app_button_edges(t, cr, e, 0.25);
            cairo_restore(cr);
        }
        double lit = pressed ? 0.6 : active ? 1.0 : hot ? 0.8 : 0.45;
        xd_path_rounded(cr, b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, rad, rad, rad, rad);
        if (flash)
            gloss(cr, b, (xd_rgba){1, 0.80, 0.45, 0.85}, (xd_rgba){1, 0.62, 0.20, 0.80},
                  (xd_rgba){0.95, 0.48, 0.08, 0.80}, (xd_rgba){1, 0.62, 0.20, 0.85});
        else
            gloss(cr, b, (xd_rgba){1, 1, 1, 0.36 * lit}, (xd_rgba){1, 1, 1, 0.18 * lit},
                  (xd_rgba){1, 1, 1, 0.08 * lit}, (xd_rgba){1, 1, 1, 0.20 * lit});
        cairo_new_path(cr);
        if (hot && !flash && ab->glow_x >= 0) {
            cairo_save(cr);
            xd_path_rounded(cr, b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, rad, rad, rad, rad);
            cairo_clip(cr);
            double gx = XD_CLAMP(ab->glow_x, b.x, b.x + b.w);
            double gy = b.y + b.h;
            cairo_pattern_t *g = cairo_pattern_create_radial(gx, gy, 0, gx, gy, b.w * 0.85);
            xd_rgba c = ab->tint;
            cairo_pattern_add_color_stop_rgba(g, 0, c.r, c.g, c.b, pressed ? 0.55 : 0.8);
            cairo_pattern_add_color_stop_rgba(g, 1, c.r, c.g, c.b, 0);
            cairo_set_source(cr, g);
            cairo_paint(cr);
            cairo_pattern_destroy(g);
            cairo_restore(cr);
        }
        app_button_edges(t, cr, b, active ? 0.55 : hot ? 0.45 : 0.28);
    }
    if (ab->progress != XD_PROGRESS_NONE)
        xd_theme_draw_taskbar_progress(t, cr, xd_rect_inset(b, xd_tpx(t, 1), xd_tpx(t, 1)),
                                       ab->progress, ab->fraction);
    if (ab->icon != nullptr) {
        double sx = 1, sy = 1;
        cairo_surface_get_device_scale(ab->icon, &sx, &sy);
        int iw = (int)(cairo_image_surface_get_width(ab->icon) / sx);
        int ih = (int)(cairo_image_surface_get_height(ab->icon) / sy);
        int ix = ab->label != nullptr ? b.x + xd_tpx(t, 6) : b.x + (b.w - iw) / 2;
        cairo_set_source_surface(cr, ab->icon, ix, b.y + (b.h - ih) / 2);
        cairo_paint(cr);
        if (ab->label != nullptr) {
            xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_START);
            st.shadow = true;
            st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
            int tx = ix + iw + xd_tpx(t, 6);
            xd_draw_text(cr, (xd_rect){tx, b.y, b.x + b.w - tx - xd_tpx(t, 4), b.h}, ab->label,
                         &st);
        }
    }
}

static void aero_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock) {
    /* No band of its own in Windows 7: the clock sits on the glass. */
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_END);
    st.shadow = true;
    st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    st.lines = true; /* a tall taskbar's day and date under the time */
    xd_draw_text(cr, xd_rect_inset4(r, xd_tpx(t, 8), 0, xd_tpx(t, 8), 0), clock, &st);
}

/* ---- menus and the toolkit ----------------------------------------------------------- */

static void aero_menu_palette([[maybe_unused]] const xd_theme *t, xd_menu_palette *p) {
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    *p = (xd_menu_palette){
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

static void aero_toolkit_colors(const xd_theme *t, xd_toolkit_colors *c) {
    xd_rgba white = xd_rgba_hex(0xFFFFFF);
    /* GTK's title bars (client-side decorations, message dialogs' frames)
     * in the glass colour, opaque: a little deeper, fading lighter. */
    xd_rgba hue = xd_rgba_hex(t->scheme.glass_color & 0xFFFFFFu);
    xd_hsv hsv = xd_rgba_to_hsv(hue);
    hsv.v *= 0.9;
    *c = (xd_toolkit_colors){
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

/* Drawn by the shell's Aero taskbar alone, so not through the style
 * table. Colours are from memory of Windows 7 with the default "Sky"
 * colourisation. */

void xd_theme_draw_thumb_panel(const xd_theme *t, cairo_t *cr, xd_rect r) {
    double rad = xd_tpx(t, 6);
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    xd_path_rounded(cr, r.x, r.y, r.w, r.h, rad, rad, rad, rad);
    cairo_clip(cr);
    /* The taskbar's glass, a little lighter, with its sheen. */
    if (t->glass)
        cairo_set_source_rgba(cr, 0.12, 0.22, 0.34, 0.66);
    else
        cairo_set_source_rgb(cr, 0.20, 0.32, 0.47);
    cairo_paint(cr);
    cairo_pattern_t *p = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.22);
    cairo_pattern_add_color_stop_rgba(p, 0.45, 1, 1, 1, 0.06);
    cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0.02);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_restore(cr);
    /* A package's frame over the glass (TaskbandExtendedUI's). */
    if (xd_pkgstyle_draw_thumb_panel(t, cr, r))
        return;
    cairo_set_line_width(cr, 1);
    xd_path_rounded(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1, rad, rad, rad, rad);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_stroke(cr);
    xd_path_rounded(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, rad - 1, rad - 1, rad - 1,
                    rad - 1);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
    cairo_stroke(cr);
}

void xd_theme_draw_thumb_tile(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    if (xd_pkgstyle_draw_thumb_tile(t, cr, r, state))
        return;
    bool hot = state & XD_STATE_HOT, pressed = state & XD_STATE_PRESSED;
    if (!hot && !pressed)
        return;
    double rad = xd_tpx(t, 3);
    xd_path_rounded(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1, rad, rad, rad, rad);
    double lit = pressed ? 0.7 : 1.0;
    gloss(cr, r, (xd_rgba){1, 1, 1, 0.34 * lit}, (xd_rgba){1, 1, 1, 0.18 * lit},
          (xd_rgba){1, 1, 1, 0.10 * lit}, (xd_rgba){1, 1, 1, 0.20 * lit});
    cairo_new_path(cr);
    app_button_edges(t, cr, r, 0.5);
}

void xd_theme_draw_thumb_title(const xd_theme *t, cairo_t *cr, xd_rect r, const char *title) {
    xd_rgba fg = xd_rgba_hex(0xFFFFFF);
    (void)xd_pkgstyle_thumb_title_color(t, &fg);
    xd_text_style st = xd_text(t, XD_FONT_MENU, fg, XD_ALIGN_START);
    st.ellipsize = true;
    st.shadow = true;
    st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    xd_draw_text(cr, r, title != nullptr ? title : "", &st);
}

void xd_theme_draw_thumb_close(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    if (xd_pkgstyle_draw_thumb_close(t, cr, r, state))
        return;
    bool hot = state & XD_STATE_HOT, pressed = state & XD_STATE_PRESSED;
    double rad = xd_tpx(t, 3);
    xd_path_rounded(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1, rad, rad, rad, rad);
    if (pressed)
        gloss(cr, r, xd_rgba_hex(0xC8705E), xd_rgba_hex(0xA83A28), xd_rgba_hex(0x8C1C10),
              xd_rgba_hex(0xB0402C));
    else if (hot)
        gloss(cr, r, xd_rgba_hex(0xF4B0A0), xd_rgba_hex(0xE8604A), xd_rgba_hex(0xD8301A),
              xd_rgba_hex(0xF07A50));
    else
        gloss(cr, r, xd_rgba_hex(0xE8A898), xd_rgba_hex(0xD06C5C), xd_rgba_hex(0xC04232),
              xd_rgba_hex(0xD06450));
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0.25, 0, 0, 0.7);
    cairo_stroke(cr);
    int gs = XD_MIN(r.w, r.h) - xd_tpx(t, 6);
    xd_rect gr = {r.x + (r.w - gs) / 2, r.y + (r.h - gs) / 2, gs, gs};
    xd_draw_glyph(t, cr, gr, XD_GLYPH_CLOSE, xd_rgba_hex(0xFFFFFF), 1.4);
}

void xd_theme_draw_jumplist_frame(const xd_theme *t, cairo_t *cr, xd_rect r, int band_y) {
    if (xd_pkgstyle_draw_jumplist_frame(t, cr, r, band_y))
        return;
    xd_fill_rect(cr, r, xd_rgba_hex(0xFFFFFF));
    if (band_y < r.y + r.h) {
        xd_rect band = {r.x, band_y, r.w, r.y + r.h - band_y};
        xd_fill_gradient(cr, band, xd_rgba_hex(0xEEF3FA), xd_rgba_hex(0xDFE8F5), false);
        int px = xd_tpx(t, 1);
        xd_fill_rect(cr, (xd_rect){r.x, band_y, r.w, px}, xd_rgba_hex(0xCCD9EA));
        xd_fill_rect(cr, (xd_rect){r.x, band_y + px, r.w, px}, xd_rgba_hex(0xF8FAFD));
    }
    cairo_set_line_width(cr, xd_tpx(t, 1));
    cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
    xd_set_color(cr, xd_rgba_hex(0x8C8C8C));
    cairo_stroke(cr);
}

void xd_theme_draw_jumplist_header(const xd_theme *t, cairo_t *cr, xd_rect r, const char *label) {
    xd_rgba fg = xd_rgba_hex(0x6D6D6D);
    (void)xd_pkgstyle_jumplist_header_color(t, &fg);
    xd_text_style st = xd_text(t, XD_FONT_MENU, fg, XD_ALIGN_START);
    st.ellipsize = true;
    xd_draw_text(cr, r, label, &st);
}

/* Windows 7's hover: a rounded pale blue box with a blue edge. */
static void hover_box(const xd_theme *t, cairo_t *cr, xd_rect r, bool strong) {
    double rad = xd_tpx(t, 2);
    xd_path_rounded(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1, rad, rad, rad, rad);
    cairo_pattern_t *p = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    xd_rgba a = xd_rgba_hex(strong ? 0xD5E6FA : 0xF2F7FE);
    xd_rgba b = xd_rgba_hex(strong ? 0xB8D6FB : 0xDDEBFC);
    cairo_pattern_add_color_stop_rgb(p, 0, a.r, a.g, a.b);
    cairo_pattern_add_color_stop_rgb(p, 1, b.r, b.g, b.b);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    cairo_set_line_width(cr, 1);
    xd_set_color(cr, xd_rgba_hex(strong ? 0x7DA2CE : 0x9DBBE1));
    cairo_stroke(cr);
}

void xd_theme_draw_jumplist_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                 const char *label, int text_x) {
    bool disabled = state & XD_STATE_DISABLED;
    xd_rgba fg = xd_rgba_hex(disabled ? 0x6D6D6D : 0x1E1E1E);
    if (!xd_pkgstyle_draw_jumplist_item(t, cr, r, state, &fg) && (state & XD_STATE_HOT) &&
        !disabled)
        hover_box(t, cr, r, false);
    xd_text_style st = xd_text(t, XD_FONT_MENU, fg, XD_ALIGN_START);
    st.ellipsize = true;
    xd_draw_text(cr, (xd_rect){r.x + text_x, r.y, r.w - text_x - xd_tpx(t, 4), r.h}, label, &st);
}

void xd_theme_draw_pushpin([[maybe_unused]] const xd_theme *t, cairo_t *cr, xd_rect r,
                           bool pinned) {
    double s = XD_MIN(r.w, r.h);
    cairo_save(cr);
    cairo_translate(cr, r.x + r.w / 2.0, r.y + r.h / 2.0);
    /* Pushed in it stands upright, needle down; otherwise it lies on its
     * side, needle to the left. */
    if (!pinned)
        cairo_rotate(cr, G_PI / 2);
    cairo_scale(cr, s / 16.0, s / 16.0);
    cairo_set_line_width(cr, 1.2);
    xd_set_color(cr, xd_rgba_hex(0x5A5A5A));
    cairo_move_to(cr, 0, 2);
    cairo_line_to(cr, 0, 7.5);
    cairo_stroke(cr);
    /* Head, then the wider collar. */
    cairo_rectangle(cr, -2.5, -6.5, 5, 5.5);
    cairo_rectangle(cr, -4.5, -1.5, 9, 2.5);
    cairo_pattern_t *p = cairo_pattern_create_linear(-4.5, 0, 4.5, 0);
    cairo_pattern_add_color_stop_rgb(p, 0, 0.98, 0.98, 0.98);
    cairo_pattern_add_color_stop_rgb(p, 1, 0.70, 0.74, 0.80);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    xd_set_color(cr, xd_rgba_hex(0x4E5B6E));
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void xd_theme_draw_jumplist_pin(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                bool pinned) {
    if ((state & (XD_STATE_HOT | XD_STATE_PRESSED)) &&
        !xd_pkgstyle_draw_jumplist_pin(t, cr, r, state))
        hover_box(t, cr, r, true);
    int g = xd_tpx(t, 16);
    xd_theme_draw_pushpin(t, cr, (xd_rect){r.x + (r.w - g) / 2, r.y + (r.h - g) / 2, g, g},
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
