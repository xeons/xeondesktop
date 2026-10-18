/* SPDX-License-Identifier: MIT
 * Windows 7's jump lists: a program's recent files, from the recently used
 * list (recently-used.xbel, through GLib's GBookmarkFile), the items
 * pinned to it (jumplist-pinned, a GKeyFile), moving items in an ordered
 * list (the taskbar's pinned programs), and which program "Combine when
 * taskbar is full" combines next.
 */
#ifndef XD_SHELL_JUMPLIST_H
#define XD_SHELL_JUMPLIST_H

#include <stddef.h>
#include <stdint.h>

typedef struct _GBookmarkFile GBookmarkFile;
typedef struct _GKeyFile GKeyFile;

/* URIs of up to `max` local files that still exist which `recent` says
 * the program used, the one it used last first. The program is its desktop
 * file id and executable (either may be nullptr). g_strfreev the result,
 * which is never nullptr. */
char **xd_jumplist_recent(GBookmarkFile *recent, const char *desktop_id, const char *executable,
                          size_t max);

/* Moves item `from` of `items` (n long) to index `to`, shifting the ones
 * between; false if either is out of range or they're the same. */
bool xd_jumplist_move(void **items, size_t n, size_t from, size_t to);

/* The URIs pinned to a program's jump list, in the order pinned: its
 * group's Pinned list in `kf` (a group per desktop file id). g_strfreev
 * the result, which is never nullptr. */
char **xd_jumplist_pinned(GKeyFile *kf, const char *desktop_id);
/* Pins `uri` at the end of the program's list, or unpins it; false if
 * that changes nothing. A program left with none loses its group. */
bool xd_jumplist_set_pinned(GKeyFile *kf, const char *desktop_id, const char *uri, bool pin);
/* Takes the pinned ones out of `uris` (nullptr-terminated, g_strfreev'd
 * by the caller), as Windows 7 doesn't list a pinned item under Recent. */
void xd_jumplist_drop_pinned(char **uris, char *const *pinned);

/* "Combine when taskbar is full" combines programs one at a time until
 * the buttons fit: the least recently used first (the smallest
 * last_used, its windows' latest activation), among those with two or
 * more windows (combining one window saves nothing) not combined yet.
 * Returns its index, or SIZE_MAX when none is left. */
size_t xd_jumplist_combine_next(const size_t *windows, const uint64_t *last_used,
                                const bool *combined, size_t n);

#endif
