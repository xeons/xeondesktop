/* SPDX-License-Identifier: MIT
 * xd-mines: Minefield, Windows XP's mine-clearing game, drawn as XP drew
 * it whatever the theme: a grey board of raised squares under a panel
 * with a red LED mine counter, the face button (a new game) and the
 * timer. Left click opens a square, right click flags it (then marks it
 * "?", if Marks is on), and both buttons (or the middle one) on a number
 * whose flags are all placed open its neighbours. Game: Beginner,
 * Intermediate, Expert or Custom, and the best times, kept with the
 * settings in ~/.config/xeondesktop/mines.ini.
 *
 *   xd-mines [--seed N]   N lays the same mines each time (for tests)
 *
 * Everything is drawn here with cairo: no bitmaps.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../gtkapp/gtkapp.h"
#include "minefield.h"

#define TITLE "Minefield"
#define APP_ID "org.xeondesktop.Minefield"
#define SETTINGS_GROUP "Minefield"
#define TIMES_GROUP "Best Times"
#define ANONYMOUS "Anonymous"

/* Logical px, as XP's: the squares, the white edge at the top and left,
 * the margins, the panel and the board's sunken edges. */
static constexpr int CELL = 16;
static constexpr int OUTER = 3;
static constexpr int MARGIN = 6;
static constexpr int PANEL_HEIGHT = 37;
static constexpr int PANEL_EDGE = 2;
static constexpr int BOARD_EDGE = 3;
static constexpr int DIGIT_WIDTH = 13;
static constexpr int DIGIT_HEIGHT = 23;
static constexpr int FACE = 26;
static constexpr int TIME_MAX = 999;

/* The classic palette: the game ignores the colour scheme, as XP's did. */
static const double GREY[3] = {0.75, 0.75, 0.75};
static const double SHADOW[3] = {0.5, 0.5, 0.5};
static const double WHITE[3] = {1, 1, 1};
static const double BLACK[3] = {0, 0, 0};
static const double RED[3] = {1, 0, 0};
static const double DIM_RED[3] = {0.28, 0, 0};
static const double YELLOW[3] = {1, 1, 0};
static const double number_colors[9][3] = {
    {0, 0, 0},     {0, 0, 1},   {0, 0.5, 0},   {1, 0, 0},    {0, 0, 0.5},
    {0.5, 0, 0},   {0, 0.5, 0.5}, {0, 0, 0},   {0.5, 0.5, 0.5},
};

typedef enum face_look : uint8_t { FACE_SMILE, FACE_WORRIED, FACE_DEAD, FACE_COOL } face_look;

typedef struct best_time {
    int seconds;
    char name[64];
} best_time;

typedef struct mines_app {
    GtkWidget *window;
    GtkWidget *board;
    GtkWidget *level_items[4];
    GtkWidget *marks_item;
    xd_minefield field;
    xd_mines_level level;
    int custom_width, custom_height, custom_mines;
    bool question_marks;
    best_time best[3]; /* Beginner, Intermediate, Expert */
    GRand *rand;
    int seconds;
    guint timer;
    /* The mouse */
    bool left_down, right_down, middle_down;
    bool chording;
    bool swallow_release; /* the other button of a chord */
    bool face_pressed;
    bool face_hover;
    int pressed_x, pressed_y; /* the square under a held button, or -1 */
    bool syncing;
} mines_app;

/* ---- settings ------------------------------------------------------------------------------- */

static char *settings_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "mines.ini", nullptr);
}

static void load_settings(mines_app *app) {
    app->level = XD_MINES_BEGINNER;
    app->question_marks = true;
    app->custom_width = 9;
    app->custom_height = 9;
    app->custom_mines = 10;
    for (int i = 0; i < 3; i++)
        app->best[i] = (best_time){.seconds = TIME_MAX, .name = ANONYMOUS};
    g_autofree char *path = settings_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, nullptr))
        return;
    g_autofree char *level = g_key_file_get_string(file, SETTINGS_GROUP, "Level", nullptr);
    xd_mines_level_parse(level, &app->level);
    GError *error = nullptr;
    gboolean marks = g_key_file_get_boolean(file, SETTINGS_GROUP, "Marks", &error);
    if (error == nullptr)
        app->question_marks = marks;
    g_clear_error(&error);
    int width = g_key_file_get_integer(file, SETTINGS_GROUP, "Width", nullptr);
    int height = g_key_file_get_integer(file, SETTINGS_GROUP, "Height", nullptr);
    int mines = g_key_file_get_integer(file, SETTINGS_GROUP, "Mines", nullptr);
    if (width > 0 && height > 0 && mines > 0) {
        xd_mines_clamp_custom(&width, &height, &mines);
        app->custom_width = width;
        app->custom_height = height;
        app->custom_mines = mines;
    }
    for (int i = 0; i < 3; i++) {
        const char *level_name = xd_mines_level_name((xd_mines_level)i);
        int seconds = g_key_file_get_integer(file, TIMES_GROUP, level_name, &error);
        if (error == nullptr)
            app->best[i].seconds = CLAMP(seconds, 1, TIME_MAX);
        g_clear_error(&error);
        g_autofree char *key = g_strconcat(level_name, "-name", nullptr);
        g_autofree char *name = g_key_file_get_string(file, TIMES_GROUP, key, nullptr);
        if (name != nullptr)
            g_strlcpy(app->best[i].name, name, sizeof(app->best[i].name));
    }
}

static void save_settings(const mines_app *app) {
    g_autofree char *path = settings_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_load_from_file(file, path, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_key_file_set_string(file, SETTINGS_GROUP, "Level", xd_mines_level_name(app->level));
    g_key_file_set_boolean(file, SETTINGS_GROUP, "Marks", app->question_marks);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Width", app->custom_width);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Height", app->custom_height);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Mines", app->custom_mines);
    for (int i = 0; i < 3; i++) {
        const char *level_name = xd_mines_level_name((xd_mines_level)i);
        g_key_file_set_integer(file, TIMES_GROUP, level_name, app->best[i].seconds);
        g_autofree char *key = g_strconcat(level_name, "-name", nullptr);
        g_key_file_set_string(file, TIMES_GROUP, key, app->best[i].name);
    }
    g_autofree char *folder = g_path_get_dirname(path);
    g_mkdir_with_parents(folder, 0700);
    GError *error = nullptr;
    if (!g_key_file_save_to_file(file, path, &error)) {
        g_warning("cannot save %s: %s", path, error->message);
        g_error_free(error);
    }
}

/* ---- geometry -------------------------------------------------------------------------------- */

static int board_left(void) {
    return OUTER + MARGIN;
}

static int board_top(void) {
    return OUTER + MARGIN + PANEL_HEIGHT + MARGIN;
}

static void content_size(const mines_app *app, int *width, int *height) {
    *width = OUTER + MARGIN + CELL * app->field.width + 2 * BOARD_EDGE + MARGIN;
    *height = board_top() + CELL * app->field.height + 2 * BOARD_EDGE + MARGIN;
}

static GdkRectangle face_box(const mines_app *app) {
    int panel_width = CELL * app->field.width + 2 * BOARD_EDGE;
    int x = board_left() + (panel_width - FACE) / 2;
    int y = OUTER + MARGIN + (PANEL_HEIGHT - FACE) / 2;
    return (GdkRectangle){x, y, FACE, FACE};
}

static bool square_at(const mines_app *app, double x, double y, int *square_x, int *square_y) {
    double left = board_left() + BOARD_EDGE, top = board_top() + BOARD_EDGE;
    if (x < left || y < top)
        return false;
    double column = floor((x - left) / CELL), row = floor((y - top) / CELL);
    *square_x = (int)column;
    *square_y = (int)row;
    return xd_minefield_contains(&app->field, *square_x, *square_y);
}

/* ---- drawing ---------------------------------------------------------------------------------- */

static void set_color(cairo_t *cairo, const double color[3]) {
    cairo_set_source_rgb(cairo, color[0], color[1], color[2]);
}

static void fill(cairo_t *cairo, const double color[3], double x, double y, double width,
                 double height) {
    set_color(cairo, color);
    cairo_rectangle(cairo, x, y, width, height);
    cairo_fill(cairo);
}

/* A raised (light top left) or sunken (dark top left) edge `thickness`
 * wide inside the box, mitred at the corners. */
static void draw_edge(cairo_t *cairo, double x, double y, double width, double height,
                      double thickness, bool raised) {
    const double *top_left = raised ? WHITE : SHADOW;
    const double *bottom_right = raised ? SHADOW : WHITE;
    set_color(cairo, top_left);
    cairo_move_to(cairo, x, y);
    cairo_line_to(cairo, x + width, y);
    cairo_line_to(cairo, x + width - thickness, y + thickness);
    cairo_line_to(cairo, x + thickness, y + thickness);
    cairo_line_to(cairo, x + thickness, y + height - thickness);
    cairo_line_to(cairo, x, y + height);
    cairo_close_path(cairo);
    cairo_fill(cairo);
    set_color(cairo, bottom_right);
    cairo_move_to(cairo, x + width, y + height);
    cairo_line_to(cairo, x, y + height);
    cairo_line_to(cairo, x + thickness, y + height - thickness);
    cairo_line_to(cairo, x + width - thickness, y + height - thickness);
    cairo_line_to(cairo, x + width - thickness, y + thickness);
    cairo_line_to(cairo, x + width, y);
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

/* Seven segments, a to g, as (x, y, horizontal) in a 13x23 digit. */
static void draw_segment(cairo_t *cairo, double x, double y, bool horizontal) {
    static constexpr double LENGTH = 7;
    static constexpr double THICK = 2;
    if (horizontal) {
        cairo_move_to(cairo, x, y + THICK / 2);
        cairo_line_to(cairo, x + THICK / 2, y);
        cairo_line_to(cairo, x + LENGTH - THICK / 2, y);
        cairo_line_to(cairo, x + LENGTH, y + THICK / 2);
        cairo_line_to(cairo, x + LENGTH - THICK / 2, y + THICK);
        cairo_line_to(cairo, x + THICK / 2, y + THICK);
    } else {
        cairo_move_to(cairo, x + THICK / 2, y);
        cairo_line_to(cairo, x + THICK, y + THICK / 2);
        cairo_line_to(cairo, x + THICK, y + LENGTH + 1 - THICK / 2);
        cairo_line_to(cairo, x + THICK / 2, y + LENGTH + 1);
        cairo_line_to(cairo, x, y + LENGTH + 1 - THICK / 2);
        cairo_line_to(cairo, x, y + THICK / 2);
    }
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

/* `digit` 0 to 9, or 10 for a minus sign. */
static void draw_digit(cairo_t *cairo, double x, double y, int digit) {
    /* Which of a..g are lit, per digit. */
    static const uint8_t lit[11] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d,
                                    0x7d, 0x07, 0x7f, 0x6f, 0x40};
    static const struct {
        double x, y;
        bool horizontal;
    } segments[7] = {
        {3, 1, true},    /* a: top */
        {10, 2.5, false}, /* b: top right */
        {10, 12.5, false}, /* c: bottom right */
        {3, 20, true},   /* d: bottom */
        {1, 12.5, false}, /* e: bottom left */
        {1, 2.5, false},  /* f: top left */
        {3, 10.5, true}, /* g: middle */
    };
    fill(cairo, BLACK, x, y, DIGIT_WIDTH, DIGIT_HEIGHT);
    for (int segment = 0; segment < 7; segment++) {
        set_color(cairo, (lit[digit] >> segment) & 1 ? RED : DIM_RED);
        draw_segment(cairo, x + segments[segment].x, y + segments[segment].y,
                     segments[segment].horizontal);
    }
}

/* Three digits; below zero, a minus first (to -99). */
static void draw_counter(cairo_t *cairo, double x, double y, int value) {
    value = CLAMP(value, -99, TIME_MAX);
    draw_edge(cairo, x - 1, y - 1, 3 * DIGIT_WIDTH + 2, DIGIT_HEIGHT + 2, 1, false);
    int digits[3];
    int magnitude = abs(value);
    digits[0] = value < 0 ? 10 : magnitude / 100;
    digits[1] = (magnitude / 10) % 10;
    digits[2] = magnitude % 10;
    for (int i = 0; i < 3; i++)
        draw_digit(cairo, x + i * DIGIT_WIDTH, y, digits[i]);
}

static void draw_face(cairo_t *cairo, GdkRectangle box, face_look look, bool pressed) {
    fill(cairo, GREY, box.x, box.y, box.width, box.height);
    set_color(cairo, SHADOW);
    cairo_rectangle(cairo, box.x + 0.5, box.y + 0.5, box.width - 1, box.height - 1);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
    if (pressed)
        draw_edge(cairo, box.x + 1, box.y + 1, box.width - 2, box.height - 2, 1, false);
    else
        draw_edge(cairo, box.x + 1, box.y + 1, box.width - 2, box.height - 2, 2, true);
    double shift = pressed ? 1 : 0;
    double center_x = box.x + box.width / 2.0 + shift, center_y = box.y + box.height / 2.0 + shift;
    cairo_arc(cairo, center_x, center_y, 8.5, 0, 2 * G_PI);
    set_color(cairo, YELLOW);
    cairo_fill_preserve(cairo);
    set_color(cairo, BLACK);
    cairo_stroke(cairo);
    switch (look) {
    case FACE_DEAD:
        /* Crosses for eyes, and a frown. */
        for (int side = -1; side <= 1; side += 2) {
            double eye_x = center_x + side * 3;
            cairo_move_to(cairo, eye_x - 1.5, center_y - 4.5);
            cairo_line_to(cairo, eye_x + 1.5, center_y - 1.5);
            cairo_move_to(cairo, eye_x + 1.5, center_y - 4.5);
            cairo_line_to(cairo, eye_x - 1.5, center_y - 1.5);
        }
        cairo_stroke(cairo);
        cairo_arc(cairo, center_x, center_y + 6.5, 3.5, 1.15 * G_PI, 1.85 * G_PI);
        cairo_stroke(cairo);
        break;
    case FACE_COOL:
        /* Sunglasses. */
        cairo_move_to(cairo, center_x - 7.5, center_y - 3);
        cairo_line_to(cairo, center_x + 7.5, center_y - 3);
        cairo_stroke(cairo);
        cairo_rectangle(cairo, center_x - 6, center_y - 3, 5, 3);
        cairo_rectangle(cairo, center_x + 1, center_y - 3, 5, 3);
        cairo_fill(cairo);
        cairo_arc(cairo, center_x, center_y + 1, 4, 0.2 * G_PI, 0.8 * G_PI);
        cairo_stroke(cairo);
        break;
    case FACE_WORRIED:
        cairo_arc(cairo, center_x - 3, center_y - 2.5, 1.3, 0, 2 * G_PI);
        cairo_fill(cairo);
        cairo_arc(cairo, center_x + 3, center_y - 2.5, 1.3, 0, 2 * G_PI);
        cairo_fill(cairo);
        cairo_arc(cairo, center_x, center_y + 3.5, 2, 0, 2 * G_PI);
        cairo_stroke(cairo);
        break;
    case FACE_SMILE:
    default:
        cairo_arc(cairo, center_x - 3, center_y - 2.5, 1.2, 0, 2 * G_PI);
        cairo_fill(cairo);
        cairo_arc(cairo, center_x + 3, center_y - 2.5, 1.2, 0, 2 * G_PI);
        cairo_fill(cairo);
        cairo_arc(cairo, center_x, center_y + 0.5, 4.5, 0.2 * G_PI, 0.8 * G_PI);
        cairo_stroke(cairo);
        break;
    }
}

static void draw_mine(cairo_t *cairo, double x, double y) {
    double center_x = x + CELL / 2.0, center_y = y + CELL / 2.0;
    set_color(cairo, BLACK);
    cairo_set_line_width(cairo, 1);
    cairo_move_to(cairo, center_x - 6.5, center_y);
    cairo_line_to(cairo, center_x + 6.5, center_y);
    cairo_move_to(cairo, center_x, center_y - 6.5);
    cairo_line_to(cairo, center_x, center_y + 6.5);
    cairo_move_to(cairo, center_x - 4.5, center_y - 4.5);
    cairo_line_to(cairo, center_x + 4.5, center_y + 4.5);
    cairo_move_to(cairo, center_x + 4.5, center_y - 4.5);
    cairo_line_to(cairo, center_x - 4.5, center_y + 4.5);
    cairo_stroke(cairo);
    cairo_arc(cairo, center_x, center_y, 4.5, 0, 2 * G_PI);
    cairo_fill(cairo);
    fill(cairo, WHITE, center_x - 2.5, center_y - 2.5, 2, 2);
}

static void draw_flag(cairo_t *cairo, double x, double y) {
    set_color(cairo, RED);
    cairo_move_to(cairo, x + 9, y + 3);
    cairo_line_to(cairo, x + 9, y + 9);
    cairo_line_to(cairo, x + 4, y + 6);
    cairo_close_path(cairo);
    cairo_fill(cairo);
    fill(cairo, BLACK, x + 8, y + 9, 1, 3);
    fill(cairo, BLACK, x + 6, y + 11, 5, 1);
    fill(cairo, BLACK, x + 4, y + 12, 9, 1);
}

static void draw_text(cairo_t *cairo, double x, double y, const char *text,
                      const double color[3]) {
    PangoLayout *layout = pango_cairo_create_layout(cairo);
    PangoFontDescription *font = pango_font_description_from_string("Sans Bold");
    pango_font_description_set_absolute_size(font, 11 * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_text(layout, text, -1);
    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);
    set_color(cairo, color);
    cairo_move_to(cairo, x + (CELL - width) / 2.0, y + (CELL - height) / 2.0);
    pango_cairo_show_layout(cairo, layout);
    g_object_unref(layout);
}

/* Whether a held button shows this covered square pressed in. */
static bool shown_pressed(const mines_app *app, int x, int y) {
    if (app->pressed_x < 0 || xd_minefield_over(&app->field))
        return false;
    if (app->chording)
        return abs(x - app->pressed_x) <= 1 && abs(y - app->pressed_y) <= 1;
    return x == app->pressed_x && y == app->pressed_y && app->left_down;
}

static void draw_square(cairo_t *cairo, mines_app *app, int square_x, int square_y) {
    double x = board_left() + BOARD_EDGE + square_x * CELL;
    double y = board_top() + BOARD_EDGE + square_y * CELL;
    const xd_mines_cell *cell = xd_minefield_cell(&app->field, square_x, square_y);
    int index = square_y * app->field.width + square_x;
    bool lost = app->field.state == XD_MINES_LOST;
    bool open = cell->mark == XD_MINES_OPEN;
    /* Once lost, the mines show, and the flags that were wrong. */
    bool shows_mine = lost && cell->mine && cell->mark != XD_MINES_FLAGGED;
    bool wrong_flag = lost && !cell->mine && cell->mark == XD_MINES_FLAGGED;
    bool pressed = (cell->mark == XD_MINES_COVERED || cell->mark == XD_MINES_QUESTIONED) &&
                   shown_pressed(app, square_x, square_y);
    if (open || shows_mine || wrong_flag || pressed) {
        fill(cairo, index == app->field.exploded ? RED : GREY, x, y, CELL, CELL);
        fill(cairo, SHADOW, x, y, CELL, 1);
        fill(cairo, SHADOW, x, y, 1, CELL);
        if (shows_mine || wrong_flag || (open && cell->mine)) {
            draw_mine(cairo, x, y);
            if (wrong_flag) {
                set_color(cairo, RED);
                cairo_set_line_width(cairo, 1.5);
                cairo_move_to(cairo, x + 3, y + 3);
                cairo_line_to(cairo, x + 13, y + 13);
                cairo_move_to(cairo, x + 13, y + 3);
                cairo_line_to(cairo, x + 3, y + 13);
                cairo_stroke(cairo);
            }
        } else if (open && cell->neighbors > 0) {
            char number[2] = {(char)('0' + cell->neighbors), '\0'};
            draw_text(cairo, x, y, number, number_colors[cell->neighbors]);
        }
        return;
    }
    fill(cairo, GREY, x, y, CELL, CELL);
    draw_edge(cairo, x, y, CELL, CELL, 2, true);
    if (cell->mark == XD_MINES_FLAGGED)
        draw_flag(cairo, x, y);
    else if (cell->mark == XD_MINES_QUESTIONED)
        draw_text(cairo, x, y, "?", BLACK);
}

static face_look current_face(const mines_app *app) {
    if (app->field.state == XD_MINES_LOST)
        return FACE_DEAD;
    if (app->field.state == XD_MINES_WON)
        return FACE_COOL;
    if (app->pressed_x >= 0 && (app->left_down || app->chording))
        return FACE_WORRIED;
    return FACE_SMILE;
}

static gboolean draw_board(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    mines_app *app = data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    fill(cairo, GREY, 0, 0, width, height);
    fill(cairo, WHITE, 0, 0, width, OUTER);
    fill(cairo, WHITE, 0, 0, OUTER, height);

    int panel_width = CELL * app->field.width + 2 * BOARD_EDGE;
    int panel_x = board_left(), panel_y = OUTER + MARGIN;
    draw_edge(cairo, panel_x, panel_y, panel_width, PANEL_HEIGHT, PANEL_EDGE, false);
    int counter_y = panel_y + (PANEL_HEIGHT - DIGIT_HEIGHT) / 2;
    draw_counter(cairo, panel_x + PANEL_EDGE + 5, counter_y,
                 xd_minefield_mines_left(&app->field));
    draw_counter(cairo, panel_x + panel_width - PANEL_EDGE - 5 - 3 * DIGIT_WIDTH, counter_y,
                 app->seconds);
    draw_face(cairo, face_box(app), current_face(app), app->face_pressed && app->face_hover);

    draw_edge(cairo, board_left(), board_top(), panel_width,
              CELL * app->field.height + 2 * BOARD_EDGE, BOARD_EDGE, false);
    for (int y = 0; y < app->field.height; y++) {
        for (int x = 0; x < app->field.width; x++)
            draw_square(cairo, app, x, y);
    }
    return TRUE;
}

/* ---- playing ---------------------------------------------------------------------------------- */

static void stop_timer(mines_app *app) {
    if (app->timer != 0)
        g_source_remove(app->timer);
    app->timer = 0;
}

static void new_game(mines_app *app) {
    stop_timer(app);
    app->seconds = 0;
    xd_minefield_reset(&app->field);
    app->pressed_x = app->pressed_y = -1;
    gtk_widget_queue_draw(app->board);
}

static void set_board_size(mines_app *app) {
    int width, height, mines;
    if (app->level == XD_MINES_CUSTOM) {
        width = app->custom_width;
        height = app->custom_height;
        mines = app->custom_mines;
    } else {
        xd_mines_level_size(app->level, &width, &height, &mines);
    }
    stop_timer(app);
    xd_minefield_finish(&app->field);
    xd_minefield_init(&app->field, width, height, mines);
    app->seconds = 0;
    app->pressed_x = app->pressed_y = -1;
    int content_width, content_height;
    content_size(app, &content_width, &content_height);
    gtk_widget_set_size_request(app->board, content_width, content_height);
    if (app->window != nullptr)
        gtk_window_resize(GTK_WINDOW(app->window), 1, 1);
    gtk_widget_queue_draw(app->board);
}

static gboolean tick(gpointer data) {
    mines_app *app = data;
    if (app->seconds < TIME_MAX)
        app->seconds++;
    gtk_widget_queue_draw(app->board);
    return G_SOURCE_CONTINUE;
}

static void record_dialog(mines_app *app);

/* After a move: the clock starts with the first, stops with the last. */
static void after_move(mines_app *app) {
    if (app->field.state == XD_MINES_PLAYING && app->timer == 0) {
        app->seconds = 1;
        app->timer = g_timeout_add_seconds(1, tick, app);
    }
    if (xd_minefield_over(&app->field))
        stop_timer(app);
    gtk_widget_queue_draw(app->board);
    if (app->field.state == XD_MINES_WON && app->level != XD_MINES_CUSTOM &&
        app->seconds < app->best[app->level].seconds)
        record_dialog(app);
}

static void press_at(mines_app *app, double x, double y) {
    int square_x, square_y;
    if (square_at(app, x, y, &square_x, &square_y)) {
        app->pressed_x = square_x;
        app->pressed_y = square_y;
    } else {
        app->pressed_x = app->pressed_y = -1;
    }
}

static bool in_face(const mines_app *app, double x, double y) {
    GdkRectangle box = face_box(app);
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}

static gboolean on_press([[maybe_unused]] GtkWidget *widget, GdkEventButton *event,
                         gpointer data) {
    mines_app *app = data;
    if (event->type != GDK_BUTTON_PRESS)
        return TRUE;
    if (event->button == GDK_BUTTON_PRIMARY && in_face(app, event->x, event->y)) {
        app->face_pressed = app->face_hover = true;
        gtk_widget_queue_draw(app->board);
        return TRUE;
    }
    if (xd_minefield_over(&app->field))
        return TRUE;
    if (event->button == GDK_BUTTON_PRIMARY)
        app->left_down = true;
    else if (event->button == GDK_BUTTON_SECONDARY)
        app->right_down = true;
    else if (event->button == GDK_BUTTON_MIDDLE)
        app->middle_down = true;
    press_at(app, event->x, event->y);
    app->chording = app->middle_down || (app->left_down && app->right_down);
    /* A right click alone marks at once. */
    if (event->button == GDK_BUTTON_SECONDARY && !app->chording && app->pressed_x >= 0)
        xd_minefield_cycle_mark(&app->field, app->pressed_x, app->pressed_y,
                                app->question_marks);
    gtk_widget_queue_draw(app->board);
    return TRUE;
}

static gboolean on_motion([[maybe_unused]] GtkWidget *widget, GdkEventMotion *event,
                          gpointer data) {
    mines_app *app = data;
    if (app->face_pressed) {
        bool hover = in_face(app, event->x, event->y);
        if (hover != app->face_hover) {
            app->face_hover = hover;
            gtk_widget_queue_draw(app->board);
        }
        return TRUE;
    }
    if (app->left_down || app->chording) {
        int old_x = app->pressed_x, old_y = app->pressed_y;
        press_at(app, event->x, event->y);
        if (old_x != app->pressed_x || old_y != app->pressed_y)
            gtk_widget_queue_draw(app->board);
    }
    return TRUE;
}

static gboolean on_release([[maybe_unused]] GtkWidget *widget, GdkEventButton *event,
                           gpointer data) {
    mines_app *app = data;
    if (event->button == GDK_BUTTON_PRIMARY && app->face_pressed) {
        bool hit = in_face(app, event->x, event->y);
        app->face_pressed = app->face_hover = false;
        if (hit)
            new_game(app);
        gtk_widget_queue_draw(app->board);
        return TRUE;
    }
    bool was_left = app->left_down;
    if (event->button == GDK_BUTTON_PRIMARY)
        app->left_down = false;
    else if (event->button == GDK_BUTTON_SECONDARY)
        app->right_down = false;
    else if (event->button == GDK_BUTTON_MIDDLE)
        app->middle_down = false;
    if (app->swallow_release) {
        if (!app->left_down && !app->right_down && !app->middle_down)
            app->swallow_release = false;
        return TRUE;
    }
    press_at(app, event->x, event->y);
    if (app->chording) {
        app->chording = false;
        app->swallow_release = app->left_down || app->right_down || app->middle_down;
        if (app->pressed_x >= 0)
            xd_minefield_chord(&app->field, app->pressed_x, app->pressed_y);
    } else if (event->button == GDK_BUTTON_PRIMARY && was_left && app->pressed_x >= 0) {
        xd_minefield_open(&app->field, app->pressed_x, app->pressed_y, app->rand);
    }
    app->pressed_x = app->pressed_y = -1;
    after_move(app);
    return TRUE;
}

/* ---- dialogs ---------------------------------------------------------------------------------- */

static GtkWidget *simple_dialog(mines_app *app, const char *title, GtkWidget **content) {
    GtkWidget *dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(*content), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), *content, TRUE,
                       TRUE, 0);
    return dialog;
}

static const char *const level_titles[] = {"Beginner", "Intermediate", "Expert"};

static void best_times_dialog(mines_app *app) {
    GtkWidget *content;
    GtkWidget *dialog = simple_dialog(app, "Fastest Times", &content);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Reset Scores", 1);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    GtkWidget *cells[3][2];
    for (int i = 0; i < 3; i++) {
        GtkWidget *level = gtk_label_new(level_titles[i]);
        gtk_label_set_xalign(GTK_LABEL(level), 0);
        gtk_grid_attach(GTK_GRID(grid), level, 0, i, 1, 1);
        for (int column = 0; column < 2; column++) {
            cells[i][column] = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(cells[i][column]), 0);
            gtk_grid_attach(GTK_GRID(grid), cells[i][column], column + 1, i, 1, 1);
        }
    }
    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    for (;;) {
        for (int i = 0; i < 3; i++) {
            g_autofree char *seconds = g_strdup_printf("%d seconds", app->best[i].seconds);
            gtk_label_set_text(GTK_LABEL(cells[i][0]), seconds);
            gtk_label_set_text(GTK_LABEL(cells[i][1]), app->best[i].name);
        }
        if (gtk_dialog_run(GTK_DIALOG(dialog)) != 1)
            break;
        for (int i = 0; i < 3; i++)
            app->best[i] = (best_time){.seconds = TIME_MAX, .name = ANONYMOUS};
        save_settings(app);
    }
    gtk_widget_destroy(dialog);
}

static void record_dialog(mines_app *app) {
    GtkWidget *content;
    GtkWidget *dialog = simple_dialog(app, TITLE, &content);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    g_autofree char *level = g_ascii_strdown(level_titles[app->level], -1);
    g_autofree char *text = g_strdup_printf(
        "You have the fastest time for %s level.\nPlease type your name.", level);
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), 32);
    gtk_entry_set_text(GTK_ENTRY(entry), app->best[app->level].name);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
    best_time *best = &app->best[app->level];
    best->seconds = app->seconds;
    g_strlcpy(best->name, name[0] != '\0' ? name : ANONYMOUS, sizeof(best->name));
    gtk_widget_destroy(dialog);
    save_settings(app);
    best_times_dialog(app);
}

static GtkWidget *custom_row(GtkWidget *grid, int row, const char *label, int value, int low,
                             int high) {
    GtkWidget *name = gtk_label_new_with_mnemonic(label);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    GtkWidget *spin = gtk_spin_button_new_with_range(low, high, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(name), spin);
    gtk_grid_attach(GTK_GRID(grid), name, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, row, 1, 1);
    return spin;
}

static void sync_levels(mines_app *app);

static void custom_dialog(mines_app *app) {
    GtkWidget *content;
    GtkWidget *dialog = simple_dialog(app, "Custom Field", &content);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    GtkWidget *height = custom_row(grid, 0, "_Height:", app->custom_height, 9, 24);
    GtkWidget *width = custom_row(grid, 1, "_Width:", app->custom_width, 9, 30);
    GtkWidget *mines = custom_row(grid, 2, "_Mines:", app->custom_mines, 10, 667);
    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        int new_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(width));
        int new_height = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(height));
        int new_mines = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(mines));
        xd_mines_clamp_custom(&new_width, &new_height, &new_mines);
        app->custom_width = new_width;
        app->custom_height = new_height;
        app->custom_mines = new_mines;
        app->level = XD_MINES_CUSTOM;
        save_settings(app);
        set_board_size(app);
    }
    sync_levels(app);
    gtk_widget_destroy(dialog);
}

/* ---- menus ------------------------------------------------------------------------------------ */

static void sync_levels(mines_app *app) {
    app->syncing = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->level_items[app->level]), TRUE);
    app->syncing = false;
}

static void on_level(GtkCheckMenuItem *item, gpointer data) {
    mines_app *app = data;
    if (app->syncing || !gtk_check_menu_item_get_active(item))
        return;
    gpointer level_data = g_object_get_data(G_OBJECT(item), "level");
    xd_mines_level level = (xd_mines_level)GPOINTER_TO_INT(level_data);
    if (level == XD_MINES_CUSTOM) {
        custom_dialog(app);
        return;
    }
    app->level = level;
    save_settings(app);
    set_board_size(app);
}

static void on_custom_activate([[maybe_unused]] GtkMenuItem *item, gpointer data) {
    mines_app *app = data;
    /* Choosing Custom again, already ticked, still asks. */
    if (app->level == XD_MINES_CUSTOM && !app->syncing)
        custom_dialog(app);
}

static void on_marks(GtkCheckMenuItem *item, gpointer data) {
    mines_app *app = data;
    app->question_marks = gtk_check_menu_item_get_active(item);
    save_settings(app);
}

static void on_new(gpointer data) {
    new_game(data);
}

static void on_best_times(gpointer data) {
    best_times_dialog(data);
}

static void on_exit_item(gpointer data) {
    mines_app *app = data;
    gtk_widget_destroy(app->window);
}

static GtkWidget *build_menus(mines_app *app, GtkAccelGroup *accel) {
    static const char *const level_labels[] = {"_Beginner", "_Intermediate", "_Expert",
                                               "_Custom..."};
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *game_item = gtk_menu_item_new_with_mnemonic("_Game");
    GtkWidget *game = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(game_item), game);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), game_item);

    GtkWidget *new_item = gtk_menu_item_new_with_mnemonic("_New");
    gtk_widget_add_accelerator(new_item, "activate", accel, GDK_KEY_F2, (GdkModifierType)0,
                               GTK_ACCEL_VISIBLE);
    g_signal_connect_swapped(new_item, "activate", G_CALLBACK(on_new), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(game), new_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(game), gtk_separator_menu_item_new());
    GSList *group = nullptr;
    for (int i = 0; i < 4; i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(group, level_labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "level", GINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(game), item);
        app->level_items[i] = item;
    }
    sync_levels(app);
    for (int i = 0; i < 4; i++)
        g_signal_connect(app->level_items[i], "toggled", G_CALLBACK(on_level), app);
    g_signal_connect(app->level_items[XD_MINES_CUSTOM], "activate",
                     G_CALLBACK(on_custom_activate), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(game), gtk_separator_menu_item_new());
    app->marks_item = gtk_check_menu_item_new_with_mnemonic("_Marks (?)");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->marks_item), app->question_marks);
    g_signal_connect(app->marks_item, "toggled", G_CALLBACK(on_marks), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(game), app->marks_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(game), gtk_separator_menu_item_new());
    GtkWidget *times = gtk_menu_item_new_with_mnemonic("Best _Times...");
    g_signal_connect_swapped(times, "activate", G_CALLBACK(on_best_times), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(game), times);
    gtk_menu_shell_append(GTK_MENU_SHELL(game), gtk_separator_menu_item_new());
    GtkWidget *exit_item = gtk_menu_item_new_with_mnemonic("E_xit");
    g_signal_connect_swapped(exit_item, "activate", G_CALLBACK(on_exit_item), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(game), exit_item);
    return bar;
}

int main(int argc, char **argv) {
    static mines_app app;
    app.pressed_x = app.pressed_y = -1;
    app.rand = g_rand_new();
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0)
            g_rand_set_seed(app.rand, (guint32)strtoul(argv[i + 1], nullptr, 10));
    }
    xd_gtk_app_preinit(APP_ID);
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    gtk_window_set_default_icon_name("applications-games");
    load_settings(&app);

    app.board = gtk_drawing_area_new();
    gtk_widget_add_events(app.board, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                         GDK_POINTER_MOTION_MASK);
    g_signal_connect(app.board, "draw", G_CALLBACK(draw_board), &app);
    g_signal_connect(app.board, "button-press-event", G_CALLBACK(on_press), &app);
    g_signal_connect(app.board, "motion-notify-event", G_CALLBACK(on_motion), &app);
    g_signal_connect(app.board, "button-release-event", G_CALLBACK(on_release), &app);
    set_board_size(&app);

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), TITLE);
    gtk_window_set_resizable(GTK_WINDOW(app.window), FALSE);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app.window), accel);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), build_menus(&app, accel), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), app.board, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app.window), box);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(app.window);
    gtk_main();
    stop_timer(&app);
    xd_minefield_finish(&app.field);
    g_rand_free(app.rand);
    return EXIT_SUCCESS;
}
