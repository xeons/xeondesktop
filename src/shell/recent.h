/* SPDX-License-Identifier: MIT
 * Recently used files, from ~/.local/share/recently-used.xbel (written by
 * GTK, Qt/KDE and most Linux apps), read with GLib's GBookmarkFile.
 *
 * Privacy's "Remember the files I open" and how long for are GNOME's
 * keys (org.gnome.desktop.privacy remember-recent-files and
 * recent-files-max-age), which GTK honours when it records and lists
 * files; the Start menus and jump lists honour them too.
 */
#ifndef XD_SHELL_RECENT_H
#define XD_SHELL_RECENT_H

#include <stdint.h>

#include "../core/str.h"

/* Up to `max` local files that still exist, most recently used first;
 * none if the privacy settings say not to remember them. Free with
 * xd_free_lines. */
void xd_recent_load(char ***paths, size_t *count, size_t max);

/* The privacy settings: whether recent files are remembered, and for how
 * many days (-1: for ever; 0: not at all). Remembered for ever when the
 * schema isn't installed. */
void xd_recent_privacy(bool *remember, int *max_age_days);

/* Whether a file last used at `used` (Unix seconds) is shown at `now`
 * under those settings. */
bool xd_recent_shown(bool remember, int max_age_days, int64_t used, int64_t now);

#endif
