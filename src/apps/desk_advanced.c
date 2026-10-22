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

static bool styled(const advanced *appearance, uint8_t field) {
    return appearance->style != XD_STYLE_CLASSIC && (items[appearance->item].styled & field);
}

/* ---- the changes ----------------------------------------------------------------- */

static void changed(advanced *appearance) {
    appearance->scheme = appearance->base;
    xd_scheme_apply_custom(&appearance->scheme, &appearance->custom);
    xd_widget_redraw(appearance->preview);
}

static void set_color(advanced *appearance, int color_index, xd_rgb rgb) {
    appearance->custom.colors[color_index] = rgb;
    appearance->custom.color_mask |= 1u << color_index;
}

/* Windows' ColorAdjustLuma: n thousandths of the way to white (n > 0) or
 * black (n < 0), in HLS. */
static xd_rgb adjust_luma(xd_rgb color, int thousandths) {
    int hue, luminosity, saturation;
    color_rgb_to_hls(color, &hue, &luminosity, &saturation);
    luminosity = thousandths > 0 ? luminosity + (240 - luminosity) * thousandths / 1000
                                 : luminosity + luminosity * thousandths / 1000;
    return color_rgb_from_hls(hue, XD_CLAMP(luminosity, 0, 240), saturation);
}

/* 3D Objects' colour takes its edges with it, as Windows' does: the
 * shades are our approximation, which gives Windows Standard's from its
 * face colour. */
static void set_3d_color(advanced *appearance, xd_rgb face) {
    set_color(appearance, XD_COLOR_BUTTON_FACE, face);
    set_color(appearance, XD_COLOR_BUTTON_LIGHT, face);
    set_color(appearance, XD_COLOR_BUTTON_HILIGHT, adjust_luma(face, 1000));
    set_color(appearance, XD_COLOR_BUTTON_SHADOW, adjust_luma(face, -400));
    set_color(appearance, XD_COLOR_BUTTON_DK_SHADOW, adjust_luma(face, -670));
}

static void set_font(advanced *appearance, const xd_font_spec *font) {
    int role = items[appearance->item].font;
    if (role == NONE)
        return;
    appearance->custom.fonts[role] = *font;
    appearance->custom.font_mask |= 1u << role;
    changed(appearance);
}

static xd_font_spec current_font(const advanced *appearance) {
    int role = items[appearance->item].font;
    return appearance->scheme.fonts[role == NONE ? XD_FONT_MESSAGE : role];
}

/* ---- the fields ------------------------------------------------------------------ */

static void load_families(advanced *appearance) {
    appearance->families = g_ptr_array_new_with_free_func(g_free);
    PangoFontFamily **fams = nullptr;
    int count = 0;
    pango_font_map_list_families(pango_cairo_font_map_get_default(), &fams, &count);
    for (int i = 0; i < count; i++)
        g_ptr_array_add(appearance->families, g_strdup(pango_font_family_get_name(fams[i])));
    g_free(fams);
    g_ptr_array_sort_values(appearance->families, (GCompareFunc)g_utf8_collate);
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
static void fill_fonts(advanced *appearance, const xd_font_spec *font) {
    xd_widget_clear_items(appearance->font_combo);
    g_ptr_array_set_size(appearance->font_names, 0);
    g_ptr_array_set_size(appearance->font_values, 0);
    if (items[appearance->item].font == NONE)
        return; /* greyed and empty; one a style overrides shows greyed */
    char *first = first_family(font->family);
    int sel = -1;
    for (guint i = 0; i < appearance->families->len; i++) {
        const char *name = g_ptr_array_index(appearance->families, i);
        bool match = g_ascii_strcasecmp(name, font->family) == 0 ||
                     (sel < 0 && g_ascii_strcasecmp(name, first) == 0);
        if (match)
            sel = (int)i;
        g_ptr_array_add(appearance->font_names, g_strdup(name));
        g_ptr_array_add(appearance->font_values, g_strdup(match ? font->family : name));
    }
    if (sel < 0) {
        guint at = 0;
        while (at < appearance->font_names->len &&
               g_utf8_collate(g_ptr_array_index(appearance->font_names, at), first) < 0)
            at++;
        g_ptr_array_insert(appearance->font_names, (gint)at, g_strdup(first));
        g_ptr_array_insert(appearance->font_values, (gint)at, g_strdup(font->family));
        sel = (int)at;
    }
    g_free(first);
    for (guint i = 0; i < appearance->font_names->len; i++)
        xd_widget_add_item(appearance->font_combo, g_ptr_array_index(appearance->font_names, i));
    xd_widget_select(appearance->font_combo, sel);
}

static int compare_sizes(const void *left, const void *right) {
    double left_size = *(const double *)left, right_size = *(const double *)right;
    return (left_size > right_size) - (left_size < right_size);
}

/* The Size list: Windows' usual sizes, and the font's own if it's another
 * (a Large Fonts scheme's 12.5). */
static void fill_sizes(advanced *appearance, double now) {
    xd_widget_clear_items(appearance->font_size);
    appearance->size_count = 0;
    if (items[appearance->item].font == NONE)
        return;
    bool listed = false;
    for (size_t i = 0; i < XD_ARRAY_LEN(font_sizes); i++) {
        appearance->sizes[appearance->size_count++] = font_sizes[i];
        listed = listed || fabs(font_sizes[i] - now) < 0.01;
    }
    if (!listed) {
        appearance->sizes[appearance->size_count++] = now;
        qsort(appearance->sizes, appearance->size_count, sizeof(appearance->sizes[0]),
              compare_sizes);
    }
    int sel = 0;
    for (size_t i = 0; i < appearance->size_count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%g", appearance->sizes[i]);
        xd_widget_add_item(appearance->font_size, buf);
        if (fabs(appearance->sizes[i] - now) < 0.01)
            sel = (int)i;
    }
    xd_widget_select(appearance->font_size, sel);
}

/* Shows the item's values, greying what it doesn't use. */
static void show_item(advanced *appearance) {
    const adv_item *it = &items[appearance->item];
    const xd_scheme *scheme = &appearance->scheme;

    bool size_on = it->metric != NONE && !styled(appearance, F_SIZE);
    char buf[16] = "";
    if (it->metric != NONE)
        snprintf(buf, sizeof(buf), "%d", scheme->metrics[it->metric]);
    xd_widget_set_text(appearance->size_edit, buf);
    xd_widget_set_enabled(appearance->size_edit, size_on);
    xd_widget_set_enabled(appearance->size_label, size_on);
    xd_widget_set_enabled(appearance->spin, size_on);
    xd_widget_redraw(appearance->spin);

    bool c1 = it->color1 != NONE && !styled(appearance, F_COLOR1);
    if (it->color1 != NONE)
        color_button_set(appearance->color1, scheme->colors[it->color1]);
    color_button_set_enabled(appearance->color1, c1);
    xd_widget_set_enabled(appearance->color1_label, c1);

    /* The second colour is a title bar's gradient: only with gradients. */
    bool c2 = it->color2 != NONE && scheme->gradient_captions && !styled(appearance, F_COLOR2);
    if (it->color2 != NONE)
        color_button_set(appearance->color2, scheme->colors[it->color2]);
    color_button_set_enabled(appearance->color2, c2);
    xd_widget_set_enabled(appearance->color2_label, c2);

    appearance->font_on = it->font != NONE && !styled(appearance, F_FONT);
    xd_font_spec font = current_font(appearance);
    fill_fonts(appearance, &font);
    fill_sizes(appearance, font.size_pt);
    xd_widget_set_enabled(appearance->font_combo, appearance->font_on);
    xd_widget_set_enabled(appearance->font_label, appearance->font_on);
    xd_widget_set_enabled(appearance->font_size, appearance->font_on);
    xd_widget_set_enabled(appearance->font_size_label, appearance->font_on);
    xd_widget_set_enabled(appearance->bold, appearance->font_on);
    xd_widget_set_enabled(appearance->italic, appearance->font_on);
    xd_widget_redraw(appearance->bold);
    xd_widget_redraw(appearance->italic);

    bool fc = it->font_color != NONE && !styled(appearance, F_FONT_COLOR);
    if (it->font_color != NONE)
        color_button_set(appearance->font_color, scheme->colors[it->font_color]);
    color_button_set_enabled(appearance->font_color, fc);
    xd_widget_set_enabled(appearance->font_color_label, fc);
}

static void select_item(advanced *appearance, int item) {
    if (item < 0 || item >= IT_COUNT)
        return;
    appearance->item = item;
    xd_widget_select(appearance->item_combo, item);
    show_item(appearance);
}

static void on_item(xd_widget *widget, void *user) {
    select_item(user, xd_widget_selected(widget));
}

static void set_size(advanced *appearance, int size) {
    const adv_item *it = &items[appearance->item];
    if (it->metric == NONE || size < it->min || size > it->max)
        return;
    appearance->custom.metrics[it->metric] = size;
    appearance->custom.metric_mask |= 1u << it->metric;
    changed(appearance);
}

static void on_size_edit(xd_widget *widget, void *user) {
    advanced *appearance = user;
    int64_t parsed;
    /* Read once it's a number in range; typing on until it is. */
    if (xd_str_parse_i64(xd_str_from_cstr(xd_widget_text(widget)), 0, 1000, &parsed))
        set_size(appearance, (int)parsed);
}

static void spin_step(advanced *appearance) {
    const adv_item *it = &items[appearance->item];
    if (it->metric == NONE)
        return;
    int size =
        XD_CLAMP(appearance->scheme.metrics[it->metric] + appearance->spin_dir, it->min, it->max);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", size);
    xd_widget_set_text(appearance->size_edit, buf);
    set_size(appearance, size);
}

/* A held arrow repeats, as an up-down control's does. */
static void spin_repeat(void *user) {
    advanced *appearance = user;
    if (!appearance->spin_repeating) {
        /* The first delay was a one-shot timer, done with now. */
        appearance->spin_repeating = true;
        appearance->spin_timer = xd_ui_add_timer(appearance->ui, 50, true, spin_repeat, appearance);
    }
    spin_step(appearance);
}

static void spin_stop(advanced *appearance) {
    xd_ui_remove_source(appearance->spin_timer);
    appearance->spin_timer = nullptr;
    appearance->spin_repeating = false;
    appearance->spin_dir = 0;
}

static void on_spin_pointer(xd_widget *widget, xd_pointer_event ev, [[maybe_unused]] double x,
                            double y, void *user) {
    advanced *appearance = user;
    if (ev == XD_POINTER_PRESS) {
        appearance->spin_dir = y < 21 / 2.0 ? 1 : -1;
        spin_step(appearance);
        spin_stop(appearance);
        appearance->spin_dir = y < 21 / 2.0 ? 1 : -1;
        appearance->spin_timer =
            xd_ui_add_timer(appearance->ui, 400, false, spin_repeat, appearance);
    } else if (ev == XD_POINTER_RELEASE) {
        spin_stop(appearance);
    }
    xd_widget_redraw(widget);
}

static void draw_arrow(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool up, bool enabled) {
    double half_width = xd_tpx(theme, 3);
    double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h / 2.0;
    double dir = up ? -1 : 1;
    cairo_move_to(cairo, cx - half_width, cy - dir * half_width / 2);
    cairo_line_to(cairo, cx + half_width, cy - dir * half_width / 2);
    cairo_line_to(cairo, cx, cy + dir * half_width / 2);
    cairo_close_path(cairo);
    xd_set_color(cairo, xd_sys(theme, enabled ? XD_COLOR_BUTTON_TEXT : XD_COLOR_GRAY_TEXT));
    cairo_fill(cairo);
}

/* The up-down control beside Size: two half-height buttons. */
static void draw_spin([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                      const xd_theme *theme, void *user) {
    advanced *appearance = user;
    const adv_item *it = &items[appearance->item];
    bool on = it->metric != NONE && !styled(appearance, F_SIZE);
    int half = rect.h / 2;
    xd_rect up = {rect.x, rect.y, rect.w, half},
            down = {rect.x, rect.y + half, rect.w, rect.h - half};
    uint8_t dis = on ? 0 : XD_STATE_DISABLED;
    xd_theme_draw_button(theme, cairo, up, dis | (appearance->spin_dir > 0 ? XD_STATE_PRESSED : 0),
                         "");
    xd_theme_draw_button(theme, cairo, down,
                         dis | (appearance->spin_dir < 0 ? XD_STATE_PRESSED : 0), "");
    draw_arrow(theme, cairo, up, true, on);
    draw_arrow(theme, cairo, down, false, on);
}

static void on_color1([[maybe_unused]] color_button *button, xd_rgb rgb, void *user) {
    advanced *appearance = user;
    int color_index = items[appearance->item].color1;
    if (color_index == NONE)
        return;
    if (color_index == XD_COLOR_BUTTON_FACE)
        set_3d_color(appearance, rgb);
    else
        set_color(appearance, color_index, rgb);
    changed(appearance);
}

static void on_color2([[maybe_unused]] color_button *button, xd_rgb rgb, void *user) {
    advanced *appearance = user;
    if (items[appearance->item].color2 != NONE) {
        set_color(appearance, items[appearance->item].color2, rgb);
        changed(appearance);
    }
}

static void on_font_color([[maybe_unused]] color_button *button, xd_rgb rgb, void *user) {
    advanced *appearance = user;
    if (items[appearance->item].font_color != NONE) {
        set_color(appearance, items[appearance->item].font_color, rgb);
        changed(appearance);
    }
}

static void on_font(xd_widget *widget, void *user) {
    advanced *appearance = user;
    int i = xd_widget_selected(widget);
    if (i < 0 || (guint)i >= appearance->font_values->len)
        return;
    xd_font_spec font = current_font(appearance);
    const char *family = g_ptr_array_index(appearance->font_values, (guint)i);
    snprintf(font.family, sizeof(font.family), "%s", family);
    set_font(appearance, &font);
}

static void on_font_size(xd_widget *widget, void *user) {
    advanced *appearance = user;
    int i = xd_widget_selected(widget);
    if (i < 0 || (size_t)i >= appearance->size_count)
        return;
    xd_font_spec font = current_font(appearance);
    font.size_pt = appearance->sizes[i];
    set_font(appearance, &font);
}

/* Bold and Italic: push buttons that stay down, labelled B and I. */
static void draw_toggle(xd_widget *widget, cairo_t *cairo, xd_rect rect, const xd_theme *theme,
                        void *user) {
    advanced *appearance = user;
    bool italic = widget == appearance->italic;
    xd_font_spec font = current_font(appearance);
    bool down = appearance->font_on && (italic ? font.italic : font.bold);
    uint8_t state = (down ? XD_STATE_PRESSED : 0) | (appearance->font_on ? 0 : XD_STATE_DISABLED);
    xd_theme_draw_button(theme, cairo, rect, state, "");
    xd_font_spec label = theme->scheme.fonts[XD_FONT_MESSAGE];
    label.bold = !italic;
    label.italic = italic;
    xd_text_style st =
        xd_text(theme, XD_FONT_MESSAGE,
                xd_sys(theme, appearance->font_on ? XD_COLOR_BUTTON_TEXT : XD_COLOR_GRAY_TEXT),
                XD_ALIGN_CENTER);
    st.font = &label;
    int shift = down ? xd_tpx(theme, 1) : 0;
    xd_draw_text(cairo, (xd_rect){rect.x + shift, rect.y + shift, rect.w, rect.h},
                 italic ? "I" : "B", &st);
}

static void on_toggle_pointer(xd_widget *widget, xd_pointer_event ev, [[maybe_unused]] double x,
                              [[maybe_unused]] double y, void *user) {
    advanced *appearance = user;
    if (ev != XD_POINTER_PRESS || !appearance->font_on)
        return;
    xd_font_spec font = current_font(appearance);
    if (widget == appearance->italic)
        font.italic = !font.italic;
    else
        font.bold = !font.bold;
    set_font(appearance, &font);
    xd_widget_redraw(widget);
}

/* ---- the preview ------------------------------------------------------------------ */

static void add_hit(advanced *appearance, xd_rect rect, int item) {
    if (appearance->hit_count < MAX_HITS)
        appearance->hits[appearance->hit_count++] = (hit){rect, item};
}

/* A window's frame and its parts. */
static xd_rect preview_frame(advanced *appearance, const xd_theme *theme, cairo_t *cairo,
                             xd_rect outer, const xd_frame *frame, int border_item, int title_item,
                             int client_item) {
    xd_theme_draw_frame(theme, cairo, outer, frame);
    xd_insets in = xd_theme_frame_insets(theme, frame);
    xd_rect client = xd_rect_inset4(outer, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cairo, client, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    add_hit(appearance, outer, border_item);
    xd_frame_layout frame_layout;
    xd_theme_frame_layout(theme, outer, frame, &frame_layout);
    add_hit(appearance, frame_layout.caption, title_item);
    for (size_t i = 0; i < frame_layout.button_count; i++)
        add_hit(appearance, frame_layout.button_rects[i], IT_CAPTION_BUTTONS);
    add_hit(appearance, client, client_item);
    return client;
}

/* Windows' preview: an inactive window behind an active one with a menu
 * bar (Normal, Disabled, Selected) and a window of text with a scroll bar,
 * and a message box on top. */
static void draw_preview([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                         const xd_theme *wt, void *user) {
    advanced *appearance = user;
    xd_theme pt;
    xd_theme_init(&pt, XD_STYLE_CLASSIC, &appearance->scheme, wt->scale);
    const xd_theme *theme = &pt;
    appearance->hit_count = 0;
    appearance->preview_r = rect;
    appearance->preview_scale = wt->scale;

    xd_rect well = xd_theme_draw_field(wt, cairo, rect, false);
    xd_fill_rect(cairo, well, xd_sys(theme, XD_COLOR_BACKGROUND));
    add_hit(appearance, well, IT_DESKTOP);
    cairo_save(cairo);
    cairo_rectangle(cairo, well.x, well.y, well.w, well.h);
    cairo_clip(cairo);

    uint8_t buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE;
    xd_frame inactive = {.title = "Inactive Window", .buttons = buttons};
    xd_rect r1 = {well.x + xd_tpx(theme, 8), well.y + xd_tpx(theme, 6), well.w - xd_tpx(theme, 56),
                  xd_tpx(theme, 80)};
    preview_frame(appearance, theme, cairo, r1, &inactive, IT_INACTIVE_BORDER, IT_INACTIVE_TITLE,
                  IT_3D);

    xd_frame active = {.title = "Active Window", .active = true, .buttons = buttons};
    xd_rect r2 = {well.x + xd_tpx(theme, 18), well.y + xd_tpx(theme, 30),
                  well.w - xd_tpx(theme, 36), well.h - xd_tpx(theme, 36)};
    xd_rect client = preview_frame(appearance, theme, cairo, r2, &active, IT_ACTIVE_BORDER,
                                   IT_ACTIVE_TITLE, IT_3D);

    /* The menu bar. */
    int menu_h = xd_metric_px(theme, XD_METRIC_MENU_HEIGHT);
    xd_rect bar = {client.x, client.y, client.w, menu_h};
    xd_fill_rect(cairo, bar,
                 xd_sys(theme, theme->scheme.flat_menus ? XD_COLOR_MENU_BAR : XD_COLOR_MENU));
    add_hit(appearance, bar, IT_MENU);
    static const char *const labels[] = {"Normal", "Disabled", "Selected"};
    int x = bar.x + xd_tpx(theme, 2);
    for (int i = 0; i < 3; i++) {
        xd_text_style st =
            xd_text(theme, XD_FONT_MENU, xd_sys(theme, XD_COLOR_MENU_TEXT), XD_ALIGN_CENTER);
        int tw = xd_text_width(cairo, labels[i], &st) + xd_tpx(theme, 14);
        xd_rect ir = {x, bar.y, tw, bar.h};
        if (i == 1) {
            /* Disabled: grey with a highlight under it, as classic menus. */
            int pixel = xd_tpx(theme, 1);
            st.color = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT);
            xd_draw_text(cairo, (xd_rect){ir.x + pixel, ir.y + pixel, ir.w, ir.h}, labels[i], &st);
            st.color = xd_sys(theme, XD_COLOR_GRAY_TEXT);
        } else if (i == 2) {
            bool flat = theme->scheme.flat_menus;
            xd_fill_rect(cairo, ir, xd_sys(theme, flat ? XD_COLOR_MENU_HILIGHT : XD_COLOR_HILIGHT));
            st.color = xd_sys(theme, XD_COLOR_HILIGHT_TEXT);
            add_hit(appearance, ir, IT_SELECTED);
        }
        xd_draw_text(cairo, ir, labels[i], &st);
        x += tw;
    }

    /* The window: text and a scroll bar. */
    xd_rect area = xd_rect_inset4(client, xd_tpx(theme, 2), menu_h + xd_tpx(theme, 2),
                                  xd_tpx(theme, 2), xd_tpx(theme, 2));
    xd_rect text = xd_theme_draw_field(theme, cairo, area, false);
    xd_fill_rect(cairo, text, xd_sys(theme, XD_COLOR_WINDOW));
    add_hit(appearance, area, IT_WINDOW);
    int sw = xd_metric_px(theme, XD_METRIC_SCROLL_WIDTH);
    xd_rect sb = {text.x + text.w - sw, text.y, sw, text.h};
    xd_theme_draw_vscrollbar(theme, cairo, sb, 0, 4, 10);
    add_hit(appearance, sb, IT_SCROLLBAR);
    xd_text_style wt_st =
        xd_text(theme, XD_FONT_MESSAGE, xd_sys(theme, XD_COLOR_WINDOW_TEXT), XD_ALIGN_START);
    xd_draw_text(cairo,
                 (xd_rect){text.x + xd_tpx(theme, 3), text.y + xd_tpx(theme, 2), text.w - sw,
                           xd_tpx(theme, 16)},
                 "Window Text", &wt_st);

    /* The message box. */
    xd_frame box = {.title = "Message Box", .active = true, .dialog = true,
                    .buttons = XD_CAPBTN_CLOSE};
    xd_insets ib = xd_theme_frame_insets(theme, &box);
    int bw = XD_MIN(xd_tpx(theme, 170), text.w - sw - xd_tpx(theme, 24));
    int bh = ib.top + ib.bottom + xd_tpx(theme, 54);
    /* Right of the window's text, as low as fits. */
    xd_rect r3 = {text.x + text.w - sw - bw - xd_tpx(theme, 10),
                  XD_MIN(text.y + xd_tpx(theme, 10), well.y + well.h - bh), bw, bh};
    xd_rect bc = preview_frame(appearance, theme, cairo, r3, &box, IT_ACTIVE_BORDER,
                               IT_ACTIVE_TITLE, IT_MESSAGE_BOX);
    xd_text_style mst =
        xd_text(theme, XD_FONT_MESSAGE, xd_sys(theme, XD_COLOR_WINDOW_TEXT), XD_ALIGN_START);
    xd_draw_text(cairo,
                 (xd_rect){bc.x + xd_tpx(theme, 8), bc.y + xd_tpx(theme, 4),
                           bc.w - xd_tpx(theme, 8), xd_tpx(theme, 16)},
                 "Message Text", &mst);
    int okw = xd_tpx(theme, 66);
    xd_rect ok = {bc.x + (bc.w - okw) / 2, bc.y + xd_tpx(theme, 26), okw, xd_tpx(theme, 22)};
    xd_theme_draw_button(theme, cairo, ok, XD_STATE_DEFAULT, "OK");
    add_hit(appearance, ok, IT_3D);
    cairo_restore(cairo);
}

/* A click in the preview picks the item under it (the topmost part). */
static void on_preview_pointer([[maybe_unused]] xd_widget *widget, xd_pointer_event ev, double x,
                               double y, void *user) {
    advanced *appearance = user;
    if (ev != XD_POINTER_PRESS)
        return;
    /* The hits are in device px; the pointer is logical, in the preview. */
    double dx = appearance->preview_r.x + x * appearance->preview_scale;
    double dy = appearance->preview_r.y + y * appearance->preview_scale;
    for (size_t i = appearance->hit_count; i-- > 0;) {
        xd_rect rect = appearance->hits[i].r;
        if (dx >= rect.x && dy >= rect.y && dx < rect.x + rect.w && dy < rect.y + rect.h) {
            select_item(appearance, appearance->hits[i].item);
            return;
        }
    }
}

/* ---- the dialog -------------------------------------------------------------------- */

static void finish(advanced *appearance, bool ok) {
    spin_stop(appearance);
    color_button_free(appearance->color1);
    color_button_free(appearance->color2);
    color_button_free(appearance->font_color);
    g_ptr_array_unref(appearance->families);
    g_ptr_array_unref(appearance->font_names);
    g_ptr_array_unref(appearance->font_values);
    xd_window_close(appearance->win);
    xd_scheme_custom custom = appearance->custom;
    xd_scheme_custom_trim(&custom, &appearance->base);
    advanced_done done = appearance->done;
    void *user = appearance->user;
    xd_free(appearance);
    done(ok ? &custom : nullptr, user);
}

void advanced_cancel(advanced *appearance) { finish(appearance, false); }

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) { finish(user, true); }
static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) { finish(user, false); }

static constexpr int ADV_W = 404;
static constexpr int ADV_H = 362;

advanced *advanced_open(xd_ui *ui, const xd_settings *pending, advanced_done done, void *user) {
    advanced *appearance = XD_NEW(advanced);
    appearance->ui = ui;
    appearance->done = done;
    appearance->user = user;
    appearance->style = pending->style;
    xd_settings plain = *pending;
    plain.custom_scheme = (xd_scheme_custom){0};
    xd_settings_scheme(&plain, &appearance->base);
    appearance->custom = pending->custom_scheme;
    appearance->scheme = appearance->base;
    xd_scheme_apply_custom(&appearance->scheme, &appearance->custom);
    appearance->item = IT_DESKTOP; /* where Windows starts */
    load_families(appearance);
    appearance->font_names = g_ptr_array_new_with_free_func(g_free);
    appearance->font_values = g_ptr_array_new_with_free_func(g_free);

    /* Windows 7 opens it from Window Color and Appearance under the same
     * title. */
    const char *title =
        appearance->style == XD_STYLE_AERO ? "Window Color and Appearance" : "Advanced Appearance";
    xd_window *win = appearance->win = xd_window_create(ui, title, "xd-desk", ADV_W, ADV_H);
    appearance->preview = xd_custom(win, (xd_rect){12, 12, 380, 168}, draw_preview, appearance);
    xd_custom_set_pointer(appearance->preview, on_preview_pointer);
    xd_label(win, (xd_rect){12, 186, 380, 30},
             "If you select a windows and buttons setting other than Windows Classic, it will "
             "override the following settings, except in some older programs.",
             XD_LABEL_WRAP);

    xd_label(win, (xd_rect){12, 222, 186, 16}, "Item:", 0);
    appearance->item_combo = xd_combo(win, (xd_rect){12, 238, 186, 21}, false, on_item, appearance);
    for (int i = 0; i < IT_COUNT; i++)
        xd_widget_add_item(appearance->item_combo, items[i].name);
    xd_widget_select(appearance->item_combo, appearance->item);
    appearance->size_label = xd_label(win, (xd_rect){206, 222, 56, 16}, "Size:", 0);
    appearance->size_edit = xd_edit(win, (xd_rect){206, 238, 40, 21}, "");
    xd_edit_set_on_change(appearance->size_edit, on_size_edit, appearance);
    appearance->spin = xd_custom(win, (xd_rect){246, 238, 16, 21}, draw_spin, appearance);
    xd_custom_set_pointer(appearance->spin, on_spin_pointer);
    appearance->color1_label = xd_label(win, (xd_rect){270, 222, 56, 16}, "Color 1:", 0);
    appearance->color1 =
        color_button_create(win, (xd_rect){270, 237, 56, 23}, on_color1, appearance);
    appearance->color2_label = xd_label(win, (xd_rect){334, 222, 58, 16}, "Color 2:", 0);
    appearance->color2 =
        color_button_create(win, (xd_rect){334, 237, 56, 23}, on_color2, appearance);

    appearance->font_label = xd_label(win, (xd_rect){12, 268, 186, 16}, "Font:", 0);
    appearance->font_combo = xd_combo(win, (xd_rect){12, 284, 186, 21}, false, on_font, appearance);
    appearance->font_size_label = xd_label(win, (xd_rect){206, 268, 56, 16}, "Size:", 0);
    appearance->font_size =
        xd_combo(win, (xd_rect){206, 284, 56, 21}, false, on_font_size, appearance);
    appearance->font_color_label = xd_label(win, (xd_rect){270, 268, 56, 16}, "Color:", 0);
    appearance->font_color =
        color_button_create(win, (xd_rect){270, 283, 56, 23}, on_font_color, appearance);
    appearance->bold = xd_custom(win, (xd_rect){334, 283, 27, 23}, draw_toggle, appearance);
    xd_custom_set_pointer(appearance->bold, on_toggle_pointer);
    appearance->italic = xd_custom(win, (xd_rect){363, 283, 27, 23}, draw_toggle, appearance);
    xd_custom_set_pointer(appearance->italic, on_toggle_pointer);

    xd_widget *ok =
        xd_button(win, (xd_rect){ADV_W - 168, ADV_H - 35, 75, 23}, "OK", on_ok, appearance);
    xd_widget *cancel =
        xd_button(win, (xd_rect){ADV_W - 87, ADV_H - 35, 75, 23}, "Cancel", on_cancel, appearance);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, appearance->item_combo);
    show_item(appearance);
    xd_window_show(win);
    return appearance;
}
