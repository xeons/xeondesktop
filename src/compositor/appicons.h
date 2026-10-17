/* SPDX-License-Identifier: MIT
 * Title bar icons: each window's application icon, from its app id as the
 * taskbar finds it, loaded in a thread of its own. Icon theme lookups and
 * image decoding (through sandboxed loaders on some systems) are far too
 * slow for the compositor's loop.
 */
#ifndef XD_COMPOSITOR_APPICONS_H
#define XD_COMPOSITOR_APPICONS_H

#include <cairo.h>
#include <wayland-server-core.h>

typedef struct xd_appicons xd_appicons;

/* Called on the compositor's loop when `app_id`'s icon has loaded. */
typedef void (*xd_appicons_ready_fn)(void *data, const char *app_id);

/* `theme`: the icon theme, as the shell's. */
[[nodiscard]] xd_appicons *xd_appicons_create(struct wl_event_loop *loop, const char *theme,
                                              xd_appicons_ready_fn ready, void *data);
void xd_appicons_destroy(xd_appicons *ai);

/* The icon for windows of `app_id` (nullptr or "" for none), px x px device
 * px. Owned by the cache and kept until it's destroyed, so the same app
 * and size always give the same surface. nullptr while it loads (`ready`
 * follows) or if the theme has no icon at all. */
cairo_surface_t *xd_appicons_get(xd_appicons *ai, const char *app_id, int px);

#endif
