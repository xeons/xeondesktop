/* SPDX-License-Identifier: MIT
 * 3D Maze, after Windows 95's, in the look of 90s first-person shooters:
 * a walk through a random maze, following the left-hand wall until it
 * reaches the exit, then another maze. Floating polyhedra spin in the
 * corridors; walking into one turns the view upside down (and back), as
 * Windows' did.
 *
 * Drawn by raycasting, as those games were: a ray per column through the
 * tile map for the walls, the floor and ceiling cast row by row, all into
 * a small picture (200 lines when Pixelated) enlarged to the screen. The
 * textures are made here from noise, nothing taken from any game. Light
 * comes in areas (some flickering), falls off with distance in bands, and
 * walls facing one way are a little darker, for contrast. The polyhedra
 * are drawn by raster3d.c against the walls' depth.
 */
#include <math.h>
#include <string.h>

#include "../core/alloc.h"
#include "raster3d.h"
#include "saver_impl.h"

static constexpr int TEX = 64;           /* textures' size, a power of two */
static constexpr int LOW_LINES = 200;    /* the picture's height when pixelated */
static constexpr int SMOOTH_LINES = 540; /* and when not, at most */
static constexpr int CRYSTALS = 12;
static constexpr int BANDS = 16;         /* light levels, as a game's colour maps had */

/* The focal length, in picture heights: what's 1 away and 1 high fills
 * 0.9 of the picture's height. The width follows the screen's shape
 * (about 90 degrees across a 16:9 screen, as the games on wide screens). */
static constexpr double FOCAL_HEIGHTS = 0.9;

typedef enum tile : uint8_t {
    T_FLOOR,
    T_BRICK,
    T_TECH,
    T_RUST,
    T_EXIT,
    T_WALL_COUNT, /* the floor and ceiling textures follow the walls' */
} tile;
enum { TX_FLOOR = T_WALL_COUNT, TX_CEILING, TX_COUNT };

/* A texel lit by itself (lamps, the exit sign): no shading. */
static constexpr uint32_t FULLBRIGHT = 0x01000000u;

typedef enum walk_state : uint8_t { WALKING, TURNING, EXITING, FADING } walk_state;

typedef struct crystal {
    double x, y;
    double spin, bob;
    int shape; /* octahedron or cube */
    bool taken;
} crystal;

struct saver_maze {
    uint64_t *rng;
    int w, h;
    double walk_speed, turn_speed;
    int cells; /* maze cells along a side */
    bool pixelated;

    int side; /* tiles along a side: 2 * cells + 1 */
    uint8_t *map;
    uint8_t *light; /* each tile's light, 0..255 */
    uint8_t *flicker;
    uint32_t textures[TX_COUNT][TEX * TEX];

    int exit_x, exit_y;       /* the last cell, and its door's facing */
    int exit_dir;
    double x, y, angle;       /* the walker: tile units, radians */
    int tile_x, tile_y, dir;  /* where it's going, and facing */
    walk_state state;
    double turn_to;
    double roll, roll_to; /* the view's roll: 0 or pi */
    double bob;           /* the walk's step, for the head bobbing */
    double plane;         /* the view's half width at distance 1 */
    double fade, time;
    crystal crystals[CRYSTALS];
    int crystal_count;

    int vw, vh;
    cairo_surface_t *view;
    float *depth;
    xd_r3_target target;
};

static const int step_x[4] = {1, 0, -1, 0}; /* east, south, west, north (y down) */
static const int step_y[4] = {0, 1, 0, -1};

static double rnd(saver_maze *maze) { return xd_saver_rand(maze->rng); }
static int rnd_int(saver_maze *maze, int count) {
    return XD_MIN((int)(rnd(maze) * count), count - 1);
}

static uint8_t *tile_at(saver_maze *maze, int x, int y) {
    if (x < 0 || y < 0 || x >= maze->side || y >= maze->side)
        return nullptr;
    return &maze->map[(size_t)y * (size_t)maze->side + (size_t)x];
}

/* Writes inside the map only; make_maze's are, but this keeps it so. */
static void set_tile(saver_maze *maze, int x, int y, uint8_t value) {
    uint8_t *at = tile_at(maze, x, y);
    if (at != nullptr)
        *at = value;
}

static bool open_at(saver_maze *maze, int x, int y) {
    const uint8_t *at = tile_at(maze, x, y);
    return at != nullptr && *at == T_FLOOR;
}

static void route_ahead(saver_maze *maze, int steps, double *x, double *y);

/* ---- textures ------------------------------------------------------------------- */

static uint32_t rgb(double red, double green, double blue) {
    return (uint32_t)XD_CLAMP(red, 0, 255) << 16 | (uint32_t)XD_CLAMP(green, 0, 255) << 8 |
           (uint32_t)XD_CLAMP(blue, 0, 255);
}

/* Value noise on a 64 x 64 torus, cells of `size` texels: textures tile. */
static double tex_noise(const double *grid, int x, int y, int size) {
    int cells = TEX / size;
    double fx = (double)x / size, fy = (double)y / size;
    int x0 = (int)fx % cells, y0 = (int)fy % cells;
    int x1 = (x0 + 1) % cells, y1 = (y0 + 1) % cells;
    double tx = fx - floor(fx), ty = fy - floor(fy);
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    double top = grid[y0 * 64 + x0] + (grid[y0 * 64 + x1] - grid[y0 * 64 + x0]) * tx;
    double bottom = grid[y1 * 64 + x0] + (grid[y1 * 64 + x1] - grid[y1 * 64 + x0]) * tx;
    return top + (bottom - top) * ty;
}

/* Grime: three octaves of noise, about 0..1. */
static double grime(const double *grid, int x, int y) {
    return 0.5 * tex_noise(grid, x, y, 16) + 0.3 * tex_noise(grid, x, y, 8) +
           0.2 * tex_noise(grid, x, y, 4);
}

static void put(uint32_t *texture, int x, int y, uint32_t color) {
    texture[(y & (TEX - 1)) * TEX + (x & (TEX - 1))] = color;
}

/* A rivet: a light dot with a dark shadow below right. */
static void rivet(uint32_t *texture, int x, int y, double base) {
    put(texture, x, y, rgb(base * 1.5, base * 1.5, base * 1.45));
    put(texture, x + 1, y, rgb(base * 0.9, base * 0.9, base * 0.9));
    put(texture, x, y + 1, rgb(base * 0.9, base * 0.9, base * 0.9));
    put(texture, x + 1, y + 1, rgb(base * 0.45, base * 0.45, base * 0.45));
}

/* A raised plate's edges: light above and left, dark below and right. */
static void bevel(uint32_t *texture, int x0, int y0, int x1, int y1, double base) {
    for (int x = x0; x <= x1; x++) {
        put(texture, x, y0, rgb(base * 1.4, base * 1.4, base * 1.4));
        put(texture, x, y1, rgb(base * 0.5, base * 0.5, base * 0.5));
    }
    for (int y = y0; y <= y1; y++) {
        put(texture, x0, y, rgb(base * 1.3, base * 1.3, base * 1.3));
        put(texture, x1, y, rgb(base * 0.55, base * 0.55, base * 0.55));
    }
}

static void make_brick(uint32_t *texture, const double *grid, saver_maze *maze) {
    /* Each brick a shade of its own: 4 rows of 16, offset every other row. */
    double shades[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            shades[i][j] = 0.8 + 0.35 * rnd(maze);
    for (int y = 0; y < TEX; y++) {
        int row = y / 16;
        for (int x = 0; x < TEX; x++) {
            int shifted = x + (row % 2) * 8;
            bool mortar = y % 16 >= 14 || shifted % 32 >= 30;
            double g = grime(grid, x, y);
            double brick = shades[row][(shifted / 32) % 4];
            double k = mortar ? 0.35 + 0.15 * g : brick * (0.75 + 0.5 * g);
            put(texture, x, y, rgb(118 * k, 88 * k, 70 * k));
        }
    }
}

static void make_tech(uint32_t *texture, const double *grid) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            double k = 0.85 + 0.3 * grime(grid, x, y);
            put(texture, x, y, rgb(96 * k, 98 * k, 100 * k));
        }
    /* Two plates, a strip of status lights between them, rivets. */
    bevel(texture, 2, 2, 29, 40, 96);
    bevel(texture, 34, 2, 61, 40, 96);
    for (int x = 2; x < 62; x++)
        for (int y = 44; y < 52; y++)
            put(texture, x, y, rgb(30, 32, 34));
    static const uint32_t lamps[] = {0xD02020u, 0x20C040u, 0xE0C020u, 0x2060E0u};
    for (int i = 0; i < 6; i++)
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                put(texture, 6 + i * 9 + dx, 47 + dy, lamps[i % 4] | FULLBRIGHT);
    for (int x = 0; x < TEX; x++)
        for (int y = 56; y < 64; y++) {
            double k = 0.6 + 0.3 * grime(grid, x, y);
            put(texture, x, y, rgb(70 * k, 72 * k, 74 * k));
        }
    static const int rivets[][2] = {{5, 5}, {25, 5}, {5, 36}, {25, 36},
                                    {37, 5}, {57, 5}, {37, 36}, {57, 36}};
    for (size_t i = 0; i < XD_ARRAY_LEN(rivets); i++)
        rivet(texture, rivets[i][0], rivets[i][1], 100);
}

static void make_rust(uint32_t *texture, const double *grid) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            /* Dark streaks running down, and blotches of bare grey metal
             * where the rust has flaked. */
            double streak = tex_noise(grid, (x * 4) & (TEX - 1), y / 4, 8);
            double g = grime(grid, x, y);
            double k = 0.45 + 0.55 * g - 0.3 * streak;
            double flake = tex_noise(grid, (x + 32) & (TEX - 1), (y + 16) & (TEX - 1), 4);
            double bare = 0.55 + 0.3 * g;
            put(texture, x, y,
                flake > 0.82 ? rgb(70 * bare, 66 * bare, 62 * bare) : rgb(104 * k, 58 * k, 32 * k));
        }
    bevel(texture, 0, 0, 63, 31, 90);
    bevel(texture, 0, 32, 63, 63, 90);
    for (int x = 4; x < 64; x += 12) {
        rivet(texture, x, 3, 95);
        rivet(texture, x, 28, 95);
        rivet(texture, x, 35, 95);
        rivet(texture, x, 60, 95);
    }
}

/* EXIT in a 5 x 7 pixel font, two texels a pixel. */
static void make_exit(uint32_t *texture, const double *grid) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            double k = 0.7 + 0.3 * grime(grid, x, y);
            bool seam = x % 16 == 0;
            put(texture, x, y, seam ? rgb(30, 30, 34) : rgb(70 * k, 74 * k, 82 * k));
        }
    /* Hazard stripes down both sides. */
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < 6; x++) {
            bool yellow = ((x + y) / 4) % 2 == 0;
            uint32_t color = yellow ? rgb(220, 180, 20) : rgb(24, 24, 24);
            put(texture, x, y, color);
            put(texture, TEX - 1 - x, y, color);
        }
    for (int y = 6; y < 26; y++)
        for (int x = 8; x < 56; x++)
            put(texture, x, y, rgb(12, 10, 10));
    static const char *const glyphs[4][7] = {
        {"#####", "#....", "#....", "####.", "#....", "#....", "#####"},
        {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"},
        {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"},
        {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."},
    };
    for (int letter = 0; letter < 4; letter++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (glyphs[letter][row][col] == '#')
                    for (int d = 0; d < 4; d++)
                        put(texture, 11 + letter * 11 + col * 2 + d % 2, 9 + row * 2 + d / 2,
                            0xFF2018u | FULLBRIGHT);
    for (int y = 30; y < 64; y += 10)
        for (int x = 12; x < 56; x += 14)
            rivet(texture, x, y, 80);
}

static void make_floor(uint32_t *texture, const double *grid) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            bool grout = x % 32 >= 30 || y % 32 >= 30;
            double g = grime(grid, x, y);
            double k = grout ? 0.3 + 0.1 * g : 0.6 + 0.55 * g;
            put(texture, x, y, rgb(92 * k, 86 * k, 78 * k));
        }
}

static void make_ceiling(uint32_t *texture, const double *grid) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            double k = 0.5 + 0.4 * grime(grid, x, y);
            bool rib = x % 16 == 0 || y % 16 == 0;
            put(texture, x, y, rib ? rgb(34, 34, 38) : rgb(70 * k, 70 * k, 76 * k));
        }
    /* A lamp in the middle, lit by itself. */
    bevel(texture, 22, 22, 41, 41, 80);
    for (int y = 24; y < 40; y++)
        for (int x = 24; x < 40; x++)
            put(texture, x, y, rgb(230, 230, 210) | FULLBRIGHT);
}

static void make_textures(saver_maze *maze) {
    double grid[64 * 64];
    for (int t = 0; t < TX_COUNT; t++) {
        for (size_t i = 0; i < XD_ARRAY_LEN(grid); i++)
            grid[i] = rnd(maze);
        uint32_t *texture = maze->textures[t];
        switch (t) {
        case T_BRICK:
            make_brick(texture, grid, maze);
            break;
        case T_TECH:
            make_tech(texture, grid);
            break;
        case T_RUST:
            make_rust(texture, grid);
            break;
        case T_EXIT:
            make_exit(texture, grid);
            break;
        case TX_FLOOR:
            make_floor(texture, grid);
            break;
        case TX_CEILING:
            make_ceiling(texture, grid);
            break;
        default:
            break;
        }
    }
}

/* ---- the maze --------------------------------------------------------------------- */

/* A maze without loops (a random depth-first walk), so following one
 * wall reaches every cell; the exit is the cell farthest from the start,
 * its door the wall at the end of it. */
static void make_maze(saver_maze *maze) {
    int side = maze->side;
    memset(maze->map, T_BRICK, (size_t)side * (size_t)side);
    int cells = maze->cells;
    int *stack = xd_alloc_array((size_t)cells * (size_t)cells, sizeof(int));
    int *distance = xd_alloc_array((size_t)cells * (size_t)cells, sizeof(int));
    for (int i = 0; i < cells * cells; i++)
        distance[i] = -1;
    int top = 0;
    stack[top++] = 0;
    distance[0] = 0;
    set_tile(maze, 1, 1, T_FLOOR);
    int far = 0;
    while (top > 0) {
        int cell = stack[top - 1];
        int cx = cell % cells, cy = cell / cells;
        int ways[4], count = 0;
        for (int d = 0; d < 4; d++) {
            int nx = cx + step_x[d], ny = cy + step_y[d];
            if (nx >= 0 && ny >= 0 && nx < cells && ny < cells && distance[ny * cells + nx] < 0)
                ways[count++] = d;
        }
        if (count == 0) {
            top--;
            continue;
        }
        int d = ways[rnd_int(maze, count)];
        int next = (cy + step_y[d]) * cells + cx + step_x[d];
        distance[next] = distance[cell] + 1;
        if (distance[next] > distance[far])
            far = next;
        set_tile(maze, 2 * cx + 1 + step_x[d], 2 * cy + 1 + step_y[d], T_FLOOR);
        set_tile(maze, 2 * cx + 1 + 2 * step_x[d], 2 * cy + 1 + 2 * step_y[d], T_FLOOR);
        stack[top++] = next;
    }
    xd_free(stack);
    xd_free(distance);

    /* Areas of other walls: tech panels and rust among the brick. */
    int regions = (side + 5) / 6;
    tile *kinds = xd_alloc_array((size_t)regions * (size_t)regions, sizeof(tile));
    uint8_t *levels = xd_alloc_array((size_t)regions * (size_t)regions, 1);
    for (int i = 0; i < regions * regions; i++) {
        double pick = rnd(maze);
        kinds[i] = pick < 0.45 ? T_BRICK : pick < 0.8 ? T_TECH : T_RUST;
        levels[i] = (uint8_t)(110 + rnd_int(maze, 120));
    }
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            int region = (y / 6) * regions + x / 6;
            uint8_t *at = tile_at(maze, x, y);
            if (*at != T_FLOOR)
                *at = kinds[region];
            maze->light[y * side + x] = levels[region];
            maze->flicker[y * side + x] = rnd(maze) < 0.04;
        }
    xd_free(kinds);
    xd_free(levels);

    maze->exit_x = 2 * (far % cells) + 1;
    maze->exit_y = 2 * (far / cells) + 1;
    maze->exit_dir = -1;
    for (int d = 0; d < 4; d++) {
        if (!open_at(maze, maze->exit_x + step_x[d], maze->exit_y + step_y[d]))
            maze->exit_dir = d;
    }
    if (maze->exit_dir >= 0) {
        set_tile(maze, maze->exit_x + step_x[maze->exit_dir],
                 maze->exit_y + step_y[maze->exit_dir], T_EXIT);
        maze->light[(maze->exit_y + step_y[maze->exit_dir]) * side + maze->exit_x +
                    step_x[maze->exit_dir]] = 255;
    }

    /* The polyhedra: the first in the corridor ahead of the start, so one
     * soon turns up, the others anywhere (more in a bigger maze). */
    maze->crystal_count = XD_CLAMP(cells * cells / 10, 2, CRYSTALS);
    for (int i = 0; i < maze->crystal_count; i++) {
        crystal *c = &maze->crystals[i];
        int cell = 1 + rnd_int(maze, cells * cells - 1);
        c->x = 2 * (cell % cells) + 1.5;
        c->y = 2 * (cell / cells) + 1.5;

        c->spin = rnd(maze) * 6.28;
        c->bob = rnd(maze) * 6.28;
        c->shape = i % 2;
        c->taken = false;
    }

    /* The start: the first cell, facing its way out. */
    maze->tile_x = 1;
    maze->tile_y = 1;
    maze->x = maze->y = 1.5;
    maze->dir = open_at(maze, 2, 1) ? 0 : 1;
    maze->angle = maze->dir * M_PI / 2;
    maze->state = WALKING;
    maze->fade = 1.0; /* in from black */
    /* The first polyhedron ten tiles along the way: it turns up soon. */
    route_ahead(maze, 10, &maze->crystals[0].x, &maze->crystals[0].y);
}

/* ---- walking ---------------------------------------------------------------------- */

static void choose_way(saver_maze *maze);

/* Where the walker will be `steps` tiles on: its route followed ahead. */
static void route_ahead(saver_maze *maze, int steps, double *x, double *y) {
    saver_maze ahead = *maze;
    ahead.exit_dir = -1; /* no stopping at the exit */
    for (int i = 0; i < steps; i++)
        choose_way(&ahead);
    *x = ahead.tile_x + 0.5;
    *y = ahead.tile_y + 0.5;
}

/* At a tile's middle: the left-hand rule (left, straight on, right, back). */
static void choose_way(saver_maze *maze) {
    if (maze->tile_x == maze->exit_x && maze->tile_y == maze->exit_y && maze->exit_dir >= 0) {
        maze->state = maze->dir == maze->exit_dir ? EXITING : TURNING;
        maze->turn_to = maze->exit_dir * M_PI / 2;
        if (maze->state == TURNING) {
            double delta = remainder(maze->turn_to - maze->angle, 2 * M_PI);
            maze->turn_to = maze->angle + delta;
        }
        return;
    }
    static const int order[4] = {3, 0, 1, 2};
    for (int i = 0; i < 4; i++) {
        int d = (maze->dir + order[i]) % 4;
        if (!open_at(maze, maze->tile_x + step_x[d], maze->tile_y + step_y[d]))
            continue;
        if (d != maze->dir) {
            int turn = order[i] == 3 ? -1 : order[i] == 1 ? 1 : 2;
            maze->turn_to = maze->angle + turn * M_PI / 2;
            maze->dir = d;
            maze->state = TURNING;
        }
        maze->tile_x += step_x[d];
        maze->tile_y += step_y[d];
        return;
    }
}

static void walk(saver_maze *maze, double dt) {
    switch (maze->state) {
    case TURNING: {
        double delta = maze->turn_to - maze->angle;
        double turned = copysign(fmin(fabs(delta), maze->turn_speed * dt), delta);
        maze->angle += turned;
        if (fabs(maze->turn_to - maze->angle) < 1e-6) {
            maze->angle = maze->turn_to;
            if (maze->tile_x == maze->exit_x && maze->tile_y == maze->exit_y &&
                maze->exit_dir >= 0 && maze->dir != maze->exit_dir) {
                maze->dir = maze->exit_dir;
                maze->state = EXITING;
            } else {
                maze->state = WALKING;
            }
        }
        break;
    }
    case WALKING: {
        double gx = maze->tile_x + 0.5, gy = maze->tile_y + 0.5;
        double dx = gx - maze->x, dy = gy - maze->y;
        double distance = hypot(dx, dy);
        double move = maze->walk_speed * dt;
        if (distance <= move) {
            maze->x = gx;
            maze->y = gy;
            choose_way(maze);
        } else {
            maze->x += dx / distance * move;
            maze->y += dy / distance * move;
        }
        maze->bob += move * 2 * M_PI / 1.3;
        break;
    }
    case EXITING:
        /* Up to the door, then out. */
        maze->x += step_x[maze->dir] * maze->walk_speed * dt * 0.6;
        maze->y += step_y[maze->dir] * maze->walk_speed * dt * 0.6;
        if (fabs(maze->x - (maze->tile_x + 0.5)) + fabs(maze->y - (maze->tile_y + 0.5)) > 0.25)
            maze->state = FADING;
        break;
    case FADING:
        maze->fade += dt;
        if (maze->fade >= 1.0)
            make_maze(maze);
        break;
    }
    if (maze->state != FADING && maze->fade > 0)
        maze->fade = fmax(0.0, maze->fade - dt);

    for (int i = 0; i < maze->crystal_count; i++) {
        crystal *c = &maze->crystals[i];
        c->spin += dt * 1.6;
        c->bob += dt * 2.0;
        if (!c->taken && hypot(c->x - maze->x, c->y - maze->y) < 0.45) {
            c->taken = true;
            maze->roll_to = maze->roll_to > M_PI / 2 ? 0 : M_PI;
        }
    }
    double delta = maze->roll_to - maze->roll;
    maze->roll += copysign(fmin(fabs(delta), M_PI * dt / 1.2), delta);
}

/* ---- drawing ---------------------------------------------------------------------- */

/* A texel under the light `level` (0..255), in BANDS steps. */
static uint32_t lit(uint32_t texel, double level) {
    if (texel & FULLBRIGHT)
        return texel & 0xFFFFFFu;
    double band = floor(XD_CLAMP(level, 8, 255) / (256.0 / BANDS)) / (BANDS - 1);
    return rgb(((texel >> 16) & 0xFF) * band, ((texel >> 8) & 0xFF) * band,
               (texel & 0xFF) * band);
}

/* The light where a ray lands: its area's, flickering or not, dimmer
 * with distance. */
static double light_at(saver_maze *maze, double x, double y, double distance) {
    int tx = XD_CLAMP((int)x, 0, maze->side - 1), ty = XD_CLAMP((int)y, 0, maze->side - 1);
    size_t at = (size_t)ty * (size_t)maze->side + (size_t)tx;
    double level = maze->light[at];
    if (maze->flicker[at] && fmod(maze->time * 7.3 + tx * 1.7 + ty, 3.1) < 0.6)
        level *= 0.45;
    return level - distance * 30;
}

static void draw_floor_and_ceiling(saver_maze *maze, uint32_t *pixels, int stride,
                                   double dir_x, double dir_y, double plane_x, double plane_y) {
    double f = maze->target.focal, cy = maze->target.cy;
    for (int y = 0; y < maze->vh; y++) {
        bool floor_row = y + 0.5 > cy;
        double from_middle = fabs(y + 0.5 - cy);
        if (from_middle < 0.5)
            continue;
        double distance = 0.5 * f / from_middle; /* the eye half a wall up */
        double left_x = dir_x - plane_x, left_y = dir_y - plane_y;
        double step_x_px = 2 * plane_x * distance / maze->vw;
        double step_y_px = 2 * plane_y * distance / maze->vw;
        double wx = maze->x + distance * left_x, wy = maze->y + distance * left_y;
        const uint32_t *texture = maze->textures[floor_row ? TX_FLOOR : TX_CEILING];
        uint32_t *row = pixels + (size_t)y * (size_t)stride;
        float *depth = maze->depth + (size_t)y * (size_t)maze->vw;
        for (int x = 0; x < maze->vw; x++) {
            int tx = (int)(wx * TEX) & (TEX - 1), ty = (int)(wy * TEX) & (TEX - 1);
            row[x] = lit(texture[ty * TEX + tx], light_at(maze, wx, wy, distance));
            depth[x] = (float)distance;
            wx += step_x_px;
            wy += step_y_px;
        }
    }
}

static void draw_walls(saver_maze *maze, uint32_t *pixels, int stride, double dir_x,
                       double dir_y, double plane_x, double plane_y) {
    double f = maze->target.focal, cy = maze->target.cy;
    for (int x = 0; x < maze->vw; x++) {
        double camera = 2.0 * (x + 0.5) / maze->vw - 1;
        double ray_x = dir_x + plane_x * camera, ray_y = dir_y + plane_y * camera;
        int map_x = (int)maze->x, map_y = (int)maze->y;
        double delta_x = ray_x == 0 ? 1e30 : fabs(1 / ray_x);
        double delta_y = ray_y == 0 ? 1e30 : fabs(1 / ray_y);
        int sx = ray_x < 0 ? -1 : 1, sy = ray_y < 0 ? -1 : 1;
        double side_x = (ray_x < 0 ? maze->x - map_x : map_x + 1 - maze->x) * delta_x;
        double side_y = (ray_y < 0 ? maze->y - map_y : map_y + 1 - maze->y) * delta_y;
        int hit_side = 0;
        uint8_t kind = T_BRICK;
        for (int steps = 0; steps < 4 * maze->side; steps++) {
            if (side_x < side_y) {
                side_x += delta_x;
                map_x += sx;
                hit_side = 0;
            } else {
                side_y += delta_y;
                map_y += sy;
                hit_side = 1;
            }
            const uint8_t *at = tile_at(maze, map_x, map_y);
            if (at == nullptr || *at != T_FLOOR) {
                kind = at != nullptr ? *at : T_BRICK;
                break;
            }
        }
        double distance = hit_side == 0 ? side_x - delta_x : side_y - delta_y;
        distance = fmax(distance, 0.02);
        double wall_x = hit_side == 0 ? maze->y + distance * ray_y : maze->x + distance * ray_x;
        wall_x -= floor(wall_x);
        int tex_x = (int)(wall_x * TEX) & (TEX - 1);
        if ((hit_side == 0 && ray_x > 0) || (hit_side == 1 && ray_y < 0))
            tex_x = TEX - 1 - tex_x;
        double height = f / distance;
        /* Clamped to the picture first: close up, a wall is enormous. */
        double top_edge = XD_CLAMP(ceil(cy - height / 2 - 0.5), 0.0, (double)maze->vh);
        double bottom_edge = XD_CLAMP(floor(cy + height / 2 - 0.5), -1.0, maze->vh - 1.0);
        int top = (int)top_edge, bottom = (int)bottom_edge;
        /* Walls running one way lit a little less: contrast at corners. */
        double level = light_at(maze, maze->x + distance * ray_x * 0.98,
                                maze->y + distance * ray_y * 0.98, distance) *
                       (hit_side == 1 ? 1.0 : 0.82);
        const uint32_t *texture = maze->textures[kind < T_WALL_COUNT ? kind : T_BRICK];
        for (int y = top; y <= bottom; y++) {
            int tex_y = (int)((y + 0.5 - (cy - height / 2)) / height * TEX) & (TEX - 1);
            pixels[(size_t)y * (size_t)stride + (size_t)x] =
                lit(texture[tex_y * TEX + tex_x], level);
            maze->depth[(size_t)y * (size_t)maze->vw + (size_t)x] = (float)distance;
        }
    }
}

/* A polyhedron spinning in camera space: an octahedron or a cube. */
static void draw_crystal(saver_maze *maze, const crystal *c, double dir_x, double dir_y) {
    double rx = c->x - maze->x, ry = c->y - maze->y;
    double z = rx * dir_x + ry * dir_y;
    double side = rx * -dir_y + ry * dir_x;
    if (z < 0.2)
        return;
    xd_r3_vec center = {side, -0.08 * sin(c->bob) - 0.05, z};
    double size = 0.2;
    double ca = cos(c->spin), sa = sin(c->spin), cb = cos(c->spin * 0.7), sb = sin(c->spin * 0.7);
    xd_r3_vec corners[8];
    int count;
    static const int octahedron_faces[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                                               {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    static const int cube_faces[12][3] = {{0, 1, 3}, {0, 3, 2}, {4, 6, 7}, {4, 7, 5},
                                          {0, 4, 5}, {0, 5, 1}, {2, 3, 7}, {2, 7, 6},
                                          {0, 2, 6}, {0, 6, 4}, {1, 5, 7}, {1, 7, 3}};
    if (c->shape == 0) {
        static const double octahedron[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                                {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        count = 6;
        for (int i = 0; i < count; i++)
            corners[i] = (xd_r3_vec){octahedron[i][0], octahedron[i][1] * 1.4, octahedron[i][2]};
    } else {
        count = 8;
        for (int i = 0; i < count; i++)
            corners[i] = (xd_r3_vec){i & 1 ? 0.7 : -0.7, i & 2 ? 0.7 : -0.7, i & 4 ? 0.7 : -0.7};
    }
    for (int i = 0; i < count; i++) {
        xd_r3_vec p = corners[i];
        xd_r3_vec turned = {p.x * ca + p.z * sa, p.y, -p.x * sa + p.z * ca};
        xd_r3_vec tilted = {turned.x, turned.y * cb - turned.z * sb, turned.y * sb + turned.z * cb};
        corners[i] = (xd_r3_vec){center.x + tilted.x * size, center.y + tilted.y * size,
                                 center.z + tilted.z * size};
    }
    xd_r3_material material = c->shape == 0
                                  ? (xd_r3_material){0.95, 0.75, 0.2, 0.8, 30, true}
                                  : (xd_r3_material){0.3, 0.85, 0.95, 0.8, 30, true};
    int faces = c->shape == 0 ? 8 : 12;
    for (int i = 0; i < faces; i++) {
        const int *face = c->shape == 0 ? octahedron_faces[i] : cube_faces[i];
        xd_r3_vec p[3] = {corners[face[0]], corners[face[1]], corners[face[2]]};
        xd_r3_triangle(&maze->target, p, p, &material);
    }
}

static void draw_view(saver_maze *maze) {
    cairo_surface_flush(maze->view);
    uint32_t *pixels = maze->target.pixels;
    int stride = maze->target.stride;
    double dir_x = cos(maze->angle), dir_y = sin(maze->angle);
    double plane_x = -dir_y * maze->plane, plane_y = dir_x * maze->plane;
    /* Bobbing: the horizon rides a little up and down with each step. */
    maze->target.cy = maze->vh / 2.0 + sin(maze->bob) * maze->vh * 0.012;
    draw_floor_and_ceiling(maze, pixels, stride, dir_x, dir_y, plane_x, plane_y);
    draw_walls(maze, pixels, stride, dir_x, dir_y, plane_x, plane_y);
    for (int i = 0; i < maze->crystal_count; i++)
        if (!maze->crystals[i].taken)
            draw_crystal(maze, &maze->crystals[i], dir_x, dir_y);
    cairo_surface_mark_dirty(maze->view);
}

/* ---- the saver -------------------------------------------------------------------- */

saver_maze *saver_maze_create(const xd_saver_settings *saver_settings, int width, int height,
                              uint64_t *rng) {
    saver_maze *maze = XD_NEW(saver_maze);
    maze->rng = rng;
    maze->w = width;
    maze->h = height;
    double speed = 0.4 + 0.16 * saver_settings->maze.speed; /* 0.4 to 2 */
    maze->walk_speed = 1.6 * speed;
    maze->turn_speed = 2.6 * speed;
    maze->cells = 5 + 2 * XD_CLAMP(saver_settings->maze.size, 0, XD_SAVER_SLIDER_MAX);
    maze->pixelated = saver_settings->maze.pixelated;
    maze->side = 2 * maze->cells + 1;
    size_t tiles = (size_t)maze->side * (size_t)maze->side;
    maze->map = xd_alloc(tiles);
    maze->light = xd_alloc(tiles);
    maze->flicker = xd_alloc(tiles);
    make_textures(maze);
    make_maze(maze);

    maze->vh = XD_MIN(maze->pixelated ? LOW_LINES : SMOOTH_LINES, height);
    maze->vh = XD_MAX(maze->vh, 16);
    maze->vw = XD_MAX((int)lround((double)maze->vh * width / XD_MAX(height, 1)), 16);
    maze->view = cairo_image_surface_create(CAIRO_FORMAT_RGB24, maze->vw, maze->vh);
    maze->depth = xd_alloc_array((size_t)maze->vw * (size_t)maze->vh, sizeof(float));
    cairo_surface_flush(maze->view);
    maze->target = (xd_r3_target){
        .pixels = (uint32_t *)(void *)cairo_image_surface_get_data(maze->view),
        .depth = maze->depth,
        .width = maze->vw,
        .height = maze->vh,
        .stride = cairo_image_surface_get_stride(maze->view) / 4,
        .focal = maze->vh * FOCAL_HEIGHTS,
        .cx = maze->vw / 2.0,
        .cy = maze->vh / 2.0,
        .light = {-0.4, -0.75, -0.5},
        .ambient = 0.3,
    };
    maze->plane = maze->vw / 2.0 / maze->target.focal;
    return maze;
}

void saver_maze_frame(saver_maze *maze, cairo_t *cairo, double dt) {
    maze->time += dt;
    walk(maze, dt);
    draw_view(maze);
    cairo_save(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_paint(cairo);
    /* The small picture enlarged to the screen, turned as far as the
     * view has rolled. */
    cairo_translate(cairo, maze->w / 2.0, maze->h / 2.0);
    cairo_rotate(cairo, maze->roll);
    /* Enlarged part way round, so the turned picture still covers it. */
    double aspect = fmax((double)maze->w / maze->h, (double)maze->h / maze->w);
    double cover = fabs(cos(maze->roll)) + aspect * fabs(sin(maze->roll));
    cairo_scale(cairo, cover * maze->w / maze->vw, cover * maze->h / maze->vh);
    cairo_translate(cairo, -maze->vw / 2.0, -maze->vh / 2.0);
    cairo_set_source_surface(cairo, maze->view, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cairo),
                             maze->pixelated ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
    cairo_paint(cairo);
    cairo_restore(cairo);
    if (maze->fade > 0) {
        cairo_set_source_rgba(cairo, 0, 0, 0, fmin(maze->fade, 1.0));
        cairo_paint(cairo);
    }
}

void saver_maze_destroy(saver_maze *maze) {
    cairo_surface_destroy(maze->view);
    xd_free(maze->depth);
    xd_free(maze->map);
    xd_free(maze->light);
    xd_free(maze->flicker);
    xd_free(maze);
}
