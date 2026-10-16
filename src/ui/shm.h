/* SPDX-License-Identifier: MIT
 * Shared-memory buffers for Wayland clients, drawable with cairo.
 *
 * Each surface keeps a small ring of buffers; a buffer is busy from commit
 * until the compositor releases it, and is never drawn into while busy.
 */
#ifndef XD_UI_SHM_H
#define XD_UI_SHM_H

#include <cairo.h>
#include <wayland-client.h>

#include "../core/common.h"

typedef struct xd_shm_buffer {
    struct wl_buffer *buffer;
    cairo_surface_t *surface;
    void *data;
    size_t size;
    int width, height;
    bool busy;
} xd_shm_buffer;

#define XD_SHM_RING 2

typedef struct xd_shm_ring {
    xd_shm_buffer slots[XD_SHM_RING];
} xd_shm_ring;

/* Returns a free buffer of exactly width x height, or nullptr if all are
 * busy (try again after a release) or allocation failed. */
[[nodiscard]] xd_shm_buffer *xd_shm_ring_acquire(xd_shm_ring *ring, struct wl_shm *shm,
                                                 int width, int height);
void xd_shm_ring_finish(xd_shm_ring *ring);

#endif
