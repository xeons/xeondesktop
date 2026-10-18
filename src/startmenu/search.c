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

static void result_free(gpointer p) {
    result *r = p;
    g_free(r->path);
    g_free(r->content_type);
    g_free(r);
}

static void job_free(job *j) {
    g_object_unref(j->cancel);
    g_free(j->root);
    xd_search_name_clear(&j->name);
    g_free(j->text);
    g_mutex_clear(&j->lock);
    g_ptr_array_unref(j->found);
    g_free(j->current);
    g_free(j);
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
    job *j = data;
    GQueue dirs = G_QUEUE_INIT;
    g_queue_push_tail(&dirs, g_file_new_for_path(j->root));
    bool text = j->text[0] != '\0';
    while (!g_queue_is_empty(&dirs) && !g_cancellable_is_cancelled(cancel)) {
        GFile *dir = g_queue_pop_head(&dirs);
        char *dir_path = g_file_get_path(dir);
        g_mutex_lock(&j->lock);
        g_free(j->current);
        j->current = g_strdup(dir_path);
        g_mutex_unlock(&j->lock);
        GFileEnumerator *e = g_file_enumerate_children(
            dir,
            G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
            "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_SIZE
            "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, nullptr);
        GFileInfo *info = nullptr;
        while (e != nullptr && !g_cancellable_is_cancelled(cancel) &&
               g_file_enumerator_iterate(e, &info, nullptr, cancel, nullptr) && info != nullptr) {
            if (g_file_info_get_is_hidden(info))
                continue;
            const char *name = g_file_info_get_name(info);
            GFileType type = g_file_info_get_file_type(info);
            GFile *child = g_file_get_child(dir, name);
            char *path = g_file_get_path(child);
            bool is_dir = type == G_FILE_TYPE_DIRECTORY;
            if (is_dir && path != nullptr && !skipped_root(path))
                g_queue_push_tail(&dirs, g_object_ref(child));
            bool match = path != nullptr && xd_search_name_matches(&j->name, name);
            if (match && text)
                match = type == G_FILE_TYPE_REGULAR &&
                        file_contains(child, g_file_info_get_size(info), j->text, cancel);
            if (match) {
                result *r = g_new0(result, 1);
                r->path = g_strdup(path);
                r->dir = is_dir;
                r->size = g_file_info_get_size(info);
                const char *ct = g_file_info_get_content_type(info);
                r->content_type = g_strdup(ct != nullptr ? ct : "application/octet-stream");
                GDateTime *t = g_file_info_get_modification_date_time(info);
                r->modified = t != nullptr ? g_date_time_to_unix(t) : 0;
                if (t != nullptr)
                    g_date_time_unref(t);
                g_mutex_lock(&j->lock);
                g_ptr_array_add(j->found, r);
                g_mutex_unlock(&j->lock);
            }
            g_free(path);
            g_object_unref(child);
        }
        g_clear_object(&e);
        g_free(dir_path);
        g_object_unref(dir);
    }
    g_queue_clear_full(&dirs, g_object_unref);
    g_task_return_boolean(task, TRUE);
}

/* ---- the window ---- */

static char *size_text(const result *r) {
    if (r->dir)
        return g_strdup("");
    /* Windows rounds up to whole KB. */
    goffset kb = (r->size + 1023) / 1024;
    return g_strdup_printf("%" G_GINT64_FORMAT " KB", (gint64)kb);
}

static void add_result(search *s, const result *r) {
    char *base = g_path_get_basename(r->path);
    char *name = g_filename_display_name(base);
    char *folder_raw = g_path_get_dirname(r->path);
    char *folder = g_filename_display_name(folder_raw);
    char *size = size_text(r);
    char *type = r->dir ? g_strdup("File Folder") : g_content_type_get_description(r->content_type);
    GIcon *icon = r->dir ? g_themed_icon_new("folder") : g_content_type_get_icon(r->content_type);
    GDateTime *t = g_date_time_new_from_unix_local(r->modified);
    char *when = t != nullptr ? g_date_time_format(t, "%x %H:%M") : g_strdup("");
    GtkTreeIter it;
    gtk_list_store_insert_with_values(s->store, &it, -1, COL_ICON, icon, COL_NAME, name,
                                      COL_FOLDER, folder, COL_SIZE, size, COL_TYPE, type,
                                      COL_MODIFIED, when, COL_PATH, r->path, COL_DIR, r->dir,
                                      COL_SIZE_SORT, (gint64)(r->dir ? -1 : r->size),
                                      COL_TIME_SORT, r->modified, -1);
    s->count++;
    g_free(when);
    if (t != nullptr)
        g_date_time_unref(t);
    g_object_unref(icon);
    g_free(type);
    g_free(size);
    g_free(folder);
    g_free(folder_raw);
    g_free(name);
    g_free(base);
}

static void show_count(search *s) {
    char *text = g_strdup_printf("%d file(s) found", s->count);
    gtk_label_set_text(GTK_LABEL(s->status), text);
    g_free(text);
}

/* Moves what the worker found into the list. */
static void drain(search *s, bool final) {
    job *j = s->running;
    if (j == nullptr)
        return;
    g_mutex_lock(&j->lock);
    GPtrArray *found = j->found;
    j->found = g_ptr_array_new_with_free_func(result_free);
    char *current = g_strdup(j->current);
    g_mutex_unlock(&j->lock);
    for (guint i = 0; i < found->len; i++)
        add_result(s, g_ptr_array_index(found, i));
    g_ptr_array_unref(found);
    if (final || current == nullptr) {
        show_count(s);
    } else {
        char *disp = g_filename_display_name(current);
        char *text = g_strdup_printf("%d file(s) found. Searching %s", s->count, disp);
        gtk_label_set_text(GTK_LABEL(s->status), text);
        g_free(text);
        g_free(disp);
    }
    g_free(current);
}

static gboolean on_drain(gpointer data) {
    drain(data, false);
    return G_SOURCE_CONTINUE;
}

static void set_searching(search *s, bool on) {
    gtk_widget_set_sensitive(s->start, !on);
    gtk_widget_set_sensitive(s->stop, on);
    gtk_widget_set_sensitive(s->named, !on);
    gtk_widget_set_sensitive(s->text, !on);
    gtk_widget_set_sensitive(s->look_in, !on);
}

static void search_done([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    job *j = data;
    g_task_propagate_boolean(G_TASK(res), nullptr);
    search *s = j->owner;
    if (s != nullptr) {
        drain(s, true);
        s->running = nullptr;
        if (s->drain != 0) {
            g_source_remove(s->drain);
            s->drain = 0;
        }
        set_searching(s, false);
    }
    job_free(j);
}

static void on_search_now([[maybe_unused]] GtkButton *b, gpointer data) {
    search *s = data;
    if (s->running != nullptr)
        return;
    const char *root = gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->look_in));
    if (root == nullptr || !g_file_test(root, G_FILE_TEST_IS_DIR)) {
        xd_message_box(GTK_WINDOW(s->window), "Search Results", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The folder in Look in does not exist.");
        return;
    }
    gtk_list_store_clear(s->store);
    s->count = 0;
    job *j = g_new0(job, 1);
    j->owner = s;
    j->cancel = g_cancellable_new();
    j->root = g_strdup(root);
    xd_search_name_init(&j->name, gtk_entry_get_text(GTK_ENTRY(s->named)));
    j->text = g_strdup(gtk_entry_get_text(GTK_ENTRY(s->text)));
    g_mutex_init(&j->lock);
    j->found = g_ptr_array_new_with_free_func(result_free);
    s->running = j;
    set_searching(s, true);
    gtk_label_set_text(GTK_LABEL(s->status), "Searching...");
    s->drain = g_timeout_add(200, on_drain, s);
    GTask *task = g_task_new(nullptr, j->cancel, search_done, j);
    g_task_set_task_data(task, j, nullptr);
    g_task_run_in_thread(task, search_thread);
    g_object_unref(task);
}

static void on_stop([[maybe_unused]] GtkButton *b, gpointer data) {
    search *s = data;
    if (s->running != nullptr)
        g_cancellable_cancel(s->running->cancel);
}

/* Look in: a few places, then "Browse..." to pick any folder. */
static void add_place(search *s, const char *path, const char *label) {
    if (path == nullptr || !g_file_test(path, G_FILE_TEST_IS_DIR))
        return;
    GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(s->look_in));
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
         ok = gtk_tree_model_iter_next(m, &it)) {
        char *id = nullptr;
        gtk_tree_model_get(m, &it, 1, &id, -1);
        bool same = g_strcmp0(id, path) == 0;
        g_free(id);
        if (same)
            return;
    }
    /* Before "Browse...", which stays last. */
    int n = gtk_tree_model_iter_n_children(m, nullptr);
    gtk_combo_box_text_insert(GTK_COMBO_BOX_TEXT(s->look_in), XD_MAX(n - 1, 0), path, label);
}

static void on_look_in_changed(GtkComboBox *box, gpointer data) {
    search *s = data;
    if (g_strcmp0(gtk_combo_box_get_active_id(box), "browse") != 0)
        return;
    GtkWidget *d = gtk_file_chooser_dialog_new("Browse For Folder", GTK_WINDOW(s->window),
                                               GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel",
                                               GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_ACCEPT,
                                               nullptr);
    char *chosen = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
        chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
    gtk_widget_destroy(d);
    if (chosen != nullptr) {
        char *label = g_filename_display_name(chosen);
        add_place(s, chosen, label);
        g_free(label);
        gtk_combo_box_set_active_id(box, chosen);
        g_free(chosen);
    } else {
        gtk_combo_box_set_active(box, 0);
    }
}

static void open_result(search *s, GtkTreePath *tp) {
    GtkTreeModel *m = GTK_TREE_MODEL(s->store);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(m, &it, tp))
        return;
    char *path = nullptr;
    gboolean dir = FALSE;
    gtk_tree_model_get(m, &it, COL_PATH, &path, COL_DIR, &dir, -1);
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
                gdk_display_get_app_launch_context(gtk_widget_get_display(s->window));
            g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &error);
            g_object_unref(ctx);
            g_free(uri);
        }
    }
    if (error != nullptr) {
        xd_message_box(GTK_WINDOW(s->window), "Search Results", XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
    }
    g_free(path);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *v, GtkTreePath *tp,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    open_result(data, tp);
}

static GtkWidget *caption(const char *mnemonic, GtkWidget *target) {
    GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
    return l;
}

static GtkWidget *search_pane(search *s, const char *start_dir) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_widget_set_size_request(box, 220, -1);

    GtkWidget *title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Search for Files and Folders</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 4);

    s->named = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(s->named), TRUE);
    gtk_box_pack_start(GTK_BOX(box), caption("Search for files or folders _named:", s->named),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->named, FALSE, FALSE, 0);

    s->text = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(s->text), TRUE);
    gtk_box_pack_start(GTK_BOX(box), caption("Containing _text:", s->text), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->text, FALSE, FALSE, 0);

    s->look_in = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->look_in), "browse", "Browse...");
    const char *home = g_get_home_dir();
    add_place(s, g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS), "My Documents");
    add_place(s, g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP), "Desktop");
    add_place(s, home, "Home Folder");
    add_place(s, "/", "Local Hard Drives");
    if (start_dir != nullptr) {
        /* The folder's name, as XP's Look in shows it ("/" has none). */
        char *label = strcmp(start_dir, "/") == 0 ? g_strdup("Local Hard Drives")
                                                  : g_filename_display_basename(start_dir);
        add_place(s, start_dir, label);
        g_free(label);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->look_in), start_dir != nullptr ? start_dir : home);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(s->look_in)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(s->look_in), 0);
    g_signal_connect(s->look_in, "changed", G_CALLBACK(on_look_in_changed), s);
    gtk_box_pack_start(GTK_BOX(box), caption("_Look in:", s->look_in), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->look_in, FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    s->start = gtk_button_new_with_mnemonic("Search _Now");
    s->stop = gtk_button_new_with_mnemonic("_Stop Search");
    gtk_widget_set_can_default(s->start, TRUE);
    gtk_widget_set_sensitive(s->stop, FALSE);
    g_signal_connect(s->start, "clicked", G_CALLBACK(on_search_now), s);
    g_signal_connect(s->stop, "clicked", G_CALLBACK(on_stop), s);
    gtk_box_pack_end(GTK_BOX(buttons), s->stop, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), s->start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 8);
    return box;
}

static void add_column(GtkTreeView *v, const char *title, int col, int sort, int width) {
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, title);
    if (col == COL_NAME) {
        GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(c, icon, FALSE);
        gtk_tree_view_column_add_attribute(c, icon, "gicon", COL_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    if (col == COL_SIZE)
        g_object_set(text, "xalign", 1.0, nullptr);
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(c, text, TRUE);
    gtk_tree_view_column_add_attribute(c, text, "text", col);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_column_set_sort_column_id(c, sort);
    gtk_tree_view_append_column(v, c);
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    search *s = data;
    if (s->drain != 0)
        g_source_remove(s->drain);
    s->drain = 0;
    if (s->running != nullptr) {
        /* The worker finishes on its own; its job outlives the window. */
        s->running->owner = nullptr;
        g_cancellable_cancel(s->running->cancel);
        s->running = nullptr;
    }
    gtk_main_quit();
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Search");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static search s;
    const char *start_dir = argc > 1 && g_file_test(argv[1], G_FILE_TEST_IS_DIR) ? argv[1] : nullptr;
    char *start_abs = start_dir != nullptr ? g_canonicalize_filename(start_dir, nullptr) : nullptr;

    s.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(s.window), "Search Results");
    gtk_window_set_icon_name(GTK_WINDOW(s.window), "system-search");
    gtk_window_set_default_size(GTK_WINDOW(s.window), 800, 460);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), search_pane(&s, start_abs), FALSE, FALSE);

    s.store = gtk_list_store_new(COL_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_BOOLEAN, G_TYPE_INT64, G_TYPE_INT64);
    s.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s.store));
    g_object_unref(s.store);
    GtkTreeView *v = GTK_TREE_VIEW(s.view);
    add_column(v, "Name", COL_NAME, COL_NAME, 150);
    add_column(v, "In Folder", COL_FOLDER, COL_FOLDER, 150);
    add_column(v, "Size", COL_SIZE, COL_SIZE_SORT, 60);
    add_column(v, "Type", COL_TYPE, COL_TYPE, 90);
    add_column(v, "Modified", COL_MODIFIED, COL_TIME_SORT, 110);
    g_signal_connect(s.view, "row-activated", G_CALLBACK(on_row_activated), &s);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), s.view);
    gtk_paned_pack2(GTK_PANED(paned), scroll, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(outer), paned, TRUE, TRUE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    s.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s.status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(s.status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(bar), s.status, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), bar, FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(s.window), outer);

    gtk_window_set_default(GTK_WINDOW(s.window), s.start);
    g_signal_connect(s.window, "destroy", G_CALLBACK(on_destroy), &s);
    gtk_widget_show_all(s.window);
    gtk_widget_grab_focus(s.named);
    gtk_main();
    g_free(start_abs);
    return EXIT_SUCCESS;
}
