/* SPDX-License-Identifier: MIT
 * Advanced Appearance, as Windows XP's (Windows 2000 had the same editor
 * on the Appearance tab itself): a preview of the scheme whose parts
 * select their item when clicked, the Item list, and for each item its
 * Size, Color 1 and Color 2, and its font with size, colour, bold and
 * italic. Fields an item doesn't use are greyed, and Color 2 (a title
 * bar's gradient) only works with gradient captions, as in Windows.
 *
 * The changes are kept apart from the scheme they change (xd_scheme_custom,
 * desktop.ini's [CustomScheme]), so the scheme file itself is untouched;
 * OK hands them to the Appearance tab, whose Apply or OK saves them.
 *
 * With a visual style (Windows XP, Whistler, Aero) Windows drew title
 * bars, borders, caption buttons and push buttons from the style, and its
 * Advanced dialog said so; the settings were kept for the classic style
 * and older programs. Here the fields our styles draw themselves are
 * greyed instead of taking changes that would show nowhere: title bars,
 * window borders, caption buttons, palette titles and 3D objects, and the
 * menu's colours. The desktop, windows, selection, tooltips, message box
 * and icon fonts, the menu's size and font and the scroll bar's size
 * still work.
 *
 * The preview draws in the classic style whatever the style, as Windows
 * XP's did.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pango/pangocairo.h>

#include "../core/alloc.h"
#include "desk_advanced.h"
#include "desk_color.h"

enum { NONE = -1 };

/* The fields of the dialog an item can use. */
enum {
    F_SIZE = 1u << 0,
    F_COLOR1 = 1u << 1,
    F_COLOR2 = 1u << 2,
    F_FONT = 1u << 3, /* the font, its size, bold and italic */
    F_FONT_COLOR = 1u << 4,
    F_ALL = 0x1F,
};

typedef struct adv_item {
    const char *name;
    int metric; /* xd_metric, or NONE */
    int min, max;
    int color1, color2; /* xd_syscolor, or NONE */
    int font;           /* xd_font_role, or NONE */
    int font_color;     /* xd_syscolor, or NONE */
    uint8_t styled;     /* fields a visual style draws itself (greyed then) */
} adv_item;

/* In the order of Windows XP's list (alphabetical). The size ranges are
 * Windows' as far as known; where not, generous ones. */
enum {
    IT_3D,
    IT_ACTIVE_TITLE,
    IT_ACTIVE_BORDER,
    IT_APP_BACKGROUND,
    IT_CAPTION_BUTTONS,
    IT_DESKTOP,
    IT_ICON,
    IT_ICON_SPACING_H,
    IT_ICON_SPACING_V,
    IT_INACTIVE_TITLE,
    IT_INACTIVE_BORDER,
    IT_MENU,
    IT_MESSAGE_BOX,
    IT_PALETTE_TITLE,
    IT_SCROLLBAR,
    IT_SELECTED,
    IT_TOOLTIP,
    IT_WINDOW,
    IT_COUNT,
};

static const adv_item items[IT_COUNT] = {
    [IT_3D] = {"3D Objects", NONE, 0, 0, XD_COLOR_BUTTON_FACE, NONE, NONE, XD_COLOR_BUTTON_TEXT,
               F_ALL},
    [IT_ACTIVE_TITLE] = {"Active Title Bar", XD_METRIC_CAPTION_HEIGHT, 8, 100,
                         XD_COLOR_ACTIVE_TITLE, XD_COLOR_GRADIENT_ACTIVE_TITLE, XD_FONT_CAPTION,
                         XD_COLOR_TITLE_TEXT, F_ALL},
    [IT_ACTIVE_BORDER] = {"Active Window Border", XD_METRIC_BORDER_WIDTH, 1, 50,
                          XD_COLOR_ACTIVE_BORDER, NONE, NONE, NONE, F_ALL},
    [IT_APP_BACKGROUND] = {"Application Background", NONE, 0, 0, XD_COLOR_APP_WORKSPACE, NONE,
                           NONE, NONE, 0},
    [IT_CAPTION_BUTTONS] = {"Caption Buttons", XD_METRIC_CAPTION_BUTTON_WIDTH, 8, 100, NONE, NONE,
                            NONE, NONE, F_ALL},
    [IT_DESKTOP] = {"Desktop", NONE, 0, 0, XD_COLOR_BACKGROUND, NONE, NONE, NONE, 0},
    [IT_ICON] = {"Icon", XD_METRIC_ICON_SIZE, 16, 72, NONE, NONE, XD_FONT_ICON, NONE, 0},
    [IT_ICON_SPACING_H] = {"Icon Spacing (Horizontal)", XD_METRIC_ICON_SPACING, 32, 180, NONE,
                           NONE, NONE, NONE, 0},
    [IT_ICON_SPACING_V] = {"Icon Spacing (Vertical)", XD_METRIC_ICON_VERTICAL_SPACING, 32, 180,
                           NONE, NONE, NONE, NONE, 0},
    [IT_INACTIVE_TITLE] = {"Inactive Title Bar", XD_METRIC_CAPTION_HEIGHT, 8, 100,
                           XD_COLOR_INACTIVE_TITLE, XD_COLOR_GRADIENT_INACTIVE_TITLE,
                           XD_FONT_CAPTION, XD_COLOR_INACTIVE_TITLE_TEXT, F_ALL},
    [IT_INACTIVE_BORDER] = {"Inactive Window Border", XD_METRIC_BORDER_WIDTH, 1, 50,
                            XD_COLOR_INACTIVE_BORDER, NONE, NONE, NONE, F_ALL},
    [IT_MENU] = {"Menu", XD_METRIC_MENU_HEIGHT, 8, 100, XD_COLOR_MENU, NONE, XD_FONT_MENU,
                 XD_COLOR_MENU_TEXT, F_COLOR1 | F_FONT_COLOR},
    [IT_MESSAGE_BOX] = {"Message Box", NONE, 0, 0, NONE, NONE, XD_FONT_MESSAGE,
                        XD_COLOR_WINDOW_TEXT, 0},
    [IT_PALETTE_TITLE] = {"Palette Title", XD_METRIC_SM_CAPTION_HEIGHT, 8, 100, NONE, NONE,
                          XD_FONT_SMCAPTION, NONE, F_ALL},
    [IT_SCROLLBAR] = {"Scrollbar", XD_METRIC_SCROLL_WIDTH, 8, 100, NONE, NONE, NONE, NONE, 0},
    [IT_SELECTED] = {"Selected Items", XD_METRIC_MENU_HEIGHT, 8, 100, XD_COLOR_HILIGHT, NONE,
                     XD_FONT_MENU, XD_COLOR_HILIGHT_TEXT, 0},
    [IT_TOOLTIP] = {"ToolTip", NONE, 0, 0, XD_COLOR_INFO_WINDOW, NONE, XD_FONT_TOOLTIP,
                    XD_COLOR_INFO_TEXT, 0},
    [IT_WINDOW] = {"Window", NONE, 0, 0, XD_COLOR_WINDOW, NONE, NONE, XD_COLOR_WINDOW_TEXT, 0},
};

/* The point sizes the font size list offers (and the current one). */
static const double font_sizes[] = {6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24};

enum { MAX_HITS = 32, MAX_SIZES = XD_ARRAY_LEN(font_sizes) + 1 };

typedef struct hit {
    xd_rect r; /* device px, in the window */
    int item;
} hit;

struct advanced {
    xd_ui *ui;
    xd_window *win;
    xd_visual_style style;
    xd_scheme base;          /* the selected scheme at its font size */
    xd_scheme_custom custom; /* the changes to it */
    xd_scheme scheme;        /* base with the changes: what the preview shows */
    int item;

    xd_widget *preview;
    hit hits[MAX_HITS]; /* the preview's parts as last drawn, bottom first */
    size_t hit_count;
    xd_rect preview_r; /* ...and where it was, at what scale */
    double preview_scale;

    xd_widget *item_combo;
    xd_widget *size_label, *size_edit, *spin;
    xd_widget *color1_label, *color2_label;
    color_button *color1, *color2;
    xd_widget *font_label, *font_size_label, *font_color_label;
    xd_widget *font_combo, *font_size;
    color_button *font_color;
    xd_widget *bold, *italic;
    bool font_on; /* the item has a font the style leaves alone */

    GPtrArray *families;   /* installed font families, sorted */
    GPtrArray *font_names; /* the Font list as shown */
    GPtrArray *font_values; /* ...and the family each item stands for */
    double sizes[MAX_SIZES];
    size_t size_count;

    int spin_dir; /* +1 / -1 while an arrow of the spin box is held */
    xd_ui_source *spin_timer;
    bool spin_repeating;

    advanced_done done;
    void *user;
};

static bool styled(const advanced *a, uint8_t field) {
    return a->style != XD_STYLE_CLASSIC && (items[a->item].styled & field);
}

/* ---- the changes ----------------------------------------------------------------- */

static void changed(advanced *a) {
    a->scheme = a->base;
    xd_scheme_apply_custom(&a->scheme, &a->custom);
    xd_widget_redraw(a->preview);
}

static void set_color(advanced *a, int c, xd_rgb rgb) {
    a->custom.colors[c] = rgb;
    a->custom.color_mask |= 1u << c;
}

/* Windows' ColorAdjustLuma: n thousandths of the way to white (n > 0) or
 * black (n < 0), in HLS. */
static xd_rgb adjust_luma(xd_rgb c, int n) {
    int h, l, s;
    color_rgb_to_hls(c, &h, &l, &s);
    l = n > 0 ? l + (240 - l) * n / 1000 : l + l * n / 1000;
    return color_rgb_from_hls(h, XD_CLAMP(l, 0, 240), s);
}

/* 3D Objects' colour takes its edges with it, as Windows' does: the
 * shades are our approximation, which gives Windows Standard's from its
 * face colour. */
static void set_3d_color(advanced *a, xd_rgb face) {
    set_color(a, XD_COLOR_BUTTON_FACE, face);
    set_color(a, XD_COLOR_BUTTON_LIGHT, face);
    set_color(a, XD_COLOR_BUTTON_HILIGHT, adjust_luma(face, 1000));
    set_color(a, XD_COLOR_BUTTON_SHADOW, adjust_luma(face, -400));
    set_color(a, XD_COLOR_BUTTON_DK_SHADOW, adjust_luma(face, -670));
}

static void set_font(advanced *a, const xd_font_spec *f) {
    int role = items[a->item].font;
    if (role == NONE)
        return;
    a->custom.fonts[role] = *f;
    a->custom.font_mask |= 1u << role;
    changed(a);
}

static xd_font_spec current_font(const advanced *a) {
    int role = items[a->item].font;
    return a->scheme.fonts[role == NONE ? XD_FONT_MESSAGE : role];
}

/* ---- the fields ------------------------------------------------------------------ */

static void load_families(advanced *a) {
    a->families = g_ptr_array_new_with_free_func(g_free);
    PangoFontFamily **fams = nullptr;
    int n = 0;
    pango_font_map_list_families(pango_cairo_font_map_get_default(), &fams, &n);
    for (int i = 0; i < n; i++)
        g_ptr_array_add(a->families, g_strdup(pango_font_family_get_name(fams[i])));
    g_free(fams);
    g_ptr_array_sort_values(a->families, (GCompareFunc)g_utf8_collate);
}

/* The first family of a fallback list ("Tahoma, DejaVu Sans, ..."). */
static char *first_family(const char *family) {
    const char *comma = strchr(family, ',');
    return g_strstrip(comma != nullptr ? g_strndup(family, (gsize)(comma - family))
                                       : g_strdup(family));
}

/* The Font list: the installed families, with the item's font selected.
 * A scheme's font is a fallback list, shown by its first name (Tahoma),
 * which is added to the list if it isn't installed. */
static void fill_fonts(advanced *a, const xd_font_spec *f) {
    xd_widget_clear_items(a->font_combo);
    g_ptr_array_set_size(a->font_names, 0);
    g_ptr_array_set_size(a->font_values, 0);
    if (items[a->item].font == NONE)
        return; /* greyed and empty; one a style overrides shows greyed */
    char *first = first_family(f->family);
    int sel = -1;
    for (guint i = 0; i < a->families->len; i++) {
        const char *name = g_ptr_array_index(a->families, i);
        bool match = g_ascii_strcasecmp(name, f->family) == 0 ||
                     (sel < 0 && g_ascii_strcasecmp(name, first) == 0);
        if (match)
            sel = (int)i;
        g_ptr_array_add(a->font_names, g_strdup(name));
        g_ptr_array_add(a->font_values, g_strdup(match ? f->family : name));
    }
    if (sel < 0) {
        guint at = 0;
        while (at < a->font_names->len && g_utf8_collate(g_ptr_array_index(a->font_names, at),
                                                         first) < 0)
            at++;
        g_ptr_array_insert(a->font_names, (gint)at, g_strdup(first));
        g_ptr_array_insert(a->font_values, (gint)at, g_strdup(f->family));
        sel = (int)at;
    }
    g_free(first);
    for (guint i = 0; i < a->font_names->len; i++)
        xd_widget_add_item(a->font_combo, g_ptr_array_index(a->font_names, i));
    xd_widget_select(a->font_combo, sel);
}

static int compare_sizes(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

/* The Size list: Windows' usual sizes, and the font's own if it's another
 * (a Large Fonts scheme's 12.5). */
static void fill_sizes(advanced *a, double now) {
    xd_widget_clear_items(a->font_size);
    a->size_count = 0;
    if (items[a->item].font == NONE)
        return;
    bool listed = false;
    for (size_t i = 0; i < XD_ARRAY_LEN(font_sizes); i++) {
        a->sizes[a->size_count++] = font_sizes[i];
        listed = listed || fabs(font_sizes[i] - now) < 0.01;
    }
    if (!listed) {
        a->sizes[a->size_count++] = now;
        qsort(a->sizes, a->size_count, sizeof(a->sizes[0]), compare_sizes);
    }
    int sel = 0;
    for (size_t i = 0; i < a->size_count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%g", a->sizes[i]);
        xd_widget_add_item(a->font_size, buf);
        if (fabs(a->sizes[i] - now) < 0.01)
            sel = (int)i;
    }
    xd_widget_select(a->font_size, sel);
}

/* Shows the item's values, greying what it doesn't use. */
static void show_item(advanced *a) {
    const adv_item *it = &items[a->item];
    const xd_scheme *s = &a->scheme;

    bool size_on = it->metric != NONE && !styled(a, F_SIZE);
    char buf[16] = "";
    if (it->metric != NONE)
        snprintf(buf, sizeof(buf), "%d", s->metrics[it->metric]);
    xd_widget_set_text(a->size_edit, buf);
    xd_widget_set_enabled(a->size_edit, size_on);
    xd_widget_set_enabled(a->size_label, size_on);
    xd_widget_set_enabled(a->spin, size_on);
    xd_widget_redraw(a->spin);

    bool c1 = it->color1 != NONE && !styled(a, F_COLOR1);
    if (it->color1 != NONE)
        color_button_set(a->color1, s->colors[it->color1]);
    color_button_set_enabled(a->color1, c1);
    xd_widget_set_enabled(a->color1_label, c1);

    /* The second colour is a title bar's gradient: only with gradients. */
    bool c2 = it->color2 != NONE && s->gradient_captions && !styled(a, F_COLOR2);
    if (it->color2 != NONE)
        color_button_set(a->color2, s->colors[it->color2]);
    color_button_set_enabled(a->color2, c2);
    xd_widget_set_enabled(a->color2_label, c2);

    a->font_on = it->font != NONE && !styled(a, F_FONT);
    xd_font_spec f = current_font(a);
    fill_fonts(a, &f);
    fill_sizes(a, f.size_pt);
    xd_widget_set_enabled(a->font_combo, a->font_on);
    xd_widget_set_enabled(a->font_label, a->font_on);
    xd_widget_set_enabled(a->font_size, a->font_on);
    xd_widget_set_enabled(a->font_size_label, a->font_on);
    xd_widget_set_enabled(a->bold, a->font_on);
    xd_widget_set_enabled(a->italic, a->font_on);
    xd_widget_redraw(a->bold);
    xd_widget_redraw(a->italic);

    bool fc = it->font_color != NONE && !styled(a, F_FONT_COLOR);
    if (it->font_color != NONE)
        color_button_set(a->font_color, s->colors[it->font_color]);
    color_button_set_enabled(a->font_color, fc);
    xd_widget_set_enabled(a->font_color_label, fc);
}

static void select_item(advanced *a, int item) {
    if (item < 0 || item >= IT_COUNT)
        return;
    a->item = item;
    xd_widget_select(a->item_combo, item);
    show_item(a);
}

static void on_item(xd_widget *w, void *user) { select_item(user, xd_widget_selected(w)); }

static void set_size(advanced *a, int v) {
    const adv_item *it = &items[a->item];
    if (it->metric == NONE || v < it->min || v > it->max)
        return;
    a->custom.metrics[it->metric] = v;
    a->custom.metric_mask |= 1u << it->metric;
    changed(a);
}

static void on_size_edit(xd_widget *w, void *user) {
    advanced *a = user;
    int64_t v;
    /* Read once it's a number in range; typing on until it is. */
    if (xd_str_parse_i64(xd_str_from_cstr(xd_widget_text(w)), 0, 1000, &v))
        set_size(a, (int)v);
}

static void spin_step(advanced *a) {
    const adv_item *it = &items[a->item];
    if (it->metric == NONE)
        return;
    int v = XD_CLAMP(a->scheme.metrics[it->metric] + a->spin_dir, it->min, it->max);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    xd_widget_set_text(a->size_edit, buf);
    set_size(a, v);
}

/* A held arrow repeats, as an up-down control's does. */
static void spin_repeat(void *user) {
    advanced *a = user;
    if (!a->spin_repeating) {
        /* The first delay was a one-shot timer, done with now. */
        a->spin_repeating = true;
        a->spin_timer = xd_ui_add_timer(a->ui, 50, true, spin_repeat, a);
    }
    spin_step(a);
}

static void spin_stop(advanced *a) {
    xd_ui_remove_source(a->spin_timer);
    a->spin_timer = nullptr;
    a->spin_repeating = false;
    a->spin_dir = 0;
}

static void on_spin_pointer(xd_widget *w, xd_pointer_event ev, [[maybe_unused]] double x,
                            double y, void *user) {
    advanced *a = user;
    if (ev == XD_POINTER_PRESS) {
        a->spin_dir = y < 21 / 2.0 ? 1 : -1;
        spin_step(a);
        spin_stop(a);
        a->spin_dir = y < 21 / 2.0 ? 1 : -1;
        a->spin_timer = xd_ui_add_timer(a->ui, 400, false, spin_repeat, a);
    } else if (ev == XD_POINTER_RELEASE) {
        spin_stop(a);
    }
    xd_widget_redraw(w);
}

static void draw_arrow(const xd_theme *t, cairo_t *cr, xd_rect r, bool up, bool enabled) {
    double a = xd_tpx(t, 3);
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double dir = up ? -1 : 1;
    cairo_move_to(cr, cx - a, cy - dir * a / 2);
    cairo_line_to(cr, cx + a, cy - dir * a / 2);
    cairo_line_to(cr, cx, cy + dir * a / 2);
    cairo_close_path(cr);
    xd_set_color(cr, xd_sys(t, enabled ? XD_COLOR_BUTTON_TEXT : XD_COLOR_GRAY_TEXT));
    cairo_fill(cr);
}

/* The up-down control beside Size: two half-height buttons. */
static void draw_spin([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                      void *user) {
    advanced *a = user;
    const adv_item *it = &items[a->item];
    bool on = it->metric != NONE && !styled(a, F_SIZE);
    int half = r.h / 2;
    xd_rect up = {r.x, r.y, r.w, half}, down = {r.x, r.y + half, r.w, r.h - half};
    uint8_t dis = on ? 0 : XD_STATE_DISABLED;
    xd_theme_draw_button(t, cr, up, dis | (a->spin_dir > 0 ? XD_STATE_PRESSED : 0), "");
    xd_theme_draw_button(t, cr, down, dis | (a->spin_dir < 0 ? XD_STATE_PRESSED : 0), "");
    draw_arrow(t, cr, up, true, on);
    draw_arrow(t, cr, down, false, on);
}

static void on_color1([[maybe_unused]] color_button *b, xd_rgb rgb, void *user) {
    advanced *a = user;
    int c = items[a->item].color1;
    if (c == NONE)
        return;
    if (c == XD_COLOR_BUTTON_FACE)
        set_3d_color(a, rgb);
    else
        set_color(a, c, rgb);
    changed(a);
}

static void on_color2([[maybe_unused]] color_button *b, xd_rgb rgb, void *user) {
    advanced *a = user;
    if (items[a->item].color2 != NONE) {
        set_color(a, items[a->item].color2, rgb);
        changed(a);
    }
}

static void on_font_color([[maybe_unused]] color_button *b, xd_rgb rgb, void *user) {
    advanced *a = user;
    if (items[a->item].font_color != NONE) {
        set_color(a, items[a->item].font_color, rgb);
        changed(a);
    }
}

static void on_font(xd_widget *w, void *user) {
    advanced *a = user;
    int i = xd_widget_selected(w);
    if (i < 0 || (guint)i >= a->font_values->len)
        return;
    xd_font_spec f = current_font(a);
    const char *family = g_ptr_array_index(a->font_values, (guint)i);
    snprintf(f.family, sizeof(f.family), "%s", family);
    set_font(a, &f);
}

static void on_font_size(xd_widget *w, void *user) {
    advanced *a = user;
    int i = xd_widget_selected(w);
    if (i < 0 || (size_t)i >= a->size_count)
        return;
    xd_font_spec f = current_font(a);
    f.size_pt = a->sizes[i];
    set_font(a, &f);
}

/* Bold and Italic: push buttons that stay down, labelled B and I. */
static void draw_toggle(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t, void *user) {
    advanced *a = user;
    bool italic = w == a->italic;
    xd_font_spec f = current_font(a);
    bool down = a->font_on && (italic ? f.italic : f.bold);
    uint8_t state = (down ? XD_STATE_PRESSED : 0) | (a->font_on ? 0 : XD_STATE_DISABLED);
    xd_theme_draw_button(t, cr, r, state, "");
    xd_font_spec label = t->scheme.fonts[XD_FONT_MESSAGE];
    label.bold = !italic;
    label.italic = italic;
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               xd_sys(t, a->font_on ? XD_COLOR_BUTTON_TEXT : XD_COLOR_GRAY_TEXT),
                               XD_ALIGN_CENTER);
    st.font = &label;
    int shift = down ? xd_tpx(t, 1) : 0;
    xd_draw_text(cr, (xd_rect){r.x + shift, r.y + shift, r.w, r.h}, italic ? "I" : "B", &st);
}

static void on_toggle_pointer(xd_widget *w, xd_pointer_event ev, [[maybe_unused]] double x,
                              [[maybe_unused]] double y, void *user) {
    advanced *a = user;
    if (ev != XD_POINTER_PRESS || !a->font_on)
        return;
    xd_font_spec f = current_font(a);
    if (w == a->italic)
        f.italic = !f.italic;
    else
        f.bold = !f.bold;
    set_font(a, &f);
    xd_widget_redraw(w);
}

/* ---- the preview ------------------------------------------------------------------ */

static void add_hit(advanced *a, xd_rect r, int item) {
    if (a->hit_count < MAX_HITS)
        a->hits[a->hit_count++] = (hit){r, item};
}

/* A window's frame and its parts. */
static xd_rect preview_frame(advanced *a, const xd_theme *t, cairo_t *cr, xd_rect outer,
                             const xd_frame *f, int border_item, int title_item, int client_item) {
    xd_theme_draw_frame(t, cr, outer, f);
    xd_insets in = xd_theme_frame_insets(t, f);
    xd_rect client = xd_rect_inset4(outer, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cr, client, xd_sys(t, XD_COLOR_BUTTON_FACE));
    add_hit(a, outer, border_item);
    xd_frame_layout l;
    xd_theme_frame_layout(t, outer, f, &l);
    add_hit(a, l.caption, title_item);
    for (size_t i = 0; i < l.button_count; i++)
        add_hit(a, l.button_rects[i], IT_CAPTION_BUTTONS);
    add_hit(a, client, client_item);
    return client;
}

/* Windows' preview: an inactive window behind an active one with a menu
 * bar (Normal, Disabled, Selected) and a window of text with a scroll bar,
 * and a message box on top. */
static void draw_preview([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r,
                         const xd_theme *wt, void *user) {
    advanced *a = user;
    xd_theme pt;
    xd_theme_init(&pt, XD_STYLE_CLASSIC, &a->scheme, wt->scale);
    const xd_theme *t = &pt;
    a->hit_count = 0;
    a->preview_r = r;
    a->preview_scale = wt->scale;

    xd_rect well = xd_theme_draw_field(wt, cr, r, false);
    xd_fill_rect(cr, well, xd_sys(t, XD_COLOR_BACKGROUND));
    add_hit(a, well, IT_DESKTOP);
    cairo_save(cr);
    cairo_rectangle(cr, well.x, well.y, well.w, well.h);
    cairo_clip(cr);

    uint8_t buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE;
    xd_frame inactive = {.title = "Inactive Window", .buttons = buttons};
    xd_rect r1 = {well.x + xd_tpx(t, 8), well.y + xd_tpx(t, 6), well.w - xd_tpx(t, 56),
                  xd_tpx(t, 80)};
    preview_frame(a, t, cr, r1, &inactive, IT_INACTIVE_BORDER, IT_INACTIVE_TITLE, IT_3D);

    xd_frame active = {.title = "Active Window", .active = true, .buttons = buttons};
    xd_rect r2 = {well.x + xd_tpx(t, 18), well.y + xd_tpx(t, 30), well.w - xd_tpx(t, 36),
                  well.h - xd_tpx(t, 36)};
    xd_rect client = preview_frame(a, t, cr, r2, &active, IT_ACTIVE_BORDER, IT_ACTIVE_TITLE,
                                   IT_3D);

    /* The menu bar. */
    int menu_h = xd_metric_px(t, XD_METRIC_MENU_HEIGHT);
    xd_rect bar = {client.x, client.y, client.w, menu_h};
    xd_fill_rect(cr, bar,
                 xd_sys(t, t->scheme.flat_menus ? XD_COLOR_MENU_BAR : XD_COLOR_MENU));
    add_hit(a, bar, IT_MENU);
    static const char *const labels[] = {"Normal", "Disabled", "Selected"};
    int x = bar.x + xd_tpx(t, 2);
    for (int i = 0; i < 3; i++) {
        xd_text_style st = xd_text(t, XD_FONT_MENU, xd_sys(t, XD_COLOR_MENU_TEXT), XD_ALIGN_CENTER);
        int tw = xd_text_width(cr, labels[i], &st) + xd_tpx(t, 14);
        xd_rect ir = {x, bar.y, tw, bar.h};
        if (i == 1) {
            /* Disabled: grey with a highlight under it, as classic menus. */
            int p = xd_tpx(t, 1);
            st.color = xd_sys(t, XD_COLOR_BUTTON_HILIGHT);
            xd_draw_text(cr, (xd_rect){ir.x + p, ir.y + p, ir.w, ir.h}, labels[i], &st);
            st.color = xd_sys(t, XD_COLOR_GRAY_TEXT);
        } else if (i == 2) {
            bool flat = t->scheme.flat_menus;
            xd_fill_rect(cr, ir, xd_sys(t, flat ? XD_COLOR_MENU_HILIGHT : XD_COLOR_HILIGHT));
            st.color = xd_sys(t, XD_COLOR_HILIGHT_TEXT);
            add_hit(a, ir, IT_SELECTED);
        }
        xd_draw_text(cr, ir, labels[i], &st);
        x += tw;
    }

    /* The window: text and a scroll bar. */
    xd_rect area = xd_rect_inset4(client, xd_tpx(t, 2), menu_h + xd_tpx(t, 2), xd_tpx(t, 2),
                                  xd_tpx(t, 2));
    xd_rect text = xd_theme_draw_field(t, cr, area, false);
    xd_fill_rect(cr, text, xd_sys(t, XD_COLOR_WINDOW));
    add_hit(a, area, IT_WINDOW);
    int sw = xd_metric_px(t, XD_METRIC_SCROLL_WIDTH);
    xd_rect sb = {text.x + text.w - sw, text.y, sw, text.h};
    xd_theme_draw_vscrollbar(t, cr, sb, 0, 4, 10);
    add_hit(a, sb, IT_SCROLLBAR);
    xd_text_style wt_st = xd_text(t, XD_FONT_MESSAGE, xd_sys(t, XD_COLOR_WINDOW_TEXT),
                                  XD_ALIGN_START);
    xd_draw_text(cr, (xd_rect){text.x + xd_tpx(t, 3), text.y + xd_tpx(t, 2), text.w - sw,
                               xd_tpx(t, 16)},
                 "Window Text", &wt_st);

    /* The message box. */
    xd_frame box = {.title = "Message Box", .active = true, .dialog = true,
                    .buttons = XD_CAPBTN_CLOSE};
    xd_insets ib = xd_theme_frame_insets(t, &box);
    int bw = XD_MIN(xd_tpx(t, 170), text.w - sw - xd_tpx(t, 24));
    int bh = ib.top + ib.bottom + xd_tpx(t, 54);
    /* Right of the window's text, as low as fits. */
    xd_rect r3 = {text.x + text.w - sw - bw - xd_tpx(t, 10),
                  XD_MIN(text.y + xd_tpx(t, 10), well.y + well.h - bh), bw, bh};
    xd_rect bc = preview_frame(a, t, cr, r3, &box, IT_ACTIVE_BORDER, IT_ACTIVE_TITLE,
                               IT_MESSAGE_BOX);
    xd_text_style mst = xd_text(t, XD_FONT_MESSAGE, xd_sys(t, XD_COLOR_WINDOW_TEXT),
                                XD_ALIGN_START);
    xd_draw_text(cr, (xd_rect){bc.x + xd_tpx(t, 8), bc.y + xd_tpx(t, 4), bc.w - xd_tpx(t, 8),
                               xd_tpx(t, 16)},
                 "Message Text", &mst);
    int okw = xd_tpx(t, 66);
    xd_rect ok = {bc.x + (bc.w - okw) / 2, bc.y + xd_tpx(t, 26), okw, xd_tpx(t, 22)};
    xd_theme_draw_button(t, cr, ok, XD_STATE_DEFAULT, "OK");
    add_hit(a, ok, IT_3D);
    cairo_restore(cr);
}

/* A click in the preview picks the item under it (the topmost part). */
static void on_preview_pointer([[maybe_unused]] xd_widget *w, xd_pointer_event ev, double x,
                               double y, void *user) {
    advanced *a = user;
    if (ev != XD_POINTER_PRESS)
        return;
    /* The hits are in device px; the pointer is logical, in the preview. */
    double dx = a->preview_r.x + x * a->preview_scale;
    double dy = a->preview_r.y + y * a->preview_scale;
    for (size_t i = a->hit_count; i-- > 0;) {
        xd_rect r = a->hits[i].r;
        if (dx >= r.x && dy >= r.y && dx < r.x + r.w && dy < r.y + r.h) {
            select_item(a, a->hits[i].item);
            return;
        }
    }
}

/* ---- the dialog -------------------------------------------------------------------- */

static void finish(advanced *a, bool ok) {
    spin_stop(a);
    color_button_free(a->color1);
    color_button_free(a->color2);
    color_button_free(a->font_color);
    g_ptr_array_unref(a->families);
    g_ptr_array_unref(a->font_names);
    g_ptr_array_unref(a->font_values);
    xd_window_close(a->win);
    xd_scheme_custom custom = a->custom;
    xd_scheme_custom_trim(&custom, &a->base);
    advanced_done done = a->done;
    void *user = a->user;
    xd_free(a);
    done(ok ? &custom : nullptr, user);
}

void advanced_cancel(advanced *a) { finish(a, false); }

static void on_ok([[maybe_unused]] xd_widget *w, void *user) { finish(user, true); }
static void on_cancel([[maybe_unused]] xd_widget *w, void *user) { finish(user, false); }

static constexpr int ADV_W = 404;
static constexpr int ADV_H = 362;

advanced *advanced_open(xd_ui *ui, const xd_settings *pending, advanced_done done, void *user) {
    advanced *a = XD_NEW(advanced);
    a->ui = ui;
    a->done = done;
    a->user = user;
    a->style = pending->style;
    xd_settings plain = *pending;
    plain.custom_scheme = (xd_scheme_custom){0};
    xd_settings_scheme(&plain, &a->base);
    a->custom = pending->custom_scheme;
    a->scheme = a->base;
    xd_scheme_apply_custom(&a->scheme, &a->custom);
    a->item = IT_DESKTOP; /* where Windows starts */
    load_families(a);
    a->font_names = g_ptr_array_new_with_free_func(g_free);
    a->font_values = g_ptr_array_new_with_free_func(g_free);

    /* Windows 7 opens it from Window Color and Appearance under the same
     * title. */
    const char *title =
        a->style == XD_STYLE_AERO ? "Window Color and Appearance" : "Advanced Appearance";
    xd_window *win = a->win = xd_window_create(ui, title, "xd-desk", ADV_W, ADV_H);
    a->preview = xd_custom(win, (xd_rect){12, 12, 380, 168}, draw_preview, a);
    xd_custom_set_pointer(a->preview, on_preview_pointer);
    xd_label(win, (xd_rect){12, 186, 380, 30},
             "If you select a windows and buttons setting other than Windows Classic, it will "
             "override the following settings, except in some older programs.",
             XD_LABEL_WRAP);

    xd_label(win, (xd_rect){12, 222, 186, 16}, "Item:", 0);
    a->item_combo = xd_combo(win, (xd_rect){12, 238, 186, 21}, false, on_item, a);
    for (int i = 0; i < IT_COUNT; i++)
        xd_widget_add_item(a->item_combo, items[i].name);
    xd_widget_select(a->item_combo, a->item);
    a->size_label = xd_label(win, (xd_rect){206, 222, 56, 16}, "Size:", 0);
    a->size_edit = xd_edit(win, (xd_rect){206, 238, 40, 21}, "");
    xd_edit_set_on_change(a->size_edit, on_size_edit, a);
    a->spin = xd_custom(win, (xd_rect){246, 238, 16, 21}, draw_spin, a);
    xd_custom_set_pointer(a->spin, on_spin_pointer);
    a->color1_label = xd_label(win, (xd_rect){270, 222, 56, 16}, "Color 1:", 0);
    a->color1 = color_button_create(win, (xd_rect){270, 237, 56, 23}, on_color1, a);
    a->color2_label = xd_label(win, (xd_rect){334, 222, 58, 16}, "Color 2:", 0);
    a->color2 = color_button_create(win, (xd_rect){334, 237, 56, 23}, on_color2, a);

    a->font_label = xd_label(win, (xd_rect){12, 268, 186, 16}, "Font:", 0);
    a->font_combo = xd_combo(win, (xd_rect){12, 284, 186, 21}, false, on_font, a);
    a->font_size_label = xd_label(win, (xd_rect){206, 268, 56, 16}, "Size:", 0);
    a->font_size = xd_combo(win, (xd_rect){206, 284, 56, 21}, false, on_font_size, a);
    a->font_color_label = xd_label(win, (xd_rect){270, 268, 56, 16}, "Color:", 0);
    a->font_color = color_button_create(win, (xd_rect){270, 283, 56, 23}, on_font_color, a);
    a->bold = xd_custom(win, (xd_rect){334, 283, 27, 23}, draw_toggle, a);
    xd_custom_set_pointer(a->bold, on_toggle_pointer);
    a->italic = xd_custom(win, (xd_rect){363, 283, 27, 23}, draw_toggle, a);
    xd_custom_set_pointer(a->italic, on_toggle_pointer);

    xd_widget *ok = xd_button(win, (xd_rect){ADV_W - 168, ADV_H - 35, 75, 23}, "OK", on_ok, a);
    xd_widget *cancel =
        xd_button(win, (xd_rect){ADV_W - 87, ADV_H - 35, 75, 23}, "Cancel", on_cancel, a);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, a->item_combo);
    show_item(a);
    xd_window_show(win);
    return a;
}
