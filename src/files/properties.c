/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "../gtkapp/gtkapp.h"
#include "properties.h"

/* ---- formatting ------------------------------------------------------------------ */

static void append_grouped(GString *out, guint64 v) {
    char digits[32];
    int n = g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, v);
    for (int i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0)
            g_string_append_c(out, ',');
        g_string_append_c(out, digits[i]);
    }
}

/* Three significant digits, truncated like Windows: 1.23 MB, 12.3 MB. */
static void append_short_size(GString *out, guint64 bytes) {
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    if (bytes < 1000) {
        append_grouped(out, bytes);
        g_string_append(out, " bytes");
        return;
    }
    double v = (double)bytes;
    size_t u = 0;
    while (v >= 1000 && u + 1 < G_N_ELEMENTS(units)) {
        v /= 1024;
        u++;
    }
    if (v < 10)
        g_string_append_printf(out, "%.2f %s", floor(v * 100) / 100, units[u]);
    else if (v < 100)
        g_string_append_printf(out, "%.1f %s", floor(v * 10) / 10, units[u]);
    else
        g_string_append_printf(out, "%.0f %s", floor(v), units[u]);
}

char *xd_format_size_long(guint64 bytes) {
    GString *s = g_string_new(nullptr);
    append_short_size(s, bytes);
    g_string_append(s, " (");
    append_grouped(s, bytes);
    g_string_append(s, " bytes)");
    return g_string_free(s, FALSE);
}

/* "Friday, October 15, 2026, 8:48:03 PM" */
static char *format_time(GDateTime *utc) {
    if (utc == nullptr)
        return g_strdup("Unavailable");
    GDateTime *local = g_date_time_to_local(utc);
    char *s = g_date_time_format(local, "%A, %B %-d, %Y, %-I:%M:%S %p");
    g_date_time_unref(local);
    return s != nullptr ? s : g_strdup("Unavailable");
}

/* ---- the files shown --------------------------------------------------------------- */

static const char INFO_ATTRS[] = "standard::*,time::*,unix::*,owner::*,access::*";
static const char COUNT_ATTRS[] =
    "standard::type,standard::size,standard::allocated-size,unix::inode,unix::device,"
    "unix::nlink,unix::mode";

typedef struct entry {
    GFile *file;
    GFileInfo *info;
    gboolean is_dir, is_link, has_unix;
    guint32 mode; /* permission and special bits (07777) */
    guint32 uid, gid;
} entry;

static void entry_free(gpointer p) {
    entry *e = p;
    g_clear_object(&e->file);
    g_clear_object(&e->info);
    g_free(e);
}

static gboolean entry_load(entry *e, GError **err) {
    GFileInfo *info =
        g_file_query_info(e->file, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, err);
    if (info == nullptr)
        return FALSE;
    g_clear_object(&e->info);
    e->info = info;
    GFileType type = g_file_info_get_file_type(info);
    e->is_link = type == G_FILE_TYPE_SYMBOLIC_LINK;
    e->is_dir = type == G_FILE_TYPE_DIRECTORY;
    e->has_unix = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    e->mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) & 07777u;
    e->uid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
    e->gid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID);
    return TRUE;
}

static guint64 info_size(GFileInfo *info) {
    goffset s = g_file_info_get_size(info);
    return s > 0 ? (guint64)s : 0;
}

/* ---- folder size counter (background thread) ------------------------------------- */

typedef struct counter {
    GMutex lock;
    GCancellable *cancel;
    GPtrArray *roots; /* GFile*, folders to walk */
    guint64 bytes, disk, files, folders;
    gboolean done;
} counter;

static void counter_clear(gpointer p) {
    counter *c = p;
    g_mutex_clear(&c->lock);
    g_clear_object(&c->cancel);
    g_ptr_array_unref(c->roots);
}

/* Walks the folders breadth-first (no recursion, so deep trees can't
 * exhaust the stack), without following symbolic links. Files with several
 * hard links are counted once. */
static gpointer count_thread(gpointer data) {
    counter *c = data;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GQueue queue = G_QUEUE_INIT;
    for (guint i = 0; i < c->roots->len; i++)
        g_queue_push_tail(&queue, g_object_ref(g_ptr_array_index(c->roots, i)));
    GFile *dir;
    while ((dir = g_queue_pop_head(&queue)) != nullptr) {
        GFileEnumerator *en =
            g_cancellable_is_cancelled(c->cancel)
                ? nullptr
                : g_file_enumerate_children(dir, COUNT_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            c->cancel, nullptr);
        g_object_unref(dir);
        if (en == nullptr)
            continue;
        GFileInfo *info;
        GFile *child;
        while (g_file_enumerator_iterate(en, &info, &child, c->cancel, nullptr) &&
               info != nullptr) {
            if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
                g_queue_push_tail(&queue, g_object_ref(child));
                g_mutex_lock(&c->lock);
                c->folders++;
                g_mutex_unlock(&c->lock);
                continue;
            }
            gboolean counted = FALSE;
            if (g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_NLINK) > 1) {
                char *key = g_strdup_printf(
                    "%u:%" G_GUINT64_FORMAT,
                    g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE),
                    g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE));
                counted = !g_hash_table_add(seen, key);
            }
            g_mutex_lock(&c->lock);
            c->files++;
            if (!counted) {
                c->bytes += info_size(info);
                c->disk +=
                    g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE);
            }
            g_mutex_unlock(&c->lock);
        }
        g_object_unref(en);
    }
    g_queue_clear_full(&queue, g_object_unref);
    g_hash_table_unref(seen);
    g_mutex_lock(&c->lock);
    c->done = TRUE;
    g_mutex_unlock(&c->lock);
    g_atomic_rc_box_release_full(c, counter_clear);
    return nullptr;
}

/* ---- dialog state ---------------------------------------------------------------- */

enum { NBITS = 12 };
static const guint32 perm_bits[NBITS] = {
    S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP,
    S_IROTH, S_IWOTH, S_IXOTH, S_ISUID, S_ISGID, S_ISVTX,
};
enum { BIT_NONE, BIT_ALL, BIT_MIXED };

/* Linux file attributes offered under Advanced... */
static const struct {
    int flag;
    const char *label;
} attr_flags[] = {
    {FS_IMMUTABLE_FL, "_Immutable: can't be changed, renamed or deleted (needs root)"},
    {FS_APPEND_FL, "_Append only: data can only be added (needs root)"},
    {FS_NODUMP_FL, "E_xclude from backups (dump)"},
    {FS_NOATIME_FL, "Don't update the _access time"},
    {FS_COMPR_FL, "_Compress contents to save disk space"},
    {FS_NOCOW_FL, "No copy-on-_write (Btrfs)"},
};

typedef struct props {
    GtkWidget *window, *apply_button, *ok_button;
    GPtrArray *entries; /* entry* */
    gboolean syncing;   /* setting widgets from code */
    gboolean can_chmod; /* we own every file (or are root) */
    gboolean close_after_apply;

    /* General */
    GtkWidget *name;
    char *orig_name;
    GtkWidget *size, *disk, *contains, *readonly, *hidden, *advanced, *opens_with;
    counter *count;
    guint count_timer;
    guint64 base_bytes, base_disk, base_files; /* files selected directly */

    /* Linux attributes (single file only) */
    gboolean attrs_ok;
    int attrs_have, attrs_want;

    /* Permissions */
    GtkWidget *bits[NBITS];
    int bit_init[NBITS];
    GtkWidget *octal, *symbolic, *group, *recursive;
    gint64 group_init; /* -1 when the files' groups differ */
} props;

static entry *first(props *p) { return g_ptr_array_index(p->entries, 0); }
static gboolean single(props *p) { return p->entries->len == 1; }

static void mark_dirty(props *p) {
    if (!p->syncing)
        gtk_widget_set_sensitive(p->apply_button, TRUE);
}

static void set_check(GtkWidget *check, int state) {
    gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(check), state == BIT_MIXED);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), state == BIT_ALL);
}

/* BIT_ALL/NONE/MIXED of a check box. */
static int check_state(GtkWidget *check) {
    if (gtk_toggle_button_get_inconsistent(GTK_TOGGLE_BUTTON(check)))
        return BIT_MIXED;
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)) ? BIT_ALL : BIT_NONE;
}

/* Which permission bits to force on, and which to force off. */
static void wanted_bits(props *p, guint32 *set, guint32 *clear, gboolean changed_only) {
    *set = 0;
    *clear = 0;
    for (int i = 0; i < NBITS; i++) {
        int s = check_state(p->bits[i]);
        if (s == BIT_MIXED || (changed_only && s == p->bit_init[i]))
            continue;
        if (s == BIT_ALL)
            *set |= perm_bits[i];
        else
            *clear |= perm_bits[i];
    }
}

/* ---- General tab ------------------------------------------------------------------ */

static GtkWidget *value_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_widget_set_can_focus(l, FALSE);
    return l;
}

/* Adds "Title:  value" on `*row` and returns the value label. */
static GtkWidget *add_row(GtkWidget *grid, int *row, const char *title, const char *value) {
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_yalign(GTK_LABEL(t), 0);
    GtkWidget *v = value_label(value != nullptr ? value : "");
    gtk_grid_attach(GTK_GRID(grid), t, 0, *row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, *row, 2, 1);
    (*row)++;
    return v;
}

static void add_separator(GtkWidget *grid, int *row) {
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(s, 4);
    gtk_widget_set_margin_bottom(s, 4);
    gtk_grid_attach(GTK_GRID(grid), s, 0, (*row)++, 3, 1);
}

static void update_sizes(props *p) {
    guint64 bytes = p->base_bytes, disk = p->base_disk, files = p->base_files, folders = 0;
    gboolean counting = FALSE;
    if (p->count != nullptr) {
        g_mutex_lock(&p->count->lock);
        bytes += p->count->bytes;
        disk += p->count->disk;
        files += p->count->files;
        folders = p->count->folders;
        counting = !p->count->done;
        g_mutex_unlock(&p->count->lock);
    }
    char *s = xd_format_size_long(bytes);
    gtk_label_set_text(GTK_LABEL(p->size), s);
    g_free(s);
    s = xd_format_size_long(disk);
    gtk_label_set_text(GTK_LABEL(p->disk), s);
    g_free(s);
    if (p->contains != nullptr) {
        GString *c = g_string_new(nullptr);
        append_grouped(c, files);
        g_string_append(c, files == 1 ? " File, " : " Files, ");
        append_grouped(c, folders);
        g_string_append(c, folders == 1 ? " Folder" : " Folders");
        if (counting)
            g_string_append(c, " (counting…)");
        gtk_label_set_text(GTK_LABEL(p->contains), c->str);
        g_string_free(c, TRUE);
    }
}

static gboolean on_count_tick(gpointer data) {
    props *p = data;
    update_sizes(p);
    g_mutex_lock(&p->count->lock);
    gboolean done = p->count->done;
    g_mutex_unlock(&p->count->lock);
    if (done) {
        p->count_timer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void start_count(props *p) {
    p->base_bytes = p->base_disk = p->base_files = 0;
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < p->entries->len; i++) {
        entry *e = g_ptr_array_index(p->entries, i);
        if (e->is_dir) {
            g_ptr_array_add(roots, g_object_ref(e->file));
        } else {
            p->base_files++;
            p->base_bytes += info_size(e->info);
            p->base_disk +=
                g_file_info_get_attribute_uint64(e->info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE);
        }
    }
    if (single(p) && !first(p)->is_dir)
        p->base_files = 0; /* "Contains" isn't shown for one file */
    if (roots->len == 0) {
        g_ptr_array_unref(roots);
        update_sizes(p);
        return;
    }
    p->count = g_atomic_rc_box_new0(counter);
    g_mutex_init(&p->count->lock);
    p->count->cancel = g_cancellable_new();
    p->count->roots = roots;
    g_thread_unref(g_thread_new("xd-count", count_thread, g_atomic_rc_box_acquire(p->count)));
    p->count_timer = g_timeout_add(150, on_count_tick, p);
    update_sizes(p);
}

static void stop_count(props *p) {
    if (p->count_timer != 0) {
        g_source_remove(p->count_timer);
        p->count_timer = 0;
    }
    if (p->count != nullptr) {
        g_cancellable_cancel(p->count->cancel);
        g_atomic_rc_box_release_full(p->count, counter_clear);
        p->count = nullptr;
    }
}

static const char *display_name(entry *e) { return g_file_info_get_display_name(e->info); }

static gboolean name_hidden(const char *name) { return name != nullptr && name[0] == '.'; }

/* Read-only = no write permission for anyone; follows the permission
 * check boxes. */
static void sync_readonly(props *p) {
    int w[3] = {check_state(p->bits[1]), check_state(p->bits[4]), check_state(p->bits[7])};
    int state = BIT_ALL;
    for (int i = 0; i < 3; i++) {
        if (w[i] == BIT_MIXED)
            state = BIT_MIXED;
        else if (w[i] == BIT_ALL && state != BIT_MIXED)
            state = BIT_NONE;
    }
    gboolean was = p->syncing;
    p->syncing = TRUE;
    set_check(p->readonly, state);
    p->syncing = was;
}

static void sync_permission_text(props *p, gboolean rewrite_octal);

static void on_readonly(GtkToggleButton *b, gpointer data) {
    props *p = data;
    if (p->syncing)
        return;
    gtk_toggle_button_set_inconsistent(b, FALSE);
    gboolean ro = gtk_toggle_button_get_active(b);
    p->syncing = TRUE;
    if (ro) {
        set_check(p->bits[1], BIT_NONE);
        set_check(p->bits[4], BIT_NONE);
        set_check(p->bits[7], BIT_NONE);
    } else {
        set_check(p->bits[1], BIT_ALL);
    }
    p->syncing = FALSE;
    sync_permission_text(p, TRUE);
    mark_dirty(p);
}

/* Hidden = the name starts with a dot; with one file, the name box shows
 * the result straight away. */
static void on_hidden(GtkToggleButton *b, gpointer data) {
    props *p = data;
    if (p->syncing)
        return;
    gtk_toggle_button_set_inconsistent(b, FALSE);
    if (single(p)) {
        const char *cur = gtk_entry_get_text(GTK_ENTRY(p->name));
        const char *bare = cur;
        while (*bare == '.')
            bare++;
        char *next = gtk_toggle_button_get_active(b) ? g_strconcat(".", bare, nullptr)
                                                     : g_strdup(bare);
        p->syncing = TRUE;
        gtk_entry_set_text(GTK_ENTRY(p->name), next);
        p->syncing = FALSE;
        g_free(next);
    }
    mark_dirty(p);
}

static void on_name_changed([[maybe_unused]] GtkEditable *ed, gpointer data) {
    props *p = data;
    if (p->syncing)
        return;
    gboolean was = p->syncing;
    p->syncing = TRUE;
    set_check(p->hidden,
              name_hidden(gtk_entry_get_text(GTK_ENTRY(p->name))) ? BIT_ALL : BIT_NONE);
    p->syncing = was;
    mark_dirty(p);
}

static void update_opens_with(props *p) {
    const char *type = g_file_info_get_content_type(first(p)->info);
    GAppInfo *app = type != nullptr ? g_app_info_get_default_for_type(type, FALSE) : nullptr;
    gtk_label_set_text(GTK_LABEL(p->opens_with),
                       app != nullptr ? g_app_info_get_display_name(app) : "(none)");
    g_clear_object(&app);
}

static void on_change_app([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    const char *type = g_file_info_get_content_type(first(p)->info);
    GtkWidget *dlg = gtk_app_chooser_dialog_new(GTK_WINDOW(p->window), GTK_DIALOG_MODAL,
                                                first(p)->file);
    gtk_window_set_title(GTK_WINDOW(dlg), "Open With");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        GAppInfo *app = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dlg));
        GError *err = nullptr;
        if (app != nullptr && !g_app_info_set_as_default_for_type(app, type, &err)) {
            g_warning("cannot set default application: %s", err->message);
            g_clear_error(&err);
        }
        g_clear_object(&app);
        update_opens_with(p);
    }
    gtk_widget_destroy(dlg);
}

static gboolean read_attr_flags(GFile *file, int *flags) {
    char *path = g_file_get_path(file);
    if (path == nullptr)
        return FALSE;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
    g_free(path);
    if (fd < 0)
        return FALSE;
    gboolean ok = ioctl(fd, FS_IOC_GETFLAGS, flags) == 0;
    close(fd);
    return ok;
}

static gboolean write_attr_flags(GFile *file, int flags, GError **err) {
    char *path = g_file_get_path(file);
    int fd = path != nullptr ? open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY)
                             : -1;
    int saved = errno;
    if (fd >= 0) {
        if (ioctl(fd, FS_IOC_SETFLAGS, &flags) != 0)
            saved = errno;
        else
            saved = 0;
        close(fd);
    }
    g_free(path);
    if (saved != 0 || fd < 0) {
        GIOErrorEnum code = g_io_error_from_errno(saved);
        g_set_error(err, G_IO_ERROR, (gint)code,
                    "Cannot change the file attributes: %s", g_strerror(saved));
        return FALSE;
    }
    return TRUE;
}

static void on_advanced([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Advanced Attributes", GTK_WINDOW(p->window),
                                                 GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK,
                                                 "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    GtkWidget *intro = gtk_label_new("Choose the settings you want for this file system object.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 4);
    GtkWidget *frame = gtk_frame_new("Linux file attributes");
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 8);
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    GtkWidget *checks[G_N_ELEMENTS(attr_flags)];
    for (size_t i = 0; i < G_N_ELEMENTS(attr_flags); i++) {
        checks[i] = gtk_check_button_new_with_mnemonic(attr_flags[i].label);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checks[i]),
                                     (p->attrs_want & attr_flags[i].flag) != 0);
        gtk_box_pack_start(GTK_BOX(inner), checks[i], FALSE, FALSE, 0);
    }
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        int want = p->attrs_want;
        for (size_t i = 0; i < G_N_ELEMENTS(attr_flags); i++) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks[i])))
                want |= attr_flags[i].flag;
            else
                want &= ~attr_flags[i].flag;
        }
        if (want != p->attrs_want) {
            p->attrs_want = want;
            mark_dirty(p);
        }
    }
    gtk_widget_destroy(dlg);
}

static GIcon *multi_icon(void) {
    static const char *const names[] = {"document-multiple", "emblem-documents", "text-x-generic",
                                        nullptr};
    return g_themed_icon_new_from_names((char **)(uintptr_t)names, -1);
}

static GtkWidget *build_general(props *p) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    int row = 0;
    entry *e = first(p);

    GIcon *gicon = single(p) ? g_object_ref(g_file_info_get_icon(e->info)) : multi_icon();
    GtkWidget *icon = gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_DND);
    g_object_unref(gicon);
    gtk_widget_set_halign(icon, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), icon, 0, row, 1, 1);
    p->name = gtk_entry_new();
    gtk_widget_set_hexpand(p->name, TRUE);
    gtk_widget_set_valign(p->name, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), p->name, 1, row++, 2, 1);

    guint files = 0, folders = 0;
    gboolean same_type = TRUE, same_parent = TRUE;
    GFile *parent = g_file_get_parent(e->file);
    for (guint i = 0; i < p->entries->len; i++) {
        entry *x = g_ptr_array_index(p->entries, i);
        if (x->is_dir)
            folders++;
        else
            files++;
        if (g_strcmp0(g_file_info_get_content_type(x->info),
                      g_file_info_get_content_type(e->info)) != 0)
            same_type = FALSE;
        GFile *xp = g_file_get_parent(x->file);
        if (xp == nullptr || parent == nullptr || !g_file_equal(xp, parent))
            same_parent = FALSE;
        g_clear_object(&xp);
    }

    if (single(p)) {
        p->orig_name = g_strdup(display_name(e));
        gtk_entry_set_text(GTK_ENTRY(p->name), p->orig_name);
        g_signal_connect(p->name, "changed", G_CALLBACK(on_name_changed), p);
        if (parent == nullptr)
            gtk_widget_set_sensitive(p->name, FALSE); /* the root folder */
    } else {
        char *s = g_strdup_printf("%u File%s, %u Folder%s", files, files == 1 ? "" : "s",
                                  folders, folders == 1 ? "" : "s");
        gtk_entry_set_text(GTK_ENTRY(p->name), s);
        gtk_widget_set_sensitive(p->name, FALSE);
        g_free(s);
    }
    add_separator(grid, &row);

    /* Type and default application */
    const char *ctype = g_file_info_get_content_type(e->info);
    char *desc;
    if (!same_type)
        desc = g_strdup("Multiple Types");
    else if (e->is_dir)
        desc = g_strdup(single(p) ? "File Folder" : "All of type File Folder");
    else if (e->is_link)
        desc = g_strdup("Symbolic Link");
    else {
        char *d = ctype != nullptr ? g_content_type_get_description(ctype) : g_strdup("File");
        desc = single(p) ? g_strdup(d) : g_strdup_printf("All of type %s", d);
        g_free(d);
    }
    add_row(grid, &row, "Type of file:", desc);
    g_free(desc);
    if (single(p) && e->is_link) {
        add_row(grid, &row, "Target:", g_file_info_get_symlink_target(e->info));
    }
    if (single(p) && !e->is_dir && !e->is_link) {
        GtkWidget *t = gtk_label_new("Opens with:");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_grid_attach(GTK_GRID(grid), t, 0, row++, 1, 1);
        p->opens_with = value_label("");
        gtk_grid_attach(GTK_GRID(grid), p->opens_with, 1, row - 1, 1, 1);
        GtkWidget *change = gtk_button_new_with_mnemonic("C_hange...");
        g_signal_connect(change, "clicked", G_CALLBACK(on_change_app), p);
        gtk_grid_attach(GTK_GRID(grid), change, 2, row - 1, 1, 1);
        update_opens_with(p);
    }
    add_separator(grid, &row);

    /* Location and sizes */
    char *loc;
    if (parent == nullptr)
        loc = g_strdup("");
    else if (!same_parent)
        loc = g_strdup("Various Folders");
    else {
        char *pn = g_file_get_parse_name(parent);
        loc = single(p) ? pn : g_strdup_printf("All in %s", pn);
        if (!single(p))
            g_free(pn);
    }
    add_row(grid, &row, "Location:", loc);
    g_free(loc);
    p->size = add_row(grid, &row, "Size:", "");
    p->disk = add_row(grid, &row, "Size on disk:", "");
    if (folders > 0)
        p->contains = add_row(grid, &row, "Contains:", "");
    add_separator(grid, &row);

    /* Times (single item) */
    if (single(p)) {
        char *t = format_time(g_file_info_get_creation_date_time(e->info));
        add_row(grid, &row, "Created:", t);
        g_free(t);
        t = format_time(g_file_info_get_modification_date_time(e->info));
        add_row(grid, &row, "Modified:", t);
        g_free(t);
        t = format_time(g_file_info_get_access_date_time(e->info));
        add_row(grid, &row, "Accessed:", t);
        g_free(t);
        add_separator(grid, &row);
    }

    /* Attributes */
    GtkWidget *attr_title = gtk_label_new("Attributes:");
    gtk_label_set_xalign(GTK_LABEL(attr_title), 0);
    gtk_grid_attach(GTK_GRID(grid), attr_title, 0, row, 1, 1);
    GtkWidget *attr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    p->readonly = gtk_check_button_new_with_mnemonic("_Read-only");
    p->hidden = gtk_check_button_new_with_mnemonic("H_idden");
    gtk_box_pack_start(GTK_BOX(attr_box), p->readonly, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(attr_box), p->hidden, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), attr_box, 1, row, 1, 1);
    p->advanced = gtk_button_new_with_mnemonic("A_dvanced...");
    gtk_grid_attach(GTK_GRID(grid), p->advanced, 2, row++, 1, 1);
    g_signal_connect(p->readonly, "toggled", G_CALLBACK(on_readonly), p);
    g_signal_connect(p->hidden, "toggled", G_CALLBACK(on_hidden), p);
    g_signal_connect(p->advanced, "clicked", G_CALLBACK(on_advanced), p);

    int hidden = BIT_NONE;
    for (guint i = 0; i < p->entries->len; i++) {
        gboolean h = name_hidden(display_name(g_ptr_array_index(p->entries, i)));
        int s = h ? BIT_ALL : BIT_NONE;
        hidden = i == 0 ? s : hidden == s ? hidden : BIT_MIXED;
    }
    set_check(p->hidden, hidden);
    if (parent == nullptr)
        gtk_widget_set_sensitive(p->hidden, FALSE);
    if (!p->can_chmod)
        gtk_widget_set_sensitive(p->readonly, FALSE);

    p->attrs_ok = single(p) && !e->is_link && read_attr_flags(e->file, &p->attrs_have);
    p->attrs_want = p->attrs_have;
    if (!p->attrs_ok) {
        gtk_widget_set_sensitive(p->advanced, FALSE);
        gtk_widget_set_tooltip_text(p->advanced,
                                    single(p) ? "This file system doesn't support attributes"
                                              : "Select a single file to change attributes");
    }
    g_clear_object(&parent);
    return grid;
}

/* ---- Permissions tab ---------------------------------------------------------- */

/* Updates the numeric and symbolic modes (and Read-only) from the check
 * boxes; the numeric box is left alone while the user types in it. */
static void sync_permission_text(props *p, gboolean rewrite_octal) {
    gboolean mixed = FALSE;
    guint32 mode = 0;
    for (int i = 0; i < NBITS; i++) {
        int s = check_state(p->bits[i]);
        if (s == BIT_MIXED)
            mixed = TRUE;
        else if (s == BIT_ALL)
            mode |= perm_bits[i];
    }
    gboolean was = p->syncing;
    p->syncing = TRUE;
    if (mixed) {
        if (rewrite_octal)
            gtk_entry_set_text(GTK_ENTRY(p->octal), "");
        gtk_entry_set_placeholder_text(GTK_ENTRY(p->octal), "mixed");
        gtk_label_set_text(GTK_LABEL(p->symbolic), "(differs between the selected items)");
    } else {
        char oct[8];
        g_snprintf(oct, sizeof(oct), "%04o", mode);
        if (rewrite_octal)
            gtk_entry_set_text(GTK_ENTRY(p->octal), oct);
        entry *e = first(p);
        char sym[11] = "----------";
        sym[0] = e->is_dir ? 'd' : e->is_link ? 'l' : '-';
        static const char rwx[] = "rwx";
        for (int i = 0; i < 9; i++) {
            if (mode & perm_bits[i])
                sym[1 + i] = rwx[i % 3];
        }
        if (mode & S_ISUID)
            sym[3] = (mode & S_IXUSR) ? 's' : 'S';
        if (mode & S_ISGID)
            sym[6] = (mode & S_IXGRP) ? 's' : 'S';
        if (mode & S_ISVTX)
            sym[9] = (mode & S_IXOTH) ? 't' : 'T';
        gtk_label_set_text(GTK_LABEL(p->symbolic), single(p) ? sym : sym + 1);
    }
    p->syncing = was;
    sync_readonly(p);
}

static void on_bit_toggled(GtkToggleButton *b, gpointer data) {
    props *p = data;
    if (p->syncing)
        return;
    gtk_toggle_button_set_inconsistent(b, FALSE);
    sync_permission_text(p, TRUE);
    mark_dirty(p);
}

/* Typing a mode like 755 or 2775 sets every check box. */
static void on_octal_changed(GtkEditable *ed, gpointer data) {
    props *p = data;
    if (p->syncing)
        return;
    const char *t = gtk_entry_get_text(GTK_ENTRY(ed));
    size_t n = strlen(t);
    if (n < 3 || n > 4)
        return;
    guint32 mode = 0;
    for (size_t i = 0; i < n; i++) {
        if (t[i] < '0' || t[i] > '7')
            return;
        mode = mode * 8 + (guint32)(t[i] - '0');
    }
    p->syncing = TRUE;
    for (int i = 0; i < NBITS; i++)
        set_check(p->bits[i], (mode & perm_bits[i]) ? BIT_ALL : BIT_NONE);
    p->syncing = FALSE;
    sync_permission_text(p, FALSE);
    mark_dirty(p);
}

static void on_octal_done(GtkWidget *w, [[maybe_unused]] GdkEvent *ev, gpointer data) {
    (void)w;
    sync_permission_text(data, TRUE);
}

static void on_octal_activate(GtkEntry *box, gpointer data) {
    on_octal_done(GTK_WIDGET(box), nullptr, data);
}

static void on_dirty_widget([[maybe_unused]] GtkWidget *w, gpointer data) { mark_dirty(data); }

static char *group_name(guint32 gid) {
    long max = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t len = max > 0 ? (size_t)max : 16384;
    char *buf = g_malloc(len);
    struct group grp, *res = nullptr;
    char *name = getgrgid_r(gid, &grp, buf, len, &res) == 0 && res != nullptr
                     ? g_strdup(res->gr_name)
                     : g_strdup_printf("%u", gid);
    g_free(buf);
    return name;
}

static void fill_groups(props *p) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->group));
    GArray *gids = g_array_new(FALSE, FALSE, sizeof(guint32));
    if (p->group_init >= 0) {
        guint32 g = (guint32)p->group_init;
        g_array_append_val(gids, g);
    }
    if (p->can_chmod) {
        int n = getgroups(0, nullptr);
        if (n > 0) {
            gid_t *list = g_new(gid_t, (gsize)n);
            n = getgroups(n, list);
            for (int i = 0; i < n; i++) {
                guint32 g = list[i];
                gboolean dup = FALSE;
                for (guint k = 0; k < gids->len; k++)
                    dup = dup || g_array_index(gids, guint32, k) == g;
                if (!dup)
                    g_array_append_val(gids, g);
            }
            g_free(list);
        }
    }
    for (guint k = 0; k < gids->len; k++) {
        guint32 g = g_array_index(gids, guint32, k);
        char id[16];
        g_snprintf(id, sizeof(id), "%u", g);
        char *name = group_name(g);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->group), id, name);
        g_free(name);
    }
    if (p->group_init >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->group), 0);
    g_array_unref(gids);
}

/* Sets the check boxes, owner and group from the files. */
static void load_permissions(props *p) {
    p->syncing = TRUE;
    for (int i = 0; i < NBITS; i++) {
        int state = BIT_NONE;
        for (guint k = 0; k < p->entries->len; k++) {
            entry *e = g_ptr_array_index(p->entries, k);
            int s = (e->mode & perm_bits[i]) ? BIT_ALL : BIT_NONE;
            state = k == 0 ? s : state == s ? state : BIT_MIXED;
        }
        p->bit_init[i] = state;
        set_check(p->bits[i], state);
    }
    p->group_init = first(p)->gid;
    for (guint k = 1; k < p->entries->len; k++) {
        entry *e = g_ptr_array_index(p->entries, k);
        if ((gint64)e->gid != p->group_init)
            p->group_init = -1;
    }
    fill_groups(p);
    p->syncing = FALSE;
    sync_permission_text(p, TRUE);
}

static GtkWidget *build_permissions(props *p) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    entry *e = first(p);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    int row = 0;
    gboolean same_owner = TRUE;
    for (guint k = 1; k < p->entries->len; k++)
        same_owner = same_owner && ((entry *)g_ptr_array_index(p->entries, k))->uid == e->uid;
    char *owner;
    if (!same_owner) {
        owner = g_strdup("(differs between the selected items)");
    } else {
        const char *user = g_file_info_get_attribute_string(e->info, G_FILE_ATTRIBUTE_OWNER_USER);
        const char *real =
            g_file_info_get_attribute_string(e->info, G_FILE_ATTRIBUTE_OWNER_USER_REAL);
        owner = real != nullptr && *real != '\0' && g_strcmp0(real, user) != 0
                    ? g_strdup_printf("%s (%s)", user, real)
                    : g_strdup(user != nullptr ? user : "unknown");
    }
    add_row(grid, &row, "Owner:", owner);
    g_free(owner);
    GtkWidget *gl = gtk_label_new_with_mnemonic("_Group:");
    gtk_label_set_xalign(GTK_LABEL(gl), 0);
    p->group = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(gl), p->group);
    gtk_widget_set_halign(p->group, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), gl, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->group, 1, row++, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    /* Read / Write / Execute for Owner, Group, Others */
    GtkWidget *frame = gtk_frame_new("Permissions");
    GtkWidget *bits = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bits), 4);
    gtk_grid_set_column_spacing(GTK_GRID(bits), 24);
    gtk_container_set_border_width(GTK_CONTAINER(bits), 8);
    gtk_container_add(GTK_CONTAINER(frame), bits);
    static const char *const cols[] = {"Read", "Write", "Execute"};
    static const char *const rows[] = {"Owner", "Group", "Others"};
    for (int c = 0; c < 3; c++) {
        GtkWidget *l = gtk_label_new(cols[c]);
        gtk_grid_attach(GTK_GRID(bits), l, c + 1, 0, 1, 1);
    }
    for (int r = 0; r < 3; r++) {
        GtkWidget *l = gtk_label_new(rows[r]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_grid_attach(GTK_GRID(bits), l, 0, r + 1, 1, 1);
        for (int c = 0; c < 3; c++) {
            GtkWidget *cb = gtk_check_button_new();
            gtk_widget_set_halign(cb, GTK_ALIGN_CENTER);
            char *tip = g_strdup_printf("%s: %s", rows[r], cols[c]);
            gtk_widget_set_tooltip_text(cb, tip);
            g_free(tip);
            p->bits[r * 3 + c] = cb;
            gtk_grid_attach(GTK_GRID(bits), cb, c + 1, r + 1, 1, 1);
        }
    }
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);

    GtkWidget *sframe = gtk_frame_new("Special");
    GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(sbox), 8);
    gtk_container_add(GTK_CONTAINER(sframe), sbox);
    static const char *const special[] = {"Set _user ID", "Set group _ID", "_Sticky"};
    static const char *const special_tip[] = {
        "Programs run with the owner's rights",
        "Programs run with the group's rights; new items in a folder get its group",
        "In a folder, only an item's owner can rename or delete it"};
    for (int i = 0; i < 3; i++) {
        p->bits[9 + i] = gtk_check_button_new_with_mnemonic(special[i]);
        gtk_widget_set_tooltip_text(p->bits[9 + i], special_tip[i]);
        gtk_box_pack_start(GTK_BOX(sbox), p->bits[9 + i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), sframe, FALSE, FALSE, 0);

    GtkWidget *mbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ml = gtk_label_new_with_mnemonic("_Numeric:");
    p->octal = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(p->octal), 6);
    gtk_entry_set_max_length(GTK_ENTRY(p->octal), 4);
    gtk_label_set_mnemonic_widget(GTK_LABEL(ml), p->octal);
    p->symbolic = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(p->symbolic), TRUE);
    gtk_widget_set_can_focus(p->symbolic, FALSE);
    gtk_box_pack_start(GTK_BOX(mbox), ml, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mbox), p->octal, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mbox), gtk_label_new("Symbolic:"), FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(mbox), p->symbolic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), mbox, FALSE, FALSE, 0);

    gboolean any_dir = FALSE;
    for (guint k = 0; k < p->entries->len; k++)
        any_dir = any_dir || ((entry *)g_ptr_array_index(p->entries, k))->is_dir;
    if (any_dir) {
        GtkWidget *hint = gtk_label_new("For folders, Read lists the contents, Write adds and "
                                        "removes items, and Execute opens the folder.");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(hint), 50);
        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
        p->recursive =
            gtk_check_button_new_with_mnemonic("_Apply changes to all files and folders inside");
        gtk_widget_set_tooltip_text(p->recursive,
                                    "Only the permissions you change are applied. Files only "
                                    "get Execute if they are already executable.");
        g_signal_connect(p->recursive, "toggled", G_CALLBACK(on_dirty_widget), p);
        gtk_box_pack_start(GTK_BOX(box), p->recursive, FALSE, FALSE, 0);
    }

    if (!p->can_chmod) {
        GtkWidget *note = gtk_label_new("Only the owner can change these permissions.");
        gtk_label_set_xalign(GTK_LABEL(note), 0);
        gtk_box_pack_end(GTK_BOX(box), note, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(frame, FALSE);
        gtk_widget_set_sensitive(sframe, FALSE);
        gtk_widget_set_sensitive(mbox, FALSE);
        gtk_widget_set_sensitive(p->group, FALSE);
        if (p->recursive != nullptr)
            gtk_widget_set_sensitive(p->recursive, FALSE);
    }

    for (int i = 0; i < NBITS; i++)
        g_signal_connect(p->bits[i], "toggled", G_CALLBACK(on_bit_toggled), p);
    g_signal_connect(p->octal, "changed", G_CALLBACK(on_octal_changed), p);
    g_signal_connect(p->octal, "focus-out-event", G_CALLBACK(on_octal_done), p);
    g_signal_connect(p->octal, "activate", G_CALLBACK(on_octal_activate), p);
    g_signal_connect(p->group, "changed", G_CALLBACK(on_dirty_widget), p);
    load_permissions(p);
    return box;
}

/* ---- Details tab ------------------------------------------------------------------ */

static GtkWidget *build_details(props *p) {
    entry *e = first(p);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    int row = 0;
    char *path = g_file_get_parse_name(e->file);
    add_row(grid, &row, "Full path:", path);
    g_free(path);
    const char *ctype = g_file_info_get_content_type(e->info);
    char *mime = ctype != nullptr ? g_content_type_get_mime_type(ctype) : nullptr;
    add_row(grid, &row, "MIME type:", mime != nullptr ? mime : "unknown");
    g_free(mime);
    add_separator(grid, &row);

    char buf[96];
    g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT,
               g_file_info_get_attribute_uint64(e->info, G_FILE_ATTRIBUTE_UNIX_INODE));
    add_row(grid, &row, "Inode:", buf);
    g_snprintf(buf, sizeof(buf), "%u",
               g_file_info_get_attribute_uint32(e->info, G_FILE_ATTRIBUTE_UNIX_NLINK));
    add_row(grid, &row, "Hard links:", buf);
    guint32 dev = g_file_info_get_attribute_uint32(e->info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
    g_snprintf(buf, sizeof(buf), "%u:%u", major(dev), minor(dev));
    add_row(grid, &row, "Device:", buf);
    g_snprintf(buf, sizeof(buf), "%u bytes",
               g_file_info_get_attribute_uint32(e->info, G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE));
    add_row(grid, &row, "I/O block size:", buf);
    GDateTime *changed = nullptr;
    if (g_file_info_has_attribute(e->info, G_FILE_ATTRIBUTE_TIME_CHANGED))
        changed = g_date_time_new_from_unix_utc(
            (gint64)g_file_info_get_attribute_uint64(e->info, G_FILE_ATTRIBUTE_TIME_CHANGED));
    char *t = format_time(changed);
    add_row(grid, &row, "Status changed:", t);
    g_free(t);
    if (changed != nullptr)
        g_date_time_unref(changed);
    add_separator(grid, &row);

    GFileInfo *fs = g_file_query_filesystem_info(e->file, "filesystem::*", nullptr, nullptr);
    if (fs != nullptr) {
        add_row(grid, &row, "File system:",
                g_file_info_get_attribute_string(fs, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
        guint64 size = g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        guint64 freeb = g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        GString *s = g_string_new(nullptr);
        append_short_size(s, freeb);
        g_string_append(s, " free of ");
        append_short_size(s, size);
        add_row(grid, &row, "Space:", s->str);
        g_string_free(s, TRUE);
        if (g_file_info_get_attribute_boolean(fs, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY))
            add_row(grid, &row, "", "Mounted read-only");
        g_object_unref(fs);
    }
    return grid;
}

/* ---- applying --------------------------------------------------------------------- */

typedef struct recurse_job {
    GPtrArray *roots; /* GFile* */
    guint32 set, clear;
    gint64 gid; /* -1 = unchanged */
    guint failures;
    char *first_error;
} recurse_job;

static void recurse_job_free(gpointer p) {
    recurse_job *j = p;
    g_ptr_array_unref(j->roots);
    g_free(j->first_error);
    g_free(j);
}

static void note_error(recurse_job *j, GError *err) {
    j->failures++;
    if (j->first_error == nullptr && err != nullptr)
        j->first_error = g_strdup(err->message);
}

/* New mode for an item: Execute is only added to files that already have
 * an Execute bit (like chmod's X), so folders don't make every file
 * runnable. */
static guint32 next_mode(guint32 mode, gboolean is_dir, guint32 set, guint32 clear) {
    guint32 add = set;
    if (!is_dir && (mode & 0111u) == 0)
        add &= ~0111u;
    return (mode & ~clear) | add;
}

static void recurse_thread(GTask *task, [[maybe_unused]] gpointer source, gpointer data,
                           GCancellable *cancel) {
    recurse_job *j = data;
    GQueue queue = G_QUEUE_INIT;
    for (guint i = 0; i < j->roots->len; i++)
        g_queue_push_tail(&queue, g_object_ref(g_ptr_array_index(j->roots, i)));
    GFile *dir;
    while ((dir = g_queue_pop_head(&queue)) != nullptr) {
        GError *err = nullptr;
        GFileEnumerator *en = g_file_enumerate_children(
            dir, COUNT_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &err);
        g_object_unref(dir);
        if (en == nullptr) {
            note_error(j, err);
            g_clear_error(&err);
            continue;
        }
        GFileInfo *info;
        GFile *child;
        while (g_file_enumerator_iterate(en, &info, &child, cancel, nullptr) && info != nullptr) {
            GFileType type = g_file_info_get_file_type(info);
            if (type == G_FILE_TYPE_SYMBOLIC_LINK)
                continue;
            gboolean is_dir = type == G_FILE_TYPE_DIRECTORY;
            if (j->gid >= 0 &&
                !g_file_set_attribute_uint32(child, G_FILE_ATTRIBUTE_UNIX_GID, (guint32)j->gid,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &err)) {
                note_error(j, err);
                g_clear_error(&err);
            }
            guint32 mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) &
                           07777u;
            guint32 next = next_mode(mode, is_dir, j->set, j->clear);
            if (next != mode &&
                !g_file_set_attribute_uint32(child, G_FILE_ATTRIBUTE_UNIX_MODE, next,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &err)) {
                note_error(j, err);
                g_clear_error(&err);
            }
            if (is_dir)
                g_queue_push_tail(&queue, g_object_ref(child));
        }
        g_object_unref(en);
    }
    g_queue_clear_full(&queue, g_object_unref);
    g_task_return_boolean(task, TRUE);
}

static void show_error(props *p, const char *msg) {
    xd_message_box(GTK_WINDOW(p->window), "Error Applying Attributes", XD_MB_ERROR, XD_MB_OK, FALSE,
                   msg);
}

static void reload(props *p);

static void finish_apply(props *p) {
    gtk_widget_set_sensitive(p->window, TRUE);
    if (p->close_after_apply)
        gtk_widget_destroy(p->window);
    else
        reload(p);
}

static void on_recurse_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    props *p = data;
    recurse_job *j = g_task_get_task_data(G_TASK(res));
    if (j->failures > 0) {
        char *msg = g_strdup_printf("%u item%s inside could not be changed.\n\n%s", j->failures,
                                    j->failures == 1 ? "" : "s",
                                    j->first_error != nullptr ? j->first_error : "");
        show_error(p, msg);
        g_free(msg);
        p->close_after_apply = FALSE;
    }
    finish_apply(p);
    g_object_unref(p->window); /* held while the job ran */
}

/* Applies the changes, reporting any failures. The recursive part runs in
 * the background; finish_apply runs when everything is done. */
static void apply(props *p) {
    GString *errors = g_string_new(nullptr);
    guint32 set, clear;
    wanted_bits(p, &set, &clear, FALSE);
    gint64 gid = -1;
    const char *gid_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->group));
    if (gid_id != nullptr) {
        gint64 g = g_ascii_strtoll(gid_id, nullptr, 10);
        if (g != p->group_init)
            gid = g;
    }

    for (guint i = 0; i < p->entries->len; i++) {
        entry *e = g_ptr_array_index(p->entries, i);
        GError *err = nullptr;
        if (p->can_chmod && e->has_unix && !e->is_link) {
            guint32 next = (e->mode & ~clear) | set;
            if (next != e->mode &&
                !g_file_set_attribute_uint32(e->file, G_FILE_ATTRIBUTE_UNIX_MODE, next,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &err)) {
                g_string_append_printf(errors, "%s\n", err->message);
                g_clear_error(&err);
            }
            if (gid >= 0 &&
                !g_file_set_attribute_uint32(e->file, G_FILE_ATTRIBUTE_UNIX_GID, (guint32)gid,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &err)) {
                g_string_append_printf(errors, "%s\n", err->message);
                g_clear_error(&err);
            }
        }
        if (single(p) && p->attrs_ok && p->attrs_want != p->attrs_have) {
            if (write_attr_flags(e->file, p->attrs_want, &err))
                p->attrs_have = p->attrs_want;
            else {
                g_string_append_printf(errors, "%s\n", err->message);
                g_clear_error(&err);
            }
        }

        /* Rename last: the name box (one file) or the Hidden box. */
        char *want = nullptr;
        const char *have = display_name(e);
        if (single(p)) {
            const char *typed = gtk_entry_get_text(GTK_ENTRY(p->name));
            if (g_strcmp0(typed, p->orig_name) != 0)
                want = g_strdup(typed);
        } else if (check_state(p->hidden) != BIT_MIXED) {
            gboolean hide = check_state(p->hidden) == BIT_ALL;
            if (hide && !name_hidden(have))
                want = g_strconcat(".", have, nullptr);
            else if (!hide && name_hidden(have)) {
                const char *bare = have;
                while (*bare == '.')
                    bare++;
                want = g_strdup(bare);
            }
        }
        if (want != nullptr) {
            if (*want == '\0' || strchr(want, '/') != nullptr) {
                g_string_append(errors, "A file name can't be empty or contain '/'.\n");
            } else {
                GFile *renamed = g_file_set_display_name(e->file, want, nullptr, &err);
                if (renamed != nullptr) {
                    g_object_unref(e->file);
                    e->file = renamed;
                } else {
                    g_string_append_printf(errors, "%s\n", err->message);
                    g_clear_error(&err);
                }
            }
            g_free(want);
        }
    }

    if (errors->len > 0) {
        show_error(p, errors->str);
        p->close_after_apply = FALSE;
    }
    g_string_free(errors, TRUE);

    if (p->recursive != nullptr && p->can_chmod &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->recursive))) {
        recurse_job *j = g_new0(recurse_job, 1);
        j->roots = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < p->entries->len; i++) {
            entry *e = g_ptr_array_index(p->entries, i);
            if (e->is_dir)
                g_ptr_array_add(j->roots, g_object_ref(e->file));
        }
        wanted_bits(p, &j->set, &j->clear, TRUE);
        j->gid = gid;
        gtk_widget_set_sensitive(p->window, FALSE);
        g_object_ref(p->window);
        GTask *task = g_task_new(nullptr, nullptr, on_recurse_done, p);
        g_task_set_task_data(task, j, recurse_job_free);
        g_task_run_in_thread(task, recurse_thread);
        g_object_unref(task);
        return;
    }
    finish_apply(p);
}

/* ---- window ----------------------------------------------------------------------- */

static void set_title(props *p) {
    char *title = single(p) ? g_strdup_printf("%s Properties", display_name(first(p)))
                            : g_strdup("Properties");
    gtk_window_set_title(GTK_WINDOW(p->window), title);
    g_free(title);
}

/* Re-reads the files after Apply and resets the page to match. */
static void reload(props *p) {
    for (guint i = 0; i < p->entries->len; i++) {
        entry *e = g_ptr_array_index(p->entries, i);
        GError *err = nullptr;
        if (!entry_load(e, &err))
            g_clear_error(&err);
    }
    p->syncing = TRUE;
    if (single(p)) {
        g_free(p->orig_name);
        p->orig_name = g_strdup(display_name(first(p)));
        gtk_entry_set_text(GTK_ENTRY(p->name), p->orig_name);
        set_check(p->hidden, name_hidden(p->orig_name) ? BIT_ALL : BIT_NONE);
    }
    if (p->recursive != nullptr)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->recursive), FALSE);
    p->syncing = FALSE;
    load_permissions(p);
    set_title(p);
    gtk_widget_set_sensitive(p->apply_button, FALSE);
}

static void on_response(GtkDialog *dlg, gint response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
    } else if (response == GTK_RESPONSE_OK) {
        if (gtk_widget_get_sensitive(p->apply_button)) {
            p->close_after_apply = TRUE;
            apply(p);
        } else {
            gtk_widget_destroy(GTK_WIDGET(dlg));
        }
    } else {
        gtk_widget_destroy(GTK_WIDGET(dlg));
    }
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    stop_count(p);
    g_ptr_array_unref(p->entries);
    g_free(p->orig_name);
    g_free(p);
}

GtkWidget *xd_properties_show(GtkWindow *parent, GList *files) {
    g_return_val_if_fail(files != nullptr, nullptr);
    props *p = g_new0(props, 1);
    p->entries = g_ptr_array_new_with_free_func(entry_free);
    uid_t me = geteuid();
    p->can_chmod = TRUE;
    GString *failed = g_string_new(nullptr);
    for (GList *l = files; l != nullptr; l = l->next) {
        entry *e = g_new0(entry, 1);
        e->file = g_object_ref(l->data);
        GError *err = nullptr;
        if (!entry_load(e, &err)) {
            g_string_append_printf(failed, "%s\n", err != nullptr ? err->message : "");
            g_clear_error(&err);
            entry_free(e);
            continue;
        }
        g_ptr_array_add(p->entries, e);
        if (!e->has_unix || (me != 0 && e->uid != me))
            p->can_chmod = FALSE;
    }

    if (p->entries->len == 0) {
        xd_message_box(parent, "Properties", XD_MB_ERROR, XD_MB_OK, FALSE, failed->str);
        g_string_free(failed, TRUE);
        g_ptr_array_unref(p->entries);
        g_free(p);
        return nullptr;
    }

    p->window = gtk_dialog_new();
    if (parent != nullptr) {
        gtk_window_set_transient_for(GTK_WINDOW(p->window), parent);
        GtkApplication *app = gtk_window_get_application(parent);
        if (app != nullptr)
            gtk_window_set_application(GTK_WINDOW(p->window), app);
    }
    gtk_window_set_resizable(GTK_WINDOW(p->window), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(p->window), "document-properties");
    g_signal_connect(p->window, "destroy", G_CALLBACK(on_destroy), p);

    g_string_free(failed, TRUE);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(notebook), 6);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(p->window))), notebook);
    xd_gtk_notebook_ctrl_tab(p->window, GTK_NOTEBOOK(notebook));
    p->ok_button = gtk_dialog_add_button(GTK_DIALOG(p->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p->window), "Cancel", GTK_RESPONSE_CANCEL);
    p->apply_button = gtk_dialog_add_button(GTK_DIALOG(p->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(p->window), GTK_RESPONSE_OK);
    g_signal_connect(p->window, "response", G_CALLBACK(on_response), p);

    p->syncing = TRUE;
    GtkWidget *general = build_general(p);
    GtkWidget *perms = build_permissions(p);
    p->syncing = TRUE;
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), general, gtk_label_new("General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), perms, gtk_label_new("Permissions"));
    if (single(p))
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_details(p),
                                 gtk_label_new("Details"));
    p->syncing = FALSE;
    sync_permission_text(p, TRUE);
    gtk_widget_set_sensitive(p->apply_button, FALSE);
    set_title(p);
    start_count(p);
    gtk_widget_show_all(p->window);
    gtk_widget_grab_focus(p->ok_button);
    return p->window;
}
