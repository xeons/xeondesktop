/* SPDX-License-Identifier: MIT
 * Where a UPS's readings come from: UPower's UPS devices (Type 3: a USB
 * UPS that its HID driver found) and NUT's upsd, through NUT's own client
 * library (libupsclient), on this computer or another.
 *
 * NUT's calls block (on the network, with libupsclient's timeouts): the
 * async ones run them on a worker thread.
 */
#ifndef XD_POWER_UPSSOURCE_H
#define XD_POWER_UPSSOURCE_H

#include <gio/gio.h>

#include "upsstate.h"

#define XD_UPOWER_NAME "org.freedesktop.UPower"
#define XD_UPOWER_PATH "/org/freedesktop/UPower"
#define XD_UPOWER_DEVICE_INTERFACE "org.freedesktop.UPower.Device"

/* A UPS found, for UPS Selection. */
typedef struct xd_ups_found {
    char *device;      /* as [UPS] Device holds it */
    char *description; /* "American Power Conversion Back-UPS 700 (USB)" */
} xd_ups_found;
void xd_ups_found_free(xd_ups_found *found);

/* A UPower device's reading (from its cached properties). */
void xd_ups_read_upower(GDBusProxy *device, xd_ups_reading *reading);
/* UPower's UPSes, added to `found` (of xd_ups_found *). Blocks (D-Bus,
 * a couple of seconds at most). */
void xd_ups_list_upower(GPtrArray *found);

/* NUT's UPSes on `host` ("localhost", "server:3493"), added to `found`.
 * False, with `error` (g_free it) set, if upsd couldn't be asked. Blocks. */
bool xd_ups_list_nut(const char *host, GPtrArray *found, char **error);
/* One reading of NUT's ups@host[:port]. Blocks; an unreachable UPS or
 * server gives a reading with is_reachable false. */
void xd_ups_read_nut(const char *address, xd_ups_reading *reading);
/* The same on a worker thread: `callback` runs in the calling thread's
 * main context, and xd_ups_read_nut_finish gives its reading. */
void xd_ups_read_nut_async(const char *address, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer data);
bool xd_ups_read_nut_finish(GAsyncResult *result, xd_ups_reading *reading);

#endif
