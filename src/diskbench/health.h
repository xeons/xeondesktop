/* SPDX-License-Identifier: MIT
 * xd-diskbench's Health tab: each drive's S.M.A.R.T. data from UDisks2
 * (Drive.Ata for ATA drives, NVMe.Controller for NVMe ones), its status
 * by the rules in benchlib.h, its details and its attributes or health
 * log.
 */
#ifndef XD_DISKBENCH_HEALTH_H
#define XD_DISKBENCH_HEALTH_H

#include <gtk/gtk.h>

typedef struct health_tab health_tab;

/* The tab, reporting to `status` (the window's status bar). It selects
 * the drive holding `mount_point` or block device `device` when given
 * (either may be nullptr). */
health_tab *health_init_tab(GtkWindow *window, GtkLabel *status, const char *mount_point,
                            const char *device);
GtkWidget *health_get_widget(health_tab *tab);
/* F5: asks UDisks2 to read the selected drive's data again. */
void health_refresh(health_tab *tab);
/* What the tab shows, as plain text for the clipboard. Free with g_free. */
char *health_format_text(health_tab *tab);
void health_free_tab(health_tab *tab);

#endif
