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
#include <stdint.h>

/* Variable refresh rate (adaptive sync; AdaptiveSync=): off by default, as
 * in Windows, since some panels flicker with it. On, it's used while a
 * full-screen window is in front, as Windows 10 and 11 use it for games
 * and KDE's "Automatic" does; "always" keeps it on for the desktop too, as
 * sway's adaptive_sync does (set in monitors.ini by hand). */
typedef enum xd_vrr_mode : uint8_t {
    XD_VRR_OFF,
    XD_VRR_FULLSCREEN,
    XD_VRR_ALWAYS,
} xd_vrr_mode;

typedef struct xd_monitor_config {
    char key[192];
    bool enabled;
    int width, height;   /* mode in pixels; 0 = preferred */
    int refresh_mhz;     /* 0 = any */
    int x, y;
    double scale;        /* 0 = automatic */
    int transform;       /* wl_output_transform */
    xd_vrr_mode vrr;
} xd_monitor_config;

/* "Make Model Serial"; "Make Model on DP-1" without a serial number; the
 * connector name alone when the monitor reports none of them. */
void xd_monitor_key(char *out, size_t cap, const char *make, const char *model,
                    const char *serial, const char *connector);
/* AdaptiveSync's values: "off", "fullscreen" or "always"; a boolean too
 * (true is "fullscreen"). Anything else is off. */
xd_vrr_mode xd_vrr_mode_parse(const char *s);
const char *xd_vrr_mode_name(xd_vrr_mode mode);
bool xd_monitors_lookup(const char *key, xd_monitor_config *out);
/* Replaces the stored settings of these monitors, keeping the others. */
bool xd_monitors_store(const xd_monitor_config *configs, size_t count);

#endif
