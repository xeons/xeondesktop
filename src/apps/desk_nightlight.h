/* SPDX-License-Identifier: MIT
 * Display Properties' Night Light dialog (from the Linux tab): turning it
 * on, its strength, and when: all the time, sunset to sunrise (at a place
 * GeoClue finds or the user types), or set hours.
 */
#ifndef XD_APPS_DESK_NIGHTLIGHT_H
#define XD_APPS_DESK_NIGHTLIGHT_H

#include "../settings/settings.h"
#include "../ui/ui.h"

typedef struct nightlight_dialog nightlight_dialog;

/* Runs once the dialog has closed: OK with the settings it ended with,
 * Cancel with nullptr. */
typedef void (*nightlight_done)(const xd_nightlight_settings *s, void *user);

nightlight_dialog *nightlight_dialog_open(xd_ui *ui, const xd_nightlight_settings *current,
                                          nightlight_done done, void *user);
/* Closes it as Cancel does (done runs). */
void nightlight_dialog_cancel(nightlight_dialog *n);

#endif
