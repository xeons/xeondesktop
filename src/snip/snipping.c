/* SPDX-License-Identifier: MIT */
#include "snipping.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define OPTIONS_GROUP "Snipping Tool"

/* Logical px: the selection ink, and the smallest selection kept. */
static constexpr double SELECTION_INK_WIDTH = 2.0;
static constexpr double SELECTION_MIN = 2.0;

static const char *const mode_names[] = {
    [XD_SNIP_FREE_FORM] = "free-form",
    [XD_SNIP_RECTANGLE] = "rectangle",
    [XD_SNIP_WINDOW] = "window",
    [XD_SNIP_FULL_SCREEN] = "full-screen",
};

const char *xd_snip_mode_name(xd_snip_mode mode) {
    return (size_t)mode < G_N_ELEMENTS(mode_names) ? mode_names[mode] : mode_names[0];
}

bool xd_snip_mode_parse(const char *name, xd_snip_mode *mode) {
    if (name == nullptr)
        return false;
    for (size_t i = 0; i < G_N_ELEMENTS(mode_names); i++) {
        if (strcmp(name, mode_names[i]) == 0) {
            *mode = (xd_snip_mode)i;
            return true;
        }
    }
    return false;
}

/* ---- selections ---------------------------------------------------------------------- */

xd_snip_box xd_snip_box_from_corners(xd_snip_point first, xd_snip_point second) {
    return (xd_snip_box){
        .x = fmin(first.x, second.x),
        .y = fmin(first.y, second.y),
        .width = fabs(second.x - first.x),
        .height = fabs(second.y - first.y),
    };
}

xd_snip_box xd_snip_box_around(const xd_snip_point *points, size_t count) {
    if (count == 0)
        return (xd_snip_box){0};
    double left = points[0].x, right = points[0].x, top = points[0].y, bottom = points[0].y;
    for (size_t i = 1; i < count; i++) {
        left = fmin(left, points[i].x);
        right = fmax(right, points[i].x);
        top = fmin(top, points[i].y);
        bottom = fmax(bottom, points[i].y);
    }
    return (xd_snip_box){left, top, right - left, bottom - top};
}

bool xd_snip_box_is_selection(xd_snip_box box) {
    return box.width >= SELECTION_MIN && box.height >= SELECTION_MIN;
}

int xd_snip_window_at(const xd_snip_window *windows, size_t count, double x, double y) {
    for (size_t i = 0; i < count; i++) {
        const xd_snip_window *window = &windows[i];
        if (x >= window->x && x < window->x + window->width && y >= window->y &&
            y < window->y + window->height)
            return (int)i;
    }
    return -1;
}

int xd_snip_window_active(const xd_snip_window *windows, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (windows[i].activated)
            return (int)i;
    }
    return -1;
}

void xd_snip_screen_free(xd_snip_screen *screen) {
    if (screen == nullptr)
        return;
    if (screen->image != nullptr)
        cairo_surface_destroy(screen->image);
    g_free(screen->windows);
    g_free(screen);
}

bool xd_snip_screen_pixels(const xd_snip_screen *screen, xd_snip_box box,
                           cairo_rectangle_int_t *pixels) {
    if (screen->image == nullptr || screen->scale <= 0)
        return false;
    int image_width = cairo_image_surface_get_width(screen->image);
    int image_height = cairo_image_surface_get_height(screen->image);
    double left = floor((box.x - screen->origin_x) * screen->scale);
    double top = floor((box.y - screen->origin_y) * screen->scale);
    double right = ceil((box.x + box.width - screen->origin_x) * screen->scale);
    double bottom = ceil((box.y + box.height - screen->origin_y) * screen->scale);
    left = fmax(left, 0);
    top = fmax(top, 0);
    right = fmin(right, image_width);
    bottom = fmin(bottom, image_height);
    if (right <= left || bottom <= top)
        return false;
    int x = (int)left, y = (int)top;
    *pixels = (cairo_rectangle_int_t){x, y, (int)right - x, (int)bottom - y};
    return true;
}

static void trace_path(cairo_t *cairo, const xd_snip_point *path, size_t count) {
    for (size_t i = 0; i < count; i++)
        cairo_line_to(cairo, path[i].x, path[i].y);
    cairo_close_path(cairo);
}

cairo_surface_t *xd_snip_screen_cut(const xd_snip_screen *screen, xd_snip_box box,
                                    const xd_snip_point *path, size_t path_count,
                                    const double *ink) {
    cairo_rectangle_int_t pixels;
    if (!xd_snip_screen_pixels(screen, box, &pixels))
        return nullptr;
    cairo_surface_t *snip =
        cairo_image_surface_create(CAIRO_FORMAT_RGB24, pixels.width, pixels.height);
    cairo_t *cairo = cairo_create(snip);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_paint(cairo);
    /* From here on, desktop coordinates. */
    cairo_translate(cairo, -pixels.x, -pixels.y);
    cairo_scale(cairo, screen->scale, screen->scale);
    cairo_translate(cairo, -screen->origin_x, -screen->origin_y);
    bool free_form = path != nullptr && path_count >= 3;
    if (free_form) {
        trace_path(cairo, path, path_count);
        cairo_clip(cairo);
    }
    cairo_save(cairo);
    cairo_translate(cairo, screen->origin_x, screen->origin_y);
    cairo_scale(cairo, 1 / screen->scale, 1 / screen->scale);
    cairo_set_source_surface(cairo, screen->image, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_NEAREST);
    cairo_paint(cairo);
    cairo_restore(cairo);
    cairo_reset_clip(cairo);
    if (ink != nullptr) {
        cairo_set_source_rgb(cairo, ink[0], ink[1], ink[2]);
        cairo_set_line_width(cairo, SELECTION_INK_WIDTH);
        cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
        if (free_form) {
            trace_path(cairo, path, path_count);
        } else {
            /* Inside the box, so all of it shows. */
            double half = SELECTION_INK_WIDTH / 2;
            cairo_rectangle(cairo, box.x + half, box.y + half, box.width - SELECTION_INK_WIDTH,
                            box.height - SELECTION_INK_WIDTH);
        }
        cairo_stroke(cairo);
    }
    cairo_destroy(cairo);
    cairo_surface_flush(snip);
    return snip;
}

/* ---- ink ------------------------------------------------------------------------------ */

static void free_stroke(gpointer data) {
    xd_ink_stroke *stroke = data;
    g_array_unref(stroke->points);
    g_free(stroke);
}

void xd_ink_init(xd_ink *ink) {
    ink->strokes = g_ptr_array_new_with_free_func(free_stroke);
}

void xd_ink_finish(xd_ink *ink) {
    if (ink->strokes != nullptr)
        g_ptr_array_unref(ink->strokes);
    ink->strokes = nullptr;
}

void xd_ink_clear(xd_ink *ink) {
    g_ptr_array_set_size(ink->strokes, 0);
}

xd_ink_stroke *xd_ink_begin(xd_ink *ink, xd_ink_tool tool, const double color[3], double width,
                            xd_snip_point start) {
    xd_ink_stroke *stroke = g_new0(xd_ink_stroke, 1);
    stroke->tool = tool;
    stroke->red = color[0];
    stroke->green = color[1];
    stroke->blue = color[2];
    stroke->width = width;
    stroke->points = g_array_new(FALSE, FALSE, sizeof(xd_snip_point));
    g_array_append_val(stroke->points, start);
    g_ptr_array_add(ink->strokes, stroke);
    return stroke;
}

void xd_ink_extend(xd_ink_stroke *stroke, xd_snip_point point) {
    g_array_append_val(stroke->points, point);
}

/* The distance from a point to the segment from `start` to `end`. */
static double segment_distance(xd_snip_point point, xd_snip_point start, xd_snip_point end) {
    double dx = end.x - start.x, dy = end.y - start.y;
    double length_squared = dx * dx + dy * dy;
    double along = 0;
    if (length_squared > 0)
        along = fmin(1, fmax(0, ((point.x - start.x) * dx + (point.y - start.y) * dy) /
                                    length_squared));
    double nearest_x = start.x + along * dx, nearest_y = start.y + along * dy;
    return hypot(point.x - nearest_x, point.y - nearest_y);
}

static bool stroke_near(const xd_ink_stroke *stroke, xd_snip_point point, double radius) {
    const xd_snip_point *points = (const xd_snip_point *)(void *)stroke->points->data;
    size_t count = stroke->points->len;
    double reach = radius + stroke->width / 2;
    if (count == 1)
        return hypot(point.x - points[0].x, point.y - points[0].y) <= reach;
    for (size_t i = 1; i < count; i++) {
        if (segment_distance(point, points[i - 1], points[i]) <= reach)
            return true;
    }
    return false;
}

bool xd_ink_erase_at(xd_ink *ink, xd_snip_point point, double radius) {
    bool erased = false;
    for (guint i = ink->strokes->len; i-- > 0;) {
        if (stroke_near(g_ptr_array_index(ink->strokes, i), point, radius)) {
            g_ptr_array_remove_index(ink->strokes, i);
            erased = true;
        }
    }
    return erased;
}

void xd_ink_draw(const xd_ink *ink, cairo_t *cairo) {
    cairo_save(cairo);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    for (guint i = 0; i < ink->strokes->len; i++) {
        const xd_ink_stroke *stroke = g_ptr_array_index(ink->strokes, i);
        const xd_snip_point *points = (const xd_snip_point *)(void *)stroke->points->data;
        bool highlighter = stroke->tool == XD_INK_HIGHLIGHTER;
        /* The highlighter's tip is a flat chisel; the pen's is round. */
        cairo_set_line_cap(cairo, highlighter ? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_ROUND);
        cairo_set_operator(cairo, highlighter ? CAIRO_OPERATOR_MULTIPLY : CAIRO_OPERATOR_OVER);
        cairo_set_source_rgb(cairo, stroke->red, stroke->green, stroke->blue);
        cairo_set_line_width(cairo, stroke->width);
        cairo_new_path(cairo);
        cairo_move_to(cairo, points[0].x, points[0].y);
        /* A dot, for a click without moving. */
        if (stroke->points->len == 1)
            cairo_line_to(cairo, points[0].x + 0.01, points[0].y);
        for (guint point = 1; point < stroke->points->len; point++)
            cairo_line_to(cairo, points[point].x, points[point].y);
        cairo_stroke(cairo);
    }
    cairo_restore(cairo);
}

/* ---- options ---------------------------------------------------------------------------- */

void xd_snip_options_init(xd_snip_options *options) {
    *options = (xd_snip_options){
        .mode = XD_SNIP_RECTANGLE,
        .delay_seconds = 0,
        .hide_instructions = false,
        .always_copy = true,
        .prompt_to_save = true,
        .show_overlay = true,
        .show_selection_ink = true,
        .ink = {1, 0, 0},
    };
}

static void load_bool(GKeyFile *file, const char *key, bool *value) {
    GError *error = nullptr;
    gboolean loaded = g_key_file_get_boolean(file, OPTIONS_GROUP, key, &error);
    if (error == nullptr)
        *value = loaded;
    g_clear_error(&error);
}

void xd_snip_options_load_data(xd_snip_options *options, GKeyFile *file) {
    g_autofree char *mode = g_key_file_get_string(file, OPTIONS_GROUP, "Mode", nullptr);
    xd_snip_mode_parse(mode, &options->mode);
    GError *error = nullptr;
    int delay = g_key_file_get_integer(file, OPTIONS_GROUP, "Delay", &error);
    if (error == nullptr)
        options->delay_seconds = CLAMP(delay, 0, 5);
    g_clear_error(&error);
    load_bool(file, "HideInstructions", &options->hide_instructions);
    load_bool(file, "AlwaysCopy", &options->always_copy);
    load_bool(file, "PromptToSave", &options->prompt_to_save);
    load_bool(file, "ShowOverlay", &options->show_overlay);
    load_bool(file, "ShowSelectionInk", &options->show_selection_ink);
    g_autofree char *ink = g_key_file_get_string(file, OPTIONS_GROUP, "InkColor", nullptr);
    unsigned red, green, blue;
    if (ink != nullptr && strlen(ink) == 7 && sscanf(ink, "#%2x%2x%2x", &red, &green, &blue) == 3) {
        options->ink[0] = red / 255.0;
        options->ink[1] = green / 255.0;
        options->ink[2] = blue / 255.0;
    }
}

static uint8_t channel_byte(double value) {
    double scaled = round(fmin(1, fmax(0, value)) * 255);
    return (uint8_t)scaled;
}

void xd_snip_options_save_data(const xd_snip_options *options, GKeyFile *file) {
    g_key_file_set_string(file, OPTIONS_GROUP, "Mode", xd_snip_mode_name(options->mode));
    g_key_file_set_integer(file, OPTIONS_GROUP, "Delay", options->delay_seconds);
    g_key_file_set_boolean(file, OPTIONS_GROUP, "HideInstructions", options->hide_instructions);
    g_key_file_set_boolean(file, OPTIONS_GROUP, "AlwaysCopy", options->always_copy);
    g_key_file_set_boolean(file, OPTIONS_GROUP, "PromptToSave", options->prompt_to_save);
    g_key_file_set_boolean(file, OPTIONS_GROUP, "ShowOverlay", options->show_overlay);
    g_key_file_set_boolean(file, OPTIONS_GROUP, "ShowSelectionInk", options->show_selection_ink);
    char ink[8];
    snprintf(ink, sizeof(ink), "#%02x%02x%02x", channel_byte(options->ink[0]),
             channel_byte(options->ink[1]), channel_byte(options->ink[2]));
    g_key_file_set_string(file, OPTIONS_GROUP, "InkColor", ink);
}
