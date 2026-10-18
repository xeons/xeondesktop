/* SPDX-License-Identifier: MIT
 * xd-sysdm: System Properties (Windows 2000's sysdm.cpl), with Linux
 * where Windows had Windows: the distribution and kernel, the hostname
 * (systemd-hostnamed), devices from udev, kernel modules, Secure Boot,
 * swap, environment.d, and the boot loader.
 */
#ifndef XD_SYSDM_H
#define XD_SYSDM_H

#include "../gtkapp/gtkapp.h"

typedef struct sysdm {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *apply;
    GDBusProxy *hostname1; /* org.freedesktop.hostname1, or nullptr */
    GCancellable *calls;   /* hostname1 calls; cancelled when the window goes */
    gboolean applying;     /* the description is being set */

    /* Network Identification */
    GtkWidget *full_name;
    GtkWidget *description;
    gboolean description_dirty;
    GtkWidget *domain;
    GtkWidget *chassis;

    /* User Profiles */
    GtkListStore *profiles;
    GCancellable *sizing;
} sysdm;

/* ---- shared helpers (main.c) ---- */

/* A left-aligned, wrapping label; markup if `markup`. */
GtkWidget *sysdm_label(const char *text);
/* An etched group box ("frame") with `child` inside. */
GtkWidget *sysdm_group(const char *title, GtkWidget *child);
/* An icon, a description and buttons below it: a Windows 2000 group. */
GtkWidget *sysdm_group_with_icon(const char *title, const char *icon, const char *text,
                                 GtkWidget *buttons);
/* A right-aligned row of buttons. */
GtkWidget *sysdm_button_row(GtkWidget *first, ...) G_GNUC_NULL_TERMINATED;
/* A button that opens something; same width as Windows' (about 112 px). */
GtkWidget *sysdm_button(const char *label, GCallback cb, gpointer data);
/* A fixed-size dialog over the System Properties window, with OK (and
 * Cancel if `cancel`). Returns the content box. */
GtkWidget *sysdm_dialog(sysdm *s, const char *title, const char *icon, gboolean cancel,
                        GtkWidget **dialog_out);
/* "1,234,567" */
char *sysdm_grouped(guint64 v);
/* Reads a small text file; nullptr if it can't. Trailing newline removed. */
char *sysdm_read_line(const char *path);
/* An error box: `what` failed (a system service's D-Bus call), why, and
 * whether it's for want of a polkit agent. */
void sysdm_call_failed(GtkWindow *parent, const char *what, GError *err);

/* ---- tabs ---- */
GtkWidget *sysdm_hardware_page(sysdm *s);
GtkWidget *sysdm_advanced_page(sysdm *s);
/* The Device Manager window (also `xd-sysdm --devmgr`). */
void sysdm_device_manager(sysdm *s);

#endif
