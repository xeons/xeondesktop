/* SPDX-License-Identifier: MIT
 * Which application a window belongs to, from its app id.
 */
#ifndef XD_UI_APPID_H
#define XD_UI_APPID_H

/* The application a window belongs to, found from its app id (Wayland)
 * or WM_CLASS (X11): the desktop file of that name, else one whose
 * StartupWMClass, id or executable matches regardless of case, else the
 * reverse-DNS id's last part ("org.gnome.Nautilus" -> "nautilus"). Sets
 * its Icon key (theme name or path) and localized Name, or nullptr; g_free
 * both. Returns whether one was found. */
bool xd_app_for_window(const char *app_id, char **icon, char **name);

#endif
