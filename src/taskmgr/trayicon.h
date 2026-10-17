/* SPDX-License-Identifier: MIT
 * Task Manager's icon in the notification area: the CPU meter, as in
 * Windows, through the StatusNotifierItem protocol (D-Bus), with its menu
 * (Restore, Always On Top, Close Task Manager) over dbusmenu.
 */
#ifndef XD_TASKMGR_TRAYICON_H
#define XD_TASKMGR_TRAYICON_H

#include <glib.h>

typedef struct xd_trayicon xd_trayicon;

typedef struct xd_trayicon_ops {
    void (*restore)(gpointer data);                     /* a click, or Restore */
    void (*set_on_top)(gboolean on_top, gpointer data); /* Always On Top */
    void (*close)(gpointer data);                       /* Close Task Manager */
    /* The notification area came or went (the shell started or quit). */
    void (*shown)(gboolean shown, gpointer data);
} xd_trayicon_ops;

/* Registers with the notification area whenever there is one. */
xd_trayicon *xd_trayicon_new(const xd_trayicon_ops *ops, gpointer data);
void xd_trayicon_free(xd_trayicon *ti);
/* Whether the notification area is showing the icon. */
gboolean xd_trayicon_shown(const xd_trayicon *ti);
/* The meter and the tooltip; `cpu` is 0..1. */
void xd_trayicon_set_cpu(xd_trayicon *ti, double cpu);
/* Always On Top's tick in the menu. */
void xd_trayicon_set_on_top(xd_trayicon *ti, gboolean on_top);

#endif
