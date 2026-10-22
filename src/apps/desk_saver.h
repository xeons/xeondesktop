/* SPDX-License-Identifier: MIT
 * Display Properties' screen saver Settings dialogs: Marquee Setup,
 * Starfield Simulation Setup, and the others, one per saver.
 */
#ifndef XD_APPS_DESK_SAVER_H
#define XD_APPS_DESK_SAVER_H

#include "../settings/settings.h"
#include "../ui/ui.h"

typedef struct saver_setup saver_setup;

/* Runs once the dialog has closed: OK with the settings it ended with,
 * Cancel with nullptr. */
typedef void (*saver_setup_done)(const xd_saver_settings *saver_settings, void *user);

/* Opens saver `id`'s Settings dialog on `current`; nullptr if the saver
 * has none. */
saver_setup *saver_setup_open(xd_ui *ui, const char *id, const xd_saver_settings *current,
                              saver_setup_done done, void *user);
/* Closes it as Cancel does (done runs). */
void saver_setup_cancel(saver_setup *setup);

#endif
