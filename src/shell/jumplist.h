/* SPDX-License-Identifier: MIT
 * Windows 7's jump lists: a program's recent files, from the recently used
 * list (recently-used.xbel, through GLib's GBookmarkFile), and moving
 * items in an ordered list (the taskbar's pinned programs).
 */
#ifndef XD_SHELL_JUMPLIST_H
#define XD_SHELL_JUMPLIST_H

#include <stddef.h>

typedef struct _GBookmarkFile GBookmarkFile;

/* URIs of up to `max` local files that still exist which `recent` says
 * the program used, the one it used last first. The program is its desktop
 * file id and executable (either may be nullptr). g_strfreev the result,
 * which is never nullptr. */
char **xd_jumplist_recent(GBookmarkFile *recent, const char *desktop_id, const char *executable,
                          size_t max);

/* Moves item `from` of `items` (n long) to index `to`, shifting the ones
 * between; false if either is out of range or they're the same. */
bool xd_jumplist_move(void **items, size_t n, size_t from, size_t to);

#endif
