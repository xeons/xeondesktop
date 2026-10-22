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

static manager *current(services *console) {
    return &console
                ->mgr[gtk_notebook_get_current_page(GTK_NOTEBOOK(console->notebook)) == 1 ? 1 : 0];
}

static GtkTreeIter *row(manager *unit_manager, const char *id) {
    return g_hash_table_lookup(unit_manager->rows, id);
}

static GtkTreeIter *add_row(manager *unit_manager, const char *id) {
    GtkTreeIter *it = row(unit_manager, id);
    if (it != nullptr)
        return it;
    GtkTreeIter iter;
    char *name = xd_unit_display_name(id);
    gtk_list_store_insert_with_values(unit_manager->store, &iter, -1, COL_ICON, "system-run",
                                      COL_ID, id, COL_NAME, name, COL_DESC, "", COL_STATUS, "",
                                      COL_STARTUP, "", COL_LOGON, "", -1);
    g_free(name);
    it = g_memdup2(&iter, sizeof(iter));
    g_hash_table_insert(unit_manager->rows, g_strdup(id), it);
    return it;
}

static void set_active(manager *unit_manager, GtkTreeIter *it, const char *active) {
    gtk_list_store_set(unit_manager->store, it, COL_ACTIVE, active, COL_STATUS,
                       xd_unit_status_label(active), -1);
}

static void set_file_state(manager *unit_manager, GtkTreeIter *it, const char *state) {
    char *label = xd_unit_startup_label(state);
    gtk_list_store_set(unit_manager->store, it, COL_FILE, state, COL_STARTUP, label, -1);
    g_free(label);
}

static void set_logon(manager *unit_manager, GtkTreeIter *it, const char *user) {
    gtk_list_store_set(unit_manager->store, it, COL_LOGON,
                       xd_unit_logon_label(user, unit_manager->user, g_get_user_name()), -1);
}

/* The selected row's unit id and ActiveState, or false. */
static bool selected(services *console, manager **m_out, char **id, char **active) {
    manager *unit_manager = current(console);
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (unit_manager->view == nullptr ||
        !gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(unit_manager->view)), &model, &it))
        return false;
    gtk_tree_model_get(model, &it, COL_ID, id, COL_ACTIVE, active, -1);
    if (m_out != nullptr)
        *m_out = unit_manager;
    return true;
}

/* Starting or stopping: neither can be asked for until it's done. */
static bool busy(const char *active) {
    return g_strcmp0(active, "activating") == 0 || g_strcmp0(active, "deactivating") == 0;
}

/* The actions and the extended view follow the selection. */
static void update_actions(services *console) {
    char *id = nullptr, *active = nullptr, *desc = nullptr;
    manager *unit_manager = nullptr;
    bool have = selected(console, &unit_manager, &id, &active);
    bool started = have && xd_unit_is_started(active);
    bool idle = have && !busy(active);
    gtk_widget_set_sensitive(console->tb_start, idle && !started);
    gtk_widget_set_sensitive(console->mi_start, idle && !started);
    gtk_widget_set_sensitive(console->tb_stop, idle && started);
    gtk_widget_set_sensitive(console->mi_stop, idle && started);
    gtk_widget_set_sensitive(console->tb_restart, idle && started);
    gtk_widget_set_sensitive(console->mi_restart, idle && started);
    gtk_widget_set_sensitive(console->tb_props, have);
    gtk_widget_set_sensitive(console->mi_props, have);
    gtk_widget_set_visible(console->ext_start, have && !started);
    gtk_widget_set_visible(console->ext_stop, started);
    gtk_widget_set_visible(console->ext_restart, started);
    if (have) {
        GtkTreeIter *it = row(unit_manager, id);
        if (it != nullptr)
            gtk_tree_model_get(GTK_TREE_MODEL(unit_manager->store), it, COL_DESC, &desc, -1);
        char *name = xd_unit_display_name(id);
        char *markup = g_markup_printf_escaped("<b>%s</b>", name);
        gtk_label_set_markup(GTK_LABEL(console->ext_name), markup);
        g_free(markup);
        g_free(name);
        char *text = desc != nullptr && desc[0] != '\0'
                         ? g_strdup_printf("Description:\n%s", desc)
                         : g_strdup("");
        gtk_label_set_text(GTK_LABEL(console->ext_desc), text);
        g_free(text);
    } else {
        gtk_label_set_text(GTK_LABEL(console->ext_name), "Select an item to view its description.");
        gtk_label_set_text(GTK_LABEL(console->ext_desc), "");
    }
    g_free(id);
    g_free(active);
    g_free(desc);
}

static void show_status(services *console) {
    manager *unit_manager = current(console);
    char *text = unit_manager->error != nullptr
                     ? g_strdup(unit_manager->error)
                     : g_strdup_printf("%u service%s", g_hash_table_size(unit_manager->rows),
                                       g_hash_table_size(unit_manager->rows) == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(console->status), text);
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

static pending *pending_new(manager *unit_manager, const char *id) {
    pending *query = g_new0(pending, 1);
    query->m = unit_manager;
    query->generation = unit_manager->generation;
    query->id = g_strdup(id);
    return query;
}

static void pending_free(pending *query) {
    g_free(query->id);
    g_free(query);
}

static bool stale(pending *query) { return query->generation != query->m->generation; }

static void got_prop(GObject *src, GAsyncResult *res, gpointer data) {
    pending *query = data;
    GVariant *response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    GtkTreeIter *it = response != nullptr && !stale(query) ? row(query->m, query->id) : nullptr;
    if (it != nullptr) {
        GVariant *variant = nullptr;
        g_variant_get(response, "(v)", &variant);
        if (g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING)) {
            const char *value = g_variant_get_string(variant, nullptr);
            if (query->file_state) {
                /* Instances ("getty@tty1") aren't in ListUnitFiles. */
                char *known = nullptr;
                gtk_tree_model_get(GTK_TREE_MODEL(query->m->store), it, COL_FILE, &known, -1);
                if (known == nullptr)
                    set_file_state(query->m, it, value);
                g_free(known);
            } else {
                set_logon(query->m, it, value);
            }
        }
        g_variant_unref(variant);
    }
    if (response != nullptr)
        g_variant_unref(response);
    pending_free(query);
}

/* Log On As (the service's User=) and, for loaded units, the startup
 * type when no unit file listed it. */
static void fetch_user(manager *unit_manager, const char *id, const char *path) {
    g_dbus_connection_call(unit_manager->bus, SD_NAME, path, "org.freedesktop.DBus.Properties",
                           "Get", g_variant_new("(ss)", SD_SERVICE, "User"), G_VARIANT_TYPE("(v)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, got_prop,
                           pending_new(unit_manager, id));
}

static void fetch_file_state(manager *unit_manager, const char *id, const char *path) {
    pending *query = pending_new(unit_manager, id);
    query->file_state = true;
    g_dbus_connection_call(unit_manager->bus, SD_NAME, path, "org.freedesktop.DBus.Properties",
                           "Get", g_variant_new("(ss)", SD_UNIT, "UnitFileState"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, got_prop,
                           query);
}

/* A unit that isn't loaded: its description and state. */
static void got_unit_props(GObject *src, GAsyncResult *res, gpointer data) {
    pending *query = data;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    GtkTreeIter *it = stale(query) ? nullptr : row(query->m, query->id);
    if (variant != nullptr && it != nullptr) {
        GVariant *props = g_variant_get_child_value(variant, 0);
        const char *desc = nullptr, *active = nullptr;
        if (g_variant_lookup(props, "Description", "&s", &desc))
            gtk_list_store_set(query->m->store, it, COL_DESC, desc, -1);
        if (g_variant_lookup(props, "ActiveState", "&s", &active))
            set_active(query->m, it, active);
        g_variant_unref(props);
    }
    if (variant != nullptr)
        g_variant_unref(variant);
    pending_free(query);
}

static void got_loaded(GObject *src, GAsyncResult *res, gpointer data) {
    pending *query = data;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    GtkTreeIter *it = stale(query) ? nullptr : row(query->m, query->id);
    if (variant != nullptr && it != nullptr) {
        const char *path = nullptr;
        g_variant_get(variant, "(&o)", &path);
        gtk_list_store_set(query->m->store, it, COL_PATH, path, -1);
        g_dbus_connection_call(query->m->bus, SD_NAME, path, "org.freedesktop.DBus.Properties",
                               "GetAll", g_variant_new("(s)", SD_UNIT), G_VARIANT_TYPE("(a{sv})"),
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, got_unit_props,
                               pending_new(query->m, query->id));
        fetch_user(query->m, query->id, path);
    }
    if (variant != nullptr)
        g_variant_unref(variant);
    pending_free(query);
}

static bool is_service(const char *id) {
    /* Templates ("getty@.service") can't run by themselves; their
     * instances are listed as they're loaded. */
    return g_str_has_suffix(id, ".service") && !g_str_has_suffix(id, "@.service");
}

static void got_unit_files(GObject *src, GAsyncResult *res, gpointer data) {
    pending *query = data;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr) {
        g_printerr("xd-services: ListUnitFiles: %s\n", error->message);
        g_error_free(error);
    } else if (!stale(query)) {
        GVariantIter *iter = nullptr;
        const char *file = nullptr, *state = nullptr;
        g_variant_get(variant, "(a(ss))", &iter);
        while (g_variant_iter_next(iter, "(&s&s)", &file, &state)) {
            char *id = g_path_get_basename(file);
            if (is_service(id)) {
                bool known = row(query->m, id) != nullptr;
                GtkTreeIter *it = add_row(query->m, id);
                set_file_state(query->m, it, state);
                if (!known) {
                    /* Not loaded: load it for its description. */
                    set_active(query->m, it, "inactive");
                    set_logon(query->m, it, nullptr);
                    g_dbus_connection_call(query->m->bus, SD_NAME, SD_PATH, SD_MANAGER, "LoadUnit",
                                           g_variant_new("(s)", id), G_VARIANT_TYPE("(o)"),
                                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, got_loaded,
                                           pending_new(query->m, id));
                }
            }
            g_free(id);
        }
        g_variant_iter_free(iter);
        show_status(query->m->app);
    }
    if (variant != nullptr)
        g_variant_unref(variant);
    pending_free(query);
}

static void got_units(GObject *src, GAsyncResult *res, gpointer data) {
    pending *query = data;
    manager *unit_manager = query->m;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr) {
        g_dbus_error_strip_remote_error(error);
        if (!stale(query)) {
            g_free(unit_manager->error);
            unit_manager->error =
                g_strdup_printf("The %s service manager can't be reached: %s",
                                unit_manager->user ? "user" : "system", error->message);
            show_status(unit_manager->app);
        }
        g_error_free(error);
        pending_free(query);
        return;
    }
    if (!stale(query)) {
        GVariantIter *iter = nullptr;
        const char *id, *desc, *load, *active, *sub, *following, *path, *jtype, *jpath;
        guint32 job_id;
        g_variant_get(variant, "(a(ssssssouso))", &iter);
        while (g_variant_iter_next(iter, "(&s&s&s&s&s&s&ou&s&o)", &id, &desc, &load, &active,
                                   &sub, &following, &path, &job_id, &jtype, &jpath)) {
            if (!is_service(id) || strcmp(load, "not-found") == 0)
                continue;
            GtkTreeIter *it = add_row(unit_manager, id);
            gtk_list_store_set(unit_manager->store, it, COL_DESC, desc, COL_PATH, path, -1);
            set_active(unit_manager, it, active);
            set_logon(unit_manager, it, nullptr);
            fetch_user(unit_manager, id, path);
            if (strchr(id, '@') != nullptr)
                fetch_file_state(unit_manager, id, path);
        }
        g_variant_iter_free(iter);
        /* The installed unit files: their startup types, and the services
         * that aren't loaded. */
        g_dbus_connection_call(unit_manager->bus, SD_NAME, SD_PATH, SD_MANAGER, "ListUnitFiles",
                               nullptr, G_VARIANT_TYPE("(a(ss))"), G_DBUS_CALL_FLAGS_NONE, -1,
                               nullptr, got_unit_files, pending_new(unit_manager, nullptr));
        show_status(unit_manager->app);
    }
    g_variant_unref(variant);
    pending_free(query);
}

static void refresh(manager *unit_manager) {
    unit_manager->generation++;
    if (unit_manager->bus == nullptr)
        return;
    g_clear_pointer(&unit_manager->error, g_free);
    /* Keep the selection across the reload. */
    char *keep = nullptr;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(unit_manager->view)), &model, &it))
        gtk_tree_model_get(model, &it, COL_ID, &keep, -1);
    g_hash_table_remove_all(unit_manager->rows);
    gtk_list_store_clear(unit_manager->store);
    if (keep != nullptr) {
        /* Selected again when it comes back. */
        g_object_set_data_full(G_OBJECT(unit_manager->view), "keep", keep, g_free);
    }
    g_dbus_connection_call(unit_manager->bus, SD_NAME, SD_PATH, SD_MANAGER, "ListUnits", nullptr,
                           G_VARIANT_TYPE("(a(ssssssouso))"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           got_units, pending_new(unit_manager, nullptr));
}

/* Reselects the unit selected before a refresh once its row is back. */
static void on_row_inserted([[maybe_unused]] GtkTreeModel *model,
                            [[maybe_unused]] GtkTreePath *path, GtkTreeIter *it, gpointer data) {
    manager *unit_manager = data;
    const char *keep = g_object_get_data(G_OBJECT(unit_manager->view), "keep");
    if (keep == nullptr)
        return;
    char *id = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(unit_manager->store), it, COL_ID, &id, -1);
    /* The id is set in the same call that inserts the row. */
    if (g_strcmp0(id, keep) == 0) {
        gtk_tree_selection_select_iter(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(unit_manager->view)), it);
        g_object_set_data(G_OBJECT(unit_manager->view), "keep", nullptr);
    }
    g_free(id);
}

/* ---- live changes -------------------------------------------------------------------- */

static void dialogs_update(manager *unit_manager, const char *id);

static const char *id_for_path(manager *unit_manager, const char *path, GtkTreeIter **it_out) {
    GHashTableIter hi;
    gpointer key, value;
    g_hash_table_iter_init(&hi, unit_manager->rows);
    while (g_hash_table_iter_next(&hi, &key, &value)) {
        char *row_path = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(unit_manager->store), value, COL_PATH, &row_path, -1);
        bool match = g_strcmp0(row_path, path) == 0;
        g_free(row_path);
        if (match) {
            *it_out = value;
            return key;
        }
    }
    return nullptr;
}

static void on_props_changed([[maybe_unused]] GDBusConnection *connection,
                             [[maybe_unused]] const char *sender, const char *path,
                             [[maybe_unused]] const char *iface,
                             [[maybe_unused]] const char *signal, GVariant *params, gpointer data) {
    manager *unit_manager = data;
    const char *interface = nullptr;
    GVariant *changed = nullptr;
    g_variant_get(params, "(&s@a{sv}@as)", &interface, &changed, nullptr);
    const char *active = nullptr;
    if (strcmp(interface, SD_UNIT) == 0 && g_variant_lookup(changed, "ActiveState", "&s", &active)) {
        GtkTreeIter *it = nullptr;
        const char *id = id_for_path(unit_manager, path, &it);
        if (id != nullptr) {
            set_active(unit_manager, it, active);
            dialogs_update(unit_manager, id);
            update_actions(unit_manager->app);
        }
    }
    g_variant_unref(changed);
}

static void on_unit_files_changed([[maybe_unused]] GDBusConnection *connection,
                                  [[maybe_unused]] const char *sender,
                                  [[maybe_unused]] const char *path,
                                  [[maybe_unused]] const char *iface,
                                  [[maybe_unused]] const char *signal,
                                  [[maybe_unused]] GVariant *params, gpointer data) {
    manager *unit_manager = data;
    g_dbus_connection_call(unit_manager->bus, SD_NAME, SD_PATH, SD_MANAGER, "ListUnitFiles",
                           nullptr, G_VARIANT_TYPE("(a(ss))"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           got_unit_files, pending_new(unit_manager, nullptr));
}

static void job_finished(job *service_job, const char *result);

static GList *jobs; /* jobs waiting for JobRemoved */

static void on_job_removed([[maybe_unused]] GDBusConnection *connection,
                           [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                           [[maybe_unused]] const char *iface, [[maybe_unused]] const char *signal,
                           GVariant *params, gpointer data) {
    manager *unit_manager = data;
    guint32 id = 0;
    const char *jpath = nullptr, *unit = nullptr, *result = nullptr;
    g_variant_get(params, "(u&o&s&s)", &id, &jpath, &unit, &result);
    for (GList *list = jobs; list != nullptr; list = list->next) {
        job *service_job = list->data;
        if (service_job->m == unit_manager && g_strcmp0(service_job->path, jpath) == 0) {
            job_finished(service_job, result);
            return;
        }
    }
    /* Ours, perhaps, before its reply: kept until the reply comes. */
    if (jobs != nullptr)
        g_hash_table_insert(unit_manager->done, g_strdup(jpath), g_strdup(result));
}

static void manager_connect(manager *unit_manager, GBusType type) {
    GError *error = nullptr;
    unit_manager->bus = g_bus_get_sync(type, nullptr, &error);
    if (unit_manager->bus == nullptr) {
        unit_manager->error =
            g_strdup_printf("The %s service manager can't be reached: %s",
                            unit_manager->user ? "user" : "system", error->message);
        g_error_free(error);
        return;
    }
    g_dbus_connection_signal_subscribe(
        unit_manager->bus, SD_NAME, "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr,
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_props_changed, unit_manager, nullptr);
    g_dbus_connection_signal_subscribe(unit_manager->bus, SD_NAME, SD_MANAGER, "UnitFilesChanged",
                                       SD_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_unit_files_changed, unit_manager, nullptr);
    g_dbus_connection_signal_subscribe(unit_manager->bus, SD_NAME, SD_MANAGER, "JobRemoved",
                                       SD_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_job_removed,
                                       unit_manager, nullptr);
    /* systemd only sends its signals while someone has subscribed. */
    g_dbus_connection_call(unit_manager->bus, SD_NAME, SD_PATH, SD_MANAGER, "Subscribe", nullptr,
                           nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

/* ---- starting and stopping ----------------------------------------------------------- */

static GtkWindow *active_window(services *console) {
    for (GList *list = gtk_window_list_toplevels(); list != nullptr;
         list = g_list_delete_link(list, list)) {
        if (gtk_window_is_active(list->data)) {
            GtkWindow *active_toplevel = list->data;
            g_list_free(list);
            return active_toplevel;
        }
    }
    return GTK_WINDOW(console->window);
}

static void job_free(job *service_job) {
    jobs = g_list_remove(jobs, service_job);
    if (service_job->dialog != nullptr)
        gtk_widget_destroy(service_job->dialog); /* on_job_dialog_destroy stops the pulse */
    g_free(service_job->id);
    g_free(service_job->verb);
    g_free(service_job->path);
    g_free(service_job);
}

static void job_finished(job *service_job, const char *result) {
    services *console = service_job->m->app;
    manager *unit_manager = service_job->m;
    char *id = g_strdup(service_job->id), *verb = g_strdup(service_job->verb);
    job_free(service_job);
    if (jobs == nullptr)
        g_hash_table_remove_all(unit_manager->done);
    if (g_strcmp0(result, "done") != 0) {
        char *name = xd_unit_display_name(id);
        char *text = g_strdup_printf(
            "Could not %s the %s service on Local Computer.\n\nThe job ended with \"%s\". See "
            "the System log in Event Viewer for details.",
            verb, name, result != nullptr ? result : "failed");
        xd_message_box(active_window(console), "Services", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
        g_free(name);
    }
    g_free(id);
    g_free(verb);
    update_actions(console);
}

static gboolean pulse(gpointer data) {
    job *service_job = data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(service_job->bar));
    return G_SOURCE_CONTINUE;
}

static void on_job_dialog_response(GtkDialog *gtk_dialog, [[maybe_unused]] int response,
                                   [[maybe_unused]] gpointer data) {
    /* Closing the box doesn't cancel the job; it runs on. */
    gtk_widget_destroy(GTK_WIDGET(gtk_dialog));
}

/* Closed, or gone with the Properties box it was over: the progress bar
 * is gone too, so stop pulsing it. */
static void on_job_dialog_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    job *service_job = data;
    service_job->dialog = nullptr;
    service_job->bar = nullptr;
    if (service_job->pulse != 0) {
        g_source_remove(service_job->pulse);
        service_job->pulse = 0;
    }
}

/* XP's progress box: "Windows is attempting to start the following
 * service on Local Computer..." */
static void job_dialog(job *service_job, const char *ing) {
    services *console = service_job->m->app;
    service_job->dialog = gtk_dialog_new_with_buttons("Service Control", active_window(console),
                                                      GTK_DIALOG_DESTROY_WITH_PARENT, "_Close",
                                                      GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(service_job->dialog), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    char *text = g_strdup_printf("XeonDesktop is attempting to %s the following service on "
                                 "Local Computer...",
                                 ing);
    GtkWidget *attempt_label = gtk_label_new(text);
    g_free(text);
    gtk_label_set_xalign(GTK_LABEL(attempt_label), 0);
    gtk_box_pack_start(GTK_BOX(box), attempt_label, FALSE, FALSE, 0);
    char *name = xd_unit_display_name(service_job->id);
    GtkWidget *service_label = gtk_label_new(name);
    g_free(name);
    gtk_label_set_xalign(GTK_LABEL(service_label), 0);
    gtk_box_pack_start(GTK_BOX(box), service_label, FALSE, FALSE, 0);
    service_job->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(service_job->bar, 320, -1);
    gtk_box_pack_start(GTK_BOX(box), service_job->bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(service_job->dialog))),
                      box);
    g_signal_connect(service_job->dialog, "response", G_CALLBACK(on_job_dialog_response),
                     service_job);
    g_signal_connect(service_job->dialog, "destroy", G_CALLBACK(on_job_dialog_destroy),
                     service_job);
    service_job->pulse = g_timeout_add(100, pulse, service_job);
    gtk_widget_show_all(service_job->dialog);
}

static void got_job(GObject *src, GAsyncResult *res, gpointer data) {
    job *service_job = data;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr) {
        services *console = service_job->m->app;
        g_dbus_error_strip_remote_error(error);
        char *name = xd_unit_display_name(service_job->id);
        char *text = g_strdup_printf("Could not %s the %s service on Local Computer.\n\n%s",
                                     service_job->verb, name, error->message);
        job_free(service_job);
        xd_message_box(active_window(console), "Services", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
        g_free(name);
        g_error_free(error);
        return;
    }
    g_variant_get(variant, "(o)", &service_job->path);
    g_variant_unref(variant);
    char *result = nullptr;
    if (g_hash_table_steal_extended(service_job->m->done, service_job->path, nullptr,
                                    (gpointer *)&result)) {
        job_finished(service_job, result);
        g_free(result);
    }
}

/* StartUnit, StopUnit or RestartUnit, then wait for its job. */
static void unit_job(manager *unit_manager, const char *id, const char *method, const char *verb,
                     const char *ing) {
    if (unit_manager->bus == nullptr)
        return;
    job *service_job = g_new0(job, 1);
    service_job->m = unit_manager;
    service_job->id = g_strdup(id);
    service_job->verb = g_strdup(verb);
    jobs = g_list_prepend(jobs, service_job);
    job_dialog(service_job, ing);
    /* No timeout: polkit's password box may be up for a while. */
    g_dbus_connection_call(unit_manager->bus, SD_NAME, SD_PATH, SD_MANAGER, method,
                           g_variant_new("(ss)", id, "replace"), G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                           got_job, service_job);
}

static void action_on_selected(services *console, const char *method, const char *verb,
                               const char *ing) {
    manager *unit_manager = nullptr;
    char *id = nullptr, *active = nullptr;
    if (selected(console, &unit_manager, &id, &active))
        unit_job(unit_manager, id, method, verb, ing);
    g_free(id);
    g_free(active);
}

static void on_start([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    action_on_selected(user_data, "StartUnit", "start", "start");
}
static void on_stop([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    action_on_selected(user_data, "StopUnit", "stop", "stop");
}
static void on_restart([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    action_on_selected(user_data, "RestartUnit", "restart", "restart");
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

static void get_all(manager *unit_manager, const char *path, const char *iface,
                    GAsyncReadyCallback done, gpointer data) {
    g_dbus_connection_call(unit_manager->bus, SD_NAME, path, "org.freedesktop.DBus.Properties",
                           "GetAll", g_variant_new("(s)", iface), G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, done, data);
}

/* GetAll's dictionary, or nullptr. */
static GVariant *get_all_finish(GObject *src, GAsyncResult *res) {
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    if (variant == nullptr)
        return nullptr;
    GVariant *dict = g_variant_get_child_value(variant, 0);
    g_variant_unref(variant);
    return dict;
}

static char *lookup_string(GVariant *dict, const char *key) {
    const char *value = nullptr;
    return dict != nullptr && g_variant_lookup(dict, key, "&s", &value) ? g_strdup(value) : nullptr;
}

static char **lookup_strv(GVariant *dict, const char *key) {
    char **values = nullptr;
    if (dict == nullptr || !g_variant_lookup(dict, key, "^as", &values))
        return nullptr;
    return values;
}

static void show_props_status(props *properties) {
    GtkTreeIter *it = row(properties->m, properties->id);
    char *active = nullptr;
    if (it != nullptr)
        gtk_tree_model_get(GTK_TREE_MODEL(properties->m->store), it, COL_ACTIVE, &active, -1);
    bool started = xd_unit_is_started(active);
    const char *label = xd_unit_status_label(active);
    gtk_label_set_text(GTK_LABEL(properties->status), label[0] != '\0' ? label : "Stopped");
    gtk_widget_set_sensitive(properties->start, !started && !busy(active));
    gtk_widget_set_sensitive(properties->stop, started && !busy(active));
    g_free(active);
}

static void dialogs_update(manager *unit_manager, const char *id) {
    for (GList *list = unit_manager->dialogs; list != nullptr; list = list->next) {
        props *properties = list->data;
        if (strcmp(properties->id, id) == 0)
            show_props_status(properties);
    }
}

static void on_props_start([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    unit_job(properties->m, properties->id, "StartUnit", "start", "start");
}

static void on_props_stop([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    unit_job(properties->m, properties->id, "StopUnit", "stop", "stop");
}

static void on_startup_changed([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (!properties->updating)
        gtk_widget_set_sensitive(properties->apply, TRUE);
}

static void unit_call_free(gpointer data) {
    unit_call *call = data;
    g_free(call->method);
    g_free(call->rtype);
    if (call->args != nullptr)
        g_variant_unref(call->args);
    g_free(call);
}

static void add_call(props *properties, const char *method, GVariant *args, const char *rtype) {
    unit_call *call = g_new0(unit_call, 1);
    call->method = g_strdup(method);
    call->rtype = g_strdup(rtype);
    call->args = args != nullptr ? g_variant_ref_sink(args) : nullptr;
    g_queue_push_tail(&properties->calls, call);
}

static GVariant *one_unit(const char *id) {
    const char *ids[] = {id, nullptr};
    return g_variant_new_strv(ids, -1);
}

static void apply_next(props *properties);

/* Stops applying, after the last call or a failed one. */
static void apply_end(props *properties, bool ok) {
    g_queue_clear_full(&properties->calls, unit_call_free);
    gtk_widget_set_sensitive(properties->startup, TRUE);
    if (ok) {
        static const char *const states[] = {"enabled", "disabled", "masked"};
        g_free(properties->file_state);
        properties->file_state = g_strdup(states[properties->choice]);
        GtkTreeIter *it = row(properties->m, properties->id);
        if (it != nullptr)
            set_file_state(properties->m, it, properties->file_state);
    }
    gtk_widget_set_sensitive(properties->apply, !ok);
    properties->choice = -1;
    if (ok && properties->close_after)
        gtk_widget_destroy(properties->dialog);
}

static void unit_call_done(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error); /* the box is gone, and `data` with it */
        return;
    }
    props *properties = data;
    if (variant == nullptr) {
        g_dbus_error_strip_remote_error(error);
        char *text = g_strdup_printf("The startup type of the %s service could not be "
                                     "changed.\n\n%s",
                                     properties->id, error->message);
        g_error_free(error);
        apply_end(properties, false);
        xd_message_box(GTK_WINDOW(properties->dialog), "Services", XD_MB_ERROR, XD_MB_OK, FALSE,
                       text);
        g_free(text);
        return;
    }
    if (g_variant_is_of_type(variant, G_VARIANT_TYPE("(ba(sss))"))) {
        gboolean install_info = TRUE;
        g_variant_get(variant, "(ba(sss))", &install_info, nullptr);
        if (!install_info)
            xd_message_box(GTK_WINDOW(properties->dialog), "Services", XD_MB_WARNING, XD_MB_OK,
                           FALSE,
                           "This service's unit file has no [Install] section, so it can't "
                           "be started automatically. It runs when another service or "
                           "program needs it.");
    }
    g_variant_unref(variant);
    apply_next(properties);
}

static void apply_next(props *properties) {
    unit_call *call = g_queue_pop_head(&properties->calls);
    if (call == nullptr) {
        apply_end(properties, true);
        return;
    }
    /* No timeout, and not waiting for it: polkit's password box may be up
     * for a while, and the window must keep working meanwhile. */
    g_dbus_connection_call(properties->m->bus, SD_NAME, SD_PATH, SD_MANAGER, call->method,
                           call->args,
                           call->rtype != nullptr ? G_VARIANT_TYPE(call->rtype) : nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
                           properties->cancel, unit_call_done, properties);
    unit_call_free(call);
}

/* Starts applying the startup type chosen; false when there's nothing to
 * apply. */
static bool apply_startup(props *properties) {
    int choice = gtk_combo_box_get_active(GTK_COMBO_BOX(properties->startup));
    xd_unit_startup was = xd_unit_startup_of(properties->file_state);
    if (choice < 0 || choice == (int)was || !gtk_widget_get_sensitive(properties->startup))
        return false;
    if (was == XD_STARTUP_MASKED)
        add_call(properties, "UnmaskUnitFiles",
                 g_variant_new("(@asb)", one_unit(properties->id), FALSE), "(a(sss))");
    if (choice == XD_STARTUP_ENABLED)
        add_call(properties, "EnableUnitFiles",
                 g_variant_new("(@asbb)", one_unit(properties->id), FALSE, FALSE), "(ba(sss))");
    else if (choice == XD_STARTUP_DISABLED && was != XD_STARTUP_MASKED)
        add_call(properties, "DisableUnitFiles",
                 g_variant_new("(@asb)", one_unit(properties->id), FALSE), "(a(sss))");
    else if (choice == XD_STARTUP_MASKED)
        add_call(properties, "MaskUnitFiles",
                 g_variant_new("(@asbb)", one_unit(properties->id), FALSE, FALSE), "(a(sss))");
    /* systemd reads the changed links on its next reload. */
    add_call(properties, "Reload", nullptr, nullptr);
    properties->choice = choice;
    gtk_widget_set_sensitive(properties->startup, FALSE);
    gtk_widget_set_sensitive(properties->apply, FALSE);
    apply_next(properties);
    return true;
}

static void on_props_response(GtkDialog *gtk_dialog, int response, gpointer data) {
    props *properties = data;
    bool apply = response == GTK_RESPONSE_APPLY || response == GTK_RESPONSE_OK;
    if (apply && properties->choice >= 0) {
        /* Still applying: OK closes once it's done. */
        properties->close_after |= response == GTK_RESPONSE_OK;
        return;
    }
    if (apply) {
        properties->close_after = response == GTK_RESPONSE_OK;
        if (apply_startup(properties) || response == GTK_RESPONSE_APPLY)
            return;
    }
    gtk_widget_destroy(GTK_WIDGET(gtk_dialog));
}

static void on_props_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    /* A unit file call still running can't reach `properties` now. */
    g_cancellable_cancel(properties->cancel);
    g_object_unref(properties->cancel);
    g_queue_clear_full(&properties->calls, unit_call_free);
    properties->m->dialogs = g_list_remove(properties->m->dialogs, properties);
    g_free(properties->id);
    g_free(properties->path);
    g_free(properties->file_state);
    g_free(properties);
}

static GtkWidget *label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    return widget;
}

static GtkWidget *readonly_entry(const char *text) {
    GtkWidget *widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), text != nullptr ? text : "");
    gtk_editable_set_editable(GTK_EDITABLE(widget), FALSE);
    gtk_widget_set_hexpand(widget, TRUE);
    return widget;
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

static GtkWidget *general_page(props *properties, GVariant *unit, GVariant *service) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *widget = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(widget), 8);
    gtk_grid_set_column_spacing(GTK_GRID(widget), 12);
    int grid_row = 0;
    gtk_grid_attach(GTK_GRID(widget), label("Service name:"), 0, grid_row, 1, 1);
    gtk_grid_attach(GTK_GRID(widget), label(properties->id), 1, grid_row++, 1, 1);
    char *name = xd_unit_display_name(properties->id);
    gtk_grid_attach(GTK_GRID(widget), label("Display name:"), 0, grid_row, 1, 1);
    GtkWidget *display = readonly_entry(name);
    gtk_widget_set_sensitive(display, FALSE);
    gtk_grid_attach(GTK_GRID(widget), display, 1, grid_row++, 1, 1);
    g_free(name);

    char *desc = lookup_string(unit, "Description");
    GtkWidget *dl = label("Description:");
    gtk_widget_set_valign(dl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(widget), dl, 0, grid_row, 1, 1);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)),
                             desc != nullptr ? desc : "", -1);
    gtk_grid_attach(GTK_GRID(widget), scrolled(tv, 48), 1, grid_row++, 1, 1);
    g_free(desc);
    gtk_box_pack_start(GTK_BOX(page), widget, FALSE, FALSE, 0);

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
    properties->startup = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(properties->startup), "Enabled");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(properties->startup), "Disabled");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(properties->startup), "Masked");
    xd_unit_startup start = xd_unit_startup_of(properties->file_state);
    if (start == XD_STARTUP_OTHER) {
        /* static, generated, ...: the unit file decides, nothing to set. */
        char *startup_label = xd_unit_startup_label(properties->file_state);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(properties->startup),
                                       startup_label[0] != '\0' ? startup_label : "Unknown");
        g_free(startup_label);
        gtk_combo_box_set_active(GTK_COMBO_BOX(properties->startup), 3);
        gtk_widget_set_sensitive(properties->startup, FALSE);
    } else {
        gtk_combo_box_set_active(GTK_COMBO_BOX(properties->startup), start);
    }
    g_signal_connect(properties->startup, "changed", G_CALLBACK(on_startup_changed), properties);
    gtk_box_pack_start(GTK_BOX(st), properties->startup, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), st, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       4);

    GtkWidget *sr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(sr), label("Service status:"), FALSE, FALSE, 0);
    properties->status = label("");
    gtk_box_pack_start(GTK_BOX(sr), properties->status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), sr, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    properties->start = gtk_button_new_with_mnemonic("_Start");
    properties->stop = gtk_button_new_with_mnemonic("S_top");
    GtkWidget *pause = gtk_button_new_with_mnemonic("_Pause");
    GtkWidget *resume = gtk_button_new_with_mnemonic("_Resume");
    /* systemd services can't be paused. */
    gtk_widget_set_sensitive(pause, FALSE);
    gtk_widget_set_sensitive(resume, FALSE);
    g_signal_connect(properties->start, "clicked", G_CALLBACK(on_props_start), properties);
    g_signal_connect(properties->stop, "clicked", G_CALLBACK(on_props_stop), properties);
    gtk_box_pack_start(GTK_BOX(buttons), properties->start, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), properties->stop, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), pause, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), resume, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    if (properties->m->user)
        gtk_box_pack_start(GTK_BOX(page),
                           label("This service runs in your own service manager."), FALSE,
                           FALSE, 4);
    return page;
}

/* A tree of dependency kinds ("Requires", ...), each with its units. */
static GtkWidget *dep_tree(GVariant *unit, const char *const *keys, const char *const *titles,
                           size_t count) {
    GtkTreeStore *ts = gtk_tree_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    for (size_t i = 0; i < count; i++) {
        char **list = lookup_strv(unit, keys[i]);
        if (list == nullptr || list[0] == nullptr) {
            g_strfreev(list);
            continue;
        }
        GtkTreeIter parent;
        gtk_tree_store_insert_with_values(ts, &parent, nullptr, -1, 0, "folder", 1, titles[i], -1);
        for (char **dependency = list; *dependency != nullptr; dependency++)
            gtk_tree_store_insert_with_values(ts, nullptr, &parent, -1, 0, "system-run", 1,
                                              *dependency, -1);
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

static GtkWidget *dependencies_page(props *properties, GVariant *unit) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    char *name = xd_unit_display_name(properties->id);
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
static bool present_properties(manager *unit_manager, const char *id) {
    for (GList *list = unit_manager->dialogs; list != nullptr; list = list->next) {
        props *open = list->data;
        if (strcmp(open->id, id) == 0) {
            gtk_window_present(GTK_WINDOW(open->dialog));
            return true;
        }
    }
    return false;
}

static void open_properties(manager *unit_manager, const char *id, const char *path, GVariant *unit,
                            GVariant *service) {
    /* Asked for twice while the unit loaded. */
    if (present_properties(unit_manager, id))
        return;
    services *console = unit_manager->app;
    props *properties = g_new0(props, 1);
    properties->m = unit_manager;
    properties->id = g_strdup(id);
    g_queue_init(&properties->calls);
    properties->choice = -1;
    properties->cancel = g_cancellable_new();
    properties->path = g_strdup(path);
    properties->file_state = lookup_string(unit, "UnitFileState");
    unit_manager->dialogs = g_list_prepend(unit_manager->dialogs, properties);

    char *name = xd_unit_display_name(id);
    char *title = g_strdup_printf("%s Properties (%s)", name,
                                  unit_manager->user ? "Your Services" : "Local Computer");
    properties->dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties->dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(properties->dialog), GTK_WINDOW(console->window));
    gtk_window_set_icon_name(GTK_WINDOW(properties->dialog), "system-run");
    gtk_window_set_resizable(GTK_WINDOW(properties->dialog), FALSE);
    g_free(title);
    g_free(name);
    gtk_dialog_add_button(GTK_DIALOG(properties->dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    properties->apply =
        gtk_dialog_add_button(GTK_DIALOG(properties->dialog), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(properties->apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(properties->dialog), GTK_RESPONSE_OK);

    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), general_page(properties, unit, service),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), dependencies_page(properties, unit),
                             gtk_label_new_with_mnemonic("_Dependencies"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties->dialog))), nb,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties->dialog, GTK_NOTEBOOK(nb));
    show_props_status(properties);
    g_signal_connect(properties->dialog, "response", G_CALLBACK(on_props_response), properties);
    g_signal_connect(properties->dialog, "destroy", G_CALLBACK(on_props_destroy), properties);
    gtk_widget_show_all(properties->dialog);
}

/* The unit, loaded and read by asynchronous calls before its box opens:
 * the manager may take its time, and the window doesn't wait. */
typedef struct props_load {
    manager *m;
    char *id, *path;
    GVariant *unit;
} props_load;

static void props_load_free(props_load *loading) {
    g_free(loading->id);
    g_free(loading->path);
    if (loading->unit != nullptr)
        g_variant_unref(loading->unit);
    g_free(loading);
}

static void on_service_read(GObject *src, GAsyncResult *res, gpointer data) {
    props_load *loading = data;
    GVariant *service = get_all_finish(src, res);
    open_properties(loading->m, loading->id, loading->path, loading->unit, service);
    if (service != nullptr)
        g_variant_unref(service);
    props_load_free(loading);
}

static void on_unit_read(GObject *src, GAsyncResult *res, gpointer data) {
    props_load *loading = data;
    loading->unit = get_all_finish(src, res);
    get_all(loading->m, loading->path, SD_SERVICE, on_service_read, loading);
}

static void on_unit_loaded(GObject *src, GAsyncResult *res, gpointer data) {
    props_load *loading = data;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr) {
        g_dbus_error_strip_remote_error(error);
        xd_message_box(GTK_WINDOW(loading->m->app->window), "Services", XD_MB_ERROR, XD_MB_OK,
                       FALSE, error->message);
        g_error_free(error);
        props_load_free(loading);
        return;
    }
    g_variant_get(variant, "(o)", &loading->path);
    g_variant_unref(variant);
    get_all(loading->m, loading->path, SD_UNIT, on_unit_read, loading);
}

static void properties(manager *unit_manager, const char *id) {
    if (present_properties(unit_manager, id))
        return;
    props_load *loading = g_new0(props_load, 1);
    loading->m = unit_manager;
    loading->id = g_strdup(id);
    g_dbus_connection_call(unit_manager->bus, SD_NAME, SD_PATH, SD_MANAGER, "LoadUnit",
                           g_variant_new("(s)", id), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
                           5000, nullptr, on_unit_loaded, loading);
}

static void on_properties([[maybe_unused]] GtkWidget *widget, gpointer data) {
    services *console = data;
    manager *unit_manager = nullptr;
    char *id = nullptr, *active = nullptr;
    if (selected(console, &unit_manager, &id, &active))
        properties(unit_manager, id);
    g_free(id);
    g_free(active);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tree_view,
                             [[maybe_unused]] GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    on_properties(nullptr, data);
}

static void on_refresh([[maybe_unused]] GtkWidget *widget, gpointer data) {
    services *console = data;
    refresh(current(console));
}

/* ---- the window ------------------------------------------------------------------------ */

static GtkWidget *menu_item(GtkWidget *menu, const char *text, GCallback cb, gpointer data) {
    GtkWidget *widget = gtk_menu_item_new_with_mnemonic(text);
    if (cb != nullptr)
        g_signal_connect(widget, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), widget);
    return widget;
}

static void separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* Start, Stop, Pause, Resume, Restart: in the Action menu and the
 * context menu. */
static GtkWidget *action_menu(services *console, bool keep) {
    GtkWidget *widget = gtk_menu_new();
    GtkWidget *start = menu_item(widget, "_Start", G_CALLBACK(on_start), console);
    GtkWidget *stop = menu_item(widget, "St_op", G_CALLBACK(on_stop), console);
    GtkWidget *pause = menu_item(widget, "_Pause", nullptr, nullptr);
    GtkWidget *resume = menu_item(widget, "Resu_me", nullptr, nullptr);
    GtkWidget *restart = menu_item(widget, "Restar_t", G_CALLBACK(on_restart), console);
    gtk_widget_set_sensitive(pause, FALSE);
    gtk_widget_set_sensitive(resume, FALSE);
    separator(widget);
    menu_item(widget, "Re_fresh", G_CALLBACK(on_refresh), console);
    separator(widget);
    GtkWidget *prop = menu_item(widget, "P_roperties", G_CALLBACK(on_properties), console);
    if (keep) {
        console->mi_start = start;
        console->mi_stop = stop;
        console->mi_pause = pause;
        console->mi_resume = resume;
        console->mi_restart = restart;
        console->mi_props = prop;
    } else {
        char *id = nullptr, *active = nullptr;
        bool have = selected(console, nullptr, &id, &active);
        bool started = have && xd_unit_is_started(active);
        bool idle = have && !busy(active);
        gtk_widget_set_sensitive(start, idle && !started);
        gtk_widget_set_sensitive(stop, idle && started);
        gtk_widget_set_sensitive(restart, idle && started);
        gtk_widget_set_sensitive(prop, have);
        g_free(id);
        g_free(active);
    }
    return widget;
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
    services *console = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (int)ev->x, (int)ev->y, &path, nullptr,
                                      nullptr, nullptr)) {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), path);
        gtk_tree_path_free(path);
    }
    GtkWidget *menu = action_menu(console, false);
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
                           [[maybe_unused]] guint page_number, gpointer data) {
    services *console = data;
    /* After the switch: "switch-page" runs before the page changes. */
    g_idle_add_once((GSourceOnceFunc)(void (*)(void))update_actions, console);
    g_idle_add_once((GSourceOnceFunc)(void (*)(void))show_status, console);
}

static GtkWidget *list_view(services *console, manager *unit_manager) {
    unit_manager->store = gtk_list_store_new(
        NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(unit_manager->store), COL_NAME,
                                         GTK_SORT_ASCENDING);
    unit_manager->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(unit_manager->store));
    g_object_unref(unit_manager->store);
    g_signal_connect(unit_manager->store, "row-inserted", G_CALLBACK(on_row_inserted),
                     unit_manager);
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
        gtk_tree_view_append_column(GTK_TREE_VIEW(unit_manager->view), col);
    }
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(unit_manager->view), COL_NAME);
    g_signal_connect(unit_manager->view, "row-activated", G_CALLBACK(on_row_activated), console);
    g_signal_connect(unit_manager->view, "button-press-event", G_CALLBACK(on_button_press),
                     console);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(unit_manager->view)), "changed",
                     G_CALLBACK(on_selection_changed), console);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), unit_manager->view);
    return sc;
}

static GtkToolItem *tool(GtkWidget *bar, const char *icon, const char *tip, GCallback cb,
                         gpointer data) {
    GtkToolItem *tool_item = gtk_tool_button_new(nullptr, tip);
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(tool_item), icon);
    gtk_widget_set_tooltip_text(GTK_WIDGET(tool_item), tip);
    if (cb != nullptr)
        g_signal_connect(tool_item, "clicked", cb, data);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), tool_item, -1);
    return tool_item;
}

static GtkWidget *link_button(const char *text, GCallback cb, gpointer data) {
    GtkWidget *button = gtk_button_new_with_label(text);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    GtkWidget *child_label = gtk_bin_get_child(GTK_BIN(button));
    char *markup = g_markup_printf_escaped("<span foreground=\"#0000ff\" underline=\"single\">%s"
                                           "</span>",
                                           text);
    gtk_label_set_markup(GTK_LABEL(child_label), markup);
    g_free(markup);
    g_signal_connect(button, "clicked", cb, data);
    return button;
}

/* The extended view's left panel: the selected service, links to start
 * or stop it, and its description. */
static GtkWidget *extended_panel(services *console) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_widget_set_size_request(box, 190, -1);
    console->ext_name = label("");
    gtk_label_set_line_wrap(GTK_LABEL(console->ext_name), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(console->ext_name), 22);
    gtk_box_pack_start(GTK_BOX(box), console->ext_name, FALSE, FALSE, 0);
    console->ext_start = link_button("Start the service", G_CALLBACK(on_start), console);
    console->ext_stop = link_button("Stop the service", G_CALLBACK(on_stop), console);
    console->ext_restart = link_button("Restart the service", G_CALLBACK(on_restart), console);
    gtk_box_pack_start(GTK_BOX(box), console->ext_start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), console->ext_stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), console->ext_restart, FALSE, FALSE, 0);
    console->ext_desc = label("");
    gtk_label_set_line_wrap(GTK_LABEL(console->ext_desc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(console->ext_desc), 22);
    gtk_widget_set_margin_top(console->ext_desc, 10);
    gtk_box_pack_start(GTK_BOX(box), console->ext_desc, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(console->ext_start, TRUE);
    gtk_widget_set_no_show_all(console->ext_stop, TRUE);
    gtk_widget_set_no_show_all(console->ext_restart, TRUE);
    return box;
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data) {
    if (ev->keyval == GDK_KEY_F5) {
        on_refresh(widget, data);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Return && (ev->state & GDK_MOD1_MASK)) {
        on_properties(widget, data);
        return TRUE;
    }
    return FALSE;
}

static void on_exit([[maybe_unused]] GtkWidget *widget, gpointer data) {
    services *console = data;
    gtk_widget_destroy(console->window);
}

static void build(services *console) {
    console->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(console->window), "Services");
    gtk_window_set_icon_name(GTK_WINDOW(console->window), "system-run");
    gtk_window_set_default_size(GTK_WINDOW(console->window), 900, 560);
    g_signal_connect(console->window, "key-press-event", G_CALLBACK(on_key), console);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(console->window), vbox);
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = menu_item(bar, "_File", nullptr, nullptr);
    GtkWidget *fm = gtk_menu_new();
    menu_item(fm, "E_xit", G_CALLBACK(on_exit), console);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fm);
    GtkWidget *action = menu_item(bar, "_Action", nullptr, nullptr);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(action), action_menu(console, true));
    GtkWidget *view = menu_item(bar, "_View", nullptr, nullptr);
    GtkWidget *vm = gtk_menu_new();
    menu_item(vm, "_Refresh", G_CALLBACK(on_refresh), console);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), vm);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    GtkWidget *tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb), GTK_ICON_SIZE_SMALL_TOOLBAR);
    console->tb_props = GTK_WIDGET(
        tool(tb, "document-properties", "Properties", G_CALLBACK(on_properties), console));
    tool(tb, "view-refresh", "Refresh", G_CALLBACK(on_refresh), console);
    gtk_toolbar_insert(GTK_TOOLBAR(tb), gtk_separator_tool_item_new(), -1);
    console->tb_start = GTK_WIDGET(
        tool(tb, "media-playback-start", "Start Service", G_CALLBACK(on_start), console));
    console->tb_stop =
        GTK_WIDGET(tool(tb, "media-playback-stop", "Stop Service", G_CALLBACK(on_stop), console));
    console->tb_pause =
        GTK_WIDGET(tool(tb, "media-playback-pause", "Pause Service", nullptr, nullptr));
    gtk_widget_set_sensitive(console->tb_pause, FALSE);
    console->tb_restart = GTK_WIDGET(
        tool(tb, "view-refresh-symbolic", "Restart Service", G_CALLBACK(on_restart), console));
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
    gtk_box_pack_start(GTK_BOX(hbox), extended_panel(console), FALSE, FALSE, 0);
    console->notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(console->notebook), GTK_POS_BOTTOM);
    for (int i = 0; i < 2; i++) {
        manager *unit_manager = &console->mgr[i];
        gtk_notebook_append_page(GTK_NOTEBOOK(console->notebook), list_view(console, unit_manager),
                                 gtk_label_new_with_mnemonic(i == 0 ? "S_ystem" : "_User"));
    }
    g_signal_connect(console->notebook, "switch-page", G_CALLBACK(on_switch_page), console);
    gtk_box_pack_start(GTK_BOX(hbox), console->notebook, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

    console->status = label("");
    gtk_widget_set_margin_start(console->status, 6);
    gtk_widget_set_margin_top(console->status, 2);
    gtk_widget_set_margin_bottom(console->status, 2);
    gtk_box_pack_start(GTK_BOX(vbox), console->status, FALSE, FALSE, 0);
    g_signal_connect(console->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Services");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static services console;
    build(&console);
    for (int i = 0; i < 2; i++) {
        manager *unit_manager = &console.mgr[i];
        unit_manager->app = &console;
        unit_manager->user = i == 1;
        unit_manager->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        unit_manager->done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        manager_connect(unit_manager, unit_manager->user ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM);
        refresh(unit_manager);
    }
    /* xd-services --user opens on the User tab. */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--user") == 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(console.notebook), 1);
    gtk_widget_show_all(console.window);
    update_actions(&console);
    show_status(&console);
    gtk_main();
    return EXIT_SUCCESS;
}
