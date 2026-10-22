/* SPDX-License-Identifier: MIT
 * Automatic Updates' schedule and what the last check found, shared by
 * the shell (which runs the check on the schedule and shows the
 * notification area icon and balloon), `xd-wuaucpl --check` (which checks)
 * and the applet (which shows and installs). No GTK, no PackageKit.
 *
 * The check's findings are a small key file in the state directory
 * ($XDG_STATE_HOME/xeondesktop/updates.ini); the shell watches it, so an
 * icon appears when a check finds updates and goes once they're installed.
 */
#ifndef XD_WUAUCPL_STATE_H
#define XD_WUAUCPL_STATE_H

#include <glib.h>

#include "../settings/settings.h"

typedef struct xd_updates_state {
    int64_t checked;   /* when the last check ended (seconds since the epoch); 0: never */
    int software;      /* packages and Flatpaks with an update waiting */
    int firmware;      /* devices with a firmware update waiting */
    int important;     /* of them: security, critical or important ones */
    bool downloaded;   /* the software updates are downloaded, ready to install */
    bool metered;      /* not downloaded: the connection is metered */
    char signature[41]; /* SHA-1 of what's waiting: a new set shows the balloon again */
    char error[256];    /* why the last check failed, or "" */
} xd_updates_state;

/* When the next scheduled check is due (seconds since the epoch, local
 * time in `tz`): the first scheduled time (every day, or every Sunday...,
 * at hour:00) after `last`, the last check. A check missed while the
 * computer was off is then due at once, as XP caught up at the next
 * start. 0 (now) if there never was one; INT64_MAX when turned off. */
int64_t xd_updates_next_check(const xd_update_settings *update_settings, int64_t last,
                              GTimeZone *tz);

/* $XDG_STATE_HOME/xeondesktop/updates.ini. g_free it. */
char *xd_updates_state_path(void);
/* Reads `path`; false (and zeros) when there's none. */
bool xd_updates_state_read(const char *path, xd_updates_state *st);
/* Replaces `path` atomically. */
bool xd_updates_state_write(const char *path, const xd_updates_state *st);

/* The signature of a set of update ids (any order). */
void xd_updates_signature(const char *const *ids, size_t count, char out[41]);

#endif
