/* SPDX-License-Identifier: MIT
 * Pieces the Task Manager's tabs share (widgets.c), and the Networking
 * (network.c) and Users (users.c) tabs.
 */
#ifndef XD_TASKMGR_TASKMGR_H
#define XD_TASKMGR_TASKMGR_H

#include <gtk/gtk.h>

/* A list in a sunken scrolled window. */
GtkWidget *xd_tm_scrolled(GtkWidget *child);
/* Buttons along the bottom right of a tab. */
GtkWidget *xd_tm_button_row(GtkWidget **buttons, int count);
/* A black well with a thin sunken edge; gives the inside rectangle. */
void xd_tm_draw_well(GtkWidget *widget, cairo_t *cairo, double *inner_x, double *inner_y,
                     double *wd, double *ht);
/* A titled frame around child. */
GtkWidget *xd_tm_group(const char *title, GtkWidget *child);

/* ---- Networking ---- */

typedef struct xd_net_tab xd_net_tab;
xd_net_tab *xd_net_tab_new(void);
void xd_net_tab_free(xd_net_tab *net_tab);
GtkWidget *xd_net_tab_widget(xd_net_tab *net_tab);
/* A new sample, every update tick (the graphs keep their history while
 * the tab is hidden); `visible` says whether to redraw. */
void xd_net_tab_sample(xd_net_tab *net_tab, gboolean visible);

/* ---- Users ---- */

typedef struct xd_users_tab xd_users_tab;
xd_users_tab *xd_users_tab_new(GtkWindow *parent);
void xd_users_tab_free(xd_users_tab *users_tab);
GtkWidget *xd_users_tab_widget(xd_users_tab *users_tab);
/* Asks logind for the sessions again. */
void xd_users_tab_refresh(xd_users_tab *users_tab);

#endif
