/* SPDX-License-Identifier: MIT
 * xd-printers' windows: the Printers and Faxes folder (main.c), a
 * printer's queue (queue.c), its Properties and Printing Preferences
 * (props.c) and the Add Printer Wizard (addwizard.c), and the actions
 * they share (actions.c).
 */
#ifndef XD_PRINTERS_PRINTERS_H
#define XD_PRINTERS_PRINTERS_H

#include <gtk/gtk.h>

#include "cupsops.h"

/* Something changed (a printer added, paused, a document cancelled):
 * the open windows refresh now rather than at their next poll. */
typedef void (*xd_printers_watch_fn)(gpointer data);
void xd_printers_watch(xd_printers_watch_fn fn, gpointer data);
void xd_printers_unwatch(xd_printers_watch_fn fn, gpointer data);
void xd_printers_changed(void);

/* An error box for `error`, unless it says the password box was
 * cancelled. */
void xd_printers_error(GtkWindow *parent, const GError *error);

/* A change to make in a worker thread (the scheduler may take its time,
 * or want a password), and what to do once it's done, on the main
 * thread. */
typedef bool (*xd_printers_change_fn)(gpointer data, GError **error);
typedef void (*xd_printers_done_fn)(bool ok, gpointer user);
/* Runs fn(data) in a worker, then frees data with `free`, shows what went
 * wrong over `parent` (if still open), tells the windows
 * (xd_printers_changed) and calls done(ok, user) if given. The program
 * waits for it. */
void xd_printers_run(GtkWindow *parent, xd_printers_change_fn fn, gpointer data,
                     GDestroyNotify free, xd_printers_done_fn done, gpointer user);

/* The actions of XP's Printer menu, made with xd_printers_run. Each asks
 * first where XP did (Cancel All Documents, Delete). */
void xd_printer_do_set_default(GtkWindow *parent, const char *name);
void xd_printer_do_pause(GtkWindow *parent, const char *name, bool paused,
                         xd_printers_done_fn done, gpointer user);
void xd_printer_do_cancel_all(GtkWindow *parent, const char *name);
void xd_printer_do_delete(GtkWindow *parent, const char *name);

/* The printer's icon, `size` pixels square at `scale`: a fax or a
 * printer, XP's check mark on the default one, a pause sign on a paused
 * one, greyed when offline. For cell renderers' "surface" and
 * gtk_image_new_from_surface. */
cairo_surface_t *xd_printer_icon(const xd_printer *printer_info, int size, int scale);

/* Periodic refresh of the open windows, in milliseconds. */
static constexpr guint XD_PRINTERS_POLL_MS = 2500;

void xd_print_queue_open(GtkWindow *parent, const char *name);
void xd_printer_properties_open(GtkWindow *parent, const char *name);
void xd_printer_preferences_open(GtkWindow *parent, const char *name);
/* A new printer calls xd_printers_changed. */
void xd_add_printer_wizard_open(GtkWindow *parent);

/* Each window counts itself in while open; the program ends with the
 * last one. */
void xd_printers_hold(GtkWidget *window);
bool xd_printers_any_open(void);
/* The same for a window still being fetched (Properties, Printing
 * Preferences): the program waits for it. */
void xd_printers_hold_begin(void);
void xd_printers_hold_end(void);

#endif
