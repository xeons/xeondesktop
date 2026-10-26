/* SPDX-License-Identifier: MIT
 * xd-search: Windows 2000's Search for Files or Folders, from the Start
 * menus' Search items. A small window of its own rather than a mode of
 * xd-files (which has none yet):
 *
 *   left   "Search for Files and Folders": named, containing text, look in,
 *          Search Now / Stop Search
 *   right  the results: Name, In Folder, Size, Type, Modified;
 *          double-click opens one (folders in xd-files)
 *
 * The folders are walked on a worker thread (GTask) without following
 * links; hidden files and folders are skipped, as Windows skips them
 * unless asked. Not here: Windows' Search Options (date, type, size,
 * advanced) and searching for computers or people.
 *
 *   xd-search [FOLDER]   start with FOLDER in Look in
 */
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "searchlib.h"

/* Files bigger than this aren't read for "Containing text". */
static constexpr goffset MAX_TEXT_FILE = 32 * 1024 * 1024;

enum { COL_ICON, COL_NAME, COL_FOLDER, COL_SIZE, COL_TYPE, COL_MODIFIED, COL_PATH, COL_DIR,
       COL_SIZE_SORT, COL_TIME_SORT, COL_COUNT };

typedef struct result {
    char *path;
    bool dir;
    goffset size;
    char *content_type;
    gint64 modified;
} result;

typedef struct search search;

/* One search, shared with its worker. */
typedef struct job {
    search *owner; /* nullptr once the window is gone */
    GCancellable *cancel;
    char *root;
    xd_search_name name;
    char *text;
    GMutex lock;
    GPtrArray *found;  /* result*, waiting for the window */
    char *current;     /* folder being searched */
} job;

struct search {
    GtkWidget *window, *named, *text, *look_in, *start, *stop, *view, *status;
    GtkListStore *store;
    job *running;
    guint drain;
    int count;
};

static void result_free(gpointer user_data) {
    result *found_file = user_data;
    g_free(found_file->path);
    g_free(found_file->content_type);
    g_free(found_file);
}

static void job_free(job *search_job) {
    g_object_unref(search_job->cancel);
    g_free(search_job->root);
    xd_search_name_clear(&search_job->name);
    g_free(search_job->text);
    g_mutex_clear(&search_job->lock);
    g_ptr_array_unref(search_job->found);
    g_free(search_job->current);
    g_free(search_job);
}

/* ---- the worker ---- */

static bool skipped_root(const char *path) {
    /* Not files: walking them is slow and finds nothing a user saved. */
    static const char *const virt[] = {"/proc", "/sys", "/dev", "/run"};
    for (size_t i = 0; i < G_N_ELEMENTS(virt); i++) {
        if (strcmp(path, virt[i]) == 0)
            return true;
    }
    return false;
}

static bool file_contains(GFile *file, goffset size, const char *text, GCancellable *cancel) {
    if (size > MAX_TEXT_FILE)
        return false;
    char *data = nullptr;
    gsize len = 0;
    if (!g_file_load_contents(file, cancel, &data, &len, nullptr, nullptr))
        return false;
    bool found = xd_search_text_in(data, len, text);
    g_free(data);
    return found;
}

static void search_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                          GCancellable *cancel) {
    job *search_job = data;
    GQueue dirs = G_QUEUE_INIT;
    g_queue_push_tail(&dirs, g_file_new_for_path(search_job->root));
    bool text = search_job->text[0] != '\0';
    while (!g_queue_is_empty(&dirs) && !g_cancellable_is_cancelled(cancel)) {
        GFile *dir = g_queue_pop_head(&dirs);
        char *dir_path = g_file_get_path(dir);
        g_mutex_lock(&search_job->lock);
        g_free(search_job->current);
        search_job->current = g_strdup(dir_path);
        g_mutex_unlock(&search_job->lock);
        GFileEnumerator *file_enumerator = g_file_enumerate_children(
            dir,
            G_FILE_ATTRIBUTE_STANDARD_NAME
            "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN
            "," G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
            "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, nullptr);
        GFileInfo *info = nullptr;
        while (file_enumerator != nullptr && !g_cancellable_is_cancelled(cancel) &&
               g_file_enumerator_iterate(file_enumerator, &info, nullptr, cancel, nullptr) &&
               info != nullptr) {
            if (g_file_info_get_is_hidden(info))
                continue;
            const char *name = g_file_info_get_name(info);
            GFileType type = g_file_info_get_file_type(info);
            GFile *child = g_file_get_child(dir, name);
            char *path = g_file_get_path(child);
            bool is_dir = type == G_FILE_TYPE_DIRECTORY;
            if (is_dir && path != nullptr && !skipped_root(path))
                g_queue_push_tail(&dirs, g_object_ref(child));
            bool match = path != nullptr && xd_search_name_matches(&search_job->name, name);
            if (match && text)
                match = type == G_FILE_TYPE_REGULAR &&
                        file_contains(child, g_file_info_get_size(info), search_job->text, cancel);
            if (match) {
                result *found_file = g_new0(result, 1);
                found_file->path = g_strdup(path);
                found_file->dir = is_dir;
                found_file->size = g_file_info_get_size(info);
                const char *ct = g_file_info_get_content_type(info);
                found_file->content_type =
                    g_strdup(ct != nullptr ? ct : "application/octet-stream");
                GDateTime *date_time = g_file_info_get_modification_date_time(info);
                found_file->modified = date_time != nullptr ? g_date_time_to_unix(date_time) : 0;
                if (date_time != nullptr)
                    g_date_time_unref(date_time);
                g_mutex_lock(&search_job->lock);
                g_ptr_array_add(search_job->found, found_file);
                g_mutex_unlock(&search_job->lock);
            }
            g_free(path);
            g_object_unref(child);
        }
        g_clear_object(&file_enumerator);
        g_free(dir_path);
        g_object_unref(dir);
    }
    g_queue_clear_full(&dirs, g_object_unref);
    g_task_return_boolean(task, TRUE);
}

/* ---- the window ---- */

static char *size_text(const result *found_file) {
    if (found_file->dir)
        return g_strdup("");
    /* Windows rounds up to whole KB. */
    goffset kb = (found_file->size + 1023) / 1024;
    return g_strdup_printf("%" G_GINT64_FORMAT " KB", (gint64)kb);
}

static void add_result(search *search_window, const result *found_file) {
    char *base = g_path_get_basename(found_file->path);
    char *name = g_filename_display_name(base);
    char *folder_raw = g_path_get_dirname(found_file->path);
    char *folder = g_filename_display_name(folder_raw);
    char *size = size_text(found_file);
    char *type = found_file->dir ? g_strdup("File Folder")
                                 : g_content_type_get_description(found_file->content_type);
    GIcon *icon = found_file->dir ? g_themed_icon_new("folder")
                                  : g_content_type_get_icon(found_file->content_type);
    GDateTime *date_time = g_date_time_new_from_unix_local(found_file->modified);
    char *when = date_time != nullptr ? g_date_time_format(date_time, "%x %H:%M") : g_strdup("");
    GtkTreeIter it;
    gtk_list_store_insert_with_values(
        search_window->store, &it, -1, COL_ICON, icon, COL_NAME, name, COL_FOLDER, folder, COL_SIZE,
        size, COL_TYPE, type, COL_MODIFIED, when, COL_PATH, found_file->path, COL_DIR,
        found_file->dir, COL_SIZE_SORT, (gint64)(found_file->dir ? -1 : found_file->size),
        COL_TIME_SORT, found_file->modified, -1);
    search_window->count++;
    g_free(when);
    if (date_time != nullptr)
        g_date_time_unref(date_time);
    g_object_unref(icon);
    g_free(type);
    g_free(size);
    g_free(folder);
    g_free(folder_raw);
    g_free(name);
    g_free(base);
}

static void show_count(search *search_window) {
    char *text = g_strdup_printf("%d file(s) found", search_window->count);
    gtk_label_set_text(GTK_LABEL(search_window->status), text);
    g_free(text);
}

/* Moves what the worker found into the list. */
static void drain(search *search_window, bool final) {
    job *search_job = search_window->running;
    if (search_job == nullptr)
        return;
    g_mutex_lock(&search_job->lock);
    GPtrArray *found = search_job->found;
    search_job->found = g_ptr_array_new_with_free_func(result_free);
    char *current = g_strdup(search_job->current);
    g_mutex_unlock(&search_job->lock);
    for (guint i = 0; i < found->len; i++)
        add_result(search_window, g_ptr_array_index(found, i));
    g_ptr_array_unref(found);
    if (final || current == nullptr) {
        show_count(search_window);
    } else {
        char *disp = g_filename_display_name(current);
        char *text = g_strdup_printf("%d file(s) found. Searching %s", search_window->count, disp);
        gtk_label_set_text(GTK_LABEL(search_window->status), text);
        g_free(text);
        g_free(disp);
    }
    g_free(current);
}

static gboolean on_drain(gpointer data) {
    drain(data, false);
    return G_SOURCE_CONTINUE;
}

static void set_searching(search *search_window, bool on) {
    gtk_widget_set_sensitive(search_window->start, !on);
    gtk_widget_set_sensitive(search_window->stop, on);
    gtk_widget_set_sensitive(search_window->named, !on);
    gtk_widget_set_sensitive(search_window->text, !on);
    gtk_widget_set_sensitive(search_window->look_in, !on);
}

static void search_done([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    job *search_job = data;
    g_task_propagate_boolean(G_TASK(res), nullptr);
    search *search_window = search_job->owner;
    if (search_window != nullptr) {
        drain(search_window, true);
        search_window->running = nullptr;
        if (search_window->drain != 0) {
            g_source_remove(search_window->drain);
            search_window->drain = 0;
        }
        set_searching(search_window, false);
    }
    job_free(search_job);
}

static void on_search_now([[maybe_unused]] GtkButton *button, gpointer data) {
    search *search_window = data;
    if (search_window->running != nullptr)
        return;
    const char *root = gtk_combo_box_get_active_id(GTK_COMBO_BOX(search_window->look_in));
    if (root == nullptr || !g_file_test(root, G_FILE_TEST_IS_DIR)) {
        xd_message_box(GTK_WINDOW(search_window->window), "Search Results", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "The folder in Look in does not exist.");
        return;
    }
    gtk_list_store_clear(search_window->store);
    search_window->count = 0;
    job *search_job = g_new0(job, 1);
    search_job->owner = search_window;
    search_job->cancel = g_cancellable_new();
    search_job->root = g_strdup(root);
    xd_search_name_init(&search_job->name, gtk_entry_get_text(GTK_ENTRY(search_window->named)));
    search_job->text = g_strdup(gtk_entry_get_text(GTK_ENTRY(search_window->text)));
    g_mutex_init(&search_job->lock);
    search_job->found = g_ptr_array_new_with_free_func(result_free);
    search_window->running = search_job;
    set_searching(search_window, true);
    gtk_label_set_text(GTK_LABEL(search_window->status), "Searching...");
    search_window->drain = g_timeout_add(200, on_drain, search_window);
    GTask *task = g_task_new(nullptr, search_job->cancel, search_done, search_job);
    g_task_set_task_data(task, search_job, nullptr);
    g_task_run_in_thread(task, search_thread);
    g_object_unref(task);
}

static void on_stop([[maybe_unused]] GtkButton *button, gpointer data) {
    search *search_window = data;
    if (search_window->running != nullptr)
        g_cancellable_cancel(search_window->running->cancel);
}

/* Look in: a few places, then "Browse..." to pick any folder. */
static void add_place(search *search_window, const char *path, const char *label) {
    if (path == nullptr || !g_file_test(path, G_FILE_TEST_IS_DIR))
        return;
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(search_window->look_in));
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        char *id = nullptr;
        gtk_tree_model_get(model, &it, 1, &id, -1);
        bool same = g_strcmp0(id, path) == 0;
        g_free(id);
        if (same)
            return;
    }
    /* Before "Browse...", which stays last. */
    int count = gtk_tree_model_iter_n_children(model, nullptr);
    gtk_combo_box_text_insert(GTK_COMBO_BOX_TEXT(search_window->look_in), XD_MAX(count - 1, 0),
                              path, label);
}

static void on_look_in_changed(GtkComboBox *box, gpointer data) {
    search *search_window = data;
    if (g_strcmp0(gtk_combo_box_get_active_id(box), "browse") != 0)
        return;
    GtkFileChooserNative *chooser =
        gtk_file_chooser_native_new("Browse For Folder", GTK_WINDOW(search_window->window),
                                    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "OK", "Cancel");
    char *chosen = nullptr;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    g_object_unref(chooser);
    if (chosen != nullptr) {
        char *label = g_filename_display_name(chosen);
        add_place(search_window, chosen, label);
        g_free(label);
        gtk_combo_box_set_active_id(box, chosen);
        g_free(chosen);
    } else {
        gtk_combo_box_set_active(box, 0);
    }
}

static void open_result(search *search_window, GtkTreePath *tp) {
    GtkTreeModel *model = GTK_TREE_MODEL(search_window->store);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(model, &it, tp))
        return;
    char *path = nullptr;
    gboolean dir = FALSE;
    gtk_tree_model_get(model, &it, COL_PATH, &path, COL_DIR, &dir, -1);
    GError *error = nullptr;
    if (dir) {
        char *self = g_file_read_link("/proc/self/exe", nullptr);
        char *bindir = self != nullptr ? g_path_get_dirname(self) : nullptr;
        char *local = bindir != nullptr ? g_build_filename(bindir, "xd-files", nullptr) : nullptr;
        if (local == nullptr || !g_file_test(local, G_FILE_TEST_IS_EXECUTABLE)) {
            g_free(local);
            local = g_strdup("xd-files");
        }
        char *argv[] = {local, path, nullptr};
        g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                      &error);
        g_free(local);
        g_free(bindir);
        g_free(self);
    } else {
        char *uri = g_filename_to_uri(path, nullptr, &error);
        if (uri != nullptr) {
            GdkAppLaunchContext *ctx =
                gdk_display_get_app_launch_context(gtk_widget_get_display(search_window->window));
            g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &error);
            g_object_unref(ctx);
            g_free(uri);
        }
    }
    if (error != nullptr) {
        xd_message_box(GTK_WINDOW(search_window->window), "Search Results", XD_MB_ERROR, XD_MB_OK,
                       FALSE, error->message);
        g_error_free(error);
    }
    g_free(path);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tree_view, GtkTreePath *tp,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    open_result(data, tp);
}

static GtkWidget *caption(const char *mnemonic, GtkWidget *target) {
    GtkWidget *widget = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), target);
    return widget;
}

static GtkWidget *search_pane(search *search_window, const char *start_dir) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_widget_set_size_request(box, 220, -1);

    GtkWidget *title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Search for Files and Folders</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 4);

    search_window->named = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(search_window->named), TRUE);
    gtk_box_pack_start(GTK_BOX(box),
                       caption("Search for files or folders _named:", search_window->named), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), search_window->named, FALSE, FALSE, 0);

    search_window->text = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(search_window->text), TRUE);
    gtk_box_pack_start(GTK_BOX(box), caption("Containing _text:", search_window->text), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), search_window->text, FALSE, FALSE, 0);

    search_window->look_in = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(search_window->look_in), "browse", "Browse...");
    const char *home = g_get_home_dir();
    add_place(search_window, g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS), "My Documents");
    add_place(search_window, g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP), "Desktop");
    add_place(search_window, home, "Home Folder");
    add_place(search_window, "/", "Local Hard Drives");
    if (start_dir != nullptr) {
        /* The folder's name, as XP's Look in shows it ("/" has none). */
        char *label = strcmp(start_dir, "/") == 0 ? g_strdup("Local Hard Drives")
                                                  : g_filename_display_basename(start_dir);
        add_place(search_window, start_dir, label);
        g_free(label);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(search_window->look_in),
                                start_dir != nullptr ? start_dir : home);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(search_window->look_in)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(search_window->look_in), 0);
    g_signal_connect(search_window->look_in, "changed", G_CALLBACK(on_look_in_changed),
                     search_window);
    gtk_box_pack_start(GTK_BOX(box), caption("_Look in:", search_window->look_in), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), search_window->look_in, FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    search_window->start = gtk_button_new_with_mnemonic("Search _Now");
    search_window->stop = gtk_button_new_with_mnemonic("_Stop Search");
    gtk_widget_set_can_default(search_window->start, TRUE);
    gtk_widget_set_sensitive(search_window->stop, FALSE);
    g_signal_connect(search_window->start, "clicked", G_CALLBACK(on_search_now), search_window);
    g_signal_connect(search_window->stop, "clicked", G_CALLBACK(on_stop), search_window);
    gtk_box_pack_end(GTK_BOX(buttons), search_window->stop, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), search_window->start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 8);
    return box;
}

static void add_column(GtkTreeView *tree_view, const char *title, int col, int sort, int width) {
    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(column, title);
    if (col == COL_NAME) {
        GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(column, icon, FALSE);
        gtk_tree_view_column_add_attribute(column, icon, "gicon", COL_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    if (col == COL_SIZE)
        g_object_set(text, "xalign", 1.0, nullptr);
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(column, text, TRUE);
    gtk_tree_view_column_add_attribute(column, text, "text", col);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, width);
    gtk_tree_view_column_set_sort_column_id(column, sort);
    gtk_tree_view_append_column(tree_view, column);
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    search *search_window = data;
    if (search_window->drain != 0)
        g_source_remove(search_window->drain);
    search_window->drain = 0;
    if (search_window->running != nullptr) {
        /* The worker finishes on its own; its job outlives the window. */
        search_window->running->owner = nullptr;
        g_cancellable_cancel(search_window->running->cancel);
        search_window->running = nullptr;
    }
    gtk_main_quit();
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Search");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static search search_window;
    const char *start_dir = argc > 1 && g_file_test(argv[1], G_FILE_TEST_IS_DIR) ? argv[1] : nullptr;
    char *start_abs = start_dir != nullptr ? g_canonicalize_filename(start_dir, nullptr) : nullptr;

    search_window.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(search_window.window), "Search Results");
    gtk_window_set_icon_name(GTK_WINDOW(search_window.window), "system-search");
    gtk_window_set_default_size(GTK_WINDOW(search_window.window), 800, 460);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), search_pane(&search_window, start_abs), FALSE, FALSE);

    search_window.store = gtk_list_store_new(
        COL_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_INT64, G_TYPE_INT64);
    search_window.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(search_window.store));
    g_object_unref(search_window.store);
    GtkTreeView *tree_view = GTK_TREE_VIEW(search_window.view);
    add_column(tree_view, "Name", COL_NAME, COL_NAME, 150);
    add_column(tree_view, "In Folder", COL_FOLDER, COL_FOLDER, 150);
    add_column(tree_view, "Size", COL_SIZE, COL_SIZE_SORT, 60);
    add_column(tree_view, "Type", COL_TYPE, COL_TYPE, 90);
    add_column(tree_view, "Modified", COL_MODIFIED, COL_TIME_SORT, 110);
    g_signal_connect(search_window.view, "row-activated", G_CALLBACK(on_row_activated),
                     &search_window);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), search_window.view);
    gtk_paned_pack2(GTK_PANED(paned), scroll, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(outer), paned, TRUE, TRUE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    search_window.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(search_window.status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(search_window.status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(bar), search_window.status, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), bar, FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(search_window.window), outer);

    gtk_window_set_default(GTK_WINDOW(search_window.window), search_window.start);
    g_signal_connect(search_window.window, "destroy", G_CALLBACK(on_destroy), &search_window);
    gtk_widget_show_all(search_window.window);
    gtk_widget_grab_focus(search_window.named);
    gtk_main();
    g_free(start_abs);
    return EXIT_SUCCESS;
}
