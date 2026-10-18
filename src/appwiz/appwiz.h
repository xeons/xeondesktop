/* SPDX-License-Identifier: MIT
 * Add or Remove Programs (xd-appwiz): what its three pages and the
 * install/remove operations share.
 */
#ifndef XD_APPWIZ_APPWIZ_H
#define XD_APPWIZ_APPWIZ_H

#include <flatpak.h>
#include <gtk/gtk.h>
#include <packagekit-glib2/packagekit.h>

#include "programs.h"

#define XD_APPWIZ_TITLE "Add or Remove Programs"

/* The buttons down the left. */
typedef enum xd_appwiz_page {
    XD_PAGE_REMOVE,
    XD_PAGE_ADD,
    XD_PAGE_COMPONENTS,
} xd_appwiz_page;

typedef struct appwiz {
    GtkWidget *window, *stack, *tabs[3];
    PkClient *pk;      /* queries */
    PkClient *pk_task; /* installs and removals: interactive, so polkit asks */
    PkControl *control;
    GCancellable *quit; /* cancelled when the window closes */
    bool busy;          /* an install or removal is running */

    /* Change or Remove Programs */
    GtkWidget *list, *sort, *status;
    GPtrArray *programs; /* program */
    GHashTable *flatpaks; /* app id -> installed Flatpak */
    GHashTable *counts;   /* Start menu launch counts */
    bool have_counts;
    GHashTable *launched; /* desktop id -> when the shell last started it */
    GBookmarkFile *recent;
    GQueue lookups;    /* programs whose package isn't known yet */
    GPtrArray *detail_ids; /* package ids waiting for GetDetails */
    GHashTable *asked;     /* package ids already asked for */
    GHashTable *details;   /* package id -> PkDetails, as they come */
    bool pk_missing;       /* PackageKit didn't answer */
    unsigned generation;   /* bumped on reload; stale replies are dropped */
    int pending;           /* PackageKit queries running */
    guint reload_idle, sort_idle;
    gulong monitor_handler;

    /* Add New Programs */
    GtkWidget *search, *results, *results_title, *results_status;
    unsigned search_generation;
    int searching;
    GHashTable *result_rows; /* package name or Flatpak id -> row */
    int result_count;

    /* Add/Remove Windows Components */
    GtkWidget *groups, *group_note;
} appwiz;

/* main.c */
void xd_appwiz_reload(appwiz *w);
void xd_appwiz_show_page(appwiz *w, xd_appwiz_page page);
void xd_appwiz_error(appwiz *w, const char *what, const char *detail);
GtkWidget *xd_appwiz_label(const char *text, const char *css_class);
void xd_appwiz_add_class(GtkWidget *w, const char *css_class);

/* add.c: Add New Programs and Add/Remove Windows Components */
GtkWidget *xd_appwiz_add_page(appwiz *w);
GtkWidget *xd_appwiz_components_page(appwiz *w);

/* ops.c: installs and removals, asked for, then shown with a progress
 * dialog. `done` runs afterwards with whether it worked. */
typedef void (*xd_appwiz_done)(appwiz *w, bool ok, gpointer data);

/* Removes a package (and, after asking, what needs it). `also` names the
 * package's other programs, for the question, or is nullptr. */
void xd_appwiz_remove_package(appwiz *w, const char *name, const char *package_id,
                              const char *also, xd_appwiz_done done, gpointer data);
void xd_appwiz_install_package(appwiz *w, const char *name, const char *package_id,
                               xd_appwiz_done done, gpointer data);
/* A package file (PackageKit's InstallFiles), .flatpakref or .flatpak. */
void xd_appwiz_install_file(appwiz *w, const char *path, xd_appwiz_done done, gpointer data);
/* `installation` is "user" or a system installation's id. */
void xd_appwiz_remove_flatpak(appwiz *w, const char *name, const char *installation,
                              const char *ref, xd_appwiz_done done, gpointer data);
void xd_appwiz_install_flatpak(appwiz *w, const char *name, const char *installation,
                               const char *remote, const char *ref, xd_appwiz_done done,
                               gpointer data);

FlatpakInstallation *xd_appwiz_open_installation(const char *id, GCancellable *cancel,
                                                 GError **error);
/* The user's installation and the system ones. */
GPtrArray *xd_appwiz_installations(GCancellable *cancel);

/* A PackageKit reply's error, from either the GError or the results;
 * false if it worked. */
bool xd_appwiz_pk_failed(PkResults *results, GError *error, PkErrorEnum *code, char **message);

#endif
