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
#include "drives.h"
#include "encrypted.h"
#include "launcher.h"
#include "places.h"
#include "properties.h"
#include "usershare.h"

static constexpr GtkDialogFlags MODAL_DIALOG =
    (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);

/* ---- formatting ------------------------------------------------------------------ */

static void append_grouped(GString *out, guint64 value) {
    char digits[32];
    int count = g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, value);
    for (int i = 0; i < count; i++) {
        if (i > 0 && (count - i) % 3 == 0)
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
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1000 && unit + 1 < G_N_ELEMENTS(units)) {
        value /= 1024;
        unit++;
    }
    if (value < 10)
        g_string_append_printf(out, "%.2f %s", floor(value * 100) / 100, units[unit]);
    else if (value < 100)
        g_string_append_printf(out, "%.1f %s", floor(value * 10) / 10, units[unit]);
    else
        g_string_append_printf(out, "%.0f %s", floor(value), units[unit]);
}

char *xd_format_size_long(guint64 bytes) {
    GString *string = g_string_new(nullptr);
    append_short_size(string, bytes);
    g_string_append(string, " (");
    append_grouped(string, bytes);
    g_string_append(string, " bytes)");
    return g_string_free(string, FALSE);
}

/* "Friday, October 15, 2026, 8:48:03 PM" */
static char *format_time(GDateTime *utc) {
    if (utc == nullptr)
        return g_strdup("Unavailable");
    GDateTime *local = g_date_time_to_local(utc);
    char *text = g_date_time_format(local, "%A, %B %-d, %Y, %-I:%M:%S %p");
    g_date_time_unref(local);
    return text != nullptr ? text : g_strdup("Unavailable");
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

static void entry_free(gpointer user_data) {
    entry *selected = user_data;
    g_clear_object(&selected->file);
    g_clear_object(&selected->info);
    g_free(selected);
}

static gboolean entry_load(entry *selected, GError **err) {
    GFileInfo *info = g_file_query_info(selected->file, INFO_ATTRS,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, err);
    if (info == nullptr)
        return FALSE;
    g_clear_object(&selected->info);
    selected->info = info;
    GFileType type = g_file_info_get_file_type(info);
    selected->is_link = type == G_FILE_TYPE_SYMBOLIC_LINK;
    selected->is_dir = type == G_FILE_TYPE_DIRECTORY;
    selected->has_unix = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    selected->mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) & 07777u;
    selected->uid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
    selected->gid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID);
    return TRUE;
}

static guint64 info_size(GFileInfo *info) {
    goffset size = g_file_info_get_size(info);
    return size > 0 ? (guint64)size : 0;
}

/* ---- folder size counter (background thread) ------------------------------------- */

typedef struct counter {
    GMutex lock;
    GCancellable *cancel;
    GPtrArray *roots; /* GFile*, folders to walk */
    guint64 bytes, disk, files, folders;
    gboolean done;
} counter;

static void counter_clear(gpointer user_data) {
    counter *count = user_data;
    g_mutex_clear(&count->lock);
    g_clear_object(&count->cancel);
    g_ptr_array_unref(count->roots);
}

/* Walks the folders breadth-first (no recursion, so deep trees can't
 * exhaust the stack), without following symbolic links. Files with several
 * hard links are counted once. */
static gpointer count_thread(gpointer data) {
    counter *count = data;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GQueue queue = G_QUEUE_INIT;
    for (guint i = 0; i < count->roots->len; i++)
        g_queue_push_tail(&queue, g_object_ref(g_ptr_array_index(count->roots, i)));
    GFile *dir;
    while ((dir = g_queue_pop_head(&queue)) != nullptr) {
        GFileEnumerator *en =
            g_cancellable_is_cancelled(count->cancel)
                ? nullptr
                : g_file_enumerate_children(dir, COUNT_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            count->cancel, nullptr);
        g_object_unref(dir);
        if (en == nullptr)
            continue;
        GFileInfo *info;
        GFile *child;
        while (g_file_enumerator_iterate(en, &info, &child, count->cancel, nullptr) &&
               info != nullptr) {
            if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
                g_queue_push_tail(&queue, g_object_ref(child));
                g_mutex_lock(&count->lock);
                count->folders++;
                g_mutex_unlock(&count->lock);
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
            g_mutex_lock(&count->lock);
            count->files++;
            if (!counted) {
                count->bytes += info_size(info);
                count->disk += g_file_info_get_attribute_uint64(
                    info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE);
            }
            g_mutex_unlock(&count->lock);
        }
        g_object_unref(en);
    }
    g_queue_clear_full(&queue, g_object_unref);
    g_hash_table_unref(seen);
    g_mutex_lock(&count->lock);
    count->done = TRUE;
    g_mutex_unlock(&count->lock);
    g_atomic_rc_box_release_full(count, counter_clear);
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
    /* The subfolders' changes are being made; the window was closed
     * meanwhile (this is freed when they're done). */
    gboolean applying, closed;

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

    /* Shortcut (one launcher): the file the fields were read from (a user
     * copy once a system launcher was changed), and as they were. */
    char *sc_path;
    XdLauncher sc_orig;
    char *sc_icon;
    GtkWidget *sc_target, *sc_start, *sc_comment, *sc_icon_image;

    /* Sharing (one folder): Samba's user shares, and the share as it was
     * (`share_name` nullptr when the folder wasn't shared). */
    GtkWidget *share_check, *share_name_entry, *share_write;
    GPtrArray *shares;
    gboolean share_ok; /* sharing is possible here */
    char *share_name;
    gboolean share_writable;
} props;

static entry *first(props *properties) { return g_ptr_array_index(properties->entries, 0); }
static gboolean single(props *properties) { return properties->entries->len == 1; }

static void mark_dirty(props *properties) {
    if (!properties->syncing)
        gtk_widget_set_sensitive(properties->apply_button, TRUE);
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
static void wanted_bits(props *properties, guint32 *set, guint32 *clear, gboolean changed_only) {
    *set = 0;
    *clear = 0;
    for (int i = 0; i < NBITS; i++) {
        int bit_state = check_state(properties->bits[i]);
        if (bit_state == BIT_MIXED || (changed_only && bit_state == properties->bit_init[i]))
            continue;
        if (bit_state == BIT_ALL)
            *set |= perm_bits[i];
        else
            *clear |= perm_bits[i];
    }
}

/* ---- General tab ------------------------------------------------------------------ */

static GtkWidget *value_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_can_focus(label, FALSE);
    return label;
}

/* Adds "Title:  value" on `*row` and returns the value label. */
static GtkWidget *add_row(GtkWidget *grid, int *row, const char *title, const char *value) {
    GtkWidget *title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    gtk_label_set_yalign(GTK_LABEL(title_label), 0);
    GtkWidget *value_widget = value_label(value != nullptr ? value : "");
    gtk_grid_attach(GTK_GRID(grid), title_label, 0, *row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value_widget, 1, *row, 2, 1);
    (*row)++;
    return value_widget;
}

static void add_separator(GtkWidget *grid, int *row) {
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 4);
    gtk_widget_set_margin_bottom(separator, 4);
    gtk_grid_attach(GTK_GRID(grid), separator, 0, (*row)++, 3, 1);
}

static void update_sizes(props *properties) {
    guint64 bytes = properties->base_bytes, disk = properties->base_disk,
            files = properties->base_files, folders = 0;
    gboolean counting = FALSE;
    if (properties->count != nullptr) {
        g_mutex_lock(&properties->count->lock);
        bytes += properties->count->bytes;
        disk += properties->count->disk;
        files += properties->count->files;
        folders = properties->count->folders;
        counting = !properties->count->done;
        g_mutex_unlock(&properties->count->lock);
    }
    char *text = xd_format_size_long(bytes);
    gtk_label_set_text(GTK_LABEL(properties->size), text);
    g_free(text);
    text = xd_format_size_long(disk);
    gtk_label_set_text(GTK_LABEL(properties->disk), text);
    g_free(text);
    if (properties->contains != nullptr) {
        GString *string = g_string_new(nullptr);
        append_grouped(string, files);
        g_string_append(string, files == 1 ? " File, " : " Files, ");
        append_grouped(string, folders);
        g_string_append(string, folders == 1 ? " Folder" : " Folders");
        if (counting)
            g_string_append(string, " (counting…)");
        gtk_label_set_text(GTK_LABEL(properties->contains), string->str);
        g_string_free(string, TRUE);
    }
}

static gboolean on_count_tick(gpointer data) {
    props *properties = data;
    update_sizes(properties);
    g_mutex_lock(&properties->count->lock);
    gboolean done = properties->count->done;
    g_mutex_unlock(&properties->count->lock);
    if (done) {
        properties->count_timer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void start_count(props *properties) {
    properties->base_bytes = properties->base_disk = properties->base_files = 0;
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < properties->entries->len; i++) {
        entry *selected = g_ptr_array_index(properties->entries, i);
        if (selected->is_dir) {
            g_ptr_array_add(roots, g_object_ref(selected->file));
        } else {
            properties->base_files++;
            properties->base_bytes += info_size(selected->info);
            properties->base_disk += g_file_info_get_attribute_uint64(
                selected->info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE);
        }
    }
    if (single(properties) && !first(properties)->is_dir)
        properties->base_files = 0; /* "Contains" isn't shown for one file */
    if (roots->len == 0) {
        g_ptr_array_unref(roots);
        update_sizes(properties);
        return;
    }
    properties->count = g_atomic_rc_box_new0(counter);
    g_mutex_init(&properties->count->lock);
    properties->count->cancel = g_cancellable_new();
    properties->count->roots = roots;
    g_thread_unref(
        g_thread_new("xd-count", count_thread, g_atomic_rc_box_acquire(properties->count)));
    properties->count_timer = g_timeout_add(150, on_count_tick, properties);
    update_sizes(properties);
}

static void stop_count(props *properties) {
    if (properties->count_timer != 0) {
        g_source_remove(properties->count_timer);
        properties->count_timer = 0;
    }
    if (properties->count != nullptr) {
        g_cancellable_cancel(properties->count->cancel);
        g_atomic_rc_box_release_full(properties->count, counter_clear);
        properties->count = nullptr;
    }
}

static const char *display_name(entry *selected) {
    return g_file_info_get_display_name(selected->info);
}

static gboolean name_hidden(const char *name) { return name != nullptr && name[0] == '.'; }

/* Read-only = no write permission for anyone; follows the permission
 * check boxes. */
static void sync_readonly(props *properties) {
    int width[3] = {check_state(properties->bits[1]), check_state(properties->bits[4]),
                    check_state(properties->bits[7])};
    int state = BIT_ALL;
    for (int i = 0; i < 3; i++) {
        if (width[i] == BIT_MIXED)
            state = BIT_MIXED;
        else if (width[i] == BIT_ALL && state != BIT_MIXED)
            state = BIT_NONE;
    }
    gboolean was = properties->syncing;
    properties->syncing = TRUE;
    set_check(properties->readonly, state);
    properties->syncing = was;
}

static void sync_permission_text(props *properties, gboolean rewrite_octal);

static void on_readonly(GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    if (properties->syncing)
        return;
    gtk_toggle_button_set_inconsistent(toggle, FALSE);
    gboolean ro = gtk_toggle_button_get_active(toggle);
    properties->syncing = TRUE;
    if (ro) {
        set_check(properties->bits[1], BIT_NONE);
        set_check(properties->bits[4], BIT_NONE);
        set_check(properties->bits[7], BIT_NONE);
    } else {
        set_check(properties->bits[1], BIT_ALL);
    }
    properties->syncing = FALSE;
    sync_permission_text(properties, TRUE);
    mark_dirty(properties);
}

/* Hidden = the name starts with a dot; with one file, the name box shows
 * the result straight away. */
static void on_hidden(GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    if (properties->syncing)
        return;
    gtk_toggle_button_set_inconsistent(toggle, FALSE);
    if (single(properties)) {
        const char *cur = gtk_entry_get_text(GTK_ENTRY(properties->name));
        const char *bare = cur;
        while (*bare == '.')
            bare++;
        char *next =
            gtk_toggle_button_get_active(toggle) ? g_strconcat(".", bare, nullptr) : g_strdup(bare);
        properties->syncing = TRUE;
        gtk_entry_set_text(GTK_ENTRY(properties->name), next);
        properties->syncing = FALSE;
        g_free(next);
    }
    mark_dirty(properties);
}

static void on_name_changed([[maybe_unused]] GtkEditable *ed, gpointer data) {
    props *properties = data;
    if (properties->syncing)
        return;
    gboolean was = properties->syncing;
    properties->syncing = TRUE;
    set_check(properties->hidden,
              name_hidden(gtk_entry_get_text(GTK_ENTRY(properties->name))) ? BIT_ALL : BIT_NONE);
    properties->syncing = was;
    mark_dirty(properties);
}

static void update_opens_with(props *properties) {
    const char *type = g_file_info_get_content_type(first(properties)->info);
    GAppInfo *app = type != nullptr ? g_app_info_get_default_for_type(type, FALSE) : nullptr;
    gtk_label_set_text(GTK_LABEL(properties->opens_with),
                       app != nullptr ? g_app_info_get_display_name(app) : "(none)");
    g_clear_object(&app);
}

static void on_change_app([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    const char *type = g_file_info_get_content_type(first(properties)->info);
    GtkWidget *dlg = gtk_app_chooser_dialog_new(GTK_WINDOW(properties->window), GTK_DIALOG_MODAL,
                                                first(properties)->file);
    gtk_window_set_title(GTK_WINDOW(dlg), "Open With");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        GAppInfo *app = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dlg));
        GError *err = nullptr;
        if (app != nullptr && !g_app_info_set_as_default_for_type(app, type, &err)) {
            g_warning("cannot set default application: %s", err->message);
            g_clear_error(&err);
        }
        g_clear_object(&app);
        update_opens_with(properties);
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

static void on_advanced([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Advanced Attributes", GTK_WINDOW(properties->window), GTK_DIALOG_MODAL, "OK",
        GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
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
                                     (properties->attrs_want & attr_flags[i].flag) != 0);
        gtk_box_pack_start(GTK_BOX(inner), checks[i], FALSE, FALSE, 0);
    }
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        int want = properties->attrs_want;
        for (size_t i = 0; i < G_N_ELEMENTS(attr_flags); i++) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks[i])))
                want |= attr_flags[i].flag;
            else
                want &= ~attr_flags[i].flag;
        }
        if (want != properties->attrs_want) {
            properties->attrs_want = want;
            mark_dirty(properties);
        }
    }
    gtk_widget_destroy(dlg);
}

static GIcon *multi_icon(void) {
    static const char *const names[] = {"document-multiple", "emblem-documents", "text-x-generic",
                                        nullptr};
    return g_themed_icon_new_from_names((char **)(uintptr_t)names, -1);
}

static GtkWidget *build_general(props *properties) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    int row = 0;
    entry *first_entry = first(properties);

    GIcon *gicon =
        single(properties) ? g_object_ref(g_file_info_get_icon(first_entry->info)) : multi_icon();
    GtkWidget *icon = gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_DND);
    g_object_unref(gicon);
    gtk_widget_set_halign(icon, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), icon, 0, row, 1, 1);
    properties->name = gtk_entry_new();
    gtk_widget_set_hexpand(properties->name, TRUE);
    gtk_widget_set_valign(properties->name, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), properties->name, 1, row++, 2, 1);

    guint files = 0, folders = 0;
    gboolean same_type = TRUE, same_parent = TRUE;
    GFile *parent = g_file_get_parent(first_entry->file);
    for (guint i = 0; i < properties->entries->len; i++) {
        entry *selected = g_ptr_array_index(properties->entries, i);
        if (selected->is_dir)
            folders++;
        else
            files++;
        if (g_strcmp0(g_file_info_get_content_type(selected->info),
                      g_file_info_get_content_type(first_entry->info)) != 0)
            same_type = FALSE;
        GFile *xp = g_file_get_parent(selected->file);
        if (xp == nullptr || parent == nullptr || !g_file_equal(xp, parent))
            same_parent = FALSE;
        g_clear_object(&xp);
    }

    if (single(properties)) {
        properties->orig_name = g_strdup(display_name(first_entry));
        gtk_entry_set_text(GTK_ENTRY(properties->name), properties->orig_name);
        g_signal_connect(properties->name, "changed", G_CALLBACK(on_name_changed), properties);
        if (parent == nullptr)
            gtk_widget_set_sensitive(properties->name, FALSE); /* the root folder */
    } else {
        char *text = g_strdup_printf("%u File%s, %u Folder%s", files, files == 1 ? "" : "s",
                                     folders, folders == 1 ? "" : "s");
        gtk_entry_set_text(GTK_ENTRY(properties->name), text);
        gtk_widget_set_sensitive(properties->name, FALSE);
        g_free(text);
    }
    add_separator(grid, &row);

    /* Type and default application */
    const char *ctype = g_file_info_get_content_type(first_entry->info);
    char *desc;
    if (!same_type)
        desc = g_strdup("Multiple Types");
    else if (first_entry->is_dir)
        desc = g_strdup(single(properties) ? "File Folder" : "All of type File Folder");
    else if (first_entry->is_link)
        desc = g_strdup("Symbolic Link");
    else {
        char *description =
            ctype != nullptr ? g_content_type_get_description(ctype) : g_strdup("File");
        desc = single(properties) ? g_strdup(description)
                                  : g_strdup_printf("All of type %s", description);
        g_free(description);
    }
    add_row(grid, &row, "Type of file:", desc);
    g_free(desc);
    if (single(properties) && first_entry->is_link) {
        add_row(grid, &row, "Target:", g_file_info_get_symlink_target(first_entry->info));
    }
    if (single(properties) && !first_entry->is_dir && !first_entry->is_link) {
        GtkWidget *opens_label = gtk_label_new("Opens with:");
        gtk_label_set_xalign(GTK_LABEL(opens_label), 0);
        gtk_grid_attach(GTK_GRID(grid), opens_label, 0, row++, 1, 1);
        properties->opens_with = value_label("");
        gtk_grid_attach(GTK_GRID(grid), properties->opens_with, 1, row - 1, 1, 1);
        GtkWidget *change = gtk_button_new_with_mnemonic("C_hange...");
        g_signal_connect(change, "clicked", G_CALLBACK(on_change_app), properties);
        gtk_grid_attach(GTK_GRID(grid), change, 2, row - 1, 1, 1);
        update_opens_with(properties);
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
        loc = single(properties) ? pn : g_strdup_printf("All in %s", pn);
        if (!single(properties))
            g_free(pn);
    }
    add_row(grid, &row, "Location:", loc);
    g_free(loc);
    properties->size = add_row(grid, &row, "Size:", "");
    properties->disk = add_row(grid, &row, "Size on disk:", "");
    if (folders > 0)
        properties->contains = add_row(grid, &row, "Contains:", "");
    add_separator(grid, &row);

    /* Times (single item) */
    if (single(properties)) {
        char *time_text = format_time(g_file_info_get_creation_date_time(first_entry->info));
        add_row(grid, &row, "Created:", time_text);
        g_free(time_text);
        time_text = format_time(g_file_info_get_modification_date_time(first_entry->info));
        add_row(grid, &row, "Modified:", time_text);
        g_free(time_text);
        time_text = format_time(g_file_info_get_access_date_time(first_entry->info));
        add_row(grid, &row, "Accessed:", time_text);
        g_free(time_text);
        add_separator(grid, &row);
    }

    /* Attributes */
    GtkWidget *attr_title = gtk_label_new("Attributes:");
    gtk_label_set_xalign(GTK_LABEL(attr_title), 0);
    gtk_grid_attach(GTK_GRID(grid), attr_title, 0, row, 1, 1);
    GtkWidget *attr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    properties->readonly = gtk_check_button_new_with_mnemonic("_Read-only");
    properties->hidden = gtk_check_button_new_with_mnemonic("H_idden");
    gtk_box_pack_start(GTK_BOX(attr_box), properties->readonly, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(attr_box), properties->hidden, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), attr_box, 1, row, 1, 1);
    properties->advanced = gtk_button_new_with_mnemonic("A_dvanced...");
    gtk_grid_attach(GTK_GRID(grid), properties->advanced, 2, row++, 1, 1);
    g_signal_connect(properties->readonly, "toggled", G_CALLBACK(on_readonly), properties);
    g_signal_connect(properties->hidden, "toggled", G_CALLBACK(on_hidden), properties);
    g_signal_connect(properties->advanced, "clicked", G_CALLBACK(on_advanced), properties);

    int hidden = BIT_NONE;
    for (guint i = 0; i < properties->entries->len; i++) {
        gboolean is_hidden = name_hidden(display_name(g_ptr_array_index(properties->entries, i)));
        int bit_state = is_hidden ? BIT_ALL : BIT_NONE;
        hidden = i == 0 ? bit_state : hidden == bit_state ? hidden : BIT_MIXED;
    }
    set_check(properties->hidden, hidden);
    if (parent == nullptr)
        gtk_widget_set_sensitive(properties->hidden, FALSE);
    if (!properties->can_chmod)
        gtk_widget_set_sensitive(properties->readonly, FALSE);

    properties->attrs_ok = single(properties) && !first_entry->is_link &&
                           read_attr_flags(first_entry->file, &properties->attrs_have);
    properties->attrs_want = properties->attrs_have;
    if (!properties->attrs_ok) {
        gtk_widget_set_sensitive(properties->advanced, FALSE);
        gtk_widget_set_tooltip_text(
            properties->advanced, single(properties) ? "This file system doesn't support attributes"
                                                     : "Select a single file to change attributes");
    }
    g_clear_object(&parent);
    return grid;
}

/* ---- Permissions tab ---------------------------------------------------------- */

/* Updates the numeric and symbolic modes (and Read-only) from the check
 * boxes; the numeric box is left alone while the user types in it. */
static void sync_permission_text(props *properties, gboolean rewrite_octal) {
    gboolean mixed = FALSE;
    guint32 mode = 0;
    for (int i = 0; i < NBITS; i++) {
        int bit_state = check_state(properties->bits[i]);
        if (bit_state == BIT_MIXED)
            mixed = TRUE;
        else if (bit_state == BIT_ALL)
            mode |= perm_bits[i];
    }
    gboolean was = properties->syncing;
    properties->syncing = TRUE;
    if (mixed) {
        if (rewrite_octal)
            gtk_entry_set_text(GTK_ENTRY(properties->octal), "");
        gtk_entry_set_placeholder_text(GTK_ENTRY(properties->octal), "mixed");
        gtk_label_set_text(GTK_LABEL(properties->symbolic), "(differs between the selected items)");
    } else {
        char oct[8];
        g_snprintf(oct, sizeof(oct), "%04o", mode);
        if (rewrite_octal)
            gtk_entry_set_text(GTK_ENTRY(properties->octal), oct);
        entry *selected = first(properties);
        char sym[11] = "----------";
        sym[0] = selected->is_dir ? 'd' : selected->is_link ? 'l' : '-';
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
        gtk_label_set_text(GTK_LABEL(properties->symbolic), single(properties) ? sym : sym + 1);
    }
    properties->syncing = was;
    sync_readonly(properties);
}

static void on_bit_toggled(GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    if (properties->syncing)
        return;
    gtk_toggle_button_set_inconsistent(toggle, FALSE);
    sync_permission_text(properties, TRUE);
    mark_dirty(properties);
}

/* Typing a mode like 755 or 2775 sets every check box. */
static void on_octal_changed(GtkEditable *ed, gpointer data) {
    props *properties = data;
    if (properties->syncing)
        return;
    const char *text = gtk_entry_get_text(GTK_ENTRY(ed));
    size_t count = strlen(text);
    if (count < 3 || count > 4)
        return;
    guint32 mode = 0;
    for (size_t i = 0; i < count; i++) {
        if (text[i] < '0' || text[i] > '7')
            return;
        mode = mode * 8 + (guint32)(text[i] - '0');
    }
    properties->syncing = TRUE;
    for (int i = 0; i < NBITS; i++)
        set_check(properties->bits[i], (mode & perm_bits[i]) ? BIT_ALL : BIT_NONE);
    properties->syncing = FALSE;
    sync_permission_text(properties, FALSE);
    mark_dirty(properties);
}

static void on_octal_done(GtkWidget *widget, [[maybe_unused]] GdkEvent *ev, gpointer data) {
    (void)widget;
    sync_permission_text(data, TRUE);
}

static void on_octal_activate(GtkEntry *box, gpointer data) {
    on_octal_done(GTK_WIDGET(box), nullptr, data);
}

static void on_dirty_widget([[maybe_unused]] GtkWidget *widget, gpointer data) { mark_dirty(data); }

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

static void fill_groups(props *properties) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(properties->group));
    GArray *gids = g_array_new(FALSE, FALSE, sizeof(guint32));
    if (properties->group_init >= 0) {
        guint32 gid = (guint32)properties->group_init;
        g_array_append_val(gids, gid);
    }
    if (properties->can_chmod) {
        int count = getgroups(0, nullptr);
        if (count > 0) {
            gid_t *list = g_new(gid_t, (gsize)count);
            count = getgroups(count, list);
            for (int i = 0; i < count; i++) {
                guint32 gid = list[i];
                gboolean dup = FALSE;
                for (guint k = 0; k < gids->len; k++)
                    dup = dup || g_array_index(gids, guint32, k) == gid;
                if (!dup)
                    g_array_append_val(gids, gid);
            }
            g_free(list);
        }
    }
    for (guint k = 0; k < gids->len; k++) {
        guint32 gid = g_array_index(gids, guint32, k);
        char id[16];
        g_snprintf(id, sizeof(id), "%u", gid);
        char *name = group_name(gid);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->group), id, name);
        g_free(name);
    }
    if (properties->group_init >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(properties->group), 0);
    g_array_unref(gids);
}

/* Sets the check boxes, owner and group from the files. */
static void load_permissions(props *properties) {
    properties->syncing = TRUE;
    for (int i = 0; i < NBITS; i++) {
        int state = BIT_NONE;
        for (guint k = 0; k < properties->entries->len; k++) {
            entry *selected = g_ptr_array_index(properties->entries, k);
            int bit_state = (selected->mode & perm_bits[i]) ? BIT_ALL : BIT_NONE;
            state = k == 0 ? bit_state : state == bit_state ? state : BIT_MIXED;
        }
        properties->bit_init[i] = state;
        set_check(properties->bits[i], state);
    }
    properties->group_init = first(properties)->gid;
    for (guint k = 1; k < properties->entries->len; k++) {
        entry *selected = g_ptr_array_index(properties->entries, k);
        if ((gint64)selected->gid != properties->group_init)
            properties->group_init = -1;
    }
    fill_groups(properties);
    properties->syncing = FALSE;
    sync_permission_text(properties, TRUE);
}

static GtkWidget *build_permissions(props *properties) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    entry *selected = first(properties);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    int row = 0;
    gboolean same_owner = TRUE;
    for (guint k = 1; k < properties->entries->len; k++)
        same_owner = same_owner &&
                     ((entry *)g_ptr_array_index(properties->entries, k))->uid == selected->uid;
    char *owner;
    if (!same_owner) {
        owner = g_strdup("(differs between the selected items)");
    } else {
        const char *user =
            g_file_info_get_attribute_string(selected->info, G_FILE_ATTRIBUTE_OWNER_USER);
        const char *real =
            g_file_info_get_attribute_string(selected->info, G_FILE_ATTRIBUTE_OWNER_USER_REAL);
        owner = real != nullptr && *real != '\0' && g_strcmp0(real, user) != 0
                    ? g_strdup_printf("%s (%s)", user, real)
                    : g_strdup(user != nullptr ? user : "unknown");
    }
    add_row(grid, &row, "Owner:", owner);
    g_free(owner);
    GtkWidget *gl = gtk_label_new_with_mnemonic("_Group:");
    gtk_label_set_xalign(GTK_LABEL(gl), 0);
    properties->group = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(gl), properties->group);
    gtk_widget_set_halign(properties->group, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), gl, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), properties->group, 1, row++, 1, 1);
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
    for (int column = 0; column < 3; column++) {
        GtkWidget *label = gtk_label_new(cols[column]);
        gtk_grid_attach(GTK_GRID(bits), label, column + 1, 0, 1, 1);
    }
    for (int row_index = 0; row_index < 3; row_index++) {
        GtkWidget *label = gtk_label_new(rows[row_index]);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_grid_attach(GTK_GRID(bits), label, 0, row_index + 1, 1, 1);
        for (int column = 0; column < 3; column++) {
            GtkWidget *cb = gtk_check_button_new();
            gtk_widget_set_halign(cb, GTK_ALIGN_CENTER);
            char *tip = g_strdup_printf("%s: %s", rows[row_index], cols[column]);
            gtk_widget_set_tooltip_text(cb, tip);
            g_free(tip);
            properties->bits[row_index * 3 + column] = cb;
            gtk_grid_attach(GTK_GRID(bits), cb, column + 1, row_index + 1, 1, 1);
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
        properties->bits[9 + i] = gtk_check_button_new_with_mnemonic(special[i]);
        gtk_widget_set_tooltip_text(properties->bits[9 + i], special_tip[i]);
        gtk_box_pack_start(GTK_BOX(sbox), properties->bits[9 + i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), sframe, FALSE, FALSE, 0);

    GtkWidget *mbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ml = gtk_label_new_with_mnemonic("_Numeric:");
    properties->octal = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(properties->octal), 6);
    gtk_entry_set_max_length(GTK_ENTRY(properties->octal), 4);
    gtk_label_set_mnemonic_widget(GTK_LABEL(ml), properties->octal);
    properties->symbolic = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(properties->symbolic), TRUE);
    gtk_widget_set_can_focus(properties->symbolic, FALSE);
    gtk_box_pack_start(GTK_BOX(mbox), ml, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mbox), properties->octal, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mbox), gtk_label_new("Symbolic:"), FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(mbox), properties->symbolic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), mbox, FALSE, FALSE, 0);

    gboolean any_dir = FALSE;
    for (guint k = 0; k < properties->entries->len; k++)
        any_dir = any_dir || ((entry *)g_ptr_array_index(properties->entries, k))->is_dir;
    if (any_dir) {
        GtkWidget *hint = gtk_label_new("For folders, Read lists the contents, Write adds and "
                                        "removes items, and Execute opens the folder.");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(hint), 50);
        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
        properties->recursive =
            gtk_check_button_new_with_mnemonic("_Apply changes to all files and folders inside");
        gtk_widget_set_tooltip_text(properties->recursive,
                                    "Only the permissions you change are applied. Files only "
                                    "get Execute if they are already executable.");
        g_signal_connect(properties->recursive, "toggled", G_CALLBACK(on_dirty_widget), properties);
        gtk_box_pack_start(GTK_BOX(box), properties->recursive, FALSE, FALSE, 0);
    }

    if (!properties->can_chmod) {
        GtkWidget *note = gtk_label_new("Only the owner can change these permissions.");
        gtk_label_set_xalign(GTK_LABEL(note), 0);
        gtk_box_pack_end(GTK_BOX(box), note, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(frame, FALSE);
        gtk_widget_set_sensitive(sframe, FALSE);
        gtk_widget_set_sensitive(mbox, FALSE);
        gtk_widget_set_sensitive(properties->group, FALSE);
        if (properties->recursive != nullptr)
            gtk_widget_set_sensitive(properties->recursive, FALSE);
    }

    for (int i = 0; i < NBITS; i++)
        g_signal_connect(properties->bits[i], "toggled", G_CALLBACK(on_bit_toggled), properties);
    g_signal_connect(properties->octal, "changed", G_CALLBACK(on_octal_changed), properties);
    g_signal_connect(properties->octal, "focus-out-event", G_CALLBACK(on_octal_done), properties);
    g_signal_connect(properties->octal, "activate", G_CALLBACK(on_octal_activate), properties);
    g_signal_connect(properties->group, "changed", G_CALLBACK(on_dirty_widget), properties);
    load_permissions(properties);
    return box;
}

/* ---- Details tab ------------------------------------------------------------------ */

static GtkWidget *build_details(props *properties) {
    entry *selected = first(properties);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    int row = 0;
    char *path = g_file_get_parse_name(selected->file);
    add_row(grid, &row, "Full path:", path);
    g_free(path);
    const char *ctype = g_file_info_get_content_type(selected->info);
    char *mime = ctype != nullptr ? g_content_type_get_mime_type(ctype) : nullptr;
    add_row(grid, &row, "MIME type:", mime != nullptr ? mime : "unknown");
    g_free(mime);
    add_separator(grid, &row);

    char buf[96];
    g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT,
               g_file_info_get_attribute_uint64(selected->info, G_FILE_ATTRIBUTE_UNIX_INODE));
    add_row(grid, &row, "Inode:", buf);
    g_snprintf(buf, sizeof(buf), "%u",
               g_file_info_get_attribute_uint32(selected->info, G_FILE_ATTRIBUTE_UNIX_NLINK));
    add_row(grid, &row, "Hard links:", buf);
    guint32 dev = g_file_info_get_attribute_uint32(selected->info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
    g_snprintf(buf, sizeof(buf), "%u:%u", major(dev), minor(dev));
    add_row(grid, &row, "Device:", buf);
    g_snprintf(buf, sizeof(buf), "%u bytes",
               g_file_info_get_attribute_uint32(selected->info, G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE));
    add_row(grid, &row, "I/O block size:", buf);
    GDateTime *changed = nullptr;
    if (g_file_info_has_attribute(selected->info, G_FILE_ATTRIBUTE_TIME_CHANGED))
        changed = g_date_time_new_from_unix_utc((gint64)g_file_info_get_attribute_uint64(
            selected->info, G_FILE_ATTRIBUTE_TIME_CHANGED));
    char *time_text = format_time(changed);
    add_row(grid, &row, "Status changed:", time_text);
    g_free(time_text);
    if (changed != nullptr)
        g_date_time_unref(changed);
    add_separator(grid, &row);

    GFileInfo *fs = g_file_query_filesystem_info(selected->file, "filesystem::*", nullptr, nullptr);
    if (fs != nullptr) {
        add_row(grid, &row, "File system:",
                g_file_info_get_attribute_string(fs, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
        guint64 size = g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        guint64 freeb = g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        GString *string = g_string_new(nullptr);
        append_short_size(string, freeb);
        g_string_append(string, " free of ");
        append_short_size(string, size);
        add_row(grid, &row, "Space:", string->str);
        g_string_free(string, TRUE);
        if (g_file_info_get_attribute_boolean(fs, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY))
            add_row(grid, &row, "", "Mounted read-only");
        g_object_unref(fs);
    }
    return grid;
}

/* ---- Shortcut tab (launchers) ---------------------------------------------------- */

/* The program a launcher's command runs, found in $PATH; nullptr if not. */
static char *target_program(const char *command) {
    char **argv = nullptr;
    if (command == nullptr || !g_shell_parse_argv(command, nullptr, &argv, nullptr))
        return nullptr;
    char *prog = argv[0] != nullptr ? g_find_program_in_path(argv[0]) : nullptr;
    g_strfreev(argv);
    return prog;
}

/* An icon from the launcher's Icon: a theme name or an image file. */
static void set_icon_image(GtkWidget *image, const char *icon) {
    if (icon != nullptr && g_path_is_absolute(icon)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(icon, 32, 32, nullptr);
        if (pb != nullptr) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(image), pb);
            g_object_unref(pb);
            return;
        }
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(image),
                                 icon != nullptr && *icon != '\0' ? icon
                                                                  : "application-x-executable",
                                 GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
}

enum { ICON_COL_PIXBUF, ICON_COL_NAME, ICON_COL_COUNT };

static void on_icon_chosen(GtkIconView *view, gpointer name_entry) {
    GList *sel = gtk_icon_view_get_selected_items(view);
    GtkTreeIter it;
    if (sel != nullptr &&
        gtk_tree_model_get_iter(gtk_icon_view_get_model(view), &it, sel->data)) {
        char *name = nullptr;
        gtk_tree_model_get(gtk_icon_view_get_model(view), &it, ICON_COL_NAME, &name, -1);
        gtk_entry_set_text(GTK_ENTRY(name_entry), name);
        g_free(name);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
}

static void on_icon_activated(GtkIconView *view, [[maybe_unused]] GtkTreePath *tp,
                              gpointer dialog) {
    (void)view;
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
}

static void on_icon_browse([[maybe_unused]] GtkButton *button, gpointer name_entry) {
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(name_entry));
    GtkWidget *icon_chooser = gtk_file_chooser_dialog_new(
        "Change Icon", GTK_WINDOW(top), GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter *icon_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(icon_filter, "Icon Files");
    gtk_file_filter_add_pixbuf_formats(icon_filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(icon_chooser), icon_filter);
    if (gtk_dialog_run(GTK_DIALOG(icon_chooser)) == GTK_RESPONSE_ACCEPT) {
        char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(icon_chooser));
        if (file != nullptr)
            gtk_entry_set_text(GTK_ENTRY(name_entry), file);
        g_free(file);
    }
    gtk_widget_destroy(icon_chooser);
}

/* Windows' Change Icon box: where the icon comes from (here a theme
 * icon's name or an image file, with Browse...) and the icons to pick
 * from (the theme's program icons). Returns the choice, or nullptr. */
static char *change_icon_dialog(GtkWindow *parent, const char *current) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Change Icon", parent,
                                                 MODAL_DIALOG,
                                                 "OK", GTK_RESPONSE_OK, "Cancel",
                                                 GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 420, 360);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *look = gtk_label_new_with_mnemonic("_Look for icons in this file or icon name:");
    gtk_label_set_xalign(GTK_LABEL(look), 0);
    gtk_box_pack_start(GTK_BOX(box), look, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name_entry), current != nullptr ? current : "");
    gtk_entry_set_activates_default(GTK_ENTRY(name_entry), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(look), name_entry);
    gtk_box_pack_start(GTK_BOX(row), name_entry, TRUE, TRUE, 0);
    GtkWidget *browse = gtk_button_new_with_mnemonic("_Browse...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_icon_browse), name_entry);
    gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    GtkWidget *pick = gtk_label_new_with_mnemonic("_Select an icon from the list below:");
    gtk_label_set_xalign(GTK_LABEL(pick), 0);
    gtk_box_pack_start(GTK_BOX(box), pick, FALSE, FALSE, 0);

    GtkListStore *store = gtk_list_store_new(ICON_COL_COUNT, GDK_TYPE_PIXBUF, G_TYPE_STRING);
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GList *names = gtk_icon_theme_list_icons(theme, "Applications");
    names = g_list_sort(names, (GCompareFunc)g_strcmp0);
    for (GList *list = names; list != nullptr; list = list->next) {
        if (g_str_has_suffix(list->data, "-symbolic"))
            continue;
        GdkPixbuf *pb =
            gtk_icon_theme_load_icon(theme, list->data, 32, GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
        if (pb == nullptr)
            continue;
        gtk_list_store_insert_with_values(store, nullptr, -1, ICON_COL_PIXBUF, pb, ICON_COL_NAME,
                                          list->data, -1);
        g_object_unref(pb);
    }
    g_list_free_full(names, g_free);
    GtkWidget *icons = gtk_icon_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(icons), ICON_COL_PIXBUF);
    gtk_icon_view_set_tooltip_column(GTK_ICON_VIEW(icons), ICON_COL_NAME);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(icons), 40);
    gtk_icon_view_set_margin(GTK_ICON_VIEW(icons), 4);
    gtk_label_set_mnemonic_widget(GTK_LABEL(pick), icons);
    g_signal_connect(icons, "selection-changed", G_CALLBACK(on_icon_chosen), name_entry);
    g_signal_connect(icons, "item-activated", G_CALLBACK(on_icon_activated), dlg);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), icons);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box, TRUE, TRUE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_widget_show_all(dlg);
    char *chosen = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK)
        chosen = g_strdup(gtk_entry_get_text(GTK_ENTRY(name_entry)));
    gtk_widget_destroy(dlg);
    return chosen;
}

static void on_change_icon([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    char *icon = change_icon_dialog(GTK_WINDOW(properties->window), properties->sc_icon);
    if (icon != nullptr && g_strcmp0(icon, properties->sc_icon) != 0) {
        g_free(properties->sc_icon);
        properties->sc_icon = icon;
        set_icon_image(properties->sc_icon_image, icon);
        mark_dirty(properties);
    } else {
        g_free(icon);
    }
}

/* Find Target...: the folder the program is in. */
static void on_find_target([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    char *prog = target_program(gtk_entry_get_text(GTK_ENTRY(properties->sc_target)));
    if (prog == nullptr) {
        xd_message_box(GTK_WINDOW(properties->window), "Problem with Shortcut", XD_MB_ERROR,
                       XD_MB_OK, FALSE, "The item that this shortcut refers to cannot be found.");
        return;
    }
    char *dir = g_path_get_dirname(prog);
    char *files = g_strdup("xd-files");
    char *argv[] = {files, dir, nullptr};
    GError *err = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &err)) {
        xd_message_box(GTK_WINDOW(properties->window), "Properties", XD_MB_ERROR, XD_MB_OK, FALSE,
                       err->message);
        g_error_free(err);
    }
    g_free(files);
    g_free(dir);
    g_free(prog);
}

/* XP's Shortcut tab for a launcher: Target (Exec, or a link's URL), Start
 * in (Path), Run (launchers have no window state: always Normal window),
 * Comment, Find Target... and Change Icon.... */
static GtkWidget *build_shortcut(props *properties) {
    const XdLauncher *launcher = &properties->sc_orig;
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    int row = 0;
    properties->sc_icon_image = gtk_image_new();
    set_icon_image(properties->sc_icon_image, properties->sc_icon);
    gtk_widget_set_halign(properties->sc_icon_image, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), properties->sc_icon_image, 0, row, 1, 1);
    GtkWidget *name =
        value_label(launcher->name != nullptr ? launcher->name : display_name(first(properties)));
    gtk_grid_attach(GTK_GRID(grid), name, 1, row++, 2, 1);
    add_separator(grid, &row);

    char *prog = launcher->is_link ? nullptr : target_program(launcher->target);
    char *where = nullptr;
    if (prog != nullptr) {
        char *dir = g_path_get_dirname(prog);
        where = g_filename_display_basename(dir);
        g_free(dir);
    }
    add_row(grid, &row, "Target type:", launcher->is_link ? "Location" : "Application");
    add_row(grid, &row, "Target location:", where != nullptr ? where : "");
    g_free(where);
    g_free(prog);

    GtkWidget *target_label = gtk_label_new_with_mnemonic("_Target:");
    gtk_label_set_xalign(GTK_LABEL(target_label), 0);
    properties->sc_target = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(properties->sc_target),
                       launcher->target != nullptr ? launcher->target : "");
    gtk_widget_set_hexpand(properties->sc_target, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(target_label), properties->sc_target);
    gtk_grid_attach(GTK_GRID(grid), target_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), properties->sc_target, 1, row++, 2, 1);
    add_separator(grid, &row);

    target_label = gtk_label_new_with_mnemonic("_Start in:");
    gtk_label_set_xalign(GTK_LABEL(target_label), 0);
    properties->sc_start = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(properties->sc_start),
                       launcher->start_in != nullptr ? launcher->start_in : "");
    gtk_label_set_mnemonic_widget(GTK_LABEL(target_label), properties->sc_start);
    gtk_widget_set_sensitive(properties->sc_start, !launcher->is_link);
    gtk_grid_attach(GTK_GRID(grid), target_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), properties->sc_start, 1, row++, 2, 1);

    target_label = gtk_label_new_with_mnemonic("_Run:");
    gtk_label_set_xalign(GTK_LABEL(target_label), 0);
    GtkWidget *run = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(run), "Normal window");
    gtk_combo_box_set_active(GTK_COMBO_BOX(run), 0);
    gtk_widget_set_sensitive(run, FALSE); /* a launcher can't ask for another */
    gtk_widget_set_halign(run, GTK_ALIGN_START);
    gtk_label_set_mnemonic_widget(GTK_LABEL(target_label), run);
    gtk_grid_attach(GTK_GRID(grid), target_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), run, 1, row++, 2, 1);

    target_label = gtk_label_new_with_mnemonic("C_omment:");
    gtk_label_set_xalign(GTK_LABEL(target_label), 0);
    properties->sc_comment = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(properties->sc_comment),
                       launcher->comment != nullptr ? launcher->comment : "");
    gtk_label_set_mnemonic_widget(GTK_LABEL(target_label), properties->sc_comment);
    gtk_grid_attach(GTK_GRID(grid), target_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), properties->sc_comment, 1, row++, 2, 1);
    g_signal_connect_swapped(properties->sc_target, "changed", G_CALLBACK(mark_dirty), properties);
    g_signal_connect_swapped(properties->sc_start, "changed", G_CALLBACK(mark_dirty), properties);
    g_signal_connect_swapped(properties->sc_comment, "changed", G_CALLBACK(mark_dirty), properties);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(buttons, 8);
    GtkWidget *find = gtk_button_new_with_mnemonic("_Find Target...");
    gtk_widget_set_sensitive(find, !launcher->is_link);
    g_signal_connect(find, "clicked", G_CALLBACK(on_find_target), properties);
    GtkWidget *change = gtk_button_new_with_mnemonic("_Change Icon...");
    g_signal_connect(change, "clicked", G_CALLBACK(on_change_icon), properties);
    gtk_box_pack_start(GTK_BOX(buttons), find, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), change, FALSE, FALSE, 0);
    gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), buttons, 0, row++, 3, 1);
    return grid;
}

/* A launcher that has a Shortcut tab (a program or a location). */
static void load_shortcut(props *properties) {
    entry *selected = first(properties);
    const char *ctype = g_file_info_get_content_type(selected->info);
    if (!single(properties) || selected->is_dir || ctype == nullptr ||
        !g_content_type_is_a(ctype, "application/x-desktop"))
        return;
    char *path = g_file_get_path(selected->file);
    if (path != nullptr && xd_launcher_read(path, &properties->sc_orig, nullptr)) {
        properties->sc_path = path;
        properties->sc_icon = g_strdup(properties->sc_orig.icon);
    } else {
        g_free(path);
    }
}

static gboolean field_changed(const char *was, const char *now) {
    return g_strcmp0(was != nullptr ? was : "", now != nullptr ? now : "") != 0;
}

/* Writes the Shortcut tab's changes (a system launcher as a user copy,
 * which is what's shown and changed from then on). */
static void apply_shortcut(props *properties, GString *errors) {
    if (properties->sc_path == nullptr)
        return;
    XdLauncher now = {
        .target = g_strdup(gtk_entry_get_text(GTK_ENTRY(properties->sc_target))),
        .start_in = g_strdup(gtk_entry_get_text(GTK_ENTRY(properties->sc_start))),
        .comment = g_strdup(gtk_entry_get_text(GTK_ENTRY(properties->sc_comment))),
        .icon = g_strdup(properties->sc_icon),
    };
    const XdLauncher *was = &properties->sc_orig;
    if (!field_changed(was->target, now.target) && !field_changed(was->start_in, now.start_in) &&
        !field_changed(was->comment, now.comment) && !field_changed(was->icon, now.icon)) {
        xd_launcher_clear(&now);
        return;
    }
    char *user_dir = xd_launcher_is_installed(properties->sc_path)
                         ? g_build_filename(g_get_user_data_dir(), "applications", nullptr)
                         : nullptr;
    GError *err = nullptr;
    char *written = xd_launcher_write(properties->sc_path, &now, user_dir, &err);
    g_free(user_dir);
    xd_launcher_clear(&now);
    if (written == nullptr) {
        g_string_append_printf(errors, "%s\n", err->message);
        g_error_free(err);
        return;
    }
    g_free(properties->sc_path);
    properties->sc_path = written;
    XdLauncher fresh;
    if (xd_launcher_read(written, &fresh, nullptr)) {
        xd_launcher_clear(&properties->sc_orig);
        properties->sc_orig = fresh;
    }
}

/* ---- Summary tab (pictures) --------------------------------------------------------- */

/* XP's Summary for a picture (its Advanced view): Width and Height, read
 * from the file's header by gdk-pixbuf without decoding the picture. */
static GtkWidget *build_image_summary(props *properties) {
    entry *selected = first(properties);
    const char *ctype = g_file_info_get_content_type(selected->info);
    if (!single(properties) || selected->is_dir || ctype == nullptr ||
        !g_str_has_prefix(ctype, "image/"))
        return nullptr;
    char *path = g_file_get_path(selected->file);
    int width = 0, height = 0;
    GdkPixbufFormat *fmt =
        path != nullptr ? gdk_pixbuf_get_file_info(path, &width, &height) : nullptr;
    g_free(path);
    if (fmt == nullptr || width <= 0 || height <= 0)
        return nullptr;
    GtkTreeStore *store = gtk_tree_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkTreeIter group, it;
    gtk_tree_store_insert_with_values(store, &group, nullptr, -1, 0, "<b>Image</b>", 1, "", -1);
    char *value = g_strdup_printf("%d pixels", width);
    gtk_tree_store_insert_with_values(store, &it, &group, -1, 0, "Width", 1, value, -1);
    g_free(value);
    value = g_strdup_printf("%d pixels", height);
    gtk_tree_store_insert_with_values(store, &it, &group, -1, 0, "Height", 1, value, -1);
    g_free(value);
    GtkWidget *list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(list), FALSE);
    gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(list), 12);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column =
        gtk_tree_view_column_new_with_attributes("Property", renderer, "markup", 0, nullptr);
    gtk_tree_view_column_set_min_width(column, 160);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(list),
        gtk_tree_view_column_new_with_attributes("Value", gtk_cell_renderer_text_new(), "text", 1,
                                                 nullptr));
    gtk_tree_view_expand_all(GTK_TREE_VIEW(list));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_container_set_border_width(GTK_CONTAINER(scroll), 10);
    return scroll;
}

/* ---- Sharing tab -------------------------------------------------------------------- */

/* Reads the folder's share, if it has one, from `net usershare`; returns
 * what an administrator must do when sharing isn't possible (free with
 * g_free), else nullptr. */
static char *load_sharing(props *properties) {
    XdUsershareStatus status;
    char *detail = nullptr;
    g_clear_pointer(&properties->shares, g_ptr_array_unref);
    properties->shares = xd_usershare_probe(&status, &detail);
    properties->share_ok = status == XD_USERSHARE_OK;
    char *help = xd_usershare_help_text(status, detail);
    g_free(detail);
    char *path = g_file_get_path(first(properties)->file);
    const XdUsershare *usershare = xd_usershare_find(properties->shares, path);
    g_free(path);
    g_free(properties->share_name);
    properties->share_name = usershare != nullptr ? g_strdup(usershare->name) : nullptr;
    properties->share_writable = usershare != nullptr && usershare->writable;
    return help;
}

static void sync_sharing(props *properties) {
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->share_check));
    gtk_widget_set_sensitive(properties->share_check, properties->share_ok);
    gtk_widget_set_sensitive(properties->share_name_entry, properties->share_ok && on);
    gtk_widget_set_sensitive(properties->share_write, properties->share_ok && on);
}

static void on_sharing_changed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    if (properties->syncing)
        return;
    sync_sharing(properties);
    mark_dirty(properties);
}

/* The check box and fields as the share is. */
static void show_sharing(props *properties) {
    properties->syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->share_check),
                                 properties->share_name != nullptr);
    char *name = properties->share_name != nullptr ? g_strdup(properties->share_name) : nullptr;
    if (name == nullptr) {
        char *path = g_file_get_path(first(properties)->file);
        name = xd_usershare_default_name(path);
        g_free(path);
    }
    gtk_entry_set_text(GTK_ENTRY(properties->share_name_entry), name);
    g_free(name);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->share_write),
                                 properties->share_writable);
    properties->syncing = FALSE;
    sync_sharing(properties);
}

/* XP's Sharing tab, as with Simple File Sharing: "Network sharing and
 * security". Without user shares turned on, what an administrator must
 * do, with the boxes greyed. */
static GtkWidget *build_sharing(props *properties) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *frame = gtk_frame_new("Network sharing and security");
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 10);
    gtk_container_add(GTK_CONTAINER(frame), inner);

    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name("network-workgroup", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(intro), icon, FALSE, FALSE, 0);
    GtkWidget *text = gtk_label_new("To share this folder with network users, select the first "
                                    "check box below and type a share name.");
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 44);
    gtk_box_pack_start(GTK_BOX(intro), text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(inner), intro, FALSE, FALSE, 0);

    properties->share_check =
        gtk_check_button_new_with_mnemonic("S_hare this folder on the network");
    gtk_box_pack_start(GTK_BOX(inner), properties->share_check, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *nl = gtk_label_new_with_mnemonic("Share _name:");
    properties->share_name_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(properties->share_name_entry), 80);
    gtk_label_set_mnemonic_widget(GTK_LABEL(nl), properties->share_name_entry);
    gtk_box_pack_start(GTK_BOX(row), nl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), properties->share_name_entry, TRUE, TRUE, 0);
    gtk_widget_set_margin_start(row, 20);
    gtk_box_pack_start(GTK_BOX(inner), row, FALSE, FALSE, 0);
    properties->share_write =
        gtk_check_button_new_with_mnemonic("Allow net_work users to change my files");
    gtk_widget_set_margin_start(properties->share_write, 20);
    gtk_box_pack_start(GTK_BOX(inner), properties->share_write, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);

    char *help = load_sharing(properties);
    if (help != nullptr) {
        GtkWidget *why = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *warn = gtk_image_new_from_icon_name("dialog-information", GTK_ICON_SIZE_DND);
        gtk_widget_set_valign(warn, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(why), warn, FALSE, FALSE, 0);
        GtkWidget *help_label = gtk_label_new(help);
        gtk_label_set_xalign(GTK_LABEL(help_label), 0);
        gtk_label_set_line_wrap(GTK_LABEL(help_label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(help_label), 50);
        gtk_label_set_selectable(GTK_LABEL(help_label), TRUE);
        gtk_box_pack_start(GTK_BOX(why), help_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), why, FALSE, FALSE, 0);
        g_free(help);
    }
    show_sharing(properties);
    g_signal_connect(properties->share_check, "toggled", G_CALLBACK(on_sharing_changed),
                     properties);
    g_signal_connect(properties->share_write, "toggled", G_CALLBACK(on_sharing_changed),
                     properties);
    g_signal_connect(properties->share_name_entry, "changed", G_CALLBACK(on_sharing_changed),
                     properties);
    return box;
}

/* Shares, unshares or changes the share, as the tab says. */
static void apply_sharing(props *properties, GString *errors) {
    if (properties->share_check == nullptr || !properties->share_ok)
        return;
    gboolean want = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->share_check));
    char *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(properties->share_name_entry))));
    gboolean writable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->share_write));
    char *path = g_file_get_path(first(properties)->file);
    GError *err = nullptr;
    gboolean same = properties->share_name != nullptr &&
                    g_ascii_strcasecmp(name, properties->share_name) == 0 &&
                    writable == properties->share_writable;
    if (!want && properties->share_name != nullptr) {
        if (!xd_usershare_delete(properties->share_name, &err))
            g_string_append_printf(errors, "%s\n", err->message);
    } else if (want && !same) {
        const char *problem = xd_usershare_name_problem(name);
        /* Another folder's share by that name would be replaced. */
        for (guint i = 0;
             problem == nullptr && properties->shares != nullptr && i < properties->shares->len;
             i++) {
            XdUsershare *usershare = g_ptr_array_index(properties->shares, i);
            if (g_ascii_strcasecmp(usershare->name, name) == 0 &&
                g_strcmp0(usershare->path, path) != 0)
                problem = "The share name you have chosen already exists for a different "
                          "resource. Please choose another share name.";
        }
        if (problem != nullptr) {
            g_string_append_printf(errors, "%s\n", problem);
        } else if (properties->share_name != nullptr &&
                   g_ascii_strcasecmp(name, properties->share_name) != 0 &&
                   !xd_usershare_delete(properties->share_name, &err)) {
            g_string_append_printf(errors, "%s\n", err->message);
        } else if (!xd_usershare_add(name, path, "", writable, &err)) {
            g_string_append_printf(errors, "%s\n", err->message);
        }
    }
    g_clear_error(&err);
    g_free(path);
    g_free(name);
    g_free(load_sharing(properties));
}

/* ---- applying --------------------------------------------------------------------- */

typedef struct recurse_job {
    GPtrArray *roots; /* GFile* */
    guint32 set, clear;
    gint64 gid; /* -1 = unchanged */
    guint failures;
    char *first_error;
} recurse_job;

static void recurse_job_free(gpointer user_data) {
    recurse_job *apply_job = user_data;
    g_ptr_array_unref(apply_job->roots);
    g_free(apply_job->first_error);
    g_free(apply_job);
}

static void note_error(recurse_job *apply_job, GError *err) {
    apply_job->failures++;
    if (apply_job->first_error == nullptr && err != nullptr)
        apply_job->first_error = g_strdup(err->message);
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
    recurse_job *apply_job = data;
    GQueue queue = G_QUEUE_INIT;
    for (guint i = 0; i < apply_job->roots->len; i++)
        g_queue_push_tail(&queue, g_object_ref(g_ptr_array_index(apply_job->roots, i)));
    GFile *dir;
    while ((dir = g_queue_pop_head(&queue)) != nullptr) {
        GError *err = nullptr;
        GFileEnumerator *en = g_file_enumerate_children(
            dir, COUNT_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &err);
        g_object_unref(dir);
        if (en == nullptr) {
            note_error(apply_job, err);
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
            if (apply_job->gid >= 0 &&
                !g_file_set_attribute_uint32(child, G_FILE_ATTRIBUTE_UNIX_GID,
                                             (guint32)apply_job->gid,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &err)) {
                note_error(apply_job, err);
                g_clear_error(&err);
            }
            guint32 mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) &
                           07777u;
            guint32 next = next_mode(mode, is_dir, apply_job->set, apply_job->clear);
            if (next != mode &&
                !g_file_set_attribute_uint32(child, G_FILE_ATTRIBUTE_UNIX_MODE, next,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &err)) {
                note_error(apply_job, err);
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

static void show_error(props *properties, const char *msg) {
    xd_message_box(GTK_WINDOW(properties->window), "Error Applying Attributes", XD_MB_ERROR,
                   XD_MB_OK, FALSE, msg);
}

static void reload(props *properties);

static void finish_apply(props *properties) {
    gtk_widget_set_sensitive(properties->window, TRUE);
    if (properties->close_after_apply)
        gtk_widget_destroy(properties->window);
    else
        reload(properties);
}

static void on_recurse_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    props *properties = data;
    GtkWidget *window = properties->window; /* held while the job ran */
    properties->applying = FALSE;
    if (properties->closed) {
        g_free(properties);
        g_object_unref(window);
        return;
    }
    recurse_job *apply_job = g_task_get_task_data(G_TASK(res));
    if (apply_job->failures > 0) {
        char *msg =
            g_strdup_printf("%u item%s inside could not be changed.\n\n%s", apply_job->failures,
                            apply_job->failures == 1 ? "" : "s",
                            apply_job->first_error != nullptr ? apply_job->first_error : "");
        show_error(properties, msg);
        g_free(msg);
        properties->close_after_apply = FALSE;
    }
    finish_apply(properties); /* may close the window, freeing `properties` */
    g_object_unref(window);
}

/* Applies the changes, reporting any failures. The recursive part runs in
 * the background; finish_apply runs when everything is done. */
static void apply(props *properties) {
    GString *errors = g_string_new(nullptr);
    guint32 set, clear;
    wanted_bits(properties, &set, &clear, FALSE);
    gint64 gid = -1;
    const char *gid_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->group));
    if (gid_id != nullptr) {
        gint64 chosen_gid = g_ascii_strtoll(gid_id, nullptr, 10);
        if (chosen_gid != properties->group_init)
            gid = chosen_gid;
    }

    apply_shortcut(properties, errors);
    /* Before a rename: the share is of the folder's path as it is. */
    apply_sharing(properties, errors);
    for (guint i = 0; i < properties->entries->len; i++) {
        entry *selected = g_ptr_array_index(properties->entries, i);
        GError *err = nullptr;
        if (properties->can_chmod && selected->has_unix && !selected->is_link) {
            guint32 next = (selected->mode & ~clear) | set;
            if (next != selected->mode &&
                !g_file_set_attribute_uint32(selected->file, G_FILE_ATTRIBUTE_UNIX_MODE, next,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &err)) {
                g_string_append_printf(errors, "%s\n", err->message);
                g_clear_error(&err);
            }
            if (gid >= 0 && !g_file_set_attribute_uint32(
                                selected->file, G_FILE_ATTRIBUTE_UNIX_GID, (guint32)gid,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &err)) {
                g_string_append_printf(errors, "%s\n", err->message);
                g_clear_error(&err);
            }
        }
        if (single(properties) && properties->attrs_ok &&
            properties->attrs_want != properties->attrs_have) {
            if (write_attr_flags(selected->file, properties->attrs_want, &err))
                properties->attrs_have = properties->attrs_want;
            else {
                g_string_append_printf(errors, "%s\n", err->message);
                g_clear_error(&err);
            }
        }

        /* Rename last: the name box (one file) or the Hidden box. */
        char *want = nullptr;
        const char *have = display_name(selected);
        if (single(properties)) {
            const char *typed = gtk_entry_get_text(GTK_ENTRY(properties->name));
            if (g_strcmp0(typed, properties->orig_name) != 0)
                want = g_strdup(typed);
        } else if (check_state(properties->hidden) != BIT_MIXED) {
            gboolean hide = check_state(properties->hidden) == BIT_ALL;
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
                GFile *renamed = g_file_set_display_name(selected->file, want, nullptr, &err);
                if (renamed != nullptr) {
                    g_object_unref(selected->file);
                    selected->file = renamed;
                } else {
                    g_string_append_printf(errors, "%s\n", err->message);
                    g_clear_error(&err);
                }
            }
            g_free(want);
        }
    }

    if (errors->len > 0) {
        show_error(properties, errors->str);
        properties->close_after_apply = FALSE;
    }
    g_string_free(errors, TRUE);

    if (properties->recursive != nullptr && properties->can_chmod &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->recursive))) {
        recurse_job *apply_job = g_new0(recurse_job, 1);
        apply_job->roots = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < properties->entries->len; i++) {
            entry *selected = g_ptr_array_index(properties->entries, i);
            if (selected->is_dir)
                g_ptr_array_add(apply_job->roots, g_object_ref(selected->file));
        }
        wanted_bits(properties, &apply_job->set, &apply_job->clear, TRUE);
        apply_job->gid = gid;
        gtk_widget_set_sensitive(properties->window, FALSE);
        g_object_ref(properties->window);
        properties->applying = TRUE;
        GTask *task = g_task_new(nullptr, nullptr, on_recurse_done, properties);
        g_task_set_task_data(task, apply_job, recurse_job_free);
        g_task_run_in_thread(task, recurse_thread);
        g_object_unref(task);
        return;
    }
    finish_apply(properties);
}

/* ---- window ----------------------------------------------------------------------- */

static void set_title(props *properties) {
    char *title = single(properties)
                      ? g_strdup_printf("%s Properties", display_name(first(properties)))
                      : g_strdup("Properties");
    gtk_window_set_title(GTK_WINDOW(properties->window), title);
    g_free(title);
}

/* Re-reads the files after Apply and resets the page to match. */
static void reload(props *properties) {
    for (guint i = 0; i < properties->entries->len; i++) {
        entry *selected = g_ptr_array_index(properties->entries, i);
        GError *err = nullptr;
        if (!entry_load(selected, &err))
            g_clear_error(&err);
    }
    properties->syncing = TRUE;
    if (single(properties)) {
        g_free(properties->orig_name);
        properties->orig_name = g_strdup(display_name(first(properties)));
        gtk_entry_set_text(GTK_ENTRY(properties->name), properties->orig_name);
        set_check(properties->hidden, name_hidden(properties->orig_name) ? BIT_ALL : BIT_NONE);
    }
    if (properties->recursive != nullptr)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->recursive), FALSE);
    properties->syncing = FALSE;
    load_permissions(properties);
    if (properties->share_check != nullptr)
        show_sharing(properties);
    set_title(properties);
    gtk_widget_set_sensitive(properties->apply_button, FALSE);
}

static void on_response(GtkDialog *dlg, gint response, gpointer data) {
    props *properties = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
    } else if (response == GTK_RESPONSE_OK) {
        if (gtk_widget_get_sensitive(properties->apply_button)) {
            properties->close_after_apply = TRUE;
            apply(properties);
        } else {
            gtk_widget_destroy(GTK_WIDGET(dlg));
        }
    } else {
        gtk_widget_destroy(GTK_WIDGET(dlg));
    }
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    stop_count(properties);
    g_ptr_array_unref(properties->entries);
    g_free(properties->orig_name);
    g_free(properties->sc_path);
    g_free(properties->sc_icon);
    xd_launcher_clear(&properties->sc_orig);
    g_clear_pointer(&properties->shares, g_ptr_array_unref);
    g_free(properties->share_name);
    /* Closed while the subfolders are being changed: freed when that's
     * done. */
    if (properties->applying)
        properties->closed = TRUE;
    else
        g_free(properties);
}

GtkWidget *xd_properties_show(GtkWindow *parent, GList *files) {
    g_return_val_if_fail(files != nullptr, nullptr);
    props *properties = g_new0(props, 1);
    properties->entries = g_ptr_array_new_with_free_func(entry_free);
    uid_t me = geteuid();
    properties->can_chmod = TRUE;
    GString *failed = g_string_new(nullptr);
    for (GList *list = files; list != nullptr; list = list->next) {
        entry *selected = g_new0(entry, 1);
        selected->file = g_object_ref(list->data);
        GError *err = nullptr;
        if (!entry_load(selected, &err)) {
            g_string_append_printf(failed, "%s\n", err != nullptr ? err->message : "");
            g_clear_error(&err);
            entry_free(selected);
            continue;
        }
        g_ptr_array_add(properties->entries, selected);
        if (!selected->has_unix || (me != 0 && selected->uid != me))
            properties->can_chmod = FALSE;
    }

    if (properties->entries->len == 0) {
        xd_message_box(parent, "Properties", XD_MB_ERROR, XD_MB_OK, FALSE, failed->str);
        g_string_free(failed, TRUE);
        g_ptr_array_unref(properties->entries);
        g_free(properties);
        return nullptr;
    }

    properties->window = gtk_dialog_new();
    if (parent != nullptr) {
        gtk_window_set_transient_for(GTK_WINDOW(properties->window), parent);
        GtkApplication *app = gtk_window_get_application(parent);
        if (app != nullptr)
            gtk_window_set_application(GTK_WINDOW(properties->window), app);
    }
    gtk_window_set_resizable(GTK_WINDOW(properties->window), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(properties->window), "document-properties");
    g_signal_connect(properties->window, "destroy", G_CALLBACK(on_destroy), properties);

    g_string_free(failed, TRUE);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(notebook), 6);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(properties->window))),
                      notebook);
    xd_gtk_notebook_ctrl_tab(properties->window, GTK_NOTEBOOK(notebook));
    properties->ok_button =
        gtk_dialog_add_button(GTK_DIALOG(properties->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties->window), "Cancel", GTK_RESPONSE_CANCEL);
    properties->apply_button =
        gtk_dialog_add_button(GTK_DIALOG(properties->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(properties->window), GTK_RESPONSE_OK);
    g_signal_connect(properties->window, "response", G_CALLBACK(on_response), properties);

    properties->syncing = TRUE;
    GtkWidget *general = build_general(properties);
    GtkWidget *perms = build_permissions(properties);
    properties->syncing = TRUE;
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), general, gtk_label_new("General"));
    /* A launcher's Shortcut tab follows General, as a .lnk's does. */
    load_shortcut(properties);
    if (properties->sc_path != nullptr)
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_shortcut(properties),
                                 gtk_label_new("Shortcut"));
    /* XP's Sharing tab, for one folder on this computer. */
    if (single(properties) && first(properties)->is_dir &&
        g_file_is_native(first(properties)->file))
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_sharing(properties),
                                 gtk_label_new("Sharing"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), perms, gtk_label_new("Permissions"));
    GtkWidget *summary = build_image_summary(properties);
    if (summary != nullptr)
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), summary, gtk_label_new("Summary"));
    if (single(properties))
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_details(properties),
                                 gtk_label_new("Details"));
    properties->syncing = FALSE;
    sync_permission_text(properties, TRUE);
    gtk_widget_set_sensitive(properties->apply_button, FALSE);
    set_title(properties);
    start_count(properties);
    gtk_widget_show_all(properties->window);
    gtk_widget_grab_focus(properties->ok_button);
    return properties->window;
}

/* ---- a drive's Properties (My Computer) ------------------------------------------- */

typedef struct drive_space {
    guint64 total, free;
} drive_space;

/* XP's 3D pie: blue for used space, magenta for free, a darker rim
 * beneath. */
static void pie_slice(cairo_t *cairo, double cx, double cy, double rx, double ry, double from,
                      double to) {
    cairo_save(cairo);
    cairo_translate(cairo, cx, cy);
    cairo_scale(cairo, rx, ry);
    cairo_move_to(cairo, 0, 0);
    cairo_arc(cairo, 0, 0, 1, from, to);
    cairo_close_path(cairo);
    cairo_restore(cairo);
}

static gboolean draw_drive_pie(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    const drive_space *space = data;
    double width = gtk_widget_get_allocated_width(widget),
           height = gtk_widget_get_allocated_height(widget);
    constexpr double depth = 10;
    double rx = width / 2 - 2, ry = (height - depth) / 2 - 2, cx = width / 2, cy = ry + 2;
    double used = space->total > 0 ? (double)(space->total - MIN(space->free, space->total)) /
                                         (double)space->total
                                   : 0;
    double split = -G_PI / 2 + used * 2 * G_PI;
    for (int layer = (int)depth; layer >= 0; layer--) {
        gboolean top = layer == 0;
        pie_slice(cairo, cx, cy + layer, rx, ry, -G_PI / 2, split);
        cairo_set_source_rgb(cairo, 0.0, 0.0, top ? 1.0 : 0.5);
        cairo_fill(cairo);
        pie_slice(cairo, cx, cy + layer, rx, ry, split, 3 * G_PI / 2);
        cairo_set_source_rgb(cairo, top ? 1.0 : 0.5, 0.0, top ? 1.0 : 0.5);
        cairo_fill(cairo);
    }
    return FALSE;
}

static gboolean draw_swatch(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    gboolean used = GPOINTER_TO_INT(data);
    cairo_rectangle(cairo, 0.5, 0.5, gtk_widget_get_allocated_width(widget) - 1,
                    gtk_widget_get_allocated_height(widget) - 1);
    cairo_set_source_rgb(cairo, used ? 0.0 : 1.0, 0.0, 1.0);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
    return FALSE;
}

/* "21,476,839,424 bytes" and "20.0 GB", in their columns, as XP has them. */
static void space_row(GtkWidget *grid, int row, const char *title, int swatch, guint64 bytes) {
    if (swatch >= 0) {
        GtkWidget *sw = gtk_drawing_area_new();
        gtk_widget_set_size_request(sw, 14, 14);
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        g_signal_connect(sw, "draw", G_CALLBACK(draw_swatch), GINT_TO_POINTER(swatch));
        gtk_grid_attach(GTK_GRID(grid), sw, 0, row, 1, 1);
    }
    GtkWidget *title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    gtk_grid_attach(GTK_GRID(grid), title_label, 1, row, 1, 1);
    GString *string = g_string_new(nullptr);
    append_grouped(string, bytes);
    g_string_append(string, " bytes");
    GtkWidget *bytes_label = gtk_label_new(string->str);
    gtk_label_set_xalign(GTK_LABEL(bytes_label), 1);
    gtk_label_set_selectable(GTK_LABEL(bytes_label), TRUE);
    gtk_widget_set_can_focus(bytes_label, FALSE);
    gtk_widget_set_hexpand(bytes_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), bytes_label, 2, row, 1, 1);
    g_string_truncate(string, 0);
    append_short_size(string, bytes);
    GtkWidget *short_label = gtk_label_new(string->str);
    gtk_label_set_xalign(GTK_LABEL(short_label), 1);
    gtk_grid_attach(GTK_GRID(grid), short_label, 3, row, 1, 1);
    g_string_free(string, TRUE);
}

/* The drive's label, changed through UDisks2 on Apply or OK. */
typedef struct drive_props {
    GtkWidget *dlg, *label;
    GFile *root;
    char *name, *orig;
    gboolean busy, close_after;
} drive_props;

static void drive_props_free(gpointer data) {
    drive_props *dp = data;
    g_object_unref(dp->root);
    g_free(dp->name);
    g_free(dp->orig);
    g_free(dp);
}

static void on_drive_label_changed([[maybe_unused]] GtkEditable *ed, gpointer data) {
    drive_props *dp = data;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dp->dlg), GTK_RESPONSE_APPLY,
                                      !dp->busy && dp->orig != nullptr &&
                                          strcmp(gtk_entry_get_text(GTK_ENTRY(dp->label)),
                                                 dp->orig) != 0);
}

static void drive_label_set(gboolean ok, gpointer data) {
    drive_props *dp = data;
    dp->busy = FALSE;
    gtk_widget_set_sensitive(dp->dlg, TRUE);
    if (ok) {
        g_free(dp->orig);
        dp->orig = g_strdup(gtk_entry_get_text(GTK_ENTRY(dp->label)));
        if (dp->close_after) {
            gtk_widget_destroy(dp->dlg);
            return;
        }
    }
    dp->close_after = FALSE;
    on_drive_label_changed(nullptr, dp);
}

static void on_drive_response(GtkDialog *dlg, gint response, gpointer data) {
    drive_props *dp = data;
    if (dp->busy)
        return;
    const char *text = gtk_entry_get_text(GTK_ENTRY(dp->label));
    gboolean changed = dp->orig != nullptr && strcmp(text, dp->orig) != 0;
    if ((response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY) && changed) {
        dp->busy = TRUE;
        dp->close_after = response == GTK_RESPONSE_OK;
        /* Until UDisks2 (and polkit) answer. */
        gtk_widget_set_sensitive(GTK_WIDGET(dlg), FALSE);
        xd_drive_set_label(GTK_WINDOW(dlg), dp->root, dp->name, text, drive_label_set, dp);
        return;
    }
    if (response != GTK_RESPONSE_APPLY)
        gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void on_disk_cleanup([[maybe_unused]] GtkButton *button, gpointer data) {
    drive_props *dp = data;
    xd_drive_cleanup(GTK_WINDOW(dp->dlg), dp->root, dp->name);
}

GtkWidget *xd_properties_show_drive(GtkWindow *parent, GFile *item) {
    GPtrArray *drives = xd_computer_drives();
    const XdDrive *drive = nullptr;
    for (guint i = 0; i < drives->len && drive == nullptr; i++) {
        const XdDrive *candidate = g_ptr_array_index(drives, i);
        if (g_file_equal(candidate->root, item) && strcmp(candidate->kind, "File Folder") != 0)
            drive = candidate;
    }
    if (drive == nullptr) {
        g_ptr_array_unref(drives);
        return nullptr;
    }
    char *title = g_strdup_printf("%s Properties", drive->name);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, parent, GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 "OK", GTK_RESPONSE_OK, "Cancel",
                                                 GTK_RESPONSE_CANCEL, "_Apply",
                                                 GTK_RESPONSE_APPLY, nullptr);
    g_free(title);
    if (parent != nullptr) {
        GtkApplication *app = gtk_window_get_application(parent);
        if (app != nullptr)
            gtk_window_set_application(GTK_WINDOW(dlg), app);
    }
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "document-properties");
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), GTK_RESPONSE_APPLY, FALSE);
    drive_props *dp = g_new0(drive_props, 1);
    dp->dlg = dlg;
    dp->root = g_object_ref(drive->root);
    dp->name = g_strdup(drive->name);
    g_object_set_data_full(G_OBJECT(dlg), "xd-drive", dp, drive_props_free);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), nb);
    xd_gtk_notebook_ctrl_tab(dlg, GTK_NOTEBOOK(nb));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    int row = 0;
    GtkWidget *icon = drive->icon != nullptr
                          ? gtk_image_new_from_gicon(drive->icon, GTK_ICON_SIZE_DND)
                          : gtk_image_new_from_icon_name("drive-harddisk", GTK_ICON_SIZE_DND);
    gtk_widget_set_halign(icon, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), icon, 0, row, 2, 1);
    /* The file system's label, as XP's; greyed where UDisks2 doesn't
     * know the drive (a network drive). */
    GtkWidget *label = gtk_entry_new();
    dp->label = label;
    gboolean can_label = xd_drive_label(drive->root, &dp->orig);
    gtk_entry_set_text(GTK_ENTRY(label), can_label ? dp->orig : drive->name);
    gtk_widget_set_sensitive(label, can_label);
    g_signal_connect(label, "changed", G_CALLBACK(on_drive_label_changed), dp);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), label, 2, row++, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, 4,
                    1);

    char *fs_type = nullptr;
    GFileInfo *fs = g_file_query_filesystem_info(drive->root, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
                                                 nullptr, nullptr);
    if (fs != nullptr) {
        fs_type = g_strdup(g_file_info_get_attribute_string(fs, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
        g_object_unref(fs);
    }
    const struct {
        const char *title, *value;
    } info[] = {{"Type:", drive->kind}, {"File system:", fs_type != nullptr ? fs_type : ""}};
    for (size_t i = 0; i < G_N_ELEMENTS(info); i++) {
        GtkWidget *title_label = gtk_label_new(info[i].title);
        gtk_label_set_xalign(GTK_LABEL(title_label), 0);
        gtk_grid_attach(GTK_GRID(grid), title_label, 0, row, 2, 1);
        GtkWidget *value_widget = value_label(info[i].value);
        gtk_grid_attach(GTK_GRID(grid), value_widget, 2, row++, 2, 1);
    }
    g_free(fs_type);
    gtk_grid_attach(GTK_GRID(grid), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, 4,
                    1);
    drive_space *space = g_new(drive_space, 1);
    space->total = drive->total;
    space->free = MIN(drive->free, drive->total);
    space_row(grid, row++, "Used space:", 1, space->total - space->free);
    space_row(grid, row++, "Free space:", 0, space->free);
    gtk_grid_attach(GTK_GRID(grid), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, 4,
                    1);
    space_row(grid, row++, "Capacity:", -1, space->total);

    GtkWidget *pie = gtk_drawing_area_new();
    gtk_widget_set_size_request(pie, 150, 80);
    gtk_widget_set_halign(pie, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(pie, 8);
    g_object_set_data_full(G_OBJECT(pie), "xd-space", space, g_free);
    g_signal_connect(pie, "draw", G_CALLBACK(draw_drive_pie), space);
    gtk_grid_attach(GTK_GRID(grid), pie, 0, row, 4, 1);
    gtk_widget_set_visible(pie, space->total > 0);
    gtk_widget_set_no_show_all(pie, space->total == 0);
    /* XP's Disk Cleanup button, right of the pie. */
    if (xd_drive_can_cleanup(drive->root, drive->kind)) {
        GtkWidget *clean = gtk_button_new_with_mnemonic("Disk Clean_up");
        gtk_widget_set_halign(clean, GTK_ALIGN_END);
        gtk_widget_set_valign(clean, GTK_ALIGN_END);
        g_signal_connect(clean, "clicked", G_CALLBACK(on_disk_cleanup), dp);
        gtk_grid_attach(GTK_GRID(grid), clean, 3, row + 1, 1, 1);
    }
    row++;
    GtkWidget *caption = gtk_label_new(drive->name);
    gtk_grid_attach(GTK_GRID(grid), caption, 1, row++, 2, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), grid, gtk_label_new("General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),
                             xd_drive_tools_page(dlg, drive->root, drive->name, drive->kind),
                             gtk_label_new("Tools"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), xd_drive_hardware_page(dlg, drive->root),
                             gtk_label_new("Hardware"));
    GtkWidget *encryption = xd_encrypted_page(dlg, drive->root, drive->name);
    if (encryption != nullptr)
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), encryption, gtk_label_new("Encryption"));
    g_ptr_array_unref(drives);

    g_signal_connect(dlg, "response", G_CALLBACK(on_drive_response), dp);
    gtk_widget_show_all(dlg);
    return dlg;
}
