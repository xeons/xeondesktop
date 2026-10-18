/* SPDX-License-Identifier: MIT
 * Renaming drives, Disk Cleanup, and a drive's Tools and Hardware tabs.
 * See drives.h.
 */
#include "drives.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <linux/magic.h>

#include "../gtkapp/gtkapp.h"
#include "cleanup.h"
#include "places.h"
#include "udisks.h"

#define UD_ERROR "org.freedesktop.UDisks2.Error."

static constexpr GtkDialogFlags MODAL_DIALOG =
    (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);

static GtkWidget *wrapped_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 48);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
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
    label_job *j = data;
    GError *err = nullptr;
    GVariant *r = xd_udisks_call_finish(res, nullptr, &err);
    g_clear_pointer(&r, g_variant_unref);
    if (j->parent != nullptr) {
        g_object_remove_weak_pointer(G_OBJECT(j->parent), (gpointer *)&j->parent);
        if (err != nullptr) {
            char *msg = g_strdup_printf("Cannot rename %s: %s", j->name, err->message);
            xd_message_box(GTK_WINDOW(j->parent), "Error Renaming Drive", XD_MB_ERROR, XD_MB_OK,
                           FALSE, msg);
            g_free(msg);
        }
        if (j->done != nullptr)
            j->done(err == nullptr, j->data);
    }
    g_clear_error(&err);
    g_free(j->name);
    g_free(j);
}

void xd_drive_set_label(GtkWindow *parent, GFile *item, const char *name, const char *label,
                        void (*done)(gboolean ok, gpointer data), gpointer data) {
    label_job *j = g_new0(label_job, 1);
    j->parent = GTK_WIDGET(parent);
    g_object_add_weak_pointer(G_OBJECT(j->parent), (gpointer *)&j->parent);
    j->name = g_strdup(name);
    j->done = done;
    j->data = data;
    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(item);
    /* No such drive any more: let the call say so. */
    xd_udisks_call(fs != nullptr ? fs->object : "/org/freedesktop/UDisks2/missing",
                   XD_UDISKS_FILESYSTEM, "SetLabel",
                   g_variant_new("(sa{sv})", label, nullptr), nullptr, label_set, j);
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
        const XdDrive *d = g_ptr_array_index(drives, i);
        char *path = strcmp(d->kind, "File Folder") != 0 ? g_file_get_path(d->root) : nullptr;
        if (path != nullptr)
            g_ptr_array_add(roots, path);
    }
    g_ptr_array_unref(drives);
    g_ptr_array_add(roots, nullptr);

    GPtrArray *temps = g_ptr_array_new_with_free_func(g_free);
    const char *candidates[] = {g_get_tmp_dir(), "/tmp", "/var/tmp"};
    for (size_t i = 0; i < G_N_ELEMENTS(candidates); i++) {
        char *real = g_canonicalize_filename(candidates[i], nullptr);
        if (!in_memory(real) &&
            !g_ptr_array_find_with_equal_func(temps, real, g_str_equal, nullptr))
            g_ptr_array_add(temps, real);
        else
            g_free(real);
    }
    g_ptr_array_add(temps, nullptr);

    XdCleanupPlaces *p = xd_cleanup_places_new(
        root, (const char *const *)roots->pdata, g_get_user_data_dir(), g_get_user_cache_dir(),
        (const char *const *)temps->pdata, getuid());
    g_ptr_array_unref(temps);
    g_ptr_array_unref(roots);
    g_free(root);
    return p;
}

enum { COL_CHECK, COL_ICON, COL_NAME, COL_SIZE, COL_INDEX, N_COLS };

typedef struct cleanup {
    GtkWidget *parent; /* weak */
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
    GtkWidget *dlg, *list, *total, *desc, *view;
    GtkListStore *store;
} cleanup;

static void cleanup_free(cleanup *c) {
    if (c->tick != 0)
        g_source_remove(c->tick);
    if (c->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(c->parent), (gpointer *)&c->parent);
    g_free(c->name);
    xd_cleanup_places_free(c->places);
    g_clear_object(&c->cancel);
    g_clear_pointer(&c->cats, g_ptr_array_unref);
    g_clear_object(&c->store);
    if (c->errors != nullptr)
        g_string_free(c->errors, TRUE);
    g_free(c);
}

static const char *const category_icons[XD_CLEANUP_KIND_COUNT] = {
    [XD_CLEANUP_INTERNET] = "web-browser",
    [XD_CLEANUP_TRASH] = "user-trash-full",
    [XD_CLEANUP_TEMP] = "text-x-generic",
    [XD_CLEANUP_THUMBNAILS] = "image-x-generic",
};

static const char *kind_title(XdCleanupKind kind) {
    static const char *const titles[XD_CLEANUP_KIND_COUNT] = {
        [XD_CLEANUP_INTERNET] = "Temporary Internet Files",
        [XD_CLEANUP_TRASH] = "Recycle Bin",
        [XD_CLEANUP_TEMP] = "Temporary files",
        [XD_CLEANUP_THUMBNAILS] = "Thumbnails",
    };
    return (unsigned)kind < XD_CLEANUP_KIND_COUNT ? titles[kind] : "";
}

static gboolean on_progress_tick(gpointer data) {
    cleanup *c = data;
    if (c->progress == nullptr)
        return G_SOURCE_CONTINUE;
    gint kind = g_atomic_int_get(&c->scanning);
    char *text = g_strdup_printf("%s %s", c->cleaning ? "Cleaning:" : "Scanning:",
                                 kind_title((XdCleanupKind)kind));
    gtk_label_set_text(GTK_LABEL(c->doing), text);
    g_free(text);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(c->bar),
                                  (double)(kind + 1) / (double)XD_CLEANUP_KIND_COUNT);
    return G_SOURCE_CONTINUE;
}

static void on_progress_gone(cleanup *c) {
    c->progress = nullptr;
    g_cancellable_cancel(c->cancel);
}

static void on_progress_cancel(GtkDialog *dlg, gint response, gpointer data) {
    (void)dlg;
    (void)response;
    g_cancellable_cancel(((cleanup *)data)->cancel);
}

/* XP's small box while Disk Cleanup works: a line of text, the bar, and
 * what it's on. */
static void show_progress(cleanup *c, const char *text) {
    char *title = g_strdup_printf("Disk Cleanup for %s", c->name);
    c->progress = gtk_dialog_new_with_buttons(title, c->parent != nullptr ? GTK_WINDOW(c->parent)
                                                                          : nullptr,
                                              GTK_DIALOG_DESTROY_WITH_PARENT, "Cancel",
                                              GTK_RESPONSE_CANCEL, nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(c->progress), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(c->progress), "edit-clear");
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(c->progress));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name("edit-clear", GTK_ICON_SIZE_DND),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), wrapped_label(text), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    c->bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), c->bar, FALSE, FALSE, 0);
    c->doing = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(c->doing), 0);
    gtk_label_set_ellipsize(GTK_LABEL(c->doing), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), c->doing, FALSE, FALSE, 0);
    g_signal_connect(c->progress, "response", G_CALLBACK(on_progress_cancel), c);
    /* Closing it cancels, as Cancel; it goes when the work has stopped. */
    g_signal_connect(c->progress, "delete-event", G_CALLBACK(gtk_true), nullptr);
    /* (Or with the window it belongs to.) */
    g_signal_connect_swapped(c->progress, "destroy", G_CALLBACK(on_progress_gone), c);
    on_progress_tick(c);
    c->tick = g_timeout_add(100, on_progress_tick, c);
    gtk_widget_show_all(c->progress);
}

static void end_progress(cleanup *c) {
    if (c->tick != 0)
        g_source_remove(c->tick);
    c->tick = 0;
    if (c->progress != nullptr) {
        g_signal_handlers_disconnect_by_data(c->progress, c);
        gtk_widget_destroy(c->progress);
        c->progress = nullptr;
    }
}

static XdCleanupCategory *selected_category(cleanup *c) {
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->list));
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return nullptr;
    guint index = 0;
    gtk_tree_model_get(model, &it, COL_INDEX, &index, -1);
    return index < c->cats->len ? g_ptr_array_index(c->cats, index) : nullptr;
}

static void update_total(cleanup *c) {
    guint64 total = 0;
    for (guint i = 0; i < c->cats->len; i++) {
        const XdCleanupCategory *cat = g_ptr_array_index(c->cats, i);
        if (cat->checked)
            total += cat->size;
    }
    char *text = xd_cleanup_format_kb(total);
    gtk_label_set_text(GTK_LABEL(c->total), text);
    g_free(text);
}

static void on_category_selected(GtkTreeSelection *sel, gpointer data) {
    (void)sel;
    cleanup *c = data;
    const XdCleanupCategory *cat = selected_category(c);
    gtk_label_set_text(GTK_LABEL(c->desc), cat != nullptr ? cat->description : "");
    gtk_widget_set_sensitive(c->view, cat != nullptr && cat->view != nullptr);
}

static void on_category_toggled(GtkCellRendererToggle *cell, char *path, gpointer data) {
    (void)cell;
    cleanup *c = data;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(c->store), &it, path))
        return;
    guint index = 0;
    gboolean on = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, COL_INDEX, &index, COL_CHECK, &on, -1);
    gtk_list_store_set(c->store, &it, COL_CHECK, !on, -1);
    if (index < c->cats->len)
        ((XdCleanupCategory *)g_ptr_array_index(c->cats, index))->checked = !on;
    update_total(c);
}

/* Space toggles the selected category's box, as in XP's list; Enter is
 * the dialog's (OK), not the list's. */
static gboolean on_list_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    cleanup *c = data;
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        gtk_window_activate_default(GTK_WINDOW(c->dlg));
        return TRUE;
    }
    if (ev->keyval != GDK_KEY_space)
        return FALSE;
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->list));
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        char *path = gtk_tree_model_get_string_from_iter(model, &it);
        on_category_toggled(nullptr, path, c);
        g_free(path);
    }
    return TRUE;
}

static void on_view_files(GtkButton *b, gpointer data) {
    (void)b;
    cleanup *c = data;
    const XdCleanupCategory *cat = selected_category(c);
    if (cat != nullptr && cat->view != nullptr)
        spawn(GTK_WINDOW(c->dlg), "Disk Cleanup", "xd-files", cat->view);
}

static void on_more_programs(GtkButton *b, gpointer data) {
    (void)b;
    spawn(GTK_WINDOW(((cleanup *)data)->dlg), "Disk Cleanup", "xd-appwiz", nullptr);
}

static void clean_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    cleanup *c = data;
    GString *errors = g_string_new(nullptr);
    for (guint i = 0; i < c->cats->len && !g_cancellable_is_cancelled(cancel); i++) {
        const XdCleanupCategory *cat = g_ptr_array_index(c->cats, i);
        if (!cat->checked)
            continue;
        g_atomic_int_set(&c->scanning, (gint)cat->kind);
        GError *err = nullptr;
        if (!xd_cleanup_delete(cat, getuid(), g_get_real_time(), cancel, &err)) {
            g_string_append_printf(errors, "%s%s", errors->len > 0 ? "\n" : "", err->message);
            g_error_free(err);
        }
    }
    g_task_return_pointer(task, errors, nullptr);
}

static void cleaned(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    cleanup *c = data;
    GString *errors = g_task_propagate_pointer(G_TASK(res), nullptr);
    end_progress(c);
    if (errors != nullptr && errors->len > 0 && c->parent != nullptr) {
        char *title = g_strdup_printf("Disk Cleanup for %s", c->name);
        char *msg = g_strdup_printf("Disk Cleanup could not delete some of the files.\n\n%s",
                                    errors->str);
        xd_message_box(GTK_WINDOW(c->parent), title, XD_MB_WARNING, XD_MB_OK, FALSE, msg);
        g_free(msg);
        g_free(title);
    }
    if (errors != nullptr)
        g_string_free(errors, TRUE);
    cleanup_free(c);
}

static void on_cleanup_response(GtkDialog *dlg, gint response, gpointer data) {
    cleanup *c = data;
    gboolean any = FALSE;
    for (guint i = 0; i < c->cats->len; i++)
        any = any || ((XdCleanupCategory *)g_ptr_array_index(c->cats, i))->checked;
    if (response == GTK_RESPONSE_OK && any) {
        char *title = g_strdup_printf("Disk Cleanup for %s", c->name);
        int answer = xd_message_box(GTK_WINDOW(dlg), title, XD_MB_QUESTION, XD_MB_YES_NO, FALSE,
                                    "Are you sure you want to perform these actions?");
        g_free(title);
        if (answer != GTK_RESPONSE_YES)
            return;
    }
    c->dlg = nullptr;
    gtk_widget_destroy(GTK_WIDGET(dlg));
    if (response != GTK_RESPONSE_OK || !any) {
        cleanup_free(c);
        return;
    }
    c->cleaning = TRUE;
    g_atomic_int_set(&c->scanning, 0);
    show_progress(c, "Disk Cleanup is cleaning up unnecessary files on your machine.");
    GTask *task = g_task_new(nullptr, c->cancel, cleaned, c);
    g_task_set_task_data(task, c, nullptr);
    g_task_run_in_thread(task, clean_thread);
    g_object_unref(task);
}

static void on_cleanup_destroy(GtkWidget *w, gpointer data) {
    (void)w;
    cleanup *c = data;
    /* Closed with its parent: nothing to do any more. */
    if (c->dlg != nullptr) {
        c->dlg = nullptr;
        cleanup_free(c);
    }
}

static GtkWidget *cleanup_page(cleanup *c) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    guint64 all = 0;
    for (guint i = 0; i < c->cats->len; i++)
        all += ((XdCleanupCategory *)g_ptr_array_index(c->cats, i))->size;
    char *kb = xd_cleanup_format_kb(all);
    char *intro =
        g_strdup_printf("You can use Disk Cleanup to free up to %s of disk space on %s.", kb,
                        c->name);
    g_free(kb);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name("edit-clear", GTK_ICON_SIZE_DND),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), wrapped_label(intro), TRUE, TRUE, 0);
    g_free(intro);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *files = gtk_label_new_with_mnemonic("_Files to delete:");
    gtk_label_set_xalign(GTK_LABEL(files), 0);
    gtk_box_pack_start(GTK_BOX(box), files, FALSE, FALSE, 0);
    c->store = gtk_list_store_new(N_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_UINT);
    for (guint i = 0; i < c->cats->len; i++) {
        const XdCleanupCategory *cat = g_ptr_array_index(c->cats, i);
        char *size = xd_cleanup_format_kb(cat->size);
        gtk_list_store_insert_with_values(c->store, nullptr, -1, COL_CHECK, cat->checked,
                                          COL_ICON, category_icons[cat->kind], COL_NAME,
                                          cat->title, COL_SIZE, size, COL_INDEX, i, -1);
        g_free(size);
    }
    c->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c->store));
    gtk_label_set_mnemonic_widget(GTK_LABEL(files), c->list);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(c->list), FALSE);
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_category_toggled), c);
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
    gtk_tree_view_append_column(GTK_TREE_VIEW(c->list), col);
    GtkCellRenderer *size = gtk_cell_renderer_text_new();
    g_object_set(size, "xalign", 1.0, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(c->list),
                                gtk_tree_view_column_new_with_attributes("", size, "text",
                                                                         COL_SIZE, nullptr));
    g_signal_connect(c->list, "key-press-event", G_CALLBACK(on_list_key), c);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_container_add(GTK_CONTAINER(scroll), c->list);
    gtk_box_pack_start(GTK_BOX(box), scroll, FALSE, FALSE, 0);

    GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *tl = gtk_label_new("Total amount of disk space you gain:");
    gtk_label_set_xalign(GTK_LABEL(tl), 0);
    gtk_box_pack_start(GTK_BOX(trow), tl, TRUE, TRUE, 0);
    c->total = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(trow), c->total, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), trow, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new("Description");
    GtkWidget *fbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(fbox), 8);
    c->desc = wrapped_label("");
    gtk_label_set_lines(GTK_LABEL(c->desc), 5);
    gtk_widget_set_size_request(c->desc, -1, 80);
    gtk_label_set_yalign(GTK_LABEL(c->desc), 0);
    gtk_box_pack_start(GTK_BOX(fbox), c->desc, TRUE, TRUE, 0);
    c->view = gtk_button_new_with_mnemonic("_View Files");
    gtk_widget_set_halign(c->view, GTK_ALIGN_END);
    g_signal_connect(c->view, "clicked", G_CALLBACK(on_view_files), c);
    gtk_box_pack_start(GTK_BOX(fbox), c->view, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), fbox);
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->list));
    g_signal_connect(sel, "changed", G_CALLBACK(on_category_selected), c);
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(c->store), &first))
        gtk_tree_selection_select_iter(sel, &first);
    on_category_selected(sel, c);
    update_total(c);
    return box;
}

/* More Options: XP's Installed programs (Add or Remove Programs); there
 * are no Windows components or System Restore points to clean up. */
static GtkWidget *more_options_page(cleanup *c) {
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
    g_signal_connect(button, "clicked", G_CALLBACK(on_more_programs), c);
    gtk_grid_attach(GTK_GRID(grid), button, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return box;
}

static void show_cleanup(cleanup *c) {
    char *title = g_strdup_printf("Disk Cleanup for %s", c->name);
    c->dlg = gtk_dialog_new_with_buttons(title, c->parent != nullptr ? GTK_WINDOW(c->parent)
                                                                     : nullptr,
                                         GTK_DIALOG_DESTROY_WITH_PARENT, "OK", GTK_RESPONSE_OK,
                                         "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(c->dlg), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(c->dlg), "edit-clear");
    gtk_dialog_set_default_response(GTK_DIALOG(c->dlg), GTK_RESPONSE_OK);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), cleanup_page(c), gtk_label_new("Disk Cleanup"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), more_options_page(c),
                             gtk_label_new("More Options"));
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(c->dlg))), nb);
    xd_gtk_notebook_ctrl_tab(c->dlg, GTK_NOTEBOOK(nb));
    g_signal_connect(c->dlg, "response", G_CALLBACK(on_cleanup_response), c);
    g_signal_connect(c->dlg, "destroy", G_CALLBACK(on_cleanup_destroy), c);
    gtk_widget_set_size_request(c->dlg, 380, -1);
    gtk_widget_show_all(c->dlg);
    gtk_widget_grab_focus(c->list);
}

static void scan_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    cleanup *c = data;
    GPtrArray *cats = xd_cleanup_scan(c->places, getuid(), g_get_real_time(), cancel,
                                      &c->scanning);
    g_task_return_pointer(task, cats, (GDestroyNotify)g_ptr_array_unref);
}

static void scanned(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    cleanup *c = data;
    c->cats = g_task_propagate_pointer(G_TASK(res), nullptr);
    end_progress(c);
    if (c->cats == nullptr || g_cancellable_is_cancelled(c->cancel) || c->parent == nullptr) {
        cleanup_free(c);
        return;
    }
    show_cleanup(c);
}

void xd_drive_cleanup(GtkWindow *parent, GFile *item, const char *name) {
    cleanup *c = g_new0(cleanup, 1);
    c->parent = GTK_WIDGET(parent);
    g_object_add_weak_pointer(G_OBJECT(c->parent), (gpointer *)&c->parent);
    c->name = g_strdup(name);
    c->cancel = g_cancellable_new();
    c->places = places_for(item);
    char *text = g_strdup_printf("Disk Cleanup is calculating how much space you will be able to "
                                 "free on %s. This may take a few minutes to complete.",
                                 name);
    show_progress(c, text);
    g_free(text);
    GTask *task = g_task_new(nullptr, c->cancel, scanned, c);
    g_task_set_task_data(task, c, nullptr);
    g_task_run_in_thread(task, scan_thread);
    g_object_unref(task);
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

static void check_free(check *k) {
    if (k->pulse != 0)
        g_source_remove(k->pulse);
    g_free(k->object);
    g_free(k->name);
    g_free(k);
}

static void on_check_destroy(GtkWidget *w, gpointer data) {
    (void)w;
    check *k = data;
    k->dlg = nullptr;
    if (k->pulse != 0)
        g_source_remove(k->pulse);
    k->pulse = 0;
    /* A check under way frees it when UDisks2 answers. */
    if (k->start != nullptr)
        check_free(k);
}

static void checked(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    check *k = data;
    GError *err = nullptr;
    g_autofree char *dbus_error = nullptr;
    GVariant *r = xd_udisks_call_finish(res, &dbus_error, &err);
    if (k->dlg == nullptr) {
        g_clear_pointer(&r, g_variant_unref);
        g_clear_error(&err);
        check_free(k);
        return;
    }
    if (k->pulse != 0)
        g_source_remove(k->pulse);
    k->pulse = 0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(k->bar), r != nullptr ? 1.0 : 0.0);
    gboolean fine = FALSE;
    if (r != nullptr) {
        g_variant_get(r, "(b)", &fine);
        g_variant_unref(r);
    }
    char *title = g_strdup_printf("Checking Disk %s", k->name);
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
    } else if (k->repair) {
        icon = XD_MB_WARNING;
        msg = g_strdup_printf("Disk Check could not repair all the errors on %s.", k->name);
    } else {
        icon = XD_MB_WARNING;
        msg = g_strdup_printf("Disk Check found errors on %s. To fix them, check the disk "
                              "again with \"Automatically fix file system errors\" selected.",
                              k->name);
    }
    g_clear_error(&err);
    xd_message_box(GTK_WINDOW(k->dlg), title, icon, XD_MB_OK, FALSE, msg);
    g_free(msg);
    g_free(title);
    /* As XP, the box closes once the result has been seen. */
    if (k->dlg != nullptr)
        gtk_widget_destroy(k->dlg);
    check_free(k);
}

static void on_check_response(GtkDialog *dlg, gint response, gpointer data) {
    check *k = data;
    if (response != GTK_RESPONSE_OK) {
        if (k->start != nullptr) /* not while UDisks2 is checking */
            gtk_widget_destroy(GTK_WIDGET(dlg));
        return;
    }
    if (k->start == nullptr)
        return;
    k->repair = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(k->fix));
    gtk_widget_set_sensitive(k->fix, FALSE);
    gtk_widget_set_sensitive(k->start, FALSE);
    gtk_dialog_set_response_sensitive(dlg, GTK_RESPONSE_CANCEL, FALSE);
    k->start = nullptr; /* under way: the dialog no longer owns it */
    k->pulse = g_timeout_add(100, on_check_pulse, k);
    xd_udisks_call(k->object, XD_UDISKS_FILESYSTEM, k->repair ? "Repair" : "Check",
                   g_variant_new("(a{sv})", nullptr), nullptr, checked, k);
}

static void check_disk(GtkWindow *parent, const char *object, const char *name,
                       gboolean can_repair) {
    check *k = g_new0(check, 1);
    k->object = g_strdup(object);
    k->name = g_strdup(name);
    char *title = g_strdup_printf("Check Disk %s", name);
    k->dlg = gtk_dialog_new_with_buttons(title, parent, MODAL_DIALOG, nullptr, nullptr);
    g_free(title);
    k->start = gtk_dialog_add_button(GTK_DIALOG(k->dlg), "_Start", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(k->dlg), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(k->dlg), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(k->dlg), FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(k->dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_set_spacing(GTK_BOX(box), 10);
    GtkWidget *frame = gtk_frame_new("Check disk options");
    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(opts), 8);
    /* Without it, UDisks2 only checks (read-only); with it, repairs. */
    k->fix = gtk_check_button_new_with_mnemonic("_Automatically fix file system errors");
    gtk_widget_set_sensitive(k->fix, can_repair);
    gtk_box_pack_start(GTK_BOX(opts), k->fix, FALSE, FALSE, 0);
    /* Linux file systems keep their own bad-block lists (badblocks, run
     * by hand on an unmounted disk); UDisks2 has nothing for it. */
    k->bad = gtk_check_button_new_with_mnemonic("_Scan for and attempt recovery of bad sectors");
    gtk_widget_set_sensitive(k->bad, FALSE);
    gtk_box_pack_start(GTK_BOX(opts), k->bad, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), opts);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    k->bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), k->bar, FALSE, FALSE, 0);
    g_signal_connect(k->dlg, "response", G_CALLBACK(on_check_response), k);
    g_signal_connect(k->dlg, "destroy", G_CALLBACK(on_check_destroy), k);
    gtk_widget_show_all(k->dlg);
}

/* ---- the Tools tab ---------------------------------------------------------------- */

typedef struct tools {
    GtkWidget *dialog;
    GFile *item;
    char *name;
    GDesktopAppInfo *backup;
} tools;

static void tools_free(gpointer p) {
    tools *t = p;
    g_object_unref(t->item);
    g_free(t->name);
    g_clear_object(&t->backup);
    g_free(t);
}

static void on_check_now(GtkButton *b, gpointer data) {
    (void)b;
    tools *t = data;
    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(t->item);
    if (fs == nullptr) {
        xd_message_box(GTK_WINDOW(t->dialog), "Check Disk", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The disk check could not be performed: UDisks2 doesn't know this drive.");
        return;
    }
    check_disk(GTK_WINDOW(t->dialog), fs->object, t->name, xd_udisks_can("CanRepair", fs->type));
}

/* Disk Defragmenter's own verdict, said straight away: Linux's file
 * systems allocate files in extents and delay allocation, so they stay
 * in one piece without help. */
static void on_defragment(GtkButton *b, gpointer data) {
    (void)b;
    tools *t = data;
    char *msg = g_strdup_printf(
        "Analysis is complete for: %s\n\nYou do not need to defragment this volume.\n\n"
        "Its file system keeps files in one piece as it writes them, so it never needs "
        "defragmenting.",
        t->name);
    xd_message_box(GTK_WINDOW(t->dialog), "Disk Defragmenter", XD_MB_INFO, XD_MB_OK, FALSE, msg);
    g_free(msg);
}

static void on_backup(GtkButton *b, gpointer data) {
    (void)b;
    tools *t = data;
    GError *err = nullptr;
    if (t->backup != nullptr && !g_app_info_launch(G_APP_INFO(t->backup), nullptr, nullptr, &err)) {
        xd_message_box(GTK_WINDOW(t->dialog), "Backup", XD_MB_ERROR, XD_MB_OK, FALSE,
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
    GtkWidget *b = gtk_button_new_with_mnemonic(button);
    gtk_widget_set_halign(b, GTK_ALIGN_END);
    gtk_widget_set_sensitive(b, enabled);
    if (!enabled && why != nullptr)
        gtk_widget_set_tooltip_text(b, why);
    g_signal_connect(b, "clicked", cb, data);
    gtk_grid_attach(GTK_GRID(grid), b, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
}

GtkWidget *xd_drive_tools_page(GtkWidget *dialog, GFile *item, const char *name,
                               const char *kind) {
    tools *t = g_new0(tools, 1);
    t->dialog = dialog;
    t->item = g_object_ref(item);
    t->name = g_strdup(name);
    t->backup = find_backup();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    g_object_set_data_full(G_OBJECT(box), "xd-tools", t, tools_free);

    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(item);
    gboolean can_check = fs != nullptr && xd_udisks_can("CanCheck", fs->type);
    tool_frame(box, "Error-checking", "system-search",
               "This option will check the volume for errors.", "_Check Now...",
               G_CALLBACK(on_check_now), t, can_check,
               fs == nullptr ? "UDisks2 doesn't know this drive."
                             : "UDisks2 can't check this file system here.");
    gboolean local = kind != nullptr && (strcmp(kind, "Local Disk") == 0 ||
                                         strcmp(kind, "Removable Disk") == 0);
    tool_frame(box, "Defragmentation", "view-grid-symbolic",
               "This option will defragment files on the volume.", "_Defragment Now...",
               G_CALLBACK(on_defragment), t, local, "Only local disks can be defragmented.");
    tool_frame(box, "Backup", "document-save", "This option will back up files on the volume.",
               "_Backup Now...", G_CALLBACK(on_backup), t, t->backup != nullptr,
               "No backup program is installed (such as Déjà Dup or Pika Backup).");
    return box;
}

/* ---- the Hardware tab -------------------------------------------------------------- */

enum { HW_NAME, HW_TYPE, HW_INDEX, HW_COLS };

typedef struct hardware {
    GtkWidget *dialog, *list, *maker, *where, *status;
    GPtrArray *drives;
} hardware;

static void hardware_free(gpointer p) {
    hardware *h = p;
    g_ptr_array_unref(h->drives);
    g_free(h);
}

static const char *manufacturer(const XdUdisksDrive *d) {
    if (d->vendor[0] != '\0')
        return d->vendor;
    return d->optical ? "(Standard CD-ROM drives)" : "(Standard disk drives)";
}

static char *location(const XdUdisksDrive *d) {
    static const struct {
        const char *bus, *name;
    } buses[] = {{"usb", "USB"}, {"sdio", "SD card reader"}, {"ieee1394", "FireWire"}};
    const char *bus = nullptr;
    for (size_t i = 0; i < G_N_ELEMENTS(buses); i++)
        if (strcmp(d->bus, buses[i].bus) == 0)
            bus = buses[i].name;
    if (d->device[0] == '\0')
        return g_strdup(bus != nullptr ? bus : "Unknown");
    return bus != nullptr ? g_strdup_printf("%s (%s)", d->device, bus) : g_strdup(d->device);
}

static const XdUdisksDrive *hw_selected(hardware *h) {
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(h->list));
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return nullptr;
    guint index = 0;
    gtk_tree_model_get(model, &it, HW_INDEX, &index, -1);
    return index < h->drives->len ? g_ptr_array_index(h->drives, index) : nullptr;
}

static void on_hw_selected(GtkTreeSelection *sel, gpointer data) {
    (void)sel;
    hardware *h = data;
    const XdUdisksDrive *d = hw_selected(h);
    char *where = d != nullptr ? location(d) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(h->maker), d != nullptr ? manufacturer(d) : "");
    gtk_label_set_text(GTK_LABEL(h->where), where);
    gtk_label_set_text(GTK_LABEL(h->status),
                       d != nullptr ? "This device is working properly." : "");
    g_free(where);
}

static void grid_row(GtkWidget *grid, int *row, const char *title, const char *value) {
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_grid_attach(GTK_GRID(grid), t, 0, *row, 1, 1);
    GtkWidget *v = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_can_focus(v, FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), v, 1, (*row)++, 1, 1);
}

/* "68,719,476,736", as Windows groups digits. */
static void append_grouped(GString *s, guint64 n) {
    char digits[32];
    g_snprintf(digits, sizeof digits, "%" G_GUINT64_FORMAT, n);
    size_t len = strlen(digits);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            g_string_append_c(s, ',');
        g_string_append_c(s, digits[i]);
    }
}

static char *megabytes(guint64 bytes) {
    GString *s = g_string_new(nullptr);
    append_grouped(s, bytes / (1024 * 1024));
    g_string_append(s, " MB");
    return g_string_free(s, FALSE);
}

/* XP's device Properties: General (type, manufacturer, location,
 * status), Volumes (the disk and its volumes, filled at once rather than
 * after Populate) and Details (what UDisks2 knows of it). */
static void device_properties(GtkWindow *parent, const XdUdisksDrive *d, guint index,
                              guint optical_index) {
    char *title = g_strdup_printf("%s Properties", d->name);
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
    const char *icon = d->optical ? "drive-optical" : d->removable ? "drive-removable-media"
                                                                   : "drive-harddisk";

    /* General */
    GtkWidget *general = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(general), 6);
    gtk_grid_set_column_spacing(GTK_GRID(general), 12);
    gtk_container_set_border_width(GTK_CONTAINER(general), 10);
    int row = 0;
    gtk_grid_attach(GTK_GRID(general), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND), 0,
                    row, 1, 1);
    GtkWidget *name = gtk_label_new(d->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_grid_attach(GTK_GRID(general), name, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(general), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++,
                    2, 1);
    char *where = location(d);
    grid_row(general, &row, "Device type:", d->type);
    grid_row(general, &row, "Manufacturer:", manufacturer(d));
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
    char *disk = d->optical ? g_strdup_printf("CD-ROM %u", optical_index)
                            : g_strdup_printf("Disk %u", index);
    char *capacity = megabytes(d->size);
    grid_row(vgrid, &row, "Disk:", disk);
    grid_row(vgrid, &row, "Type:", d->optical ? "DVD" : d->removable ? "Removable" : "Basic");
    grid_row(vgrid, &row, "Status:", d->size > 0 ? "Online" : "No Media");
    grid_row(vgrid, &row, "Partition style:",
             strcmp(d->table, "gpt") == 0   ? "GUID Partition Table (GPT)"
             : strcmp(d->table, "dos") == 0 ? "Master Boot Record (MBR)"
                                            : "Not Applicable");
    grid_row(vgrid, &row, "Capacity:", capacity);
    g_free(disk);
    g_free(capacity);
    gtk_box_pack_start(GTK_BOX(vbox), vgrid, FALSE, FALSE, 0);
    GtkWidget *vl = gtk_label_new("Volumes:");
    gtk_label_set_xalign(GTK_LABEL(vl), 0);
    gtk_box_pack_start(GTK_BOX(vbox), vl, FALSE, FALSE, 0);
    GtkListStore *vstore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GPtrArray *vols = xd_udisks_volumes(d->object);
    for (guint i = 0; i < vols->len; i++) {
        const XdUdisksVolume *v = g_ptr_array_index(vols, i);
        char *label = v->label[0] != '\0' ? g_strdup_printf("%s (%s)", v->label, v->device)
                                          : g_strdup(v->device);
        char *size = megabytes(v->size);
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
    GString *b = g_string_new(nullptr);
    append_grouped(b, d->size);
    g_string_append(b, " bytes");
    char *bytes = g_string_free(b, FALSE);
    char *rpm = d->rotation > 0    ? g_strdup_printf("%d rpm", d->rotation)
                : d->rotation == 0 ? g_strdup("Solid state (no rotation)")
                                   : g_strdup("Unknown");
    grid_row(details, &row, "Model:", d->model);
    grid_row(details, &row, "Vendor:", d->vendor);
    grid_row(details, &row, "Revision:", d->revision);
    grid_row(details, &row, "Serial number:", d->serial);
    grid_row(details, &row, "Connection:", d->bus[0] != '\0' ? d->bus : "Internal");
    grid_row(details, &row, "Size:", bytes);
    grid_row(details, &row, "Rotation rate:", rpm);
    grid_row(details, &row, "Removable media:", d->removable ? "Yes" : "No");
    grid_row(details, &row, "Device:", d->device);
    g_free(bytes);
    g_free(rpm);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), details, gtk_label_new("Details"));

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_hw_properties(GtkWidget *w, gpointer data) {
    (void)w;
    hardware *h = data;
    const XdUdisksDrive *d = hw_selected(h);
    if (d == nullptr)
        return;
    /* Disk Management's numbers: disks and CD-ROMs counted apart. */
    guint disks = 0, opticals = 0;
    for (guint i = 0; i < h->drives->len; i++) {
        const XdUdisksDrive *x = g_ptr_array_index(h->drives, i);
        if (x == d)
            break;
        if (x->optical)
            opticals++;
        else
            disks++;
    }
    device_properties(GTK_WINDOW(h->dialog), d, disks, opticals);
}

static void on_hw_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col,
                            gpointer data) {
    (void)view;
    (void)path;
    (void)col;
    on_hw_properties(nullptr, data);
}

GtkWidget *xd_drive_hardware_page(GtkWidget *dialog, GFile *item) {
    hardware *h = g_new0(hardware, 1);
    h->dialog = dialog;
    h->drives = xd_udisks_drives();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    g_object_set_data_full(G_OBJECT(box), "xd-hardware", h, hardware_free);

    GtkWidget *title = gtk_label_new_with_mnemonic("All disk _drives:");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    GtkListStore *store = gtk_list_store_new(HW_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
    g_autoptr(XdUdisksFs) fs = xd_udisks_fs_for(item);
    GtkTreeIter mine = {0};
    gboolean have_mine = FALSE;
    for (guint i = 0; i < h->drives->len; i++) {
        const XdUdisksDrive *d = g_ptr_array_index(h->drives, i);
        GtkTreeIter it;
        gtk_list_store_insert_with_values(store, &it, -1, HW_NAME, d->name, HW_TYPE, d->type,
                                          HW_INDEX, i, -1);
        if (!have_mine && fs != nullptr && strcmp(fs->drive, d->object) == 0) {
            mine = it;
            have_mine = TRUE;
        }
    }
    h->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_label_set_mnemonic_widget(GTK_LABEL(title), h->list);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Name", gtk_cell_renderer_text_new(), "text", HW_NAME, nullptr);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(h->list), col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(h->list),
                                gtk_tree_view_column_new_with_attributes(
                                    "Type", gtk_cell_renderer_text_new(), "text", HW_TYPE,
                                    nullptr));
    g_signal_connect(h->list, "row-activated", G_CALLBACK(on_hw_activated), h);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 320, 130);
    gtk_container_add(GTK_CONTAINER(scroll), h->list);
    gtk_box_pack_start(GTK_BOX(box), scroll, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new("Device Properties");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    int row = 0;
    grid_row(grid, &row, "Manufacturer:", "");
    h->maker = gtk_grid_get_child_at(GTK_GRID(grid), 1, row - 1);
    grid_row(grid, &row, "Location:", "");
    h->where = gtk_grid_get_child_at(GTK_GRID(grid), 1, row - 1);
    grid_row(grid, &row, "Device status:", "");
    h->status = gtk_grid_get_child_at(GTK_GRID(grid), 1, row - 1);
    GtkWidget *props = gtk_button_new_with_mnemonic("P_roperties");
    gtk_widget_set_halign(props, GTK_ALIGN_END);
    g_signal_connect(props, "clicked", G_CALLBACK(on_hw_properties), h);
    gtk_grid_attach(GTK_GRID(grid), props, 0, row++, 2, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(h->list));
    g_signal_connect(sel, "changed", G_CALLBACK(on_hw_selected), h);
    GtkTreeIter first;
    if (have_mine)
        gtk_tree_selection_select_iter(sel, &mine);
    else if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first))
        gtk_tree_selection_select_iter(sel, &first);
    gtk_widget_set_sensitive(props, h->drives->len > 0);
    g_object_unref(store);
    on_hw_selected(sel, h);
    return box;
}
