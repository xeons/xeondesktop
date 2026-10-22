/* SPDX-License-Identifier: MIT
 * xd-snip, the Snipping Tool: what its parts share.
 *
 *   main.c     the toolbar window, Options, the command line
 *   capture.c  the screen (wlr-screencopy), where the windows are
 *              (xd-window-rectangles-v1), and the clipboard (ext-data-control)
 *   overlay.c  choosing what to snip, over the frozen screen (layer-shell)
 *   editor.c   the Mark-up window: pen, highlighter, eraser, save, copy
 *   record.c   screen recording (wlr-screencopy into GStreamer)
 */
#ifndef XD_SNIP_SNIP_H
#define XD_SNIP_SNIP_H

#include <gtk/gtk.h>

#include "snipping.h"

#define SNIP_TITLE "Snipping Tool"
#define SNIP_APP_ID "org.xeondesktop.SnippingTool"
/* Where a running recording is found, to stop it. */
#define SNIP_RECORDER_NAME "org.xeondesktop.ScreenRecorder"
#define SNIP_RECORDER_PATH "/org/xeondesktop/ScreenRecorder"

typedef struct snip_editor snip_editor;
typedef struct snip_recording snip_recording;

typedef struct snip_app {
    xd_snip_options options;
    GtkWidget *window; /* the toolbar window */
    GtkWidget *instructions;
    GtkWidget *cancel;
    GtkWidget *mode_items[4]; /* the New menu's, by xd_snip_mode */
    snip_recording *recording; /* while one runs */
    bool choosing_area;   /* the overlay is for a recording */
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

/* The area to record was chosen (desktop coordinates), or nullptr:
 * cancelled. */
void snip_app_area_chosen(snip_app *app, const xd_snip_box *area);

/* overlay.c: lets the user choose what to snip of `screen` (which it
 * takes), then calls snip_app_snip_taken; or, with app->choosing_area,
 * the area to record (a rectangle dragged out, or the monitor clicked),
 * then snip_app_area_chosen. */
void snip_overlay_start(snip_app *app, xd_snip_screen *screen, xd_snip_mode mode);

/* record.c. Called once the recording is over: the file it's in, or
 * nullptr and why not. It mustn't free the recording (do it later, from
 * the main loop). */
typedef void (*snip_recording_done)(snip_recording *recording, const char *path,
                                    const char *error, void *data);
/* Whether GStreamer has what a recording with that sound needs. */
bool snip_recording_possible(xd_record_sound sound, GError **error);
/* Records `area` (desktop coordinates, within `monitor`) until stopped. */
snip_recording *snip_recording_start(snip_app *app, GdkMonitor *monitor, xd_snip_box area,
                                     xd_record_sound sound, snip_recording_done done,
                                     void *done_data, GError **error);
/* Ends it: the file is finished, then `done` is called. */
void snip_recording_stop(snip_recording *recording);
void snip_recording_free(snip_recording *recording);
/* Stops the recording another xd-snip is making; false if none is. */
bool snip_recording_stop_running(void);
/* Tells the user where the recording went, offering to open it; calls
 * `gone` once the notification is done with. */
void snip_recording_notify_saved(const char *path, GSourceFunc gone, gpointer gone_data);

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
