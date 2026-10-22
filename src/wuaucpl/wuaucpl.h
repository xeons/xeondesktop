/* SPDX-License-Identifier: MIT
 * Automatic Updates (xd-wuaucpl): what its tabs and the background check
 * share.
 */
#ifndef XD_WUAUCPL_WUAUCPL_H
#define XD_WUAUCPL_WUAUCPL_H

#include <flatpak.h>
#include <gtk/gtk.h>
#include <packagekit-glib2/packagekit.h>

#include "../settings/settings.h"
#include "state.h"

#define XD_WUAU_TITLE "Automatic Updates"

#define FWUPD_NAME "org.freedesktop.fwupd"
#define FWUPD_PATH "/"
#define FWUPD_IFACE "org.freedesktop.fwupd"

/* The tabs, in order (--page). */
enum { XD_WUAU_PAGE_OPTIONS, XD_WUAU_PAGE_SOFTWARE, XD_WUAU_PAGE_FIRMWARE,
       XD_WUAU_PAGE_SECURITY };

typedef struct wuau {
    GtkWidget *window, *notebook, *apply;
    bool filling; /* the widgets are being filled in: not the user's change */
    GCancellable *quit; /* cancelled when the window closes */

    /* Automatic Updates */
    GtkWidget *mode[XD_UPDATES_MODE_COUNT], *day, *hour, *schedule, *last_checked;

    /* Software Updates */
    PkClient *pk;      /* queries: never asks for a password */
    PkClient *pk_task; /* installs: interactive, so polkit asks */
    PkControl *control;
    char *backend; /* PackageKit's backend, for the Source column; nullptr until known */
    GtkListStore *sw_store;
    GtkWidget *sw_view, *sw_status, *sw_details, *sw_check, *sw_install;
    unsigned sw_generation; /* bumped on reload; late replies are dropped */
    int sw_pending;         /* queries running */
    bool sw_failed;         /* PackageKit didn't answer */
    bool sw_refresh;        /* "Check for Updates": refresh, then count it as a check */
    bool busy;              /* an install is running */

    /* Firmware */
    GDBusProxy *fwupd; /* nullptr: no fwupd */
    GtkListStore *fw_store;
    GtkWidget *fw_view, *fw_status, *fw_details, *fw_install;
    int fw_updates; /* devices with an update; -1 until listed */

    /* Device Security */
    GtkWidget *hsi_level, *hsi_text, *hsi_view;
    GtkTreeStore *hsi_store;
} wuau;

/* main.c */
GtkWidget *xd_wuau_label(const char *text);
GtkWidget *xd_wuau_frame(const char *title, GtkWidget *child);
void xd_wuau_error(wuau *applet, const char *what, const char *detail);
/* Records what's waiting for the shell's icon (after a list changed).
 * checked: a refresh just happened, so it counts as a check. */
void xd_wuau_save_state(wuau *applet, bool checked);
/* Asks to restart (an update needs it); Restart Now runs xd-shutdown. */
void xd_wuau_ask_restart(wuau *applet, const char *why);

/* A modal progress window for installs. */
typedef struct xd_wuau_progress xd_wuau_progress;
xd_wuau_progress *xd_wuau_progress_new(wuau *applet, const char *text, GCancellable *cancel);
/* fraction -1: moving back and forth; -2: as it is (only the status). */
void xd_wuau_progress_set(xd_wuau_progress *progress, double fraction, const char *status);
void xd_wuau_progress_cancellable(xd_wuau_progress *progress, bool can);
void xd_wuau_progress_free(xd_wuau_progress *progress);

/* software.c: the Software Updates tab */
GtkWidget *xd_wuau_software_page(wuau *applet);
void xd_wuau_software_load(wuau *applet);
/* What's listed: updates, how many important, and their ids. */
int xd_wuau_software_count(wuau *applet, int *important, GPtrArray *ids);
void xd_wuau_software_finish(wuau *applet);
/* `xd-wuaucpl --check` (no window): refresh, count, maybe download. */
int xd_wuau_check(void);

/* firmware.c: the Firmware and Device Security tabs */
GtkWidget *xd_wuau_firmware_page(wuau *applet);
GtkWidget *xd_wuau_security_page(wuau *applet);
void xd_wuau_firmware_load(wuau *applet);
void xd_wuau_firmware_ids(wuau *applet, int *important, GPtrArray *ids);
void xd_wuau_firmware_finish(wuau *applet);
/* For --check: devices with firmware updates (-1 without fwupd); adds
 * the release ids to `ids`, the important ones to *important. */
int xd_wuau_firmware_check(GDBusConnection *bus, int *important, GPtrArray *ids);

#endif
