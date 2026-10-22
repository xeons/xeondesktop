/* SPDX-License-Identifier: MIT
 * xd-keys' window and what its three tabs share.
 */
#ifndef XD_KEYS_KEYS_H
#define XD_KEYS_KEYS_H

#include <gtk/gtk.h>

#define KEYS_TITLE "Passwords and Keys"

typedef struct keys_app {
    GtkWidget *window;
    char *askpass; /* this program, as ssh's askpass */
    char *ssh_dir; /* ~/.ssh */
} keys_app;

/* The tabs. */
GtkWidget *passwords_page_new(keys_app *app);
GtkWidget *ssh_page_new(keys_app *app);
GtkWidget *gpg_page_new(keys_app *app);

/* ---- shared widgets (main.c) ---- */

/* A left-aligned, wrapping label. */
GtkWidget *keys_label(const char *text);
/* A list in a sunken scrolled frame, as Windows' list views. */
GtkWidget *keys_scrolled(GtkWidget *child, int width, int height);
/* A list view's text column. */
GtkTreeViewColumn *keys_text_column(GtkWidget *view, const char *title, int column, int width);
/* A row of buttons, right-aligned (XP's below a list). */
GtkWidget *keys_button_row(GtkWidget *first, ...) G_GNUC_NULL_TERMINATED;
GtkWidget *keys_button(const char *mnemonic, GCallback cb, gpointer data);
/* "Label:" (mnemonic, for `field`) and field, on `row` of a grid. */
void keys_grid_row(GtkWidget *grid, int row, const char *label, GtkWidget *field);
GtkWidget *keys_grid(void);
/* A modal dialog with OK (or `ok`'s label) and Cancel; its content box. */
GtkWidget *keys_dialog(GtkWindow *parent, const char *title, const char *ok, GtkWidget **box);
/* A password entry: masked, Enter activating the default button. */
GtkWidget *keys_password_entry(void);

/* An error box: `what` failed, and why. */
void keys_error(GtkWindow *parent, const char *what, const GError *error);
/* Asks for one passphrase (`text` above the box); nullptr if cancelled.
 * Clear and g_free what it returns. */
char *keys_ask_passphrase(GtkWindow *parent, const char *title, const char *text);
/* Whether two new passwords match; says so if they don't. */
bool keys_check_match(GtkWindow *parent, const char *title, GtkWidget *entry, GtkWidget *again);
/* Puts `text` on the clipboard. */
void keys_copy(const char *text);
/* The hourglass over `widget`'s window while something runs. */
void keys_busy(GtkWidget *widget, bool busy);

#endif
