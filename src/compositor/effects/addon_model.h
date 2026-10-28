/* SPDX-License-Identifier: MIT
 * The Animations Add-On's maths: the effects of Compiz Fusion's
 * animationaddon plugin we draw (Airplane, Beam Up, Domino, Razr, Fold,
 * Glide 3, Leaf Spread, Skewer), reimplemented from how they looked.
 * Each cuts the window into flat pieces that turn about hinges or fly
 * off in 3D, seen in perspective; Beam Up adds beams of light. Pure, no
 * wlroots or GL, so it's unit-tested, and the same from the same seed.
 *
 * Units are the window's px from its top left, z toward the viewer.
 * `progress` runs from 0 (the window whole, where it is) to 1 (gone); a
 * window appearing (opened, restored) runs it backwards, as Compiz played
 * its closing animations in reverse.
 */
#ifndef XD_COMPOSITOR_EFFECTS_ADDON_MODEL_H
#define XD_COMPOSITOR_EFFECTS_ADDON_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "explode_model.h"
#include "geometry.h"

enum { XD_ADDON_MAX_CORNERS = XD_EXPLODE_MAX_CORNERS, XD_ADDON_MAX_HINGES = 20 };

typedef enum xd_addon_effect : uint8_t {
    XD_ADDON_AIRPLANE,
    XD_ADDON_BEAM_UP,
    XD_ADDON_DOMINO,
    XD_ADDON_FOLD,
    XD_ADDON_GLIDE_3,
    XD_ADDON_LEAF_SPREAD,
    XD_ADDON_RAZR,
    XD_ADDON_SKEWER,
    XD_ADDON_EFFECT_COUNT,
} xd_addon_effect;

/* Which way pieces go: Domino and Razr take the first four (and Random),
 * Skewer all of them. */
typedef enum xd_addon_direction : uint8_t {
    XD_ADDON_UP,
    XD_ADDON_DOWN,
    XD_ADDON_LEFT,
    XD_ADDON_RIGHT,
    XD_ADDON_IN,  /* away from the viewer */
    XD_ADDON_OUT, /* toward the viewer */
    XD_ADDON_LEFT_RIGHT,
    XD_ADDON_UP_DOWN,
    XD_ADDON_IN_OUT,
    XD_ADDON_RANDOM,
} xd_addon_direction;

/* The Add-On's options (Desktop Effects' Animations Add-On page). */
typedef struct xd_addon_options {
    double airplane_path_length; /* how far it flies, 1 the usual */
    bool airplane_fly_to_taskbar;
    double beam_size;    /* a beam's width, px */
    double beam_spacing; /* between beams, px */
    double beam_life;    /* a beam's life, a share of the animation */
    double beam_slowdown;
    xd_addon_direction domino_direction, razr_direction;
    double domino_piece_size, razr_piece_size; /* px */
    int fold_columns, fold_rows;
    int fold_count; /* how many of the grid's lines it folds along */
    bool fold_out;  /* toward the viewer, rather than away */
    double glide_away_position; /* how far it goes back, in window heights */
    double glide_away_angle;    /* how far it tips, degrees */
    double glide_thickness;     /* px */
    double leaf_spread;
    xd_addon_direction skewer_direction;
    xd_explode_tessellation skewer_tessellation;
    int skewer_across, skewer_down;
    double skewer_thickness; /* px */
    double skewer_rotation;  /* degrees, about the way a piece flies */
    double skewer_depth;     /* how far pieces part toward and away from the viewer */
} xd_addon_options;

void xd_addon_get_default_options(xd_addon_options *options);
/* Within the options' ranges (Desktop Effects clamps them too; this is
 * for values set any other way). */
void xd_addon_clamp_options(xd_addon_options *options);

/* A fold or turn: the piece turns by `angle` (radians) about the line
 * through `point` along the unit `axis`, from `start` to `end` of the
 * animation (shares of it), easing in and out, and is lifted `lift` px
 * toward the viewer by then (so pieces folded flat onto others draw over
 * them). Hinges are in the window's own frame, applied in order. */
typedef struct xd_addon_hinge {
    double point[3], axis[3];
    double angle, start, end, lift;
} xd_addon_hinge;

typedef struct xd_addon_piece {
    int corner_count;
    double corners[XD_ADDON_MAX_CORNERS][2]; /* where it is, whole; convex */
    double center_x, center_y;
    xd_addon_hinge hinges[XD_ADDON_MAX_HINGES];
    int hinge_count;
    /* Flying off (Leaf Spread, Skewer, Domino's drop): when it starts and
     * stops (shares of the animation), the way (a unit vector), how far
     * (px), and its spin about `axis` (radians). */
    double start, end;
    double direction[3];
    double distance;
    double axis[3];
    double spin;
    double sway; /* Leaf Spread: the phase of its sway */
} xd_addon_piece;

/* Beam Up's beams: a streak of light rising from where it's lit. */
typedef struct xd_addon_beam {
    double x, y;   /* its middle across, and its foot, where it's lit */
    double length; /* px */
    double start;  /* when it's lit, a share of the animation */
    double life;   /* how long it lasts, a share of the animation */
    double speed;  /* px it rises over the whole animation, before slowing */
    double brightness;
} xd_addon_beam;

typedef struct xd_addon {
    xd_addon_effect effect;
    double width, height;
    xd_addon_options options;
    xd_addon_direction direction; /* Random resolved */
    xd_addon_piece *pieces;
    size_t count;
    xd_addon_beam *beams;
    size_t beam_count;
    double thickness; /* of the pieces, px */
    /* The viewer's eye: in front of (x, y) at distance z. */
    double eye[3];
    bool has_target; /* Airplane flies into it, minimizing */
    xd_effect_rect target;
} xd_addon;

/* Cuts a width x height window for `effect`; `target` (the taskbar
 * button, in the window's px) or nullptr. The eye is in front of the
 * window's middle until xd_addon_set_eye. False for an empty window. */
bool xd_addon_init(xd_addon *addon, xd_addon_effect effect, double width, double height,
                   const xd_addon_options *options, const xd_effect_rect *target, uint64_t seed);
void xd_addon_free(xd_addon *addon);
/* Compiz looked at the whole screen: the eye in front of the output's
 * middle (x, y in the window's px) at `distance`. */
void xd_addon_set_eye(xd_addon *addon, double x, double y, double distance);
/* The eye's distance Compiz had for an output `height` px high: its 60
 * degree field of view. */
double xd_addon_get_eye_distance(double height);

/* A piece at `progress`: its corners (window px, z toward the viewer),
 * which way its face points, how opaque each corner is, and how far its
 * colour has gone over to Beam Up's light (0..1). */
typedef struct xd_addon_pose {
    double corners[XD_ADDON_MAX_CORNERS][3];
    double normal[3];
    double alpha[XD_ADDON_MAX_CORNERS];
    double glow;
} xd_addon_pose;

void xd_addon_get_pose(const xd_addon *addon, size_t piece, double progress, xd_addon_pose *pose);
/* A point in perspective: where it shows (window px) and how much it's
 * grown (1 at z = 0; textures are divided by it to stay true). */
void xd_addon_project(const xd_addon *addon, const double point[3], double *x, double *y,
                      double *grow);

typedef struct xd_addon_beam_state {
    double x, top, width, height; /* window px */
    double brightness;            /* 0..1 */
} xd_addon_beam_state;

/* A beam at `progress`; false while it isn't lit. */
bool xd_addon_get_beam(const xd_addon *addon, size_t beam, double progress,
                       xd_addon_beam_state *state);

#endif
