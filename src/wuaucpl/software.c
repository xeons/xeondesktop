/* SPDX-License-Identifier: MIT
 * Software Updates: PackageKit's GetUpdates and every Flatpak
 * installation's updates in one list, with what PackageKit's
 * GetUpdateDetail says about each; Install updates the checked ones
 * (UpdatePackages, then a Flatpak transaction per installation), each
 * asking polkit itself. And the shell's scheduled check, which never
 * installs.
 */
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "../appwiz/programs.h"
#include "../gtkapp/gtkapp.h"
#include "updlib.h"
#include "wuaucpl.h"

enum {
    COL_SELECTED,
    COL_NAME,
    COL_VERSION,
    COL_IMPORTANCE,
    COL_SOURCE,
    COL_KIND,
    COL_ID,           /* a package id or a Flatpak ref */
    COL_INSTALLATION, /* Flatpak: "user" or a system installation's id */
    COL_RANK,
    COL_SUMMARY,
    COL_DETAIL, /* GetUpdateDetail's text */
    COL_RESTART,
    COL_OLD, /* the version it replaces */
    COL_COUNT
};

enum { KIND_PACKAGE, KIND_FLATPAK };

/* ---- Flatpak ---------------------------------------------------------------------- */

typedef struct fp_update {
    char *installation, *ref, *name, *version, *origin;
} fp_update;

static void fp_update_free(gpointer user_data) {
    fp_update *flatpak_update = user_data;
    g_free(flatpak_update->installation);
    g_free(flatpak_update->ref);
    g_free(flatpak_update->name);
    g_free(flatpak_update->version);
    g_free(flatpak_update->origin);
    g_free(flatpak_update);
}

static FlatpakInstallation *open_installation(const char *id, GCancellable *cancel,
                                              GError **error) {
    if (strcmp(id, "user") == 0)
        return flatpak_installation_new_user(cancel, error);
    return flatpak_installation_new_system_with_id(strcmp(id, "default") == 0 ? nullptr : id,
                                                   cancel, error);
}

/* The user's installation and the system ones. */
static GPtrArray *installations(GCancellable *cancel) {
    GPtrArray *all = g_ptr_array_new_with_free_func(g_object_unref);
    FlatpakInstallation *user = flatpak_installation_new_user(cancel, nullptr);
    if (user != nullptr)
        g_ptr_array_add(all, user);
    GPtrArray *system = flatpak_get_system_installations(cancel, nullptr);
    for (guint i = 0; system != nullptr && i < system->len; i++)
        g_ptr_array_add(all, g_object_ref(g_ptr_array_index(system, i)));
    if (system != nullptr)
        g_ptr_array_unref(system);
    return all;
}

static const char *installation_id(FlatpakInstallation *inst) {
    return flatpak_installation_get_is_user(inst) ? "user" : flatpak_installation_get_id(inst);
}

/* Every installed app and runtime with an update (asks the remotes:
 * network, so not on the main thread). */
static GPtrArray *flatpak_updates(GCancellable *cancel) {
    GPtrArray *out = g_ptr_array_new_with_free_func(fp_update_free);
    GPtrArray *all = installations(cancel);
    for (guint i = 0; i < all->len; i++) {
        FlatpakInstallation *inst = g_ptr_array_index(all, i);
        GPtrArray *refs = flatpak_installation_list_installed_refs_for_update(inst, cancel,
                                                                              nullptr);
        for (guint j = 0; refs != nullptr && j < refs->len; j++) {
            FlatpakInstalledRef *installed = g_ptr_array_index(refs, j);
            FlatpakRef *ref = FLATPAK_REF(installed);
            fp_update *flatpak_update = g_new0(fp_update, 1);
            flatpak_update->installation = g_strdup(installation_id(inst));
            flatpak_update->ref = flatpak_ref_format_ref(ref);
            const char *name = flatpak_installed_ref_get_appdata_name(installed);
            flatpak_update->name = g_strdup(name != nullptr ? name : flatpak_ref_get_name(ref));
            const char *version = flatpak_installed_ref_get_appdata_version(installed);
            flatpak_update->version =
                g_strdup(version != nullptr ? version : flatpak_ref_get_branch(ref));
            flatpak_update->origin = g_strdup(flatpak_installed_ref_get_origin(installed));
            g_ptr_array_add(out, flatpak_update);
        }
        if (refs != nullptr)
            g_ptr_array_unref(refs);
    }
    g_ptr_array_unref(all);
    return out;
}

/* ---- the list ---------------------------------------------------------------------- */

typedef struct sw_ctx {
    wuau *w;
    unsigned generation;
} sw_ctx;

static sw_ctx *ctx_new(wuau *applet) {
    sw_ctx *software_call = g_new0(sw_ctx, 1);
    *software_call = (sw_ctx){applet, applet->sw_generation};
    return software_call;
}

/* A reply for the list as it is now; frees the context if not. */
static bool current(sw_ctx *software_call) {
    bool ok = software_call->generation == software_call->w->sw_generation &&
              !g_cancellable_is_cancelled(software_call->w->quit);
    if (!ok)
        g_free(software_call);
    return ok;
}

static void set_status(wuau *applet, const char *text) {
    gtk_label_set_text(GTK_LABEL(applet->sw_status), text);
}

static bool any_selected(wuau *applet) {
    GtkTreeIter it;
    GtkTreeModel *model = GTK_TREE_MODEL(applet->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        gboolean on = FALSE;
        gtk_tree_model_get(model, &it, COL_SELECTED, &on, -1);
        if (on)
            return true;
    }
    return false;
}

static void sync_buttons(wuau *applet) {
    gtk_widget_set_sensitive(applet->sw_install,
                             !applet->busy && applet->sw_pending == 0 && any_selected(applet));
    gtk_widget_set_sensitive(applet->sw_check, !applet->busy && applet->sw_pending == 0);
}

int xd_wuau_software_count(wuau *applet, int *important, GPtrArray *ids) {
    int count = 0;
    GtkTreeIter it;
    GtkTreeModel *model = GTK_TREE_MODEL(applet->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        int rank = 0;
        char *id = nullptr;
        gtk_tree_model_get(model, &it, COL_RANK, &rank, COL_ID, &id, -1);
        count++;
        if (xd_update_rank_important((xd_update_rank)rank))
            ++*important;
        g_ptr_array_add(ids, id);
    }
    return count;
}

static void show_count(wuau *applet) {
    int important = 0;
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    int count = xd_wuau_software_count(applet, &important, ids);
    g_ptr_array_free(ids, TRUE);
    char *text;
    if (count == 0)
        text = g_strdup("Your computer is up to date: there are no updates for its programs.");
    else if (important > 0)
        text = g_strdup_printf("%d %s available for your computer, %d of them important. "
                               "Check the updates you want, then click Install.",
                               count, count == 1 ? "update is" : "updates are", important);
    else
        text = g_strdup_printf("%d %s available for your computer. Check the updates you want, "
                               "then click Install.",
                               count, count == 1 ? "update is" : "updates are");
    set_status(applet, text);
    g_free(text);
}

static void query_done(wuau *applet, bool failed) {
    applet->sw_failed = applet->sw_failed || failed;
    if (--applet->sw_pending > 0)
        return;
    bool checked = applet->sw_refresh;
    applet->sw_refresh = false;
    /* Without PackageKit's answer the count would be wrong: the shell
     * keeps what the last check found. */
    if (!applet->sw_failed) {
        show_count(applet);
        xd_wuau_save_state(applet, checked);
    }
    sync_buttons(applet);
}

/* Fills the details below the list from the selected row. */
static void show_details(wuau *applet) {
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(applet->sw_view));
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        gtk_label_set_text(GTK_LABEL(applet->sw_details), "Select an update to see its details.");
        return;
    }
    char *name = nullptr, *version = nullptr, *old = nullptr, *summary = nullptr;
    char *detail = nullptr, *importance = nullptr, *source = nullptr;
    unsigned restart = 0;
    gtk_tree_model_get(model, &it, COL_NAME, &name, COL_VERSION, &version, COL_OLD, &old,
                       COL_SUMMARY, &summary, COL_DETAIL, &detail, COL_IMPORTANCE, &importance,
                       COL_SOURCE, &source, COL_RESTART, &restart, -1);
    GString *string = g_string_new(nullptr);
    char *line = g_markup_printf_escaped("<b>%s</b>", name);
    g_string_append(string, line);
    g_free(line);
    if (summary != nullptr && summary[0] != '\0') {
        line = g_markup_printf_escaped(": %s", summary);
        g_string_append(string, line);
        g_free(line);
    }
    line = old != nullptr && old[0] != '\0'
               ? g_markup_printf_escaped("\nVersion %s replaces %s.", version, old)
               : g_markup_printf_escaped("\nVersion %s.", version);
    g_string_append(string, line);
    g_free(line);
    if (importance != nullptr && importance[0] != '\0') {
        line = g_markup_printf_escaped(" Importance: %s.", importance);
        g_string_append(string, line);
        g_free(line);
    }
    line = g_markup_printf_escaped(" From %s.", source);
    g_string_append(string, line);
    g_free(line);
    if (detail != nullptr && detail[0] != '\0') {
        line = g_markup_printf_escaped("\n\n%s", detail);
        g_string_append(string, line);
        g_free(line);
    }
    const char *hint = xd_update_restart_text((PkRestartEnum)restart);
    if (hint != nullptr) {
        line = g_markup_printf_escaped("\n\n%s", hint);
        g_string_append(string, line);
        g_free(line);
    }
    gtk_label_set_markup(GTK_LABEL(applet->sw_details), string->str);
    g_string_free(string, TRUE);
    g_free(name);
    g_free(version);
    g_free(old);
    g_free(summary);
    g_free(detail);
    g_free(importance);
    g_free(source);
}

static bool find_row(wuau *applet, const char *id, GtkTreeIter *out) {
    GtkTreeModel *model = GTK_TREE_MODEL(applet->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(model, out); ok;
         ok = gtk_tree_model_iter_next(model, out)) {
        char *row = nullptr;
        gtk_tree_model_get(model, out, COL_ID, &row, -1);
        bool same = g_strcmp0(row, id) == 0;
        g_free(row);
        if (same)
            return true;
    }
    return false;
}

static void on_details(GObject *src, GAsyncResult *res, gpointer data) {
    sw_ctx *software_call = data;
    PkResults *results = pk_client_generic_finish(PK_CLIENT(src), res, nullptr);
    if (!current(software_call)) {
        g_clear_object(&results);
        return;
    }
    wuau *applet = software_call->w;
    g_free(software_call);
    GPtrArray *details = results != nullptr ? pk_results_get_update_detail_array(results) : nullptr;
    for (guint i = 0; details != nullptr && i < details->len; i++) {
        PkUpdateDetail *detail = g_ptr_array_index(details, i);
        GtkTreeIter it;
        if (!find_row(applet, pk_update_detail_get_package_id(detail), &it))
            continue;
        GString *text = g_string_new(pk_update_detail_get_update_text(detail));
        g_strstrip(text->str);
        text->len = strlen(text->str);
        char **cves = pk_update_detail_get_cve_urls(detail);
        for (char **cve_url = cves; cve_url != nullptr && *cve_url != nullptr; cve_url++)
            g_string_append_printf(text, "%sSecurity issue: %s", text->len > 0 ? "\n" : "",
                                   *cve_url);
        char *old = nullptr;
        char **replaces = pk_update_detail_get_updates(detail);
        if (replaces != nullptr && replaces[0] != nullptr)
            xd_appwiz_split_package_id(replaces[0], nullptr, &old, nullptr);
        PkRestartEnum restart = pk_update_detail_get_restart(detail);
        gtk_list_store_set(applet->sw_store, &it, COL_DETAIL, text->str, COL_RESTART,
                           (unsigned)restart, COL_OLD, old, -1);
        g_free(old);
        g_string_free(text, TRUE);
    }
    if (details != nullptr)
        g_ptr_array_unref(details);
    g_clear_object(&results);
    show_details(applet);
    query_done(applet, false);
}

static void on_updates(GObject *src, GAsyncResult *res, gpointer data) {
    sw_ctx *software_call = data;
    GError *error = nullptr;
    PkResults *results = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (!current(software_call)) {
        g_clear_error(&error);
        g_clear_object(&results);
        return;
    }
    wuau *applet = software_call->w;
    PkError *pe = results != nullptr ? pk_results_get_error_code(results) : nullptr;
    if (error != nullptr || pe != nullptr) {
        g_free(software_call);
        char *text = g_strdup_printf("PackageKit couldn't list the updates: %s",
                                     error != nullptr ? error->message
                                                      : pk_error_get_details(pe));
        set_status(applet, text);
        g_free(text);
        g_clear_error(&error);
        g_clear_object(&pe);
        g_clear_object(&results);
        query_done(applet, true);
        return;
    }
    GPtrArray *pkgs = pk_results_get_package_array(results);
    char **ids = g_new0(char *, pkgs->len + 1);
    for (guint i = 0; i < pkgs->len; i++) {
        PkPackage *package = g_ptr_array_index(pkgs, i);
        xd_update_rank rank = xd_update_rank_of_package(pk_package_get_info(package),
                                                        pk_package_get_update_severity(package));
        char *source = xd_appwiz_package_source(applet->backend, pk_package_get_id(package));
        gtk_list_store_insert_with_values(
            applet->sw_store, nullptr, -1, COL_SELECTED, TRUE, COL_NAME,
            pk_package_get_name(package), COL_VERSION, pk_package_get_version(package),
            COL_IMPORTANCE, xd_update_rank_name(rank), COL_SOURCE, source, COL_KIND, KIND_PACKAGE,
            COL_ID, pk_package_get_id(package), COL_RANK, (int)rank, COL_SUMMARY,
            pk_package_get_summary(package), -1);
        g_free(source);
        ids[i] = g_strdup(pk_package_get_id(package));
    }
    g_ptr_array_unref(pkgs);
    g_clear_object(&results);
    if (ids[0] != nullptr) {
        /* What each fixes and whether it needs a restart. */
        applet->sw_pending++;
        pk_client_get_update_detail_async(applet->pk, ids, applet->quit, nullptr, nullptr,
                                          on_details, software_call);
    } else {
        g_free(software_call);
    }
    g_strfreev(ids);
    query_done(applet, false);
}

static void fp_list_thread(GTask *task, [[maybe_unused]] gpointer src,
                           [[maybe_unused]] gpointer data, GCancellable *cancel) {
    g_task_return_pointer(task, flatpak_updates(cancel), (GDestroyNotify)g_ptr_array_unref);
}

static void on_flatpaks([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    sw_ctx *software_call = data;
    GPtrArray *list = g_task_propagate_pointer(G_TASK(res), nullptr);
    if (!current(software_call)) {
        if (list != nullptr)
            g_ptr_array_unref(list);
        return;
    }
    wuau *applet = software_call->w;
    g_free(software_call);
    for (guint i = 0; list != nullptr && i < list->len; i++) {
        fp_update *flatpak_update = g_ptr_array_index(list, i);
        char *source = xd_appwiz_flatpak_source(flatpak_update->origin,
                                                strcmp(flatpak_update->installation, "default") == 0
                                                    ? "system"
                                                    : flatpak_update->installation);
        gtk_list_store_insert_with_values(
            applet->sw_store, nullptr, -1, COL_SELECTED, TRUE, COL_NAME, flatpak_update->name,
            COL_VERSION, flatpak_update->version, COL_IMPORTANCE, "", COL_SOURCE, source, COL_KIND,
            KIND_FLATPAK, COL_ID, flatpak_update->ref, COL_INSTALLATION,
            flatpak_update->installation, COL_RANK, (int)XD_RANK_UNKNOWN, COL_SUMMARY, "", -1);
        g_free(source);
    }
    if (list != nullptr)
        g_ptr_array_unref(list);
    query_done(applet, false);
}

static void list(wuau *applet) {
    applet->sw_pending += 2;
    pk_client_get_updates_async(applet->pk, pk_bitfield_value(PK_FILTER_ENUM_NONE), applet->quit,
                                nullptr, nullptr, on_updates, ctx_new(applet));
    GTask *task = g_task_new(nullptr, applet->quit, on_flatpaks, ctx_new(applet));
    g_task_run_in_thread(task, fp_list_thread);
    g_object_unref(task);
}

static void on_refreshed(GObject *src, GAsyncResult *res, gpointer data) {
    sw_ctx *software_call = data;
    GError *error = nullptr;
    PkResults *results = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (!current(software_call)) {
        g_clear_error(&error);
        g_clear_object(&results);
        return;
    }
    wuau *applet = software_call->w;
    g_free(software_call);
    PkError *pe = results != nullptr ? pk_results_get_error_code(results) : nullptr;
    if (error != nullptr || pe != nullptr) {
        /* Still list what the old lists know. */
        xd_wuau_error(applet, "The list of updates could not be refreshed.",
                      error != nullptr ? error->message : pk_error_get_details(pe));
        applet->sw_refresh = false;
    }
    g_clear_error(&error);
    g_clear_object(&pe);
    g_clear_object(&results);
    list(applet);
    query_done(applet, false);
}

void xd_wuau_software_load(wuau *applet) {
    applet->sw_generation++;
    applet->sw_pending = 0;
    applet->sw_failed = false;
    gtk_list_store_clear(applet->sw_store);
    set_status(applet, applet->sw_refresh ? "Checking for updates..." : "Looking for updates...");
    if (applet->sw_refresh) {
        applet->sw_pending++;
        pk_client_refresh_cache_async(applet->pk_task, FALSE, applet->quit, nullptr, nullptr,
                                      on_refreshed, ctx_new(applet));
    } else {
        list(applet);
    }
    sync_buttons(applet);
}

static void on_check([[maybe_unused]] GtkButton *button, gpointer data) {
    wuau *applet = data;
    applet->sw_refresh = true;
    xd_wuau_software_load(applet);
}

static void on_toggled([[maybe_unused]] GtkCellRendererToggle *renderer, char *path,
                       gpointer data) {
    wuau *applet = data;
    GtkTreeIter it;
    if (applet->busy ||
        !gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(applet->sw_store), &it, path))
        return;
    gboolean on = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(applet->sw_store), &it, COL_SELECTED, &on, -1);
    gtk_list_store_set(applet->sw_store, &it, COL_SELECTED, !on, -1);
    sync_buttons(applet);
}

static void on_selection([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    show_details(data);
}

/* Most important first, then by name. */
static int compare_rows(GtkTreeModel *model, GtkTreeIter *left, GtkTreeIter *right,
                        [[maybe_unused]] gpointer data) {
    int ra = 0, rb = 0;
    char *na = nullptr, *nb = nullptr;
    gtk_tree_model_get(model, left, COL_RANK, &ra, COL_NAME, &na, -1);
    gtk_tree_model_get(model, right, COL_RANK, &rb, COL_NAME, &nb, -1);
    int order =
        ra != rb ? rb - ra : g_utf8_collate(na != nullptr ? na : "", nb != nullptr ? nb : "");
    g_free(na);
    g_free(nb);
    return order;
}

/* ---- installing -------------------------------------------------------------------- */

typedef struct install {
    wuau *w;
    GCancellable *cancel;
    xd_wuau_progress *progress;
    char **pk_ids;
    GPtrArray *flatpaks; /* fp_update *: installation, ref, name */
    GPtrArray *names;    /* char *: every update's name, for the history */
    PkRestartEnum restart;
    bool failed;
    bool only_some; /* some of the packages were left unticked */
    /* Why it failed, for the history. */
    char *error_name, *summary, *details;
} install;

static const char *const status_texts[PK_STATUS_ENUM_LAST] = {
    [PK_STATUS_ENUM_WAIT] = "Waiting for other software changes to finish...",
    [PK_STATUS_ENUM_WAITING_FOR_LOCK] = "Waiting for other software changes to finish...",
    [PK_STATUS_ENUM_WAITING_FOR_AUTH] = "Waiting for permission...",
    [PK_STATUS_ENUM_DEP_RESOLVE] = "Checking what else is needed...",
    [PK_STATUS_ENUM_DOWNLOAD] = "Downloading updates...",
    [PK_STATUS_ENUM_SIG_CHECK] = "Checking signatures...",
    [PK_STATUS_ENUM_INSTALL] = "Installing updates...",
    [PK_STATUS_ENUM_UPDATE] = "Installing updates...",
    [PK_STATUS_ENUM_COMMIT] = "Installing updates...",
    [PK_STATUS_ENUM_CLEANUP] = "Cleaning up...",
    [PK_STATUS_ENUM_RUN_HOOK] = "Running setup steps...",
};

static const char *status_text(PkStatusEnum status) {
    return (unsigned)status < XD_ARRAY_LEN(status_texts) ? status_texts[status] : nullptr;
}

static void on_pk_progress(PkProgress *progress, PkProgressType type, gpointer data) {
    install *in = data;
    if (in->progress == nullptr)
        return; /* it failed already */
    if (type == PK_PROGRESS_TYPE_PERCENTAGE) {
        int pct = pk_progress_get_percentage(progress);
        xd_wuau_progress_set(in->progress, pct >= 0 && pct <= 100 ? pct / 100.0 : -1.0, nullptr);
    } else if (type == PK_PROGRESS_TYPE_STATUS) {
        const char *text = status_text(pk_progress_get_status(progress));
        if (text != nullptr)
            xd_wuau_progress_set(in->progress, -2, text);
    } else if (type == PK_PROGRESS_TYPE_PACKAGE) {
        PkPackage *package = pk_progress_get_package(progress);
        if (package != nullptr && pk_package_get_info(package) == PK_INFO_ENUM_UPDATING) {
            char *installing = g_strdup_printf("Installing %s...", pk_package_get_name(package));
            xd_wuau_progress_set(in->progress, -2, installing);
            g_free(installing);
        }
    } else if (type == PK_PROGRESS_TYPE_ALLOW_CANCEL) {
        xd_wuau_progress_cancellable(in->progress, pk_progress_get_allow_cancel(progress));
    }
}

/* Shows why the install failed (not when it was cancelled: no summary),
 * writes it to the journal, and keeps it for the history. */
static void install_failed(install *in, const char *headline, const char *error_name,
                           const char *summary, const char *details) {
    in->failed = true;
    xd_wuau_log_failure(headline, error_name, details);
    if (in->error_name == nullptr) {
        in->error_name = g_strdup(error_name);
        in->summary = g_strdup(summary);
        in->details = g_strdup(details);
    }
    /* The progress window goes first: the error box replaces it. */
    xd_wuau_progress_free(in->progress);
    in->progress = nullptr;
    if (summary != nullptr)
        xd_wuau_show_failure(in->w, headline, summary, details);
}

static void install_finish(install *in) {
    wuau *applet = in->w;
    xd_wuau_progress_free(in->progress);
    applet->busy = false;
    bool cancelled = g_cancellable_is_cancelled(in->cancel);
    PkRestartEnum restart = in->restart;
    bool ok = !in->failed && !cancelled;
    xd_wuau_record(XD_UPDATE_ACTION_INSTALL,
                   ok          ? XD_UPDATE_SUCCEEDED
                   : cancelled ? XD_UPDATE_CANCELED
                               : XD_UPDATE_FAILED,
                   in->names, in->error_name, in->summary, in->details);
    g_free(in->error_name);
    g_free(in->summary);
    g_free(in->details);
    g_ptr_array_unref(in->names);
    g_strfreev(in->pk_ids);
    g_ptr_array_unref(in->flatpaks);
    g_object_unref(in->cancel);
    g_free(in);
    xd_wuau_software_load(applet);
    if (!ok)
        return;
    if (xd_update_restart_system(restart))
        xd_wuau_ask_restart(applet, "Updating your computer is almost complete.");
    else if (restart == PK_RESTART_ENUM_SESSION || restart == PK_RESTART_ENUM_SECURITY_SESSION)
        xd_message_box(GTK_WINDOW(applet->window), XD_WUAU_TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       "The updates were installed. Log off and log on again for them to take "
                       "effect.");
    else
        xd_message_box(GTK_WINDOW(applet->window), XD_WUAU_TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       "The updates were installed successfully.");
}

typedef struct fp_status {
    install *in;
    char *text;
    int percent;
} fp_status;

static gboolean fp_show(gpointer data) {
    fp_status *status = data;
    if (status->in->progress != nullptr)
        xd_wuau_progress_set(status->in->progress,
                             status->percent >= 0 ? status->percent / 100.0 : -1.0, status->text);
    g_free(status->text);
    g_free(status);
    return G_SOURCE_REMOVE;
}

/* From the transaction's thread; ahead of the task's completion, which
 * frees the install. */
static void fp_post(install *in, const char *text, int percent) {
    fp_status *status = g_new0(fp_status, 1);
    *status = (fp_status){in, g_strdup(text), percent};
    g_main_context_invoke_full(nullptr, G_PRIORITY_HIGH, fp_show, status, nullptr);
}

static void on_fp_changed(FlatpakTransactionProgress *progress, gpointer data) {
    fp_post(data, nullptr,
            flatpak_transaction_progress_get_is_estimating(progress)
                ? -1
                : flatpak_transaction_progress_get_progress(progress));
}

static void on_fp_operation([[maybe_unused]] FlatpakTransaction *transaction,
                            FlatpakTransactionOperation *op, FlatpakTransactionProgress *progress,
                            gpointer data) {
    FlatpakRef *ref = flatpak_ref_parse(flatpak_transaction_operation_get_ref(op), nullptr);
    char *text = g_strdup_printf("Installing %s...",
                                 ref != nullptr ? flatpak_ref_get_name(ref)
                                                : flatpak_transaction_operation_get_ref(op));
    fp_post(data, text, -1);
    g_free(text);
    g_clear_object(&ref);
    flatpak_transaction_progress_set_update_frequency(progress, 200);
    g_signal_connect(progress, "changed", G_CALLBACK(on_fp_changed), data);
}

/* Updates `refs` (fp_update *) of one installation each in turn. */
static bool run_flatpak_updates(GPtrArray *refs, bool download_only, bool interactive,
                                GCancellable *cancel, gpointer progress_data, GError **error) {
    bool ok = true;
    GHashTable *done = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; ok && i < refs->len; i++) {
        fp_update *first = g_ptr_array_index(refs, i);
        if (g_hash_table_contains(done, first->installation))
            continue;
        g_hash_table_add(done, first->installation);
        FlatpakInstallation *inst = open_installation(first->installation, cancel, error);
        if (inst == nullptr) {
            ok = false;
            break;
        }
        /* The scheduled download mustn't ask anyone anything. */
        flatpak_installation_set_no_interaction(inst, !interactive);
        FlatpakTransaction *transaction =
            flatpak_transaction_new_for_installation(inst, cancel, error);
        ok = transaction != nullptr;
        if (ok) {
            flatpak_transaction_set_no_deploy(transaction, download_only);
            if (progress_data != nullptr)
                g_signal_connect(transaction, "new-operation", G_CALLBACK(on_fp_operation),
                                 progress_data);
            for (guint j = i; ok && j < refs->len; j++) {
                fp_update *flatpak_update = g_ptr_array_index(refs, j);
                if (strcmp(flatpak_update->installation, first->installation) == 0)
                    ok = flatpak_transaction_add_update(transaction, flatpak_update->ref, nullptr,
                                                        nullptr, error);
            }
            ok = ok && flatpak_transaction_run(transaction, cancel, error);
        }
        g_clear_object(&transaction);
        g_object_unref(inst);
    }
    g_hash_table_unref(done);
    return ok;
}

static void fp_install_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                              GCancellable *cancel) {
    install *in = data;
    GError *error = nullptr;
    if (run_flatpak_updates(in->flatpaks, false, true, cancel, in, &error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);
}

static void on_fp_installed([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    install *in = data;
    GError *error = nullptr;
    if (!g_task_propagate_boolean(G_TASK(res), &error)) {
        bool cancelled = g_cancellable_is_cancelled(in->cancel) ||
                         g_error_matches(error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED);
        install_failed(in, "Some apps could not be updated.",
                       cancelled ? "transaction-cancelled" : "flatpak-error",
                       cancelled ? nullptr
                                 : "Flatpak couldn't update them. Click Details to see what it "
                                   "reported.",
                       error->message);
        g_clear_error(&error);
    }
    install_finish(in);
}

static void install_flatpaks(install *in) {
    if (in->flatpaks->len == 0 || g_cancellable_is_cancelled(in->cancel)) {
        install_finish(in);
        return;
    }
    xd_wuau_progress_cancellable(in->progress, true);
    GTask *task = g_task_new(nullptr, in->cancel, on_fp_installed, in);
    g_task_set_task_data(task, in, nullptr);
    g_task_run_in_thread(task, fp_install_thread);
    g_object_unref(task);
}

typedef struct pk_reply {
    install *in;
    PkResults *results;
    GError *error;
} pk_reply;

/* From an idle callback: packagekit-glib2 finishes its task inside the
 * transaction's Finished signal and again on Destroy if a nested main
 * loop (an error box) lets that through first. */
static gboolean pk_updated_later(gpointer data) {
    pk_reply *packagekit_reply = data;
    install *in = packagekit_reply->in;
    if (packagekit_reply->results != nullptr)
        in->restart = pk_results_get_require_restart_worst(packagekit_reply->results);
    PkErrorEnum code = PK_ERROR_ENUM_UNKNOWN;
    char *details = nullptr;
    if (xd_update_get_failure(packagekit_reply->error, packagekit_reply->results, &code,
                              &details)) {
        if (g_cancellable_is_cancelled(in->cancel))
            code = PK_ERROR_ENUM_TRANSACTION_CANCELLED;
        char *summary = xd_update_get_error_summary(code, in->only_some);
        install_failed(in, "The updates could not be installed.", pk_error_enum_to_string(code),
                       summary, details);
        g_free(summary);
        g_free(details);
    }
    g_clear_error(&packagekit_reply->error);
    g_clear_object(&packagekit_reply->results);
    g_free(packagekit_reply);
    if (in->failed) {
        install_finish(in);
        return G_SOURCE_REMOVE;
    }
    install_flatpaks(in);
    return G_SOURCE_REMOVE;
}

static void on_pk_updated(GObject *src, GAsyncResult *res, gpointer data) {
    pk_reply *packagekit_reply = g_new0(pk_reply, 1);
    packagekit_reply->in = data;
    packagekit_reply->results =
        pk_client_generic_finish(PK_CLIENT(src), res, &packagekit_reply->error);
    g_idle_add(pk_updated_later, packagekit_reply);
}

/* How many package updates are listed, and how many of them are ticked. */
static void count_packages(wuau *applet, unsigned *listed, unsigned *ticked) {
    *listed = *ticked = 0;
    GtkTreeIter it;
    GtkTreeModel *model = GTK_TREE_MODEL(applet->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        gboolean on = FALSE;
        int kind = 0;
        gtk_tree_model_get(model, &it, COL_SELECTED, &on, COL_KIND, &kind, -1);
        if (kind == KIND_PACKAGE) {
            ++*listed;
            *ticked += on ? 1 : 0;
        }
    }
}

static void tick_all_packages(wuau *applet) {
    GtkTreeIter it;
    GtkTreeModel *model = GTK_TREE_MODEL(applet->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        int kind = 0;
        gtk_tree_model_get(model, &it, COL_KIND, &kind, -1);
        if (kind == KIND_PACKAGE)
            gtk_list_store_set(applet->sw_store, &it, COL_SELECTED, TRUE, -1);
    }
}

/* Arch Linux (PackageKit's alpm backend) upgrades the whole system or
 * nothing: pacman doesn't support partial upgrades, and the backend adds
 * every upgrade to an UpdatePackages of a few anyway. False: Cancel. */
static bool confirm_partial(wuau *applet, bool *only_some) {
    unsigned listed = 0, ticked = 0;
    count_packages(applet, &listed, &ticked);
    *only_some = ticked > 0 && ticked < listed;
    if (!*only_some || g_strcmp0(applet->backend, "alpm") != 0)
        return true;
    static const char *const labels[] = {"Install _All", "Install _Selected", "Cancel"};
    int choice = xd_message_box_choose(
        GTK_WINDOW(applet->window), XD_WUAU_TITLE, XD_MB_WARNING,
        "Installing only some updates isn't supported on this system: its package manager may "
        "install all of them anyway, or fail.\n\nInstall all of them?",
        labels, 3, 0, 2);
    if (choice == 0) {
        tick_all_packages(applet);
        *only_some = false;
    }
    return choice != 2;
}

static void on_install([[maybe_unused]] GtkButton *button, gpointer data) {
    wuau *applet = data;
    bool only_some = false;
    if (applet->busy || !confirm_partial(applet, &only_some))
        return;
    install *in = g_new0(install, 1);
    in->w = applet;
    in->cancel = g_cancellable_new();
    in->flatpaks = g_ptr_array_new_with_free_func(fp_update_free);
    in->names = g_ptr_array_new_with_free_func(g_free);
    in->only_some = only_some;
    GPtrArray *ids = g_ptr_array_new();
    GtkTreeIter it;
    GtkTreeModel *model = GTK_TREE_MODEL(applet->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        gboolean on = FALSE;
        int kind = 0;
        unsigned restart = 0;
        char *id = nullptr, *inst = nullptr, *name = nullptr, *version = nullptr;
        gtk_tree_model_get(model, &it, COL_SELECTED, &on, COL_KIND, &kind, COL_ID, &id,
                           COL_INSTALLATION, &inst, COL_NAME, &name, COL_RESTART, &restart,
                           COL_VERSION, &version, -1);
        if (on)
            g_ptr_array_add(in->names, g_strdup_printf("%s %s", name, version));
        g_free(version);
        if (on && kind == KIND_PACKAGE) {
            g_ptr_array_add(ids, id);
            id = nullptr;
            /* GetUpdateDetail's hint, in case the transaction doesn't say. */
            if (restart > (unsigned)in->restart && restart != PK_RESTART_ENUM_UNKNOWN)
                in->restart = (PkRestartEnum)restart;
        } else if (on) {
            fp_update *flatpak_update = g_new0(fp_update, 1);
            flatpak_update->installation = inst;
            flatpak_update->ref = id;
            flatpak_update->name = name;
            inst = id = name = nullptr;
            g_ptr_array_add(in->flatpaks, flatpak_update);
        }
        g_free(id);
        g_free(inst);
        g_free(name);
    }
    g_ptr_array_add(ids, nullptr);
    in->pk_ids = (char **)g_ptr_array_free(ids, FALSE);
    applet->busy = true;
    sync_buttons(applet);
    in->progress =
        xd_wuau_progress_new(applet, "Please wait while the updates are installed.", in->cancel);
    if (in->pk_ids[0] == nullptr) {
        install_flatpaks(in);
        return;
    }
    pk_client_update_packages_async(applet->pk_task,
                                    pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED),
                                    in->pk_ids, in->cancel, on_pk_progress, in, on_pk_updated, in);
}

/* ---- the page ------------------------------------------------------------------------ */

static void text_column(wuau *applet, const char *title, int col, bool expand) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    if (expand)
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    GtkTreeViewColumn *column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", col, nullptr);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_expand(column, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(applet->sw_view), column);
}

GtkWidget *xd_wuau_software_page(wuau *applet) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    applet->sw_status = xd_wuau_label("");
    gtk_box_pack_start(GTK_BOX(page), applet->sw_status, FALSE, FALSE, 0);

    applet->sw_store =
        gtk_list_store_new(COL_COUNT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                           G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                           G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING);
    gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(applet->sw_store), compare_rows,
                                            nullptr, nullptr);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(applet->sw_store),
                                         GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                         GTK_SORT_ASCENDING);
    applet->sw_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(applet->sw_store));
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_toggled), applet);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(applet->sw_view),
        gtk_tree_view_column_new_with_attributes("", toggle, "active", COL_SELECTED, nullptr));
    text_column(applet, "Name", COL_NAME, true);
    text_column(applet, "Version", COL_VERSION, false);
    text_column(applet, "Importance", COL_IMPORTANCE, false);
    text_column(applet, "Source", COL_SOURCE, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(applet->sw_view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection), applet);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 170);
    gtk_container_add(GTK_CONTAINER(scroll), applet->sw_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    applet->sw_details = xd_wuau_label("Select an update to see its details.");
    gtk_label_set_selectable(GTK_LABEL(applet->sw_details), TRUE);
    gtk_label_set_yalign(GTK_LABEL(applet->sw_details), 0);
    GtkWidget *details_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(details_scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(details_scroll, -1, 90);
    gtk_container_add(GTK_CONTAINER(details_scroll), applet->sw_details);
    gtk_box_pack_start(GTK_BOX(page), xd_wuau_frame("Details", details_scroll), FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    applet->sw_check = gtk_button_new_with_mnemonic("_Check for Updates");
    g_signal_connect(applet->sw_check, "clicked", G_CALLBACK(on_check), applet);
    gtk_box_pack_start(GTK_BOX(buttons), applet->sw_check, FALSE, FALSE, 0);
    GtkWidget *history = gtk_button_new_with_mnemonic("View Installation _History");
    g_signal_connect_swapped(history, "clicked", G_CALLBACK(xd_wuau_show_history), applet);
    gtk_box_pack_start(GTK_BOX(buttons), history, FALSE, FALSE, 0);
    applet->sw_install = gtk_button_new_with_mnemonic("_Install");
    gtk_widget_set_size_request(applet->sw_install, 80, -1);
    g_signal_connect(applet->sw_install, "clicked", G_CALLBACK(on_install), applet);
    gtk_box_pack_end(GTK_BOX(buttons), applet->sw_install, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    sync_buttons(applet);
    return page;
}

void xd_wuau_software_finish(wuau *applet) { g_clear_object(&applet->sw_store); }

/* ---- the scheduled check (no window) ----------------------------------------------- */

/* NetworkManager's Metered: yes (1) or guess-yes (3). */
static bool metered(GDBusConnection *bus) {
    if (bus == nullptr)
        return false;
    GVariant *metered_reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.NetworkManager", "Metered"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, nullptr, nullptr);
    if (metered_reply == nullptr)
        return false;
    GVariant *value = nullptr;
    g_variant_get(metered_reply, "(v)", &value);
    unsigned metered_state =
        g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(value) : 0;
    g_variant_unref(value);
    g_variant_unref(metered_reply);
    return metered_state == 1 || metered_state == 3;
}

/* A step of the check failed, with nobody watching: the state says so (for
 * Automatic Updates' "Last checked" line and the shell's balloon), and the
 * journal and the installation history get the details. */
static void note_failure(xd_updates_state *st, const char *what, xd_update_action action,
                         const GPtrArray *names, const char *code_name, const char *summary,
                         const char *details) {
    xd_wuau_log_failure(what, code_name, details);
    xd_wuau_record(action, XD_UPDATE_FAILED, names, code_name, summary, details);
    if (st->error[0] == '\0')
        g_snprintf(st->error, sizeof(st->error), "%s: %s", what,
                   summary != nullptr && summary[0] != '\0' ? summary : details);
}

static void note_error(xd_updates_state *st, const char *what, xd_update_action action,
                       const GPtrArray *names, GError *error, PkResults *results) {
    PkErrorEnum code = PK_ERROR_ENUM_UNKNOWN;
    char *details = nullptr;
    if (!xd_update_get_failure(error, results, &code, &details))
        details = g_strdup("PackageKit didn't finish.");
    char *summary = xd_update_get_error_summary(code, false);
    note_failure(st, what, action, names, pk_error_enum_to_string(code), summary, details);
    g_free(summary);
    g_free(details);
}

static bool pk_ok(GError *error, PkResults *results) {
    if (error != nullptr || results == nullptr)
        return false;
    PkError *pe = pk_results_get_error_code(results);
    bool failed = pe != nullptr;
    g_clear_object(&pe);
    return !failed && pk_results_get_exit_code(results) == PK_EXIT_ENUM_SUCCESS;
}

int xd_wuau_check(void) {
    /* One check at a time: the shell's, or one started by hand. */
    char *lock_path = g_build_filename(g_get_user_runtime_dir(), "xeondesktop-updates.lock",
                                       nullptr);
    int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    g_free(lock_path);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        if (lock >= 0)
            close(lock);
        return EXIT_SUCCESS;
    }
    xd_settings settings;
    xd_settings_load(&settings);
    xd_updates_state st = {0};
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);

    /* Never asks for a password and runs at the back of the queue. */
    PkClient *pk = pk_client_new();
    pk_client_set_interactive(pk, FALSE);
    pk_client_set_background(pk, TRUE);
    GError *error = nullptr;
    PkResults *results = pk_client_refresh_cache(pk, FALSE, nullptr, nullptr, nullptr, &error);
    if (!pk_ok(error, results))
        note_error(&st, "Refreshing the list of updates", XD_UPDATE_ACTION_CHECK, nullptr, error,
                   results);
    g_clear_error(&error);
    g_clear_object(&results);
    GPtrArray *pk_ids = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *pk_names = g_ptr_array_new_with_free_func(g_free);
    results = pk_client_get_updates(pk, pk_bitfield_value(PK_FILTER_ENUM_NONE), nullptr, nullptr,
                                    nullptr, &error);
    if (pk_ok(error, results)) {
        GPtrArray *pkgs = pk_results_get_package_array(results);
        for (guint i = 0; i < pkgs->len; i++) {
            PkPackage *package = g_ptr_array_index(pkgs, i);
            if (xd_update_rank_important(xd_update_rank_of_package(
                    pk_package_get_info(package), pk_package_get_update_severity(package))))
                st.important++;
            g_ptr_array_add(pk_ids, g_strdup(pk_package_get_id(package)));
            g_ptr_array_add(ids, g_strdup(pk_package_get_id(package)));
            g_ptr_array_add(pk_names, g_strdup_printf("%s %s", pk_package_get_name(package),
                                                      pk_package_get_version(package)));
        }
        g_ptr_array_unref(pkgs);
    } else {
        note_error(&st, "Listing the updates", XD_UPDATE_ACTION_CHECK, nullptr, error, results);
    }
    g_clear_error(&error);
    g_clear_object(&results);

    GPtrArray *flatpaks = flatpak_updates(nullptr);
    for (guint i = 0; i < flatpaks->len; i++) {
        fp_update *flatpak_update = g_ptr_array_index(flatpaks, i);
        g_ptr_array_add(
            ids, g_strdup_printf("%s/%s", flatpak_update->installation, flatpak_update->ref));
    }
    st.software = (int)(pk_ids->len + flatpaks->len);

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    int firmware = xd_wuau_firmware_check(bus, &st.important, ids);
    st.firmware = XD_MAX(firmware, 0);

    /* Download what there is, never install it; not on a metered
     * connection (the user may pay by the megabyte). */
    if (settings.updates.mode == XD_UPDATES_DOWNLOAD && st.software > 0) {
        st.metered = metered(bus);
        if (!st.metered) {
            bool ok = true;
            if (pk_ids->len > 0) {
                g_ptr_array_add(pk_ids, nullptr);
                PkBitfield flags = pk_bitfield_from_enums(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED,
                                                          PK_TRANSACTION_FLAG_ENUM_ONLY_DOWNLOAD,
                                                          -1);
                results = pk_client_update_packages(pk, flags, (char **)pk_ids->pdata, nullptr,
                                                    nullptr, nullptr, &error);
                ok = pk_ok(error, results);
                if (!ok)
                    note_error(&st, "Downloading the updates", XD_UPDATE_ACTION_DOWNLOAD,
                               pk_names, error, results);
                g_clear_error(&error);
                g_clear_object(&results);
            }
            if (flatpaks->len > 0 &&
                !run_flatpak_updates(flatpaks, true, false, nullptr, nullptr, &error)) {
                ok = false;
                GPtrArray *names = g_ptr_array_new();
                for (guint i = 0; i < flatpaks->len; i++)
                    g_ptr_array_add(names,
                                    ((fp_update *)g_ptr_array_index(flatpaks, i))->name);
                note_failure(&st, "Downloading the updates", XD_UPDATE_ACTION_DOWNLOAD, names,
                             "flatpak-error",
                             "Some apps' updates could not be downloaded with Flatpak.",
                             error->message);
                g_ptr_array_unref(names);
                g_clear_error(&error);
            }
            st.downloaded = ok;
        }
    }
    g_clear_object(&bus);

    st.checked = g_get_real_time() / G_USEC_PER_SEC;
    xd_updates_signature((const char *const *)ids->pdata, ids->len, st.signature);
    char *path = xd_updates_state_path();
    bool saved = xd_updates_state_write(path, &st);
    if (!saved)
        g_printerr("xd-wuaucpl: cannot write %s\n", path);
    g_free(path);
    g_ptr_array_unref(flatpaks);
    g_ptr_array_unref(pk_ids);
    g_ptr_array_unref(pk_names);
    g_ptr_array_unref(ids);
    g_object_unref(pk);
    close(lock);
    return saved ? EXIT_SUCCESS : EXIT_FAILURE;
}
