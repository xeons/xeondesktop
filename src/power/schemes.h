/* SPDX-License-Identifier: MIT
 * Power Options' power schemes: XP's six, as the user changed them, and
 * the user's own (Save As). Kept in ~/.config/xeondesktop/power-schemes.ini,
 * one group per scheme by its name:
 *
 *   [Portable/Laptop]
 *   MonitorOff=15          minutes plugged in; 0 = never
 *   Standby=20
 *   Hibernate=180
 *   MonitorOffOnBattery=5  and running on batteries
 *   StandbyOnBattery=5
 *   HibernateOnBattery=120
 *
 *   [Max Battery]
 *   Deleted=true           a built-in scheme the user deleted
 *
 * The timers in use are desktop.ini's ([Power], [ScreenSaver] MonitorOff),
 * which the shell reads; this file only remembers the schemes to choose
 * from.
 */
#ifndef XD_POWER_SCHEMES_H
#define XD_POWER_SCHEMES_H

#include <glib.h>

#include "../settings/settings.h"

typedef struct xd_power_timers {
    int monitor, standby, hibernate; /* minutes; 0 = never */
} xd_power_timers;

typedef struct xd_power_scheme_def {
    char *name;
    xd_power_timers ac, dc; /* plugged in, running on batteries */
    int builtin;            /* its xd_power_scheme, or -1 for the user's own */
} xd_power_scheme_def;

/* XP's name of a built-in scheme ("Home/Office Desk"). */
const char *xd_power_builtin_label(xd_power_scheme scheme);

/* Whether `name` can name a scheme: not empty, not too long, printable,
 * and without the brackets a key file's group names can't hold. */
bool xd_power_scheme_name_valid(const char *name);

/* The schemes: the built-in ones the user hasn't deleted, as changed,
 * then the user's own in the file's order. A missing or unreadable file
 * gives XP's six. Array of xd_power_scheme_def *, freeing them. */
GPtrArray *xd_power_schemes_load(const char *path);
/* Writes them back (every scheme, and the deleted built-in ones). */
bool xd_power_schemes_save(const char *path, GPtrArray *schemes, GError **error);
/* By name, or nullptr. */
xd_power_scheme_def *xd_power_schemes_find(GPtrArray *schemes, const char *name);
/* Adds a scheme (or replaces the timers of one so named); returns it. */
xd_power_scheme_def *xd_power_schemes_put(GPtrArray *schemes, const char *name,
                                          xd_power_timers ac, xd_power_timers dc);

#endif
