/* SPDX-License-Identifier: MIT */
#include "minefield.h"

#include <string.h>

static constexpr int WIDTH_MIN = 9;
static constexpr int WIDTH_MAX = 30;
static constexpr int HEIGHT_MIN = 9;
static constexpr int HEIGHT_MAX = 24;
static constexpr int MINES_MIN = 10;

static const char *const level_names[] = {
    [XD_MINES_BEGINNER] = "beginner",
    [XD_MINES_INTERMEDIATE] = "intermediate",
    [XD_MINES_EXPERT] = "expert",
    [XD_MINES_CUSTOM] = "custom",
};

void xd_mines_level_size(xd_mines_level level, int *width, int *height, int *mines) {
    switch (level) {
    case XD_MINES_INTERMEDIATE:
        *width = 16;
        *height = 16;
        *mines = 40;
        return;
    case XD_MINES_EXPERT:
        *width = 30;
        *height = 16;
        *mines = 99;
        return;
    case XD_MINES_BEGINNER:
    case XD_MINES_CUSTOM:
    default:
        *width = 9;
        *height = 9;
        *mines = 10;
        return;
    }
}

const char *xd_mines_level_name(xd_mines_level level) {
    return (size_t)level < G_N_ELEMENTS(level_names) ? level_names[level] : level_names[0];
}

bool xd_mines_level_parse(const char *name, xd_mines_level *level) {
    if (name == nullptr)
        return false;
    for (size_t i = 0; i < G_N_ELEMENTS(level_names); i++) {
        if (strcmp(name, level_names[i]) == 0) {
            *level = (xd_mines_level)i;
            return true;
        }
    }
    return false;
}

void xd_mines_clamp_custom(int *width, int *height, int *mines) {
    *width = CLAMP(*width, WIDTH_MIN, WIDTH_MAX);
    *height = CLAMP(*height, HEIGHT_MIN, HEIGHT_MAX);
    *mines = CLAMP(*mines, MINES_MIN, (*width - 1) * (*height - 1));
}

void xd_minefield_init(xd_minefield *field, int width, int height, int mines) {
    xd_mines_clamp_custom(&width, &height, &mines);
    *field = (xd_minefield){.width = width, .height = height, .mines = mines};
    field->cells = g_new0(xd_mines_cell, (size_t)(width * height));
    xd_minefield_reset(field);
}

void xd_minefield_finish(xd_minefield *field) {
    g_free(field->cells);
    field->cells = nullptr;
}

void xd_minefield_reset(xd_minefield *field) {
    memset(field->cells, 0, sizeof(*field->cells) * (size_t)(field->width * field->height));
    field->state = XD_MINES_READY;
    field->opened = 0;
    field->flags = 0;
    field->exploded = -1;
}

bool xd_minefield_contains(const xd_minefield *field, int x, int y) {
    return x >= 0 && y >= 0 && x < field->width && y < field->height;
}

xd_mines_cell *xd_minefield_cell(xd_minefield *field, int x, int y) {
    return xd_minefield_contains(field, x, y) ? &field->cells[y * field->width + x] : nullptr;
}

static void lay_mines(xd_minefield *field, int safe_x, int safe_y, GRand *rand) {
    int count = field->width * field->height;
    int safe = safe_y * field->width + safe_x;
    /* Shuffle the squares (Fisher-Yates) but the safe one; the first
     * `mines` are mined. */
    int *order = g_new(int, (size_t)count);
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (i != safe)
            order[kept++] = i;
    }
    for (int i = kept - 1; i > 0; i--) {
        int other = g_rand_int_range(rand, 0, i + 1);
        int swap = order[i];
        order[i] = order[other];
        order[other] = swap;
    }
    for (int i = 0; i < field->mines && i < kept; i++)
        field->cells[order[i]].mine = true;
    g_free(order);
    for (int y = 0; y < field->height; y++) {
        for (int x = 0; x < field->width; x++) {
            int around = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const xd_mines_cell *near = xd_minefield_cell(field, x + dx, y + dy);
                    around += near != nullptr && near->mine ? 1 : 0;
                }
            }
            xd_minefield_cell(field, x, y)->neighbors = (uint8_t)around;
        }
    }
}

static void lose(xd_minefield *field, int index) {
    field->state = XD_MINES_LOST;
    field->exploded = index;
}

static void check_won(xd_minefield *field) {
    if (field->state != XD_MINES_PLAYING ||
        field->opened != field->width * field->height - field->mines)
        return;
    field->state = XD_MINES_WON;
    /* The counter comes down to 0: every mine flagged. */
    int count = field->width * field->height;
    for (int i = 0; i < count; i++) {
        if (field->cells[i].mine)
            field->cells[i].mark = XD_MINES_FLAGGED;
    }
    field->flags = field->mines;
}

/* Opens one square and, from an empty one, the area around it. */
static int open_area(xd_minefield *field, int start_x, int start_y) {
    int opened = 0;
    GArray *pending = g_array_new(FALSE, FALSE, sizeof(int));
    int start = start_y * field->width + start_x;
    g_array_append_val(pending, start);
    while (pending->len > 0) {
        int index = g_array_index(pending, int, pending->len - 1);
        g_array_set_size(pending, pending->len - 1);
        xd_mines_cell *cell = &field->cells[index];
        if (cell->mark == XD_MINES_OPEN || cell->mark == XD_MINES_FLAGGED)
            continue;
        cell->mark = XD_MINES_OPEN;
        opened++;
        if (cell->mine) {
            lose(field, index);
            break;
        }
        if (cell->neighbors != 0)
            continue;
        int x = index % field->width, y = index / field->width;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                const xd_mines_cell *near = xd_minefield_cell(field, x + dx, y + dy);
                if (near != nullptr && near->mark != XD_MINES_OPEN &&
                    near->mark != XD_MINES_FLAGGED) {
                    int next = (y + dy) * field->width + x + dx;
                    g_array_append_val(pending, next);
                }
            }
        }
    }
    g_array_unref(pending);
    field->opened += opened;
    return opened;
}

int xd_minefield_open(xd_minefield *field, int x, int y, GRand *rand) {
    xd_mines_cell *cell = xd_minefield_cell(field, x, y);
    if (cell == nullptr || xd_minefield_over(field) || cell->mark == XD_MINES_OPEN ||
        cell->mark == XD_MINES_FLAGGED)
        return 0;
    if (field->state == XD_MINES_READY) {
        lay_mines(field, x, y, rand);
        field->state = XD_MINES_PLAYING;
    }
    int opened = open_area(field, x, y);
    check_won(field);
    return opened;
}

int xd_minefield_chord(xd_minefield *field, int x, int y) {
    xd_mines_cell *cell = xd_minefield_cell(field, x, y);
    if (cell == nullptr || field->state != XD_MINES_PLAYING || cell->mark != XD_MINES_OPEN ||
        cell->neighbors == 0)
        return 0;
    int flags = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const xd_mines_cell *near = xd_minefield_cell(field, x + dx, y + dy);
            flags += near != nullptr && near->mark == XD_MINES_FLAGGED ? 1 : 0;
        }
    }
    if (flags != cell->neighbors)
        return 0;
    int opened = 0;
    for (int dy = -1; dy <= 1 && field->state == XD_MINES_PLAYING; dy++) {
        for (int dx = -1; dx <= 1 && field->state == XD_MINES_PLAYING; dx++) {
            const xd_mines_cell *near = xd_minefield_cell(field, x + dx, y + dy);
            if (near != nullptr &&
                (near->mark == XD_MINES_COVERED || near->mark == XD_MINES_QUESTIONED))
                opened += open_area(field, x + dx, y + dy);
        }
    }
    check_won(field);
    return opened;
}

void xd_minefield_cycle_mark(xd_minefield *field, int x, int y, bool question_marks) {
    xd_mines_cell *cell = xd_minefield_cell(field, x, y);
    if (cell == nullptr || xd_minefield_over(field) || cell->mark == XD_MINES_OPEN)
        return;
    switch (cell->mark) {
    case XD_MINES_COVERED:
        cell->mark = XD_MINES_FLAGGED;
        field->flags++;
        break;
    case XD_MINES_FLAGGED:
        cell->mark = question_marks ? XD_MINES_QUESTIONED : XD_MINES_COVERED;
        field->flags--;
        break;
    case XD_MINES_QUESTIONED:
    case XD_MINES_OPEN:
    default:
        cell->mark = XD_MINES_COVERED;
        break;
    }
}

int xd_minefield_mines_left(const xd_minefield *field) {
    return field->mines - field->flags;
}

bool xd_minefield_over(const xd_minefield *field) {
    return field->state == XD_MINES_WON || field->state == XD_MINES_LOST;
}
