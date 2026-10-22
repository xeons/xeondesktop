/* SPDX-License-Identifier: MIT
 * xd-eventvwr: Windows XP's Event Viewer, reading the systemd journal
 * (sd-journal). XP's three logs are views of it:
 *
 *   Application  your own programs and user services
 *   Security     authentication: the auth facilities, sudo, polkit, sshd...
 *   System       the kernel and the system's services
 *
 * Read-only. Without the systemd-journal (or wheel/adm) group only your
 * own journal can be read; the window says so.
 *
 *   xd-eventvwr [application|security|system]
 */
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-journal.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "admin.h"

/* XP read whole logs; journals are much bigger. The newest events up to
 * this many are shown, looking through at most SCAN_MAX entries. */
static constexpr guint MAX_EVENTS = 5000;
static constexpr guint SCAN_MAX = 400000;

enum {
    COL_ICON,
    COL_TYPE,
    COL_DATE,
    COL_TIME,
    COL_SOURCE,
    COL_CATEGORY,
    COL_EVENT,
    COL_USER,
    COL_COMPUTER,
    COL_USEC, /* for sorting by date and time */
    COL_INDEX, /* into the loaded events */
    NCOLS
};

typedef struct event {
    guint64 usec;
    xd_event_type type;
    char *source, *category, *event_id, *user, *computer, *message, *cursor;
} event;

typedef struct filter {
    bool types[XD_EVENT_TYPE_COUNT];
    char *source; /* nullptr: all */
    char *user;
    guint64 since, until; /* 0: first or last event */
} filter;

typedef struct load {
    xd_event_log log;
    filter f;
    /* results */
    GPtrArray *events;
    bool truncated;
    bool system_unreadable;
    char *error;
} load;

typedef struct eventvwr {
    GtkWidget *window, *tree, *view, *title, *status, *note;
    GtkListStore *store;
    xd_event_log log;
    filter filters[XD_LOG_COUNT];
    GPtrArray *events; /* of the log shown */
    GCancellable *loading;
    /* the open Event Properties box */
    GtkWidget *props;
    GtkWidget *p_date, *p_time, *p_type, *p_user, *p_computer, *p_source, *p_category, *p_id;
    GtkTextBuffer *p_desc, *p_data;
    /* The journal the box reads entries' fields from, open while it is:
     * opening it reads every journal file, too slow for each Up/Down. */
    sd_journal *p_journal;
} eventvwr;

static const char *const log_names[XD_LOG_COUNT] = {
    [XD_LOG_APPLICATION] = "Application",
    [XD_LOG_SECURITY] = "Security",
    [XD_LOG_SYSTEM] = "System",
};

static void event_free(gpointer data) {
    event *record = data;
    g_free(record->source);
    g_free(record->category);
    g_free(record->event_id);
    g_free(record->user);
    g_free(record->computer);
    g_free(record->message);
    g_free(record->cursor);
    g_free(record);
}

static void filter_reset(filter *event_filter) {
    g_clear_pointer(&event_filter->source, g_free);
    g_clear_pointer(&event_filter->user, g_free);
    for (int i = 0; i < XD_EVENT_TYPE_COUNT; i++)
        event_filter->types[i] = true;
    event_filter->since = event_filter->until = 0;
}

static void filter_copy(filter *dst, const filter *src) {
    *dst = *src;
    dst->source = g_strdup(src->source);
    dst->user = g_strdup(src->user);
}

static bool filter_active(const filter *event_filter) {
    for (int i = 0; i < XD_EVENT_TYPE_COUNT; i++)
        if (!event_filter->types[i])
            return true;
    return event_filter->source != nullptr || event_filter->user != nullptr ||
           event_filter->since != 0 || event_filter->until != 0;
}

/* ---- reading the journal (in a thread) ---------------------------------------------- */

/* A field's value, or nullptr. */
static char *field(sd_journal *journal, const char *name) {
    const void *data = nullptr;
    size_t len = 0;
    if (sd_journal_get_data(journal, name, &data, &len) < 0)
        return nullptr;
    size_t prefix = strlen(name) + 1; /* "NAME=" */
    if (len < prefix)
        return nullptr;
    return g_utf8_make_valid((const char *)data + prefix, (gssize)(len - prefix));
}

static long field_long(sd_journal *journal, const char *name) {
    char *value = field(journal, name);
    gint64 number = -1;
    if (value == nullptr || !g_ascii_string_to_signed(value, 10, 0, G_MAXINT32, &number, nullptr))
        number = -1;
    g_free(value);
    return (long)number;
}

static char *user_name(GHashTable *cache, long uid) {
    if (uid < 0)
        return g_strdup("N/A");
    const char *known = g_hash_table_lookup(cache, GINT_TO_POINTER(uid));
    if (known != nullptr)
        return g_strdup(known);
    struct passwd pw, *res = nullptr;
    char buf[4096];
    char *name = getpwuid_r((uid_t)uid, &pw, buf, sizeof(buf), &res) == 0 && res != nullptr
                     ? g_strdup(pw.pw_name)
                     : g_strdup_printf("%ld", uid);
    g_hash_table_insert(cache, GINT_TO_POINTER(uid), g_strdup(name));
    return name;
}

/* Security has its own matches, so its few events don't need the whole
 * journal read; Application is your own uid's. */
static void add_matches(sd_journal *journal, xd_event_log log) {
    if (log == XD_LOG_SECURITY) {
        sd_journal_add_match(journal, "SYSLOG_FACILITY=4", 0);
        sd_journal_add_disjunction(journal);
        sd_journal_add_match(journal, "SYSLOG_FACILITY=10", 0);
        sd_journal_add_disjunction(journal);
        sd_journal_add_match(journal, "SYSLOG_FACILITY=13", 0);
        sd_journal_add_disjunction(journal);
        sd_journal_add_match(journal, "_TRANSPORT=audit", 0);
        for (const char *const *id = xd_security_identifiers; *id != nullptr; id++) {
            sd_journal_add_disjunction(journal);
            char *match = g_strdup_printf("SYSLOG_IDENTIFIER=%s", *id);
            sd_journal_add_match(journal, match, 0);
            g_free(match);
        }
    } else if (log == XD_LOG_APPLICATION) {
        char *match = g_strdup_printf("_UID=%u", (unsigned)getuid());
        sd_journal_add_match(journal, match, 0);
        g_free(match);
    }
}

/* Whether the system's journal has anything we can read. */
static bool system_readable(void) {
    sd_journal *journal = nullptr;
    if (sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM) < 0)
        return false;
    bool ok = sd_journal_seek_head(journal) >= 0 && sd_journal_next(journal) > 0;
    sd_journal_close(journal);
    return ok;
}

static void load_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        GCancellable *cancel) {
    load *log_load = data;
    log_load->events = g_ptr_array_new_with_free_func(event_free);
    sd_journal *journal = nullptr;
    int status = sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY);
    if (status < 0) {
        log_load->error =
            g_strdup_printf("The journal could not be opened: %s", g_strerror(-status));
        g_task_return_boolean(task, TRUE);
        return;
    }
    log_load->system_unreadable = !system_readable();
    add_matches(journal, log_load->log);
    if (log_load->f.until != 0)
        sd_journal_seek_realtime_usec(journal, log_load->f.until);
    else
        sd_journal_seek_tail(journal);
    GHashTable *users = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, g_free);
    guint scanned = 0;
    while (sd_journal_previous(journal) > 0) {
        if (++scanned > SCAN_MAX || log_load->events->len >= MAX_EVENTS) {
            log_load->truncated = true;
            break;
        }
        if ((scanned & 1023) == 0 && g_cancellable_is_cancelled(cancel))
            break;
        guint64 usec = 0;
        sd_journal_get_realtime_usec(journal, &usec);
        if (log_load->f.until != 0 && usec > log_load->f.until)
            continue;
        if (log_load->f.since != 0 && usec < log_load->f.since)
            break;
        char *ident = field(journal, "SYSLOG_IDENTIFIER");
        char *transport = field(journal, "_TRANSPORT");
        char *user_unit = field(journal, "_SYSTEMD_USER_UNIT");
        long uid = field_long(journal, "_UID");
        xd_event_log log = xd_event_log_of((int)field_long(journal, "SYSLOG_FACILITY"), ident,
                                           transport, user_unit != nullptr, uid);
        xd_event_type type = log == log_load->log
                                 ? xd_event_type_of((int)field_long(journal, "PRIORITY"), log)
                                 : XD_EVENT_INFORMATION;
        char *source = nullptr;
        if (log == log_load->log && log_load->f.types[type]) {
            source = ident != nullptr ? g_strdup(ident) : field(journal, "_COMM");
            if (source == nullptr)
                source = g_strdup(g_strcmp0(transport, "kernel") == 0 ? "kernel" : "Unknown");
        }
        char *user = source != nullptr ? user_name(users, uid) : nullptr;
        if (source != nullptr &&
            (log_load->f.source == nullptr || strcmp(source, log_load->f.source) == 0) &&
            (log_load->f.user == nullptr || strcmp(user, log_load->f.user) == 0)) {
            event *record = g_new0(event, 1);
            record->usec = usec;
            record->type = type;
            record->source = g_steal_pointer(&source);
            record->user = g_steal_pointer(&user);
            record->category =
                user_unit != nullptr ? g_strdup(user_unit) : field(journal, "_SYSTEMD_UNIT");
            if (record->category == nullptr)
                record->category = g_strdup("None");
            char *id = field(journal, "MESSAGE_ID");
            if (id != nullptr) {
                record->event_id = id;
            } else {
                long prio = field_long(journal, "PRIORITY");
                record->event_id = g_strdup_printf("%ld", prio >= 0 ? prio : 6);
            }
            record->computer = field(journal, "_HOSTNAME");
            record->message = field(journal, "MESSAGE");
            sd_journal_get_cursor(journal, &record->cursor);
            g_ptr_array_add(log_load->events, record);
        }
        g_free(source);
        g_free(user);
        g_free(ident);
        g_free(transport);
        g_free(user_unit);
    }
    g_hash_table_unref(users);
    sd_journal_close(journal);
    g_task_return_boolean(task, TRUE);
}

static void load_free(gpointer data) {
    load *log_load = data;
    if (log_load->events != nullptr)
        g_ptr_array_unref(log_load->events);
    g_free(log_load->f.source);
    g_free(log_load->f.user);
    g_free(log_load->error);
    g_free(log_load);
}

/* ---- the list ------------------------------------------------------------------------ */

static void props_show(eventvwr *event_viewer);

static char *grouped(guint number) {
    char *plain = g_strdup_printf("%u", number);
    GString *string = g_string_new(nullptr);
    size_t len = strlen(plain);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            g_string_append_c(string, ',');
        g_string_append_c(string, plain[i]);
    }
    g_free(plain);
    return g_string_free(string, FALSE);
}

static void loaded([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    eventvwr *event_viewer = data;
    GTask *task = G_TASK(res);
    load *log_load = g_task_get_task_data(task);
    /* A refresh replaced it. */
    if (g_task_had_error(task) || g_cancellable_is_cancelled(g_task_get_cancellable(task)))
        return;
    g_clear_object(&event_viewer->loading);
    if (event_viewer->events != nullptr)
        g_ptr_array_unref(event_viewer->events);
    event_viewer->events = g_ptr_array_ref(log_load->events);
    gtk_list_store_clear(event_viewer->store);
    for (guint i = 0; i < event_viewer->events->len; i++) {
        event *record = g_ptr_array_index(event_viewer->events, i);
        GDateTime *dt = g_date_time_new_from_unix_local_usec((gint64)record->usec);
        char *date = dt != nullptr ? g_date_time_format(dt, "%x") : g_strdup("");
        char *time = dt != nullptr ? g_date_time_format(dt, "%X") : g_strdup("");
        /* MESSAGE_IDs are 128-bit; the list shows their start. */
        char *id = strlen(record->event_id) > 8 ? g_strndup(record->event_id, 8)
                                                : g_strdup(record->event_id);
        gtk_list_store_insert_with_values(
            event_viewer->store, nullptr, -1, COL_ICON, xd_event_type_icon(record->type), COL_TYPE,
            xd_event_type_label(record->type), COL_DATE, date, COL_TIME, time, COL_SOURCE,
            record->source, COL_CATEGORY, record->category, COL_EVENT, id, COL_USER, record->user,
            COL_COMPUTER, record->computer != nullptr ? record->computer : "", COL_USEC,
            (gint64)record->usec, COL_INDEX, (int)i, -1);
        g_free(id);
        g_free(date);
        g_free(time);
        if (dt != nullptr)
            g_date_time_unref(dt);
    }
    char *count_text = grouped(event_viewer->events->len);
    char *title = g_strdup_printf(
        "<b>%s</b>   %s event%s%s", log_names[event_viewer->log], count_text,
        event_viewer->events->len == 1 ? "" : "s",
        filter_active(&event_viewer->filters[event_viewer->log]) ? " (filtered)" : "");
    gtk_label_set_markup(GTK_LABEL(event_viewer->title), title);
    g_free(title);
    char *status = log_load->error != nullptr ? g_strdup(log_load->error)
                   : log_load->truncated
                       ? g_strdup_printf("Showing the newest %s events.", count_text)
                       : g_strdup("");
    gtk_label_set_text(GTK_LABEL(event_viewer->status), status);
    g_free(status);
    g_free(count_text);
    gtk_widget_set_visible(event_viewer->note,
                           log_load->system_unreadable && event_viewer->log != XD_LOG_APPLICATION);
    GtkTreePath *first = gtk_tree_path_new_first();
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(event_viewer->view), first, nullptr, FALSE);
    gtk_tree_path_free(first);
    gdk_window_set_cursor(gtk_widget_get_window(event_viewer->window), nullptr);
}

static void refresh(eventvwr *event_viewer) {
    if (event_viewer->loading != nullptr) {
        g_cancellable_cancel(event_viewer->loading);
        g_clear_object(&event_viewer->loading);
    }
    event_viewer->loading = g_cancellable_new();
    load *log_load = g_new0(load, 1);
    log_load->log = event_viewer->log;
    filter_copy(&log_load->f, &event_viewer->filters[event_viewer->log]);
    GTask *task = g_task_new(nullptr, event_viewer->loading, loaded, event_viewer);
    g_task_set_task_data(task, log_load, load_free);
    g_task_run_in_thread(task, load_thread);
    g_object_unref(task);
    char *title =
        g_strdup_printf("<b>%s</b>   reading the journal...", log_names[event_viewer->log]);
    gtk_label_set_markup(GTK_LABEL(event_viewer->title), title);
    g_free(title);
    GdkWindow *gdk_window = gtk_widget_get_window(event_viewer->window);
    if (gdk_window != nullptr) {
        GdkCursor *busy = gdk_cursor_new_from_name(gdk_window_get_display(gdk_window), "progress");
        gdk_window_set_cursor(gdk_window, busy);
        g_clear_object(&busy);
    }
}

static event *selected_event(eventvwr *event_viewer, GtkTreeIter *it_out) {
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(event_viewer->view)), &model, &it))
        return nullptr;
    int index = -1;
    gtk_tree_model_get(model, &it, COL_INDEX, &index, -1);
    if (it_out != nullptr)
        *it_out = it;
    return event_viewer->events != nullptr && index >= 0 && (guint)index < event_viewer->events->len
               ? g_ptr_array_index(event_viewer->events, (guint)index)
               : nullptr;
}

/* ---- Event Properties ------------------------------------------------------------------ */

/* Every field of the entry, as the Data box shows it. */
static bool seek_entry(sd_journal *journal, const char *cursor) {
    return sd_journal_seek_cursor(journal, cursor) >= 0 && sd_journal_next(journal) > 0 &&
           sd_journal_test_cursor(journal, cursor) > 0;
}

static char *all_fields(eventvwr *event_viewer, const char *cursor) {
    if (cursor == nullptr)
        return g_strdup("");
    if (event_viewer->p_journal == nullptr &&
        sd_journal_open(&event_viewer->p_journal, SD_JOURNAL_LOCAL_ONLY) < 0)
        event_viewer->p_journal = nullptr;
    sd_journal *journal = event_viewer->p_journal;
    if (journal != nullptr && !seek_entry(journal, cursor)) {
        /* In a journal file newer than the handle, maybe: once more,
         * freshly opened. */
        sd_journal_close(journal);
        if (sd_journal_open(&event_viewer->p_journal, SD_JOURNAL_LOCAL_ONLY) < 0)
            event_viewer->p_journal = nullptr;
        journal = event_viewer->p_journal;
        if (journal != nullptr && !seek_entry(journal, cursor))
            journal = nullptr;
    }
    GString *string = g_string_new(nullptr);
    if (journal != nullptr) {
        const void *data = nullptr;
        size_t len = 0;
        SD_JOURNAL_FOREACH_DATA(journal, data, len) {
            char *line = g_utf8_make_valid(data, (gssize)len);
            g_string_append(string, line);
            g_string_append_c(string, '\n');
            g_free(line);
        }
    }
    return g_string_free(string, FALSE);
}

static void props_fill(eventvwr *event_viewer) {
    event *record = selected_event(event_viewer, nullptr);
    if (record == nullptr || event_viewer->props == nullptr)
        return;
    GDateTime *dt = g_date_time_new_from_unix_local_usec((gint64)record->usec);
    char *date = dt != nullptr ? g_date_time_format(dt, "%x") : g_strdup("");
    char *time = dt != nullptr ? g_date_time_format(dt, "%X") : g_strdup("");
    gtk_label_set_text(GTK_LABEL(event_viewer->p_date), date);
    gtk_label_set_text(GTK_LABEL(event_viewer->p_time), time);
    g_free(date);
    g_free(time);
    if (dt != nullptr)
        g_date_time_unref(dt);
    gtk_label_set_text(GTK_LABEL(event_viewer->p_type), xd_event_type_label(record->type));
    gtk_label_set_text(GTK_LABEL(event_viewer->p_user), record->user);
    gtk_label_set_text(GTK_LABEL(event_viewer->p_computer),
                       record->computer != nullptr ? record->computer : "");
    gtk_label_set_text(GTK_LABEL(event_viewer->p_source), record->source);
    gtk_label_set_text(GTK_LABEL(event_viewer->p_category), record->category);
    gtk_label_set_text(GTK_LABEL(event_viewer->p_id), record->event_id);
    gtk_text_buffer_set_text(event_viewer->p_desc,
                             record->message != nullptr ? record->message : "", -1);
    char *fields = all_fields(event_viewer, record->cursor);
    gtk_text_buffer_set_text(event_viewer->p_data, fields, -1);
    g_free(fields);
}

/* Up and Down: the event above or below in the list. */
static void props_move(eventvwr *event_viewer, bool up) {
    GtkTreeIter it;
    if (selected_event(event_viewer, &it) == nullptr)
        return;
    GtkTreeModel *model = GTK_TREE_MODEL(event_viewer->store);
    bool ok = up ? gtk_tree_model_iter_previous(model, &it) : gtk_tree_model_iter_next(model, &it);
    if (!ok) {
        /* XP asked "Do you want to continue from the beginning?" */
        if (xd_message_box(GTK_WINDOW(event_viewer->props), "Event Viewer", XD_MB_QUESTION,
                           XD_MB_YES_NO, FALSE,
                           up ? "You have reached the beginning of the event log. Do you want "
                                "to continue from the end?"
                              : "You have reached the end of the event log. Do you want to "
                                "continue from the beginning?") != GTK_RESPONSE_YES)
            return;
        int count = gtk_tree_model_iter_n_children(model, nullptr);
        if (count == 0 || !gtk_tree_model_iter_nth_child(model, &it, nullptr, up ? count - 1 : 0))
            return;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &it);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(event_viewer->view), path, nullptr, FALSE);
    gtk_tree_path_free(path);
    props_fill(event_viewer);
}

static void on_up([[maybe_unused]] GtkButton *button, gpointer user_data) {
    props_move(user_data, true);
}
static void on_down([[maybe_unused]] GtkButton *button, gpointer user_data) {
    props_move(user_data, false);
}

static void on_copy([[maybe_unused]] GtkButton *button, gpointer data) {
    eventvwr *event_viewer = data;
    event *record = selected_event(event_viewer, nullptr);
    if (record == nullptr)
        return;
    GString *string = g_string_new(nullptr);
    g_string_append_printf(string,
                           "Event Type:\t%s\nEvent Source:\t%s\nEvent Category:\t%s\n"
                           "Event ID:\t%s\nDate:\t\t%s\nTime:\t\t%s\nUser:\t\t%s\n"
                           "Computer:\t%s\nDescription:\n%s\n",
                           xd_event_type_label(record->type), record->source, record->category,
                           record->event_id, gtk_label_get_text(GTK_LABEL(event_viewer->p_date)),
                           gtk_label_get_text(GTK_LABEL(event_viewer->p_time)), record->user,
                           record->computer != nullptr ? record->computer : "",
                           record->message != nullptr ? record->message : "");
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), string->str,
                           (gint)string->len);
    g_string_free(string, TRUE);
}

static void on_props_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    eventvwr *event_viewer = data;
    event_viewer->props = nullptr;
    if (event_viewer->p_journal != nullptr)
        sd_journal_close(event_viewer->p_journal);
    event_viewer->p_journal = nullptr;
}

static GtkWidget *value_label(GtkWidget **out) {
    *out = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(*out), 0);
    gtk_label_set_selectable(GTK_LABEL(*out), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(*out), PANGO_ELLIPSIZE_END);
    gtk_widget_set_can_focus(*out, FALSE);
    gtk_widget_set_hexpand(*out, TRUE);
    return *out;
}

static GtkWidget *key_label(const char *text) {
    GtkWidget *widget = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    return widget;
}

static GtkWidget *text_box(GtkTextBuffer **buf, bool mono, int height) {
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), mono ? GTK_WRAP_CHAR : GTK_WRAP_WORD);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), mono);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 4);
    *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sc, -1, height);
    gtk_container_add(GTK_CONTAINER(sc), tv);
    return sc;
}

static GtkWidget *icon_button(const char *icon, const char *tip, GCallback cb, gpointer data) {
    GtkWidget *widget = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(widget, tip);
    g_signal_connect(widget, "clicked", cb, data);
    return widget;
}

static void props_open(eventvwr *event_viewer) {
    if (selected_event(event_viewer, nullptr) == nullptr)
        return;
    if (event_viewer->props != nullptr) {
        props_fill(event_viewer);
        gtk_window_present(GTK_WINDOW(event_viewer->props));
        return;
    }
    event_viewer->props =
        gtk_dialog_new_with_buttons("Event Properties", GTK_WINDOW(event_viewer->window),
                                    GTK_DIALOG_DESTROY_WITH_PARENT, "OK", GTK_RESPONSE_OK, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(event_viewer->props), 440, 500);
    g_signal_connect(event_viewer->props, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    g_signal_connect(event_viewer->props, "destroy", G_CALLBACK(on_props_destroy), event_viewer);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *widget = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(widget), 4);
    gtk_grid_set_column_spacing(GTK_GRID(widget), 8);
    gtk_widget_set_hexpand(widget, TRUE);
    struct {
        const char *key;
        GtkWidget **value;
        int col, row;
    } rows[] = {
        {"Date:", &event_viewer->p_date, 0, 0}, {"Source:", &event_viewer->p_source, 2, 0},
        {"Time:", &event_viewer->p_time, 0, 1}, {"Category:", &event_viewer->p_category, 2, 1},
        {"Type:", &event_viewer->p_type, 0, 2}, {"Event ID:", &event_viewer->p_id, 2, 2},
        {"User:", &event_viewer->p_user, 0, 3}, {"Computer:", &event_viewer->p_computer, 0, 4},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(rows); i++) {
        gtk_grid_attach(GTK_GRID(widget), key_label(rows[i].key), rows[i].col, rows[i].row, 1, 1);
        gtk_grid_attach(GTK_GRID(widget), value_label(rows[i].value), rows[i].col + 1, rows[i].row,
                        rows[i].row >= 3 ? 3 : 1, 1);
    }
    gtk_widget_set_size_request(event_viewer->p_id, 90, -1);
    gtk_box_pack_start(GTK_BOX(top), widget, TRUE, TRUE, 0);
    GtkWidget *arrows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(arrows),
                       icon_button("go-up", "Previous event", G_CALLBACK(on_up), event_viewer),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(arrows),
                       icon_button("go-down", "Next event", G_CALLBACK(on_down), event_viewer),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(arrows),
        icon_button("edit-copy", "Copy to the clipboard", G_CALLBACK(on_copy), event_viewer), FALSE,
        FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), arrows, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), key_label("_Description:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), text_box(&event_viewer->p_desc, false, 120), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), key_label("D_ata (the journal's fields):"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), text_box(&event_viewer->p_data, true, 120), TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), page, gtk_label_new("Event"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(event_viewer->props))), nb,
                       TRUE, TRUE, 0);
    props_fill(event_viewer);
    gtk_widget_show_all(event_viewer->props);
}

static void props_show(eventvwr *event_viewer) { props_open(event_viewer); }

/* ---- Filter ---------------------------------------------------------------------------- */

typedef struct filter_dlg {
    eventvwr *v;
    GtkWidget *types[XD_EVENT_TYPE_COUNT];
    GtkWidget *source, *user, *from_kind, *from, *to_kind, *to;
} filter_dlg;

/* "Events On" a date, from the entry (any format GDate takes:
 * 2026-10-17, 10/17/2026, ...); 0 if it doesn't parse. `end` takes the
 * day's last moment. */
static guint64 parse_day(const char *text, bool end) {
    GDate date;
    g_date_clear(&date, 1);
    g_date_set_parse(&date, text);
    if (!g_date_valid(&date))
        return 0;
    GDateMonth month = g_date_get_month(&date);
    GDateTime *dt = g_date_time_new_local(g_date_get_year(&date), (int)month, g_date_get_day(&date),
                                          end ? 23 : 0, end ? 59 : 0, end ? 59.999999 : 0);
    if (dt == nullptr)
        return 0;
    guint64 usec = (guint64)g_date_time_to_unix_usec(dt);
    g_date_time_unref(dt);
    return usec;
}

static char *day_text(guint64 usec) {
    GDateTime *dt = usec != 0 ? g_date_time_new_from_unix_local_usec((gint64)usec)
                              : g_date_time_new_now_local();
    char *day = g_date_time_format(dt, "%Y-%m-%d");
    g_date_time_unref(dt);
    return day;
}

static void on_kind_changed(GtkComboBox *combo, gpointer entry) {
    gtk_widget_set_sensitive(entry, gtk_combo_box_get_active(combo) == 1);
}

static void filter_dlg_show(filter_dlg *controls, const filter *event_filter) {
    for (int i = 0; i < XD_EVENT_TYPE_COUNT; i++)
        if (controls->types[i] != nullptr)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(controls->types[i]),
                                         event_filter->types[i]);
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(controls->source))),
                       event_filter->source != nullptr ? event_filter->source : "");
    gtk_entry_set_text(GTK_ENTRY(controls->user),
                       event_filter->user != nullptr ? event_filter->user : "");
    gtk_combo_box_set_active(GTK_COMBO_BOX(controls->from_kind), event_filter->since != 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(controls->to_kind), event_filter->until != 0);
    char *from = day_text(event_filter->since), *to = day_text(event_filter->until);
    gtk_entry_set_text(GTK_ENTRY(controls->from), from);
    gtk_entry_set_text(GTK_ENTRY(controls->to), to);
    g_free(from);
    g_free(to);
    gtk_widget_set_sensitive(controls->from, event_filter->since != 0);
    gtk_widget_set_sensitive(controls->to, event_filter->until != 0);
}

static GtkWidget *kind_combo(const char *all, GtkWidget *entry) {
    GtkWidget *widget = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), all);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "Events On");
    g_signal_connect(widget, "changed", G_CALLBACK(on_kind_changed), entry);
    return widget;
}

static void filter_dialog(eventvwr *event_viewer) {
    filter_dlg controls = {.v = event_viewer};
    char *title = g_strdup_printf("%s Properties - Filter", log_names[event_viewer->log]);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(event_viewer->window), GTK_DIALOG_MODAL, "_Restore Defaults", 1, "OK",
        GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    g_free(title);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);

    GtkWidget *types = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(types), 24);
    gtk_grid_set_row_spacing(GTK_GRID(types), 2);
    static const xd_event_type order[] = {XD_EVENT_INFORMATION, XD_EVENT_WARNING,
                                          XD_EVENT_ERROR, XD_EVENT_AUDIT_SUCCESS,
                                          XD_EVENT_AUDIT_FAILURE};
    bool security = event_viewer->log == XD_LOG_SECURITY;
    for (size_t i = 0; i < G_N_ELEMENTS(order); i++) {
        xd_event_type event_type = order[i];
        bool audit = event_type == XD_EVENT_AUDIT_SUCCESS || event_type == XD_EVENT_AUDIT_FAILURE;
        controls.types[event_type] =
            gtk_check_button_new_with_label(xd_event_type_label(event_type));
        /* Each log has only its own kinds of event. */
        gtk_widget_set_sensitive(controls.types[event_type], audit == security);
        gtk_grid_attach(GTK_GRID(types), controls.types[event_type], i < 3 ? 0 : 1,
                        (int)(i < 3 ? i : i - 3), 1, 1);
    }
    GtkWidget *tf = gtk_frame_new("Event types");
    gtk_container_set_border_width(GTK_CONTAINER(types), 8);
    gtk_container_add(GTK_CONTAINER(tf), types);
    gtk_box_pack_start(GTK_BOX(page), tf, FALSE, FALSE, 0);

    GtkWidget *widget = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(widget), 6);
    gtk_grid_set_column_spacing(GTK_GRID(widget), 8);
    controls.source = gtk_combo_box_text_new_with_entry();
    /* The sources in the list now. */
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *sources = g_ptr_array_new();
    for (guint i = 0; event_viewer->events != nullptr && i < event_viewer->events->len; i++) {
        event *record = g_ptr_array_index(event_viewer->events, i);
        if (g_hash_table_add(seen, record->source))
            g_ptr_array_add(sources, record->source);
    }
    g_ptr_array_sort_values(sources, (GCompareFunc)g_utf8_collate);
    for (guint i = 0; i < sources->len; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(controls.source),
                                       g_ptr_array_index(sources, i));
    g_ptr_array_unref(sources);
    g_hash_table_unref(seen);
    gtk_entry_set_placeholder_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(controls.source))), "(All)");
    controls.user = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(controls.user), "(All)");
    gtk_grid_attach(GTK_GRID(widget), key_label("Event _source:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(widget), controls.source, 1, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(widget), key_label("Us_er:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(widget), controls.user, 1, 1, 2, 1);
    controls.from = gtk_entry_new();
    controls.to = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(controls.from), 12);
    gtk_entry_set_width_chars(GTK_ENTRY(controls.to), 12);
    controls.from_kind = kind_combo("First Event", controls.from);
    controls.to_kind = kind_combo("Last Event", controls.to);
    gtk_grid_attach(GTK_GRID(widget), key_label("_From:"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(widget), controls.from_kind, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(widget), controls.from, 2, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(widget), key_label("_To:"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(widget), controls.to_kind, 1, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(widget), controls.to, 2, 3, 1, 1);
    gtk_box_pack_start(GTK_BOX(page), widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), page, TRUE, TRUE,
                       0);
    filter_dlg_show(&controls, &event_viewer->filters[event_viewer->log]);
    gtk_widget_show_all(dlg);

    int response;
    while ((response = gtk_dialog_run(GTK_DIALOG(dlg))) == 1) {
        filter defaults = {0};
        filter_reset(&defaults);
        filter_dlg_show(&controls, &defaults);
    }
    if (response == GTK_RESPONSE_OK) {
        filter *event_filter = &event_viewer->filters[event_viewer->log];
        filter_reset(event_filter);
        for (int i = 0; i < XD_EVENT_TYPE_COUNT; i++)
            event_filter->types[i] =
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(controls.types[i])) ||
                !gtk_widget_get_sensitive(controls.types[i]);
        const char *src =
            gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(controls.source))));
        const char *user = gtk_entry_get_text(GTK_ENTRY(controls.user));
        event_filter->source = src[0] != '\0' ? g_strstrip(g_strdup(src)) : nullptr;
        event_filter->user = user[0] != '\0' ? g_strstrip(g_strdup(user)) : nullptr;
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(controls.from_kind)) == 1)
            event_filter->since = parse_day(gtk_entry_get_text(GTK_ENTRY(controls.from)), false);
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(controls.to_kind)) == 1)
            event_filter->until = parse_day(gtk_entry_get_text(GTK_ENTRY(controls.to)), true);
        refresh(event_viewer);
    }
    gtk_widget_destroy(dlg);
}

/* ---- the window -------------------------------------------------------------------------- */

static void on_tree_changed(GtkTreeSelection *sel, gpointer data) {
    eventvwr *event_viewer = data;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return;
    int log = -1;
    gtk_tree_model_get(model, &it, 2, &log, -1);
    if (log < 0 || log >= XD_LOG_COUNT)
        return;
    event_viewer->log = (xd_event_log)log;
    refresh(event_viewer);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tv, [[maybe_unused]] GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer user_data) {
    props_show(user_data);
}

static void on_refresh([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    refresh(user_data);
}
static void on_properties([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    props_show(user_data);
}
static void on_filter([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    filter_dialog(user_data);
}

static void on_all_records([[maybe_unused]] GtkWidget *widget, gpointer data) {
    eventvwr *event_viewer = data;
    filter_reset(&event_viewer->filters[event_viewer->log]);
    refresh(event_viewer);
}

static void on_exit([[maybe_unused]] GtkWidget *widget, gpointer data) {
    eventvwr *event_viewer = data;
    gtk_widget_destroy(event_viewer->window);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *text, GCallback cb, gpointer data) {
    GtkWidget *widget = gtk_menu_item_new_with_mnemonic(text);
    if (cb != nullptr)
        g_signal_connect(widget, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), widget);
    return widget;
}

static GtkWidget *view_menu(eventvwr *event_viewer) {
    GtkWidget *widget = gtk_menu_new();
    menu_item(widget, "_All Records", G_CALLBACK(on_all_records), event_viewer);
    menu_item(widget, "_Filter...", G_CALLBACK(on_filter), event_viewer);
    return widget;
}

static GtkWidget *action_menu(eventvwr *event_viewer) {
    GtkWidget *widget = gtk_menu_new();
    menu_item(widget, "_Refresh", G_CALLBACK(on_refresh), event_viewer);
    gtk_menu_shell_append(GTK_MENU_SHELL(widget), gtk_separator_menu_item_new());
    menu_item(widget, "_Filter...", G_CALLBACK(on_filter), event_viewer);
    menu_item(widget, "_All Records", G_CALLBACK(on_all_records), event_viewer);
    gtk_menu_shell_append(GTK_MENU_SHELL(widget), gtk_separator_menu_item_new());
    menu_item(widget, "P_roperties", G_CALLBACK(on_properties), event_viewer);
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

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path, nullptr,
                                      nullptr, nullptr)) {
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), path, nullptr, FALSE);
        gtk_tree_path_free(path);
    }
    GtkWidget *menu = action_menu(data);
    gtk_menu_attach_to_widget(GTK_MENU(menu), widget, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data) {
    if (ev->keyval == GDK_KEY_F5) {
        on_refresh(widget, data);
        return TRUE;
    }
    return FALSE;
}

static GtkToolItem *tool(GtkWidget *bar, const char *icon, const char *tip, GCallback cb,
                         gpointer data) {
    GtkToolItem *tool_item = gtk_tool_button_new(nullptr, tip);
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(tool_item), icon);
    gtk_widget_set_tooltip_text(GTK_WIDGET(tool_item), tip);
    g_signal_connect(tool_item, "clicked", cb, data);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), tool_item, -1);
    return tool_item;
}

static GtkWidget *log_tree(eventvwr *event_viewer) {
    GtkTreeStore *ts = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    GtkTreeIter root;
    gtk_tree_store_insert_with_values(ts, &root, nullptr, -1, 0, "utilities-log-viewer", 1,
                                      "Event Viewer (Local)", 2, -1, -1);
    for (int i = 0; i < XD_LOG_COUNT; i++)
        gtk_tree_store_insert_with_values(ts, nullptr, &root, -1, 0,
                                          i == XD_LOG_SECURITY ? "security-high"
                                                               : "text-x-generic",
                                          1, log_names[i], 2, i, -1);
    event_viewer->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ts));
    g_object_unref(ts);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(event_viewer->tree), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", 0);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", 1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(event_viewer->tree), col);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(event_viewer->tree));
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), event_viewer->tree);
    gtk_widget_set_size_request(sc, 170, -1);
    return sc;
}

static GtkWidget *event_list(eventvwr *event_viewer) {
    event_viewer->store = gtk_list_store_new(
        NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_INT);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(event_viewer->store), COL_USEC,
                                         GTK_SORT_DESCENDING);
    event_viewer->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(event_viewer->store));
    g_object_unref(event_viewer->store);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(event_viewer->view), TRUE);
    static const struct {
        const char *title;
        int col, sort, width;
    } cols[] = {
        {"Type", COL_TYPE, COL_TYPE, 110},          {"Date", COL_DATE, COL_USEC, 80},
        {"Time", COL_TIME, COL_USEC, 80},           {"Source", COL_SOURCE, COL_SOURCE, 130},
        {"Category", COL_CATEGORY, COL_CATEGORY, 130}, {"Event", COL_EVENT, COL_EVENT, 70},
        {"User", COL_USER, COL_USER, 80},           {"Computer", COL_COMPUTER, COL_COMPUTER, 100},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cols); i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, cols[i].title);
        if (cols[i].col == COL_TYPE) {
            GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
            gtk_tree_view_column_pack_start(col, icon, FALSE);
            gtk_tree_view_column_add_attribute(col, icon, "icon-name", COL_ICON);
        }
        GtkCellRenderer *text = gtk_cell_renderer_text_new();
        g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        gtk_tree_view_column_pack_start(col, text, TRUE);
        gtk_tree_view_column_add_attribute(col, text, "text", cols[i].col);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, cols[i].width);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_sort_column_id(col, cols[i].sort);
        gtk_tree_view_append_column(GTK_TREE_VIEW(event_viewer->view), col);
    }
    g_signal_connect(event_viewer->view, "row-activated", G_CALLBACK(on_row_activated),
                     event_viewer);
    g_signal_connect(event_viewer->view, "button-press-event", G_CALLBACK(on_button_press),
                     event_viewer);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), event_viewer->view);
    return sc;
}

static void build(eventvwr *event_viewer) {
    event_viewer->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(event_viewer->window), "Event Viewer");
    gtk_window_set_icon_name(GTK_WINDOW(event_viewer->window), "utilities-log-viewer");
    gtk_window_set_default_size(GTK_WINDOW(event_viewer->window), 960, 580);
    g_signal_connect(event_viewer->window, "key-press-event", G_CALLBACK(on_key), event_viewer);
    g_signal_connect(event_viewer->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(event_viewer->window), vbox);

    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = menu_item(bar, "_File", nullptr, nullptr);
    GtkWidget *fm = gtk_menu_new();
    menu_item(fm, "E_xit", G_CALLBACK(on_exit), event_viewer);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fm);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item(bar, "_Action", nullptr, nullptr)),
                              action_menu(event_viewer));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item(bar, "_View", nullptr, nullptr)),
                              view_menu(event_viewer));
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);
    GtkWidget *tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb), GTK_ICON_SIZE_SMALL_TOOLBAR);
    tool(tb, "document-properties", "Properties", G_CALLBACK(on_properties), event_viewer);
    tool(tb, "view-refresh", "Refresh", G_CALLBACK(on_refresh), event_viewer);
    tool(tb, "view-filter", "Filter", G_CALLBACK(on_filter), event_viewer);
    gtk_box_pack_start(GTK_BOX(vbox), tb, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), log_tree(event_viewer), FALSE, FALSE);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    event_viewer->title = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(event_viewer->title), 0);
    gtk_widget_set_margin_start(event_viewer->title, 6);
    gtk_widget_set_margin_top(event_viewer->title, 4);
    gtk_widget_set_margin_bottom(event_viewer->title, 4);
    gtk_box_pack_start(GTK_BOX(right), event_viewer->title, FALSE, FALSE, 0);
    /* No access to the system's journal: say why some events are missing. */
    event_viewer->note = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(event_viewer->note), GTK_MESSAGE_WARNING);
    GtkWidget *note = gtk_label_new("Only your own events can be read. To see the system's, "
                                    "your account must be in the systemd-journal group (or "
                                    "wheel or adm).");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_container_add(
        GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(event_viewer->note))), note);
    gtk_widget_set_no_show_all(event_viewer->note, TRUE);
    gtk_widget_show(note);
    gtk_box_pack_start(GTK_BOX(right), event_viewer->note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), event_list(event_viewer), TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);
    event_viewer->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(event_viewer->status), 0);
    gtk_widget_set_margin_start(event_viewer->status, 6);
    gtk_widget_set_margin_top(event_viewer->status, 2);
    gtk_widget_set_margin_bottom(event_viewer->status, 2);
    gtk_box_pack_start(GTK_BOX(vbox), event_viewer->status, FALSE, FALSE, 0);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.EventViewer");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static eventvwr event_viewer;
    for (int i = 0; i < XD_LOG_COUNT; i++)
        filter_reset(&event_viewer.filters[i]);
    event_viewer.log = XD_LOG_SYSTEM;
    for (int i = 1; i < argc; i++)
        for (int log_index = 0; log_index < XD_LOG_COUNT; log_index++)
            if (g_ascii_strcasecmp(argv[i], log_names[log_index]) == 0)
                event_viewer.log = (xd_event_log)log_index;
    build(&event_viewer);
    gtk_widget_show_all(event_viewer.window);
    /* Selecting the log loads it. */
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, (int)event_viewer.log, -1);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(event_viewer.tree)), "changed",
                     G_CALLBACK(on_tree_changed), &event_viewer);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(event_viewer.tree), path, nullptr, FALSE);
    gtk_tree_path_free(path);
    gtk_main();
    return EXIT_SUCCESS;
}
