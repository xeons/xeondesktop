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

/* The actions of XP's Printer menu. Each asks first where XP did (Cancel
 * All Documents, Delete), shows what went wrong, and returns whether it
 * was done. */
bool xd_printer_do_set_default(GtkWindow *parent, const char *name);
bool xd_printer_do_pause(GtkWindow *parent, const char *name, bool paused);
bool xd_printer_do_cancel_all(GtkWindow *parent, const char *name);
bool xd_printer_do_delete(GtkWindow *parent, const char *name);

/* The printer's icon, `size` pixels square at `scale`: a fax or a
 * printer, XP's check mark on the default one, a pause sign on a paused
 * one, greyed when offline. For cell renderers' "surface" and
 * gtk_image_new_from_surface. */
cairo_surface_t *xd_printer_icon(const xd_printer *p, int size, int scale);

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

#endif
