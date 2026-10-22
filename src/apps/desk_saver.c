/* SPDX-License-Identifier: MIT
 * The screen savers' Settings dialogs, laid out after Windows 2000's
 * (Marquee Setup, Starfield Simulation Setup, Mystify Setup, ...). They
 * edit a copy of the settings; OK hands it back to Display Properties,
 * which saves it at once, as Windows' savers wrote their own registry
 * keys on OK whether or not Display Properties was then applied.
 *
 * libxdui has no up-down (spin) control, so the counts Windows typed into
 * an edit box with arrows (Starfield density, Mystify's lines...) are
 * drop-down lists here.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pango/pangocairo.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "desk_saver.h"

/* The colours Windows' Marquee and Font dialogs listed by name. */
static const struct {
    const char *name;
    xd_rgb rgb;
} named_colors[] = {
    {"Black", {0, 0, 0}},     {"Maroon", {128, 0, 0}},     {"Green", {0, 128, 0}},
    {"Olive", {128, 128, 0}}, {"Navy", {0, 0, 128}},       {"Purple", {128, 0, 128}},
    {"Teal", {0, 128, 128}},  {"Gray", {128, 128, 128}},   {"Silver", {192, 192, 192}},
    {"Red", {255, 0, 0}},     {"Lime", {0, 255, 0}},       {"Yellow", {255, 255, 0}},
    {"Blue", {0, 0, 255}},    {"Fuchsia", {255, 0, 255}},  {"Aqua", {0, 255, 255}},
    {"White", {255, 255, 255}},
};
#define NAMED_COLORS ((int)XD_ARRAY_LEN(named_colors))

/* A colour drop-down: the named colours, and "Custom" for one that isn't
 * among them (set in desktop.ini by hand). */
typedef struct color_combo {
    xd_widget *w;
    xd_rgb custom;
} color_combo;

/* A drop-down of counts: min, min + step, ... */
typedef struct count_combo {
    xd_widget *w;
    int min, step;
} count_combo;

struct saver_setup {
    xd_ui *ui;
    xd_window *win;
    char id[16];
    xd_saver_settings edit;
    saver_setup_done done;
    void *user;

    /* Marquee */
    xd_widget *centered, *random_pos, *speed, *text, *format;
    color_combo background;
    xd_widget *example;
    xd_ui_source *anim;
    double example_x;
    /* Format Text */
    xd_window *fmt;
    xd_widget *fmt_font, *fmt_style, *fmt_size;
    color_combo fmt_color;
    char **families;
    size_t family_count;
    int fmt_sizes[24];
    size_t fmt_size_count;

    /* Starfield, Flying Penguins, Beziers, Mystify */
    count_combo count1, count2;

    /* Mystify */
    xd_widget *shape, *active, *two_colors, *random_colors, *clear;
    color_combo shape_colors[2];
    int shape_shown;

    /* 3D Pipes */
    xd_widget *multiple;
    xd_widget *joints[XD_PIPES_JOINT_COUNT];

    /* 3D Text */
    xd_widget *show_time, *text3d, *rotation, *size, *reflection;
    color_combo text_color;
};

/* ---- shared pieces ------------------------------------------------------------ */

static void color_combo_make(color_combo *combo, xd_window *win, xd_rect rect, xd_rgb now,
                             xd_widget_cb on_change, void *user) {
    combo->w = xd_combo(win, rect, false, on_change, user);
    combo->custom = now;
    int sel = -1;
    for (int i = 0; i < NAMED_COLORS; i++) {
        xd_widget_add_item(combo->w, named_colors[i].name);
        xd_rgb color = named_colors[i].rgb;
        if (color.r == now.r && color.g == now.g && color.b == now.b)
            sel = i;
    }
    if (sel < 0) {
        xd_widget_add_item(combo->w, "Custom");
        sel = NAMED_COLORS;
    }
    xd_widget_select(combo->w, sel);
}

static xd_rgb color_combo_value(const color_combo *combo) {
    int i = xd_widget_selected(combo->w);
    return i >= 0 && i < NAMED_COLORS ? named_colors[i].rgb : combo->custom;
}

static void count_combo_make(count_combo *combo, xd_window *win, xd_rect rect, int min, int max,
                             int step, int now) {
    combo->w = xd_combo(win, rect, false, nullptr, nullptr);
    combo->min = min;
    combo->step = step;
    int sel = 0;
    for (int value = min, i = 0; value <= max; value += step, i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", value);
        xd_widget_add_item(combo->w, buf);
        if (abs(value - now) < abs(min + sel * step - now))
            sel = i;
    }
    xd_widget_select(combo->w, sel);
}

static int count_combo_value(const count_combo *combo) {
    return combo->min + XD_MAX(xd_widget_selected(combo->w), 0) * combo->step;
}

/* A trackbar in a group box, its ends labelled underneath. */
static xd_widget *slider_group(xd_window *win, xd_rect rect, const char *title, const char *lo,
                               const char *hi, int value) {
    xd_groupbox(win, rect, title);
    xd_widget *widget = xd_slider(win, (xd_rect){rect.x + 10, rect.y + 18, rect.w - 20, 26}, 0,
                                  XD_SAVER_SLIDER_MAX, value, nullptr, nullptr);
    xd_label(win, (xd_rect){rect.x + 14, rect.y + 46, 48, 14}, lo, 0);
    xd_label(win, (xd_rect){rect.x + rect.w - 42, rect.y + 46, 32, 14}, hi, 0);
    return widget;
}

static void finish(saver_setup *setup, bool ok) {
    if (setup->anim != nullptr)
        xd_ui_remove_source(setup->anim);
    if (setup->fmt != nullptr)
        xd_window_close(setup->fmt);
    for (size_t i = 0; i < setup->family_count; i++)
        xd_free(setup->families[i]);
    xd_free(setup->families);
    xd_window_close(setup->win);
    saver_setup_done done = setup->done;
    void *user = setup->user;
    xd_saver_settings edit = setup->edit;
    xd_free(setup);
    done(ok ? &edit : nullptr, user);
}

void saver_setup_cancel(saver_setup *setup) { finish(setup, false); }

static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) { finish(user, false); }

static void read_back(saver_setup *setup);

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) {
    saver_setup *setup = user;
    if (setup->fmt != nullptr)
        return; /* answer Format Text first */
    read_back(setup);
    finish(setup, true);
}

static xd_window *make_window(saver_setup *setup, const char *title, int width, int height) {
    setup->win = xd_window_create(setup->ui, title, "xd-desk", width, height);
    xd_widget *ok =
        xd_button(setup->win, (xd_rect){width - 168, height - 33, 75, 23}, "OK", on_ok, setup);
    xd_widget *cancel = xd_button(setup->win, (xd_rect){width - 87, height - 33, 75, 23}, "Cancel",
                                  on_cancel, setup);
    xd_window_set_default(setup->win, ok);
    xd_window_set_cancel(setup->win, cancel);
    return setup->win;
}

/* ---- Marquee ------------------------------------------------------------------ */

/* Text Example: the text as formatted, going by. */
static void draw_example(xd_widget *widget, cairo_t *cairo, xd_rect rect, const xd_theme *theme,
                         void *user) {
    (void)widget;
    saver_setup *setup = user;
    const xd_marquee_settings *marquee_settings = &setup->edit.marquee;
    const char *text = xd_widget_text(setup->text);
    xd_fill_rect(cairo, rect, xd_rgba_from_rgb(color_combo_value(&setup->background)));
    xd_font_spec font = marquee_settings->font;
    snprintf(font.family, sizeof(font.family), "%.100s, Liberation Serif, serif",
             marquee_settings->font.family);
    /* At its own size, as far as the box is high. */
    font.size_pt = fmin(font.size_pt, rect.h / theme->scale * 0.6);
    xd_text_style st = {
        .font = &font, .scale = theme->scale, .color = xd_rgba_from_rgb(marquee_settings->color)};
    int tw = xd_text_width(cairo, text, &st);
    double span = rect.w + tw;
    double x = rect.x + rect.w - fmod(setup->example_x * theme->scale, span);
    cairo_save(cairo);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_clip(cairo);
    xd_draw_text(cairo, (xd_rect){(int)x, rect.y, tw + 4, rect.h}, text, &st);
    cairo_restore(cairo);
}

static void example_tick(void *user) {
    saver_setup *setup = user;
    double k = 0.25 + 1.5 * xd_slider_value(setup->speed) / (double)XD_SAVER_SLIDER_MAX;
    setup->example_x += 6.0 * k;
    xd_widget_redraw(setup->example);
}

static void on_background([[maybe_unused]] xd_widget *widget, void *user) {
    saver_setup *setup = user;
    xd_widget_redraw(setup->example);
}

static void format_close(saver_setup *setup) {
    xd_window_close(setup->fmt);
    setup->fmt = nullptr;
    xd_widget_set_enabled(setup->format, true);
}

static void on_format_ok([[maybe_unused]] xd_widget *widget, void *user) {
    saver_setup *setup = user;
    xd_font_spec *font = &setup->edit.marquee.font;
    int i = xd_widget_selected(setup->fmt_font);
    if (i >= 0 && (size_t)i < setup->family_count)
        snprintf(font->family, sizeof(font->family), "%s", setup->families[i]);
    int st = xd_widget_selected(setup->fmt_style);
    font->italic = st == 1 || st == 3;
    font->bold = st >= 2;
    int z = xd_widget_selected(setup->fmt_size);
    if (z >= 0 && (size_t)z < setup->fmt_size_count)
        font->size_pt = setup->fmt_sizes[z];
    setup->edit.marquee.color = color_combo_value(&setup->fmt_color);
    format_close(setup);
    xd_widget_redraw(setup->example);
}

static void on_format_cancel([[maybe_unused]] xd_widget *widget, void *user) { format_close(user); }

static int compare_names(const void *left, const void *right) {
    return g_utf8_collate(*(char *const *)left, *(char *const *)right);
}

static void load_families(saver_setup *setup) {
    if (setup->families != nullptr)
        return;
    PangoFontFamily **fams = nullptr;
    int count = 0;
    pango_font_map_list_families(pango_cairo_font_map_get_default(), &fams, &count);
    setup->families = xd_alloc_array((size_t)count + 1, sizeof(char *));
    for (int i = 0; i < count; i++)
        setup->families[setup->family_count++] =
            xd_str_dup_cstr(xd_str_from_cstr(pango_font_family_get_name(fams[i])));
    g_free(fams);
    qsort(setup->families, setup->family_count, sizeof(char *), compare_names);
}

/* Windows' Font dialog, cut down to what Marquee uses. */
static void on_format(xd_widget *widget, void *user) {
    saver_setup *setup = user;
    if (setup->fmt != nullptr)
        return;
    xd_widget_set_enabled(widget, false);
    load_families(setup);
    const xd_font_spec *font = &setup->edit.marquee.font;
    /* The font named in desktop.ini (Times New Roman by default) is listed
     * even when it isn't installed, rather than changed behind the user's
     * back; the saver falls back to a serif face for it. */
    bool known = false;
    for (size_t i = 0; i < setup->family_count; i++)
        known = known || g_ascii_strcasecmp(setup->families[i], font->family) == 0;
    if (!known && font->family[0] != '\0') {
        /* Grown each time: a family whose name was cut short when saved
         * is never found, so it's added on every opening. */
        setup->families =
            xd_realloc_array(setup->families, setup->family_count + 1, sizeof(char *));
        setup->families[setup->family_count++] = xd_str_dup_cstr(xd_str_from_cstr(font->family));
        qsort(setup->families, setup->family_count, sizeof(char *), compare_names);
    }
    xd_window *win = setup->fmt = xd_window_create(setup->ui, "Format Text", "xd-desk", 380, 160);
    xd_label(win, (xd_rect){12, 12, 160, 16}, "Font:", 0);
    setup->fmt_font = xd_combo(win, (xd_rect){12, 30, 170, 21}, false, nullptr, nullptr);
    int sel = 0;
    for (size_t i = 0; i < setup->family_count; i++) {
        xd_widget_add_item(setup->fmt_font, setup->families[i]);
        if (g_ascii_strcasecmp(setup->families[i], font->family) == 0)
            sel = (int)i;
    }
    xd_widget_select(setup->fmt_font, sel);
    xd_label(win, (xd_rect){192, 12, 100, 16}, "Font style:", 0);
    setup->fmt_style = xd_combo(win, (xd_rect){192, 30, 110, 21}, false, nullptr, nullptr);
    static const char *const styles[] = {"Regular", "Italic", "Bold", "Bold Italic"};
    for (size_t i = 0; i < XD_ARRAY_LEN(styles); i++)
        xd_widget_add_item(setup->fmt_style, styles[i]);
    xd_widget_select(setup->fmt_style, (font->bold ? 2 : 0) + (font->italic ? 1 : 0));
    xd_label(win, (xd_rect){312, 12, 56, 16}, "Size:", 0);
    setup->fmt_size = xd_combo(win, (xd_rect){312, 30, 56, 21}, false, nullptr, nullptr);
    /* The Font dialog's list, with the size now if it isn't one of them. */
    static const int sizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72};
    int now = (int)lround(font->size_pt);
    bool listed = false;
    setup->fmt_size_count = 0;
    for (size_t i = 0; i < XD_ARRAY_LEN(sizes); i++) {
        if (!listed && sizes[i] >= now) {
            if (sizes[i] != now)
                setup->fmt_sizes[setup->fmt_size_count++] = now;
            listed = true;
        }
        setup->fmt_sizes[setup->fmt_size_count++] = sizes[i];
    }
    if (!listed)
        setup->fmt_sizes[setup->fmt_size_count++] = now;
    for (size_t i = 0; i < setup->fmt_size_count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", setup->fmt_sizes[i]);
        xd_widget_add_item(setup->fmt_size, buf);
        if (setup->fmt_sizes[i] == now)
            xd_widget_select(setup->fmt_size, (int)i);
    }
    xd_groupbox(win, (xd_rect){12, 62, 170, 56}, "Effects");
    xd_label(win, (xd_rect){24, 86, 40, 16}, "Color:", 0);
    color_combo_make(&setup->fmt_color, win, (xd_rect){68, 83, 104, 21}, setup->edit.marquee.color,
                     nullptr, nullptr);
    xd_widget *ok = xd_button(win, (xd_rect){212, 127, 75, 23}, "OK", on_format_ok, setup);
    xd_widget *cancel =
        xd_button(win, (xd_rect){293, 127, 75, 23}, "Cancel", on_format_cancel, setup);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, setup->fmt_font);
    xd_window_show(win);
}

static void build_marquee(saver_setup *setup) {
    const xd_marquee_settings *marquee_settings = &setup->edit.marquee;
    xd_window *win = make_window(setup, "Marquee Setup", 360, 252);
    xd_groupbox(win, (xd_rect){12, 8, 110, 72}, "Position");
    setup->centered = xd_radio(win, (xd_rect){24, 28, 90, 18}, "Centered", true, nullptr, nullptr);
    setup->random_pos = xd_radio(win, (xd_rect){24, 52, 90, 18}, "Random", false, nullptr, nullptr);
    xd_radio_set(marquee_settings->random_position ? setup->random_pos : setup->centered);
    setup->speed = slider_group(win, (xd_rect){132, 8, 216, 72}, "Speed", "Slow", "Fast",
                                marquee_settings->speed);
    xd_label(win, (xd_rect){12, 94, 100, 16}, "Background Color:", 0);
    color_combo_make(&setup->background, win, (xd_rect){116, 91, 110, 21},
                     marquee_settings->background, on_background, setup);
    xd_label(win, (xd_rect){12, 124, 36, 16}, "Text:", 0);
    setup->text = xd_edit(win, (xd_rect){50, 121, 200, 21}, marquee_settings->text);
    setup->format = xd_button(win, (xd_rect){258, 120, 90, 23}, "Format Text...", on_format, setup);
    xd_groupbox(win, (xd_rect){12, 152, 336, 56}, "Text Example");
    setup->example = xd_custom(win, (xd_rect){22, 172, 316, 28}, draw_example, setup);
    /* Also picks up the Text box as it's typed in. */
    setup->anim = xd_ui_add_timer(setup->ui, 50, true, example_tick, setup);
    xd_window_set_focus(win, setup->text);
}

/* ---- Starfield, Flying Penguins, Beziers ------------------------------------------ */

static void build_starfield(saver_setup *setup) {
    xd_window *win = make_window(setup, "Starfield Simulation Setup", 300, 156);
    setup->speed = slider_group(win, (xd_rect){12, 8, 276, 72}, "Warp speed", "Slow", "Fast",
                                setup->edit.starfield.speed);
    xd_label(win, (xd_rect){12, 94, 110, 16}, "Starfield density:", 0);
    count_combo_make(&setup->count1, win, (xd_rect){124, 91, 64, 21}, XD_STARFIELD_DENSITY_MIN,
                     XD_STARFIELD_DENSITY_MAX, 10, setup->edit.starfield.density);
    xd_window_set_focus(win, setup->speed);
}

static void build_flying(saver_setup *setup) {
    xd_window *win = make_window(setup, "Flying Penguins Setup", 300, 156);
    setup->speed = slider_group(win, (xd_rect){12, 8, 276, 72}, "Warp speed", "Slow", "Fast",
                                setup->edit.flying.speed);
    xd_label(win, (xd_rect){12, 94, 170, 16}, "Density of flying penguins:", 0);
    count_combo_make(&setup->count1, win, (xd_rect){184, 91, 64, 21}, XD_FLYING_DENSITY_MIN,
                     XD_FLYING_DENSITY_MAX, 10, setup->edit.flying.density);
    xd_window_set_focus(win, setup->speed);
}

static void build_beziers(saver_setup *setup) {
    xd_window *win = make_window(setup, "Beziers Setup", 300, 180);
    xd_groupbox(win, (xd_rect){12, 8, 276, 56}, "Length");
    xd_label(win, (xd_rect){24, 32, 44, 16}, "Curves:", 0);
    count_combo_make(&setup->count1, win, (xd_rect){70, 29, 56, 21}, 1, XD_BEZIERS_CURVES_MAX, 1,
                     setup->edit.beziers.curves);
    xd_label(win, (xd_rect){148, 32, 40, 16}, "Lines:", 0);
    count_combo_make(&setup->count2, win, (xd_rect){190, 29, 56, 21}, 1, XD_BEZIERS_LINES_MAX, 1,
                     setup->edit.beziers.lines);
    setup->speed = slider_group(win, (xd_rect){12, 72, 276, 72}, "Speed", "Slow", "Fast",
                                setup->edit.beziers.speed);
    xd_window_set_focus(win, setup->count1.w);
}

/* ---- Mystify ------------------------------------------------------------------- */

/* The controls under Shape show one polygon's settings at a time. */
static void mystify_store(saver_setup *setup) {
    xd_mystify_shape *sh = &setup->edit.mystify.shapes[setup->shape_shown];
    sh->active = xd_checkbox_checked(setup->active);
    sh->lines = count_combo_value(&setup->count1);
    sh->two_colors = xd_checkbox_checked(setup->two_colors);
    sh->colors[0] = color_combo_value(&setup->shape_colors[0]);
    sh->colors[1] = color_combo_value(&setup->shape_colors[1]);
}

static void select_color(color_combo *combo, xd_rgb wanted) {
    combo->custom = wanted;
    int sel = -1;
    for (int i = 0; i < NAMED_COLORS; i++) {
        xd_rgb named_color = named_colors[i].rgb;
        if (named_color.r == wanted.r && named_color.g == wanted.g && named_color.b == wanted.b)
            sel = i;
    }
    if (sel < 0 && xd_widget_item_count(combo->w) == (size_t)NAMED_COLORS)
        xd_widget_add_item(combo->w, "Custom");
    xd_widget_select(combo->w, sel >= 0 ? sel : NAMED_COLORS);
}

static void mystify_enable(saver_setup *setup) {
    bool on = xd_checkbox_checked(setup->active);
    bool two = xd_checkbox_checked(setup->two_colors);
    xd_widget_set_enabled(setup->count1.w, on);
    xd_widget_set_enabled(setup->two_colors, on);
    xd_widget_set_enabled(setup->random_colors, on);
    xd_widget_set_enabled(setup->shape_colors[0].w, on && two);
    xd_widget_set_enabled(setup->shape_colors[1].w, on && two);
}

static void mystify_show(saver_setup *setup) {
    const xd_mystify_shape *sh = &setup->edit.mystify.shapes[setup->shape_shown];
    xd_checkbox_set(setup->active, sh->active);
    xd_widget_select(setup->count1.w, sh->lines - setup->count1.min);
    xd_radio_set(sh->two_colors ? setup->two_colors : setup->random_colors);
    select_color(&setup->shape_colors[0], sh->colors[0]);
    select_color(&setup->shape_colors[1], sh->colors[1]);
    mystify_enable(setup);
}

static void on_shape(xd_widget *widget, void *user) {
    saver_setup *setup = user;
    int i = xd_widget_selected(widget);
    if (i < 0 || i >= XD_MYSTIFY_SHAPES || i == setup->shape_shown)
        return;
    mystify_store(setup);
    setup->shape_shown = i;
    mystify_show(setup);
}

static void on_mystify_toggle([[maybe_unused]] xd_widget *widget, void *user) {
    mystify_enable(user);
}

static void build_mystify(saver_setup *setup) {
    xd_window *win = make_window(setup, "Mystify Setup", 320, 232);
    xd_groupbox(win, (xd_rect){12, 8, 296, 150}, "Shape");
    setup->shape = xd_combo(win, (xd_rect){24, 28, 110, 21}, false, on_shape, setup);
    xd_widget_add_item(setup->shape, "Polygon 1");
    xd_widget_add_item(setup->shape, "Polygon 2");
    xd_widget_select(setup->shape, 0);
    setup->active =
        xd_checkbox(win, (xd_rect){150, 29, 80, 18}, "Active", false, on_mystify_toggle, setup);
    xd_label(win, (xd_rect){24, 62, 40, 16}, "Lines:", 0);
    count_combo_make(&setup->count1, win, (xd_rect){66, 59, 56, 21}, 1, XD_MYSTIFY_LINES_MAX, 1, 1);
    xd_groupbox(win, (xd_rect){24, 88, 272, 62}, "Colors");
    setup->two_colors =
        xd_radio(win, (xd_rect){34, 106, 90, 18}, "Two colors", true, on_mystify_toggle, setup);
    setup->random_colors = xd_radio(win, (xd_rect){34, 126, 170, 18}, "Multiple random colors",
                                    false, on_mystify_toggle, setup);
    color_combo_make(&setup->shape_colors[0], win, (xd_rect){128, 104, 78, 21}, (xd_rgb){0, 0, 0},
                     nullptr, nullptr);
    color_combo_make(&setup->shape_colors[1], win, (xd_rect){210, 104, 78, 21}, (xd_rgb){0, 0, 0},
                     nullptr, nullptr);
    setup->clear = xd_checkbox(win, (xd_rect){12, 168, 120, 18}, "Clear screen",
                               setup->edit.mystify.clear_screen, nullptr, nullptr);
    setup->shape_shown = 0;
    mystify_show(setup);
    xd_window_set_focus(win, setup->shape);
}

/* ---- 3D Pipes ------------------------------------------------------------------ */

static void build_pipes(saver_setup *setup) {
    xd_window *win = make_window(setup, "3D Pipes Setup", 320, 188);
    xd_groupbox(win, (xd_rect){12, 8, 140, 64}, "Pipes");
    xd_widget *single = xd_radio(win, (xd_rect){24, 26, 110, 18}, "Single", true, nullptr, nullptr);
    setup->multiple =
        xd_radio(win, (xd_rect){24, 46, 110, 18}, "Multiple", false, nullptr, nullptr);
    xd_radio_set(setup->edit.pipes.multiple ? setup->multiple : single);
    xd_groupbox(win, (xd_rect){164, 8, 144, 108}, "Joint Type");
    static const char *const joints[XD_PIPES_JOINT_COUNT] = {
        [XD_PIPES_ELBOW] = "Elbow", [XD_PIPES_BALL] = "Ball", [XD_PIPES_MIXED] = "Mixed",
        [XD_PIPES_CYCLE] = "Cycle"};
    for (int i = 0; i < XD_PIPES_JOINT_COUNT; i++)
        setup->joints[i] = xd_radio(win, (xd_rect){176, 26 + 20 * i, 110, 18}, joints[i], i == 0,
                                    nullptr, nullptr);
    xd_radio_set(setup->joints[setup->edit.pipes.joints]);
    /* Windows' textures (and its Choose Texture...) aren't drawn here. */
    xd_groupbox(win, (xd_rect){12, 80, 140, 64}, "Surface Style");
    xd_radio_set(xd_radio(win, (xd_rect){24, 98, 110, 18}, "Solid", true, nullptr, nullptr));
    xd_widget_set_enabled(
        xd_radio(win, (xd_rect){24, 118, 110, 18}, "Textured", false, nullptr, nullptr), false);
    xd_window_set_focus(win, setup->edit.pipes.multiple ? setup->multiple : single);
}

/* ---- 3D Text ------------------------------------------------------------------- */

static void text3d_enable(saver_setup *setup) {
    xd_widget_set_enabled(setup->text3d, !xd_checkbox_checked(setup->show_time));
    xd_widget_set_enabled(setup->text_color.w, !xd_checkbox_checked(setup->reflection));
}

static void on_text3d_toggle([[maybe_unused]] xd_widget *widget, void *user) {
    text3d_enable(user);
}

static void build_text3d(saver_setup *setup) {
    const xd_text3d_settings *text3d_settings = &setup->edit.text3d;
    xd_window *win = make_window(setup, "3D Text Setup", 380, 296);
    xd_groupbox(win, (xd_rect){12, 8, 356, 64}, "Display");
    xd_widget *text =
        xd_radio(win, (xd_rect){24, 26, 60, 18}, "Text", true, on_text3d_toggle, setup);
    setup->text3d = xd_edit(win, (xd_rect){88, 24, 268, 21}, text3d_settings->text);
    setup->show_time =
        xd_radio(win, (xd_rect){24, 48, 60, 18}, "Time", false, on_text3d_toggle, setup);
    xd_radio_set(text3d_settings->show_time ? setup->show_time : text);

    xd_groupbox(win, (xd_rect){12, 80, 356, 100}, "Motion");
    xd_label(win, (xd_rect){24, 102, 84, 16}, "Rotation Type:", 0);
    setup->rotation = xd_combo(win, (xd_rect){110, 99, 110, 21}, false, nullptr, nullptr);
    static const char *const rotations[XD_TEXT3D_ROTATION_COUNT] = {
        [XD_TEXT3D_NONE] = "None", [XD_TEXT3D_SPIN] = "Spin", [XD_TEXT3D_SEESAW] = "See-Saw",
        [XD_TEXT3D_WOBBLE] = "Wobble", [XD_TEXT3D_RANDOM] = "Random"};
    for (int i = 0; i < XD_TEXT3D_ROTATION_COUNT; i++)
        xd_widget_add_item(setup->rotation, rotations[i]);
    xd_widget_select(setup->rotation, text3d_settings->rotation);
    xd_label(win, (xd_rect){24, 134, 40, 16}, "Speed", 0);
    setup->speed = xd_slider(win, (xd_rect){66, 128, 290, 26}, 0, XD_SAVER_SLIDER_MAX,
                             text3d_settings->speed, nullptr, nullptr);
    xd_label(win, (xd_rect){70, 156, 40, 14}, "Slow", 0);
    xd_label(win, (xd_rect){324, 156, 32, 14}, "Fast", 0);

    setup->size = slider_group(win, (xd_rect){12, 188, 170, 66}, "Size", "Small", "Large",
                               text3d_settings->size);
    xd_groupbox(win, (xd_rect){192, 188, 176, 66}, "Surface Style");
    xd_widget *solid =
        xd_radio(win, (xd_rect){202, 208, 84, 18}, "Solid Color", true, on_text3d_toggle, setup);
    setup->reflection =
        xd_radio(win, (xd_rect){202, 230, 84, 18}, "Reflection", false, on_text3d_toggle, setup);
    xd_radio_set(text3d_settings->surface == XD_TEXT3D_REFLECTION ? setup->reflection : solid);
    color_combo_make(&setup->text_color, win, (xd_rect){288, 206, 72, 21}, text3d_settings->color,
                     nullptr, nullptr);
    text3d_enable(setup);
    xd_window_set_focus(win, text3d_settings->show_time ? setup->show_time : setup->text3d);
}

/* ---- OK ------------------------------------------------------------------------- */

static void read_back(saver_setup *setup) {
    xd_saver_settings *saver_settings = &setup->edit;
    if (strcmp(setup->id, "marquee") == 0) {
        saver_settings->marquee.random_position = xd_checkbox_checked(setup->random_pos);
        saver_settings->marquee.speed = xd_slider_value(setup->speed);
        saver_settings->marquee.background = color_combo_value(&setup->background);
        snprintf(saver_settings->marquee.text, sizeof(saver_settings->marquee.text), "%s",
                 xd_widget_text(setup->text));
    } else if (strcmp(setup->id, "starfield") == 0) {
        saver_settings->starfield.speed = xd_slider_value(setup->speed);
        saver_settings->starfield.density = count_combo_value(&setup->count1);
    } else if (strcmp(setup->id, "flying") == 0) {
        saver_settings->flying.speed = xd_slider_value(setup->speed);
        saver_settings->flying.density = count_combo_value(&setup->count1);
    } else if (strcmp(setup->id, "beziers") == 0) {
        saver_settings->beziers.curves = count_combo_value(&setup->count1);
        saver_settings->beziers.lines = count_combo_value(&setup->count2);
        saver_settings->beziers.speed = xd_slider_value(setup->speed);
    } else if (strcmp(setup->id, "mystify") == 0) {
        mystify_store(setup);
        saver_settings->mystify.clear_screen = xd_checkbox_checked(setup->clear);
    } else if (strcmp(setup->id, "pipes") == 0) {
        saver_settings->pipes.multiple = xd_checkbox_checked(setup->multiple);
        for (int i = 0; i < XD_PIPES_JOINT_COUNT; i++) {
            if (xd_checkbox_checked(setup->joints[i]))
                saver_settings->pipes.joints = (xd_pipes_joint)i;
        }
    } else if (strcmp(setup->id, "text3d") == 0) {
        xd_text3d_settings *text3d_settings = &saver_settings->text3d;
        text3d_settings->show_time = xd_checkbox_checked(setup->show_time);
        snprintf(text3d_settings->text, sizeof(text3d_settings->text), "%s",
                 xd_widget_text(setup->text3d));
        int rotation = xd_widget_selected(setup->rotation);
        if (rotation >= 0 && rotation < XD_TEXT3D_ROTATION_COUNT)
            text3d_settings->rotation = (xd_text3d_rotation)rotation;
        text3d_settings->speed = xd_slider_value(setup->speed);
        text3d_settings->size = xd_slider_value(setup->size);
        text3d_settings->surface =
            xd_checkbox_checked(setup->reflection) ? XD_TEXT3D_REFLECTION : XD_TEXT3D_SOLID;
        text3d_settings->color = color_combo_value(&setup->text_color);
    }
}

saver_setup *saver_setup_open(xd_ui *ui, const char *id, const xd_saver_settings *current,
                              saver_setup_done done, void *user) {
    static const struct {
        const char *id;
        void (*build)(saver_setup *setup);
    } dialogs[] = {
        {"marquee", build_marquee}, {"starfield", build_starfield}, {"flying", build_flying},
        {"beziers", build_beziers}, {"mystify", build_mystify},     {"pipes", build_pipes},
        {"text3d", build_text3d},
    };
    for (size_t i = 0; id != nullptr && i < XD_ARRAY_LEN(dialogs); i++) {
        if (strcmp(dialogs[i].id, id) != 0)
            continue;
        saver_setup *setup = XD_NEW(saver_setup);
        setup->ui = ui;
        snprintf(setup->id, sizeof(setup->id), "%s", id);
        setup->edit = *current;
        setup->done = done;
        setup->user = user;
        dialogs[i].build(setup);
        xd_window_show(setup->win);
        return setup;
    }
    return nullptr;
}
