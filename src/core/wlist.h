/* SPDX-License-Identifier: MIT
 * Type-safe iteration over libwayland's intrusive lists.
 *
 * wl_container_of() casts char * straight to the container type, which
 * -Wcast-align=strict rejects. These versions go through void * and name
 * the container type explicitly, so a wrong member name fails to compile.
 */
#ifndef XD_CORE_WLIST_H
#define XD_CORE_WLIST_H

#include <wayland-util.h>

#include "common.h"

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

#endif
