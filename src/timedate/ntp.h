/* SPDX-License-Identifier: MIT
 * Date and Time Properties' Internet Time tab, the parts that don't need
 * GTK: systemd-timesyncd's server lists (org.freedesktop.timesync1) and
 * XP's synchronization messages.
 */
#ifndef XD_TIMEDATE_NTP_H
#define XD_TIMEDATE_NTP_H

#include <glib.h>

/* The servers timesyncd knows, in the order it tries them: the runtime
 * list (SetRuntimeNTPServers), timesyncd.conf's NTP=, the network's
 * (networkd), then FallbackNTP=; each once. Any list may be nullptr.
 * nullptr-terminated; never nullptr. */
char **xd_ntp_server_list(const char *const *runtime, const char *const *link,
                          const char *const *system, const char *const *fallback);

/* When timesyncd received its last answer (NTPMessage's
 * DestinationTimestamp, microseconds since the epoch), or 0 when it has
 * none or `message` isn't an NTPMessage. */
gint64 xd_ntp_message_received(GVariant *message);

/* XP's "The time has been successfully synchronized with time.windows.com
 * on 10/18/2026 at 12:00 PM.", in the locale's short date and time. */
char *xd_ntp_synced_text(const char *server, GDateTime *when);
/* "Next synchronization: 10/25/2026 at 12:00 PM" */
char *xd_ntp_next_text(GDateTime *when);
/* "An error occurred while synchronizing with time.windows.com." and
 * `reason` after it when there is one. */
char *xd_ntp_failed_text(const char *server, const char *reason);

#endif
