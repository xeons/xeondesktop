/* SPDX-License-Identifier: MIT
 * Renaming drives, Disk Cleanup, and a drive's Tools and Hardware tabs.
 * See drives.h.
 */
#include "drives.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/magic.h>

#include "../gtkapp/gtkapp.h"
#include "cleanup.h"
#include "flatpakclean.h"
#include "places.h"
#include "syscleanup.h"
#include "udisks.h"

#define UD_ERROR "org.freedesktop.UDisks2.Error."

static constexpr GtkDialogFlags MODAL_DIALOG =
    (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);

static GtkWidget *wrapped_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 48);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    return label;
}

/* Runs a program (argv[0] from PATH), reporting failure over `parent`. */
static void spawn(GtkWindow *parent, const char *title, const char *prog, const char *arg) {
    char *argv[] = {g_strdup(prog), g_strdup(arg), nullptr};
    GError *err = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &err)) {
        xd_message_box(parent, title, XD_MB_ERROR, XD_MB_OK, FALSE, err->message);
        g_error_free(err);
    }
    g_free(argv[0]);
    g_free(argv[1]);
}

/* ---- the label -------------------------------------------------------------------- */

gboolean xd_drive_label(GFile *item, char **label) {
    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(item);
    if (label != nullptr)
        *label = fs != nullptr ? g_strdup(fs->label) : nullptr;
    return fs != nullptr;
}

typedef struct label_job {
    GtkWidget *parent; /* weak */
    char *name;
    void (*done)(gboolean ok, gpointer data);
    gpointer data;
} label_job;

static void label_set(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    label_job *label_request = data;
    GError *err = nullptr;
    GVariant *variant = xd_udisks_call_finish(res, nullptr, &err);
    g_clear_pointer(&variant, g_variant_unref);
    if (label_request->parent != nullptr) {
        g_object_remove_weak_pointer(G_OBJECT(label_request->parent),
                                     (gpointer *)&label_request->parent);
        if (err != nullptr) {
            char *msg = g_strdup_printf("Cannot rename %s: %s", label_request->name, err->message);
            xd_message_box(GTK_WINDOW(label_request->parent), "Error Renaming Drive", XD_MB_ERROR,
                           XD_MB_OK, FALSE, msg);
            g_free(msg);
        }
        if (label_request->done != nullptr)
            label_request->done(err == nullptr, label_request->data);
    }
    g_clear_error(&err);
    g_free(label_request->name);
    g_free(label_request);
}

void xd_drive_set_label(GtkWindow *parent, GFile *item, const char *name, const char *label,
                        void (*done)(gboolean ok, gpointer data), gpointer data) {
    label_job *label_request = g_new0(label_job, 1);
    label_request->parent = GTK_WIDGET(parent);
    g_object_add_weak_pointer(G_OBJECT(label_request->parent), (gpointer *)&label_request->parent);
    label_request->name = g_strdup(name);
    label_request->done = done;
    label_request->data = data;
    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(item);
    /* No such drive any more: let the call say so. */
    xd_udisks_call(fs != nullptr ? fs->object : "/org/freedesktop/UDisks2/missing",
                   XD_UDISKS_FILESYSTEM, "SetLabel", g_variant_new("(sa{sv})", label, nullptr),
                   nullptr, label_set, label_request);
}

/* ---- Disk Cleanup ---------------------------------------------------------------- */

gboolean xd_drive_can_cleanup(GFile *item, const char *kind) {
    if (!g_file_is_native(item) || kind == nullptr)
        return FALSE;
    return strcmp(kind, "Local Disk") == 0 || strcmp(kind, "Removable Disk") == 0;
}

/* Temporary folders held in memory free nothing on a disk. */
static gboolean in_memory(const char *path) {
    struct statfs fs;
    return statfs(path, &fs) == 0 && (fs.f_type == TMPFS_MAGIC || fs.f_type == RAMFS_MAGIC);
}

static XdCleanupPlaces *places_for(GFile *item) {
    char *root = g_file_get_path(item);
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *drives = xd_computer_drives();
    for (guint i = 0; i < drives->len; i++) {
        const XdDrive *drive = g_ptr_array_index(drives, i);
        char *path =
            strcmp(drive->kind, "File Folder") != 0 ? g_file_get_path(drive->root) : nullptr;
        if (path != nullptr)
            g_ptr_array_add(roots, path);
    }
    g_ptr_array_unref(drives);
    g_ptr_array_add(roots, nullptr);

    GPtrArray *temps = g_ptr_array_new_with_free_func(g_free);
    /* XD_CLEANUP_TEMP_DIRS (folders separated by colons; empty for none)
     * stands in for the temporary folders: the tests' sessions share the
     * real ones. */
    const char *given = g_getenv("XD_CLEANUP_TEMP_DIRS");
    char **from_env = given != nullptr ? g_strsplit(given, ":", -1) : nullptr;
    const char *defaults[] = {g_get_tmp_dir(), "/tmp", "/var/tmp", nullptr};
    const char *const *candidates =
        from_env != nullptr ? (const char *const *)from_env : defaults;
    for (const char *const *candidate = candidates; *candidate != nullptr; candidate++) {
        if ((*candidate)[0] == '\0')
            continue;
        char *real = g_canonicalize_filename(*candidate, nullptr);
        if (!in_memory(real) &&
            !g_ptr_array_find_with_equal_func(temps, real, g_str_equal, nullptr))
            g_ptr_array_add(temps, real);
        else
            g_free(real);
    }
    g_strfreev(from_env);
    g_ptr_array_add(temps, nullptr);

    XdCleanupPlaces *cleanup_places =
        xd_cleanup_places_new(root, (const char *const *)roots->pdata, g_get_user_data_dir(),
                              g_get_user_cache_dir(), g_get_home_dir(),
                              (const char *const *)temps->pdata, getuid());
    g_ptr_array_unref(temps);
    g_ptr_array_unref(roots);
    g_free(root);
    return cleanup_places;
}

enum { COL_CHECK, COL_ICON, COL_NAME, COL_SIZE, COL_INDEX, N_COLS };

typedef struct cleanup {
    GtkWidget *parent; /* weak */
    GApplication *app; /* run on its own (xd-files --disk-cleanup): held while it runs */
    char *name;
    XdCleanupPlaces *places;
    GCancellable *cancel;
    gint scanning; /* the category being scanned or cleaned */
    guint tick;
    GPtrArray *cats;
    /* the progress box */
    GtkWidget *progress, *bar, *doing;
    gboolean cleaning;
    GString *errors;
    /* the Disk Cleanup for <name> window */
    GtkWidget *dlg, *list, *total, *desc, *view, *intro, *system_files;
    GtkListStore *store;
} cleanup;

static void cleanup_free(cleanup *disk_cleanup) {
    if (disk_cleanup->tick != 0)
        g_source_remove(disk_cleanup->tick);
    if (disk_cleanup->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(disk_cleanup->parent),
                                     (gpointer *)&disk_cleanup->parent);
    g_free(disk_cleanup->name);
    xd_cleanup_places_free(disk_cleanup->places);
    g_clear_object(&disk_cleanup->cancel);
    g_clear_pointer(&disk_cleanup->cats, g_ptr_array_unref);
    g_clear_object(&disk_cleanup->store);
    if (disk_cleanup->errors != nullptr)
        g_string_free(disk_cleanup->errors, TRUE);
    if (disk_cleanup->app != nullptr)
        g_application_release(disk_cleanup->app);
    g_free(disk_cleanup);
}

static const char *const category_icons[XD_CLEANUP_KIND_COUNT] = {
    [XD_CLEANUP_INTERNET] = "web-browser",
    [XD_CLEANUP_TRASH] = "user-trash-full",
    [XD_CLEANUP_TEMP] = "text-x-generic",
    [XD_CLEANUP_THUMBNAILS] = "image-x-generic",
    [XD_CLEANUP_SHADERS] = "video-display",
    [XD_CLEANUP_DEVELOPER] = "applications-development",
    [XD_CLEANUP_PROGRAMS] = "application-x-executable",
    [XD_CLEANUP_FLATPAK] = "system-software-install",
    [XD_CLEANUP_PACKAGES] = "package-x-generic",
    [XD_CLEANUP_LOGS] = "text-x-generic",
    [XD_CLEANUP_CRASHES] = "dialog-warning",
};

static gboolean on_progress_tick(gpointer data) {
    cleanup *disk_cleanup = data;
    if (disk_cleanup->progress == nullptr)
        return G_SOURCE_CONTINUE;
    gint kind = g_atomic_int_get(&disk_cleanup->scanning);
    char *text = g_strdup_printf("%s %s", disk_cleanup->cleaning ? "Cleaning:" : "Scanning:",
                                 xd_cleanup_kind_title((XdCleanupKind)kind));
    gtk_label_set_text(GTK_LABEL(disk_cleanup->doing), text);
    g_free(text);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(disk_cleanup->bar),
                                  (double)(kind + 1) / (double)XD_CLEANUP_KIND_COUNT);
    return G_SOURCE_CONTINUE;
}

static void on_progress_gone(cleanup *disk_cleanup) {
    disk_cleanup->progress = nullptr;
    g_cancellable_cancel(disk_cleanup->cancel);
}

static void on_progress_cancel(GtkDialog *dlg, gint response, gpointer data) {
    (void)dlg;
    (void)response;
    g_cancellable_cancel(((cleanup *)data)->cancel);
}

/* XP's small box while Disk Cleanup works: a line of text, the bar, and
 * what it's on. */
static void show_progress(cleanup *disk_cleanup, const char *text) {
    char *title = g_strdup_printf("Disk Cleanup for %s", disk_cleanup->name);
    disk_cleanup->progress = gtk_dialog_new_with_buttons(
        title, disk_cleanup->parent != nullptr ? GTK_WINDOW(disk_cleanup->parent) : nullptr,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(disk_cleanup->progress), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(disk_cleanup->progress), "edit-clear");
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(disk_cleanup->progress));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name("edit-clear", GTK_ICON_SIZE_DND),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), wrapped_label(text), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    disk_cleanup->bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), disk_cleanup->bar, FALSE, FALSE, 0);
    disk_cleanup->doing = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(disk_cleanup->doing), 0);
    gtk_label_set_ellipsize(GTK_LABEL(disk_cleanup->doing), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), disk_cleanup->doing, FALSE, FALSE, 0);
    g_signal_connect(disk_cleanup->progress, "response", G_CALLBACK(on_progress_cancel),
                     disk_cleanup);
    /* Closing it cancels, as Cancel; it goes when the work has stopped. */
    g_signal_connect(disk_cleanup->progress, "delete-event", G_CALLBACK(gtk_true), nullptr);
    /* (Or with the window it belongs to.) */
    g_signal_connect_swapped(disk_cleanup->progress, "destroy", G_CALLBACK(on_progress_gone),
                             disk_cleanup);
    on_progress_tick(disk_cleanup);
    disk_cleanup->tick = g_timeout_add(100, on_progress_tick, disk_cleanup);
    gtk_widget_show_all(disk_cleanup->progress);
}

static void end_progress(cleanup *disk_cleanup) {
    if (disk_cleanup->tick != 0)
        g_source_remove(disk_cleanup->tick);
    disk_cleanup->tick = 0;
    if (disk_cleanup->progress != nullptr) {
        g_signal_handlers_disconnect_by_data(disk_cleanup->progress, disk_cleanup);
        gtk_widget_destroy(disk_cleanup->progress);
        disk_cleanup->progress = nullptr;
    }
}

static XdCleanupCategory *selected_category(cleanup *disk_cleanup) {
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(disk_cleanup->list));
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return nullptr;
    guint index = 0;
    gtk_tree_model_get(model, &it, COL_INDEX, &index, -1);
    return index < disk_cleanup->cats->len ? g_ptr_array_index(disk_cleanup->cats, index) : nullptr;
}

static void update_total(cleanup *disk_cleanup) {
    guint64 total = 0;
    for (guint i = 0; i < disk_cleanup->cats->len; i++) {
        const XdCleanupCategory *cat = g_ptr_array_index(disk_cleanup->cats, i);
        if (cat->checked)
            total += cat->size;
    }
    char *text = xd_cleanup_format_kb(total);
    gtk_label_set_text(GTK_LABEL(disk_cleanup->total), text);
    g_free(text);
}

static void on_category_selected(GtkTreeSelection *sel, gpointer data) {
    (void)sel;
    cleanup *disk_cleanup = data;
    const XdCleanupCategory *cat = selected_category(disk_cleanup);
    gtk_label_set_text(GTK_LABEL(disk_cleanup->desc), cat != nullptr ? cat->description : "");
    gtk_widget_set_sensitive(disk_cleanup->view, cat != nullptr && cat->view != nullptr);
}

static void on_category_toggled(GtkCellRendererToggle *cell, char *path, gpointer data) {
    (void)cell;
    cleanup *disk_cleanup = data;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(disk_cleanup->store), &it, path))
        return;
    guint index = 0;
    gboolean on = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(disk_cleanup->store), &it, COL_INDEX, &index, COL_CHECK, &on,
                       -1);
    gtk_list_store_set(disk_cleanup->store, &it, COL_CHECK, !on, -1);
    if (index < disk_cleanup->cats->len)
        ((XdCleanupCategory *)g_ptr_array_index(disk_cleanup->cats, index))->checked = !on;
    update_total(disk_cleanup);
}

/* Space toggles the selected category's box, as in XP's list; Enter is
 * the dialog's (OK), not the list's. */
static gboolean on_list_key(GtkWidget *widget, GdkEventKey *ev, gpointer data) {
    (void)widget;
    cleanup *disk_cleanup = data;
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        gtk_window_activate_default(GTK_WINDOW(disk_cleanup->dlg));
        return TRUE;
    }
    if (ev->keyval != GDK_KEY_space)
        return FALSE;
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(disk_cleanup->list));
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        char *path = gtk_tree_model_get_string_from_iter(model, &it);
        on_category_toggled(nullptr, path, disk_cleanup);
        g_free(path);
    }
    return TRUE;
}

static void on_view_files(GtkButton *button, gpointer data) {
    (void)button;
    cleanup *disk_cleanup = data;
    const XdCleanupCategory *cat = selected_category(disk_cleanup);
    if (cat != nullptr && cat->view != nullptr)
        spawn(GTK_WINDOW(disk_cleanup->dlg), "Disk Cleanup", "xd-files", cat->view);
}

static void on_more_programs(GtkButton *button, gpointer data) {
    (void)button;
    spawn(GTK_WINDOW(((cleanup *)data)->dlg), "Disk Cleanup", "xd-appwiz", nullptr);
}

/* The system files' places: the real ones, or a test's stand-ins. */
static xd_sysclean_env system_env(void) {
    const char *root = g_getenv("XD_CLEANMGR_ROOT");
    return (xd_sysclean_env){.root = root != nullptr ? root : "",
                             .tools = g_getenv("XD_CLEANMGR_TOOLS"),
                             .now = g_get_real_time()};
}

/* Cleans the ticked system files at once: xd-cleanmgr-helper, through
 * pkexec (one password for them all). A test runs its own helper,
 * XD_CLEANMGR_HELPER, directly. */
static void clean_system_files(GPtrArray *names, GString *errors) {
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    const char *helper = g_getenv("XD_CLEANMGR_HELPER");
    if (helper != nullptr) {
        g_ptr_array_add(argv, g_strdup(helper));
    } else {
        g_ptr_array_add(argv, g_strdup("pkexec"));
        g_ptr_array_add(argv, g_build_filename(XD_LIBEXECDIR, "xd-cleanmgr-helper", nullptr));
    }
    g_ptr_array_add(argv, g_strdup("clean"));
    for (guint i = 0; i < names->len; i++)
        g_ptr_array_add(argv, g_strdup(g_ptr_array_index(names, i)));
    g_ptr_array_add(argv, nullptr);
    char *err_out = nullptr;
    int status = 0;
    GError *err = nullptr;
    const char *problem = nullptr;
    if (!g_spawn_sync(nullptr, (char **)argv->pdata, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                      nullptr, nullptr, &err_out, &status, &err)) {
        problem = err->message;
    } else if (!g_spawn_check_wait_status(status, nullptr)) {
        bool refused = helper == nullptr && WIFEXITED(status) &&
                       (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127);
        /* pkexec's 126 and 127: not allowed, or the password box was
         * cancelled. */
        if (refused)
            problem = "System files can only be cleaned up with an administrator's password.";
        else if (err_out != nullptr && err_out[0] != '\0')
            problem = g_strstrip(err_out);
        else
            problem = "System files could not be cleaned up.";
    }
    if (problem != nullptr)
        g_string_append_printf(errors, "%s%s", errors->len > 0 ? "\n" : "", problem);
    g_clear_error(&err);
    g_free(err_out);
    g_ptr_array_free(argv, TRUE);
}

static void clean_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    cleanup *disk_cleanup = data;
    GString *errors = g_string_new(nullptr);
    GPtrArray *system_names = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < disk_cleanup->cats->len && !g_cancellable_is_cancelled(cancel); i++) {
        const XdCleanupCategory *cat = g_ptr_array_index(disk_cleanup->cats, i);
        if (!cat->checked)
            continue;
        if (xd_cleanup_kind_is_system(cat->kind)) {
            g_ptr_array_add(system_names, g_strdup(xd_sysclean_name(cat->kind)));
            continue;
        }
        g_atomic_int_set(&disk_cleanup->scanning, (gint)cat->kind);
        GError *err = nullptr;
        bool ok = cat->kind == XD_CLEANUP_FLATPAK
                      ? xd_flatpak_unused_remove(cat, cancel, &err)
                      : xd_cleanup_delete(cat, getuid(), g_get_real_time(), cancel, &err);
        if (!ok) {
            g_string_append_printf(errors, "%s%s", errors->len > 0 ? "\n" : "",
                                   err != nullptr ? err->message : "");
            g_clear_error(&err);
        }
    }
    if (system_names->len > 0 && !g_cancellable_is_cancelled(cancel)) {
        g_atomic_int_set(&disk_cleanup->scanning, (gint)XD_CLEANUP_PACKAGES);
        clean_system_files(system_names, errors);
    }
    g_ptr_array_free(system_names, TRUE);
    g_task_return_pointer(task, errors, nullptr);
}

static void cleaned(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    cleanup *disk_cleanup = data;
    GString *errors = g_task_propagate_pointer(G_TASK(res), nullptr);
    end_progress(disk_cleanup);
    if (errors != nullptr && errors->len > 0 &&
        (disk_cleanup->parent != nullptr || disk_cleanup->app != nullptr)) {
        char *title = g_strdup_printf("Disk Cleanup for %s", disk_cleanup->name);
        char *msg = g_strdup_printf("Disk Cleanup could not delete some of the files.\n\n%s",
                                    errors->str);
        xd_message_box(disk_cleanup->parent != nullptr ? GTK_WINDOW(disk_cleanup->parent) : nullptr,
                       title, XD_MB_WARNING, XD_MB_OK, FALSE, msg);
        g_free(msg);
        g_free(title);
    }
    if (errors != nullptr)
        g_string_free(errors, TRUE);
    cleanup_free(disk_cleanup);
}

static void on_cleanup_response(GtkDialog *dlg, gint response, gpointer data) {
    cleanup *disk_cleanup = data;
    gboolean any = FALSE;
    for (guint i = 0; i < disk_cleanup->cats->len; i++)
        any = any || ((XdCleanupCategory *)g_ptr_array_index(disk_cleanup->cats, i))->checked;
    if (response == GTK_RESPONSE_OK && any) {
        char *title = g_strdup_printf("Disk Cleanup for %s", disk_cleanup->name);
        int answer = xd_message_box(GTK_WINDOW(dlg), title, XD_MB_QUESTION, XD_MB_YES_NO, FALSE,
                                    "Are you sure you want to perform these actions?");
        g_free(title);
        if (answer != GTK_RESPONSE_YES)
            return;
    }
    disk_cleanup->dlg = nullptr;
    gtk_widget_destroy(GTK_WIDGET(dlg));
    if (response != GTK_RESPONSE_OK || !any) {
        cleanup_free(disk_cleanup);
        return;
    }
    disk_cleanup->cleaning = TRUE;
    g_atomic_int_set(&disk_cleanup->scanning, 0);
    show_progress(disk_cleanup, "Disk Cleanup is cleaning up unnecessary files on your machine.");
    GTask *task = g_task_new(nullptr, disk_cleanup->cancel, cleaned, disk_cleanup);
    g_task_set_task_data(task, disk_cleanup, nullptr);
    g_task_run_in_thread(task, clean_thread);
    g_object_unref(task);
}

static void on_cleanup_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    cleanup *disk_cleanup = data;
    /* Closed with its parent: nothing to do any more. */
    if (disk_cleanup->dlg != nullptr) {
        disk_cleanup->dlg = nullptr;
        cleanup_free(disk_cleanup);
    }
}

/* "You can use Disk Cleanup to free up to 12,345 KB of disk space on
 * <name>.", for every category listed. */
static void update_intro(cleanup *disk_cleanup) {
    guint64 all = 0;
    for (guint i = 0; i < disk_cleanup->cats->len; i++)
        all += ((XdCleanupCategory *)g_ptr_array_index(disk_cleanup->cats, i))->size;
    char *kb = xd_cleanup_format_kb(all);
    char *intro = g_strdup_printf("You can use Disk Cleanup to free up to %s of disk space on %s.",
                                  kb, disk_cleanup->name);
    gtk_label_set_text(GTK_LABEL(disk_cleanup->intro), intro);
    g_free(intro);
    g_free(kb);
}

static void add_category_row(cleanup *disk_cleanup, guint index) {
    const XdCleanupCategory *cat = g_ptr_array_index(disk_cleanup->cats, index);
    char *size = xd_cleanup_format_kb(cat->size);
    gtk_list_store_insert_with_values(disk_cleanup->store, nullptr, -1, COL_CHECK, cat->checked,
                                      COL_ICON, category_icons[cat->kind], COL_NAME, cat->title,
                                      COL_SIZE, size, COL_INDEX, index, -1);
    g_free(size);
}

static void system_scan_thread(GTask *task, gpointer source, gpointer data,
                               GCancellable *cancel) {
    (void)source;
    cleanup *disk_cleanup = data;
    xd_sysclean_env env = system_env();
    GPtrArray *found = g_ptr_array_new();
    const XdCleanupKind kinds[] = {XD_CLEANUP_PACKAGES, XD_CLEANUP_LOGS, XD_CLEANUP_CRASHES};
    for (size_t i = 0; i < G_N_ELEMENTS(kinds) && !g_cancellable_is_cancelled(cancel); i++) {
        g_atomic_int_set(&disk_cleanup->scanning, (gint)kinds[i]);
        XdCleanupCategory *cat = xd_sysclean_scan(kinds[i], &env, cancel);
        if (cat != nullptr)
            g_ptr_array_add(found, cat);
    }
    g_task_return_pointer(task, found, nullptr);
}

static void system_scanned(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    cleanup *disk_cleanup = data;
    GPtrArray *found = g_task_propagate_pointer(G_TASK(res), nullptr);
    end_progress(disk_cleanup);
    /* The list's array frees them, from now on. */
    for (guint i = 0; found != nullptr && i < found->len; i++) {
        g_ptr_array_add(disk_cleanup->cats, g_ptr_array_index(found, i));
        if (disk_cleanup->dlg != nullptr)
            add_category_row(disk_cleanup, disk_cleanup->cats->len - 1);
    }
    if (disk_cleanup->dlg != nullptr) {
        update_intro(disk_cleanup);
        update_total(disk_cleanup);
        if (found == nullptr || found->len == 0) {
            char *title = g_strdup_printf("Disk Cleanup for %s", disk_cleanup->name);
            xd_message_box(GTK_WINDOW(disk_cleanup->dlg), title, XD_MB_INFO, XD_MB_OK,
                           FALSE, "Disk Cleanup found no system files to clean up.");
            g_free(title);
        }
    }
    if (found != nullptr)
        g_ptr_array_free(found, TRUE);
}

/* Windows 7's Clean up system files: the system's files that only an
 * administrator may delete join the list (measured as the user; the
 * password is asked for when they're cleaned). */
static void on_system_files(GtkButton *button, gpointer data) {
    cleanup *disk_cleanup = data;
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    disk_cleanup->cleaning = FALSE;
    g_atomic_int_set(&disk_cleanup->scanning, (gint)XD_CLEANUP_PACKAGES);
    char *text = g_strdup_printf("Disk Cleanup is calculating how much space you will be able to "
                                 "free on %s. This may take a few minutes to complete.",
                                 disk_cleanup->name);
    show_progress(disk_cleanup, text);
    g_free(text);
    GTask *task = g_task_new(nullptr, disk_cleanup->cancel, system_scanned, disk_cleanup);
    g_task_set_task_data(task, disk_cleanup, nullptr);
    g_task_run_in_thread(task, system_scan_thread);
    g_object_unref(task);
}

static GtkWidget *cleanup_page(cleanup *disk_cleanup) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name("edit-clear", GTK_ICON_SIZE_DND),
                       FALSE, FALSE, 0);
    disk_cleanup->intro = wrapped_label("");
    gtk_box_pack_start(GTK_BOX(row), disk_cleanup->intro, TRUE, TRUE, 0);
    update_intro(disk_cleanup);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *files = gtk_label_new_with_mnemonic("_Files to delete:");
    gtk_label_set_xalign(GTK_LABEL(files), 0);
    gtk_box_pack_start(GTK_BOX(box), files, FALSE, FALSE, 0);
    disk_cleanup->store = gtk_list_store_new(N_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_UINT);
    for (guint i = 0; i < disk_cleanup->cats->len; i++)
        add_category_row(disk_cleanup, i);
    disk_cleanup->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(disk_cleanup->store));
    gtk_label_set_mnemonic_widget(GTK_LABEL(files), disk_cleanup->list);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(disk_cleanup->list), FALSE);
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_category_toggled), disk_cleanup);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col, toggle, FALSE);
    gtk_tree_view_column_add_attribute(col, toggle, "active", COL_CHECK);
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", COL_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", COL_NAME);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(disk_cleanup->list), col);
    GtkCellRenderer *size = gtk_cell_renderer_text_new();
    g_object_set(size, "xalign", 1.0, nullptr);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(disk_cleanup->list),
        gtk_tree_view_column_new_with_attributes("", size, "text", COL_SIZE, nullptr));
    g_signal_connect(disk_cleanup->list, "key-press-event", G_CALLBACK(on_list_key), disk_cleanup);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_container_add(GTK_CONTAINER(scroll), disk_cleanup->list);
    gtk_box_pack_start(GTK_BOX(box), scroll, FALSE, FALSE, 0);

    GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *tl = gtk_label_new("Total amount of disk space you gain:");
    gtk_label_set_xalign(GTK_LABEL(tl), 0);
    gtk_box_pack_start(GTK_BOX(trow), tl, TRUE, TRUE, 0);
    disk_cleanup->total = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(trow), disk_cleanup->total, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), trow, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new("Description");
    GtkWidget *fbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(fbox), 8);
    disk_cleanup->desc = wrapped_label("");
    gtk_label_set_lines(GTK_LABEL(disk_cleanup->desc), 5);
    gtk_widget_set_size_request(disk_cleanup->desc, -1, 80);
    gtk_label_set_yalign(GTK_LABEL(disk_cleanup->desc), 0);
    gtk_box_pack_start(GTK_BOX(fbox), disk_cleanup->desc, TRUE, TRUE, 0);
    disk_cleanup->view = gtk_button_new_with_mnemonic("_View Files");
    gtk_widget_set_halign(disk_cleanup->view, GTK_ALIGN_END);
    g_signal_connect(disk_cleanup->view, "clicked", G_CALLBACK(on_view_files), disk_cleanup);
    gtk_box_pack_start(GTK_BOX(fbox), disk_cleanup->view, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), fbox);
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);
    /* Only for the drive that holds the system's files. */
    if (xd_cleanup_places_holds(disk_cleanup->places, "/var")) {
        disk_cleanup->system_files = gtk_button_new_with_mnemonic("Clean up _system files");
        gtk_button_set_image(GTK_BUTTON(disk_cleanup->system_files),
                             gtk_image_new_from_icon_name("security-high", GTK_ICON_SIZE_BUTTON));
        gtk_button_set_always_show_image(GTK_BUTTON(disk_cleanup->system_files), TRUE);
        gtk_widget_set_halign(disk_cleanup->system_files, GTK_ALIGN_START);
        g_signal_connect(disk_cleanup->system_files, "clicked", G_CALLBACK(on_system_files),
                         disk_cleanup);
        gtk_box_pack_start(GTK_BOX(box), disk_cleanup->system_files, FALSE, FALSE, 0);
    }

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(disk_cleanup->list));
    g_signal_connect(sel, "changed", G_CALLBACK(on_category_selected), disk_cleanup);
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(disk_cleanup->store), &first))
        gtk_tree_selection_select_iter(sel, &first);
    on_category_selected(sel, disk_cleanup);
    update_total(disk_cleanup);
    return box;
}

/* More Options: XP's Installed programs (Add or Remove Programs); there
 * are no Windows components or System Restore points to clean up. */
static GtkWidget *more_options_page(cleanup *disk_cleanup) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *frame = gtk_frame_new("Installed programs");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    gtk_grid_attach(GTK_GRID(grid),
                    gtk_image_new_from_icon_name("system-software-install", GTK_ICON_SIZE_DND), 0,
                    0, 1, 1);
    GtkWidget *text =
        wrapped_label("You can free more disk space by removing programs that you do not use.");
    gtk_widget_set_hexpand(text, TRUE);
    gtk_grid_attach(GTK_GRID(grid), text, 1, 0, 1, 1);
    GtkWidget *button = gtk_button_new_with_mnemonic("_Clean up...");
    gtk_widget_set_halign(button, GTK_ALIGN_END);
    g_signal_connect(button, "clicked", G_CALLBACK(on_more_programs), disk_cleanup);
    gtk_grid_attach(GTK_GRID(grid), button, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return box;
}

static void show_cleanup(cleanup *disk_cleanup) {
    char *title = g_strdup_printf("Disk Cleanup for %s", disk_cleanup->name);
    disk_cleanup->dlg = gtk_dialog_new_with_buttons(
        title, disk_cleanup->parent != nullptr ? GTK_WINDOW(disk_cleanup->parent) : nullptr,
        GTK_DIALOG_DESTROY_WITH_PARENT, "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
        nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(disk_cleanup->dlg), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(disk_cleanup->dlg), "edit-clear");
    gtk_dialog_set_default_response(GTK_DIALOG(disk_cleanup->dlg), GTK_RESPONSE_OK);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), cleanup_page(disk_cleanup),
                             gtk_label_new("Disk Cleanup"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), more_options_page(disk_cleanup),
                             gtk_label_new("More Options"));
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(disk_cleanup->dlg))),
                      nb);
    xd_gtk_notebook_ctrl_tab(disk_cleanup->dlg, GTK_NOTEBOOK(nb));
    g_signal_connect(disk_cleanup->dlg, "response", G_CALLBACK(on_cleanup_response), disk_cleanup);
    g_signal_connect(disk_cleanup->dlg, "destroy", G_CALLBACK(on_cleanup_destroy), disk_cleanup);
    gtk_widget_set_size_request(disk_cleanup->dlg, 380, -1);
    gtk_widget_show_all(disk_cleanup->dlg);
    gtk_widget_grab_focus(disk_cleanup->list);
}

static void scan_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    cleanup *disk_cleanup = data;
    GPtrArray *cats = xd_cleanup_scan(disk_cleanup->places, getuid(), g_get_real_time(), cancel,
                                      &disk_cleanup->scanning);
    g_atomic_int_set(&disk_cleanup->scanning, (gint)XD_CLEANUP_FLATPAK);
    XdCleanupCategory *flatpak = xd_flatpak_unused_scan(disk_cleanup->places, cancel);
    if (flatpak != nullptr)
        g_ptr_array_add(cats, flatpak);
    g_task_return_pointer(task, cats, (GDestroyNotify)g_ptr_array_unref);
}

static void scanned(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    cleanup *disk_cleanup = data;
    disk_cleanup->cats = g_task_propagate_pointer(G_TASK(res), nullptr);
    end_progress(disk_cleanup);
    if (disk_cleanup->cats == nullptr || g_cancellable_is_cancelled(disk_cleanup->cancel) ||
        (disk_cleanup->parent == nullptr && disk_cleanup->app == nullptr)) {
        cleanup_free(disk_cleanup);
        return;
    }
    show_cleanup(disk_cleanup);
}

static void start_cleanup(GtkWindow *parent, GApplication *app, GFile *item, const char *name) {
    cleanup *disk_cleanup = g_new0(cleanup, 1);
    if (parent != nullptr) {
        disk_cleanup->parent = GTK_WIDGET(parent);
        g_object_add_weak_pointer(G_OBJECT(disk_cleanup->parent),
                                  (gpointer *)&disk_cleanup->parent);
    }
    if (app != nullptr) {
        disk_cleanup->app = app;
        g_application_hold(app);
    }
    disk_cleanup->name = g_strdup(name);
    disk_cleanup->cancel = g_cancellable_new();
    disk_cleanup->places = places_for(item);
    char *text = g_strdup_printf("Disk Cleanup is calculating how much space you will be able to "
                                 "free on %s. This may take a few minutes to complete.",
                                 name);
    show_progress(disk_cleanup, text);
    g_free(text);
    GTask *task = g_task_new(nullptr, disk_cleanup->cancel, scanned, disk_cleanup);
    g_task_set_task_data(task, disk_cleanup, nullptr);
    g_task_run_in_thread(task, scan_thread);
    g_object_unref(task);
}

void xd_drive_cleanup(GtkWindow *parent, GFile *item, const char *name) {
    start_cleanup(parent, nullptr, item, name);
}

/* The drives Disk Cleanup can clean, in My Computer's order. */
static GPtrArray *cleanable_drives(void) {
    GPtrArray *drives = xd_computer_drives();
    for (guint i = drives->len; i > 0; i--) {
        const XdDrive *drive = g_ptr_array_index(drives, i - 1);
        if (!xd_drive_can_cleanup(drive->root, drive->kind))
            g_ptr_array_remove_index(drives, i - 1);
    }
    return drives;
}

typedef struct select_drive {
    GApplication *app;
    GPtrArray *drives;
    GtkWidget *combo;
} select_drive;

static void on_select_drive_response(GtkDialog *dlg, gint response, gpointer data) {
    select_drive *selection = data;
    gint index = gtk_combo_box_get_active(GTK_COMBO_BOX(selection->combo));
    if (response == GTK_RESPONSE_OK && index >= 0 && (guint)index < selection->drives->len) {
        const XdDrive *drive = g_ptr_array_index(selection->drives, (guint)index);
        start_cleanup(nullptr, selection->app, drive->root, drive->name);
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
    g_ptr_array_unref(selection->drives);
    g_application_release(selection->app);
    g_free(selection);
}

void xd_disk_cleanup_run(GApplication *app, GFile *drive_root) {
    GPtrArray *drives = cleanable_drives();
    /* A drive named on the command line, or the only one (most Linux
     * systems have one file system for everything): straight to it. */
    for (guint i = 0; i < drives->len; i++) {
        const XdDrive *drive = g_ptr_array_index(drives, i);
        if (drives->len == 1 || (drive_root != nullptr && g_file_equal(drive->root, drive_root))) {
            start_cleanup(nullptr, app, drive->root, drive->name);
            g_ptr_array_unref(drives);
            return;
        }
    }
    /* XP's Select Drive: the drives in a list, the first chosen. */
    select_drive *selection = g_new0(select_drive, 1);
    selection->app = app;
    selection->drives = drives;
    g_application_hold(app);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Disk Cleanup : Drive Selection", nullptr, GTK_DIALOG_DESTROY_WITH_PARENT, "OK",
        GTK_RESPONSE_OK, "E_xit", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "edit-clear");
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_attach(GTK_GRID(grid), gtk_image_new_from_icon_name("edit-clear", GTK_ICON_SIZE_DND),
                    0, 0, 1, 2);
    GtkWidget *text = gtk_label_new("Select the drive you want to clean up.");
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_grid_attach(GTK_GRID(grid), text, 1, 0, 1, 1);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Drives:");
    selection->combo = gtk_combo_box_text_new();
    for (guint i = 0; i < drives->len; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(selection->combo),
                                       ((const XdDrive *)g_ptr_array_index(drives, i))->name);
    gtk_combo_box_set_active(GTK_COMBO_BOX(selection->combo), drives->len > 0 ? 0 : -1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), selection->combo);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), selection->combo, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(grid), row, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid);
    g_signal_connect(dlg, "response", G_CALLBACK(on_select_drive_response), selection);
    gtk_widget_show_all(dlg);
}

/* ---- Tools: Check Disk ------------------------------------------------------------- */

typedef struct check {
    GtkWidget *dlg, *fix, *bad, *bar, *start;
    char *object, *name;
    gboolean repair;
    guint pulse;
} check;

static gboolean on_check_pulse(gpointer data) {
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(((check *)data)->bar));
    return G_SOURCE_CONTINUE;
}

static void check_free(check *disk_check) {
    if (disk_check->pulse != 0)
        g_source_remove(disk_check->pulse);
    g_free(disk_check->object);
    g_free(disk_check->name);
    g_free(disk_check);
}

static void on_check_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    check *disk_check = data;
    disk_check->dlg = nullptr;
    if (disk_check->pulse != 0)
        g_source_remove(disk_check->pulse);
    disk_check->pulse = 0;
    /* A check under way frees it when UDisks2 answers. */
    if (disk_check->start != nullptr)
        check_free(disk_check);
}

static void checked(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    check *disk_check = data;
    GError *err = nullptr;
    g_autofree char *dbus_error = nullptr;
    GVariant *variant = xd_udisks_call_finish(res, &dbus_error, &err);
    if (disk_check->dlg == nullptr) {
        g_clear_pointer(&variant, g_variant_unref);
        g_clear_error(&err);
        check_free(disk_check);
        return;
    }
    if (disk_check->pulse != 0)
        g_source_remove(disk_check->pulse);
    disk_check->pulse = 0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(disk_check->bar),
                                  variant != nullptr ? 1.0 : 0.0);
    gboolean fine = FALSE;
    if (variant != nullptr) {
        g_variant_get(variant, "(b)", &fine);
        g_variant_unref(variant);
    }
    char *title = g_strdup_printf("Checking Disk %s", disk_check->name);
    char *msg;
    xd_mb_icon icon = XD_MB_INFO;
    if (err != nullptr) {
        icon = XD_MB_ERROR;
        gboolean busy = g_strcmp0(dbus_error, UD_ERROR "DeviceBusy") == 0 ||
                        strstr(err->message, "mounted") != nullptr;
        msg = busy ? g_strdup_printf("The disk check could not be performed because exclusive "
                                     "access to the drive could not be obtained.\n\n%s",
                                     err->message)
                   : g_strdup_printf("The disk check could not be performed.\n\n%s",
                                     err->message);
    } else if (fine) {
        msg = g_strdup("Disk Check Complete.");
    } else if (disk_check->repair) {
        icon = XD_MB_WARNING;
        msg =
            g_strdup_printf("Disk Check could not repair all the errors on %s.", disk_check->name);
    } else {
        icon = XD_MB_WARNING;
        msg = g_strdup_printf("Disk Check found errors on %s. To fix them, check the disk "
                              "again with \"Automatically fix file system errors\" selected.",
                              disk_check->name);
    }
    g_clear_error(&err);
    xd_message_box(GTK_WINDOW(disk_check->dlg), title, icon, XD_MB_OK, FALSE, msg);
    g_free(msg);
    g_free(title);
    /* As XP, the box closes once the result has been seen. */
    if (disk_check->dlg != nullptr)
        gtk_widget_destroy(disk_check->dlg);
    check_free(disk_check);
}

static void on_check_response(GtkDialog *dlg, gint response, gpointer data) {
    check *disk_check = data;
    if (response != GTK_RESPONSE_OK) {
        if (disk_check->start != nullptr) /* not while UDisks2 is checking */
            gtk_widget_destroy(GTK_WIDGET(dlg));
        return;
    }
    if (disk_check->start == nullptr)
        return;
    disk_check->repair = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(disk_check->fix));
    gtk_widget_set_sensitive(disk_check->fix, FALSE);
    gtk_widget_set_sensitive(disk_check->start, FALSE);
    gtk_dialog_set_response_sensitive(dlg, GTK_RESPONSE_CANCEL, FALSE);
    disk_check->start = nullptr; /* under way: the dialog no longer owns it */
    disk_check->pulse = g_timeout_add(100, on_check_pulse, disk_check);
    xd_udisks_call(disk_check->object, XD_UDISKS_FILESYSTEM,
                   disk_check->repair ? "Repair" : "Check", g_variant_new("(a{sv})", nullptr),
                   nullptr, checked, disk_check);
}

static void check_disk(GtkWindow *parent, const char *object, const char *name,
                       gboolean can_repair) {
    check *disk_check = g_new0(check, 1);
    disk_check->object = g_strdup(object);
    disk_check->name = g_strdup(name);
    char *title = g_strdup_printf("Check Disk %s", name);
    disk_check->dlg = gtk_dialog_new_with_buttons(title, parent, MODAL_DIALOG, nullptr, nullptr);
    g_free(title);
    disk_check->start =
        gtk_dialog_add_button(GTK_DIALOG(disk_check->dlg), "_Start", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(disk_check->dlg), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(disk_check->dlg), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(disk_check->dlg), FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(disk_check->dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_set_spacing(GTK_BOX(box), 10);
    GtkWidget *frame = gtk_frame_new("Check disk options");
    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(opts), 8);
    /* Without it, UDisks2 only checks (read-only); with it, repairs. */
    disk_check->fix = gtk_check_button_new_with_mnemonic("_Automatically fix file system errors");
    gtk_widget_set_sensitive(disk_check->fix, can_repair);
    gtk_box_pack_start(GTK_BOX(opts), disk_check->fix, FALSE, FALSE, 0);
    /* Linux file systems keep their own bad-block lists (badblocks, run
     * by hand on an unmounted disk); UDisks2 has nothing for it. */
    disk_check->bad =
        gtk_check_button_new_with_mnemonic("_Scan for and attempt recovery of bad sectors");
    gtk_widget_set_sensitive(disk_check->bad, FALSE);
    gtk_box_pack_start(GTK_BOX(opts), disk_check->bad, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), opts);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    disk_check->bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), disk_check->bar, FALSE, FALSE, 0);
    g_signal_connect(disk_check->dlg, "response", G_CALLBACK(on_check_response), disk_check);
    g_signal_connect(disk_check->dlg, "destroy", G_CALLBACK(on_check_destroy), disk_check);
    gtk_widget_show_all(disk_check->dlg);
}

/* ---- the Tools tab ---------------------------------------------------------------- */

typedef struct tools {
    GtkWidget *dialog;
    GFile *item;
    char *name;
    GDesktopAppInfo *backup;
} tools;

static void tools_free(gpointer user_data) {
    tools *tools_tab = user_data;
    g_object_unref(tools_tab->item);
    g_free(tools_tab->name);
    g_clear_object(&tools_tab->backup);
    g_free(tools_tab);
}

static void on_check_now(GtkButton *button, gpointer data) {
    (void)button;
    tools *tools_tab = data;
    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(tools_tab->item);
    if (fs == nullptr) {
        xd_message_box(GTK_WINDOW(tools_tab->dialog), "Check Disk", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The disk check could not be performed: UDisks2 doesn't know this drive.");
        return;
    }
    check_disk(GTK_WINDOW(tools_tab->dialog), fs->object, tools_tab->name,
               xd_udisks_can("CanRepair", fs->type));
}

/* Disk Defragmenter's own verdict, said straight away: Linux's file
 * systems allocate files in extents and delay allocation, so they stay
 * in one piece without help. */
static void on_defragment(GtkButton *button, gpointer data) {
    (void)button;
    tools *tools_tab = data;
    char *msg = g_strdup_printf(
        "Analysis is complete for: %s\n\nYou do not need to defragment this volume.\n\n"
        "Its file system keeps files in one piece as it writes them, so it never needs "
        "defragmenting.",
        tools_tab->name);
    xd_message_box(GTK_WINDOW(tools_tab->dialog), "Disk Defragmenter", XD_MB_INFO, XD_MB_OK, FALSE,
                   msg);
    g_free(msg);
}

/* Disk Benchmark and Health, on this drive. */
static void on_test_now([[maybe_unused]] GtkButton *button, gpointer data) {
    tools *tools_tab = data;
    g_autofree char *path = g_file_get_path(tools_tab->item);
    const char *argv[] = {"xd-diskbench", "--drive", path, nullptr};
    GError *error = nullptr;
    if (path == nullptr ||
        !g_spawn_async(nullptr, (char **)(void *)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        xd_message_box(GTK_WINDOW(tools_tab->dialog), "Disk Benchmark and Health", XD_MB_ERROR,
                       XD_MB_OK, FALSE,
                       error != nullptr ? error->message : "This drive isn't a local one.");
        g_clear_error(&error);
    }
}

static void on_backup(GtkButton *button, gpointer data) {
    (void)button;
    tools *tools_tab = data;
    GError *err = nullptr;
    if (tools_tab->backup != nullptr &&
        !g_app_info_launch(G_APP_INFO(tools_tab->backup), nullptr, nullptr, &err)) {
        xd_message_box(GTK_WINDOW(tools_tab->dialog), "Backup", XD_MB_ERROR, XD_MB_OK, FALSE,
                       err->message);
        g_error_free(err);
    }
}

/* The backup programs Backup Now... can open, best known first. */
static GDesktopAppInfo *find_backup(void) {
    static const char *const ids[] = {
        "org.gnome.DejaDup.desktop",   "org.gnome.World.PikaBackup.desktop",
        "com.borgbase.Vorta.desktop",  "backintime-qt.desktop",
        "org.kde.kbackup.desktop",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(ids); i++) {
        GDesktopAppInfo *app = g_desktop_app_info_new(ids[i]);
        if (app != nullptr)
            return app;
    }
    return nullptr;
}

static void tool_frame(GtkWidget *box, const char *title, const char *icon, const char *text,
                       const char *button, GCallback cb, gpointer data, gboolean enabled,
                       const char *why) {
    GtkWidget *frame = gtk_frame_new(title);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    gtk_grid_attach(GTK_GRID(grid), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND), 0, 0,
                    1, 2);
    GtkWidget *label = wrapped_label(text);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 1, 0, 1, 1);
    GtkWidget *tool_button = gtk_button_new_with_mnemonic(button);
    gtk_widget_set_halign(tool_button, GTK_ALIGN_END);
    gtk_widget_set_sensitive(tool_button, enabled);
    if (!enabled && why != nullptr)
        gtk_widget_set_tooltip_text(tool_button, why);
    g_signal_connect(tool_button, "clicked", cb, data);
    gtk_grid_attach(GTK_GRID(grid), tool_button, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
}

GtkWidget *xd_drive_tools_page(GtkWidget *dialog, GFile *item, const char *name,
                               const char *kind) {
    tools *tools_tab = g_new0(tools, 1);
    tools_tab->dialog = dialog;
    tools_tab->item = g_object_ref(item);
    tools_tab->name = g_strdup(name);
    tools_tab->backup = find_backup();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    g_object_set_data_full(G_OBJECT(box), "xd-tools", tools_tab, tools_free);

    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(item);
    gboolean can_check = fs != nullptr && xd_udisks_can("CanCheck", fs->type);
    tool_frame(box, "Error-checking", "system-search",
               "This option will check the volume for errors.", "_Check Now...",
               G_CALLBACK(on_check_now), tools_tab, can_check,
               fs == nullptr ? "UDisks2 doesn't know this drive."
                             : "UDisks2 can't check this file system here.");
    gboolean local = kind != nullptr && (strcmp(kind, "Local Disk") == 0 ||
                                         strcmp(kind, "Removable Disk") == 0);
    tool_frame(box, "Defragmentation", "view-grid-symbolic",
               "This option will defragment files on the volume.", "_Defragment Now...",
               G_CALLBACK(on_defragment), tools_tab, local,
               "Only local disks can be defragmented.");
    tool_frame(box, "Performance and health", "drive-harddisk",
               "This option will test the drive's speed and show its health.", "_Test Now...",
               G_CALLBACK(on_test_now), tools_tab, local, "Only local disks can be tested.");
    tool_frame(box, "Backup", "document-save", "This option will back up files on the volume.",
               "_Backup Now...", G_CALLBACK(on_backup), tools_tab, tools_tab->backup != nullptr,
               "No backup program is installed (such as Déjà Dup or Pika Backup).");
    return box;
}

/* ---- the Hardware tab -------------------------------------------------------------- */

enum { HW_NAME, HW_TYPE, HW_INDEX, HW_COLS };

typedef struct hardware {
    GtkWidget *dialog, *list, *maker, *where, *status;
    GPtrArray *drives;
} hardware;

static void hardware_free(gpointer user_data) {
    hardware *hardware_tab = user_data;
    g_ptr_array_unref(hardware_tab->drives);
    g_free(hardware_tab);
}

static const char *manufacturer(const XdUdisksDrive *udisks_drive) {
    if (udisks_drive->vendor[0] != '\0')
        return udisks_drive->vendor;
    return udisks_drive->optical ? "(Standard CD-ROM drives)" : "(Standard disk drives)";
}

static char *location(const XdUdisksDrive *udisks_drive) {
    static const struct {
        const char *bus, *name;
    } buses[] = {{"usb", "USB"}, {"sdio", "SD card reader"}, {"ieee1394", "FireWire"}};
    const char *bus = nullptr;
    for (size_t i = 0; i < G_N_ELEMENTS(buses); i++)
        if (strcmp(udisks_drive->bus, buses[i].bus) == 0)
            bus = buses[i].name;
    if (udisks_drive->device[0] == '\0')
        return g_strdup(bus != nullptr ? bus : "Unknown");
    return bus != nullptr ? g_strdup_printf("%s (%s)", udisks_drive->device, bus)
                          : g_strdup(udisks_drive->device);
}

static const XdUdisksDrive *hw_selected(hardware *hardware_tab) {
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(hardware_tab->list));
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return nullptr;
    guint index = 0;
    gtk_tree_model_get(model, &it, HW_INDEX, &index, -1);
    return index < hardware_tab->drives->len ? g_ptr_array_index(hardware_tab->drives, index)
                                             : nullptr;
}

static void on_hw_selected(GtkTreeSelection *sel, gpointer data) {
    (void)sel;
    hardware *hardware_tab = data;
    const XdUdisksDrive *udisks_drive = hw_selected(hardware_tab);
    char *where = udisks_drive != nullptr ? location(udisks_drive) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(hardware_tab->maker),
                       udisks_drive != nullptr ? manufacturer(udisks_drive) : "");
    gtk_label_set_text(GTK_LABEL(hardware_tab->where), where);
    gtk_label_set_text(GTK_LABEL(hardware_tab->status),
                       udisks_drive != nullptr ? "This device is working properly." : "");
    g_free(where);
}

static void grid_row(GtkWidget *grid, int *row, const char *title, const char *value) {
    GtkWidget *title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    gtk_grid_attach(GTK_GRID(grid), title_label, 0, *row, 1, 1);
    GtkWidget *value_label = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(value_label), 0);
    gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
    gtk_widget_set_can_focus(value_label, FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(value_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(value_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), value_label, 1, (*row)++, 1, 1);
}

/* "68,719,476,736", as Windows groups digits. */
static void append_grouped(GString *string, guint64 number) {
    char digits[32];
    g_snprintf(digits, sizeof digits, "%" G_GUINT64_FORMAT, number);
    size_t len = strlen(digits);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            g_string_append_c(string, ',');
        g_string_append_c(string, digits[i]);
    }
}

static char *megabytes(guint64 bytes) {
    GString *string = g_string_new(nullptr);
    append_grouped(string, bytes / (1024 * 1024));
    g_string_append(string, " MB");
    return g_string_free(string, FALSE);
}

/* XP's device Properties: General (type, manufacturer, location,
 * status), Volumes (the disk and its volumes, filled at once rather than
 * after Populate) and Details (what UDisks2 knows of it). */
static void device_properties(GtkWindow *parent, const XdUdisksDrive *udisks_drive, guint index,
                              guint optical_index) {
    char *title = g_strdup_printf("%s Properties", udisks_drive->name);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, parent, MODAL_DIALOG, "OK",
                                                 GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
                                                 nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "document-properties");
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), nb);
    xd_gtk_notebook_ctrl_tab(dlg, GTK_NOTEBOOK(nb));
    const char *icon = udisks_drive->optical     ? "drive-optical"
                       : udisks_drive->removable ? "drive-removable-media"
                                                 : "drive-harddisk";

    /* General */
    GtkWidget *general = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(general), 6);
    gtk_grid_set_column_spacing(GTK_GRID(general), 12);
    gtk_container_set_border_width(GTK_CONTAINER(general), 10);
    int row = 0;
    gtk_grid_attach(GTK_GRID(general), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND), 0,
                    row, 1, 1);
    GtkWidget *name = gtk_label_new(udisks_drive->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_grid_attach(GTK_GRID(general), name, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(general), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++,
                    2, 1);
    char *where = location(udisks_drive);
    grid_row(general, &row, "Device type:", udisks_drive->type);
    grid_row(general, &row, "Manufacturer:", manufacturer(udisks_drive));
    grid_row(general, &row, "Location:", where);
    g_free(where);
    GtkWidget *frame = gtk_frame_new("Device status");
    GtkWidget *status = wrapped_label("This device is working properly.");
    gtk_widget_set_margin_start(status, 8);
    gtk_widget_set_margin_end(status, 8);
    gtk_widget_set_margin_top(status, 8);
    gtk_widget_set_margin_bottom(status, 30);
    gtk_container_add(GTK_CONTAINER(frame), status);
    gtk_grid_attach(GTK_GRID(general), frame, 0, row++, 2, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), general, gtk_label_new("General"));

    /* Volumes */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_box_pack_start(GTK_BOX(vbox),
                       wrapped_label("The volumes contained on this disk are listed below."),
                       FALSE, FALSE, 0);
    GtkWidget *vgrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(vgrid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(vgrid), 12);
    row = 0;
    char *disk = udisks_drive->optical ? g_strdup_printf("CD-ROM %u", optical_index)
                                       : g_strdup_printf("Disk %u", index);
    char *capacity = megabytes(udisks_drive->size);
    grid_row(vgrid, &row, "Disk:", disk);
    grid_row(vgrid, &row, "Type:",
             udisks_drive->optical     ? "DVD"
             : udisks_drive->removable ? "Removable"
                                       : "Basic");
    grid_row(vgrid, &row, "Status:", udisks_drive->size > 0 ? "Online" : "No Media");
    grid_row(vgrid, &row, "Partition style:",
             strcmp(udisks_drive->table, "gpt") == 0   ? "GUID Partition Table (GPT)"
             : strcmp(udisks_drive->table, "dos") == 0 ? "Master Boot Record (MBR)"
                                                       : "Not Applicable");
    grid_row(vgrid, &row, "Capacity:", capacity);
    g_free(disk);
    g_free(capacity);
    gtk_box_pack_start(GTK_BOX(vbox), vgrid, FALSE, FALSE, 0);
    GtkWidget *vl = gtk_label_new("Volumes:");
    gtk_label_set_xalign(GTK_LABEL(vl), 0);
    gtk_box_pack_start(GTK_BOX(vbox), vl, FALSE, FALSE, 0);
    GtkListStore *vstore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GPtrArray *vols = xd_udisks_volumes(udisks_drive->object);
    for (guint i = 0; i < vols->len; i++) {
        const XdUdisksVolume *udisks_volume = g_ptr_array_index(vols, i);
        char *label = udisks_volume->label[0] != '\0'
                          ? g_strdup_printf("%s (%s)", udisks_volume->label, udisks_volume->device)
                          : g_strdup(udisks_volume->device);
        char *size = megabytes(udisks_volume->size);
        gtk_list_store_insert_with_values(vstore, nullptr, -1, 0, label, 1, size, -1);
        g_free(label);
        g_free(size);
    }
    g_ptr_array_unref(vols);
    GtkWidget *vlist = gtk_tree_view_new_with_model(GTK_TREE_MODEL(vstore));
    g_object_unref(vstore);
    GtkTreeViewColumn *vcol = gtk_tree_view_column_new_with_attributes(
        "Volume", gtk_cell_renderer_text_new(), "text", 0, nullptr);
    gtk_tree_view_column_set_expand(vcol, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(vlist), vcol);
    GtkCellRenderer *right = gtk_cell_renderer_text_new();
    g_object_set(right, "xalign", 1.0, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(vlist), gtk_tree_view_column_new_with_attributes(
                                                          "Capacity", right, "text", 1, nullptr));
    GtkWidget *vscroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(vscroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(vscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(vscroll, 320, 90);
    gtk_container_add(GTK_CONTAINER(vscroll), vlist);
    gtk_box_pack_start(GTK_BOX(vbox), vscroll, TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), vbox, gtk_label_new("Volumes"));

    /* Details */
    GtkWidget *details = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(details), 6);
    gtk_grid_set_column_spacing(GTK_GRID(details), 12);
    gtk_container_set_border_width(GTK_CONTAINER(details), 10);
    row = 0;
    GString *string = g_string_new(nullptr);
    append_grouped(string, udisks_drive->size);
    g_string_append(string, " bytes");
    char *bytes = g_string_free(string, FALSE);
    char *rpm = udisks_drive->rotation > 0    ? g_strdup_printf("%d rpm", udisks_drive->rotation)
                : udisks_drive->rotation == 0 ? g_strdup("Solid state (no rotation)")
                                              : g_strdup("Unknown");
    grid_row(details, &row, "Model:", udisks_drive->model);
    grid_row(details, &row, "Vendor:", udisks_drive->vendor);
    grid_row(details, &row, "Revision:", udisks_drive->revision);
    grid_row(details, &row, "Serial number:", udisks_drive->serial);
    grid_row(details, &row,
             "Connection:", udisks_drive->bus[0] != '\0' ? udisks_drive->bus : "Internal");
    grid_row(details, &row, "Size:", bytes);
    grid_row(details, &row, "Rotation rate:", rpm);
    grid_row(details, &row, "Removable media:", udisks_drive->removable ? "Yes" : "No");
    grid_row(details, &row, "Device:", udisks_drive->device);
    g_free(bytes);
    g_free(rpm);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), details, gtk_label_new("Details"));

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_hw_properties(GtkWidget *widget, gpointer data) {
    (void)widget;
    hardware *hardware_tab = data;
    const XdUdisksDrive *drive = hw_selected(hardware_tab);
    if (drive == nullptr)
        return;
    /* Disk Management's numbers: disks and CD-ROMs counted apart. */
    guint disks = 0, opticals = 0;
    for (guint i = 0; i < hardware_tab->drives->len; i++) {
        const XdUdisksDrive *candidate = g_ptr_array_index(hardware_tab->drives, i);
        if (candidate == drive)
            break;
        if (candidate->optical)
            opticals++;
        else
            disks++;
    }
    device_properties(GTK_WINDOW(hardware_tab->dialog), drive, disks, opticals);
}

static void on_hw_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col,
                            gpointer data) {
    (void)view;
    (void)path;
    (void)col;
    on_hw_properties(nullptr, data);
}

GtkWidget *xd_drive_hardware_page(GtkWidget *dialog, GFile *item) {
    hardware *hardware_tab = g_new0(hardware, 1);
    hardware_tab->dialog = dialog;
    hardware_tab->drives = xd_udisks_drives();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    g_object_set_data_full(G_OBJECT(box), "xd-hardware", hardware_tab, hardware_free);

    GtkWidget *title = gtk_label_new_with_mnemonic("All disk _drives:");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    GtkListStore *store = gtk_list_store_new(HW_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(item);
    GtkTreeIter mine = {0};
    gboolean have_mine = FALSE;
    for (guint i = 0; i < hardware_tab->drives->len; i++) {
        const XdUdisksDrive *udisks_drive = g_ptr_array_index(hardware_tab->drives, i);
        GtkTreeIter it;
        gtk_list_store_insert_with_values(store, &it, -1, HW_NAME, udisks_drive->name, HW_TYPE,
                                          udisks_drive->type, HW_INDEX, i, -1);
        if (!have_mine && fs != nullptr && strcmp(fs->drive, udisks_drive->object) == 0) {
            mine = it;
            have_mine = TRUE;
        }
    }
    hardware_tab->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_label_set_mnemonic_widget(GTK_LABEL(title), hardware_tab->list);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Name", gtk_cell_renderer_text_new(), "text", HW_NAME, nullptr);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(hardware_tab->list), col);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(hardware_tab->list),
        gtk_tree_view_column_new_with_attributes("Type", gtk_cell_renderer_text_new(), "text",
                                                 HW_TYPE, nullptr));
    g_signal_connect(hardware_tab->list, "row-activated", G_CALLBACK(on_hw_activated),
                     hardware_tab);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 320, 130);
    gtk_container_add(GTK_CONTAINER(scroll), hardware_tab->list);
    gtk_box_pack_start(GTK_BOX(box), scroll, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new("Device Properties");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    int row = 0;
    grid_row(grid, &row, "Manufacturer:", "");
    hardware_tab->maker = gtk_grid_get_child_at(GTK_GRID(grid), 1, row - 1);
    grid_row(grid, &row, "Location:", "");
    hardware_tab->where = gtk_grid_get_child_at(GTK_GRID(grid), 1, row - 1);
    grid_row(grid, &row, "Device status:", "");
    hardware_tab->status = gtk_grid_get_child_at(GTK_GRID(grid), 1, row - 1);
    GtkWidget *props = gtk_button_new_with_mnemonic("P_roperties");
    gtk_widget_set_halign(props, GTK_ALIGN_END);
    g_signal_connect(props, "clicked", G_CALLBACK(on_hw_properties), hardware_tab);
    gtk_grid_attach(GTK_GRID(grid), props, 0, row++, 2, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(hardware_tab->list));
    g_signal_connect(sel, "changed", G_CALLBACK(on_hw_selected), hardware_tab);
    GtkTreeIter first;
    if (have_mine)
        gtk_tree_selection_select_iter(sel, &mine);
    else if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first))
        gtk_tree_selection_select_iter(sel, &first);
    gtk_widget_set_sensitive(props, hardware_tab->drives->len > 0);
    g_object_unref(store);
    on_hw_selected(sel, hardware_tab);
    return box;
}
