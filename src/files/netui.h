/* SPDX-License-Identifier: MIT
 * XP's network dialogs for xd-files: Map Network Drive (with Connect As),
 * Disconnect Network Drives, the Add Network Place Wizard, and "Connect
 * to server", the box that asks for a user name and password when a
 * network folder is mounted (GIO's GMountOperation, through gvfs).
 */
#ifndef XD_FILES_NETUI_H
#define XD_FILES_NETUI_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Called with a folder to open once it's connected (Map Network Drive,
 * the Add Network Place Wizard's "Open this network place"). */
typedef void (*XdNetOpenFn)(GFile *location, gpointer data);
/* Called when a mount finished: `error` is nullptr on success; a mount
 * the user cancelled reports G_IO_ERROR_FAILED_HANDLED. */
typedef void (*XdNetMountedFn)(GFile *location, const GError *error, gpointer data);

/* A mount operation whose password questions are XP's "Connect to
 * server" box (other questions are GTK's). `user` and `password`, when
 * given (Connect As), answer the first question; `user` alone fills in
 * the box. */
GMountOperation *xd_net_mount_operation_new(GtkWindow *parent, const char *uri, const char *user,
                                            const char *password);
/* Mounts the network folder `location` is in, asking as above. `done`
 * runs only while `parent` (if any) still exists. */
void xd_net_mount(GtkWindow *parent, GFile *location, const char *user, const char *password,
                  XdNetMountedFn done, gpointer data);

/* Map Network Drive... (`folder`: filled in, or nullptr). `open` runs
 * with the new drive once it's connected, while `parent` exists. A
 * parentless wizard holds `app` (if given) until it closes. */
void xd_net_map_drive(GtkWindow *parent, GApplication *app, const char *folder, XdNetOpenFn open,
                      gpointer data);
/* Disconnect Network Drives: the mapped drives and the network folders
 * mounted now; those chosen are unmounted and forgotten. */
void xd_net_disconnect_dialog(GtkWindow *parent, GApplication *app);
/* Disconnects one drive or mounted share (My Computer's and My Network
 * Places' Disconnect). */
void xd_net_disconnect(GtkWindow *parent, GFile *item);
/* The Add Network Place Wizard. */
void xd_net_add_place(GtkWindow *parent, GApplication *app, XdNetOpenFn open, gpointer data);

G_END_DECLS

#endif
