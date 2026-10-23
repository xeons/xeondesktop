/* SPDX-License-Identifier: MIT
 * Power Options' UPS tab, as Windows 2000's and XP's: the selected UPS's
 * Status (power source, runtime, capacity, battery condition) and
 * Details (manufacturer, model), with UPS Selection (Select...) and UPS
 * Configuration (Configure...) for [UPS], which the shell acts on.
 */
#ifndef XD_POWER_UPSTAB_H
#define XD_POWER_UPSTAB_H

#include <gtk/gtk.h>

#include "../settings/settings.h"

typedef struct xd_power_ups_tab xd_power_ups_tab;

/* The page, showing `settings` (copied). `on_changed(data)` runs when the
 * user changes them; `can_hibernate` offers Hibernate as the critical
 * action. Free the tab after the page is gone. */
xd_power_ups_tab *xd_power_ups_tab_new(GtkWindow *window, const xd_ups_settings *settings,
                                       bool can_hibernate, void (*on_changed)(void *data),
                                       void *data);
GtkWidget *xd_power_ups_tab_get_page(xd_power_ups_tab *tab);
/* The settings as the user left them. */
void xd_power_ups_tab_get_settings(const xd_power_ups_tab *tab, xd_ups_settings *out);
void xd_power_ups_tab_free(xd_power_ups_tab *tab);

#endif
