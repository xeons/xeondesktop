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

static void fp_update_free(gpointer p) {
    fp_update *u = p;
    g_free(u->installation);
    g_free(u->ref);
    g_free(u->name);
    g_free(u->version);
    g_free(u->origin);
    g_free(u);
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
            FlatpakInstalledRef *r = g_ptr_array_index(refs, j);
            FlatpakRef *ref = FLATPAK_REF(r);
            fp_update *u = g_new0(fp_update, 1);
            u->installation = g_strdup(installation_id(inst));
            u->ref = flatpak_ref_format_ref(ref);
            const char *name = flatpak_installed_ref_get_appdata_name(r);
            u->name = g_strdup(name != nullptr ? name : flatpak_ref_get_name(ref));
            const char *version = flatpak_installed_ref_get_appdata_version(r);
            u->version = g_strdup(version != nullptr ? version : flatpak_ref_get_branch(ref));
            u->origin = g_strdup(flatpak_installed_ref_get_origin(r));
            g_ptr_array_add(out, u);
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

static sw_ctx *ctx_new(wuau *w) {
    sw_ctx *c = g_new0(sw_ctx, 1);
    *c = (sw_ctx){w, w->sw_generation};
    return c;
}

/* A reply for the list as it is now; frees the context if not. */
static bool current(sw_ctx *c) {
    bool ok = c->generation == c->w->sw_generation && !g_cancellable_is_cancelled(c->w->quit);
    if (!ok)
        g_free(c);
    return ok;
}

static void set_status(wuau *w, const char *text) {
    gtk_label_set_text(GTK_LABEL(w->sw_status), text);
}

static bool any_selected(wuau *w) {
    GtkTreeIter it;
    GtkTreeModel *m = GTK_TREE_MODEL(w->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        gboolean on = FALSE;
        gtk_tree_model_get(m, &it, COL_SELECTED, &on, -1);
        if (on)
            return true;
    }
    return false;
}

static void sync_buttons(wuau *w) {
    gtk_widget_set_sensitive(w->sw_install, !w->busy && w->sw_pending == 0 && any_selected(w));
    gtk_widget_set_sensitive(w->sw_check, !w->busy && w->sw_pending == 0);
}

int xd_wuau_software_count(wuau *w, int *important, GPtrArray *ids) {
    int n = 0;
    GtkTreeIter it;
    GtkTreeModel *m = GTK_TREE_MODEL(w->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        int rank = 0;
        char *id = nullptr;
        gtk_tree_model_get(m, &it, COL_RANK, &rank, COL_ID, &id, -1);
        n++;
        if (xd_update_rank_important((xd_update_rank)rank))
            ++*important;
        g_ptr_array_add(ids, id);
    }
    return n;
}

static void show_count(wuau *w) {
    int important = 0;
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    int n = xd_wuau_software_count(w, &important, ids);
    g_ptr_array_free(ids, TRUE);
    char *text;
    if (n == 0)
        text = g_strdup("Your computer is up to date: there are no updates for its programs.");
    else if (important > 0)
        text = g_strdup_printf("%d %s available for your computer, %d of them important. "
                               "Check the updates you want, then click Install.",
                               n, n == 1 ? "update is" : "updates are", important);
    else
        text = g_strdup_printf("%d %s available for your computer. Check the updates you want, "
                               "then click Install.",
                               n, n == 1 ? "update is" : "updates are");
    set_status(w, text);
    g_free(text);
}

static void query_done(wuau *w, bool failed) {
    w->sw_failed = w->sw_failed || failed;
    if (--w->sw_pending > 0)
        return;
    bool checked = w->sw_refresh;
    w->sw_refresh = false;
    /* Without PackageKit's answer the count would be wrong: the shell
     * keeps what the last check found. */
    if (!w->sw_failed) {
        show_count(w);
        xd_wuau_save_state(w, checked);
    }
    sync_buttons(w);
}

/* Fills the details below the list from the selected row. */
static void show_details(wuau *w) {
    GtkTreeModel *m;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->sw_view));
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) {
        gtk_label_set_text(GTK_LABEL(w->sw_details), "Select an update to see its details.");
        return;
    }
    char *name = nullptr, *version = nullptr, *old = nullptr, *summary = nullptr;
    char *detail = nullptr, *importance = nullptr, *source = nullptr;
    unsigned restart = 0;
    gtk_tree_model_get(m, &it, COL_NAME, &name, COL_VERSION, &version, COL_OLD, &old,
                       COL_SUMMARY, &summary, COL_DETAIL, &detail, COL_IMPORTANCE, &importance,
                       COL_SOURCE, &source, COL_RESTART, &restart, -1);
    GString *s = g_string_new(nullptr);
    char *line = g_markup_printf_escaped("<b>%s</b>", name);
    g_string_append(s, line);
    g_free(line);
    if (summary != nullptr && summary[0] != '\0') {
        line = g_markup_printf_escaped(": %s", summary);
        g_string_append(s, line);
        g_free(line);
    }
    line = old != nullptr && old[0] != '\0'
               ? g_markup_printf_escaped("\nVersion %s replaces %s.", version, old)
               : g_markup_printf_escaped("\nVersion %s.", version);
    g_string_append(s, line);
    g_free(line);
    if (importance != nullptr && importance[0] != '\0') {
        line = g_markup_printf_escaped(" Importance: %s.", importance);
        g_string_append(s, line);
        g_free(line);
    }
    line = g_markup_printf_escaped(" From %s.", source);
    g_string_append(s, line);
    g_free(line);
    if (detail != nullptr && detail[0] != '\0') {
        line = g_markup_printf_escaped("\n\n%s", detail);
        g_string_append(s, line);
        g_free(line);
    }
    const char *hint = xd_update_restart_text((PkRestartEnum)restart);
    if (hint != nullptr) {
        line = g_markup_printf_escaped("\n\n%s", hint);
        g_string_append(s, line);
        g_free(line);
    }
    gtk_label_set_markup(GTK_LABEL(w->sw_details), s->str);
    g_string_free(s, TRUE);
    g_free(name);
    g_free(version);
    g_free(old);
    g_free(summary);
    g_free(detail);
    g_free(importance);
    g_free(source);
}

static bool find_row(wuau *w, const char *id, GtkTreeIter *out) {
    GtkTreeModel *m = GTK_TREE_MODEL(w->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(m, out); ok; ok = gtk_tree_model_iter_next(m, out)) {
        char *row = nullptr;
        gtk_tree_model_get(m, out, COL_ID, &row, -1);
        bool same = g_strcmp0(row, id) == 0;
        g_free(row);
        if (same)
            return true;
    }
    return false;
}

static void on_details(GObject *src, GAsyncResult *res, gpointer data) {
    sw_ctx *c = data;
    PkResults *r = pk_client_generic_finish(PK_CLIENT(src), res, nullptr);
    if (!current(c)) {
        g_clear_object(&r);
        return;
    }
    wuau *w = c->w;
    g_free(c);
    GPtrArray *details = r != nullptr ? pk_results_get_update_detail_array(r) : nullptr;
    for (guint i = 0; details != nullptr && i < details->len; i++) {
        PkUpdateDetail *d = g_ptr_array_index(details, i);
        GtkTreeIter it;
        if (!find_row(w, pk_update_detail_get_package_id(d), &it))
            continue;
        GString *text = g_string_new(pk_update_detail_get_update_text(d));
        g_strstrip(text->str);
        text->len = strlen(text->str);
        char **cves = pk_update_detail_get_cve_urls(d);
        for (char **u = cves; u != nullptr && *u != nullptr; u++)
            g_string_append_printf(text, "%sSecurity issue: %s", text->len > 0 ? "\n" : "", *u);
        char *old = nullptr;
        char **replaces = pk_update_detail_get_updates(d);
        if (replaces != nullptr && replaces[0] != nullptr)
            xd_appwiz_split_package_id(replaces[0], nullptr, &old, nullptr);
        PkRestartEnum restart = pk_update_detail_get_restart(d);
        gtk_list_store_set(w->sw_store, &it, COL_DETAIL, text->str, COL_RESTART,
                           (unsigned)restart, COL_OLD, old, -1);
        g_free(old);
        g_string_free(text, TRUE);
    }
    if (details != nullptr)
        g_ptr_array_unref(details);
    g_clear_object(&r);
    show_details(w);
    query_done(w, false);
}

static void on_updates(GObject *src, GAsyncResult *res, gpointer data) {
    sw_ctx *c = data;
    GError *error = nullptr;
    PkResults *r = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (!current(c)) {
        g_clear_error(&error);
        g_clear_object(&r);
        return;
    }
    wuau *w = c->w;
    PkError *pe = r != nullptr ? pk_results_get_error_code(r) : nullptr;
    if (error != nullptr || pe != nullptr) {
        g_free(c);
        char *text = g_strdup_printf("PackageKit couldn't list the updates: %s",
                                     error != nullptr ? error->message
                                                      : pk_error_get_details(pe));
        set_status(w, text);
        g_free(text);
        g_clear_error(&error);
        g_clear_object(&pe);
        g_clear_object(&r);
        query_done(w, true);
        return;
    }
    GPtrArray *pkgs = pk_results_get_package_array(r);
    char **ids = g_new0(char *, pkgs->len + 1);
    for (guint i = 0; i < pkgs->len; i++) {
        PkPackage *p = g_ptr_array_index(pkgs, i);
        xd_update_rank rank = xd_update_rank_of_package(pk_package_get_info(p),
                                                        pk_package_get_update_severity(p));
        char *source = xd_appwiz_package_source(w->backend, pk_package_get_id(p));
        gtk_list_store_insert_with_values(
            w->sw_store, nullptr, -1, COL_SELECTED, TRUE, COL_NAME, pk_package_get_name(p),
            COL_VERSION, pk_package_get_version(p), COL_IMPORTANCE, xd_update_rank_name(rank),
            COL_SOURCE, source, COL_KIND, KIND_PACKAGE, COL_ID, pk_package_get_id(p), COL_RANK,
            (int)rank, COL_SUMMARY, pk_package_get_summary(p), -1);
        g_free(source);
        ids[i] = g_strdup(pk_package_get_id(p));
    }
    g_ptr_array_unref(pkgs);
    g_clear_object(&r);
    if (ids[0] != nullptr) {
        /* What each fixes and whether it needs a restart. */
        w->sw_pending++;
        pk_client_get_update_detail_async(w->pk, ids, w->quit, nullptr, nullptr, on_details, c);
    } else {
        g_free(c);
    }
    g_strfreev(ids);
    query_done(w, false);
}

static void fp_list_thread(GTask *task, [[maybe_unused]] gpointer src,
                           [[maybe_unused]] gpointer data, GCancellable *cancel) {
    g_task_return_pointer(task, flatpak_updates(cancel), (GDestroyNotify)g_ptr_array_unref);
}

static void on_flatpaks([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    sw_ctx *c = data;
    GPtrArray *list = g_task_propagate_pointer(G_TASK(res), nullptr);
    if (!current(c)) {
        if (list != nullptr)
            g_ptr_array_unref(list);
        return;
    }
    wuau *w = c->w;
    g_free(c);
    for (guint i = 0; list != nullptr && i < list->len; i++) {
        fp_update *u = g_ptr_array_index(list, i);
        char *source = xd_appwiz_flatpak_source(
            u->origin, strcmp(u->installation, "default") == 0 ? "system" : u->installation);
        gtk_list_store_insert_with_values(
            w->sw_store, nullptr, -1, COL_SELECTED, TRUE, COL_NAME, u->name, COL_VERSION,
            u->version, COL_IMPORTANCE, "", COL_SOURCE, source, COL_KIND, KIND_FLATPAK, COL_ID,
            u->ref, COL_INSTALLATION, u->installation, COL_RANK, (int)XD_RANK_UNKNOWN,
            COL_SUMMARY, "", -1);
        g_free(source);
    }
    if (list != nullptr)
        g_ptr_array_unref(list);
    query_done(w, false);
}

static void list(wuau *w) {
    w->sw_pending += 2;
    pk_client_get_updates_async(w->pk, pk_bitfield_value(PK_FILTER_ENUM_NONE), w->quit, nullptr,
                                nullptr, on_updates, ctx_new(w));
    GTask *task = g_task_new(nullptr, w->quit, on_flatpaks, ctx_new(w));
    g_task_run_in_thread(task, fp_list_thread);
    g_object_unref(task);
}

static void on_refreshed(GObject *src, GAsyncResult *res, gpointer data) {
    sw_ctx *c = data;
    GError *error = nullptr;
    PkResults *r = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (!current(c)) {
        g_clear_error(&error);
        g_clear_object(&r);
        return;
    }
    wuau *w = c->w;
    g_free(c);
    PkError *pe = r != nullptr ? pk_results_get_error_code(r) : nullptr;
    if (error != nullptr || pe != nullptr) {
        /* Still list what the old lists know. */
        xd_wuau_error(w, "The list of updates could not be refreshed.",
                      error != nullptr ? error->message : pk_error_get_details(pe));
        w->sw_refresh = false;
    }
    g_clear_error(&error);
    g_clear_object(&pe);
    g_clear_object(&r);
    list(w);
    query_done(w, false);
}

void xd_wuau_software_load(wuau *w) {
    w->sw_generation++;
    w->sw_pending = 0;
    w->sw_failed = false;
    gtk_list_store_clear(w->sw_store);
    set_status(w, w->sw_refresh ? "Checking for updates..." : "Looking for updates...");
    if (w->sw_refresh) {
        w->sw_pending++;
        pk_client_refresh_cache_async(w->pk_task, FALSE, w->quit, nullptr, nullptr, on_refreshed,
                                      ctx_new(w));
    } else {
        list(w);
    }
    sync_buttons(w);
}

static void on_check([[maybe_unused]] GtkButton *b, gpointer data) {
    wuau *w = data;
    w->sw_refresh = true;
    xd_wuau_software_load(w);
}

static void on_toggled([[maybe_unused]] GtkCellRendererToggle *r, char *path, gpointer data) {
    wuau *w = data;
    GtkTreeIter it;
    if (w->busy || !gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(w->sw_store), &it, path))
        return;
    gboolean on = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(w->sw_store), &it, COL_SELECTED, &on, -1);
    gtk_list_store_set(w->sw_store, &it, COL_SELECTED, !on, -1);
    sync_buttons(w);
}

static void on_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    show_details(data);
}

/* Most important first, then by name. */
static int compare_rows(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b,
                        [[maybe_unused]] gpointer data) {
    int ra = 0, rb = 0;
    char *na = nullptr, *nb = nullptr;
    gtk_tree_model_get(m, a, COL_RANK, &ra, COL_NAME, &na, -1);
    gtk_tree_model_get(m, b, COL_RANK, &rb, COL_NAME, &nb, -1);
    int r = ra != rb ? rb - ra : g_utf8_collate(na != nullptr ? na : "", nb != nullptr ? nb : "");
    g_free(na);
    g_free(nb);
    return r;
}

/* ---- installing -------------------------------------------------------------------- */

typedef struct install {
    wuau *w;
    GCancellable *cancel;
    xd_wuau_progress *progress;
    char **pk_ids;
    GPtrArray *flatpaks; /* fp_update *: installation, ref, name */
    PkRestartEnum restart;
    bool failed;
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

static const char *status_text(PkStatusEnum s) {
    return (unsigned)s < XD_ARRAY_LEN(status_texts) ? status_texts[s] : nullptr;
}

static void on_pk_progress(PkProgress *progress, PkProgressType type, gpointer data) {
    install *in = data;
    if (type == PK_PROGRESS_TYPE_PERCENTAGE) {
        int pct = pk_progress_get_percentage(progress);
        xd_wuau_progress_set(in->progress, pct >= 0 && pct <= 100 ? pct / 100.0 : -1.0, nullptr);
    } else if (type == PK_PROGRESS_TYPE_STATUS) {
        const char *text = status_text(pk_progress_get_status(progress));
        if (text != nullptr)
            xd_wuau_progress_set(in->progress, -2, text);
    } else if (type == PK_PROGRESS_TYPE_PACKAGE) {
        PkPackage *p = pk_progress_get_package(progress);
        if (p != nullptr && pk_package_get_info(p) == PK_INFO_ENUM_UPDATING) {
            char *s = g_strdup_printf("Installing %s...", pk_package_get_name(p));
            xd_wuau_progress_set(in->progress, -2, s);
            g_free(s);
        }
    } else if (type == PK_PROGRESS_TYPE_ALLOW_CANCEL) {
        xd_wuau_progress_cancellable(in->progress, pk_progress_get_allow_cancel(progress));
    }
}

static void install_finish(install *in) {
    wuau *w = in->w;
    xd_wuau_progress_free(in->progress);
    w->busy = false;
    bool cancelled = g_cancellable_is_cancelled(in->cancel);
    PkRestartEnum restart = in->restart;
    bool ok = !in->failed && !cancelled;
    g_strfreev(in->pk_ids);
    g_ptr_array_unref(in->flatpaks);
    g_object_unref(in->cancel);
    g_free(in);
    xd_wuau_software_load(w);
    if (!ok)
        return;
    if (xd_update_restart_system(restart))
        xd_wuau_ask_restart(w, "Updating your computer is almost complete.");
    else if (restart == PK_RESTART_ENUM_SESSION || restart == PK_RESTART_ENUM_SECURITY_SESSION)
        xd_message_box(GTK_WINDOW(w->window), XD_WUAU_TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       "The updates were installed. Log off and log on again for them to take "
                       "effect.");
    else
        xd_message_box(GTK_WINDOW(w->window), XD_WUAU_TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       "The updates were installed successfully.");
}

typedef struct fp_status {
    install *in;
    char *text;
    int percent;
} fp_status;

static gboolean fp_show(gpointer data) {
    fp_status *s = data;
    xd_wuau_progress_set(s->in->progress, s->percent >= 0 ? s->percent / 100.0 : -1.0, s->text);
    g_free(s->text);
    g_free(s);
    return G_SOURCE_REMOVE;
}

/* From the transaction's thread; ahead of the task's completion, which
 * frees the install. */
static void fp_post(install *in, const char *text, int percent) {
    fp_status *s = g_new0(fp_status, 1);
    *s = (fp_status){in, g_strdup(text), percent};
    g_main_context_invoke_full(nullptr, G_PRIORITY_HIGH, fp_show, s, nullptr);
}

static void on_fp_changed(FlatpakTransactionProgress *progress, gpointer data) {
    fp_post(data, nullptr,
            flatpak_transaction_progress_get_is_estimating(progress)
                ? -1
                : flatpak_transaction_progress_get_progress(progress));
}

static void on_fp_operation([[maybe_unused]] FlatpakTransaction *t,
                            FlatpakTransactionOperation *op, FlatpakTransactionProgress *progress,
                            gpointer data) {
    FlatpakRef *ref = flatpak_ref_parse(flatpak_transaction_operation_get_ref(op), nullptr);
    char *s = g_strdup_printf("Installing %s...", ref != nullptr
                                                      ? flatpak_ref_get_name(ref)
                                                      : flatpak_transaction_operation_get_ref(op));
    fp_post(data, s, -1);
    g_free(s);
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
        FlatpakTransaction *t = flatpak_transaction_new_for_installation(inst, cancel, error);
        ok = t != nullptr;
        if (ok) {
            flatpak_transaction_set_no_deploy(t, download_only);
            if (progress_data != nullptr)
                g_signal_connect(t, "new-operation", G_CALLBACK(on_fp_operation), progress_data);
            for (guint j = i; ok && j < refs->len; j++) {
                fp_update *u = g_ptr_array_index(refs, j);
                if (strcmp(u->installation, first->installation) == 0)
                    ok = flatpak_transaction_add_update(t, u->ref, nullptr, nullptr, error);
            }
            ok = ok && flatpak_transaction_run(t, cancel, error);
        }
        g_clear_object(&t);
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
        in->failed = true;
        if (!g_cancellable_is_cancelled(in->cancel) &&
            !g_error_matches(error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
            xd_wuau_error(in->w, "Some updates could not be installed.", error->message);
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
    pk_reply *r = data;
    install *in = r->in;
    PkError *pe = r->results != nullptr ? pk_results_get_error_code(r->results) : nullptr;
    if (r->results != nullptr)
        in->restart = pk_results_get_require_restart_worst(r->results);
    if (r->error != nullptr || pe != nullptr) {
        in->failed = true;
        bool cancelled = g_cancellable_is_cancelled(in->cancel) ||
                         (pe != nullptr &&
                          pk_error_get_code(pe) == PK_ERROR_ENUM_TRANSACTION_CANCELLED);
        if (!cancelled) {
            bool denied = pe != nullptr && pk_error_get_code(pe) == PK_ERROR_ENUM_NOT_AUTHORIZED;
            xd_wuau_error(in->w,
                          denied ? "You don't have permission to install updates on this computer."
                                 : "The updates could not be installed.",
                          r->error != nullptr ? r->error->message : pk_error_get_details(pe));
        }
    }
    g_clear_object(&pe);
    g_clear_error(&r->error);
    g_clear_object(&r->results);
    g_free(r);
    if (in->failed) {
        install_finish(in);
        return G_SOURCE_REMOVE;
    }
    install_flatpaks(in);
    return G_SOURCE_REMOVE;
}

static void on_pk_updated(GObject *src, GAsyncResult *res, gpointer data) {
    pk_reply *r = g_new0(pk_reply, 1);
    r->in = data;
    r->results = pk_client_generic_finish(PK_CLIENT(src), res, &r->error);
    g_idle_add(pk_updated_later, r);
}

static void on_install([[maybe_unused]] GtkButton *b, gpointer data) {
    wuau *w = data;
    if (w->busy)
        return;
    install *in = g_new0(install, 1);
    in->w = w;
    in->cancel = g_cancellable_new();
    in->flatpaks = g_ptr_array_new_with_free_func(fp_update_free);
    GPtrArray *ids = g_ptr_array_new();
    GtkTreeIter it;
    GtkTreeModel *m = GTK_TREE_MODEL(w->sw_store);
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        gboolean on = FALSE;
        int kind = 0;
        unsigned restart = 0;
        char *id = nullptr, *inst = nullptr, *name = nullptr;
        gtk_tree_model_get(m, &it, COL_SELECTED, &on, COL_KIND, &kind, COL_ID, &id,
                           COL_INSTALLATION, &inst, COL_NAME, &name, COL_RESTART, &restart, -1);
        if (on && kind == KIND_PACKAGE) {
            g_ptr_array_add(ids, id);
            id = nullptr;
            /* GetUpdateDetail's hint, in case the transaction doesn't say. */
            if (restart > (unsigned)in->restart && restart != PK_RESTART_ENUM_UNKNOWN)
                in->restart = (PkRestartEnum)restart;
        } else if (on) {
            fp_update *u = g_new0(fp_update, 1);
            u->installation = inst;
            u->ref = id;
            u->name = name;
            inst = id = name = nullptr;
            g_ptr_array_add(in->flatpaks, u);
        }
        g_free(id);
        g_free(inst);
        g_free(name);
    }
    g_ptr_array_add(ids, nullptr);
    in->pk_ids = (char **)g_ptr_array_free(ids, FALSE);
    w->busy = true;
    sync_buttons(w);
    in->progress = xd_wuau_progress_new(w, "Please wait while the updates are installed.",
                                        in->cancel);
    if (in->pk_ids[0] == nullptr) {
        install_flatpaks(in);
        return;
    }
    pk_client_update_packages_async(w->pk_task,
                                    pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED),
                                    in->pk_ids, in->cancel, on_pk_progress, in, on_pk_updated, in);
}

/* ---- the page ------------------------------------------------------------------------ */

static void text_column(wuau *w, const char *title, int col, bool expand) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    if (expand)
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col,
                                                                    nullptr);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_expand(c, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(w->sw_view), c);
}

GtkWidget *xd_wuau_software_page(wuau *w) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    w->sw_status = xd_wuau_label("");
    gtk_box_pack_start(GTK_BOX(page), w->sw_status, FALSE, FALSE, 0);

    w->sw_store = gtk_list_store_new(COL_COUNT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_UINT, G_TYPE_STRING);
    gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(w->sw_store), compare_rows,
                                            nullptr, nullptr);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(w->sw_store),
                                         GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                         GTK_SORT_ASCENDING);
    w->sw_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(w->sw_store));
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_toggled), w);
    gtk_tree_view_append_column(GTK_TREE_VIEW(w->sw_view),
                                gtk_tree_view_column_new_with_attributes(
                                    "", toggle, "active", COL_SELECTED, nullptr));
    text_column(w, "Name", COL_NAME, true);
    text_column(w, "Version", COL_VERSION, false);
    text_column(w, "Importance", COL_IMPORTANCE, false);
    text_column(w, "Source", COL_SOURCE, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->sw_view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection), w);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 170);
    gtk_container_add(GTK_CONTAINER(scroll), w->sw_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    w->sw_details = xd_wuau_label("Select an update to see its details.");
    gtk_label_set_selectable(GTK_LABEL(w->sw_details), TRUE);
    gtk_label_set_yalign(GTK_LABEL(w->sw_details), 0);
    GtkWidget *details_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(details_scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(details_scroll, -1, 90);
    gtk_container_add(GTK_CONTAINER(details_scroll), w->sw_details);
    gtk_box_pack_start(GTK_BOX(page), xd_wuau_frame("Details", details_scroll), FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    w->sw_check = gtk_button_new_with_mnemonic("_Check for Updates");
    g_signal_connect(w->sw_check, "clicked", G_CALLBACK(on_check), w);
    gtk_box_pack_start(GTK_BOX(buttons), w->sw_check, FALSE, FALSE, 0);
    w->sw_install = gtk_button_new_with_mnemonic("_Install");
    gtk_widget_set_size_request(w->sw_install, 80, -1);
    g_signal_connect(w->sw_install, "clicked", G_CALLBACK(on_install), w);
    gtk_box_pack_end(GTK_BOX(buttons), w->sw_install, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    sync_buttons(w);
    return page;
}

void xd_wuau_software_finish(wuau *w) { g_clear_object(&w->sw_store); }

/* ---- the scheduled check (no window) ----------------------------------------------- */

/* NetworkManager's Metered: yes (1) or guess-yes (3). */
static bool metered(GDBusConnection *bus) {
    if (bus == nullptr)
        return false;
    GVariant *r = g_dbus_connection_call_sync(
        bus, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.NetworkManager", "Metered"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, nullptr, nullptr);
    if (r == nullptr)
        return false;
    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    unsigned m = g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v) : 0;
    g_variant_unref(v);
    g_variant_unref(r);
    return m == 1 || m == 3;
}

static void note_error(xd_updates_state *st, const char *what, GError *error, PkResults *r) {
    if (st->error[0] != '\0')
        return;
    PkError *pe = r != nullptr ? pk_results_get_error_code(r) : nullptr;
    const char *why = error != nullptr ? error->message
                      : pe != nullptr  ? pk_error_get_details(pe)
                                       : nullptr;
    if (why != nullptr)
        g_snprintf(st->error, sizeof(st->error), "%s: %s", what, why);
    g_clear_object(&pe);
}

static bool pk_ok(GError *error, PkResults *r) {
    if (error != nullptr || r == nullptr)
        return false;
    PkError *pe = pk_results_get_error_code(r);
    bool failed = pe != nullptr;
    g_clear_object(&pe);
    return !failed && pk_results_get_exit_code(r) == PK_EXIT_ENUM_SUCCESS;
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
    xd_settings s;
    xd_settings_load(&s);
    xd_updates_state st = {0};
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);

    /* Never asks for a password and runs at the back of the queue. */
    PkClient *pk = pk_client_new();
    pk_client_set_interactive(pk, FALSE);
    pk_client_set_background(pk, TRUE);
    GError *error = nullptr;
    PkResults *r = pk_client_refresh_cache(pk, FALSE, nullptr, nullptr, nullptr, &error);
    if (!pk_ok(error, r))
        note_error(&st, "Refreshing the list of updates", error, r);
    g_clear_error(&error);
    g_clear_object(&r);
    GPtrArray *pk_ids = g_ptr_array_new_with_free_func(g_free);
    r = pk_client_get_updates(pk, pk_bitfield_value(PK_FILTER_ENUM_NONE), nullptr, nullptr,
                              nullptr, &error);
    if (pk_ok(error, r)) {
        GPtrArray *pkgs = pk_results_get_package_array(r);
        for (guint i = 0; i < pkgs->len; i++) {
            PkPackage *p = g_ptr_array_index(pkgs, i);
            if (xd_update_rank_important(xd_update_rank_of_package(
                    pk_package_get_info(p), pk_package_get_update_severity(p))))
                st.important++;
            g_ptr_array_add(pk_ids, g_strdup(pk_package_get_id(p)));
            g_ptr_array_add(ids, g_strdup(pk_package_get_id(p)));
        }
        g_ptr_array_unref(pkgs);
    } else {
        note_error(&st, "Listing the updates", error, r);
    }
    g_clear_error(&error);
    g_clear_object(&r);

    GPtrArray *flatpaks = flatpak_updates(nullptr);
    for (guint i = 0; i < flatpaks->len; i++) {
        fp_update *u = g_ptr_array_index(flatpaks, i);
        g_ptr_array_add(ids, g_strdup_printf("%s/%s", u->installation, u->ref));
    }
    st.software = (int)(pk_ids->len + flatpaks->len);

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    int firmware = xd_wuau_firmware_check(bus, &st.important, ids);
    st.firmware = XD_MAX(firmware, 0);

    /* Download what there is, never install it; not on a metered
     * connection (the user may pay by the megabyte). */
    if (s.updates.mode == XD_UPDATES_DOWNLOAD && st.software > 0) {
        st.metered = metered(bus);
        if (!st.metered) {
            bool ok = true;
            if (pk_ids->len > 0) {
                g_ptr_array_add(pk_ids, nullptr);
                PkBitfield flags = pk_bitfield_from_enums(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED,
                                                          PK_TRANSACTION_FLAG_ENUM_ONLY_DOWNLOAD,
                                                          -1);
                r = pk_client_update_packages(pk, flags, (char **)pk_ids->pdata, nullptr, nullptr,
                                              nullptr, &error);
                ok = pk_ok(error, r);
                if (!ok)
                    note_error(&st, "Downloading the updates", error, r);
                g_clear_error(&error);
                g_clear_object(&r);
            }
            if (flatpaks->len > 0 &&
                !run_flatpak_updates(flatpaks, true, false, nullptr, nullptr, &error)) {
                ok = false;
                if (st.error[0] == '\0')
                    g_snprintf(st.error, sizeof(st.error), "Downloading the updates: %s",
                               error->message);
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
    g_ptr_array_unref(ids);
    g_object_unref(pk);
    close(lock);
    return saved ? EXIT_SUCCESS : EXIT_FAILURE;
}
