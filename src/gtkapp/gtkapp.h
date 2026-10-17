/* SPDX-License-Identifier: MIT
 * Shared setup for XeonDesktop's GTK3 apps (xd-files, Control Panel
 * applets): use the generated XeonDesktop theme and our icon theme, and
 * follow theme changes live.
 */
#ifndef XD_GTKAPP_GTKAPP_H
#define XD_GTKAPP_GTKAPP_H

#include <gtk/gtk.h>

/* Call before GTK starts (before g_application_run). `app_id` (e.g.
 * "org.xeondesktop.Files") becomes the program name, which GTK 3 sends
 * as the Wayland app id: the taskbar finds the desktop file (and icon) by
 * it, and the compositor finds the window. */
void xd_gtk_app_preinit(const char *app_id);
/* Call once GTK is initialized (e.g. in GApplication::startup). */
void xd_gtk_app_follow_theme(void);

/* Ctrl+Tab and Ctrl+Shift+Tab switch the notebook's pages anywhere in
 * `window`, as in Windows property sheets. */
void xd_gtk_notebook_ctrl_tab(GtkWidget *window, GtkNotebook *notebook);

typedef enum { XD_MB_ERROR, XD_MB_WARNING, XD_MB_INFO, XD_MB_QUESTION } xd_mb_icon;
typedef enum { XD_MB_OK, XD_MB_YES_NO } xd_mb_buttons;

/* A Windows-style message box (icon, text, centred buttons; GTK's
 * message dialog looks nothing like it). Returns GTK_RESPONSE_OK, or
 * GTK_RESPONSE_YES / GTK_RESPONSE_NO (Escape and closing count as No).
 * With default_no, No has the focus, for destructive questions. */
int xd_message_box(GtkWindow *parent, const char *title, xd_mb_icon icon, xd_mb_buttons buttons,
                   gboolean default_no, const char *text);

/* A message box with its own buttons (mnemonic labels, left to right).
 * Returns the index of the button pressed; Escape and closing the box
 * return cancel_index. */
int xd_message_box_choose(GtkWindow *parent, const char *title, xd_mb_icon icon,
                          const char *text, const char *const *labels, int n, int default_index,
                          int cancel_index);

#endif
