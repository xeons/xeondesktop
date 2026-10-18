/* SPDX-License-Identifier: MIT
 * Housekeeping: Privacy's "Empty old items from the Recycle Bin" and
 * "Delete old temporary files" (GNOME's keys, org.gnome.desktop.privacy
 * remove-old-trash-files, remove-old-temp-files and old-files-age). Under
 * GNOME, gsd-housekeeping does this; here nothing else would, so the
 * shell does: five minutes after logon (not to slow it down), once a day
 * after that, and a few seconds after the settings change, as
 * gsd-housekeeping does.
 *
 * What goes is decided by Disk Cleanup's code (src/files/cleanup.c), in
 * a thread: from the Recycle Bins (the home one and other drives'), the
 * items deleted more than so many days ago by their .trashinfo's
 * DeletionDate; from the temporary folder (g_get_tmp_dir, as
 * gsd-housekeeping), only the user's own entries untouched for as long,
 * with no socket or FIFO inside, never through a link or into another
 * file system.
 *
 * Only in a real session (under another desktop that desktop does its
 * own housekeeping, and a nested or test session would empty the
 * developer's real Recycle Bin), unless XD_HOUSEKEEPING=1 asks for it
 * (tests, with a scratch home).
 */
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../files/cleanup.h"
#include "shell.h"

#define PRIVACY_SCHEMA "org.gnome.desktop.privacy"

static constexpr guint FIRST_DELAY_S = 5 * 60;
static constexpr guint INTERVAL_S = 24 * 60 * 60;
static constexpr guint CHANGED_DELAY_S = 3;

typedef struct housekeeping {
    GSettings *settings;
    GCancellable *cancel;
    guint timer;
    bool running; /* a purge is in its thread */
    bool again;   /* the settings changed meanwhile */
} housekeeping;

static housekeeping hk;

typedef struct job {
    gboolean trash, temp;
    gint64 age;   /* µs */
    char **roots; /* "/" and every mounted drive's root */
} job;

static void job_free(gpointer p) {
    job *j = p;
    g_strfreev(j->roots);
    g_free(j);
}

static void schedule(guint seconds);

static void purge_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                         GCancellable *cancel) {
    const job *j = data;
    uid_t uid = getuid();
    gint64 now = g_get_real_time();
    char *tmp = g_canonicalize_filename(g_get_tmp_dir(), nullptr);
    const char *temps[] = {tmp, nullptr};
    guint trashed = 0, temps_gone = 0;
    for (char **root = j->roots; *root != nullptr && !g_cancellable_is_cancelled(cancel);
         root++) {
        XdCleanupPlaces *places =
            xd_cleanup_places_new(*root, (const char *const *)j->roots, g_get_user_data_dir(),
                                  nullptr, temps, uid);
        GPtrArray *cats = xd_cleanup_scan_old(places, uid, now, j->age, j->trash, j->temp, cancel);
        for (guint i = 0; i < cats->len; i++) {
            const XdCleanupCategory *c = g_ptr_array_index(cats, i);
            GError *error = nullptr;
            if (!xd_cleanup_delete(c, uid, now, cancel, &error)) {
                fprintf(stderr, "xd-shell: housekeeping: %s\n", error->message);
                g_error_free(error);
            }
            for (guint k = 0; k < c->items->len; k++) {
                const char *item = g_ptr_array_index(c->items, k);
                if (c->kind == XD_CLEANUP_TEMP)
                    temps_gone++;
                else if (g_str_has_suffix(item, ".trashinfo"))
                    trashed++;
            }
        }
        g_ptr_array_unref(cats);
        xd_cleanup_places_free(places);
    }
    if (trashed > 0 || temps_gone > 0)
        fprintf(stderr,
                "xd-shell: housekeeping: %u old items from the Recycle Bin, %u temporary files\n",
                trashed, temps_gone);
    g_free(tmp);
    g_task_return_boolean(task, TRUE);
}

static void purged([[maybe_unused]] GObject *src, [[maybe_unused]] GAsyncResult *res,
                   [[maybe_unused]] gpointer data) {
    hk.running = false;
    if (hk.cancel == nullptr || g_cancellable_is_cancelled(hk.cancel))
        return;
    schedule(hk.again ? CHANGED_DELAY_S : INTERVAL_S);
    hk.again = false;
}

/* "/" and the mounted drives' roots, where other drives' trashes are;
 * GIO's volume monitor only from the main thread. */
static char **drive_roots(void) {
    GPtrArray *roots = g_ptr_array_new();
    g_ptr_array_add(roots, g_strdup("/"));
    GVolumeMonitor *monitor = g_volume_monitor_get();
    GList *mounts = g_volume_monitor_get_mounts(monitor);
    for (GList *l = mounts; l != nullptr; l = l->next) {
        GFile *top = g_mount_get_root(l->data);
        char *path = g_file_get_path(top);
        if (path != nullptr && strcmp(path, "/") != 0 &&
            !g_ptr_array_find_with_equal_func(roots, path, g_str_equal, nullptr))
            g_ptr_array_add(roots, path);
        else
            g_free(path);
        g_object_unref(top);
    }
    g_list_free_full(mounts, g_object_unref);
    g_object_unref(monitor);
    g_ptr_array_add(roots, nullptr);
    return (char **)g_ptr_array_free(roots, FALSE);
}

static gboolean run_purge([[maybe_unused]] gpointer data) {
    hk.timer = 0;
    if (hk.running) {
        hk.again = true;
        return G_SOURCE_REMOVE;
    }
    job *j = g_new0(job, 1);
    j->trash = g_settings_get_boolean(hk.settings, "remove-old-trash-files");
    j->temp = g_settings_get_boolean(hk.settings, "remove-old-temp-files");
    j->age = (gint64)g_settings_get_uint(hk.settings, "old-files-age") * G_TIME_SPAN_DAY;
    if (!j->trash && !j->temp) {
        g_free(j);
        schedule(INTERVAL_S);
        return G_SOURCE_REMOVE;
    }
    j->roots = drive_roots();
    hk.running = true;
    GTask *task = g_task_new(nullptr, hk.cancel, purged, nullptr);
    g_task_set_task_data(task, j, job_free);
    g_task_run_in_thread(task, purge_thread);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

static void schedule(guint seconds) {
    if (hk.timer != 0)
        g_source_remove(hk.timer);
    hk.timer = g_timeout_add_seconds(seconds, run_purge, nullptr);
}

static void on_changed([[maybe_unused]] GSettings *s, const char *key,
                       [[maybe_unused]] gpointer data) {
    if (strcmp(key, "remove-old-trash-files") == 0 || strcmp(key, "remove-old-temp-files") == 0 ||
        strcmp(key, "old-files-age") == 0) {
        if (hk.running)
            hk.again = true;
        else
            schedule(CHANGED_DELAY_S);
    }
}

void xd_housekeeping_init(xd_shell *shell) {
    const char *nested = getenv("XEONDESKTOP_NESTED");
    const char *forced = getenv("XD_HOUSEKEEPING");
    bool real = !shell->standalone && nested != nullptr && strcmp(nested, "0") == 0;
    if (!real && (forced == nullptr || strcmp(forced, "1") != 0))
        return;
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema =
        src != nullptr ? g_settings_schema_source_lookup(src, PRIVACY_SCHEMA, TRUE) : nullptr;
    if (schema == nullptr)
        return;
    if (g_settings_schema_has_key(schema, "remove-old-trash-files") &&
        g_settings_schema_has_key(schema, "remove-old-temp-files") &&
        g_settings_schema_has_key(schema, "old-files-age")) {
        hk.settings = g_settings_new_full(schema, nullptr, nullptr);
        hk.cancel = g_cancellable_new();
        g_signal_connect(hk.settings, "changed", G_CALLBACK(on_changed), nullptr);
        schedule(FIRST_DELAY_S);
    }
    g_settings_schema_unref(schema);
}

void xd_housekeeping_finish([[maybe_unused]] xd_shell *shell) {
    if (hk.timer != 0)
        g_source_remove(hk.timer);
    hk.timer = 0;
    if (hk.cancel != nullptr) {
        /* A purge under way stops at its next item; its thread keeps its
         * own references. */
        g_cancellable_cancel(hk.cancel);
        g_clear_object(&hk.cancel);
    }
    g_clear_object(&hk.settings);
}
