/* SPDX-License-Identifier: MIT
 * The Previous Versions tab. See versionsui.h.
 *
 * Looking for versions and restoring run on worker threads (GTask). The
 * page's state belongs to the Properties window and goes with it; a task
 * still running then finds its cancellable cancelled and touches nothing.
 */
#include "versionsui.h"

#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "fileops.h"
#include "versions.h"

enum {
    COLUMN_ICON,
    COLUMN_NAME,
    COLUMN_TIME,
    COLUMN_LOCATION,
    COLUMN_INDEX, /* into versions_page.versions */
    COLUMN_COUNT,
};

typedef struct versions_page {
    GtkWidget *dialog;
    GtkNotebook *notebook;
    GtkWidget *page;
    GtkWidget *page_reference; /* ours, until the page is in the notebook */
    gboolean is_attached;
    GtkWidget *tree, *list_stack, *open_button, *copy_button, *restore_button;
    GtkListStore *store;
    GIcon *icon;
    char *path, *name;
    gboolean is_directory;
    gboolean is_seven; /* Windows 7's tab, not XP's */
    gboolean is_busy;  /* restoring */
    GPtrArray *versions; /* xd_version*; nullptr until found */
    GCancellable *cancellable;
} versions_page;

static const char TITLE[] = "Previous Versions";

/* ---- formatting ------------------------------------------------------------------ */

/* XP's Time: "Today, October 22, 2026, 12:00 PM", "Yesterday, ...", else
 * the weekday's name. */
static char *versions_format_snapshot_time(gint64 seconds) {
    GDateTime *when = g_date_time_new_from_unix_local(seconds);
    if (when == nullptr)
        return g_strdup("");
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *yesterday = g_date_time_add_days(now, -1);
    const char *day = nullptr;
    int year, month, date;
    g_date_time_get_ymd(when, &year, &month, &date);
    int now_year, now_month, now_date;
    g_date_time_get_ymd(now, &now_year, &now_month, &now_date);
    int past_year, past_month, past_date;
    g_date_time_get_ymd(yesterday, &past_year, &past_month, &past_date);
    if (year == now_year && month == now_month && date == now_date)
        day = "Today";
    else if (year == past_year && month == past_month && date == past_date)
        day = "Yesterday";
    char *rest = g_date_time_format(when, "%B %d, %Y, %-I:%M %p");
    char *weekday = g_date_time_format(when, "%A");
    char *text = g_strdup_printf("%s, %s", day != nullptr ? day : weekday, rest);
    g_free(weekday);
    g_free(rest);
    g_date_time_unref(yesterday);
    g_date_time_unref(now);
    g_date_time_unref(when);
    return text;
}

/* 7's Date modified: "10/22/2026 12:00 PM". */
static char *versions_format_modified_time(gint64 seconds) {
    GDateTime *when = g_date_time_new_from_unix_local(seconds);
    if (when == nullptr)
        return g_strdup("");
    char *text = g_date_time_format(when, "%-m/%-d/%Y %-I:%M %p");
    g_date_time_unref(when);
    return text;
}

/* ---- the list --------------------------------------------------------------------- */

static const xd_version *versions_get_selected(versions_page *page) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(page->tree));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (page->versions == nullptr || !gtk_tree_selection_get_selected(selection, &model, &iter))
        return nullptr;
    int index = -1;
    gtk_tree_model_get(model, &iter, COLUMN_INDEX, &index, -1);
    if (index < 0 || (guint)index >= page->versions->len)
        return nullptr;
    return g_ptr_array_index(page->versions, index);
}

static void versions_update_buttons(versions_page *page) {
    gboolean can_act = !page->is_busy && versions_get_selected(page) != nullptr;
    gtk_widget_set_sensitive(page->open_button, can_act);
    gtk_widget_set_sensitive(page->copy_button, can_act);
    gtk_widget_set_sensitive(page->restore_button, can_act);
}

static void versions_fill_list(versions_page *page) {
    gtk_list_store_clear(page->store);
    for (guint i = 0; i < page->versions->len; i++) {
        const xd_version *version = g_ptr_array_index(page->versions, i);
        char *time = page->is_seven ? versions_format_modified_time(version->modified_time)
                                    : versions_format_snapshot_time(version->snapshot_time);
        gtk_list_store_insert_with_values(page->store, nullptr, -1, COLUMN_ICON, page->icon,
                                          COLUMN_NAME, page->name, COLUMN_TIME, time,
                                          COLUMN_LOCATION, version->location, COLUMN_INDEX,
                                          (int)i, -1);
        g_free(time);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(page->list_stack),
                                     page->versions->len > 0 ? "list" : "none");
    versions_update_buttons(page);
}

static void versions_add_to_notebook(versions_page *page, gboolean bring_to_front) {
    if (page->is_attached)
        return;
    gtk_widget_show_all(page->page);
    int index = gtk_notebook_append_page(page->notebook, page->page, gtk_label_new(TITLE));
    page->is_attached = TRUE;
    g_clear_object(&page->page_reference);
    if (bring_to_front)
        gtk_notebook_set_current_page(page->notebook, index);
}

/* ---- looking for versions -------------------------------------------------------- */

typedef struct versions_found {
    GPtrArray *versions;
    gboolean has_snapshots;
} versions_found;

static void versions_free_found(gpointer data) {
    versions_found *found = data;
    g_ptr_array_unref(found->versions);
    g_free(found);
}

static void versions_find_in_thread(GTask *task, [[maybe_unused]] gpointer source,
                                    gpointer task_data,
                                    [[maybe_unused]] GCancellable *cancellable) {
    versions_found *found = g_new0(versions_found, 1);
    found->versions = xd_versions_list(task_data, &found->has_snapshots);
    g_task_return_pointer(task, found, versions_free_found);
}

static void versions_on_found([[maybe_unused]] GObject *source, GAsyncResult *result,
                              gpointer data) {
    if (g_cancellable_is_cancelled(g_task_get_cancellable(G_TASK(result))))
        return; /* the window has gone, and `data` with it */
    versions_page *page = data;
    versions_found *found = g_task_propagate_pointer(G_TASK(result), nullptr);
    if (found == nullptr)
        return;
    g_clear_pointer(&page->versions, g_ptr_array_unref);
    page->versions = g_ptr_array_ref(found->versions);
    /* Not on a file system with snapshots: no tab at all. */
    if (found->has_snapshots)
        versions_add_to_notebook(page, FALSE);
    versions_free_found(found);
    if (page->is_attached)
        versions_fill_list(page);
}

static void versions_find(versions_page *page) {
    GTask *task = g_task_new(nullptr, page->cancellable, versions_on_found, page);
    g_task_set_task_data(task, g_strdup(page->path), g_free);
    g_task_run_in_thread(task, versions_find_in_thread);
    g_object_unref(task);
}

/* ---- View / Open, Copy... ---------------------------------------------------------- */

static void versions_show_error(versions_page *page, const char *message) {
    xd_message_box(GTK_WINDOW(page->dialog), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, message);
}

/* The snapshot's copy: a folder in a window of its own, a file in its
 * program. Snapshots are read-only, so it's opened as it is there. */
static void versions_open(versions_page *page) {
    const xd_version *version = versions_get_selected(page);
    if (version == nullptr)
        return;
    GError *error = nullptr;
    if (version->is_directory) {
        char *program = g_strdup("xd-files");
        char *argv[] = {program, version->path, nullptr};
        if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                           nullptr, &error)) {
            versions_show_error(page, error->message);
            g_error_free(error);
        }
        g_free(program);
        return;
    }
    char *uri = g_filename_to_uri(version->path, nullptr, nullptr);
    if (uri != nullptr && !g_app_info_launch_default_for_uri(uri, nullptr, &error)) {
        versions_show_error(page, error->message);
        g_error_free(error);
    }
    g_free(uri);
}

static void versions_on_open([[maybe_unused]] GtkButton *button, gpointer data) {
    versions_open(data);
}

static void versions_on_row_activated([[maybe_unused]] GtkTreeView *tree,
                                      [[maybe_unused]] GtkTreePath *row,
                                      [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    versions_open(data);
}

/* Copy...: a folder to copy the version into, then the usual copy, with
 * its progress and Confirm File Replace. */
static void versions_on_copy([[maybe_unused]] GtkButton *button, gpointer data) {
    versions_page *page = data;
    const xd_version *version = versions_get_selected(page);
    if (version == nullptr)
        return;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Copy Items", GTK_WINDOW(page->dialog), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel",
        GTK_RESPONSE_CANCEL, "_Copy", GTK_RESPONSE_ACCEPT, nullptr);
    char *folder = g_path_get_dirname(page->path);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), folder);
    g_free(folder);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        GFile *destination = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(chooser));
        if (destination != nullptr) {
            GFile *copy = g_file_new_for_path(version->path);
            GList *sources = g_list_append(nullptr, copy);
            xd_fileop_start(GTK_WINDOW(page->dialog), XD_FILEOP_COPY, sources, destination,
                            nullptr, nullptr);
            g_list_free(sources);
            g_object_unref(copy);
            g_object_unref(destination);
        }
    }
    gtk_widget_destroy(chooser);
}

/* ---- Restore ---------------------------------------------------------------------- */

typedef struct versions_restore_job {
    char *source, *target;
} versions_restore_job;

static void versions_free_restore_job(gpointer data) {
    versions_restore_job *job = data;
    g_free(job->source);
    g_free(job->target);
    g_free(job);
}

static void versions_restore_in_thread(GTask *task, [[maybe_unused]] gpointer source,
                                       gpointer task_data, GCancellable *cancellable) {
    versions_restore_job *job = task_data;
    GError *error = nullptr;
    if (xd_versions_restore(job->source, job->target, cancellable, &error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);
}

static void versions_on_restored([[maybe_unused]] GObject *source, GAsyncResult *result,
                                 gpointer data) {
    if (g_cancellable_is_cancelled(g_task_get_cancellable(G_TASK(result))))
        return;
    versions_page *page = data;
    page->is_busy = FALSE;
    versions_update_buttons(page);
    GError *error = nullptr;
    if (g_task_propagate_boolean(G_TASK(result), &error)) {
        xd_message_box(GTK_WINDOW(page->dialog), TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       page->is_directory ? "The folder has been successfully restored."
                                          : "The file has been successfully restored.");
    } else {
        char *message = g_strdup_printf("“%s” couldn't be restored completely.\n\n%s",
                                        page->name, error->message);
        versions_show_error(page, message);
        g_free(message);
        g_error_free(error);
    }
    /* The item has changed: the list again, as it compares with it. */
    versions_find(page);
}

static void versions_on_restore([[maybe_unused]] GtkButton *button, gpointer data) {
    versions_page *page = data;
    const xd_version *version = versions_get_selected(page);
    if (version == nullptr || page->is_busy)
        return;
    char *question;
    if (page->is_seven)
        question = g_strdup_printf(
            "Are you sure you want to restore the previous version of “%s”?\n\n"
            "Restoring this version will replace the current version of the %s and can't "
            "be undone.",
            page->name, page->is_directory ? "files in this folder" : "file");
    else
        question = g_strdup("Restoring a previous version will replace the current version.\n\n"
                            "Are you sure you want to restore the previous version?");
    int answer =
        xd_message_box(GTK_WINDOW(page->dialog), TITLE, XD_MB_WARNING, XD_MB_YES_NO, TRUE, question);
    g_free(question);
    if (answer != GTK_RESPONSE_YES)
        return;
    versions_restore_job *job = g_new0(versions_restore_job, 1);
    job->source = g_strdup(version->path);
    job->target = g_strdup(page->path);
    page->is_busy = TRUE;
    versions_update_buttons(page);
    GTask *task = g_task_new(nullptr, page->cancellable, versions_on_restored, page);
    g_task_set_task_data(task, job, versions_free_restore_job);
    g_task_run_in_thread(task, versions_restore_in_thread);
    g_object_unref(task);
}

/* ---- building the page ------------------------------------------------------------ */

static void versions_on_selection_changed([[maybe_unused]] GtkTreeSelection *selection,
                                          gpointer data) {
    versions_update_buttons(data);
}

static void versions_on_dialog_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    versions_page *page = data;
    g_cancellable_cancel(page->cancellable);
    g_object_unref(page->cancellable);
    g_clear_object(&page->page_reference);
    g_clear_pointer(&page->versions, g_ptr_array_unref);
    g_clear_object(&page->store);
    g_clear_object(&page->icon);
    g_free(page->path);
    g_free(page->name);
    g_free(page);
}

static GtkTreeViewColumn *versions_add_text_column(versions_page *page, const char *title,
                                                   int column) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *tree_column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", column, nullptr);
    gtk_tree_view_column_set_resizable(tree_column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(page->tree), tree_column);
    return tree_column;
}

static GtkWidget *versions_build_page(versions_page *page) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);

    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name("document-open-recent", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(intro), icon, FALSE, FALSE, 0);
    const char *kind = page->is_directory ? "folder" : "file";
    char *text =
        page->is_seven
            ? g_strdup("Previous versions come from snapshots of the file system, taken by "
                       "Snapper, Timeshift or ZFS.")
            : g_strdup_printf("To view a previous version of the %s, select the version from "
                              "the list below and click View. You can also copy the %s to a "
                              "different location or restore the %s to a previous version.",
                              kind, kind, kind);
    GtkWidget *label = gtk_label_new(text);
    g_free(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 46);
    gtk_box_pack_start(GTK_BOX(intro), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    page->store = gtk_list_store_new(COLUMN_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_INT);
    page->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(page->store));
    GtkWidget *heading =
        gtk_label_new_with_mnemonic(page->is_directory ? "_Folder versions:" : "_File versions:");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(heading), page->tree);
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);

    GtkCellRenderer *icon_renderer = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *name_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *name_column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(name_column, "Name");
    gtk_tree_view_column_pack_start(name_column, icon_renderer, FALSE);
    gtk_tree_view_column_add_attribute(name_column, icon_renderer, "gicon", COLUMN_ICON);
    gtk_tree_view_column_pack_start(name_column, name_renderer, TRUE);
    gtk_tree_view_column_add_attribute(name_column, name_renderer, "text", COLUMN_NAME);
    gtk_tree_view_column_set_resizable(name_column, TRUE);
    gtk_tree_view_column_set_expand(name_column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(page->tree), name_column);
    versions_add_text_column(page, page->is_seven ? "Date modified" : "Time", COLUMN_TIME);
    if (page->is_seven && page->is_directory)
        versions_add_text_column(page, "Location", COLUMN_LOCATION);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(page->tree));
    g_signal_connect(selection, "changed", G_CALLBACK(versions_on_selection_changed), page);
    g_signal_connect(page->tree, "row-activated", G_CALLBACK(versions_on_row_activated), page);

    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), page->tree);
    /* Windows says so in the list's place when the item has none. */
    GtkWidget *none_frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(none_frame), GTK_SHADOW_IN);
    GtkWidget *none = gtk_label_new("There are no previous versions available.");
    gtk_widget_set_margin_top(none, 24);
    /* In a box: the theme colours a frame's own label as a group box's. */
    GtkWidget *none_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(none_box), none, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(none_frame), none_box);
    page->list_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(page->list_stack), scroll, "list");
    gtk_stack_add_named(GTK_STACK(page->list_stack), none_frame, "none");
    gtk_widget_set_size_request(page->list_stack, 340, 220);
    gtk_box_pack_start(GTK_BOX(box), page->list_stack, TRUE, TRUE, 0);

    GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(buttons), 6);
    page->open_button = gtk_button_new_with_mnemonic(page->is_seven ? "_Open" : "_View");
    page->copy_button = gtk_button_new_with_mnemonic("_Copy...");
    page->restore_button = gtk_button_new_with_mnemonic(page->is_seven ? "_Restore..." : "_Restore");
    g_signal_connect(page->open_button, "clicked", G_CALLBACK(versions_on_open), page);
    g_signal_connect(page->copy_button, "clicked", G_CALLBACK(versions_on_copy), page);
    g_signal_connect(page->restore_button, "clicked", G_CALLBACK(versions_on_restore), page);
    gtk_container_add(GTK_CONTAINER(buttons), page->open_button);
    gtk_container_add(GTK_CONTAINER(buttons), page->copy_button);
    gtk_container_add(GTK_CONTAINER(buttons), page->restore_button);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    versions_update_buttons(page);
    return box;
}

void xd_versions_page_attach(GtkWidget *dialog, GtkNotebook *notebook, GFile *file,
                             gboolean is_directory, gboolean show_now) {
    char *path = g_file_get_path(file);
    if (path == nullptr)
        return;
    versions_page *page = g_new0(versions_page, 1);
    page->dialog = dialog;
    page->notebook = notebook;
    page->path = path;
    page->is_directory = is_directory;
    page->cancellable = g_cancellable_new();
    xd_settings settings;
    xd_settings_load(&settings);
    page->is_seven = settings.style == XD_STYLE_AERO;
    GFileInfo *info = g_file_query_info(file, "standard::display-name,standard::icon",
                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    if (info != nullptr) {
        page->name = g_strdup(g_file_info_get_display_name(info));
        GIcon *icon = g_file_info_get_icon(info);
        page->icon = icon != nullptr ? g_object_ref(icon) : nullptr;
        g_object_unref(info);
    }
    if (page->name == nullptr)
        page->name = g_path_get_basename(path);
    page->page = versions_build_page(page);
    page->page_reference = g_object_ref_sink(page->page);
    g_signal_connect(dialog, "destroy", G_CALLBACK(versions_on_dialog_destroy), page);
    if (show_now) {
        versions_add_to_notebook(page, TRUE);
        page->versions = g_ptr_array_new();
        versions_fill_list(page);
        gtk_stack_set_visible_child_name(GTK_STACK(page->list_stack), "list");
    }
    versions_find(page);
}
