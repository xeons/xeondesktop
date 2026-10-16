/* SPDX-License-Identifier: MIT
 * Remembered monitor settings (~/.config/xeondesktop/monitors.ini): the
 * compositor saves what Display Properties (or wlr-randr) applied and
 * restores it when the monitor appears again. One group per monitor, keyed
 * by make, model and serial number so it follows the monitor between
 * connectors.
 */
#ifndef XD_SETTINGS_MONITORS_H
#define XD_SETTINGS_MONITORS_H

#include <stddef.h>

typedef struct xd_monitor_config {
    char key[192];
    bool enabled;
    int width, height;   /* mode in pixels; 0 = preferred */
    int refresh_mhz;     /* 0 = any */
    int x, y;
    double scale;        /* 0 = automatic */
    int transform;       /* wl_output_transform */
} xd_monitor_config;

/* "Make Model Serial"; "Make Model on DP-1" without a serial number; the
 * connector name alone when the monitor reports none of them. */
void xd_monitor_key(char *out, size_t cap, const char *make, const char *model,
                    const char *serial, const char *connector);
bool xd_monitors_lookup(const char *key, xd_monitor_config *out);
/* Replaces the stored settings of these monitors, keeping the others. */
bool xd_monitors_store(const xd_monitor_config *configs, size_t count);

#endif
