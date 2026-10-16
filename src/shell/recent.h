/* SPDX-License-Identifier: MIT
 * Recently used files, from ~/.local/share/recently-used.xbel (written by
 * GTK, Qt/KDE and most Linux apps), read with GLib's GBookmarkFile.
 */
#ifndef XD_SHELL_RECENT_H
#define XD_SHELL_RECENT_H

#include "../core/str.h"

/* Up to `max` local files that still exist, most recently used first.
 * Free with xd_free_lines. */
void xd_recent_load(char ***paths, size_t *count, size_t max);

#endif
