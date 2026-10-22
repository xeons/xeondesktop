/* SPDX-License-Identifier: MIT
 * xd-snip, the Snipping Tool: what its parts share.
 *
 *   main.c     the toolbar window, Options, the command line
 *   capture.c  the screen (wlr-screencopy), where the windows are
 *              (xd-window-rectangles-v1), and the clipboard (ext-data-control)
 *   overlay.c  choosing what to snip, over the frozen screen (layer-shell)
 *   editor.c   the Mark-up window: pen, highlighter, eraser, save, copy
 */
#ifndef XD_SNIP_SNIP_H
#define XD_SNIP_SNIP_H

#include <gtk/gtk.h>

#include "snipping.h"

#define SNIP_TITLE "Snipping Tool"
#define SNIP_APP_ID "org.xeondesktop.SnippingTool"

typedef struct snip_editor snip_editor;

typedef struct snip_app {
    xd_snip_options options;
    GtkWidget *window; /* the toolbar window */
    GtkWidget *instructions;
    GtkWidget *cancel;
    GtkWidget *mode_items[4]; /* the New menu's, by xd_snip_mode */
    snip_editor *editor; /* once a snip was taken */
    guint delay_source;   /* counting down to a snip */
    bool snipping;        /* from New until the snip is taken or cancelled */
    bool quit_on_cancel;  /* started with --new: nothing else to show */
    bool syncing;         /* marking the saved kind in New's menu */
} snip_app;

/* main.c */
void snip_app_new_snip(snip_app *app, xd_snip_mode mode);
void snip_app_show_options(snip_app *app, GtkWindow *parent);
void snip_app_save_options(snip_app *app);
/* The snip was taken (or nullptr: cancelled); takes `snip`. */
void snip_app_snip_taken(snip_app *app, cairo_surface_t *snip, double scale);

/* capture.c: the whole desktop as it is now, and where its windows are;
 * nullptr (and `error`) when the compositor won't let us. */
xd_snip_screen *snip_capture_screen(GdkDisplay *display, GError **error);
/* Puts a PNG on the clipboard and keeps serving it, without a window,
 * until another program takes the clipboard; then calls gtk_main_quit. */
bool snip_clipboard_serve(GdkDisplay *display, GBytes *png, GError **error);
/* Hands a snip to a new `xd-snip --copy-stdin`, which keeps it on the
 * clipboard after we're gone. */
void snip_clipboard_copy(cairo_surface_t *snip);
/* A snip as PNG data. */
GBytes *snip_png_bytes(cairo_surface_t *snip);

/* overlay.c: lets the user choose what to snip of `screen` (which it
 * takes), then calls snip_app_snip_taken. */
void snip_overlay_start(snip_app *app, xd_snip_screen *screen, xd_snip_mode mode);

/* editor.c */
snip_editor *snip_editor_new(snip_app *app);
/* Shows `snip` (taken) in it, ink cleared, and brings it forward. */
void snip_editor_show(snip_editor *editor, cairo_surface_t *snip, double scale);
/* Shows it again with what it had (a new snip was cancelled). */
void snip_editor_present(snip_editor *editor);
void snip_editor_hide(snip_editor *editor);
/* Asks to save an unsaved snip first; false if the user cancelled. */
bool snip_editor_may_discard(snip_editor *editor);

#endif
