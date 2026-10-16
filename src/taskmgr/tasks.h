/* SPDX-License-Identifier: MIT
 * The open windows (Applications tab), from the compositor's
 * wlr-foreign-toplevel-management protocol over GTK's Wayland connection.
 */
#ifndef XD_TASKMGR_TASKS_H
#define XD_TASKMGR_TASKS_H

#include <gtk/gtk.h>

typedef struct xd_tasklist xd_tasklist;

typedef struct xd_task {
    char *title, *app_id;
    gboolean minimized, maximized, activated;
    gpointer user; /* for the UI */
} xd_task;

typedef void (*xd_task_cb)(xd_task *task, gpointer data);

/* `changed` runs when a window appears or changes, `closed` just before
 * it's freed. Returns nullptr if GTK isn't on Wayland. Check
 * xd_tasklist_available once the first events have arrived. */
xd_tasklist *xd_tasklist_new(GdkDisplay *display, xd_task_cb changed, xd_task_cb closed,
                             gpointer data);
void xd_tasklist_free(xd_tasklist *list);
gboolean xd_tasklist_available(const xd_tasklist *list);

void xd_task_activate(xd_task *task);
void xd_task_close(xd_task *task);
void xd_task_set_minimized(xd_task *task, gboolean minimized);
void xd_task_set_maximized(xd_task *task, gboolean maximized);

#endif
