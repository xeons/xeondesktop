/* SPDX-License-Identifier: MIT
 * Display Properties' Advanced Appearance: Windows 2000/XP's editor of
 * the colour scheme item by item.
 */
#ifndef XD_APPS_DESK_ADVANCED_H
#define XD_APPS_DESK_ADVANCED_H

#include "../settings/settings.h"
#include "../ui/ui.h"

typedef struct advanced advanced;

/* Runs once the dialog has closed: OK with the scheme's changes as they
 * now stand (those `pending` had, and the new ones), Cancel with nullptr. */
typedef void (*advanced_done)(const xd_scheme_custom *custom, void *user);

/* Opens it on the appearance `pending` describes (style, scheme, font
 * size and its [CustomScheme]). */
advanced *advanced_open(xd_ui *ui, const xd_settings *pending, advanced_done done, void *user);
/* Closes it as Cancel does (done runs). */
void advanced_cancel(advanced *appearance);

#endif
