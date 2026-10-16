/* SPDX-License-Identifier: MIT
 * Shared setup for XeonDesktop's GTK3 apps (xd-files, Control Panel
 * applets): use the generated XeonDesktop theme and our icon theme, and
 * follow theme changes live.
 */
#ifndef XD_GTKAPP_GTKAPP_H
#define XD_GTKAPP_GTKAPP_H

#include <gtk/gtk.h>

/* Call before GTK starts (before g_application_run). */
void xd_gtk_app_preinit(void);
/* Call once GTK is initialized (e.g. in GApplication::startup). */
void xd_gtk_app_follow_theme(void);

#endif
