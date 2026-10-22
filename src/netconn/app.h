/* SPDX-License-Identifier: MIT
 * xd-netconn's window and dialogs: what they share.
 */
#ifndef XD_NETCONN_APP_H
#define XD_NETCONN_APP_H

#include <NetworkManager.h>
#include <gtk/gtk.h>

#include "netconn.h"

/* One icon in the window: a network adapter (its active or best saved
 * connection), or a connection of its own (VPN, broadband, a second
 * wired profile). Rebuilt whenever NetworkManager changes, so dialogs
 * keep the key and look the item up again. */
typedef struct nc_item {
    char *key; /* "dev:<object path>", "con:<uuid>" or "wizard" */
    xd_nc_group group;
    char *name, *status, *device_name;
    const char *icon;
    xd_nc_state state;
    bool connected, disabled;
    NMDevice *device;          /* or nullptr */
    NMRemoteConnection *conn;  /* its settings, or nullptr (an adapter with none) */
    NMActiveConnection *active; /* or nullptr */
} nc_item;

typedef struct nc_app {
    NMClient *client; /* nullptr: NetworkManager isn't running */
    GtkWidget *window;
    GPtrArray *items; /* nc_item */
    char *selected;   /* key */
    /* The window (main.c). */
    GtkWidget *stack, *groups, *tree, *pane, *file_menu, *tiles_item, *details_item;
    GtkListStore *store;
    guint rebuild_idle;
    bool selecting;
    /* Without the window (xd-netconn --status), we quit once the last
     * dialog is closed and NetworkManager has answered every call. */
    unsigned dialogs, busy;
    /* When each active connection came up, as far as we know (status.c). */
    GKeyFile *up_since;
} nc_app;

/* main.c */
nc_item *nc_find(nc_app *app, const char *key);
/* An item's icon, with fallbacks for icon themes without it. */
GIcon *nc_icon(const char *name);
/* The window to put dialogs over. */
GtkWindow *nc_parent(nc_app *app);
/* Every dialog is tracked; every call to NetworkManager held until its
 * answer (see nc_app's dialogs and busy). */
void nc_track(nc_app *app, GtkWidget *dialog);
void nc_hold(nc_app *app);
void nc_release(nc_app *app);
/* An error from NetworkManager in a message box. */
void nc_error(GtkWindow *parent, const char *what, const GError *error);
/* The actions, on the item with this key. */
void nc_enable(nc_app *app, const char *key);
void nc_disable(nc_app *app, const char *key);
void nc_connect(nc_app *app, const char *key);
void nc_disconnect(nc_app *app, const char *key);
void nc_repair(nc_app *app, const char *key);
/* Saves `changed` as the connection's settings / adds a connection;
 * `what` heads the error message. */
void nc_commit(nc_app *app, NMRemoteConnection *remote, NMConnection *changed, const char *what);
void nc_add(nc_app *app, NMConnection *connection, const char *what);
/* The path of one of our programs (next to us, else on PATH) or another. */
char *nc_own_program(const char *name);
/* Launches one of those. */
bool nc_spawn(GtkWindow *parent, const char *const *argv);
/* NetworkManager's own editor for what we don't cover; false if it
 * isn't installed. */
bool nc_have_editor(void);
void nc_run_editor(GtkWindow *parent, const char *uuid);

/* status.c: the Status dialog. */
void nc_status_show(nc_app *app, const char *key);
/* Seconds the active connection has been up (0 if unknown). */
guint64 nc_up_seconds(nc_app *app, NMActiveConnection *active);
/* The connection's IPv4 address, mask, gateway, and whether DHCP gave
 * them (for the Details box and Status' Support tab). Strings g_free'd;
 * empty if none. */
typedef struct nc_ipv4_info {
    char *address, *netmask, *gateway, *dns, *dhcp_server, *mac;
    char *lease_obtained, *lease_expires;
    bool dhcp;
} nc_ipv4_info;
void nc_ipv4_info_get(NMDevice *device, NMActiveConnection *active, nc_ipv4_info *out);
void nc_ipv4_info_clear(nc_ipv4_info *info);

/* props.c: the Properties dialog (TCP/IP inside). */
void nc_properties_show(nc_app *app, const char *key);

/* wizard.c: New Connection Wizard; the wireless network chooser. */
void nc_wizard_show(nc_app *app);
void nc_wireless_show(nc_app *app, NMDevice *device);

#endif
