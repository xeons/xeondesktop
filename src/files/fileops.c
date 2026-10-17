/* SPDX-License-Identifier: MIT
 * File operations (copy, move, shortcut, Recycle Bin, delete) with XP's
 * progress dialog and questions, and the file clipboard. See fileops.h.
 */
#include "fileops.h"

#include <math.h>

#include "../gtkapp/gtkapp.h"
#include "folderview.h"

static constexpr int SHOW_DELAY_MS = 600; /* quick operations show no dialog */
static constexpr int UPDATE_MS = 50;

#define INFO_ATTRS                                                                             \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE                           \
                                   "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME                   \
                                   "," G_FILE_ATTRIBUTE_TIME_MODIFIED

/* Answers to the questions; also the buttons' order. */
typedef enum { ANSWER_YES, ANSWER_YES_ALL, ANSWER_NO, ANSWER_CANCEL } answer;

typedef struct job {
    XdFileOpKind kind;
    GPtrArray *sources; /* GFile* */
    GFile *dest;
    GtkWindow *parent; /* weak */
    XdFileOpDone done;
    gpointer user;
    GCancellable *cancel;
    GList *created; /* GFile*, built by the worker */

    /* Shared with the worker, under `lock`. */
    GMutex lock;
    GCond cond;
    gboolean scanning;
    char *current; /* the item being worked on */
    char *from, *to;
    guint64 total_bytes, done_bytes, file_bytes;
    guint total_items, done_items;
    /* A question for the main thread, and its answer. */
    char *q_title, *q_text;
    xd_mb_icon q_icon;
    const char *const *q_labels;
    int q_count, q_default, q_cancel;
    int q_answer;
    gboolean q_answered;

    /* Worker only: "Yes to All" answers. */
    bool replace_files, replace_folders, delete_unrecyclable;
    bool wrote; /* the last transfer() put something at its destination */

    /* Main thread: the progress dialog. */
    GtkWidget *dialog, *anim, *name_label, *from_label, *bar, *time_label, *cancel_button;
    guint show_timer, update_timer;
    gboolean asking; /* a question is up: the dialog waits for it */
    gint64 started;
    gint64 rate_since; /* when byte counting began, after the scan */
} job;

/* ---- talking to the main thread -------------------------------------------------- */

static gboolean ask_in_main(gpointer data) {
    job *j = data;
    GtkWindow *parent = j->dialog != nullptr && gtk_widget_get_visible(j->dialog)
                            ? GTK_WINDOW(j->dialog)
                            : j->parent;
    g_mutex_lock(&j->lock);
    char *title = g_strdup(j->q_title);
    char *text = g_strdup(j->q_text);
    g_mutex_unlock(&j->lock);
    j->asking = TRUE;
    int r = xd_message_box_choose(parent, title, j->q_icon, text, j->q_labels, j->q_count,
                                  j->q_default, j->q_cancel);
    j->asking = FALSE;
    g_free(title);
    g_free(text);
    g_mutex_lock(&j->lock);
    j->q_answer = r;
    j->q_answered = TRUE;
    g_cond_signal(&j->cond);
    g_mutex_unlock(&j->lock);
    return G_SOURCE_REMOVE;
}

/* Worker: shows a message box on the main thread and waits for it. Takes
 * `text`. */
static int ask(job *j, const char *title, xd_mb_icon icon, char *text,
               const char *const *labels, int n, int default_index, int cancel_index) {
    g_mutex_lock(&j->lock);
    g_free(j->q_title);
    g_free(j->q_text);
    j->q_title = g_strdup(title);
    j->q_text = text;
    j->q_icon = icon;
    j->q_labels = labels;
    j->q_count = n;
    j->q_default = default_index;
    j->q_cancel = cancel_index;
    j->q_answered = FALSE;
    g_main_context_invoke(nullptr, ask_in_main, j);
    while (!j->q_answered)
        g_cond_wait(&j->cond, &j->lock);
    int r = j->q_answer;
    g_mutex_unlock(&j->lock);
    return r;
}

static const char *const replace_buttons[] = {"_Yes", "Yes to _All", "_No", "Cancel"};

/* Yes / Yes to All / No / Cancel. Takes `text`. */
static answer ask_replace(job *j, const char *title, xd_mb_icon icon, char *text,
                          int default_index) {
    int r = ask(j, title, icon, text, replace_buttons, 4, default_index, ANSWER_CANCEL);
    return r >= ANSWER_YES && r <= ANSWER_CANCEL ? (answer)r : ANSWER_CANCEL;
}

static const char *verb(const job *j) {
    switch (j->kind) {
    case XD_FILEOP_COPY:
        return "copy";
    case XD_FILEOP_MOVE:
        return "move";
    case XD_FILEOP_LINK:
        return "create shortcut to";
    case XD_FILEOP_TRASH:
    case XD_FILEOP_DELETE:
        break;
    }
    return "delete";
}

static char *display_name(GFile *f) {
    char *base = g_file_get_basename(f);
    char *name = base != nullptr ? g_filename_display_name(base) : g_file_get_parse_name(f);
    g_free(base);
    return name;
}

/* Worker: Windows' "Cannot copy 'x': reason". Returns whether to go on
 * (Skip) rather than stop. Takes `err`. */
static bool report_error(job *j, GFile *f, GError *err) {
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        g_cancellable_is_cancelled(j->cancel)) {
        g_error_free(err);
        return false;
    }
    char *name = display_name(f);
    char *text = g_strdup_printf("Cannot %s %s: %s", verb(j), name, err->message);
    g_free(name);
    g_error_free(err);
    static const char *const buttons[] = {"_Skip", "Cancel"};
    return ask(j, "Error", XD_MB_ERROR, text, buttons, 2, 0, 1) == 0;
}

static void set_current(job *j, GFile *src, GFile *dst_dir) {
    g_mutex_lock(&j->lock);
    g_free(j->current);
    j->current = display_name(src);
    GFile *parent = g_file_get_parent(src);
    char *from = parent != nullptr ? display_name(parent) : g_strdup("");
    g_clear_object(&parent);
    char *to = dst_dir != nullptr ? display_name(dst_dir) : nullptr;
    g_free(j->from);
    g_free(j->to);
    j->from = from;
    j->to = to;
    j->file_bytes = 0;
    g_mutex_unlock(&j->lock);
}

static void add_done(job *j, guint64 bytes, guint items) {
    g_mutex_lock(&j->lock);
    j->done_bytes += bytes;
    j->done_items += items;
    j->file_bytes = 0;
    g_mutex_unlock(&j->lock);
}

static void copy_progress(goffset current, [[maybe_unused]] goffset total, gpointer data) {
    job *j = data;
    g_mutex_lock(&j->lock);
    j->file_bytes = current > 0 ? (guint64)current : 0;
    g_mutex_unlock(&j->lock);
}

/* ---- worker: sizes ------------------------------------------------------------------ */

/* Adds up a tree's bytes and items (without following links). */
static void measure(job *j, GFile *f, guint64 *bytes, guint *items) {
    if (g_cancellable_is_cancelled(j->cancel))
        return;
    GFileInfo *info = g_file_query_info(f, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        j->cancel, nullptr);
    if (info == nullptr)
        return;
    (*items)++;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
        goffset size = g_file_info_get_size(info);
        *bytes += size > 0 ? (guint64)size : 0;
        g_object_unref(info);
        return;
    }
    g_object_unref(info);
    GFileEnumerator *en = g_file_enumerate_children(
        f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel, nullptr);
    if (en == nullptr)
        return;
    GFileInfo *child_info;
    while ((child_info = g_file_enumerator_next_file(en, j->cancel, nullptr)) != nullptr) {
        GFile *child = g_file_get_child(f, g_file_info_get_name(child_info));
        measure(j, child, bytes, items);
        g_object_unref(child);
        g_object_unref(child_info);
    }
    g_object_unref(en);
}

static void measure_all(job *j, GPtrArray *files) {
    guint64 bytes = 0;
    guint items = 0;
    for (guint i = 0; i < files->len; i++)
        measure(j, g_ptr_array_index(files, i), &bytes, &items);
    g_mutex_lock(&j->lock);
    j->total_bytes += bytes;
    j->total_items += items;
    j->scanning = FALSE;
    g_mutex_unlock(&j->lock);
}

/* ---- worker: deleting -------------------------------------------------------------- */

/* Deletes a tree, children first. Returns false to stop the operation;
 * a skipped item leaves its folder in place. */
static bool delete_tree(job *j, GFile *f, bool *removed) {
    *removed = false;
    if (g_cancellable_is_cancelled(j->cancel))
        return false;
    GError *err = nullptr;
    GFileInfo *info = g_file_query_info(f, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        j->cancel, &err);
    if (info == nullptr)
        return report_error(j, f, err);
    gboolean is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    goffset size = g_file_info_get_size(info);
    g_object_unref(info);
    bool all_gone = true;
    if (is_dir) {
        GFileEnumerator *en = g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        j->cancel, &err);
        if (en == nullptr)
            return report_error(j, f, err);
        GFileInfo *child_info;
        bool go_on = true;
        while (go_on && (child_info = g_file_enumerator_next_file(en, j->cancel, nullptr)) !=
                            nullptr) {
            GFile *child = g_file_get_child(f, g_file_info_get_name(child_info));
            bool gone = false;
            go_on = delete_tree(j, child, &gone);
            all_gone = all_gone && gone;
            g_object_unref(child);
            g_object_unref(child_info);
        }
        g_object_unref(en);
        if (!go_on)
            return false;
    }
    if (!all_gone)
        return true;
    set_current(j, f, nullptr);
    if (!g_file_delete(f, j->cancel, &err))
        return report_error(j, f, err);
    add_done(j, !is_dir && size > 0 ? (guint64)size : 0, 1);
    *removed = true;
    return true;
}

static bool delete_top(job *j, GFile *f) {
    bool removed = false;
    return delete_tree(j, f, &removed);
}

/* Recycle Bin: when the file's volume has none, Windows offers to delete
 * it permanently. */
static bool trash_one(job *j, GFile *f) {
    set_current(j, f, nullptr);
    GError *err = nullptr;
    if (g_file_trash(f, j->cancel, &err)) {
        add_done(j, 0, 1);
        return true;
    }
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        return report_error(j, f, err);
    g_error_free(err);
    if (!j->delete_unrecyclable) {
        char *name = display_name(f);
        char *text = g_strdup_printf(
            "%s can't be moved to the Recycle Bin. Do you want to delete it permanently?", name);
        g_free(name);
        switch (ask_replace(j, "Confirm File Delete", XD_MB_WARNING, text, ANSWER_NO)) {
        case ANSWER_YES:
            break;
        case ANSWER_YES_ALL:
            j->delete_unrecyclable = true;
            break;
        case ANSWER_NO:
            add_done(j, 0, 1);
            return true;
        case ANSWER_CANCEL:
            return false;
        }
    }
    bool ok = delete_top(j, f);
    add_done(j, 0, 1);
    return ok;
}

/* ---- worker: copying and moving --------------------------------------------------------- */

/* "Copy of x", "Copy (2) of x", ... in `dir`: Windows' names for a copy
 * or shortcut beside the original. */
static GFile *numbered_name(GFile *dir, const char *word, const char *prep, const char *name) {
    for (guint n = 1;; n++) {
        char *candidate = n == 1 ? g_strdup_printf("%s %s %s", word, prep, name)
                                 : g_strdup_printf("%s (%u) %s %s", word, n, prep, name);
        GFile *f = g_file_get_child(dir, candidate);
        g_free(candidate);
        if (!g_file_query_exists(f, nullptr))
            return f;
        g_object_unref(f);
    }
}

static char *describe(GFileInfo *info) {
    goffset size = g_file_info_get_size(info);
    char *size_text = xd_format_size(size > 0 ? (guint64)size : 0);
    GDateTime *utc = g_file_info_get_modification_date_time(info);
    GDateTime *mtime = utc != nullptr ? g_date_time_to_local(utc) : nullptr;
    if (utc != nullptr)
        g_date_time_unref(utc);
    char *date = mtime != nullptr ? g_date_time_format(mtime, "%A, %B %e, %Y, %l:%M:%S %p")
                                  : g_strdup("an unknown date");
    if (mtime != nullptr)
        g_date_time_unref(mtime);
    char *out = g_strdup_printf("        %s\n        modified on %s", size_text, date);
    g_free(size_text);
    g_free(date);
    return out;
}

static answer ask_replace_file(job *j, GFileInfo *existing, GFileInfo *incoming) {
    char *a = describe(existing);
    char *b = describe(incoming);
    char *text = g_strdup_printf("This folder already contains a file named '%s'.\n\n"
                                 "Would you like to replace the existing file\n\n%s\n\n"
                                 "with this one?\n\n%s",
                                 g_file_info_get_display_name(existing), a, b);
    g_free(a);
    g_free(b);
    return ask_replace(j, "Confirm File Replace", XD_MB_QUESTION, text, ANSWER_YES);
}

static answer ask_replace_folder(job *j, GFileInfo *existing) {
    char *text = g_strdup_printf(
        "This folder already contains a folder named '%s'.\n\n"
        "If the files in the existing folder have the same name as files in the folder you "
        "are moving or copying, they will be replaced. Do you still want to move or copy the "
        "folder?",
        g_file_info_get_display_name(existing));
    return ask_replace(j, "Confirm Folder Replace", XD_MB_QUESTION, text, ANSWER_YES);
}

static bool transfer(job *j, GFile *src, GFile *dst);

static bool transfer_children(job *j, GFile *src, GFile *dst) {
    GError *err = nullptr;
    GFileEnumerator *en = g_file_enumerate_children(src, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                    j->cancel, &err);
    if (en == nullptr)
        return report_error(j, src, err);
    bool go_on = true;
    GFileInfo *info;
    while (go_on && (info = g_file_enumerator_next_file(en, j->cancel, nullptr)) != nullptr) {
        const char *name = g_file_info_get_name(info);
        GFile *s = g_file_get_child(src, name);
        GFile *d = g_file_get_child(dst, name);
        go_on = transfer(j, s, d);
        g_object_unref(s);
        g_object_unref(d);
        g_object_unref(info);
    }
    g_object_unref(en);
    return go_on;
}

/* Copies or moves `src` to `dst`, asking about anything in the way. A
 * folder over a folder merges into it. Returns false to stop. */
static bool transfer(job *j, GFile *src, GFile *dst) {
    if (g_cancellable_is_cancelled(j->cancel))
        return false;
    GFile *dst_dir = g_file_get_parent(dst);
    set_current(j, src, dst_dir);
    g_clear_object(&dst_dir);
    GError *err = nullptr;
    GFileInfo *si = g_file_query_info(src, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      j->cancel, &err);
    if (si == nullptr)
        return report_error(j, src, err);
    gboolean src_dir = g_file_info_get_file_type(si) == G_FILE_TYPE_DIRECTORY;
    goffset size = g_file_info_get_size(si);
    guint64 bytes = !src_dir && size > 0 ? (guint64)size : 0;
    bool move = j->kind == XD_FILEOP_MOVE;

    bool overwrite = false, merge = false;
    GFileInfo *di = g_file_query_info(dst, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      j->cancel, nullptr);
    if (di != nullptr) {
        gboolean dst_is_dir = g_file_info_get_file_type(di) == G_FILE_TYPE_DIRECTORY;
        bool *yes_all = src_dir && dst_is_dir ? &j->replace_folders : &j->replace_files;
        answer a = ANSWER_YES;
        if (!*yes_all)
            a = src_dir && dst_is_dir ? ask_replace_folder(j, di) : ask_replace_file(j, di, si);
        g_object_unref(di);
        switch (a) {
        case ANSWER_YES_ALL:
            *yes_all = true;
            break;
        case ANSWER_YES:
            break;
        case ANSWER_NO: {
            /* Skipped: its bytes count as done. */
            guint64 skipped = 0;
            guint items = 0;
            measure(j, src, &skipped, &items);
            add_done(j, skipped, items);
            g_object_unref(si);
            return true;
        }
        case ANSWER_CANCEL:
            g_object_unref(si);
            return false;
        }
        if (src_dir && dst_is_dir) {
            merge = true;
        } else if (src_dir || dst_is_dir) {
            /* A file in place of a folder or the other way round: the old
             * one goes first. */
            if (!delete_top(j, dst)) {
                g_object_unref(si);
                return false;
            }
        } else {
            overwrite = true;
        }
    }

    GFileCopyFlags flags = (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS |
                                            G_FILE_COPY_ALL_METADATA |
                                            (overwrite ? G_FILE_COPY_OVERWRITE : 0));
    if (move && !merge) {
        /* A rename when it's on the same file system. */
        guint64 tree_bytes = bytes;
        guint tree_items = 1;
        if (src_dir) {
            tree_bytes = 0;
            tree_items = 0;
            measure(j, src, &tree_bytes, &tree_items);
        }
        if (g_file_move(src, dst, (GFileCopyFlags)(flags | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                        j->cancel, nullptr, nullptr, &err)) {
            add_done(j, tree_bytes, tree_items);
            g_object_unref(si);
            j->wrote = true;
            return true;
        }
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
            !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
            g_object_unref(si);
            return report_error(j, src, err);
        }
        g_clear_error(&err);
    }
    g_object_unref(si);

    if (src_dir) {
        if (!merge && !g_file_make_directory(dst, j->cancel, &err))
            return report_error(j, src, err);
        j->wrote = true;
        if (!transfer_children(j, src, dst))
            return false;
        /* Times and permissions once the contents are in. */
        g_file_copy_attributes(src, dst, (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS |
                                                          G_FILE_COPY_ALL_METADATA),
                               j->cancel, nullptr);
        if (move) {
            /* Only if everything came out (nothing was skipped). */
            g_file_delete(src, j->cancel, nullptr);
        }
        add_done(j, 0, 1);
        return true;
    }
    if (!g_file_copy(src, dst, flags, j->cancel, copy_progress, j, &err))
        return report_error(j, src, err);
    j->wrote = true;
    if (move && !g_file_delete(src, j->cancel, &err))
        return report_error(j, src, err);
    add_done(j, bytes, 1);
    return true;
}

/* Where a top-level item goes, or nullptr when it can't (reported). */
static GFile *destination(job *j, GFile *src, bool *go_on) {
    *go_on = true;
    if (g_file_equal(src, j->dest) || g_file_has_prefix(j->dest, src)) {
        char *name = display_name(src);
        char *text = g_strdup_printf("Cannot %s %s: The destination folder is a subfolder of "
                                     "the source folder.",
                                     verb(j), name);
        g_free(name);
        static const char *const buttons[] = {"_Skip", "Cancel"};
        *go_on = ask(j, "Error", XD_MB_ERROR, text, buttons, 2, 0, 1) == 0;
        return nullptr;
    }
    GFile *parent = g_file_get_parent(src);
    bool beside = parent != nullptr && g_file_equal(parent, j->dest);
    g_clear_object(&parent);
    char *base = g_file_get_basename(src);
    GFile *dst = nullptr;
    if (j->kind == XD_FILEOP_LINK)
        dst = numbered_name(j->dest, "Shortcut", "to", base);
    else if (beside && j->kind == XD_FILEOP_COPY)
        dst = numbered_name(j->dest, "Copy", "of", base);
    else if (!beside)
        dst = g_file_get_child(j->dest, base);
    /* else: moving into the folder it's in, which changes nothing */
    g_free(base);
    return dst;
}

static void created(job *j, GFile *f) { j->created = g_list_prepend(j->created, g_object_ref(f)); }

static void run_transfer(job *j) {
    GPtrArray *slow = g_ptr_array_new_with_free_func(g_object_unref);
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_object_unref);
    bool go_on = true;
    for (guint i = 0; go_on && i < j->sources->len; i++) {
        GFile *src = g_ptr_array_index(j->sources, i);
        GFile *dst = destination(j, src, &go_on);
        if (dst == nullptr)
            continue;
        if (j->kind == XD_FILEOP_LINK) {
            char *path = g_file_get_path(src);
            char *uri = path == nullptr ? g_file_get_uri(src) : nullptr;
            GError *err = nullptr;
            set_current(j, src, j->dest);
            if (g_file_make_symbolic_link(dst, path != nullptr ? path : uri, j->cancel, &err))
                created(j, dst);
            else
                go_on = report_error(j, src, err);
            g_free(path);
            g_free(uri);
            g_object_unref(dst);
            continue;
        }
        if (j->kind == XD_FILEOP_MOVE) {
            /* Most moves are renames: try that before counting anything. */
            GError *err = nullptr;
            set_current(j, src, j->dest);
            if (g_file_move(src, dst,
                            (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS |
                                             G_FILE_COPY_ALL_METADATA |
                                             G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                            j->cancel, nullptr, nullptr, &err)) {
                created(j, dst);
                g_object_unref(dst);
                continue;
            }
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS) &&
                !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
                !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
                go_on = report_error(j, src, err);
                g_object_unref(dst);
                continue;
            }
            g_error_free(err);
        }
        g_ptr_array_add(slow, g_object_ref(src));
        g_ptr_array_add(targets, dst);
    }
    if (go_on && slow->len > 0) {
        measure_all(j, slow);
        for (guint i = 0; go_on && i < slow->len; i++) {
            GFile *dst = g_ptr_array_index(targets, i);
            j->wrote = false;
            go_on = transfer(j, g_ptr_array_index(slow, i), dst);
            if (j->wrote)
                created(j, dst);
        }
    }
    g_ptr_array_unref(slow);
    g_ptr_array_unref(targets);
}

static void worker(GTask *task, [[maybe_unused]] gpointer source, gpointer data,
                   [[maybe_unused]] GCancellable *cancel) {
    job *j = data;
    switch (j->kind) {
    case XD_FILEOP_COPY:
    case XD_FILEOP_MOVE:
    case XD_FILEOP_LINK:
        run_transfer(j);
        break;
    case XD_FILEOP_TRASH:
        g_mutex_lock(&j->lock);
        j->total_items = j->sources->len;
        j->scanning = FALSE;
        g_mutex_unlock(&j->lock);
        for (guint i = 0; i < j->sources->len; i++)
            if (!trash_one(j, g_ptr_array_index(j->sources, i)))
                break;
        break;
    case XD_FILEOP_DELETE:
        measure_all(j, j->sources);
        for (guint i = 0; i < j->sources->len; i++)
            if (!delete_top(j, g_ptr_array_index(j->sources, i)))
                break;
        break;
    }
    j->created = g_list_reverse(j->created);
    g_task_return_boolean(task, TRUE);
}

/* ---- the progress dialog (main thread) ------------------------------------------------ */

static const char *title_for(XdFileOpKind kind) {
    switch (kind) {
    case XD_FILEOP_COPY:
        return "Copying...";
    case XD_FILEOP_MOVE:
        return "Moving...";
    case XD_FILEOP_LINK:
        return "Creating Shortcuts...";
    case XD_FILEOP_TRASH:
    case XD_FILEOP_DELETE:
        break;
    }
    return "Deleting...";
}

static GdkPixbuf *load_icon(GtkWidget *w, const char *name, int px) {
    GtkIconTheme *theme = gtk_icon_theme_get_for_screen(gtk_widget_get_screen(w));
    return gtk_icon_theme_load_icon(theme, name, px, GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
}

/* XP's animation: a sheet of paper flying from one folder to the other
 * (or into the Recycle Bin, or away when deleting). */
static gboolean draw_anim(GtkWidget *w, cairo_t *cr, gpointer data) {
    job *j = data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    GdkPixbuf *from = load_icon(w, "folder", 32);
    GdkPixbuf *to = load_icon(w, j->kind == XD_FILEOP_TRASH ? "user-trash-full" : "folder", 32);
    GdkPixbuf *paper = load_icon(w, "text-x-generic", 24);
    double y0 = (height - 32) / 2.0;
    if (from != nullptr) {
        gdk_cairo_set_source_pixbuf(cr, from, 4, y0);
        cairo_paint(cr);
    }
    if (to != nullptr && j->kind != XD_FILEOP_DELETE) {
        gdk_cairo_set_source_pixbuf(cr, to, width - 36, y0);
        cairo_paint(cr);
    }
    if (paper != nullptr) {
        double t = fmod((double)(g_get_monotonic_time() - j->started) / 1.2e6, 1.0);
        double x = 24 + t * (width - 72);
        double arc = sin(t * G_PI);
        double y = y0 + 4 - arc * (height / 2.0 - 8);
        gdk_cairo_set_source_pixbuf(cr, paper, x, y);
        cairo_paint_with_alpha(cr, j->kind == XD_FILEOP_DELETE ? 1.0 - t : 1.0);
    }
    g_clear_object(&from);
    g_clear_object(&to);
    g_clear_object(&paper);
    return TRUE;
}

static char *remaining_text(guint64 done, guint64 total, double seconds) {
    if (done == 0 || seconds < 1.0 || total <= done)
        return g_strdup("");
    double rate = (double)done / seconds;
    double left = (double)(total - done) / rate;
    if (left < 60.0) {
        double up = ceil(left);
        int s = (int)up;
        return g_strdup_printf("%d Second%s Remaining", s, s == 1 ? "" : "s");
    }
    double up = ceil(left / 60.0);
    int m = (int)up;
    return g_strdup_printf("%d Minute%s Remaining", m, m == 1 ? "" : "s");
}

static gboolean update_dialog(gpointer data) {
    job *j = data;
    if (j->dialog == nullptr)
        return G_SOURCE_CONTINUE;
    g_mutex_lock(&j->lock);
    gboolean scanning = j->scanning;
    char *current = g_strdup(j->current != nullptr ? j->current : "");
    char *from = g_strdup(j->from != nullptr ? j->from : "");
    char *to = g_strdup(j->to);
    guint64 done = j->done_bytes + j->file_bytes;
    guint64 total = j->total_bytes;
    guint items = j->done_items;
    guint total_items = j->total_items;
    g_mutex_unlock(&j->lock);

    if (scanning) {
        const char *what = j->kind == XD_FILEOP_MOVE ? "move" : j->kind == XD_FILEOP_COPY
                                                                     ? "copy"
                                                                     : "delete";
        char *text = g_strdup_printf("Preparing to %s...", what);
        gtk_label_set_text(GTK_LABEL(j->name_label), text);
        g_free(text);
        gtk_label_set_text(GTK_LABEL(j->from_label), "");
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(j->bar));
    } else {
        if (j->rate_since == 0)
            j->rate_since = g_get_monotonic_time();
        gtk_label_set_text(GTK_LABEL(j->name_label), current);
        char *line = to != nullptr ? g_strdup_printf("From '%s' to '%s'", from, to)
                                   : g_strdup_printf("From '%s'", from);
        gtk_label_set_text(GTK_LABEL(j->from_label), line);
        g_free(line);
        /* Bytes when there are any to count, else items. */
        double fraction = total > 0 ? (double)done / (double)total
                          : total_items > 0 ? (double)items / (double)total_items
                                            : 0.0;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(j->bar), CLAMP(fraction, 0.0, 1.0));
        double seconds = (double)(g_get_monotonic_time() - j->rate_since) / 1e6;
        char *left = total > 0 ? remaining_text(done, total, seconds) : g_strdup("");
        gtk_label_set_text(GTK_LABEL(j->time_label), left);
        g_free(left);
    }
    g_free(current);
    g_free(from);
    g_free(to);
    gtk_widget_queue_draw(j->anim);
    return G_SOURCE_CONTINUE;
}

static void on_cancel([[maybe_unused]] GtkWidget *w, gpointer data) {
    job *j = data;
    g_cancellable_cancel(j->cancel);
    gtk_widget_set_sensitive(j->cancel_button, FALSE);
}

static gboolean on_dialog_delete([[maybe_unused]] GtkWidget *w, [[maybe_unused]] GdkEvent *ev,
                                 gpointer data) {
    on_cancel(nullptr, data);
    return TRUE; /* closes when the worker stops */
}

static GtkWidget *left_label(GtkWidget *box) {
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}

static gboolean show_dialog(gpointer data) {
    job *j = data;
    if (j->asking)
        return G_SOURCE_CONTINUE; /* not over the question */
    j->show_timer = 0;
    GtkWidget *dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dlg), title_for(j->kind));
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "system-file-manager");
    if (j->parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), j->parent);
    g_signal_connect(dlg, "delete-event", G_CALLBACK(on_dialog_delete), j);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    j->anim = gtk_drawing_area_new();
    gtk_widget_set_size_request(j->anim, 272, 60);
    g_signal_connect(j->anim, "draw", G_CALLBACK(draw_anim), j);
    gtk_box_pack_start(GTK_BOX(box), j->anim, FALSE, FALSE, 0);
    j->name_label = left_label(box);
    j->from_label = left_label(box);
    j->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(j->bar, 360, -1);
    gtk_box_pack_start(GTK_BOX(box), j->bar, FALSE, FALSE, 4);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    j->time_label = left_label(row);
    gtk_box_set_child_packing(GTK_BOX(row), j->time_label, TRUE, TRUE, 0, GTK_PACK_START);
    j->cancel_button = gtk_button_new_with_label("Cancel");
    gtk_widget_set_size_request(j->cancel_button, 75, -1);
    g_signal_connect(j->cancel_button, "clicked", G_CALLBACK(on_cancel), j);
    gtk_box_pack_end(GTK_BOX(row), j->cancel_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(dlg), box);
    j->dialog = dlg;
    update_dialog(j);
    gtk_widget_show_all(dlg);
    return G_SOURCE_REMOVE;
}

static void job_free(job *j) {
    if (j->show_timer != 0)
        g_source_remove(j->show_timer);
    if (j->update_timer != 0)
        g_source_remove(j->update_timer);
    if (j->dialog != nullptr)
        gtk_widget_destroy(j->dialog);
    if (j->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(j->parent), (gpointer *)&j->parent);
    g_ptr_array_unref(j->sources);
    g_clear_object(&j->dest);
    g_clear_object(&j->cancel);
    g_list_free_full(j->created, g_object_unref);
    g_mutex_clear(&j->lock);
    g_cond_clear(&j->cond);
    g_free(j->current);
    g_free(j->from);
    g_free(j->to);
    g_free(j->q_title);
    g_free(j->q_text);
    g_free(j);
}

static void finished([[maybe_unused]] GObject *source, GAsyncResult *res, gpointer data) {
    job *j = data;
    (void)g_task_propagate_boolean(G_TASK(res), nullptr);
    /* No one to tell when the window that started it has closed. */
    if (j->done != nullptr && j->parent != nullptr)
        j->done(j->created, j->user);
    job_free(j);
}

/* Windows asks before anything is deleted. */
static bool confirm_delete(GtkWindow *parent, XdFileOpKind kind, GList *sources) {
    bool trash = kind == XD_FILEOP_TRASH;
    guint n = g_list_length(sources);
    char *text;
    const char *title;
    if (n == 1) {
        GFile *f = sources->data;
        char *name = display_name(f);
        bool dir = g_file_query_file_type(f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr) ==
                   G_FILE_TYPE_DIRECTORY;
        title = dir ? "Confirm Folder Delete" : "Confirm File Delete";
        if (dir)
            text = trash ? g_strdup_printf("Are you sure you want to remove the folder '%s' and "
                                           "move all its contents to the Recycle Bin?",
                                           name)
                         : g_strdup_printf("Are you sure you want to remove the folder '%s' and "
                                           "all its contents?",
                                           name);
        else
            text = trash ? g_strdup_printf("Are you sure you want to send '%s' to the Recycle "
                                           "Bin?",
                                           name)
                         : g_strdup_printf("Are you sure you want to delete '%s'?", name);
        g_free(name);
    } else {
        title = "Confirm Multiple File Delete";
        text = trash ? g_strdup_printf("Are you sure you want to send these %u items to the "
                                       "Recycle Bin?",
                                       n)
                     : g_strdup_printf("Are you sure you want to delete these %u items?", n);
    }
    int r = xd_message_box(parent, title, trash ? XD_MB_QUESTION : XD_MB_WARNING, XD_MB_YES_NO,
                           FALSE, text);
    g_free(text);
    return r == GTK_RESPONSE_YES;
}

void xd_fileop_start(GtkWindow *parent, XdFileOpKind kind, GList *sources, GFile *dest,
                     XdFileOpDone done, gpointer user) {
    if (sources == nullptr)
        return;
    if ((kind == XD_FILEOP_TRASH || kind == XD_FILEOP_DELETE) &&
        !confirm_delete(parent, kind, sources))
        return;
    job *j = g_new0(job, 1);
    j->kind = kind;
    j->sources = g_ptr_array_new_with_free_func(g_object_unref);
    for (GList *l = sources; l != nullptr; l = l->next)
        g_ptr_array_add(j->sources, g_object_ref(l->data));
    j->dest = dest != nullptr ? g_object_ref(dest) : nullptr;
    if (dest == nullptr && kind != XD_FILEOP_TRASH && kind != XD_FILEOP_DELETE) {
        job_free(j);
        return;
    }
    j->parent = parent;
    if (parent != nullptr)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&j->parent);
    j->done = done;
    j->user = user;
    j->cancel = g_cancellable_new();
    j->scanning = TRUE;
    g_mutex_init(&j->lock);
    g_cond_init(&j->cond);
    j->started = g_get_monotonic_time();
    j->show_timer = g_timeout_add(SHOW_DELAY_MS, show_dialog, j);
    j->update_timer = g_timeout_add(UPDATE_MS, update_dialog, j);

    GTask *task = g_task_new(nullptr, nullptr, finished, j);
    g_task_set_task_data(task, j, nullptr);
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
}

/* ---- names ---------------------------------------------------------------------------- */

char *xd_unique_name(GFile *dir, const char *base, const char *ext) {
    for (guint n = 1;; n++) {
        char *name = n == 1 ? g_strdup_printf("%s%s", base, ext != nullptr ? ext : "")
                            : g_strdup_printf("%s (%u)%s", base, n, ext != nullptr ? ext : "");
        GFile *f = g_file_get_child(dir, name);
        gboolean exists = g_file_query_exists(f, nullptr);
        g_object_unref(f);
        if (!exists)
            return name;
        g_free(name);
    }
}

/* ---- the clipboard -------------------------------------------------------------------- */

#define GNOME_FILES "x-special/gnome-copied-files"
#define KDE_CUT "application/x-kde-cutselection"

enum { TARGET_GNOME = 1, TARGET_KDE_CUT, TARGET_URIS, TARGET_TEXT };

typedef struct clip_data {
    gboolean cut;
    char **uris;
} clip_data;

static gboolean clip_has_files;
static clip_data *clip_ours; /* while we own the clipboard */
static gboolean clip_watching;

static void clip_get(GtkClipboard *cb, GtkSelectionData *sel, guint info, gpointer data) {
    (void)cb;
    clip_data *c = data;
    switch (info) {
    case TARGET_GNOME: {
        GString *s = g_string_new(c->cut ? "cut" : "copy");
        for (char **u = c->uris; *u != nullptr; u++)
            g_string_append_printf(s, "\n%s", *u);
        gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
                               (const guchar *)s->str, (gint)s->len);
        g_string_free(s, TRUE);
        break;
    }
    case TARGET_KDE_CUT:
        gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
                               (const guchar *)(c->cut ? "1" : "0"), 1);
        break;
    case TARGET_URIS:
        gtk_selection_data_set_uris(sel, c->uris);
        break;
    case TARGET_TEXT: {
        GString *s = g_string_new(nullptr);
        for (char **u = c->uris; *u != nullptr; u++) {
            GFile *f = g_file_new_for_uri(*u);
            char *name = g_file_get_parse_name(f);
            g_string_append_printf(s, "%s%s", s->len > 0 ? "\n" : "", name);
            g_free(name);
            g_object_unref(f);
        }
        gtk_selection_data_set_text(sel, s->str, (gint)s->len);
        g_string_free(s, TRUE);
        break;
    }
    default:
        break;
    }
}

static void clip_clear([[maybe_unused]] GtkClipboard *cb, gpointer data) {
    clip_data *c = data;
    if (clip_ours == c)
        clip_ours = nullptr;
    g_strfreev(c->uris);
    g_free(c);
}

static void got_targets([[maybe_unused]] GtkClipboard *cb, GdkAtom *atoms, gint n,
                        [[maybe_unused]] gpointer data) {
    clip_has_files = FALSE;
    for (gint i = 0; i < n; i++) {
        char *name = gdk_atom_name(atoms[i]);
        if (g_strcmp0(name, GNOME_FILES) == 0 || g_strcmp0(name, "text/uri-list") == 0)
            clip_has_files = TRUE;
        g_free(name);
    }
}

static void on_owner_change(GtkClipboard *cb, [[maybe_unused]] GdkEvent *ev,
                            [[maybe_unused]] gpointer data) {
    gtk_clipboard_request_targets(cb, got_targets, nullptr);
}

static GtkClipboard *clipboard_for(GtkWidget *widget) {
    GtkClipboard *cb = gtk_widget_get_clipboard(widget, GDK_SELECTION_CLIPBOARD);
    if (!clip_watching) {
        clip_watching = TRUE;
        g_signal_connect(cb, "owner-change", G_CALLBACK(on_owner_change), nullptr);
        gtk_clipboard_request_targets(cb, got_targets, nullptr);
    }
    return cb;
}

void xd_clipboard_set_files(GtkWidget *widget, GList *files, gboolean cut) {
    GtkClipboard *cb = clipboard_for(widget);
    clip_data *c = g_new0(clip_data, 1);
    c->cut = cut;
    c->uris = g_new0(char *, g_list_length(files) + 1);
    guint k = 0;
    for (GList *l = files; l != nullptr; l = l->next)
        c->uris[k++] = g_file_get_uri(l->data);

    GtkTargetList *list = gtk_target_list_new(nullptr, 0);
    gtk_target_list_add(list, gdk_atom_intern_static_string(GNOME_FILES), 0, TARGET_GNOME);
    gtk_target_list_add(list, gdk_atom_intern_static_string(KDE_CUT), 0, TARGET_KDE_CUT);
    gtk_target_list_add_uri_targets(list, TARGET_URIS);
    gtk_target_list_add_text_targets(list, TARGET_TEXT);
    gint n = 0;
    GtkTargetEntry *entries = gtk_target_table_new_from_list(list, &n);
    gtk_target_list_unref(list);
    if (gtk_clipboard_set_with_data(cb, entries, (guint)n, clip_get, clip_clear, c)) {
        clip_ours = c;
        clip_has_files = TRUE;
    } else {
        g_strfreev(c->uris);
        g_free(c);
    }
    gtk_target_table_free(entries, n);
}

typedef struct clip_request {
    XdClipboardFiles got;
    gpointer user;
} clip_request;

static GList *files_from_uris(char **uris) {
    GList *files = nullptr;
    for (char **u = uris; u != nullptr && *u != nullptr; u++)
        if (**u != '\0')
            files = g_list_prepend(files, g_file_new_for_uri(*u));
    return g_list_reverse(files);
}

static void got_uris([[maybe_unused]] GtkClipboard *cb, char **uris, gpointer data) {
    clip_request *r = data;
    GList *files = files_from_uris(uris);
    r->got(files, FALSE, r->user);
    g_list_free_full(files, g_object_unref);
    g_free(r);
}

static void got_gnome(GtkClipboard *cb, GtkSelectionData *sel, gpointer data) {
    clip_request *r = data;
    gint len = gtk_selection_data_get_length(sel);
    if (len <= 0) {
        gtk_clipboard_request_uris(cb, got_uris, r);
        return;
    }
    char *text = g_strndup((const char *)gtk_selection_data_get_data(sel), (gsize)len);
    char **lines = g_strsplit(text, "\n", -1);
    g_free(text);
    gboolean cut = lines[0] != nullptr && g_strcmp0(g_strstrip(lines[0]), "cut") == 0;
    for (char **l = lines; *l != nullptr; l++)
        g_strstrip(*l);
    GList *files = lines[0] != nullptr ? files_from_uris(lines + 1) : nullptr;
    g_strfreev(lines);
    r->got(files, cut, r->user);
    g_list_free_full(files, g_object_unref);
    g_free(r);
}

void xd_clipboard_get_files(GtkWidget *widget, XdClipboardFiles got, gpointer user) {
    clip_request *r = g_new0(clip_request, 1);
    r->got = got;
    r->user = user;
    gtk_clipboard_request_contents(clipboard_for(widget), gdk_atom_intern_static_string(GNOME_FILES),
                                   got_gnome, r);
}

gboolean xd_clipboard_has_files(void) { return clip_has_files; }

void xd_clipboard_clear_cut(GtkWidget *widget) {
    if (clip_ours != nullptr && clip_ours->cut)
        gtk_clipboard_clear(clipboard_for(widget));
}
