/* SPDX-License-Identifier: MIT
 * xd-services: Windows XP's Services console with systemd's services in
 * it, from the system manager and your own user manager (the System and
 * User tabs), through org.freedesktop.systemd1.
 *
 * Starting, stopping and enabling system services goes through polkit
 * (xd-polkit asks for a password); your user manager's don't need it.
 * systemd has no paused state, so Pause and Resume are greyed.
 */
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "admin.h"

#define SD_NAME "org.freedesktop.systemd1"
#define SD_PATH "/org/freedesktop/systemd1"
#define SD_MANAGER "org.freedesktop.systemd1.Manager"
#define SD_UNIT "org.freedesktop.systemd1.Unit"
#define SD_SERVICE "org.freedesktop.systemd1.Service"

enum {
    COL_ICON,
    COL_ID,
    COL_NAME,
    COL_DESC,
    COL_STATUS,
    COL_STARTUP,
    COL_LOGON,
    COL_PATH,   /* the unit's object path, once known */
    COL_ACTIVE, /* ActiveState */
    COL_FILE,   /* UnitFileState */
    NCOLS
};

typedef struct services services;

typedef struct manager {
    services *app;
    bool user;
    GDBusConnection *bus;
    GtkListStore *store;
    GtkWidget *view;
    GHashTable *rows;    /* unit id -> GtkTreeIter (the store's iters persist) */
    guint generation;    /* bumped by each refresh; older replies are dropped */
    GHashTable *done;    /* job path -> result, for jobs that ended before we knew them */
    GList *dialogs;      /* open Properties dialogs */
    char *error;         /* why the manager can't be reached, or nullptr */
} manager;

struct services {
    GtkWidget *window, *notebook, *status;
    /* the extended view's panel */
    GtkWidget *ext_name, *ext_start, *ext_stop, *ext_restart, *ext_desc;
    /* actions, enabled by the selection */
    GtkWidget *tb_start, *tb_stop, *tb_pause, *tb_restart, *tb_props;
    GtkWidget *mi_start, *mi_stop, *mi_pause, *mi_resume, *mi_restart, *mi_props;
    manager mgr[2]; /* system, user */
};

/* A job we started and wait for, with XP's "Service Control" box. */
typedef struct job {
    manager *m;
    char *id, *verb, *path;
    GtkWidget *dialog, *bar;
    guint pulse;
} job;

/* ---- the list ---------------------------------------------------------------------- */

static manager *current(services *s) {
    return &s->mgr[gtk_notebook_get_current_page(GTK_NOTEBOOK(s->notebook)) == 1 ? 1 : 0];
}

static GtkTreeIter *row(manager *m, const char *id) { return g_hash_table_lookup(m->rows, id); }

static GtkTreeIter *add_row(manager *m, const char *id) {
    GtkTreeIter *it = row(m, id);
    if (it != nullptr)
        return it;
    GtkTreeIter iter;
    char *name = xd_unit_display_name(id);
    gtk_list_store_insert_with_values(m->store, &iter, -1, COL_ICON, "system-run", COL_ID, id,
                                      COL_NAME, name, COL_DESC, "", COL_STATUS, "", COL_STARTUP,
                                      "", COL_LOGON, "", -1);
    g_free(name);
    it = g_memdup2(&iter, sizeof(iter));
    g_hash_table_insert(m->rows, g_strdup(id), it);
    return it;
}

static void set_active(manager *m, GtkTreeIter *it, const char *active) {
    gtk_list_store_set(m->store, it, COL_ACTIVE, active, COL_STATUS, xd_unit_status_label(active),
                       -1);
}

static void set_file_state(manager *m, GtkTreeIter *it, const char *state) {
    char *label = xd_unit_startup_label(state);
    gtk_list_store_set(m->store, it, COL_FILE, state, COL_STARTUP, label, -1);
    g_free(label);
}

static void set_logon(manager *m, GtkTreeIter *it, const char *user) {
    gtk_list_store_set(m->store, it, COL_LOGON,
                       xd_unit_logon_label(user, m->user, g_get_user_name()), -1);
}

/* The selected row's unit id and ActiveState, or false. */
static bool selected(services *s, manager **m_out, char **id, char **active) {
    manager *m = current(s);
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (m->view == nullptr ||
        !gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(m->view)),
                                         &model, &it))
        return false;
    gtk_tree_model_get(model, &it, COL_ID, id, COL_ACTIVE, active, -1);
    if (m_out != nullptr)
        *m_out = m;
    return true;
}

/* Starting or stopping: neither can be asked for until it's done. */
static bool busy(const char *active) {
    return g_strcmp0(active, "activating") == 0 || g_strcmp0(active, "deactivating") == 0;
}

/* The actions and the extended view follow the selection. */
static void update_actions(services *s) {
    char *id = nullptr, *active = nullptr, *desc = nullptr;
    manager *m = nullptr;
    bool have = selected(s, &m, &id, &active);
    bool started = have && xd_unit_is_started(active);
    bool idle = have && !busy(active);
    gtk_widget_set_sensitive(s->tb_start, idle && !started);
    gtk_widget_set_sensitive(s->mi_start, idle && !started);
    gtk_widget_set_sensitive(s->tb_stop, idle && started);
    gtk_widget_set_sensitive(s->mi_stop, idle && started);
    gtk_widget_set_sensitive(s->tb_restart, idle && started);
    gtk_widget_set_sensitive(s->mi_restart, idle && started);
    gtk_widget_set_sensitive(s->tb_props, have);
    gtk_widget_set_sensitive(s->mi_props, have);
    gtk_widget_set_visible(s->ext_start, have && !started);
    gtk_widget_set_visible(s->ext_stop, started);
    gtk_widget_set_visible(s->ext_restart, started);
    if (have) {
        GtkTreeIter *it = row(m, id);
        if (it != nullptr)
            gtk_tree_model_get(GTK_TREE_MODEL(m->store), it, COL_DESC, &desc, -1);
        char *name = xd_unit_display_name(id);
        char *markup = g_markup_printf_escaped("<b>%s</b>", name);
        gtk_label_set_markup(GTK_LABEL(s->ext_name), markup);
        g_free(markup);
        g_free(name);
        char *text = desc != nullptr && desc[0] != '\0'
                         ? g_strdup_printf("Description:\n%s", desc)
                         : g_strdup("");
        gtk_label_set_text(GTK_LABEL(s->ext_desc), text);
        g_free(text);
    } else {
        gtk_label_set_text(GTK_LABEL(s->ext_name), "Select an item to view its description.");
        gtk_label_set_text(GTK_LABEL(s->ext_desc), "");
    }
    g_free(id);
    g_free(active);
    g_free(desc);
}

static void show_status(services *s) {
    manager *m = current(s);
    char *text = m->error != nullptr
                     ? g_strdup(m->error)
                     : g_strdup_printf("%u service%s", g_hash_table_size(m->rows),
                                       g_hash_table_size(m->rows) == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(s->status), text);
    g_free(text);
}

/* ---- loading ------------------------------------------------------------------------ */

/* What an asynchronous reply needs: its manager, the refresh it belongs
 * to and the unit. */
typedef struct pending {
    manager *m;
    guint generation;
    char *id;
    bool file_state; /* fetching UnitFileState rather than User */
} pending;

static pending *pending_new(manager *m, const char *id) {
    pending *p = g_new0(pending, 1);
    p->m = m;
    p->generation = m->generation;
    p->id = g_strdup(id);
    return p;
}

static void pending_free(pending *p) {
    g_free(p->id);
    g_free(p);
}

static bool stale(pending *p) { return p->generation != p->m->generation; }

static void got_prop(GObject *src, GAsyncResult *res, gpointer data) {
    pending *p = data;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    GtkTreeIter *it = r != nullptr && !stale(p) ? row(p->m, p->id) : nullptr;
    if (it != nullptr) {
        GVariant *v = nullptr;
        g_variant_get(r, "(v)", &v);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
            const char *value = g_variant_get_string(v, nullptr);
            if (p->file_state) {
                /* Instances ("getty@tty1") aren't in ListUnitFiles. */
                char *known = nullptr;
                gtk_tree_model_get(GTK_TREE_MODEL(p->m->store), it, COL_FILE, &known, -1);
                if (known == nullptr)
                    set_file_state(p->m, it, value);
                g_free(known);
            } else {
                set_logon(p->m, it, value);
            }
        }
        g_variant_unref(v);
    }
    if (r != nullptr)
        g_variant_unref(r);
    pending_free(p);
}

/* Log On As (the service's User=) and, for loaded units, the startup
 * type when no unit file listed it. */
static void fetch_user(manager *m, const char *id, const char *path) {
    g_dbus_connection_call(m->bus, SD_NAME, path, "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", SD_SERVICE, "User"), G_VARIANT_TYPE("(v)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, got_prop, pending_new(m, id));
}

static void fetch_file_state(manager *m, const char *id, const char *path) {
    pending *p = pending_new(m, id);
    p->file_state = true;
    g_dbus_connection_call(m->bus, SD_NAME, path, "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", SD_UNIT, "UnitFileState"), G_VARIANT_TYPE("(v)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, got_prop, p);
}

/* A unit that isn't loaded: its description and state. */
static void got_unit_props(GObject *src, GAsyncResult *res, gpointer data) {
    pending *p = data;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    GtkTreeIter *it = stale(p) ? nullptr : row(p->m, p->id);
    if (r != nullptr && it != nullptr) {
        GVariant *props = g_variant_get_child_value(r, 0);
        const char *desc = nullptr, *active = nullptr;
        if (g_variant_lookup(props, "Description", "&s", &desc))
            gtk_list_store_set(p->m->store, it, COL_DESC, desc, -1);
        if (g_variant_lookup(props, "ActiveState", "&s", &active))
            set_active(p->m, it, active);
        g_variant_unref(props);
    }
    if (r != nullptr)
        g_variant_unref(r);
    pending_free(p);
}

static void got_loaded(GObject *src, GAsyncResult *res, gpointer data) {
    pending *p = data;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    GtkTreeIter *it = stale(p) ? nullptr : row(p->m, p->id);
    if (r != nullptr && it != nullptr) {
        const char *path = nullptr;
        g_variant_get(r, "(&o)", &path);
        gtk_list_store_set(p->m->store, it, COL_PATH, path, -1);
        g_dbus_connection_call(p->m->bus, SD_NAME, path, "org.freedesktop.DBus.Properties",
                               "GetAll", g_variant_new("(s)", SD_UNIT),
                               G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                               got_unit_props, pending_new(p->m, p->id));
        fetch_user(p->m, p->id, path);
    }
    if (r != nullptr)
        g_variant_unref(r);
    pending_free(p);
}

static bool is_service(const char *id) {
    /* Templates ("getty@.service") can't run by themselves; their
     * instances are listed as they're loaded. */
    return g_str_has_suffix(id, ".service") && !g_str_has_suffix(id, "@.service");
}

static void got_unit_files(GObject *src, GAsyncResult *res, gpointer data) {
    pending *p = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        g_printerr("xd-services: ListUnitFiles: %s\n", error->message);
        g_error_free(error);
    } else if (!stale(p)) {
        GVariantIter *iter = nullptr;
        const char *file = nullptr, *state = nullptr;
        g_variant_get(r, "(a(ss))", &iter);
        while (g_variant_iter_next(iter, "(&s&s)", &file, &state)) {
            char *id = g_path_get_basename(file);
            if (is_service(id)) {
                bool known = row(p->m, id) != nullptr;
                GtkTreeIter *it = add_row(p->m, id);
                set_file_state(p->m, it, state);
                if (!known) {
                    /* Not loaded: load it for its description. */
                    set_active(p->m, it, "inactive");
                    set_logon(p->m, it, nullptr);
                    g_dbus_connection_call(p->m->bus, SD_NAME, SD_PATH, SD_MANAGER, "LoadUnit",
                                           g_variant_new("(s)", id), G_VARIANT_TYPE("(o)"),
                                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, got_loaded,
                                           pending_new(p->m, id));
                }
            }
            g_free(id);
        }
        g_variant_iter_free(iter);
        show_status(p->m->app);
    }
    if (r != nullptr)
        g_variant_unref(r);
    pending_free(p);
}

static void got_units(GObject *src, GAsyncResult *res, gpointer data) {
    pending *p = data;
    manager *m = p->m;
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        g_dbus_error_strip_remote_error(error);
        if (!stale(p)) {
            g_free(m->error);
            m->error = g_strdup_printf("The %s service manager can't be reached: %s",
                                       m->user ? "user" : "system", error->message);
            show_status(m->app);
        }
        g_error_free(error);
        pending_free(p);
        return;
    }
    if (!stale(p)) {
        GVariantIter *iter = nullptr;
        const char *id, *desc, *load, *active, *sub, *following, *path, *jtype, *jpath;
        guint32 job_id;
        g_variant_get(r, "(a(ssssssouso))", &iter);
        while (g_variant_iter_next(iter, "(&s&s&s&s&s&s&ou&s&o)", &id, &desc, &load, &active,
                                   &sub, &following, &path, &job_id, &jtype, &jpath)) {
            if (!is_service(id) || strcmp(load, "not-found") == 0)
                continue;
            GtkTreeIter *it = add_row(m, id);
            gtk_list_store_set(m->store, it, COL_DESC, desc, COL_PATH, path, -1);
            set_active(m, it, active);
            set_logon(m, it, nullptr);
            fetch_user(m, id, path);
            if (strchr(id, '@') != nullptr)
                fetch_file_state(m, id, path);
        }
        g_variant_iter_free(iter);
        /* The installed unit files: their startup types, and the services
         * that aren't loaded. */
        g_dbus_connection_call(m->bus, SD_NAME, SD_PATH, SD_MANAGER, "ListUnitFiles", nullptr,
                               G_VARIANT_TYPE("(a(ss))"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                               got_unit_files, pending_new(m, nullptr));
        show_status(m->app);
    }
    g_variant_unref(r);
    pending_free(p);
}

static void refresh(manager *m) {
    m->generation++;
    if (m->bus == nullptr)
        return;
    g_clear_pointer(&m->error, g_free);
    /* Keep the selection across the reload. */
    char *keep = nullptr;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(m->view)),
                                        &model, &it))
        gtk_tree_model_get(model, &it, COL_ID, &keep, -1);
    g_hash_table_remove_all(m->rows);
    gtk_list_store_clear(m->store);
    if (keep != nullptr) {
        /* Selected again when it comes back. */
        g_object_set_data_full(G_OBJECT(m->view), "keep", keep, g_free);
    }
    g_dbus_connection_call(m->bus, SD_NAME, SD_PATH, SD_MANAGER, "ListUnits", nullptr,
                           G_VARIANT_TYPE("(a(ssssssouso))"), G_DBUS_CALL_FLAGS_NONE, -1,
                           nullptr, got_units, pending_new(m, nullptr));
}

/* Reselects the unit selected before a refresh once its row is back. */
static void on_row_inserted([[maybe_unused]] GtkTreeModel *model, [[maybe_unused]] GtkTreePath *p,
                            GtkTreeIter *it, gpointer data) {
    manager *m = data;
    const char *keep = g_object_get_data(G_OBJECT(m->view), "keep");
    if (keep == nullptr)
        return;
    char *id = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(m->store), it, COL_ID, &id, -1);
    /* The id is set in the same call that inserts the row. */
    if (g_strcmp0(id, keep) == 0) {
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(m->view)), it);
        g_object_set_data(G_OBJECT(m->view), "keep", nullptr);
    }
    g_free(id);
}

/* ---- live changes -------------------------------------------------------------------- */

static void dialogs_update(manager *m, const char *id);

static const char *id_for_path(manager *m, const char *path, GtkTreeIter **it_out) {
    GHashTableIter hi;
    gpointer key, value;
    g_hash_table_iter_init(&hi, m->rows);
    while (g_hash_table_iter_next(&hi, &key, &value)) {
        char *p = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(m->store), value, COL_PATH, &p, -1);
        bool match = g_strcmp0(p, path) == 0;
        g_free(p);
        if (match) {
            *it_out = value;
            return key;
        }
    }
    return nullptr;
}

static void on_props_changed([[maybe_unused]] GDBusConnection *c,
                             [[maybe_unused]] const char *sender, const char *path,
                             [[maybe_unused]] const char *iface,
                             [[maybe_unused]] const char *signal, GVariant *params,
                             gpointer data) {
    manager *m = data;
    const char *interface = nullptr;
    GVariant *changed = nullptr;
    g_variant_get(params, "(&s@a{sv}@as)", &interface, &changed, nullptr);
    const char *active = nullptr;
    if (strcmp(interface, SD_UNIT) == 0 && g_variant_lookup(changed, "ActiveState", "&s", &active)) {
        GtkTreeIter *it = nullptr;
        const char *id = id_for_path(m, path, &it);
        if (id != nullptr) {
            set_active(m, it, active);
            dialogs_update(m, id);
            update_actions(m->app);
        }
    }
    g_variant_unref(changed);
}

static void on_unit_files_changed([[maybe_unused]] GDBusConnection *c,
                                  [[maybe_unused]] const char *sender,
                                  [[maybe_unused]] const char *path,
                                  [[maybe_unused]] const char *iface,
                                  [[maybe_unused]] const char *signal,
                                  [[maybe_unused]] GVariant *params, gpointer data) {
    manager *m = data;
    g_dbus_connection_call(m->bus, SD_NAME, SD_PATH, SD_MANAGER, "ListUnitFiles", nullptr,
                           G_VARIANT_TYPE("(a(ss))"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           got_unit_files, pending_new(m, nullptr));
}

static void job_finished(job *j, const char *result);

static GList *jobs; /* jobs waiting for JobRemoved */

static void on_job_removed([[maybe_unused]] GDBusConnection *c,
                           [[maybe_unused]] const char *sender,
                           [[maybe_unused]] const char *path,
                           [[maybe_unused]] const char *iface,
                           [[maybe_unused]] const char *signal, GVariant *params, gpointer data) {
    manager *m = data;
    guint32 id = 0;
    const char *jpath = nullptr, *unit = nullptr, *result = nullptr;
    g_variant_get(params, "(u&o&s&s)", &id, &jpath, &unit, &result);
    for (GList *l = jobs; l != nullptr; l = l->next) {
        job *j = l->data;
        if (j->m == m && g_strcmp0(j->path, jpath) == 0) {
            job_finished(j, result);
            return;
        }
    }
    /* Ours, perhaps, before its reply: kept until the reply comes. */
    if (jobs != nullptr)
        g_hash_table_insert(m->done, g_strdup(jpath), g_strdup(result));
}

static void manager_connect(manager *m, GBusType type) {
    GError *error = nullptr;
    m->bus = g_bus_get_sync(type, nullptr, &error);
    if (m->bus == nullptr) {
        m->error = g_strdup_printf("The %s service manager can't be reached: %s",
                                   m->user ? "user" : "system", error->message);
        g_error_free(error);
        return;
    }
    g_dbus_connection_signal_subscribe(m->bus, SD_NAME, "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged", nullptr, nullptr,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_props_changed, m, nullptr);
    g_dbus_connection_signal_subscribe(m->bus, SD_NAME, SD_MANAGER, "UnitFilesChanged", SD_PATH,
                                       nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_unit_files_changed,
                                       m, nullptr);
    g_dbus_connection_signal_subscribe(m->bus, SD_NAME, SD_MANAGER, "JobRemoved", SD_PATH,
                                       nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_job_removed, m,
                                       nullptr);
    /* systemd only sends its signals while someone has subscribed. */
    g_dbus_connection_call(m->bus, SD_NAME, SD_PATH, SD_MANAGER, "Subscribe", nullptr, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

/* ---- starting and stopping ----------------------------------------------------------- */

static GtkWindow *active_window(services *s) {
    for (GList *l = gtk_window_list_toplevels(); l != nullptr; l = g_list_delete_link(l, l)) {
        if (gtk_window_is_active(l->data)) {
            GtkWindow *w = l->data;
            g_list_free(l);
            return w;
        }
    }
    return GTK_WINDOW(s->window);
}

static void job_free(job *j) {
    jobs = g_list_remove(jobs, j);
    if (j->dialog != nullptr)
        gtk_widget_destroy(j->dialog); /* on_job_dialog_destroy stops the pulse */
    g_free(j->id);
    g_free(j->verb);
    g_free(j->path);
    g_free(j);
}

static void job_finished(job *j, const char *result) {
    services *s = j->m->app;
    manager *m = j->m;
    char *id = g_strdup(j->id), *verb = g_strdup(j->verb);
    job_free(j);
    if (jobs == nullptr)
        g_hash_table_remove_all(m->done);
    if (g_strcmp0(result, "done") != 0) {
        char *name = xd_unit_display_name(id);
        char *text = g_strdup_printf(
            "Could not %s the %s service on Local Computer.\n\nThe job ended with \"%s\". See "
            "the System log in Event Viewer for details.",
            verb, name, result != nullptr ? result : "failed");
        xd_message_box(active_window(s), "Services", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
        g_free(name);
    }
    g_free(id);
    g_free(verb);
    update_actions(s);
}

static gboolean pulse(gpointer data) {
    job *j = data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(j->bar));
    return G_SOURCE_CONTINUE;
}

static void on_job_dialog_response(GtkDialog *d, [[maybe_unused]] int response,
                                   [[maybe_unused]] gpointer data) {
    /* Closing the box doesn't cancel the job; it runs on. */
    gtk_widget_destroy(GTK_WIDGET(d));
}

/* Closed, or gone with the Properties box it was over: the progress bar
 * is gone too, so stop pulsing it. */
static void on_job_dialog_destroy([[maybe_unused]] GtkWidget *d, gpointer data) {
    job *j = data;
    j->dialog = nullptr;
    j->bar = nullptr;
    if (j->pulse != 0) {
        g_source_remove(j->pulse);
        j->pulse = 0;
    }
}

/* XP's progress box: "Windows is attempting to start the following
 * service on Local Computer..." */
static void job_dialog(job *j, const char *ing) {
    services *s = j->m->app;
    j->dialog = gtk_dialog_new_with_buttons("Service Control", active_window(s),
                                            GTK_DIALOG_DESTROY_WITH_PARENT, "_Close",
                                            GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(j->dialog), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    char *text = g_strdup_printf("XeonDesktop is attempting to %s the following service on "
                                 "Local Computer...",
                                 ing);
    GtkWidget *l = gtk_label_new(text);
    g_free(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    char *name = xd_unit_display_name(j->id);
    GtkWidget *n = gtk_label_new(name);
    g_free(name);
    gtk_label_set_xalign(GTK_LABEL(n), 0);
    gtk_box_pack_start(GTK_BOX(box), n, FALSE, FALSE, 0);
    j->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(j->bar, 320, -1);
    gtk_box_pack_start(GTK_BOX(box), j->bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(j->dialog))), box);
    g_signal_connect(j->dialog, "response", G_CALLBACK(on_job_dialog_response), j);
    g_signal_connect(j->dialog, "destroy", G_CALLBACK(on_job_dialog_destroy), j);
    j->pulse = g_timeout_add(100, pulse, j);
    gtk_widget_show_all(j->dialog);
}

static void got_job(GObject *src, GAsyncResult *res, gpointer data) {
    job *j = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        services *s = j->m->app;
        g_dbus_error_strip_remote_error(error);
        char *name = xd_unit_display_name(j->id);
        char *text = g_strdup_printf("Could not %s the %s service on Local Computer.\n\n%s",
                                     j->verb, name, error->message);
        job_free(j);
        xd_message_box(active_window(s), "Services", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
        g_free(name);
        g_error_free(error);
        return;
    }
    g_variant_get(r, "(o)", &j->path);
    g_variant_unref(r);
    char *result = nullptr;
    if (g_hash_table_steal_extended(j->m->done, j->path, nullptr, (gpointer *)&result)) {
        job_finished(j, result);
        g_free(result);
    }
}

/* StartUnit, StopUnit or RestartUnit, then wait for its job. */
static void unit_job(manager *m, const char *id, const char *method, const char *verb,
                     const char *ing) {
    if (m->bus == nullptr)
        return;
    job *j = g_new0(job, 1);
    j->m = m;
    j->id = g_strdup(id);
    j->verb = g_strdup(verb);
    jobs = g_list_prepend(jobs, j);
    job_dialog(j, ing);
    /* No timeout: polkit's password box may be up for a while. */
    g_dbus_connection_call(m->bus, SD_NAME, SD_PATH, SD_MANAGER, method,
                           g_variant_new("(ss)", id, "replace"), G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                           got_job, j);
}

static void action_on_selected(services *s, const char *method, const char *verb,
                               const char *ing) {
    manager *m = nullptr;
    char *id = nullptr, *active = nullptr;
    if (selected(s, &m, &id, &active))
        unit_job(m, id, method, verb, ing);
    g_free(id);
    g_free(active);
}

static void on_start([[maybe_unused]] GtkWidget *w, gpointer s) {
    action_on_selected(s, "StartUnit", "start", "start");
}
static void on_stop([[maybe_unused]] GtkWidget *w, gpointer s) {
    action_on_selected(s, "StopUnit", "stop", "stop");
}
static void on_restart([[maybe_unused]] GtkWidget *w, gpointer s) {
    action_on_selected(s, "RestartUnit", "restart", "restart");
}

/* ---- Properties ------------------------------------------------------------------------ */

typedef struct props {
    manager *m;
    char *id, *path;
    char *file_state; /* as loaded */
    GtkWidget *dialog, *startup, *status, *start, *stop, *apply;
    bool updating;
    /* Applying a startup type: its unit file calls, one after another. */
    GQueue calls;
    int choice;          /* what's being applied, or -1 */
    bool close_after;    /* OK: close once it's applied */
    GCancellable *cancel; /* cancelled when the box goes */
} props;

typedef struct unit_call {
    char *method, *rtype;
    GVariant *args;
} unit_call;

static void get_all(manager *m, const char *path, const char *iface, GAsyncReadyCallback done,
                    gpointer data) {
    g_dbus_connection_call(m->bus, SD_NAME, path, "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", iface), G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, done, data);
}

/* GetAll's dictionary, or nullptr. */
static GVariant *get_all_finish(GObject *src, GAsyncResult *res) {
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    if (r == nullptr)
        return nullptr;
    GVariant *dict = g_variant_get_child_value(r, 0);
    g_variant_unref(r);
    return dict;
}

static char *lookup_string(GVariant *dict, const char *key) {
    const char *v = nullptr;
    return dict != nullptr && g_variant_lookup(dict, key, "&s", &v) ? g_strdup(v) : nullptr;
}

static char **lookup_strv(GVariant *dict, const char *key) {
    char **v = nullptr;
    if (dict == nullptr || !g_variant_lookup(dict, key, "^as", &v))
        return nullptr;
    return v;
}

static void show_props_status(props *p) {
    GtkTreeIter *it = row(p->m, p->id);
    char *active = nullptr;
    if (it != nullptr)
        gtk_tree_model_get(GTK_TREE_MODEL(p->m->store), it, COL_ACTIVE, &active, -1);
    bool started = xd_unit_is_started(active);
    const char *label = xd_unit_status_label(active);
    gtk_label_set_text(GTK_LABEL(p->status), label[0] != '\0' ? label : "Stopped");
    gtk_widget_set_sensitive(p->start, !started && !busy(active));
    gtk_widget_set_sensitive(p->stop, started && !busy(active));
    g_free(active);
}

static void dialogs_update(manager *m, const char *id) {
    for (GList *l = m->dialogs; l != nullptr; l = l->next) {
        props *p = l->data;
        if (strcmp(p->id, id) == 0)
            show_props_status(p);
    }
}

static void on_props_start([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    unit_job(p->m, p->id, "StartUnit", "start", "start");
}

static void on_props_stop([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    unit_job(p->m, p->id, "StopUnit", "stop", "stop");
}

static void on_startup_changed([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    if (!p->updating)
        gtk_widget_set_sensitive(p->apply, TRUE);
}

static void unit_call_free(gpointer data) {
    unit_call *c = data;
    g_free(c->method);
    g_free(c->rtype);
    if (c->args != nullptr)
        g_variant_unref(c->args);
    g_free(c);
}

static void add_call(props *p, const char *method, GVariant *args, const char *rtype) {
    unit_call *c = g_new0(unit_call, 1);
    c->method = g_strdup(method);
    c->rtype = g_strdup(rtype);
    c->args = args != nullptr ? g_variant_ref_sink(args) : nullptr;
    g_queue_push_tail(&p->calls, c);
}

static GVariant *one_unit(const char *id) {
    const char *ids[] = {id, nullptr};
    return g_variant_new_strv(ids, -1);
}

static void apply_next(props *p);

/* Stops applying, after the last call or a failed one. */
static void apply_end(props *p, bool ok) {
    g_queue_clear_full(&p->calls, unit_call_free);
    gtk_widget_set_sensitive(p->startup, TRUE);
    if (ok) {
        static const char *const states[] = {"enabled", "disabled", "masked"};
        g_free(p->file_state);
        p->file_state = g_strdup(states[p->choice]);
        GtkTreeIter *it = row(p->m, p->id);
        if (it != nullptr)
            set_file_state(p->m, it, p->file_state);
    }
    gtk_widget_set_sensitive(p->apply, !ok);
    p->choice = -1;
    if (ok && p->close_after)
        gtk_widget_destroy(p->dialog);
}

static void unit_call_done(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error); /* the box is gone, and `data` with it */
        return;
    }
    props *p = data;
    if (r == nullptr) {
        g_dbus_error_strip_remote_error(error);
        char *text = g_strdup_printf("The startup type of the %s service could not be "
                                     "changed.\n\n%s",
                                     p->id, error->message);
        g_error_free(error);
        apply_end(p, false);
        xd_message_box(GTK_WINDOW(p->dialog), "Services", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
        return;
    }
    if (g_variant_is_of_type(r, G_VARIANT_TYPE("(ba(sss))"))) {
        gboolean install_info = TRUE;
        g_variant_get(r, "(ba(sss))", &install_info, nullptr);
        if (!install_info)
            xd_message_box(GTK_WINDOW(p->dialog), "Services", XD_MB_WARNING, XD_MB_OK, FALSE,
                           "This service's unit file has no [Install] section, so it can't "
                           "be started automatically. It runs when another service or "
                           "program needs it.");
    }
    g_variant_unref(r);
    apply_next(p);
}

static void apply_next(props *p) {
    unit_call *c = g_queue_pop_head(&p->calls);
    if (c == nullptr) {
        apply_end(p, true);
        return;
    }
    /* No timeout, and not waiting for it: polkit's password box may be up
     * for a while, and the window must keep working meanwhile. */
    g_dbus_connection_call(p->m->bus, SD_NAME, SD_PATH, SD_MANAGER, c->method, c->args,
                           c->rtype != nullptr ? G_VARIANT_TYPE(c->rtype) : nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, p->cancel,
                           unit_call_done, p);
    unit_call_free(c);
}

/* Starts applying the startup type chosen; false when there's nothing to
 * apply. */
static bool apply_startup(props *p) {
    int choice = gtk_combo_box_get_active(GTK_COMBO_BOX(p->startup));
    xd_unit_startup was = xd_unit_startup_of(p->file_state);
    if (choice < 0 || choice == (int)was || !gtk_widget_get_sensitive(p->startup))
        return false;
    if (was == XD_STARTUP_MASKED)
        add_call(p, "UnmaskUnitFiles", g_variant_new("(@asb)", one_unit(p->id), FALSE),
                 "(a(sss))");
    if (choice == XD_STARTUP_ENABLED)
        add_call(p, "EnableUnitFiles", g_variant_new("(@asbb)", one_unit(p->id), FALSE, FALSE),
                 "(ba(sss))");
    else if (choice == XD_STARTUP_DISABLED && was != XD_STARTUP_MASKED)
        add_call(p, "DisableUnitFiles", g_variant_new("(@asb)", one_unit(p->id), FALSE),
                 "(a(sss))");
    else if (choice == XD_STARTUP_MASKED)
        add_call(p, "MaskUnitFiles", g_variant_new("(@asbb)", one_unit(p->id), FALSE, FALSE),
                 "(a(sss))");
    /* systemd reads the changed links on its next reload. */
    add_call(p, "Reload", nullptr, nullptr);
    p->choice = choice;
    gtk_widget_set_sensitive(p->startup, FALSE);
    gtk_widget_set_sensitive(p->apply, FALSE);
    apply_next(p);
    return true;
}

static void on_props_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    bool apply = response == GTK_RESPONSE_APPLY || response == GTK_RESPONSE_OK;
    if (apply && p->choice >= 0) {
        /* Still applying: OK closes once it's done. */
        p->close_after |= response == GTK_RESPONSE_OK;
        return;
    }
    if (apply) {
        p->close_after = response == GTK_RESPONSE_OK;
        if (apply_startup(p) || response == GTK_RESPONSE_APPLY)
            return;
    }
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_props_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    /* A unit file call still running can't reach `p` now. */
    g_cancellable_cancel(p->cancel);
    g_object_unref(p->cancel);
    g_queue_clear_full(&p->calls, unit_call_free);
    p->m->dialogs = g_list_remove(p->m->dialogs, p);
    g_free(p->id);
    g_free(p->path);
    g_free(p->file_state);
    g_free(p);
}

static GtkWidget *label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *readonly_entry(const char *text) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), text != nullptr ? text : "");
    gtk_editable_set_editable(GTK_EDITABLE(e), FALSE);
    gtk_widget_set_hexpand(e, TRUE);
    return e;
}

static GtkWidget *scrolled(GtkWidget *child, int height) {
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sc, -1, height);
    gtk_container_add(GTK_CONTAINER(sc), child);
    return sc;
}

/* "Path to executable": the first ExecStart= command line. */
static char *exec_start(GVariant *service) {
    GVariant *exec = service != nullptr ? g_variant_lookup_value(service, "ExecStart",
                                                                 G_VARIANT_TYPE("a(sasbttttuii)"))
                                        : nullptr;
    char *out = nullptr;
    if (exec != nullptr && g_variant_n_children(exec) > 0) {
        GVariant *first = g_variant_get_child_value(exec, 0);
        const char *path = nullptr;
        char **argv = nullptr;
        g_variant_get_child(first, 0, "&s", &path);
        g_variant_get_child(first, 1, "^as", &argv);
        /* argv[0] is what the program's told it is; show the real path. */
        if (argv != nullptr && argv[0] != nullptr && path != nullptr) {
            g_free(argv[0]);
            argv[0] = g_strdup(path);
        }
        out = argv != nullptr && argv[0] != nullptr
                  ? xd_unit_command_line((const char *const *)argv)
                  : g_strdup(path);
        g_strfreev(argv);
        g_variant_unref(first);
    }
    if (exec != nullptr)
        g_variant_unref(exec);
    return out;
}

static GtkWidget *general_page(props *p, GVariant *unit, GVariant *service) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    int r = 0;
    gtk_grid_attach(GTK_GRID(g), label("Service name:"), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(g), label(p->id), 1, r++, 1, 1);
    char *name = xd_unit_display_name(p->id);
    gtk_grid_attach(GTK_GRID(g), label("Display name:"), 0, r, 1, 1);
    GtkWidget *display = readonly_entry(name);
    gtk_widget_set_sensitive(display, FALSE);
    gtk_grid_attach(GTK_GRID(g), display, 1, r++, 1, 1);
    g_free(name);

    char *desc = lookup_string(unit, "Description");
    GtkWidget *dl = label("Description:");
    gtk_widget_set_valign(dl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g), dl, 0, r, 1, 1);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)),
                             desc != nullptr ? desc : "", -1);
    gtk_grid_attach(GTK_GRID(g), scrolled(tv, 48), 1, r++, 1, 1);
    g_free(desc);
    gtk_box_pack_start(GTK_BOX(page), g, FALSE, FALSE, 0);

    GtkWidget *pl = label("Path to executable:");
    gtk_box_pack_start(GTK_BOX(page), pl, FALSE, FALSE, 0);
    char *exec = exec_start(service);
    gtk_box_pack_start(GTK_BOX(page), readonly_entry(exec != nullptr ? exec : "(none)"), FALSE,
                       FALSE, 0);
    g_free(exec);
    char *fragment = lookup_string(unit, "FragmentPath");
    if (fragment != nullptr && fragment[0] != '\0') {
        gtk_box_pack_start(GTK_BOX(page), label("Unit file:"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), readonly_entry(fragment), FALSE, FALSE, 0);
    }
    g_free(fragment);

    GtkWidget *st = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(st), label("Startup type:"), FALSE, FALSE, 0);
    p->startup = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->startup), "Enabled");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->startup), "Disabled");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->startup), "Masked");
    xd_unit_startup start = xd_unit_startup_of(p->file_state);
    if (start == XD_STARTUP_OTHER) {
        /* static, generated, ...: the unit file decides, nothing to set. */
        char *l = xd_unit_startup_label(p->file_state);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->startup), l[0] != '\0' ? l : "Unknown");
        g_free(l);
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->startup), 3);
        gtk_widget_set_sensitive(p->startup, FALSE);
    } else {
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->startup), start);
    }
    g_signal_connect(p->startup, "changed", G_CALLBACK(on_startup_changed), p);
    gtk_box_pack_start(GTK_BOX(st), p->startup, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), st, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       4);

    GtkWidget *sr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(sr), label("Service status:"), FALSE, FALSE, 0);
    p->status = label("");
    gtk_box_pack_start(GTK_BOX(sr), p->status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), sr, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    p->start = gtk_button_new_with_mnemonic("_Start");
    p->stop = gtk_button_new_with_mnemonic("S_top");
    GtkWidget *pause = gtk_button_new_with_mnemonic("_Pause");
    GtkWidget *resume = gtk_button_new_with_mnemonic("_Resume");
    /* systemd services can't be paused. */
    gtk_widget_set_sensitive(pause, FALSE);
    gtk_widget_set_sensitive(resume, FALSE);
    g_signal_connect(p->start, "clicked", G_CALLBACK(on_props_start), p);
    g_signal_connect(p->stop, "clicked", G_CALLBACK(on_props_stop), p);
    gtk_box_pack_start(GTK_BOX(buttons), p->start, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), p->stop, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), pause, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), resume, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    if (p->m->user)
        gtk_box_pack_start(GTK_BOX(page),
                           label("This service runs in your own service manager."), FALSE,
                           FALSE, 4);
    return page;
}

/* A tree of dependency kinds ("Requires", ...), each with its units. */
static GtkWidget *dep_tree(GVariant *unit, const char *const *keys, const char *const *titles,
                           size_t n) {
    GtkTreeStore *ts = gtk_tree_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    for (size_t i = 0; i < n; i++) {
        char **list = lookup_strv(unit, keys[i]);
        if (list == nullptr || list[0] == nullptr) {
            g_strfreev(list);
            continue;
        }
        GtkTreeIter parent;
        gtk_tree_store_insert_with_values(ts, &parent, nullptr, -1, 0, "folder", 1, titles[i], -1);
        for (char **u = list; *u != nullptr; u++)
            gtk_tree_store_insert_with_values(ts, nullptr, &parent, -1, 0, "system-run", 1, *u, -1);
        g_strfreev(list);
    }
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ts));
    g_object_unref(ts);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", 0);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", 1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tv));
    return scrolled(tv, 120);
}

static GtkWidget *dependencies_page(props *p, GVariant *unit) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    char *name = xd_unit_display_name(p->id);
    char *text = g_strdup_printf("Some services depend on other services, system drivers or "
                                 "load order groups. If a system component is stopped, or is "
                                 "not running properly, dependent services can be affected.");
    GtkWidget *intro = label(text);
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 56);
    gtk_box_pack_start(GTK_BOX(page), intro, FALSE, FALSE, 0);
    g_free(text);
    text = g_strdup_printf("%s depends on these system components:", name);
    gtk_box_pack_start(GTK_BOX(page), label(text), FALSE, FALSE, 4);
    g_free(text);
    static const char *const deps[] = {"Requires", "Wants", "After"};
    static const char *const dep_titles[] = {"Requires", "Wants", "Starts after"};
    gtk_box_pack_start(GTK_BOX(page), dep_tree(unit, deps, dep_titles, G_N_ELEMENTS(deps)), TRUE,
                       TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), label("These system components depend on it:"), FALSE,
                       FALSE, 4);
    static const char *const rdeps[] = {"RequiredBy", "WantedBy", "Before"};
    static const char *const rdep_titles[] = {"Required by", "Wanted by", "Starts before"};
    gtk_box_pack_start(GTK_BOX(page), dep_tree(unit, rdeps, rdep_titles, G_N_ELEMENTS(rdeps)),
                       TRUE, TRUE, 0);
    g_free(name);
    return page;
}

/* The Properties box already open for `id`, brought to the front. */
static bool present_properties(manager *m, const char *id) {
    for (GList *l = m->dialogs; l != nullptr; l = l->next) {
        props *open = l->data;
        if (strcmp(open->id, id) == 0) {
            gtk_window_present(GTK_WINDOW(open->dialog));
            return true;
        }
    }
    return false;
}

static void open_properties(manager *m, const char *id, const char *path, GVariant *unit,
                            GVariant *service) {
    /* Asked for twice while the unit loaded. */
    if (present_properties(m, id))
        return;
    services *s = m->app;
    props *p = g_new0(props, 1);
    p->m = m;
    p->id = g_strdup(id);
    g_queue_init(&p->calls);
    p->choice = -1;
    p->cancel = g_cancellable_new();
    p->path = g_strdup(path);
    p->file_state = lookup_string(unit, "UnitFileState");
    m->dialogs = g_list_prepend(m->dialogs, p);

    char *name = xd_unit_display_name(id);
    char *title = g_strdup_printf("%s Properties (%s)", name, m->user ? "Your Services" :
                                                                       "Local Computer");
    p->dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p->dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(p->dialog), GTK_WINDOW(s->window));
    gtk_window_set_icon_name(GTK_WINDOW(p->dialog), "system-run");
    gtk_window_set_resizable(GTK_WINDOW(p->dialog), FALSE);
    g_free(title);
    g_free(name);
    gtk_dialog_add_button(GTK_DIALOG(p->dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    p->apply = gtk_dialog_add_button(GTK_DIALOG(p->dialog), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(p->apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(p->dialog), GTK_RESPONSE_OK);

    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), general_page(p, unit, service),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), dependencies_page(p, unit),
                             gtk_label_new_with_mnemonic("_Dependencies"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p->dialog))), nb, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p->dialog, GTK_NOTEBOOK(nb));
    show_props_status(p);
    g_signal_connect(p->dialog, "response", G_CALLBACK(on_props_response), p);
    g_signal_connect(p->dialog, "destroy", G_CALLBACK(on_props_destroy), p);
    gtk_widget_show_all(p->dialog);
}

/* The unit, loaded and read by asynchronous calls before its box opens:
 * the manager may take its time, and the window doesn't wait. */
typedef struct props_load {
    manager *m;
    char *id, *path;
    GVariant *unit;
} props_load;

static void props_load_free(props_load *l) {
    g_free(l->id);
    g_free(l->path);
    if (l->unit != nullptr)
        g_variant_unref(l->unit);
    g_free(l);
}

static void on_service_read(GObject *src, GAsyncResult *res, gpointer data) {
    props_load *l = data;
    GVariant *service = get_all_finish(src, res);
    open_properties(l->m, l->id, l->path, l->unit, service);
    if (service != nullptr)
        g_variant_unref(service);
    props_load_free(l);
}

static void on_unit_read(GObject *src, GAsyncResult *res, gpointer data) {
    props_load *l = data;
    l->unit = get_all_finish(src, res);
    get_all(l->m, l->path, SD_SERVICE, on_service_read, l);
}

static void on_unit_loaded(GObject *src, GAsyncResult *res, gpointer data) {
    props_load *l = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        g_dbus_error_strip_remote_error(error);
        xd_message_box(GTK_WINDOW(l->m->app->window), "Services", XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
        props_load_free(l);
        return;
    }
    g_variant_get(r, "(o)", &l->path);
    g_variant_unref(r);
    get_all(l->m, l->path, SD_UNIT, on_unit_read, l);
}

static void properties(manager *m, const char *id) {
    if (present_properties(m, id))
        return;
    props_load *l = g_new0(props_load, 1);
    l->m = m;
    l->id = g_strdup(id);
    g_dbus_connection_call(m->bus, SD_NAME, SD_PATH, SD_MANAGER, "LoadUnit",
                           g_variant_new("(s)", id), G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, on_unit_loaded, l);
}

static void on_properties([[maybe_unused]] GtkWidget *w, gpointer data) {
    services *s = data;
    manager *m = nullptr;
    char *id = nullptr, *active = nullptr;
    if (selected(s, &m, &id, &active))
        properties(m, id);
    g_free(id);
    g_free(active);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    on_properties(nullptr, data);
}

static void on_refresh([[maybe_unused]] GtkWidget *w, gpointer data) {
    services *s = data;
    refresh(current(s));
}

/* ---- the window ------------------------------------------------------------------------ */

static GtkWidget *menu_item(GtkWidget *menu, const char *text, GCallback cb, gpointer data) {
    GtkWidget *i = gtk_menu_item_new_with_mnemonic(text);
    if (cb != nullptr)
        g_signal_connect(i, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
    return i;
}

static void separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* Start, Stop, Pause, Resume, Restart: in the Action menu and the
 * context menu. */
static GtkWidget *action_menu(services *s, bool keep) {
    GtkWidget *m = gtk_menu_new();
    GtkWidget *start = menu_item(m, "_Start", G_CALLBACK(on_start), s);
    GtkWidget *stop = menu_item(m, "St_op", G_CALLBACK(on_stop), s);
    GtkWidget *pause = menu_item(m, "_Pause", nullptr, nullptr);
    GtkWidget *resume = menu_item(m, "Resu_me", nullptr, nullptr);
    GtkWidget *restart = menu_item(m, "Restar_t", G_CALLBACK(on_restart), s);
    gtk_widget_set_sensitive(pause, FALSE);
    gtk_widget_set_sensitive(resume, FALSE);
    separator(m);
    menu_item(m, "Re_fresh", G_CALLBACK(on_refresh), s);
    separator(m);
    GtkWidget *prop = menu_item(m, "P_roperties", G_CALLBACK(on_properties), s);
    if (keep) {
        s->mi_start = start;
        s->mi_stop = stop;
        s->mi_pause = pause;
        s->mi_resume = resume;
        s->mi_restart = restart;
        s->mi_props = prop;
    } else {
        char *id = nullptr, *active = nullptr;
        bool have = selected(s, nullptr, &id, &active);
        bool started = have && xd_unit_is_started(active);
        bool idle = have && !busy(active);
        gtk_widget_set_sensitive(start, idle && !started);
        gtk_widget_set_sensitive(stop, idle && started);
        gtk_widget_set_sensitive(restart, idle && started);
        gtk_widget_set_sensitive(prop, have);
        g_free(id);
        g_free(active);
    }
    return m;
}

/* GTK deactivates a menu before running the chosen item, so destroying it
 * on "deactivate" would drop the click; destroy it once that's done. */
static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu_later(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static gboolean on_button_press(GtkWidget *view, GdkEventButton *ev, gpointer data) {
    services *s = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (int)ev->x, (int)ev->y, &path, nullptr,
                                      nullptr, nullptr)) {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), path);
        gtk_tree_path_free(path);
    }
    GtkWidget *menu = action_menu(s, false);
    gtk_menu_attach_to_widget(GTK_MENU(menu), view, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static void on_selection_changed([[maybe_unused]] GtkTreeSelection *sel, gpointer data) {
    update_actions(data);
}

static void on_switch_page([[maybe_unused]] GtkNotebook *nb, [[maybe_unused]] GtkWidget *page,
                           [[maybe_unused]] guint n, gpointer data) {
    services *s = data;
    /* After the switch: "switch-page" runs before the page changes. */
    g_idle_add_once((GSourceOnceFunc)(void (*)(void))update_actions, s);
    g_idle_add_once((GSourceOnceFunc)(void (*)(void))show_status, s);
}

static GtkWidget *list_view(services *s, manager *m) {
    m->store = gtk_list_store_new(NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(m->store), COL_NAME,
                                         GTK_SORT_ASCENDING);
    m->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(m->store));
    g_object_unref(m->store);
    g_signal_connect(m->store, "row-inserted", G_CALLBACK(on_row_inserted), m);
    static const struct {
        const char *title;
        int col, width;
    } cols[] = {
        {"Name", COL_NAME, 190},        {"Description", COL_DESC, 260},
        {"Status", COL_STATUS, 70},     {"Startup Type", COL_STARTUP, 90},
        {"Log On As", COL_LOGON, 100},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cols); i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, cols[i].title);
        if (cols[i].col == COL_NAME) {
            GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
            gtk_tree_view_column_pack_start(col, icon, FALSE);
            gtk_tree_view_column_add_attribute(col, icon, "icon-name", COL_ICON);
        }
        GtkCellRenderer *text = gtk_cell_renderer_text_new();
        g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        gtk_tree_view_column_pack_start(col, text, TRUE);
        gtk_tree_view_column_add_attribute(col, text, "text", cols[i].col);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, cols[i].width);
        gtk_tree_view_column_set_sort_column_id(col, cols[i].col);
        gtk_tree_view_append_column(GTK_TREE_VIEW(m->view), col);
    }
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(m->view), COL_NAME);
    g_signal_connect(m->view, "row-activated", G_CALLBACK(on_row_activated), s);
    g_signal_connect(m->view, "button-press-event", G_CALLBACK(on_button_press), s);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(m->view)), "changed",
                     G_CALLBACK(on_selection_changed), s);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), m->view);
    return sc;
}

static GtkToolItem *tool(GtkWidget *bar, const char *icon, const char *tip, GCallback cb,
                         gpointer data) {
    GtkToolItem *t = gtk_tool_button_new(nullptr, tip);
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(t), icon);
    gtk_widget_set_tooltip_text(GTK_WIDGET(t), tip);
    if (cb != nullptr)
        g_signal_connect(t, "clicked", cb, data);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), t, -1);
    return t;
}

static GtkWidget *link_button(const char *text, GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    GtkWidget *l = gtk_bin_get_child(GTK_BIN(b));
    char *markup = g_markup_printf_escaped("<span foreground=\"#0000ff\" underline=\"single\">%s"
                                           "</span>",
                                           text);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    g_free(markup);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

/* The extended view's left panel: the selected service, links to start
 * or stop it, and its description. */
static GtkWidget *extended_panel(services *s) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_widget_set_size_request(box, 190, -1);
    s->ext_name = label("");
    gtk_label_set_line_wrap(GTK_LABEL(s->ext_name), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(s->ext_name), 22);
    gtk_box_pack_start(GTK_BOX(box), s->ext_name, FALSE, FALSE, 0);
    s->ext_start = link_button("Start the service", G_CALLBACK(on_start), s);
    s->ext_stop = link_button("Stop the service", G_CALLBACK(on_stop), s);
    s->ext_restart = link_button("Restart the service", G_CALLBACK(on_restart), s);
    gtk_box_pack_start(GTK_BOX(box), s->ext_start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->ext_stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->ext_restart, FALSE, FALSE, 0);
    s->ext_desc = label("");
    gtk_label_set_line_wrap(GTK_LABEL(s->ext_desc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(s->ext_desc), 22);
    gtk_widget_set_margin_top(s->ext_desc, 10);
    gtk_box_pack_start(GTK_BOX(box), s->ext_desc, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(s->ext_start, TRUE);
    gtk_widget_set_no_show_all(s->ext_stop, TRUE);
    gtk_widget_set_no_show_all(s->ext_restart, TRUE);
    return box;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    if (ev->keyval == GDK_KEY_F5) {
        on_refresh(w, data);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Return && (ev->state & GDK_MOD1_MASK)) {
        on_properties(w, data);
        return TRUE;
    }
    return FALSE;
}

static void on_exit([[maybe_unused]] GtkWidget *w, gpointer data) {
    services *s = data;
    gtk_widget_destroy(s->window);
}

static void build(services *s) {
    s->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(s->window), "Services");
    gtk_window_set_icon_name(GTK_WINDOW(s->window), "system-run");
    gtk_window_set_default_size(GTK_WINDOW(s->window), 900, 560);
    g_signal_connect(s->window, "key-press-event", G_CALLBACK(on_key), s);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(s->window), vbox);
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = menu_item(bar, "_File", nullptr, nullptr);
    GtkWidget *fm = gtk_menu_new();
    menu_item(fm, "E_xit", G_CALLBACK(on_exit), s);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fm);
    GtkWidget *action = menu_item(bar, "_Action", nullptr, nullptr);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(action), action_menu(s, true));
    GtkWidget *view = menu_item(bar, "_View", nullptr, nullptr);
    GtkWidget *vm = gtk_menu_new();
    menu_item(vm, "_Refresh", G_CALLBACK(on_refresh), s);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), vm);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    GtkWidget *tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb), GTK_ICON_SIZE_SMALL_TOOLBAR);
    s->tb_props = GTK_WIDGET(tool(tb, "document-properties", "Properties",
                                  G_CALLBACK(on_properties), s));
    tool(tb, "view-refresh", "Refresh", G_CALLBACK(on_refresh), s);
    gtk_toolbar_insert(GTK_TOOLBAR(tb), gtk_separator_tool_item_new(), -1);
    s->tb_start = GTK_WIDGET(tool(tb, "media-playback-start", "Start Service",
                                  G_CALLBACK(on_start), s));
    s->tb_stop = GTK_WIDGET(tool(tb, "media-playback-stop", "Stop Service", G_CALLBACK(on_stop),
                                 s));
    s->tb_pause = GTK_WIDGET(tool(tb, "media-playback-pause", "Pause Service", nullptr, nullptr));
    gtk_widget_set_sensitive(s->tb_pause, FALSE);
    s->tb_restart = GTK_WIDGET(tool(tb, "view-refresh-symbolic", "Restart Service",
                                    G_CALLBACK(on_restart), s));
    gtk_box_pack_start(GTK_BOX(vbox), tb, FALSE, FALSE, 0);

    /* "Services (Local)", as the console's banner. */
    GtkWidget *banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(banner), 4);
    gtk_box_pack_start(GTK_BOX(banner), gtk_image_new_from_icon_name("system-run",
                                                                     GTK_ICON_SIZE_LARGE_TOOLBAR),
                       FALSE, FALSE, 0);
    GtkWidget *title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b><big>Services (Local)</big></b>");
    gtk_box_pack_start(GTK_BOX(banner), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), banner, FALSE, FALSE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), extended_panel(s), FALSE, FALSE, 0);
    s->notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(s->notebook), GTK_POS_BOTTOM);
    for (int i = 0; i < 2; i++) {
        manager *m = &s->mgr[i];
        gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), list_view(s, m),
                                 gtk_label_new_with_mnemonic(i == 0 ? "S_ystem" : "_User"));
    }
    g_signal_connect(s->notebook, "switch-page", G_CALLBACK(on_switch_page), s);
    gtk_box_pack_start(GTK_BOX(hbox), s->notebook, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

    s->status = label("");
    gtk_widget_set_margin_start(s->status, 6);
    gtk_widget_set_margin_top(s->status, 2);
    gtk_widget_set_margin_bottom(s->status, 2);
    gtk_box_pack_start(GTK_BOX(vbox), s->status, FALSE, FALSE, 0);
    g_signal_connect(s->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Services");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static services s;
    build(&s);
    for (int i = 0; i < 2; i++) {
        manager *m = &s.mgr[i];
        m->app = &s;
        m->user = i == 1;
        m->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        m->done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        manager_connect(m, m->user ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM);
        refresh(m);
    }
    /* xd-services --user opens on the User tab. */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--user") == 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(s.notebook), 1);
    gtk_widget_show_all(s.window);
    update_actions(&s);
    show_status(&s);
    gtk_main();
    return EXIT_SUCCESS;
}
