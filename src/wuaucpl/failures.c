/* SPDX-License-Identifier: MIT
 * Why an install failed, in words: the error box (what went wrong in a
 * sentence or two, the packages or files involved, and Details >> with
 * everything the package manager said), the line in the journal, and XP's
 * installation history, which keeps the same for later (the scheduled
 * check's failures too, which happen with nobody watching).
 */
#include <stdio.h>
#include <string.h>

#include "../appwiz/programs.h"
#include "../settings/soundplay.h"
#include "updlib.h"
#include "wuaucpl.h"

/* Bullets under the summary; the rest are under Details. */
static constexpr unsigned SHOWN_ITEMS = 10;

void xd_wuau_log_failure(const char *what, const char *code_name, const char *details) {
    fprintf(stderr, "xd-wuaucpl: %s (%s): %s\n", what, code_name != nullptr ? code_name : "error",
            details != nullptr && details[0] != '\0' ? details : "no details");
}

void xd_wuau_record(xd_update_action action, xd_update_result result, const GPtrArray *names,
                    const char *code_name, const char *summary, const char *details) {
    GPtrArray *updates = g_ptr_array_new();
    for (guint i = 0; names != nullptr && i < names->len; i++)
        g_ptr_array_add(updates, g_ptr_array_index(names, i));
    g_ptr_array_add(updates, nullptr);
    char *error = g_strdup(code_name != nullptr ? code_name : "");
    char *text = g_strdup(summary != nullptr ? summary : "");
    char *full = g_strdup(details != nullptr ? details : "");
    xd_update_history_entry entry = {
        .time = g_get_real_time() / G_USEC_PER_SEC,
        .action = action,
        .result = result,
        .updates = (char **)updates->pdata,
        .error = error,
        .summary = text,
        .details = full,
    };
    char *path = xd_update_history_path();
    if (!xd_update_history_add(path, &entry))
        fprintf(stderr, "xd-wuaucpl: cannot write %s\n", path);
    g_free(path);
    g_free(error);
    g_free(text);
    g_free(full);
    g_ptr_array_free(updates, TRUE);
}

/* ---- the error box ------------------------------------------------------------------ */

typedef struct failure_box {
    GtkWidget *dialog, *details, *toggle;
    char *details_text; /* one item a line, for the view and Copy */
    char *box_text;     /* the box as Ctrl+C copies it */
} failure_box;

/* A read-only, selectable text; `fixed` text is in a monospace font, as
 * the package manager wrote it. Our theme's CSS doesn't style GTK's
 * .monospace class, so it's a tag. */
static GtkWidget *text_view(const char *fixed) {
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 4);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_create_tag(buffer, "fixed", "family", "monospace", nullptr);
    if (fixed != nullptr) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_insert_with_tags_by_name(buffer, &end, fixed, -1, "fixed", nullptr);
    }
    return view;
}

static GtkWidget *scrolled(GtkWidget *child, int width, int height) {
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, width, height);
    gtk_container_add(GTK_CONTAINER(scroll), child);
    return scroll;
}

static void on_details_toggle(GtkButton *button, gpointer data) {
    failure_box *box = data;
    bool show = !gtk_widget_get_visible(box->details);
    gtk_widget_set_visible(box->details, show);
    /* Windows' Details >> and << Details. */
    gtk_button_set_label(button, show ? "<< _Details" : "_Details >>");
}

static void on_ok([[maybe_unused]] GtkButton *button, gpointer dialog) {
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
}

static void on_copy([[maybe_unused]] GtkButton *button, gpointer data) {
    failure_box *box = data;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), box->details_text, -1);
}

/* Ctrl+C copies the whole box as text, as Windows' message boxes do,
 * unless there's a selection in the details to copy instead. */
static gboolean on_box_key(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    failure_box *box = data;
    bool ctrl_c = (event->state & GDK_CONTROL_MASK) &&
                  (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C ||
                   event->keyval == GDK_KEY_Insert);
    if (!ctrl_c)
        return FALSE;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(widget));
    if (GTK_IS_TEXT_VIEW(focus) &&
        gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(GTK_TEXT_VIEW(focus))))
        return FALSE;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), box->box_text, -1);
    return TRUE;
}

static GtkWidget *wrapped_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 64);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    return label;
}

void xd_wuau_show_failure(wuau *applet, const char *headline, const char *summary,
                          const char *details) {
    failure_box box = {0};
    GPtrArray *items = xd_update_get_error_items(details);
    char *bullets = xd_update_list_error_items(items, SHOWN_ITEMS);
    g_ptr_array_unref(items);
    box.details_text = xd_update_format_error_details(details);
    bool have_details = box.details_text[0] != '\0';

    xd_sound_play_event(XD_SOUND_CRITICAL_STOP);
    GtkWidget *dialog = gtk_dialog_new();
    box.dialog = dialog;
    gtk_window_set_title(GTK_WINDOW(dialog), XD_WUAU_TITLE);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(applet->window));
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkWidget *message = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *image = gtk_image_new_from_icon_name("dialog-error", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
    gtk_widget_set_valign(image, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(message), image, FALSE, FALSE, 0);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *head = wrapped_label(nullptr);
    char *markup = g_markup_printf_escaped("<b>%s</b>", headline);
    gtk_label_set_markup(GTK_LABEL(head), markup);
    g_free(markup);
    gtk_box_pack_start(GTK_BOX(texts), head, FALSE, FALSE, 0);
    if (summary != nullptr && summary[0] != '\0')
        gtk_box_pack_start(GTK_BOX(texts), wrapped_label(summary), FALSE, FALSE, 0);
    if (bullets[0] != '\0') {
        GtkWidget *list = wrapped_label(bullets);
        gtk_label_set_ellipsize(GTK_LABEL(list), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_line_wrap(GTK_LABEL(list), FALSE);
        gtk_widget_set_margin_start(list, 8);
        gtk_box_pack_start(GTK_BOX(texts), list, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(message), texts, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), message, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    GtkWidget *ok = gtk_button_new_with_label("OK");
    gtk_widget_set_size_request(ok, 75, -1);
    gtk_widget_set_can_default(ok, TRUE);
    g_signal_connect(ok, "clicked", G_CALLBACK(on_ok), dialog);
    gtk_box_pack_start(GTK_BOX(row), ok, FALSE, FALSE, 0);
    if (have_details) {
        box.toggle = gtk_button_new_with_mnemonic("_Details >>");
        gtk_widget_set_size_request(box.toggle, 75, -1);
        g_signal_connect(box.toggle, "clicked", G_CALLBACK(on_details_toggle), &box);
        gtk_box_pack_start(GTK_BOX(row), box.toggle, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(content), row, FALSE, FALSE, 0);

    if (have_details) {
        /* Everything the package manager said, one item a line. */
        box.details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *view = text_view(box.details_text);
        gtk_box_pack_start(GTK_BOX(box.details), scrolled(view, 540, 200), TRUE, TRUE, 0);
        GtkWidget *copy = gtk_button_new_with_mnemonic("_Copy");
        gtk_widget_set_size_request(copy, 75, -1);
        gtk_widget_set_halign(copy, GTK_ALIGN_END);
        g_signal_connect(copy, "clicked", G_CALLBACK(on_copy), &box);
        gtk_box_pack_start(GTK_BOX(box.details), copy, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), box.details, TRUE, TRUE, 0);
    }
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), content);

    GString *copied = g_string_new("---------------------------\n" XD_WUAU_TITLE
                                   "\n---------------------------\n");
    g_string_append(copied, headline);
    if (summary != nullptr && summary[0] != '\0')
        g_string_append_printf(copied, "\n\n%s", summary);
    if (bullets[0] != '\0')
        g_string_append_printf(copied, "\n\n%s", bullets);
    g_string_append_printf(copied, "\n---------------------------\nOK%s\n"
                                   "---------------------------\n",
                           have_details ? "   Details >>" : "");
    box.box_text = g_string_free(copied, FALSE);
    g_signal_connect(dialog, "key-press-event", G_CALLBACK(on_box_key), &box);

    gtk_widget_show_all(dialog);
    if (box.details != nullptr)
        gtk_widget_hide(box.details);
    gtk_widget_grab_default(ok);
    gtk_widget_grab_focus(ok);
    /* Until OK, Escape or the close box. */
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(bullets);
    g_free(box.details_text);
    g_free(box.box_text);
}

/* ---- the installation history ----------------------------------------------------- */

enum { HISTORY_DATE, HISTORY_UPDATES, HISTORY_STATUS, HISTORY_INDEX, HISTORY_COUNT };

typedef struct history_window {
    GPtrArray *entries; /* xd_update_history_entry * */
    GtkWidget *view, *text;
} history_window;

/* "10/27/2026 3:00 AM". g_free it. */
static char *format_when(int64_t when) {
    char *date = xd_appwiz_format_date(when);
    GDateTime *date_time = g_date_time_new_from_unix_local(when);
    char *time = date_time != nullptr ? g_date_time_format(date_time, "%-I:%M %p") : g_strdup("");
    char *text = g_strdup_printf("%s %s", date, time);
    if (date_time != nullptr)
        g_date_time_unref(date_time);
    g_free(date);
    g_free(time);
    return text;
}

/* What the entry was about, for its row. g_free it. */
static char *describe_updates(const xd_update_history_entry *entry) {
    char *names = g_strjoinv(", ", entry->updates);
    char *text;
    if (entry->action == XD_UPDATE_ACTION_CHECK)
        text = g_strdup("Checking for updates");
    else if (entry->action == XD_UPDATE_ACTION_DOWNLOAD)
        text = names[0] != '\0' ? g_strdup_printf("Downloading %s", names)
                                : g_strdup("Downloading updates");
    else
        text = names[0] != '\0' ? g_strdup(names) : g_strdup("Updates");
    g_free(names);
    return text;
}

static void show_entry(history_window *history) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(history->text));
    gtk_text_buffer_set_text(buffer, "", -1);
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(history->view));
    if (!gtk_tree_selection_get_selected(selection, &model, &it)) {
        gtk_text_buffer_set_text(buffer, "Select an entry to see its details.", -1);
        return;
    }
    unsigned index = 0;
    gtk_tree_model_get(model, &it, HISTORY_INDEX, &index, -1);
    if (index >= history->entries->len)
        return;
    const xd_update_history_entry *entry = g_ptr_array_index(history->entries, index);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    static const char *const actions[XD_UPDATE_ACTION_COUNT] = {
        [XD_UPDATE_ACTION_INSTALL] = "Installation",
        [XD_UPDATE_ACTION_DOWNLOAD] = "Download",
        [XD_UPDATE_ACTION_CHECK] = "Check for updates",
    };
    char *when = format_when(entry->time);
    char *line = g_strdup_printf("%s: %s, %s\n", actions[entry->action],
                                 xd_update_result_name(entry->result), when);
    gtk_text_buffer_insert_with_tags_by_name(buffer, &end, line, -1, "bold", nullptr);
    g_free(line);
    g_free(when);
    if (entry->summary[0] != '\0') {
        gtk_text_buffer_insert(buffer, &end, entry->summary, -1);
        gtk_text_buffer_insert(buffer, &end, "\n", -1);
    }
    if (entry->updates[0] != nullptr) {
        gtk_text_buffer_insert(buffer, &end, "\nUpdates:\n", -1);
        for (char **name = entry->updates; *name != nullptr; name++) {
            char *bullet = g_strdup_printf("• %s\n", *name);
            gtk_text_buffer_insert(buffer, &end, bullet, -1);
            g_free(bullet);
        }
    }
    if (entry->details[0] != '\0') {
        char *error = entry->error[0] != '\0' ? g_strdup_printf("\nDetails (%s):\n", entry->error)
                                              : g_strdup("\nDetails:\n");
        gtk_text_buffer_insert(buffer, &end, error, -1);
        g_free(error);
        char *details = xd_update_format_error_details(entry->details);
        gtk_text_buffer_insert_with_tags_by_name(buffer, &end, details, -1, "fixed", nullptr);
        g_free(details);
    }
}

static void on_history_selection([[maybe_unused]] GtkTreeSelection *selection, gpointer data) {
    show_entry(data);
}

static void add_history_column(GtkWidget *view, const char *title, int column, bool expand) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    if (expand)
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    GtkTreeViewColumn *tree_column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", column, nullptr);
    gtk_tree_view_column_set_resizable(tree_column, TRUE);
    gtk_tree_view_column_set_expand(tree_column, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), tree_column);
}

void xd_wuau_show_history(wuau *applet) {
    history_window history = {0};
    char *path = xd_update_history_path();
    history.entries = xd_update_history_read(path);
    g_free(path);

    GtkWidget *dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Installation History");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(applet->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Close", GTK_RESPONSE_CLOSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 460);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       xd_wuau_label(history.entries->len > 0
                                         ? "Updates installed on this computer, and the ones "
                                           "that could not be installed or downloaded, newest "
                                           "first. Select one to see its details."
                                         : "No updates have been installed on this computer "
                                           "yet."),
                       FALSE, FALSE, 0);

    GtkListStore *store = gtk_list_store_new(HISTORY_COUNT, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_UINT);
    for (guint i = 0; i < history.entries->len; i++) {
        const xd_update_history_entry *entry = g_ptr_array_index(history.entries, i);
        char *when = format_when(entry->time);
        char *updates = describe_updates(entry);
        gtk_list_store_insert_with_values(store, nullptr, -1, HISTORY_DATE, when, HISTORY_UPDATES,
                                          updates, HISTORY_STATUS,
                                          xd_update_result_name(entry->result), HISTORY_INDEX, i,
                                          -1);
        g_free(when);
        g_free(updates);
    }
    history.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    add_history_column(history.view, "Date", HISTORY_DATE, false);
    add_history_column(history.view, "Updates", HISTORY_UPDATES, true);
    add_history_column(history.view, "Status", HISTORY_STATUS, false);
    GtkWidget *list = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(list), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(list, -1, 150);
    gtk_container_add(GTK_CONTAINER(list), history.view);
    gtk_box_pack_start(GTK_BOX(page), list, TRUE, TRUE, 0);

    history.text = text_view(nullptr);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(history.text));
    gtk_text_buffer_create_tag(buffer, "bold", "weight", PANGO_WEIGHT_BOLD, nullptr);
    gtk_box_pack_start(GTK_BOX(page), xd_wuau_frame("Details", scrolled(history.text, -1, 170)),
                       TRUE, TRUE, 0);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(history.view));
    g_signal_connect(selection, "changed", G_CALLBACK(on_history_selection), &history);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), page, TRUE, TRUE,
                       0);
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first))
        gtk_tree_selection_select_iter(selection, &first);
    else
        show_entry(&history);
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(history.view);
    gtk_dialog_run(GTK_DIALOG(dialog));
    g_signal_handlers_disconnect_by_data(selection, &history);
    gtk_widget_destroy(dialog);
    g_ptr_array_unref(history.entries);
}
