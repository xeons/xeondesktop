/* SPDX-License-Identifier: MIT
 * Window mirrors, for the scene-graph effects (Scale, Ring Switcher,
 * Animations) in both compositor builds: a window's picture drawn again
 * elsewhere in the scene, moved, scaled and faded, while the window
 * itself stays where it is (or is hidden).
 *
 * A mirror is a tree of scene buffers showing the same buffers as the
 * window's frame and surfaces (subsurfaces and popups too), copied from
 * the window's own nodes by xd_mirror_update: live while it's attached,
 * the last picture once it's detached (the window closed). The scene
 * buffers hold their buffers, so a closed window's last frame stays.
 */
#ifndef XD_COMPOSITOR_EFFECTS_MIRROR_H
#define XD_COMPOSITOR_EFFECTS_MIRROR_H

#include <time.h>

#include "../server.h"

typedef struct xd_mirror xd_mirror;

/* A mirror of `view` in `parent`, showing what it shows now, placed
 * over it at scale 1. */
xd_mirror *xd_mirror_create(xd_view *view, struct wlr_scene_tree *parent);
void xd_mirror_destroy(xd_mirror *mirror);
/* Copies what the window shows now, if it's still attached and changed
 * (it committed, or its frame was redrawn). */
void xd_mirror_update(xd_mirror *mirror);
/* Lets go of the window, keeping its last picture (it's closing). Done
 * by itself when the window's surface goes. */
void xd_mirror_detach(xd_mirror *mirror);
/* The window, or nullptr once detached. */
xd_view *xd_mirror_get_view(const xd_mirror *mirror);
/* The window's outer box when last updated, layout coordinates. */
struct wlr_box xd_mirror_get_box(const xd_mirror *mirror);
struct wlr_scene_tree *xd_mirror_get_tree(const xd_mirror *mirror);
/* Draws the window's outer box at (x, y), its size multiplied by
 * scale_x and scale_y. */
void xd_mirror_place(xd_mirror *mirror, double x, double y, double scale_x, double scale_y);
void xd_mirror_set_opacity(xd_mirror *mirror, float opacity);
/* While the window itself is hidden its program gets no frame events
 * from the scene: these keep it drawing. */
void xd_mirror_send_frame_done(xd_mirror *mirror, const struct timespec *now);

#endif
