/* SPDX-License-Identifier: MIT
 * A UPS as Power Options' UPS tab shows it and the shell watches it,
 * whichever reports it (UPower or NUT): what it says, in the tab's words,
 * and the UPS service's decisions (when to notify, when the critical
 * alarm goes off), as pure functions of the time.
 */
#ifndef XD_POWER_UPSSTATE_H
#define XD_POWER_UPSSTATE_H

#include <stddef.h>
#include <stdint.h>

#include "../settings/settings.h"

/* What [UPS] Device names. */
typedef enum xd_ups_device_kind : uint8_t {
    XD_UPS_DEVICE_NONE,
    XD_UPS_DEVICE_UPOWER, /* "upower:/org/freedesktop/UPower/devices/ups_hiddev0" */
    XD_UPS_DEVICE_NUT,    /* "nut:myups@localhost" */
} xd_ups_device_kind;

/* Its kind; `address` (may be nullptr) gets what follows the prefix. An
 * unknown prefix or an empty address is none. */
xd_ups_device_kind xd_ups_get_device_kind(const char *device, const char **address);

/* One reading of the UPS. */
typedef struct xd_ups_reading {
    bool is_reachable;      /* the UPS (or its server) answered */
    bool is_on_battery;     /* NUT's OB; UPower's Discharging */
    bool is_low_battery;    /* NUT's LB; UPower's WarningLevel Low or worse */
    bool is_replace_battery; /* NUT's RB */
    bool has_condition;     /* it reports the battery's condition (NUT does) */
    double charge;          /* percent; < 0 unknown */
    int runtime;            /* seconds; < 0 unknown */
    char manufacturer[64];
    char model[64];
} xd_ups_reading;

/* An unreachable reading, nothing known. */
void xd_ups_reading_init(xd_ups_reading *reading);

/* NUT's ups.status ("OL", "OB DISCHRG LB", "OL CHRG RB"...) into the
 * reading's flags. */
void xd_ups_parse_nut_status(const char *status, xd_ups_reading *reading);

/* The tab's Status values ("AC Power", "37 minutes", "100%", "Good"),
 * written into `out`. */
void xd_ups_format_power_source(const xd_ups_reading *reading, char *out, size_t cap);
void xd_ups_format_runtime(const xd_ups_reading *reading, char *out, size_t cap);
void xd_ups_format_capacity(const xd_ups_reading *reading, char *out, size_t cap);
void xd_ups_format_condition(const xd_ups_reading *reading, char *out, size_t cap);

/* The UPS service's state between readings. Zero it to start. */
typedef struct xd_ups_watch {
    bool is_on_battery;
    bool has_notified;      /* a power failure notification went out */
    bool is_critical_done;  /* the critical alarm went off on this failure */
    int64_t failed_at;      /* seconds, when the power failed */
    int64_t next_notice_at; /* seconds, when the next notification is due */
} xd_ups_watch;

/* What the shell does now (xd_ups_watch_step's result, flags). */
enum {
    XD_UPS_EVENT_POWER_FAILED = 1 << 0,   /* a power failure notification */
    XD_UPS_EVENT_POWER_RESTORED = 1 << 1, /* power restored (after a notification) */
    XD_UPS_EVENT_CRITICAL = 1 << 2,       /* run the program, then shut down or hibernate */
};

/* The service's decision at `now` (seconds, monotonic) on `reading`.
 * `wake_at` gets when to look again without a new reading (the next
 * notification, or the minutes on battery running out), or -1. An
 * unreachable UPS changes nothing. */
unsigned xd_ups_watch_step(const xd_ups_settings *settings, xd_ups_watch *watch,
                           const xd_ups_reading *reading, int64_t now, int64_t *wake_at);

#endif
