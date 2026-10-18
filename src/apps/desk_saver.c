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

static void color_combo_make(color_combo *c, xd_window *win, xd_rect r, xd_rgb now,
                             xd_widget_cb on_change, void *user) {
    c->w = xd_combo(win, r, false, on_change, user);
    c->custom = now;
    int sel = -1;
    for (int i = 0; i < NAMED_COLORS; i++) {
        xd_widget_add_item(c->w, named_colors[i].name);
        xd_rgb n = named_colors[i].rgb;
        if (n.r == now.r && n.g == now.g && n.b == now.b)
            sel = i;
    }
    if (sel < 0) {
        xd_widget_add_item(c->w, "Custom");
        sel = NAMED_COLORS;
    }
    xd_widget_select(c->w, sel);
}

static xd_rgb color_combo_value(const color_combo *c) {
    int i = xd_widget_selected(c->w);
    return i >= 0 && i < NAMED_COLORS ? named_colors[i].rgb : c->custom;
}

static void count_combo_make(count_combo *c, xd_window *win, xd_rect r, int min, int max,
                             int step, int now) {
    c->w = xd_combo(win, r, false, nullptr, nullptr);
    c->min = min;
    c->step = step;
    int sel = 0;
    for (int v = min, i = 0; v <= max; v += step, i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", v);
        xd_widget_add_item(c->w, buf);
        if (abs(v - now) < abs(min + sel * step - now))
            sel = i;
    }
    xd_widget_select(c->w, sel);
}

static int count_combo_value(const count_combo *c) {
    return c->min + XD_MAX(xd_widget_selected(c->w), 0) * c->step;
}

/* A trackbar in a group box, its ends labelled underneath. */
static xd_widget *slider_group(xd_window *win, xd_rect r, const char *title, const char *lo,
                               const char *hi, int value) {
    xd_groupbox(win, r, title);
    xd_widget *s = xd_slider(win, (xd_rect){r.x + 10, r.y + 18, r.w - 20, 26}, 0,
                             XD_SAVER_SLIDER_MAX, value, nullptr, nullptr);
    xd_label(win, (xd_rect){r.x + 14, r.y + 46, 48, 14}, lo, 0);
    xd_label(win, (xd_rect){r.x + r.w - 42, r.y + 46, 32, 14}, hi, 0);
    return s;
}

static void finish(saver_setup *s, bool ok) {
    if (s->anim != nullptr)
        xd_ui_remove_source(s->anim);
    if (s->fmt != nullptr)
        xd_window_close(s->fmt);
    for (size_t i = 0; i < s->family_count; i++)
        xd_free(s->families[i]);
    xd_free(s->families);
    xd_window_close(s->win);
    saver_setup_done done = s->done;
    void *user = s->user;
    xd_saver_settings edit = s->edit;
    xd_free(s);
    done(ok ? &edit : nullptr, user);
}

void saver_setup_cancel(saver_setup *s) { finish(s, false); }

static void on_cancel([[maybe_unused]] xd_widget *w, void *user) { finish(user, false); }

static void read_back(saver_setup *s);

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    saver_setup *s = user;
    if (s->fmt != nullptr)
        return; /* answer Format Text first */
    read_back(s);
    finish(s, true);
}

static xd_window *make_window(saver_setup *s, const char *title, int w, int h) {
    s->win = xd_window_create(s->ui, title, "xd-desk", w, h);
    xd_widget *ok = xd_button(s->win, (xd_rect){w - 168, h - 33, 75, 23}, "OK", on_ok, s);
    xd_widget *cancel =
        xd_button(s->win, (xd_rect){w - 87, h - 33, 75, 23}, "Cancel", on_cancel, s);
    xd_window_set_default(s->win, ok);
    xd_window_set_cancel(s->win, cancel);
    return s->win;
}

/* ---- Marquee ------------------------------------------------------------------ */

/* Text Example: the text as formatted, going by. */
static void draw_example(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t, void *user) {
    (void)w;
    saver_setup *s = user;
    const xd_marquee_settings *m = &s->edit.marquee;
    const char *text = xd_widget_text(s->text);
    xd_fill_rect(cr, r, xd_rgba_from_rgb(color_combo_value(&s->background)));
    xd_font_spec font = m->font;
    snprintf(font.family, sizeof(font.family), "%.100s, Liberation Serif, serif", m->font.family);
    /* At its own size, as far as the box is high. */
    font.size_pt = fmin(font.size_pt, r.h / t->scale * 0.6);
    xd_text_style st = {.font = &font, .scale = t->scale, .color = xd_rgba_from_rgb(m->color)};
    int tw = xd_text_width(cr, text, &st);
    double span = r.w + tw;
    double x = r.x + r.w - fmod(s->example_x * t->scale, span);
    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    xd_draw_text(cr, (xd_rect){(int)x, r.y, tw + 4, r.h}, text, &st);
    cairo_restore(cr);
}

static void example_tick(void *user) {
    saver_setup *s = user;
    double k = 0.25 + 1.5 * xd_slider_value(s->speed) / (double)XD_SAVER_SLIDER_MAX;
    s->example_x += 6.0 * k;
    xd_widget_redraw(s->example);
}

static void on_background([[maybe_unused]] xd_widget *w, void *user) {
    saver_setup *s = user;
    xd_widget_redraw(s->example);
}

static void format_close(saver_setup *s) {
    xd_window_close(s->fmt);
    s->fmt = nullptr;
    xd_widget_set_enabled(s->format, true);
}

static void on_format_ok([[maybe_unused]] xd_widget *w, void *user) {
    saver_setup *s = user;
    xd_font_spec *f = &s->edit.marquee.font;
    int i = xd_widget_selected(s->fmt_font);
    if (i >= 0 && (size_t)i < s->family_count)
        snprintf(f->family, sizeof(f->family), "%s", s->families[i]);
    int st = xd_widget_selected(s->fmt_style);
    f->italic = st == 1 || st == 3;
    f->bold = st >= 2;
    int z = xd_widget_selected(s->fmt_size);
    if (z >= 0 && (size_t)z < s->fmt_size_count)
        f->size_pt = s->fmt_sizes[z];
    s->edit.marquee.color = color_combo_value(&s->fmt_color);
    format_close(s);
    xd_widget_redraw(s->example);
}

static void on_format_cancel([[maybe_unused]] xd_widget *w, void *user) { format_close(user); }

static int compare_names(const void *a, const void *b) {
    return g_utf8_collate(*(char *const *)a, *(char *const *)b);
}

static void load_families(saver_setup *s) {
    if (s->families != nullptr)
        return;
    PangoFontFamily **fams = nullptr;
    int n = 0;
    pango_font_map_list_families(pango_cairo_font_map_get_default(), &fams, &n);
    s->families = xd_alloc_array((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++)
        s->families[s->family_count++] =
            xd_str_dup_cstr(xd_str_from_cstr(pango_font_family_get_name(fams[i])));
    g_free(fams);
    qsort(s->families, s->family_count, sizeof(char *), compare_names);
}

/* Windows' Font dialog, cut down to what Marquee uses. */
static void on_format(xd_widget *w, void *user) {
    saver_setup *s = user;
    if (s->fmt != nullptr)
        return;
    xd_widget_set_enabled(w, false);
    load_families(s);
    const xd_font_spec *f = &s->edit.marquee.font;
    /* The font named in desktop.ini (Times New Roman by default) is listed
     * even when it isn't installed, rather than changed behind the user's
     * back; the saver falls back to a serif face for it. */
    bool known = false;
    for (size_t i = 0; i < s->family_count; i++)
        known = known || g_ascii_strcasecmp(s->families[i], f->family) == 0;
    if (!known && f->family[0] != '\0') {
        /* load_families left room for one more. */
        s->families[s->family_count++] = xd_str_dup_cstr(xd_str_from_cstr(f->family));
        qsort(s->families, s->family_count, sizeof(char *), compare_names);
    }
    xd_window *win = s->fmt = xd_window_create(s->ui, "Format Text", "xd-desk", 380, 160);
    xd_label(win, (xd_rect){12, 12, 160, 16}, "Font:", 0);
    s->fmt_font = xd_combo(win, (xd_rect){12, 30, 170, 21}, false, nullptr, nullptr);
    int sel = 0;
    for (size_t i = 0; i < s->family_count; i++) {
        xd_widget_add_item(s->fmt_font, s->families[i]);
        if (g_ascii_strcasecmp(s->families[i], f->family) == 0)
            sel = (int)i;
    }
    xd_widget_select(s->fmt_font, sel);
    xd_label(win, (xd_rect){192, 12, 100, 16}, "Font style:", 0);
    s->fmt_style = xd_combo(win, (xd_rect){192, 30, 110, 21}, false, nullptr, nullptr);
    static const char *const styles[] = {"Regular", "Italic", "Bold", "Bold Italic"};
    for (size_t i = 0; i < XD_ARRAY_LEN(styles); i++)
        xd_widget_add_item(s->fmt_style, styles[i]);
    xd_widget_select(s->fmt_style, (f->bold ? 2 : 0) + (f->italic ? 1 : 0));
    xd_label(win, (xd_rect){312, 12, 56, 16}, "Size:", 0);
    s->fmt_size = xd_combo(win, (xd_rect){312, 30, 56, 21}, false, nullptr, nullptr);
    /* The Font dialog's list, with the size now if it isn't one of them. */
    static const int sizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72};
    int now = (int)lround(f->size_pt);
    bool listed = false;
    s->fmt_size_count = 0;
    for (size_t i = 0; i < XD_ARRAY_LEN(sizes); i++) {
        if (!listed && sizes[i] >= now) {
            if (sizes[i] != now)
                s->fmt_sizes[s->fmt_size_count++] = now;
            listed = true;
        }
        s->fmt_sizes[s->fmt_size_count++] = sizes[i];
    }
    if (!listed)
        s->fmt_sizes[s->fmt_size_count++] = now;
    for (size_t i = 0; i < s->fmt_size_count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s->fmt_sizes[i]);
        xd_widget_add_item(s->fmt_size, buf);
        if (s->fmt_sizes[i] == now)
            xd_widget_select(s->fmt_size, (int)i);
    }
    xd_groupbox(win, (xd_rect){12, 62, 170, 56}, "Effects");
    xd_label(win, (xd_rect){24, 86, 40, 16}, "Color:", 0);
    color_combo_make(&s->fmt_color, win, (xd_rect){68, 83, 104, 21}, s->edit.marquee.color,
                     nullptr, nullptr);
    xd_widget *ok = xd_button(win, (xd_rect){212, 127, 75, 23}, "OK", on_format_ok, s);
    xd_widget *cancel =
        xd_button(win, (xd_rect){293, 127, 75, 23}, "Cancel", on_format_cancel, s);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, s->fmt_font);
    xd_window_show(win);
}

static void build_marquee(saver_setup *s) {
    const xd_marquee_settings *m = &s->edit.marquee;
    xd_window *win = make_window(s, "Marquee Setup", 360, 252);
    xd_groupbox(win, (xd_rect){12, 8, 110, 72}, "Position");
    s->centered = xd_radio(win, (xd_rect){24, 28, 90, 18}, "Centered", true, nullptr, nullptr);
    s->random_pos = xd_radio(win, (xd_rect){24, 52, 90, 18}, "Random", false, nullptr, nullptr);
    xd_radio_set(m->random_position ? s->random_pos : s->centered);
    s->speed = slider_group(win, (xd_rect){132, 8, 216, 72}, "Speed", "Slow", "Fast", m->speed);
    xd_label(win, (xd_rect){12, 94, 100, 16}, "Background Color:", 0);
    color_combo_make(&s->background, win, (xd_rect){116, 91, 110, 21}, m->background,
                     on_background, s);
    xd_label(win, (xd_rect){12, 124, 36, 16}, "Text:", 0);
    s->text = xd_edit(win, (xd_rect){50, 121, 200, 21}, m->text);
    s->format = xd_button(win, (xd_rect){258, 120, 90, 23}, "Format Text...", on_format, s);
    xd_groupbox(win, (xd_rect){12, 152, 336, 56}, "Text Example");
    s->example = xd_custom(win, (xd_rect){22, 172, 316, 28}, draw_example, s);
    /* Also picks up the Text box as it's typed in. */
    s->anim = xd_ui_add_timer(s->ui, 50, true, example_tick, s);
    xd_window_set_focus(win, s->text);
}

/* ---- Starfield, Flying Penguins, Beziers ------------------------------------------ */

static void build_starfield(saver_setup *s) {
    xd_window *win = make_window(s, "Starfield Simulation Setup", 300, 156);
    s->speed = slider_group(win, (xd_rect){12, 8, 276, 72}, "Warp speed", "Slow", "Fast",
                            s->edit.starfield.speed);
    xd_label(win, (xd_rect){12, 94, 110, 16}, "Starfield density:", 0);
    count_combo_make(&s->count1, win, (xd_rect){124, 91, 64, 21}, XD_STARFIELD_DENSITY_MIN,
                     XD_STARFIELD_DENSITY_MAX, 10, s->edit.starfield.density);
    xd_window_set_focus(win, s->speed);
}

static void build_flying(saver_setup *s) {
    xd_window *win = make_window(s, "Flying Penguins Setup", 300, 156);
    s->speed = slider_group(win, (xd_rect){12, 8, 276, 72}, "Warp speed", "Slow", "Fast",
                            s->edit.flying.speed);
    xd_label(win, (xd_rect){12, 94, 170, 16}, "Density of flying penguins:", 0);
    count_combo_make(&s->count1, win, (xd_rect){184, 91, 64, 21}, XD_FLYING_DENSITY_MIN,
                     XD_FLYING_DENSITY_MAX, 10, s->edit.flying.density);
    xd_window_set_focus(win, s->speed);
}

static void build_beziers(saver_setup *s) {
    xd_window *win = make_window(s, "Beziers Setup", 300, 180);
    xd_groupbox(win, (xd_rect){12, 8, 276, 56}, "Length");
    xd_label(win, (xd_rect){24, 32, 44, 16}, "Curves:", 0);
    count_combo_make(&s->count1, win, (xd_rect){70, 29, 56, 21}, 1, XD_BEZIERS_CURVES_MAX, 1,
                     s->edit.beziers.curves);
    xd_label(win, (xd_rect){148, 32, 40, 16}, "Lines:", 0);
    count_combo_make(&s->count2, win, (xd_rect){190, 29, 56, 21}, 1, XD_BEZIERS_LINES_MAX, 1,
                     s->edit.beziers.lines);
    s->speed = slider_group(win, (xd_rect){12, 72, 276, 72}, "Speed", "Slow", "Fast",
                            s->edit.beziers.speed);
    xd_window_set_focus(win, s->count1.w);
}

/* ---- Mystify ------------------------------------------------------------------- */

/* The controls under Shape show one polygon's settings at a time. */
static void mystify_store(saver_setup *s) {
    xd_mystify_shape *sh = &s->edit.mystify.shapes[s->shape_shown];
    sh->active = xd_checkbox_checked(s->active);
    sh->lines = count_combo_value(&s->count1);
    sh->two_colors = xd_checkbox_checked(s->two_colors);
    sh->colors[0] = color_combo_value(&s->shape_colors[0]);
    sh->colors[1] = color_combo_value(&s->shape_colors[1]);
}

static void select_color(color_combo *c, xd_rgb v) {
    c->custom = v;
    int sel = -1;
    for (int i = 0; i < NAMED_COLORS; i++) {
        xd_rgb n = named_colors[i].rgb;
        if (n.r == v.r && n.g == v.g && n.b == v.b)
            sel = i;
    }
    if (sel < 0 && xd_widget_item_count(c->w) == (size_t)NAMED_COLORS)
        xd_widget_add_item(c->w, "Custom");
    xd_widget_select(c->w, sel >= 0 ? sel : NAMED_COLORS);
}

static void mystify_enable(saver_setup *s) {
    bool on = xd_checkbox_checked(s->active);
    bool two = xd_checkbox_checked(s->two_colors);
    xd_widget_set_enabled(s->count1.w, on);
    xd_widget_set_enabled(s->two_colors, on);
    xd_widget_set_enabled(s->random_colors, on);
    xd_widget_set_enabled(s->shape_colors[0].w, on && two);
    xd_widget_set_enabled(s->shape_colors[1].w, on && two);
}

static void mystify_show(saver_setup *s) {
    const xd_mystify_shape *sh = &s->edit.mystify.shapes[s->shape_shown];
    xd_checkbox_set(s->active, sh->active);
    xd_widget_select(s->count1.w, sh->lines - s->count1.min);
    xd_radio_set(sh->two_colors ? s->two_colors : s->random_colors);
    select_color(&s->shape_colors[0], sh->colors[0]);
    select_color(&s->shape_colors[1], sh->colors[1]);
    mystify_enable(s);
}

static void on_shape(xd_widget *w, void *user) {
    saver_setup *s = user;
    int i = xd_widget_selected(w);
    if (i < 0 || i >= XD_MYSTIFY_SHAPES || i == s->shape_shown)
        return;
    mystify_store(s);
    s->shape_shown = i;
    mystify_show(s);
}

static void on_mystify_toggle([[maybe_unused]] xd_widget *w, void *user) { mystify_enable(user); }

static void build_mystify(saver_setup *s) {
    xd_window *win = make_window(s, "Mystify Setup", 320, 232);
    xd_groupbox(win, (xd_rect){12, 8, 296, 150}, "Shape");
    s->shape = xd_combo(win, (xd_rect){24, 28, 110, 21}, false, on_shape, s);
    xd_widget_add_item(s->shape, "Polygon 1");
    xd_widget_add_item(s->shape, "Polygon 2");
    xd_widget_select(s->shape, 0);
    s->active = xd_checkbox(win, (xd_rect){150, 29, 80, 18}, "Active", false, on_mystify_toggle, s);
    xd_label(win, (xd_rect){24, 62, 40, 16}, "Lines:", 0);
    count_combo_make(&s->count1, win, (xd_rect){66, 59, 56, 21}, 1, XD_MYSTIFY_LINES_MAX, 1, 1);
    xd_groupbox(win, (xd_rect){24, 88, 272, 62}, "Colors");
    s->two_colors =
        xd_radio(win, (xd_rect){34, 106, 90, 18}, "Two colors", true, on_mystify_toggle, s);
    s->random_colors = xd_radio(win, (xd_rect){34, 126, 170, 18}, "Multiple random colors",
                                false, on_mystify_toggle, s);
    color_combo_make(&s->shape_colors[0], win, (xd_rect){128, 104, 78, 21}, (xd_rgb){0, 0, 0},
                     nullptr, nullptr);
    color_combo_make(&s->shape_colors[1], win, (xd_rect){210, 104, 78, 21}, (xd_rgb){0, 0, 0},
                     nullptr, nullptr);
    s->clear = xd_checkbox(win, (xd_rect){12, 168, 120, 18}, "Clear screen",
                           s->edit.mystify.clear_screen, nullptr, nullptr);
    s->shape_shown = 0;
    mystify_show(s);
    xd_window_set_focus(win, s->shape);
}

/* ---- 3D Pipes ------------------------------------------------------------------ */

static void build_pipes(saver_setup *s) {
    xd_window *win = make_window(s, "3D Pipes Setup", 320, 188);
    xd_groupbox(win, (xd_rect){12, 8, 140, 64}, "Pipes");
    xd_widget *single = xd_radio(win, (xd_rect){24, 26, 110, 18}, "Single", true, nullptr, nullptr);
    s->multiple = xd_radio(win, (xd_rect){24, 46, 110, 18}, "Multiple", false, nullptr, nullptr);
    xd_radio_set(s->edit.pipes.multiple ? s->multiple : single);
    xd_groupbox(win, (xd_rect){164, 8, 144, 108}, "Joint Type");
    static const char *const joints[XD_PIPES_JOINT_COUNT] = {
        [XD_PIPES_ELBOW] = "Elbow", [XD_PIPES_BALL] = "Ball", [XD_PIPES_MIXED] = "Mixed",
        [XD_PIPES_CYCLE] = "Cycle"};
    for (int i = 0; i < XD_PIPES_JOINT_COUNT; i++)
        s->joints[i] = xd_radio(win, (xd_rect){176, 26 + 20 * i, 110, 18}, joints[i], i == 0,
                                nullptr, nullptr);
    xd_radio_set(s->joints[s->edit.pipes.joints]);
    /* Windows' textures (and its Choose Texture...) aren't drawn here. */
    xd_groupbox(win, (xd_rect){12, 80, 140, 64}, "Surface Style");
    xd_radio_set(xd_radio(win, (xd_rect){24, 98, 110, 18}, "Solid", true, nullptr, nullptr));
    xd_widget_set_enabled(
        xd_radio(win, (xd_rect){24, 118, 110, 18}, "Textured", false, nullptr, nullptr), false);
    xd_window_set_focus(win, s->edit.pipes.multiple ? s->multiple : single);
}

/* ---- 3D Text ------------------------------------------------------------------- */

static void text3d_enable(saver_setup *s) {
    xd_widget_set_enabled(s->text3d, !xd_checkbox_checked(s->show_time));
    xd_widget_set_enabled(s->text_color.w, !xd_checkbox_checked(s->reflection));
}

static void on_text3d_toggle([[maybe_unused]] xd_widget *w, void *user) { text3d_enable(user); }

static void build_text3d(saver_setup *s) {
    const xd_text3d_settings *t = &s->edit.text3d;
    xd_window *win = make_window(s, "3D Text Setup", 380, 296);
    xd_groupbox(win, (xd_rect){12, 8, 356, 64}, "Display");
    xd_widget *text =
        xd_radio(win, (xd_rect){24, 26, 60, 18}, "Text", true, on_text3d_toggle, s);
    s->text3d = xd_edit(win, (xd_rect){88, 24, 268, 21}, t->text);
    s->show_time = xd_radio(win, (xd_rect){24, 48, 60, 18}, "Time", false, on_text3d_toggle, s);
    xd_radio_set(t->show_time ? s->show_time : text);

    xd_groupbox(win, (xd_rect){12, 80, 356, 100}, "Motion");
    xd_label(win, (xd_rect){24, 102, 84, 16}, "Rotation Type:", 0);
    s->rotation = xd_combo(win, (xd_rect){110, 99, 110, 21}, false, nullptr, nullptr);
    static const char *const rotations[XD_TEXT3D_ROTATION_COUNT] = {
        [XD_TEXT3D_NONE] = "None", [XD_TEXT3D_SPIN] = "Spin", [XD_TEXT3D_SEESAW] = "See-Saw",
        [XD_TEXT3D_WOBBLE] = "Wobble", [XD_TEXT3D_RANDOM] = "Random"};
    for (int i = 0; i < XD_TEXT3D_ROTATION_COUNT; i++)
        xd_widget_add_item(s->rotation, rotations[i]);
    xd_widget_select(s->rotation, t->rotation);
    xd_label(win, (xd_rect){24, 134, 40, 16}, "Speed", 0);
    s->speed = xd_slider(win, (xd_rect){66, 128, 290, 26}, 0, XD_SAVER_SLIDER_MAX, t->speed,
                         nullptr, nullptr);
    xd_label(win, (xd_rect){70, 156, 40, 14}, "Slow", 0);
    xd_label(win, (xd_rect){324, 156, 32, 14}, "Fast", 0);

    s->size = slider_group(win, (xd_rect){12, 188, 170, 66}, "Size", "Small", "Large", t->size);
    xd_groupbox(win, (xd_rect){192, 188, 176, 66}, "Surface Style");
    xd_widget *solid =
        xd_radio(win, (xd_rect){202, 208, 84, 18}, "Solid Color", true, on_text3d_toggle, s);
    s->reflection =
        xd_radio(win, (xd_rect){202, 230, 84, 18}, "Reflection", false, on_text3d_toggle, s);
    xd_radio_set(t->surface == XD_TEXT3D_REFLECTION ? s->reflection : solid);
    color_combo_make(&s->text_color, win, (xd_rect){288, 206, 72, 21}, t->color, nullptr,
                     nullptr);
    text3d_enable(s);
    xd_window_set_focus(win, t->show_time ? s->show_time : s->text3d);
}

/* ---- OK ------------------------------------------------------------------------- */

static void read_back(saver_setup *s) {
    xd_saver_settings *e = &s->edit;
    if (strcmp(s->id, "marquee") == 0) {
        e->marquee.random_position = xd_checkbox_checked(s->random_pos);
        e->marquee.speed = xd_slider_value(s->speed);
        e->marquee.background = color_combo_value(&s->background);
        snprintf(e->marquee.text, sizeof(e->marquee.text), "%s", xd_widget_text(s->text));
    } else if (strcmp(s->id, "starfield") == 0) {
        e->starfield.speed = xd_slider_value(s->speed);
        e->starfield.density = count_combo_value(&s->count1);
    } else if (strcmp(s->id, "flying") == 0) {
        e->flying.speed = xd_slider_value(s->speed);
        e->flying.density = count_combo_value(&s->count1);
    } else if (strcmp(s->id, "beziers") == 0) {
        e->beziers.curves = count_combo_value(&s->count1);
        e->beziers.lines = count_combo_value(&s->count2);
        e->beziers.speed = xd_slider_value(s->speed);
    } else if (strcmp(s->id, "mystify") == 0) {
        mystify_store(s);
        e->mystify.clear_screen = xd_checkbox_checked(s->clear);
    } else if (strcmp(s->id, "pipes") == 0) {
        e->pipes.multiple = xd_checkbox_checked(s->multiple);
        for (int i = 0; i < XD_PIPES_JOINT_COUNT; i++) {
            if (xd_checkbox_checked(s->joints[i]))
                e->pipes.joints = (xd_pipes_joint)i;
        }
    } else if (strcmp(s->id, "text3d") == 0) {
        xd_text3d_settings *t = &e->text3d;
        t->show_time = xd_checkbox_checked(s->show_time);
        snprintf(t->text, sizeof(t->text), "%s", xd_widget_text(s->text3d));
        int r = xd_widget_selected(s->rotation);
        if (r >= 0 && r < XD_TEXT3D_ROTATION_COUNT)
            t->rotation = (xd_text3d_rotation)r;
        t->speed = xd_slider_value(s->speed);
        t->size = xd_slider_value(s->size);
        t->surface = xd_checkbox_checked(s->reflection) ? XD_TEXT3D_REFLECTION : XD_TEXT3D_SOLID;
        t->color = color_combo_value(&s->text_color);
    }
}

saver_setup *saver_setup_open(xd_ui *ui, const char *id, const xd_saver_settings *current,
                              saver_setup_done done, void *user) {
    static const struct {
        const char *id;
        void (*build)(saver_setup *s);
    } dialogs[] = {
        {"marquee", build_marquee}, {"starfield", build_starfield}, {"flying", build_flying},
        {"beziers", build_beziers}, {"mystify", build_mystify},     {"pipes", build_pipes},
        {"text3d", build_text3d},
    };
    for (size_t i = 0; id != nullptr && i < XD_ARRAY_LEN(dialogs); i++) {
        if (strcmp(dialogs[i].id, id) != 0)
            continue;
        saver_setup *s = XD_NEW(saver_setup);
        s->ui = ui;
        snprintf(s->id, sizeof(s->id), "%s", id);
        s->edit = *current;
        s->done = done;
        s->user = user;
        dialogs[i].build(s);
        xd_window_show(s->win);
        return s;
    }
    return nullptr;
}
