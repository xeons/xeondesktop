/* SPDX-License-Identifier: MIT
 * XP's task pane, Explorer's left pane when the Folders bar is off: groups
 * of tasks ("File and Folder Tasks", "Other Places") and Details, each
 * under a title bar that folds it away. Drawn by libxdtheme (panes.h), from
 * the theme package where it styles it.
 */
#ifndef XD_FILES_TASKPANE_H
#define XD_FILES_TASKPANE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* What a task does: called with the pane and the task's data. */
typedef void (*XdTaskFunc)(GtkWidget *pane, gpointer data);

/* The pane, in a scrolled window (what's returned). */
GtkWidget *xd_task_pane_new(void);
/* Rereads the appearance (style, colours, theme package) from
 * desktop.ini. */
void xd_task_pane_reload_theme(GtkWidget *pane);

/* Filling it: begin, then groups with their tasks and lines, then end.
 * A group keeps whether it's folded by its title. `special` is XP's
 * darker group (My Computer's System Tasks). */
void xd_task_pane_begin(GtkWidget *pane);
int xd_task_pane_group(GtkWidget *pane, const char *title, gboolean special,
                       gboolean folded_at_first);
/* `data` is freed with `free` (may be nullptr) when the pane is refilled. */
void xd_task_pane_task(GtkWidget *pane, int group, const char *icon_name, const char *label,
                       XdTaskFunc func, gpointer data, GDestroyNotify free);
/* A line of text (Details): its first in bold when `name`. */
void xd_task_pane_line(GtkWidget *pane, int group, const char *text, gboolean name);
void xd_task_pane_end(GtkWidget *pane);

G_END_DECLS

#endif
