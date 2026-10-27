/* SPDX-License-Identifier: MIT
 * Explode's maths: a closing window cut into pieces (rectangles,
 * hexagons or glass shards), each flying out from the middle, spinning
 * about an axis of its own and coming toward the viewer, seen in
 * perspective. Pure, no wlroots or GL, so it's unit-tested.
 */
#ifndef XD_COMPOSITOR_EFFECTS_EXPLODE_MODEL_H
#define XD_COMPOSITOR_EFFECTS_EXPLODE_MODEL_H

#include <stddef.h>
#include <stdint.h>

enum { XD_EXPLODE_MAX_CORNERS = 12 };

typedef enum xd_explode_tessellation : uint8_t {
    XD_EXPLODE_RECTANGULAR,
    XD_EXPLODE_HEXAGONAL,
    XD_EXPLODE_GLASS, /* shards round a point of impact in the middle */
} xd_explode_tessellation;

/* A convex piece; units are the window's px from its top left. */
typedef struct xd_explode_piece {
    int corner_count;
    double corners[XD_EXPLODE_MAX_CORNERS][2];
    double center_x, center_y;
    double direction_x, direction_y; /* the way it flies, a unit vector */
    double speed;                    /* how far, a share of the window's diagonal */
    double axis[3];                  /* what it spins about, a unit vector */
    double spin;                     /* radians, over the whole effect */
    double rise;                     /* toward the viewer, a share of the diagonal */
} xd_explode_piece;

typedef struct xd_explode {
    xd_explode_piece *pieces;
    size_t count;
    double width, height;
} xd_explode;

/* Cuts a width x height window: `across` x `down` rectangles or
 * hexagons, or `spokes` x `tiers` shards; the pieces' motions are
 * random from `seed`. False for an empty window. */
bool xd_explode_init(xd_explode *explode, xd_explode_tessellation tessellation, double width,
                     double height, int across, int down, int spokes, int tiers, uint64_t seed);
void xd_explode_free(xd_explode *explode);
/* Where a piece's corner is at `progress` (0..1), the window's px from its
 * old top left, in perspective. */
void xd_explode_get_corner(const xd_explode *explode, const xd_explode_piece *piece, int corner,
                           double progress, double *x, double *y);
/* The way the piece's face points at `progress` (0, 0, 1 is at the
 * viewer). */
void xd_explode_get_normal(const xd_explode_piece *piece, double progress, double normal[3]);
/* How opaque the pieces are at `progress`. */
double xd_explode_get_alpha(double progress);

#endif
