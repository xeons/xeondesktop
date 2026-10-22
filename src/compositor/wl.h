/* SPDX-License-Identifier: MIT
 * Helpers around libwayland's signals, for the compositor.
 */
#ifndef XD_COMPOSITOR_WL_H
#define XD_COMPOSITOR_WL_H

#include <wayland-server-core.h>

#include "../core/wlist.h"

/* Hooks obj->field up to signal with handler fn. */
#define XD_LISTEN(signal, obj, field, fn)                                      \
    do {                                                                       \
        (obj)->field.notify = (fn);                                            \
        wl_signal_add((signal), &(obj)->field);                                \
    } while (0)

/* Removes a listener that may or may not be attached. */
static inline void xd_unlisten(struct wl_listener *listener) {
    if (listener->link.next != nullptr) {
        wl_list_remove(&listener->link);
        listener->link.next = nullptr;
        listener->link.prev = nullptr;
    }
}

#endif
