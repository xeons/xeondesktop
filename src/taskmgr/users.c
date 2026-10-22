/* SPDX-License-Identifier: MIT
 * Task Manager's Users tab, as in Windows XP with Fast User Switching:
 * the logged-on sessions (logind's) with User, ID, Status, Client Name
 * and Session, and Disconnect, Logoff and Send Message... below.
 *
 * Status is Active for the session in front on its seat, Disconnected for
 * one switched away from, as XP showed them. Logoff asks logind to end the
 * session (TerminateSession; polkit decides, and may ask for a password
 * for another user's). Disconnect on our own session goes to the display
 * manager's greeter, as Switch User does (xd-shutdown); logind can't
 * disconnect someone else's. There's nothing like XP's Send Message on
 * Linux, so that button stays greyed out.
 *
 * In a nested or test session sessions are listed, but Logoff and
 * Disconnect only print what they'd do, unless DBUS_SYSTEM_BUS_ADDRESS
 * points at a mock logind (see login1.h).
 */
#include <unistd.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/login1.h"
#include "taskmgr.h"

#define FDO_DM "org.freedesktop.DisplayManager"
#define GDM "org.gnome.DisplayManager"

enum { US_USER, US_ID, US_STATUS, US_CLIENT, US_SESSION, US_PATH, US_COUNT };

struct xd_users_tab {
    GtkWindow *parent;
    GtkWidget *box, *tree, *disconnect, *logoff, *send;
    GtkListStore *store;
    GCancellable *cancel;
    GDBusConnection *bus;
    char *own_path;  /* our session's object path */
    guint subs[3];   /* SessionNew, SessionRemoved, PropertiesChanged */
    guint watch_dm[2];
    gboolean have_dm[2];
    gboolean busy;   /* a refresh is under way */
    gboolean again;  /* refresh once more when it's done */
    GPtrArray *rows; /* GVariant* (a{sv}) of the refresh under way */
    guint pending;
};

/* ---- the list ---- */

static char *prop_string(GVariant *props, const char *key) {
    const char *value = nullptr;
    if (!g_variant_lookup(props, key, "&s", &value))
        value = "";
    return g_strdup(value);
}

static gboolean prop_bool(GVariant *props, const char *key) {
    gboolean value = FALSE;
    g_variant_lookup(props, key, "b", &value);
    return value;
}

/* XP's Session column: Console for the local display, else where it came
 * from (the remote service, or the text terminal). */
static char *session_name(GVariant *props) {
    if (prop_bool(props, "Remote"))
        return prop_string(props, "Service");
    char *type = prop_string(props, "Type");
    char *tty = prop_string(props, "TTY");
    gboolean graphical = g_strcmp0(type, "x11") == 0 || g_strcmp0(type, "wayland") == 0 ||
                         g_strcmp0(type, "mir") == 0;
    char *name = graphical || tty[0] == '\0' ? g_strdup("Console") : g_strdup(tty);
    g_free(tty);
    g_free(type);
    return name;
}

static char *selected_path(xd_users_tab *users_tab) {
    GtkTreeIter iter;
    GtkTreeModel *model = nullptr;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(users_tab->tree));
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return nullptr;
    char *path = nullptr;
    gtk_tree_model_get(model, &iter, US_PATH, &path, -1);
    return path;
}

static void update_buttons(xd_users_tab *users_tab) {
    char *path = selected_path(users_tab);
    gboolean own = path != nullptr && g_strcmp0(path, users_tab->own_path) == 0;
    gtk_widget_set_sensitive(users_tab->logoff, path != nullptr);
    gtk_widget_set_sensitive(users_tab->disconnect,
                             own && (users_tab->have_dm[0] || users_tab->have_dm[1]));
    g_free(path);
}

static gint by_id(gconstpointer left, gconstpointer right) {
    char *ia = prop_string(*(GVariant *const *)left, "Id");
    char *ib = prop_string(*(GVariant *const *)right, "Id");
    gint order = g_utf8_collate(ia, ib);
    g_free(ia);
    g_free(ib);
    return order;
}

static void fill(xd_users_tab *users_tab) {
    char *keep = selected_path(users_tab);
    g_ptr_array_sort(users_tab->rows, by_id);
    gtk_list_store_clear(users_tab->store);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(users_tab->tree));
    for (guint i = 0; i < users_tab->rows->len; i++) {
        GVariant *props = g_ptr_array_index(users_tab->rows, i);
        char *cls = prop_string(props, "Class");
        /* Greeters and the user manager's own sessions aren't logons. */
        gboolean user = g_strcmp0(cls, "user") == 0;
        g_free(cls);
        if (!user)
            continue;
        const char *path = nullptr;
        g_variant_lookup(props, "__path", "&o", &path);
        char *name = prop_string(props, "Name");
        char *id = prop_string(props, "Id");
        char *client = prop_bool(props, "Remote") ? prop_string(props, "RemoteHost")
                                                  : g_strdup("");
        char *session = session_name(props);
        GtkTreeIter iter;
        gtk_list_store_insert_with_values(
            users_tab->store, &iter, -1, US_USER, name, US_ID, id, US_STATUS,
            prop_bool(props, "Active") ? "Active" : "Disconnected", US_CLIENT, client, US_SESSION,
            session, US_PATH, path, -1);
        if (g_strcmp0(path, keep) == 0)
            gtk_tree_selection_select_iter(sel, &iter);
        g_free(session);
        g_free(client);
        g_free(id);
        g_free(name);
    }
    g_free(keep);
    update_buttons(users_tab);
}

static void refresh_done(xd_users_tab *users_tab) {
    fill(users_tab);
    g_ptr_array_set_size(users_tab->rows, 0);
    users_tab->busy = FALSE;
    if (users_tab->again) {
        users_tab->again = FALSE;
        xd_users_tab_refresh(users_tab);
    }
}

typedef struct got_props_data {
    xd_users_tab *u;
    char *path;
} got_props_data;

static void got_props(GObject *src, GAsyncResult *res, gpointer data) {
    got_props_data *props_request = data;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        g_free(props_request->path);
        g_free(props_request);
        return; /* the tab is gone */
    }
    xd_users_tab *users_tab = props_request->u;
    if (variant != nullptr) {
        GVariant *props = nullptr;
        g_variant_get(variant, "(@a{sv})", &props);
        /* The path rides along with the properties. */
        GVariantDict dict;
        g_variant_dict_init(&dict, props);
        g_variant_dict_insert(&dict, "__path", "o", props_request->path);
        g_ptr_array_add(users_tab->rows, g_variant_ref_sink(g_variant_dict_end(&dict)));
        g_variant_unref(props);
        g_variant_unref(variant);
    } else {
        /* Ended meanwhile. */
        g_error_free(error);
    }
    g_free(props_request->path);
    g_free(props_request);
    if (--users_tab->pending == 0)
        refresh_done(users_tab);
}

static void got_sessions(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            xd_users_tab *users_tab = data;
            g_printerr("xd-taskmgr: cannot list sessions: %s\n", error->message);
            users_tab->busy = FALSE;
        }
        g_error_free(error);
        return;
    }
    xd_users_tab *users_tab = data;
    GVariantIter *it = nullptr;
    g_variant_get(variant, "(a(susso))", &it);
    const char *path = nullptr;
    users_tab->pending = 1; /* held until every call is out */
    while (g_variant_iter_next(it, "(&su&s&s&o)", nullptr, nullptr, nullptr, nullptr, &path)) {
        got_props_data *props_request = g_new0(got_props_data, 1);
        props_request->u = users_tab;
        props_request->path = g_strdup(path);
        users_tab->pending++;
        g_dbus_connection_call(users_tab->bus, XD_LOGIN1, path, "org.freedesktop.DBus.Properties",
                               "GetAll", g_variant_new("(s)", XD_LOGIN1_SESSION),
                               G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1,
                               users_tab->cancel, got_props, props_request);
    }
    g_variant_iter_free(it);
    g_variant_unref(variant);
    if (--users_tab->pending == 0)
        refresh_done(users_tab);
}

void xd_users_tab_refresh(xd_users_tab *users_tab) {
    if (users_tab->bus == nullptr)
        return;
    if (users_tab->busy) {
        users_tab->again = TRUE;
        return;
    }
    users_tab->busy = TRUE;
    g_dbus_connection_call(users_tab->bus, XD_LOGIN1, XD_LOGIN1_PATH, XD_LOGIN1_MANAGER,
                           "ListSessions", nullptr, G_VARIANT_TYPE("(a(susso))"),
                           G_DBUS_CALL_FLAGS_NONE, -1, users_tab->cancel, got_sessions, users_tab);
}

/* ---- buttons ---- */

static void logged_off(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant != nullptr) {
        g_variant_unref(variant);
        return;
    }
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        xd_users_tab *users_tab = data;
        char *remote = g_dbus_error_get_remote_error(error);
        gboolean denied = remote != nullptr && g_str_has_suffix(remote, ".AccessDenied");
        g_free(remote);
        char *text = g_strdup_printf("Could not log off the selected user.\n\n%s",
                                     denied ? "Access is denied." : error->message);
        xd_message_box(users_tab->parent, "Task Manager", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
    }
    g_error_free(error);
}

static void cb_logoff([[maybe_unused]] GtkWidget *widget, gpointer data) {
    xd_users_tab *users_tab = data;
    char *path = selected_path(users_tab);
    if (path == nullptr)
        return;
    GtkTreeIter iter;
    GtkTreeModel *model = nullptr;
    char *id = nullptr;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(users_tab->tree)),
                                        &model, &iter))
        gtk_tree_model_get(model, &iter, US_ID, &id, -1);
    /* Destructive: No is the default, as for End Process. */
    if (id != nullptr &&
        xd_message_box(users_tab->parent, "Task Manager", XD_MB_WARNING, XD_MB_YES_NO, TRUE,
                       "Are you sure you want to log off the selected user?\n\nAny unsaved "
                       "data will be lost.") == GTK_RESPONSE_YES) {
        if (xd_login1_simulated())
            g_printerr("xd-taskmgr: nested session, not running: logind TerminateSession %s\n",
                       id);
        else
            g_dbus_connection_call(users_tab->bus, XD_LOGIN1, XD_LOGIN1_PATH, XD_LOGIN1_MANAGER,
                                   "TerminateSession", g_variant_new("(s)", id), nullptr,
                                   G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                                   users_tab->cancel, logged_off, users_tab);
    }
    g_free(id);
    g_free(path);
}

/* Our programs live next to this one in a build tree; else use PATH. */
static char *own_program(const char *name) {
    char *exe = g_file_read_link("/proc/self/exe", nullptr);
    if (exe != nullptr) {
        char *dir = g_path_get_dirname(exe);
        char *path = g_build_filename(dir, name, nullptr);
        g_free(dir);
        g_free(exe);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
            return path;
        g_free(path);
    }
    return g_strdup(name);
}

/* XP: disconnecting your own session goes to the Welcome screen and
 * leaves the session running. */
static void cb_disconnect([[maybe_unused]] GtkWidget *widget, gpointer data) {
    xd_users_tab *users_tab = data;
    char *prog = own_program("xd-shutdown");
    char action[] = "--action", which[] = "switchuser";
    char *argv[] = {prog, action, which, nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr,
                       &error)) {
        xd_message_box(users_tab->parent, "Task Manager", XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
    }
    g_free(prog);
}

static void on_selection([[maybe_unused]] GtkTreeSelection *sel, gpointer data) {
    update_buttons(data);
}

/* ---- logind ---- */

static void on_logind_signal([[maybe_unused]] GDBusConnection *bus,
                             [[maybe_unused]] const char *sender,
                             [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                             [[maybe_unused]] const char *member,
                             [[maybe_unused]] GVariant *params, gpointer data) {
    xd_users_tab_refresh(data);
}

static void got_own(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr) {
        /* Not in a logind session (a test): no session is ours. */
        g_error_free(error);
        return;
    }
    xd_users_tab *users_tab = data;
    g_variant_get(variant, "(o)", &users_tab->own_path);
    g_variant_unref(variant);
    update_buttons(users_tab);
}

static void dm_appeared([[maybe_unused]] GDBusConnection *bus, const char *name,
                        [[maybe_unused]] const char *owner, gpointer data) {
    xd_users_tab *users_tab = data;
    users_tab->have_dm[strcmp(name, FDO_DM) == 0 ? 0 : 1] = TRUE;
    update_buttons(users_tab);
}

static void dm_vanished([[maybe_unused]] GDBusConnection *bus, const char *name, gpointer data) {
    xd_users_tab *users_tab = data;
    users_tab->have_dm[strcmp(name, FDO_DM) == 0 ? 0 : 1] = FALSE;
    update_buttons(users_tab);
}

static void got_bus([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GDBusConnection *bus = g_bus_get_finish(res, &error);
    if (bus == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_printerr("xd-taskmgr: no system bus for the Users tab: %s\n", error->message);
        g_error_free(error);
        return;
    }
    xd_users_tab *users_tab = data;
    users_tab->bus = bus;
    users_tab->subs[0] = g_dbus_connection_signal_subscribe(
        bus, XD_LOGIN1, XD_LOGIN1_MANAGER, "SessionNew", XD_LOGIN1_PATH, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_logind_signal, users_tab, nullptr);
    users_tab->subs[1] = g_dbus_connection_signal_subscribe(
        bus, XD_LOGIN1, XD_LOGIN1_MANAGER, "SessionRemoved", XD_LOGIN1_PATH, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_logind_signal, users_tab, nullptr);
    /* A session coming to the front or going to the back (Active). */
    users_tab->subs[2] = g_dbus_connection_signal_subscribe(
        bus, XD_LOGIN1, "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr,
        XD_LOGIN1_SESSION, G_DBUS_SIGNAL_FLAGS_NONE, on_logind_signal, users_tab, nullptr);
    const char *id = g_getenv("XDG_SESSION_ID");
    if (id != nullptr && id[0] != '\0')
        g_dbus_connection_call(bus, XD_LOGIN1, XD_LOGIN1_PATH, XD_LOGIN1_MANAGER, "GetSession",
                               g_variant_new("(s)", id), G_VARIANT_TYPE("(o)"),
                               G_DBUS_CALL_FLAGS_NONE, -1, users_tab->cancel, got_own, users_tab);
    else
        g_dbus_connection_call(bus, XD_LOGIN1, XD_LOGIN1_PATH, XD_LOGIN1_MANAGER, "GetSessionByPID",
                               g_variant_new("(u)", (guint32)getpid()), G_VARIANT_TYPE("(o)"),
                               G_DBUS_CALL_FLAGS_NONE, -1, users_tab->cancel, got_own, users_tab);
    /* Disconnect needs a display manager to go to. */
    users_tab->watch_dm[0] = g_bus_watch_name_on_connection(
        bus, FDO_DM, G_BUS_NAME_WATCHER_FLAGS_NONE, dm_appeared, dm_vanished, users_tab, nullptr);
    users_tab->watch_dm[1] = g_bus_watch_name_on_connection(
        bus, GDM, G_BUS_NAME_WATCHER_FLAGS_NONE, dm_appeared, dm_vanished, users_tab, nullptr);
    xd_users_tab_refresh(users_tab);
}

/* ---- setup ---- */

static void text_column(GtkWidget *tree, const char *title, int col, int width) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", col, nullptr);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, width);
    gtk_tree_view_column_set_sort_column_id(column, col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
}

xd_users_tab *xd_users_tab_new(GtkWindow *parent) {
    xd_users_tab *users_tab = g_new0(xd_users_tab, 1);
    users_tab->parent = parent;
    users_tab->cancel = g_cancellable_new();
    users_tab->rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
    users_tab->store = gtk_list_store_new(US_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    users_tab->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(users_tab->store));
    text_column(users_tab->tree, "User", US_USER, 110);
    text_column(users_tab->tree, "ID", US_ID, 40);
    text_column(users_tab->tree, "Status", US_STATUS, 80);
    text_column(users_tab->tree, "Client Name", US_CLIENT, 90);
    text_column(users_tab->tree, "Session", US_SESSION, 80);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(users_tab->tree)), "changed",
                     G_CALLBACK(on_selection), users_tab);

    users_tab->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(users_tab->box), 8);
    gtk_box_pack_start(GTK_BOX(users_tab->box), xd_tm_scrolled(users_tab->tree), TRUE, TRUE, 0);
    users_tab->disconnect = gtk_button_new_with_mnemonic("_Disconnect");
    users_tab->logoff = gtk_button_new_with_mnemonic("_Logoff");
    users_tab->send = gtk_button_new_with_mnemonic("_Send Message...");
    g_signal_connect(users_tab->disconnect, "clicked", G_CALLBACK(cb_disconnect), users_tab);
    g_signal_connect(users_tab->logoff, "clicked", G_CALLBACK(cb_logoff), users_tab);
    gtk_widget_set_sensitive(users_tab->send, FALSE);
    GtkWidget *buttons[] = {users_tab->disconnect, users_tab->logoff, users_tab->send};
    gtk_box_pack_start(GTK_BOX(users_tab->box), xd_tm_button_row(buttons, 3), FALSE, FALSE, 0);
    update_buttons(users_tab);

    g_bus_get(G_BUS_TYPE_SYSTEM, users_tab->cancel, got_bus, users_tab);
    return users_tab;
}

GtkWidget *xd_users_tab_widget(xd_users_tab *users_tab) { return users_tab->box; }

void xd_users_tab_free(xd_users_tab *users_tab) {
    if (users_tab == nullptr)
        return;
    g_cancellable_cancel(users_tab->cancel);
    g_object_unref(users_tab->cancel);
    for (size_t i = 0; i < G_N_ELEMENTS(users_tab->watch_dm); i++) {
        if (users_tab->watch_dm[i] != 0)
            g_bus_unwatch_name(users_tab->watch_dm[i]);
    }
    if (users_tab->bus != nullptr) {
        for (size_t i = 0; i < G_N_ELEMENTS(users_tab->subs); i++) {
            if (users_tab->subs[i] != 0)
                g_dbus_connection_signal_unsubscribe(users_tab->bus, users_tab->subs[i]);
        }
        g_object_unref(users_tab->bus);
    }
    g_ptr_array_unref(users_tab->rows);
    g_object_unref(users_tab->store);
    g_free(users_tab->own_path);
    g_free(users_tab);
}
