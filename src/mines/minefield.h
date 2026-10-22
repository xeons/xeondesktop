/* SPDX-License-Identifier: MIT
 * Minefield's game, apart from GTK so the unit tests can play it: a grid
 * of covered squares, some mined; opening one shows how many mines touch
 * it (and opens the empty area around a square with none), flags mark
 * mines, and opening every square without a mine wins.
 *
 * As Windows XP's game: the first square opened is never a mine (the
 * mines are laid then), opening a numbered square whose flags are all
 * placed opens its neighbours (chording), and a win flags the rest.
 */
#ifndef XD_MINES_MINEFIELD_H
#define XD_MINES_MINEFIELD_H

#include <glib.h>
#include <stdint.h>

typedef enum xd_mines_state : uint8_t {
    XD_MINES_READY,   /* nothing opened yet: no mines laid */
    XD_MINES_PLAYING,
    XD_MINES_WON,
    XD_MINES_LOST,
} xd_mines_state;

typedef enum xd_mines_mark : uint8_t {
    XD_MINES_COVERED,
    XD_MINES_FLAGGED,
    XD_MINES_QUESTIONED,
    XD_MINES_OPEN,
} xd_mines_mark;

typedef struct xd_mines_cell {
    bool mine;
    uint8_t neighbors; /* mines around it */
    xd_mines_mark mark;
} xd_mines_cell;

typedef struct xd_minefield {
    int width, height, mines;
    xd_mines_cell *cells; /* row by row */
    xd_mines_state state;
    int opened;
    int flags;
    int exploded; /* the mine that went off, or -1 */
} xd_minefield;

/* Windows XP's levels, and the limits of Custom. */
typedef enum xd_mines_level : uint8_t {
    XD_MINES_BEGINNER,     /* 9 x 9, 10 mines */
    XD_MINES_INTERMEDIATE, /* 16 x 16, 40 */
    XD_MINES_EXPERT,       /* 30 x 16, 99 */
    XD_MINES_CUSTOM,
} xd_mines_level;

void xd_mines_level_size(xd_mines_level level, int *width, int *height, int *mines);
/* "beginner", "intermediate", "expert", "custom", and back. */
const char *xd_mines_level_name(xd_mines_level level);
bool xd_mines_level_parse(const char *name, xd_mines_level *level);
/* Custom's limits: 9 to 30 wide, 9 to 24 high, 10 to (width-1)*(height-1)
 * mines. */
void xd_mines_clamp_custom(int *width, int *height, int *mines);

/* A new game, sizes clamped to Custom's limits. */
void xd_minefield_init(xd_minefield *field, int width, int height, int mines);
void xd_minefield_finish(xd_minefield *field);
/* The same size again, all covered. */
void xd_minefield_reset(xd_minefield *field);

xd_mines_cell *xd_minefield_cell(xd_minefield *field, int x, int y);
bool xd_minefield_contains(const xd_minefield *field, int x, int y);

/* Opens a covered square (not a flagged one); the first lays the mines,
 * from `rand`, never there. Returns how many squares opened. */
int xd_minefield_open(xd_minefield *field, int x, int y, GRand *rand);
/* An open number with as many flags around it as it says: opens the
 * rest of its neighbours. Returns how many squares opened. */
int xd_minefield_chord(xd_minefield *field, int x, int y);
/* Covered, flagged, then questioned (with `question_marks`), round. */
void xd_minefield_cycle_mark(xd_minefield *field, int x, int y, bool question_marks);
/* The counter: mines less flags (negative with too many flags). */
int xd_minefield_mines_left(const xd_minefield *field);
bool xd_minefield_over(const xd_minefield *field);

#endif
