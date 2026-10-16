/* SPDX-License-Identifier: MIT
 * Helpers around libwayland's intrusive lists and signals.
 *
 * wl_container_of() casts char * straight to the container type, which
 * -Wcast-align=strict rejects. These versions go through void * and name
 * the container type explicitly, so a wrong member name fails to compile.
 */
#ifndef XD_COMPOSITOR_WL_H
#define XD_COMPOSITOR_WL_H

#include <wayland-server-core.h>

#include "../core/common.h"

#define XD_CONTAINER_OF(ptr, type, member)                                     \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#define XD_LIST_FOR_EACH(pos, head, type, member)                              \
    for (type *pos = XD_CONTAINER_OF((head)->next, type, member);              \
         &pos->member != (head);                                               \
         pos = XD_CONTAINER_OF(pos->member.next, type, member))

/* Safe against removal of `pos` inside the loop body. */
#define XD_LIST_FOR_EACH_SAFE(pos, tmp, head, type, member)                    \
    for (type *pos = XD_CONTAINER_OF((head)->next, type, member),              \
              *tmp = XD_CONTAINER_OF(pos->member.next, type, member);          \
         &pos->member != (head);                                               \
         pos = tmp, tmp = XD_CONTAINER_OF(pos->member.next, type, member))

/* Hooks obj->field up to signal with handler fn. */
#define XD_LISTEN(signal, obj, field, fn)                                      \
    do {                                                                       \
        (obj)->field.notify = (fn);                                            \
        wl_signal_add((signal), &(obj)->field);                                \
    } while (0)

/* Removes a listener that may or may not be attached. */
static inline void xd_unlisten(struct wl_listener *l) {
    if (l->link.next != nullptr) {
        wl_list_remove(&l->link);
        l->link.next = nullptr;
        l->link.prev = nullptr;
    }
}

#endif
