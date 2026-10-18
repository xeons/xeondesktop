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
    event *e = data;
    g_free(e->source);
    g_free(e->category);
    g_free(e->event_id);
    g_free(e->user);
    g_free(e->computer);
    g_free(e->message);
    g_free(e->cursor);
    g_free(e);
}

static void filter_reset(filter *f) {
    g_clear_pointer(&f->source, g_free);
    g_clear_pointer(&f->user, g_free);
    for (int i = 0; i < XD_EVENT_TYPE_COUNT; i++)
        f->types[i] = true;
    f->since = f->until = 0;
}

static void filter_copy(filter *dst, const filter *src) {
    *dst = *src;
    dst->source = g_strdup(src->source);
    dst->user = g_strdup(src->user);
}

static bool filter_active(const filter *f) {
    for (int i = 0; i < XD_EVENT_TYPE_COUNT; i++)
        if (!f->types[i])
            return true;
    return f->source != nullptr || f->user != nullptr || f->since != 0 || f->until != 0;
}

/* ---- reading the journal (in a thread) ---------------------------------------------- */

/* A field's value, or nullptr. */
static char *field(sd_journal *j, const char *name) {
    const void *data = nullptr;
    size_t len = 0;
    if (sd_journal_get_data(j, name, &data, &len) < 0)
        return nullptr;
    size_t prefix = strlen(name) + 1; /* "NAME=" */
    if (len < prefix)
        return nullptr;
    return g_utf8_make_valid((const char *)data + prefix, (gssize)(len - prefix));
}

static long field_long(sd_journal *j, const char *name) {
    char *v = field(j, name);
    gint64 n = -1;
    if (v == nullptr || !g_ascii_string_to_signed(v, 10, 0, G_MAXINT32, &n, nullptr))
        n = -1;
    g_free(v);
    return (long)n;
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
static void add_matches(sd_journal *j, xd_event_log log) {
    if (log == XD_LOG_SECURITY) {
        sd_journal_add_match(j, "SYSLOG_FACILITY=4", 0);
        sd_journal_add_disjunction(j);
        sd_journal_add_match(j, "SYSLOG_FACILITY=10", 0);
        sd_journal_add_disjunction(j);
        sd_journal_add_match(j, "SYSLOG_FACILITY=13", 0);
        sd_journal_add_disjunction(j);
        sd_journal_add_match(j, "_TRANSPORT=audit", 0);
        for (const char *const *id = xd_security_identifiers; *id != nullptr; id++) {
            sd_journal_add_disjunction(j);
            char *m = g_strdup_printf("SYSLOG_IDENTIFIER=%s", *id);
            sd_journal_add_match(j, m, 0);
            g_free(m);
        }
    } else if (log == XD_LOG_APPLICATION) {
        char *m = g_strdup_printf("_UID=%u", (unsigned)getuid());
        sd_journal_add_match(j, m, 0);
        g_free(m);
    }
}

/* Whether the system's journal has anything we can read. */
static bool system_readable(void) {
    sd_journal *j = nullptr;
    if (sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM) < 0)
        return false;
    bool ok = sd_journal_seek_head(j) >= 0 && sd_journal_next(j) > 0;
    sd_journal_close(j);
    return ok;
}

static void load_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        GCancellable *cancel) {
    load *l = data;
    l->events = g_ptr_array_new_with_free_func(event_free);
    sd_journal *j = nullptr;
    int r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
    if (r < 0) {
        l->error = g_strdup_printf("The journal could not be opened: %s", g_strerror(-r));
        g_task_return_boolean(task, TRUE);
        return;
    }
    l->system_unreadable = !system_readable();
    add_matches(j, l->log);
    if (l->f.until != 0)
        sd_journal_seek_realtime_usec(j, l->f.until);
    else
        sd_journal_seek_tail(j);
    GHashTable *users = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, g_free);
    guint scanned = 0;
    while (sd_journal_previous(j) > 0) {
        if (++scanned > SCAN_MAX || l->events->len >= MAX_EVENTS) {
            l->truncated = true;
            break;
        }
        if ((scanned & 1023) == 0 && g_cancellable_is_cancelled(cancel))
            break;
        guint64 usec = 0;
        sd_journal_get_realtime_usec(j, &usec);
        if (l->f.until != 0 && usec > l->f.until)
            continue;
        if (l->f.since != 0 && usec < l->f.since)
            break;
        char *ident = field(j, "SYSLOG_IDENTIFIER");
        char *transport = field(j, "_TRANSPORT");
        char *user_unit = field(j, "_SYSTEMD_USER_UNIT");
        long uid = field_long(j, "_UID");
        xd_event_log log = xd_event_log_of((int)field_long(j, "SYSLOG_FACILITY"), ident,
                                           transport, user_unit != nullptr, uid);
        xd_event_type type = log == l->log
                                 ? xd_event_type_of((int)field_long(j, "PRIORITY"), log)
                                 : XD_EVENT_INFORMATION;
        char *source = nullptr;
        if (log == l->log && l->f.types[type]) {
            source = ident != nullptr ? g_strdup(ident) : field(j, "_COMM");
            if (source == nullptr)
                source = g_strdup(g_strcmp0(transport, "kernel") == 0 ? "kernel" : "Unknown");
        }
        char *user = source != nullptr ? user_name(users, uid) : nullptr;
        if (source != nullptr && (l->f.source == nullptr || strcmp(source, l->f.source) == 0) &&
            (l->f.user == nullptr || strcmp(user, l->f.user) == 0)) {
            event *e = g_new0(event, 1);
            e->usec = usec;
            e->type = type;
            e->source = g_steal_pointer(&source);
            e->user = g_steal_pointer(&user);
            e->category = user_unit != nullptr ? g_strdup(user_unit) : field(j, "_SYSTEMD_UNIT");
            if (e->category == nullptr)
                e->category = g_strdup("None");
            char *id = field(j, "MESSAGE_ID");
            if (id != nullptr) {
                e->event_id = id;
            } else {
                long prio = field_long(j, "PRIORITY");
                e->event_id = g_strdup_printf("%ld", prio >= 0 ? prio : 6);
            }
            e->computer = field(j, "_HOSTNAME");
            e->message = field(j, "MESSAGE");
            sd_journal_get_cursor(j, &e->cursor);
            g_ptr_array_add(l->events, e);
        }
        g_free(source);
        g_free(user);
        g_free(ident);
        g_free(transport);
        g_free(user_unit);
    }
    g_hash_table_unref(users);
    sd_journal_close(j);
    g_task_return_boolean(task, TRUE);
}

static void load_free(gpointer data) {
    load *l = data;
    if (l->events != nullptr)
        g_ptr_array_unref(l->events);
    g_free(l->f.source);
    g_free(l->f.user);
    g_free(l->error);
    g_free(l);
}

/* ---- the list ------------------------------------------------------------------------ */

static void props_show(eventvwr *v);

static char *grouped(guint n) {
    char *plain = g_strdup_printf("%u", n);
    GString *s = g_string_new(nullptr);
    size_t len = strlen(plain);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            g_string_append_c(s, ',');
        g_string_append_c(s, plain[i]);
    }
    g_free(plain);
    return g_string_free(s, FALSE);
}

static void loaded([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    eventvwr *v = data;
    GTask *task = G_TASK(res);
    load *l = g_task_get_task_data(task);
    /* A refresh replaced it. */
    if (g_task_had_error(task) || g_cancellable_is_cancelled(g_task_get_cancellable(task)))
        return;
    g_clear_object(&v->loading);
    if (v->events != nullptr)
        g_ptr_array_unref(v->events);
    v->events = g_ptr_array_ref(l->events);
    gtk_list_store_clear(v->store);
    for (guint i = 0; i < v->events->len; i++) {
        event *e = g_ptr_array_index(v->events, i);
        GDateTime *dt = g_date_time_new_from_unix_local_usec((gint64)e->usec);
        char *date = dt != nullptr ? g_date_time_format(dt, "%x") : g_strdup("");
        char *time = dt != nullptr ? g_date_time_format(dt, "%X") : g_strdup("");
        /* MESSAGE_IDs are 128-bit; the list shows their start. */
        char *id = strlen(e->event_id) > 8 ? g_strndup(e->event_id, 8) : g_strdup(e->event_id);
        gtk_list_store_insert_with_values(
            v->store, nullptr, -1, COL_ICON, xd_event_type_icon(e->type), COL_TYPE,
            xd_event_type_label(e->type), COL_DATE, date, COL_TIME, time, COL_SOURCE, e->source,
            COL_CATEGORY, e->category, COL_EVENT, id, COL_USER, e->user, COL_COMPUTER,
            e->computer != nullptr ? e->computer : "", COL_USEC, (gint64)e->usec, COL_INDEX,
            (int)i, -1);
        g_free(id);
        g_free(date);
        g_free(time);
        if (dt != nullptr)
            g_date_time_unref(dt);
    }
    char *n = grouped(v->events->len);
    char *title = g_strdup_printf("<b>%s</b>   %s event%s%s", log_names[v->log], n,
                                  v->events->len == 1 ? "" : "s",
                                  filter_active(&v->filters[v->log]) ? " (filtered)" : "");
    gtk_label_set_markup(GTK_LABEL(v->title), title);
    g_free(title);
    char *status = l->error != nullptr ? g_strdup(l->error)
                   : l->truncated      ? g_strdup_printf("Showing the newest %s events.", n)
                                       : g_strdup("");
    gtk_label_set_text(GTK_LABEL(v->status), status);
    g_free(status);
    g_free(n);
    gtk_widget_set_visible(v->note, l->system_unreadable && v->log != XD_LOG_APPLICATION);
    GtkTreePath *first = gtk_tree_path_new_first();
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(v->view), first, nullptr, FALSE);
    gtk_tree_path_free(first);
    gdk_window_set_cursor(gtk_widget_get_window(v->window), nullptr);
}

static void refresh(eventvwr *v) {
    if (v->loading != nullptr) {
        g_cancellable_cancel(v->loading);
        g_clear_object(&v->loading);
    }
    v->loading = g_cancellable_new();
    load *l = g_new0(load, 1);
    l->log = v->log;
    filter_copy(&l->f, &v->filters[v->log]);
    GTask *task = g_task_new(nullptr, v->loading, loaded, v);
    g_task_set_task_data(task, l, load_free);
    g_task_run_in_thread(task, load_thread);
    g_object_unref(task);
    char *title = g_strdup_printf("<b>%s</b>   reading the journal...", log_names[v->log]);
    gtk_label_set_markup(GTK_LABEL(v->title), title);
    g_free(title);
    GdkWindow *w = gtk_widget_get_window(v->window);
    if (w != nullptr) {
        GdkCursor *busy = gdk_cursor_new_from_name(gdk_window_get_display(w), "progress");
        gdk_window_set_cursor(w, busy);
        g_clear_object(&busy);
    }
}

static event *selected_event(eventvwr *v, GtkTreeIter *it_out) {
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(v->view)), &m,
                                         &it))
        return nullptr;
    int index = -1;
    gtk_tree_model_get(m, &it, COL_INDEX, &index, -1);
    if (it_out != nullptr)
        *it_out = it;
    return v->events != nullptr && index >= 0 && (guint)index < v->events->len
               ? g_ptr_array_index(v->events, (guint)index)
               : nullptr;
}

/* ---- Event Properties ------------------------------------------------------------------ */

/* Every field of the entry, as the Data box shows it. */
static bool seek_entry(sd_journal *j, const char *cursor) {
    return sd_journal_seek_cursor(j, cursor) >= 0 && sd_journal_next(j) > 0 &&
           sd_journal_test_cursor(j, cursor) > 0;
}

static char *all_fields(eventvwr *v, const char *cursor) {
    if (cursor == nullptr)
        return g_strdup("");
    if (v->p_journal == nullptr && sd_journal_open(&v->p_journal, SD_JOURNAL_LOCAL_ONLY) < 0)
        v->p_journal = nullptr;
    sd_journal *j = v->p_journal;
    if (j != nullptr && !seek_entry(j, cursor)) {
        /* In a journal file newer than the handle, maybe: once more,
         * freshly opened. */
        sd_journal_close(j);
        if (sd_journal_open(&v->p_journal, SD_JOURNAL_LOCAL_ONLY) < 0)
            v->p_journal = nullptr;
        j = v->p_journal;
        if (j != nullptr && !seek_entry(j, cursor))
            j = nullptr;
    }
    GString *s = g_string_new(nullptr);
    if (j != nullptr) {
        const void *data = nullptr;
        size_t len = 0;
        SD_JOURNAL_FOREACH_DATA(j, data, len) {
            char *line = g_utf8_make_valid(data, (gssize)len);
            g_string_append(s, line);
            g_string_append_c(s, '\n');
            g_free(line);
        }
    }
    return g_string_free(s, FALSE);
}

static void props_fill(eventvwr *v) {
    event *e = selected_event(v, nullptr);
    if (e == nullptr || v->props == nullptr)
        return;
    GDateTime *dt = g_date_time_new_from_unix_local_usec((gint64)e->usec);
    char *date = dt != nullptr ? g_date_time_format(dt, "%x") : g_strdup("");
    char *time = dt != nullptr ? g_date_time_format(dt, "%X") : g_strdup("");
    gtk_label_set_text(GTK_LABEL(v->p_date), date);
    gtk_label_set_text(GTK_LABEL(v->p_time), time);
    g_free(date);
    g_free(time);
    if (dt != nullptr)
        g_date_time_unref(dt);
    gtk_label_set_text(GTK_LABEL(v->p_type), xd_event_type_label(e->type));
    gtk_label_set_text(GTK_LABEL(v->p_user), e->user);
    gtk_label_set_text(GTK_LABEL(v->p_computer), e->computer != nullptr ? e->computer : "");
    gtk_label_set_text(GTK_LABEL(v->p_source), e->source);
    gtk_label_set_text(GTK_LABEL(v->p_category), e->category);
    gtk_label_set_text(GTK_LABEL(v->p_id), e->event_id);
    gtk_text_buffer_set_text(v->p_desc, e->message != nullptr ? e->message : "", -1);
    char *fields = all_fields(v, e->cursor);
    gtk_text_buffer_set_text(v->p_data, fields, -1);
    g_free(fields);
}

/* Up and Down: the event above or below in the list. */
static void props_move(eventvwr *v, bool up) {
    GtkTreeIter it;
    if (selected_event(v, &it) == nullptr)
        return;
    GtkTreeModel *m = GTK_TREE_MODEL(v->store);
    bool ok = up ? gtk_tree_model_iter_previous(m, &it) : gtk_tree_model_iter_next(m, &it);
    if (!ok) {
        /* XP asked "Do you want to continue from the beginning?" */
        if (xd_message_box(GTK_WINDOW(v->props), "Event Viewer", XD_MB_QUESTION, XD_MB_YES_NO,
                           FALSE,
                           up ? "You have reached the beginning of the event log. Do you want "
                                "to continue from the end?"
                              : "You have reached the end of the event log. Do you want to "
                                "continue from the beginning?") != GTK_RESPONSE_YES)
            return;
        int n = gtk_tree_model_iter_n_children(m, nullptr);
        if (n == 0 || !gtk_tree_model_iter_nth_child(m, &it, nullptr, up ? n - 1 : 0))
            return;
    }
    GtkTreePath *p = gtk_tree_model_get_path(m, &it);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(v->view), p, nullptr, FALSE);
    gtk_tree_path_free(p);
    props_fill(v);
}

static void on_up([[maybe_unused]] GtkButton *b, gpointer v) { props_move(v, true); }
static void on_down([[maybe_unused]] GtkButton *b, gpointer v) { props_move(v, false); }

static void on_copy([[maybe_unused]] GtkButton *b, gpointer data) {
    eventvwr *v = data;
    event *e = selected_event(v, nullptr);
    if (e == nullptr)
        return;
    GString *s = g_string_new(nullptr);
    g_string_append_printf(s, "Event Type:\t%s\nEvent Source:\t%s\nEvent Category:\t%s\n"
                              "Event ID:\t%s\nDate:\t\t%s\nTime:\t\t%s\nUser:\t\t%s\n"
                              "Computer:\t%s\nDescription:\n%s\n",
                           xd_event_type_label(e->type), e->source, e->category, e->event_id,
                           gtk_label_get_text(GTK_LABEL(v->p_date)),
                           gtk_label_get_text(GTK_LABEL(v->p_time)), e->user,
                           e->computer != nullptr ? e->computer : "",
                           e->message != nullptr ? e->message : "");
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s->str, (gint)s->len);
    g_string_free(s, TRUE);
}

static void on_props_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    eventvwr *v = data;
    v->props = nullptr;
    if (v->p_journal != nullptr)
        sd_journal_close(v->p_journal);
    v->p_journal = nullptr;
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
    GtkWidget *l = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
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
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static void props_open(eventvwr *v) {
    if (selected_event(v, nullptr) == nullptr)
        return;
    if (v->props != nullptr) {
        props_fill(v);
        gtk_window_present(GTK_WINDOW(v->props));
        return;
    }
    v->props = gtk_dialog_new_with_buttons("Event Properties", GTK_WINDOW(v->window),
                                           GTK_DIALOG_DESTROY_WITH_PARENT, "OK", GTK_RESPONSE_OK,
                                           nullptr);
    gtk_window_set_default_size(GTK_WINDOW(v->props), 440, 500);
    g_signal_connect(v->props, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    g_signal_connect(v->props, "destroy", G_CALLBACK(on_props_destroy), v);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 4);
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    gtk_widget_set_hexpand(g, TRUE);
    struct {
        const char *key;
        GtkWidget **value;
        int col, row;
    } rows[] = {
        {"Date:", &v->p_date, 0, 0},       {"Source:", &v->p_source, 2, 0},
        {"Time:", &v->p_time, 0, 1},       {"Category:", &v->p_category, 2, 1},
        {"Type:", &v->p_type, 0, 2},       {"Event ID:", &v->p_id, 2, 2},
        {"User:", &v->p_user, 0, 3},       {"Computer:", &v->p_computer, 0, 4},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(rows); i++) {
        gtk_grid_attach(GTK_GRID(g), key_label(rows[i].key), rows[i].col, rows[i].row, 1, 1);
        gtk_grid_attach(GTK_GRID(g), value_label(rows[i].value), rows[i].col + 1, rows[i].row,
                        rows[i].row >= 3 ? 3 : 1, 1);
    }
    gtk_widget_set_size_request(v->p_id, 90, -1);
    gtk_box_pack_start(GTK_BOX(top), g, TRUE, TRUE, 0);
    GtkWidget *arrows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(arrows), icon_button("go-up", "Previous event", G_CALLBACK(on_up),
                                                    v),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(arrows), icon_button("go-down", "Next event",
                                                    G_CALLBACK(on_down), v),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(arrows), icon_button("edit-copy", "Copy to the clipboard",
                                                    G_CALLBACK(on_copy), v),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), arrows, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), key_label("_Description:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), text_box(&v->p_desc, false, 120), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), key_label("D_ata (the journal's fields):"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), text_box(&v->p_data, true, 120), TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), page, gtk_label_new("Event"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(v->props))), nb, TRUE,
                       TRUE, 0);
    props_fill(v);
    gtk_widget_show_all(v->props);
}

static void props_show(eventvwr *v) { props_open(v); }

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
    GDate d;
    g_date_clear(&d, 1);
    g_date_set_parse(&d, text);
    if (!g_date_valid(&d))
        return 0;
    GDateMonth month = g_date_get_month(&d);
    GDateTime *dt = g_date_time_new_local(g_date_get_year(&d), (int)month,
                                          g_date_get_day(&d), end ? 23 : 0, end ? 59 : 0,
                                          end ? 59.999999 : 0);
    if (dt == nullptr)
        return 0;
    guint64 usec = (guint64)g_date_time_to_unix_usec(dt);
    g_date_time_unref(dt);
    return usec;
}

static char *day_text(guint64 usec) {
    GDateTime *dt = usec != 0 ? g_date_time_new_from_unix_local_usec((gint64)usec)
                              : g_date_time_new_now_local();
    char *s = g_date_time_format(dt, "%Y-%m-%d");
    g_date_time_unref(dt);
    return s;
}

static void on_kind_changed(GtkComboBox *c, gpointer entry) {
    gtk_widget_set_sensitive(entry, gtk_combo_box_get_active(c) == 1);
}

static void filter_dlg_show(filter_dlg *d, const filter *f) {
    for (int i = 0; i < XD_EVENT_TYPE_COUNT; i++)
        if (d->types[i] != nullptr)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->types[i]), f->types[i]);
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->source))),
                       f->source != nullptr ? f->source : "");
    gtk_entry_set_text(GTK_ENTRY(d->user), f->user != nullptr ? f->user : "");
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->from_kind), f->since != 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->to_kind), f->until != 0);
    char *from = day_text(f->since), *to = day_text(f->until);
    gtk_entry_set_text(GTK_ENTRY(d->from), from);
    gtk_entry_set_text(GTK_ENTRY(d->to), to);
    g_free(from);
    g_free(to);
    gtk_widget_set_sensitive(d->from, f->since != 0);
    gtk_widget_set_sensitive(d->to, f->until != 0);
}

static GtkWidget *kind_combo(const char *all, GtkWidget *entry) {
    GtkWidget *c = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), all);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), "Events On");
    g_signal_connect(c, "changed", G_CALLBACK(on_kind_changed), entry);
    return c;
}

static void filter_dialog(eventvwr *v) {
    filter_dlg d = {.v = v};
    char *title = g_strdup_printf("%s Properties - Filter", log_names[v->log]);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, GTK_WINDOW(v->window), GTK_DIALOG_MODAL,
                                                 "_Restore Defaults", 1, "OK", GTK_RESPONSE_OK,
                                                 "Cancel", GTK_RESPONSE_CANCEL, nullptr);
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
    bool security = v->log == XD_LOG_SECURITY;
    for (size_t i = 0; i < G_N_ELEMENTS(order); i++) {
        xd_event_type t = order[i];
        bool audit = t == XD_EVENT_AUDIT_SUCCESS || t == XD_EVENT_AUDIT_FAILURE;
        d.types[t] = gtk_check_button_new_with_label(xd_event_type_label(t));
        /* Each log has only its own kinds of event. */
        gtk_widget_set_sensitive(d.types[t], audit == security);
        gtk_grid_attach(GTK_GRID(types), d.types[t], i < 3 ? 0 : 1, (int)(i < 3 ? i : i - 3), 1,
                        1);
    }
    GtkWidget *tf = gtk_frame_new("Event types");
    gtk_container_set_border_width(GTK_CONTAINER(types), 8);
    gtk_container_add(GTK_CONTAINER(tf), types);
    gtk_box_pack_start(GTK_BOX(page), tf, FALSE, FALSE, 0);

    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    d.source = gtk_combo_box_text_new_with_entry();
    /* The sources in the list now. */
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *sources = g_ptr_array_new();
    for (guint i = 0; v->events != nullptr && i < v->events->len; i++) {
        event *e = g_ptr_array_index(v->events, i);
        if (g_hash_table_add(seen, e->source))
            g_ptr_array_add(sources, e->source);
    }
    g_ptr_array_sort_values(sources, (GCompareFunc)g_utf8_collate);
    for (guint i = 0; i < sources->len; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d.source), g_ptr_array_index(sources, i));
    g_ptr_array_unref(sources);
    g_hash_table_unref(seen);
    gtk_entry_set_placeholder_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d.source))), "(All)");
    d.user = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d.user), "(All)");
    gtk_grid_attach(GTK_GRID(g), key_label("Event _source:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), d.source, 1, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(g), key_label("Us_er:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(g), d.user, 1, 1, 2, 1);
    d.from = gtk_entry_new();
    d.to = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(d.from), 12);
    gtk_entry_set_width_chars(GTK_ENTRY(d.to), 12);
    d.from_kind = kind_combo("First Event", d.from);
    d.to_kind = kind_combo("Last Event", d.to);
    gtk_grid_attach(GTK_GRID(g), key_label("_From:"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(g), d.from_kind, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(g), d.from, 2, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(g), key_label("_To:"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(g), d.to_kind, 1, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(g), d.to, 2, 3, 1, 1);
    gtk_box_pack_start(GTK_BOX(page), g, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), page, TRUE, TRUE,
                       0);
    filter_dlg_show(&d, &v->filters[v->log]);
    gtk_widget_show_all(dlg);

    int r;
    while ((r = gtk_dialog_run(GTK_DIALOG(dlg))) == 1) {
        filter defaults = {0};
        filter_reset(&defaults);
        filter_dlg_show(&d, &defaults);
    }
    if (r == GTK_RESPONSE_OK) {
        filter *f = &v->filters[v->log];
        filter_reset(f);
        for (int i = 0; i < XD_EVENT_TYPE_COUNT; i++)
            f->types[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d.types[i])) ||
                          !gtk_widget_get_sensitive(d.types[i]);
        const char *src = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d.source))));
        const char *user = gtk_entry_get_text(GTK_ENTRY(d.user));
        f->source = src[0] != '\0' ? g_strstrip(g_strdup(src)) : nullptr;
        f->user = user[0] != '\0' ? g_strstrip(g_strdup(user)) : nullptr;
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(d.from_kind)) == 1)
            f->since = parse_day(gtk_entry_get_text(GTK_ENTRY(d.from)), false);
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(d.to_kind)) == 1)
            f->until = parse_day(gtk_entry_get_text(GTK_ENTRY(d.to)), true);
        refresh(v);
    }
    gtk_widget_destroy(dlg);
}

/* ---- the window -------------------------------------------------------------------------- */

static void on_tree_changed(GtkTreeSelection *sel, gpointer data) {
    eventvwr *v = data;
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it))
        return;
    int log = -1;
    gtk_tree_model_get(m, &it, 2, &log, -1);
    if (log < 0 || log >= XD_LOG_COUNT)
        return;
    v->log = (xd_event_log)log;
    refresh(v);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tv, [[maybe_unused]] GtkTreePath *p,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer v) {
    props_show(v);
}

static void on_refresh([[maybe_unused]] GtkWidget *w, gpointer v) { refresh(v); }
static void on_properties([[maybe_unused]] GtkWidget *w, gpointer v) { props_show(v); }
static void on_filter([[maybe_unused]] GtkWidget *w, gpointer v) { filter_dialog(v); }

static void on_all_records([[maybe_unused]] GtkWidget *w, gpointer data) {
    eventvwr *v = data;
    filter_reset(&v->filters[v->log]);
    refresh(v);
}

static void on_exit([[maybe_unused]] GtkWidget *w, gpointer data) {
    eventvwr *v = data;
    gtk_widget_destroy(v->window);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *text, GCallback cb, gpointer data) {
    GtkWidget *i = gtk_menu_item_new_with_mnemonic(text);
    if (cb != nullptr)
        g_signal_connect(i, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
    return i;
}

static GtkWidget *view_menu(eventvwr *v) {
    GtkWidget *m = gtk_menu_new();
    menu_item(m, "_All Records", G_CALLBACK(on_all_records), v);
    menu_item(m, "_Filter...", G_CALLBACK(on_filter), v);
    return m;
}

static GtkWidget *action_menu(eventvwr *v) {
    GtkWidget *m = gtk_menu_new();
    menu_item(m, "_Refresh", G_CALLBACK(on_refresh), v);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    menu_item(m, "_Filter...", G_CALLBACK(on_filter), v);
    menu_item(m, "_All Records", G_CALLBACK(on_all_records), v);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    menu_item(m, "P_roperties", G_CALLBACK(on_properties), v);
    return m;
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

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, nullptr,
                                      nullptr, nullptr)) {
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(w), path, nullptr, FALSE);
        gtk_tree_path_free(path);
    }
    GtkWidget *menu = action_menu(data);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    if (ev->keyval == GDK_KEY_F5) {
        on_refresh(w, data);
        return TRUE;
    }
    return FALSE;
}

static GtkToolItem *tool(GtkWidget *bar, const char *icon, const char *tip, GCallback cb,
                         gpointer data) {
    GtkToolItem *t = gtk_tool_button_new(nullptr, tip);
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(t), icon);
    gtk_widget_set_tooltip_text(GTK_WIDGET(t), tip);
    g_signal_connect(t, "clicked", cb, data);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), t, -1);
    return t;
}

static GtkWidget *log_tree(eventvwr *v) {
    GtkTreeStore *ts = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    GtkTreeIter root;
    gtk_tree_store_insert_with_values(ts, &root, nullptr, -1, 0, "utilities-log-viewer", 1,
                                      "Event Viewer (Local)", 2, -1, -1);
    for (int i = 0; i < XD_LOG_COUNT; i++)
        gtk_tree_store_insert_with_values(ts, nullptr, &root, -1, 0,
                                          i == XD_LOG_SECURITY ? "security-high"
                                                               : "text-x-generic",
                                          1, log_names[i], 2, i, -1);
    v->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ts));
    g_object_unref(ts);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(v->tree), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", 0);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", 1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(v->tree), col);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(v->tree));
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), v->tree);
    gtk_widget_set_size_request(sc, 170, -1);
    return sc;
}

static GtkWidget *event_list(eventvwr *v) {
    v->store = gtk_list_store_new(NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_INT);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(v->store), COL_USEC,
                                         GTK_SORT_DESCENDING);
    v->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(v->store));
    g_object_unref(v->store);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(v->view), TRUE);
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
        gtk_tree_view_append_column(GTK_TREE_VIEW(v->view), col);
    }
    g_signal_connect(v->view, "row-activated", G_CALLBACK(on_row_activated), v);
    g_signal_connect(v->view, "button-press-event", G_CALLBACK(on_button_press), v);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), v->view);
    return sc;
}

static void build(eventvwr *v) {
    v->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(v->window), "Event Viewer");
    gtk_window_set_icon_name(GTK_WINDOW(v->window), "utilities-log-viewer");
    gtk_window_set_default_size(GTK_WINDOW(v->window), 960, 580);
    g_signal_connect(v->window, "key-press-event", G_CALLBACK(on_key), v);
    g_signal_connect(v->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(v->window), vbox);

    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = menu_item(bar, "_File", nullptr, nullptr);
    GtkWidget *fm = gtk_menu_new();
    menu_item(fm, "E_xit", G_CALLBACK(on_exit), v);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fm);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item(bar, "_Action", nullptr, nullptr)),
                              action_menu(v));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item(bar, "_View", nullptr, nullptr)),
                              view_menu(v));
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);
    GtkWidget *tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb), GTK_ICON_SIZE_SMALL_TOOLBAR);
    tool(tb, "document-properties", "Properties", G_CALLBACK(on_properties), v);
    tool(tb, "view-refresh", "Refresh", G_CALLBACK(on_refresh), v);
    tool(tb, "view-filter", "Filter", G_CALLBACK(on_filter), v);
    gtk_box_pack_start(GTK_BOX(vbox), tb, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), log_tree(v), FALSE, FALSE);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    v->title = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v->title), 0);
    gtk_widget_set_margin_start(v->title, 6);
    gtk_widget_set_margin_top(v->title, 4);
    gtk_widget_set_margin_bottom(v->title, 4);
    gtk_box_pack_start(GTK_BOX(right), v->title, FALSE, FALSE, 0);
    /* No access to the system's journal: say why some events are missing. */
    v->note = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(v->note), GTK_MESSAGE_WARNING);
    GtkWidget *note = gtk_label_new("Only your own events can be read. To see the system's, "
                                    "your account must be in the systemd-journal group (or "
                                    "wheel or adm).");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(v->note))), note);
    gtk_widget_set_no_show_all(v->note, TRUE);
    gtk_widget_show(note);
    gtk_box_pack_start(GTK_BOX(right), v->note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), event_list(v), TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);
    v->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v->status), 0);
    gtk_widget_set_margin_start(v->status, 6);
    gtk_widget_set_margin_top(v->status, 2);
    gtk_widget_set_margin_bottom(v->status, 2);
    gtk_box_pack_start(GTK_BOX(vbox), v->status, FALSE, FALSE, 0);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.EventViewer");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static eventvwr v;
    for (int i = 0; i < XD_LOG_COUNT; i++)
        filter_reset(&v.filters[i]);
    v.log = XD_LOG_SYSTEM;
    for (int i = 1; i < argc; i++)
        for (int l = 0; l < XD_LOG_COUNT; l++)
            if (g_ascii_strcasecmp(argv[i], log_names[l]) == 0)
                v.log = (xd_event_log)l;
    build(&v);
    gtk_widget_show_all(v.window);
    /* Selecting the log loads it. */
    GtkTreePath *p = gtk_tree_path_new_from_indices(0, (int)v.log, -1);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(v.tree)), "changed",
                     G_CALLBACK(on_tree_changed), &v);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(v.tree), p, nullptr, FALSE);
    gtk_tree_path_free(p);
    gtk_main();
    return EXIT_SUCCESS;
}
