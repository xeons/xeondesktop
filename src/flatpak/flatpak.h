/* SPDX-License-Identifier: MIT
 * xd-flatpak's parts: the Applications tab and its permissions dialog
 * (main.c, appperms.c) and the Remotes tab (remotes.c).
 */
#ifndef XD_FLATPAK_FLATPAK_H
#define XD_FLATPAK_FLATPAK_H

#include <flatpak.h>
#include <gtk/gtk.h>

#include "../gtkapp/gtkapp.h"

#define XD_FLATPAK_TITLE "Flatpak"

/* An installed app, as the Applications tab lists it. */
typedef struct xd_fp_app {
    char *id, *name, *branch, *arch;
    GIcon *icon;               /* from its exported desktop file, or nullptr */
    FlatpakInstallation *inst; /* where it's installed */
    FlatpakInstalledRef *ref;
} xd_fp_app;

void xd_fp_app_free(xd_fp_app *app);

/* The user installation, then the system ones. */
GPtrArray *xd_fp_installations(void);
/* "Just me", "All users", or a system installation's own name. */
const char *xd_fp_installation_label(FlatpakInstallation *inst);
/* The user's override file for the app. g_free it. */
char *xd_fp_user_override_path(const char *app_id);

/* The app's permissions dialog, modal over `parent`; true if the user's
 * override was saved (or removed). */
bool xd_fp_app_dialog(GtkWindow *parent, const xd_fp_app *app);

/* The Remotes tab. */
GtkWidget *xd_fp_remotes_page(GtkWidget *window);

/* A labelled frame, as XP's group boxes. */
GtkWidget *xd_fp_frame(const char *title, GtkWidget *child);
/* A wrapping, left-aligned label. */
GtkWidget *xd_fp_label(const char *text);
/* A list in a sunken scrolled window of the given height. */
GtkWidget *xd_fp_scrolled(GtkWidget *view, int height);
/* A text column. */
GtkTreeViewColumn *xd_fp_text_column(GtkWidget *view, const char *title, int column,
                                     int weight_column);
/* A button of XP's size. */
GtkWidget *xd_fp_button(const char *mnemonic);
/* An error box: `what` and GLib's message. */
void xd_fp_error(GtkWindow *parent, const char *what, const GError *error);

#endif
