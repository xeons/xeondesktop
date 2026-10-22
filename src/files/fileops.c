/* SPDX-License-Identifier: MIT
 * File operations (copy, move, shortcut, Recycle Bin, delete) with XP's
 * progress dialog and questions, and the file clipboard. See fileops.h.
 */
#include "fileops.h"

#include <errno.h>
#include <math.h>

#include "../gtkapp/gtkapp.h"
#include "extract.h"
#include "folderview.h"
#include "undo.h"

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
    bool had_parent;
    XdFileOpDone done;
    gpointer user;
    GCancellable *cancel;
    GList *created; /* GFile*, built by the worker: the top-level items made */
    /* For Edit > Undo: the files the operation made or moved (`made`),
     * what each came from (`origins`, same order), and what went to the
     * Recycle Bin. Only what wasn't there before: a folder copied into one
     * of the same name has the files it brought in listed, not the folder,
     * and a copy over a file isn't listed at all, so Undo never takes away
     * what the user already had. */
    bool undoable;
    GList *made;
    GList *origins;
    GList *trashed;

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
    GdkPixbuf *anim_from, *anim_to, *anim_paper; /* loaded on the first frame */
    GApplication *app; /* held while the job runs */
    guint show_timer, update_timer;
    gboolean asking; /* a question is up: the dialog waits for it */
    gint64 started;
    gint64 rate_since; /* when byte counting began, after the scan */
} job;

/* ---- talking to the main thread -------------------------------------------------- */

static gboolean ask_in_main(gpointer data) {
    job *operation = data;
    GtkWindow *parent = operation->dialog != nullptr && gtk_widget_get_visible(operation->dialog)
                            ? GTK_WINDOW(operation->dialog)
                            : operation->parent;
    g_mutex_lock(&operation->lock);
    char *title = g_strdup(operation->q_title);
    char *text = g_strdup(operation->q_text);
    g_mutex_unlock(&operation->lock);
    operation->asking = TRUE;
    int response =
        xd_message_box_choose(parent, title, operation->q_icon, text, operation->q_labels,
                              operation->q_count, operation->q_default, operation->q_cancel);
    operation->asking = FALSE;
    g_free(title);
    g_free(text);
    g_mutex_lock(&operation->lock);
    operation->q_answer = response;
    operation->q_answered = TRUE;
    g_cond_signal(&operation->cond);
    g_mutex_unlock(&operation->lock);
    return G_SOURCE_REMOVE;
}

/* Worker: shows a message box on the main thread and waits for it. Takes
 * `text`. */
static int ask(job *operation, const char *title, xd_mb_icon icon, char *text,
               const char *const *labels, int count, int default_index, int cancel_index) {
    g_mutex_lock(&operation->lock);
    g_free(operation->q_title);
    g_free(operation->q_text);
    operation->q_title = g_strdup(title);
    operation->q_text = text;
    operation->q_icon = icon;
    operation->q_labels = labels;
    operation->q_count = count;
    operation->q_default = default_index;
    operation->q_cancel = cancel_index;
    operation->q_answered = FALSE;
    g_main_context_invoke(nullptr, ask_in_main, operation);
    while (!operation->q_answered)
        g_cond_wait(&operation->cond, &operation->lock);
    int response = operation->q_answer;
    g_mutex_unlock(&operation->lock);
    return response;
}

static const char *const replace_buttons[] = {"_Yes", "Yes to _All", "_No", "Cancel"};

/* Yes / Yes to All / No / Cancel. Takes `text`. */
static answer ask_replace(job *operation, const char *title, xd_mb_icon icon, char *text,
                          int default_index) {
    int response =
        ask(operation, title, icon, text, replace_buttons, 4, default_index, ANSWER_CANCEL);
    return response >= ANSWER_YES && response <= ANSWER_CANCEL ? (answer)response : ANSWER_CANCEL;
}

static const char *verb(const job *operation) {
    switch (operation->kind) {
    case XD_FILEOP_COPY:
        return "copy";
    case XD_FILEOP_MOVE:
        return "move";
    case XD_FILEOP_LINK:
        return "create shortcut to";
    case XD_FILEOP_EXTRACT_HERE:
    case XD_FILEOP_EXTRACT_FOLDER:
        return "extract";
    case XD_FILEOP_TRASH:
    case XD_FILEOP_DELETE:
        break;
    }
    return "delete";
}

static char *display_name(GFile *file) {
    char *base = g_file_get_basename(file);
    char *name = base != nullptr ? g_filename_display_name(base) : g_file_get_parse_name(file);
    g_free(base);
    return name;
}

/* Worker: Windows' "Cannot copy 'x': reason". Returns whether to go on
 * (Skip) rather than stop. Takes `err`. */
static bool report_error(job *operation, GFile *file, GError *err) {
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        g_cancellable_is_cancelled(operation->cancel)) {
        g_error_free(err);
        return false;
    }
    char *name = display_name(file);
    char *text = g_strdup_printf("Cannot %s %s: %s", verb(operation), name, err->message);
    g_free(name);
    g_error_free(err);
    static const char *const buttons[] = {"_Skip", "Cancel"};
    return ask(operation, "Error", XD_MB_ERROR, text, buttons, 2, 0, 1) == 0;
}

static void set_current(job *operation, GFile *src, GFile *dst_dir) {
    g_mutex_lock(&operation->lock);
    g_free(operation->current);
    operation->current = display_name(src);
    GFile *parent = g_file_get_parent(src);
    char *from = parent != nullptr ? display_name(parent) : g_strdup("");
    g_clear_object(&parent);
    char *to = dst_dir != nullptr ? display_name(dst_dir) : nullptr;
    g_free(operation->from);
    g_free(operation->to);
    operation->from = from;
    operation->to = to;
    operation->file_bytes = 0;
    g_mutex_unlock(&operation->lock);
}

static void add_done(job *operation, guint64 bytes, guint items) {
    g_mutex_lock(&operation->lock);
    operation->done_bytes += bytes;
    operation->done_items += items;
    operation->file_bytes = 0;
    g_mutex_unlock(&operation->lock);
}

static void copy_progress(goffset current, [[maybe_unused]] goffset total, gpointer data) {
    job *operation = data;
    g_mutex_lock(&operation->lock);
    operation->file_bytes = current > 0 ? (guint64)current : 0;
    g_mutex_unlock(&operation->lock);
}

/* ---- worker: sizes ------------------------------------------------------------------ */

/* Adds up a tree's bytes and items (without following links). */
static void measure(job *operation, GFile *file, guint64 *bytes, guint *items) {
    if (g_cancellable_is_cancelled(operation->cancel))
        return;
    GFileInfo *info = g_file_query_info(file, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        operation->cancel, nullptr);
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
    GFileEnumerator *en =
        g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, operation->cancel, nullptr);
    if (en == nullptr)
        return;
    GFileInfo *child_info;
    while ((child_info = g_file_enumerator_next_file(en, operation->cancel, nullptr)) != nullptr) {
        GFile *child = g_file_get_child(file, g_file_info_get_name(child_info));
        measure(operation, child, bytes, items);
        g_object_unref(child);
        g_object_unref(child_info);
    }
    g_object_unref(en);
}

static void measure_all(job *operation, GPtrArray *files) {
    guint64 bytes = 0;
    guint items = 0;
    for (guint i = 0; i < files->len; i++)
        measure(operation, g_ptr_array_index(files, i), &bytes, &items);
    g_mutex_lock(&operation->lock);
    operation->total_bytes += bytes;
    operation->total_items += items;
    operation->scanning = FALSE;
    g_mutex_unlock(&operation->lock);
}

/* ---- worker: deleting -------------------------------------------------------------- */

/* Deletes a tree, children first. Returns false to stop the operation;
 * a skipped item leaves its folder in place. */
static bool delete_tree(job *operation, GFile *file, bool *removed) {
    *removed = false;
    if (g_cancellable_is_cancelled(operation->cancel))
        return false;
    GError *err = nullptr;
    GFileInfo *info = g_file_query_info(file, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        operation->cancel, &err);
    if (info == nullptr)
        return report_error(operation, file, err);
    gboolean is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    goffset size = g_file_info_get_size(info);
    g_object_unref(info);
    bool all_gone = true;
    if (is_dir) {
        GFileEnumerator *en =
            g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, operation->cancel, &err);
        if (en == nullptr)
            return report_error(operation, file, err);
        GFileInfo *child_info;
        bool go_on = true;
        while (go_on &&
               (child_info = g_file_enumerator_next_file(en, operation->cancel, &err)) != nullptr) {
            GFile *child = g_file_get_child(file, g_file_info_get_name(child_info));
            bool gone = false;
            go_on = delete_tree(operation, child, &gone);
            all_gone = all_gone && gone;
            g_object_unref(child);
            g_object_unref(child_info);
        }
        g_object_unref(en);
        /* The listing broke off (not its end): what's left wasn't seen, so
         * the folder stays. */
        if (go_on && err != nullptr) {
            all_gone = false;
            go_on = report_error(operation, file, err);
        }
        if (!go_on)
            return false;
    }
    if (!all_gone)
        return true;
    set_current(operation, file, nullptr);
    if (!g_file_delete(file, operation->cancel, &err))
        return report_error(operation, file, err);
    add_done(operation, !is_dir && size > 0 ? (guint64)size : 0, 1);
    *removed = true;
    return true;
}

static bool delete_top(job *operation, GFile *file) {
    bool removed = false;
    return delete_tree(operation, file, &removed);
}

/* Recycle Bin: when the file's volume has none, Windows offers to delete
 * it permanently. */
static bool trash_one(job *operation, GFile *file) {
    set_current(operation, file, nullptr);
    GError *err = nullptr;
    if (g_file_trash(file, operation->cancel, &err)) {
        add_done(operation, 0, 1);
        operation->trashed = g_list_prepend(operation->trashed, g_object_ref(file));
        return true;
    }
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        return report_error(operation, file, err);
    g_error_free(err);
    if (!operation->delete_unrecyclable) {
        char *name = display_name(file);
        char *text = g_strdup_printf(
            "%s can't be moved to the Recycle Bin. Do you want to delete it permanently?", name);
        g_free(name);
        switch (ask_replace(operation, "Confirm File Delete", XD_MB_WARNING, text, ANSWER_NO)) {
        case ANSWER_YES:
            break;
        case ANSWER_YES_ALL:
            operation->delete_unrecyclable = true;
            break;
        case ANSWER_NO:
            add_done(operation, 0, 1);
            return true;
        case ANSWER_CANCEL:
            return false;
        }
    }
    bool ok = delete_top(operation, file);
    add_done(operation, 0, 1);
    return ok;
}

/* ---- worker: copying and moving --------------------------------------------------------- */

/* "Copy of x", "Copy (2) of x", ... in `dir`: Windows' names for a copy
 * or shortcut beside the original. */
static GFile *numbered_name(GFile *dir, const char *word, const char *prep, const char *name) {
    for (guint count = 1;; count++) {
        char *candidate = count == 1 ? g_strdup_printf("%s %s %s", word, prep, name)
                                     : g_strdup_printf("%s (%u) %s %s", word, count, prep, name);
        GFile *file = g_file_get_child(dir, candidate);
        g_free(candidate);
        if (!g_file_query_exists(file, nullptr))
            return file;
        g_object_unref(file);
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

static answer ask_replace_file(job *operation, GFileInfo *existing, GFileInfo *incoming) {
    char *existing_text = describe(existing);
    char *incoming_text = describe(incoming);
    char *text =
        g_strdup_printf("This folder already contains a file named '%s'.\n\n"
                        "Would you like to replace the existing file\n\n%s\n\n"
                        "with this one?\n\n%s",
                        g_file_info_get_display_name(existing), existing_text, incoming_text);
    g_free(existing_text);
    g_free(incoming_text);
    return ask_replace(operation, "Confirm File Replace", XD_MB_QUESTION, text, ANSWER_YES);
}

static answer ask_replace_folder(job *operation, GFileInfo *existing) {
    char *text = g_strdup_printf(
        "This folder already contains a folder named '%s'.\n\n"
        "If the files in the existing folder have the same name as files in the folder you "
        "are moving or copying, they will be replaced. Do you still want to move or copy the "
        "folder?",
        g_file_info_get_display_name(existing));
    return ask_replace(operation, "Confirm Folder Replace", XD_MB_QUESTION, text, ANSWER_YES);
}

static bool transfer(job *operation, GFile *src, GFile *dst, bool undo);

/* `undo`: whether what each child makes is listed for Undo (it's in a
 * folder that was there before, not one this operation made). */
static bool transfer_children(job *operation, GFile *src, GFile *dst, bool undo) {
    GError *err = nullptr;
    GFileEnumerator *en =
        g_file_enumerate_children(src, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, operation->cancel, &err);
    if (en == nullptr)
        return report_error(operation, src, err);
    bool go_on = true;
    GFileInfo *info;
    while (go_on && (info = g_file_enumerator_next_file(en, operation->cancel, &err)) != nullptr) {
        const char *name = g_file_info_get_name(info);
        GFile *source_child = g_file_get_child(src, name);
        GFile *dest_child = g_file_get_child(dst, name);
        go_on = transfer(operation, source_child, dest_child, undo);
        g_object_unref(source_child);
        g_object_unref(dest_child);
        g_object_unref(info);
    }
    g_object_unref(en);
    /* The listing broke off: not everything came over. Skipped, a move
     * leaves the source folder (it's deleted only when empty). */
    if (go_on && err != nullptr)
        go_on = report_error(operation, src, err);
    return go_on;
}

static void made(job *operation, GFile *src, GFile *dst) {
    operation->made = g_list_prepend(operation->made, g_object_ref(dst));
    operation->origins = g_list_prepend(operation->origins, g_object_ref(src));
}

/* Copies or moves `src` to `dst`, asking about anything in the way. A
 * folder over a folder merges into it. `undo`: list what it makes for
 * Undo. Returns false to stop. */
static bool transfer(job *operation, GFile *src, GFile *dst, bool undo) {
    if (g_cancellable_is_cancelled(operation->cancel))
        return false;
    GFile *dst_dir = g_file_get_parent(dst);
    set_current(operation, src, dst_dir);
    g_clear_object(&dst_dir);
    GError *err = nullptr;
    GFileInfo *si = g_file_query_info(src, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      operation->cancel, &err);
    if (si == nullptr)
        return report_error(operation, src, err);
    gboolean src_dir = g_file_info_get_file_type(si) == G_FILE_TYPE_DIRECTORY;
    goffset size = g_file_info_get_size(si);
    guint64 bytes = !src_dir && size > 0 ? (guint64)size : 0;
    /* What's extracted is moved out of the folder it was extracted to. */
    bool move = operation->kind != XD_FILEOP_COPY;

    bool overwrite = false, merge = false, replaced = false;
    GFileInfo *di = g_file_query_info(dst, INFO_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      operation->cancel, nullptr);
    if (di != nullptr) {
        gboolean dst_is_dir = g_file_info_get_file_type(di) == G_FILE_TYPE_DIRECTORY;
        bool *yes_all =
            src_dir && dst_is_dir ? &operation->replace_folders : &operation->replace_files;
        answer decision = ANSWER_YES;
        if (!*yes_all)
            decision = src_dir && dst_is_dir ? ask_replace_folder(operation, di)
                                             : ask_replace_file(operation, di, si);
        g_object_unref(di);
        switch (decision) {
        case ANSWER_YES_ALL:
            *yes_all = true;
            break;
        case ANSWER_YES:
            break;
        case ANSWER_NO: {
            /* Skipped: its bytes count as done. */
            guint64 skipped = 0;
            guint items = 0;
            measure(operation, src, &skipped, &items);
            add_done(operation, skipped, items);
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
            if (!delete_top(operation, dst)) {
                g_object_unref(si);
                return false;
            }
            replaced = true;
        } else {
            overwrite = true;
            replaced = true;
        }
    }

    /* Undo of a move puts it back where it came from; of a copy, sends the
     * copy to the Recycle Bin, which mustn't take a file's old place. */
    bool listed = undo && !merge && (operation->kind == XD_FILEOP_MOVE || !replaced);
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
            measure(operation, src, &tree_bytes, &tree_items);
        }
        if (g_file_move(src, dst, (GFileCopyFlags)(flags | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                        operation->cancel, nullptr, nullptr, &err)) {
            add_done(operation, tree_bytes, tree_items);
            g_object_unref(si);
            operation->wrote = true;
            if (listed)
                made(operation, src, dst);
            return true;
        }
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
            !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
            g_object_unref(si);
            return report_error(operation, src, err);
        }
        g_clear_error(&err);
    }
    g_object_unref(si);

    if (src_dir) {
        if (!merge && !g_file_make_directory(dst, operation->cancel, &err))
            return report_error(operation, src, err);
        operation->wrote = true;
        if (listed)
            made(operation, src, dst);
        /* In a folder made here, Undo takes the folder; merged into one
         * that was there, what came in, one by one. */
        if (!transfer_children(operation, src, dst, undo && merge))
            return false;
        /* Times and permissions once the contents are in, on a folder made
         * here; one merged into keeps its own (an archive's or a drive's
         * folder of the same name mustn't make ~/Documents world-writable). */
        if (!merge)
            g_file_copy_attributes(
                src, dst,
                (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA),
                operation->cancel, nullptr);
        if (move) {
            /* Only if everything came out (nothing was skipped). */
            g_file_delete(src, operation->cancel, nullptr);
        }
        add_done(operation, 0, 1);
        return true;
    }
    if (!g_file_copy(src, dst, flags, operation->cancel, copy_progress, operation, &err))
        return report_error(operation, src, err);
    operation->wrote = true;
    if (listed)
        made(operation, src, dst);
    if (move && !g_file_delete(src, operation->cancel, &err))
        return report_error(operation, src, err);
    add_done(operation, bytes, 1);
    return true;
}

/* Where a top-level item goes, or nullptr when it can't (reported). */
static GFile *destination(job *operation, GFile *src, bool *go_on) {
    *go_on = true;
    if (g_file_equal(src, operation->dest) || g_file_has_prefix(operation->dest, src)) {
        char *name = display_name(src);
        char *text = g_strdup_printf("Cannot %s %s: The destination folder is a subfolder of "
                                     "the source folder.",
                                     verb(operation), name);
        g_free(name);
        static const char *const buttons[] = {"_Skip", "Cancel"};
        *go_on = ask(operation, "Error", XD_MB_ERROR, text, buttons, 2, 0, 1) == 0;
        return nullptr;
    }
    GFile *parent = g_file_get_parent(src);
    bool beside = parent != nullptr && g_file_equal(parent, operation->dest);
    g_clear_object(&parent);
    char *base = g_file_get_basename(src);
    GFile *dst = nullptr;
    if (operation->kind == XD_FILEOP_LINK)
        dst = numbered_name(operation->dest, "Shortcut", "to", base);
    else if (beside && operation->kind == XD_FILEOP_COPY)
        dst = numbered_name(operation->dest, "Copy", "of", base);
    else if (!beside)
        dst = g_file_get_child(operation->dest, base);
    /* else: moving into the folder it's in, which changes nothing */
    g_free(base);
    return dst;
}

static void created(job *operation, GFile *file) {
    operation->created = g_list_prepend(operation->created, g_object_ref(file));
}

static void run_transfer(job *operation) {
    GPtrArray *slow = g_ptr_array_new_with_free_func(g_object_unref);
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_object_unref);
    bool go_on = true;
    for (guint i = 0; go_on && i < operation->sources->len; i++) {
        GFile *src = g_ptr_array_index(operation->sources, i);
        GFile *dst = destination(operation, src, &go_on);
        if (dst == nullptr)
            continue;
        if (operation->kind == XD_FILEOP_LINK) {
            char *path = g_file_get_path(src);
            char *uri = path == nullptr ? g_file_get_uri(src) : nullptr;
            GError *err = nullptr;
            set_current(operation, src, operation->dest);
            if (g_file_make_symbolic_link(dst, path != nullptr ? path : uri, operation->cancel,
                                          &err))
                created(operation, dst);
            else
                go_on = report_error(operation, src, err);
            g_free(path);
            g_free(uri);
            g_object_unref(dst);
            continue;
        }
        if (operation->kind == XD_FILEOP_MOVE) {
            /* Most moves are renames: try that before counting anything. */
            GError *err = nullptr;
            set_current(operation, src, operation->dest);
            if (g_file_move(src, dst,
                            (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS |
                                             G_FILE_COPY_ALL_METADATA |
                                             G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                            operation->cancel, nullptr, nullptr, &err)) {
                created(operation, dst); /* nothing was in the way */
                made(operation, src, dst);
                g_object_unref(dst);
                continue;
            }
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS) &&
                !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
                !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
                go_on = report_error(operation, src, err);
                g_object_unref(dst);
                continue;
            }
            g_error_free(err);
        }
        g_ptr_array_add(slow, g_object_ref(src));
        g_ptr_array_add(targets, dst);
    }
    if (go_on && slow->len > 0) {
        measure_all(operation, slow);
        for (guint i = 0; go_on && i < slow->len; i++) {
            GFile *dst = g_ptr_array_index(targets, i);
            operation->wrote = false;
            go_on = transfer(operation, g_ptr_array_index(slow, i), dst, true);
            if (operation->wrote)
                created(operation, dst);
        }
    }
    g_ptr_array_unref(slow);
    g_ptr_array_unref(targets);
}

/* ---- worker: extracting --------------------------------------------------------- */

static void extract_progress(guint64 read, gpointer data) {
    job *operation = data;
    g_mutex_lock(&operation->lock);
    operation->file_bytes = read;
    g_mutex_unlock(&operation->lock);
}

/* Whatever's left of the folder extracted to (what wasn't moved out:
 * answered No, or stopped). */
static void remove_tree(GFile *file) {
    GFileEnumerator *en =
        g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    GFileInfo *info;
    while (en != nullptr && (info = g_file_enumerator_next_file(en, nullptr, nullptr)) != nullptr) {
        GFile *child = g_file_get_child(file, g_file_info_get_name(info));
        remove_tree(child);
        g_object_unref(child);
        g_object_unref(info);
    }
    g_clear_object(&en);
    g_file_delete(file, nullptr, nullptr);
}

/* Each archive is extracted into a hidden folder beside where it goes,
 * on the same file system; what's in it is then moved into place as a
 * move would (a rename each), asking about anything in the way. */
static bool extract_one(job *operation, GFile *src) {
    char *archive = g_file_get_path(src);
    char *dest = g_file_get_path(operation->dest);
    if (archive == nullptr || dest == nullptr) {
        g_free(archive);
        g_free(dest);
        return report_error(operation, src,
                            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                                "Only archives on this computer's drives can "
                                                "be extracted."));
    }
    set_current(operation, src, operation->dest);
    char *tmp = g_build_filename(dest, ".xd-extract-XXXXXX", nullptr);
    g_free(dest);
    if (g_mkdtemp(tmp) == nullptr) {
        int error_number = errno;
        g_free(archive);
        g_free(tmp);
        return report_error(
            operation, src,
            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, g_strerror(error_number)));
    }
    char *base = g_file_get_basename(src);
    char *name = xd_archive_base_name(base);
    g_free(base);
    GError *err = nullptr;
    bool go_on = true;
    GFile *tmpf = g_file_new_for_path(tmp);
    goffset size = 0;
    GFileInfo *si = g_file_query_info(src, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                      G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    if (si != nullptr) {
        size = g_file_info_get_size(si);
        g_object_unref(si);
    }
    if (!xd_extract_archive(archive, tmp, name, operation->cancel, extract_progress, operation,
                            &err)) {
        go_on = report_error(operation, src, err);
    } else {
        add_done(operation, size > 0 ? (guint64)size : 0, 1);
        /* Moving out counts too. */
        guint64 bytes = 0;
        guint items = 0;
        measure(operation, tmpf, &bytes, &items);
        g_mutex_lock(&operation->lock);
        operation->total_bytes += bytes;
        operation->total_items += items;
        g_mutex_unlock(&operation->lock);
        if (operation->kind == XD_FILEOP_EXTRACT_FOLDER) {
            GFile *dst = g_file_get_child(operation->dest, name);
            operation->wrote = false;
            go_on = transfer(operation, tmpf, dst, true);
            if (operation->wrote)
                created(operation, dst);
            g_object_unref(dst);
        } else {
            GFileEnumerator *en = g_file_enumerate_children(tmpf, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                            operation->cancel, &err);
            if (en == nullptr)
                go_on = report_error(operation, src, err);
            GFileInfo *info;
            while (go_on && en != nullptr &&
                   (info = g_file_enumerator_next_file(en, operation->cancel, nullptr)) !=
                       nullptr) {
                const char *child = g_file_info_get_name(info);
                GFile *from = g_file_get_child(tmpf, child);
                GFile *dst = g_file_get_child(operation->dest, child);
                operation->wrote = false;
                go_on = transfer(operation, from, dst, true);
                if (operation->wrote)
                    created(operation, dst);
                g_object_unref(from);
                g_object_unref(dst);
                g_object_unref(info);
            }
            g_clear_object(&en);
        }
    }
    remove_tree(tmpf);
    g_object_unref(tmpf);
    g_free(name);
    g_free(tmp);
    g_free(archive);
    return go_on;
}

static void run_extract(job *operation) {
    guint64 bytes = 0;
    for (guint i = 0; i < operation->sources->len; i++) {
        GFileInfo *info = g_file_query_info(g_ptr_array_index(operation->sources, i),
                                            G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE,
                                            nullptr, nullptr);
        if (info != nullptr) {
            goffset size = g_file_info_get_size(info);
            bytes += size > 0 ? (guint64)size : 0;
            g_object_unref(info);
        }
    }
    g_mutex_lock(&operation->lock);
    operation->total_bytes = bytes;
    operation->total_items = operation->sources->len;
    operation->scanning = FALSE;
    g_mutex_unlock(&operation->lock);
    for (guint i = 0; i < operation->sources->len; i++)
        if (!extract_one(operation, g_ptr_array_index(operation->sources, i)))
            break;
}

static void worker(GTask *task, [[maybe_unused]] gpointer source, gpointer data,
                   [[maybe_unused]] GCancellable *cancel) {
    job *operation = data;
    switch (operation->kind) {
    case XD_FILEOP_COPY:
    case XD_FILEOP_MOVE:
    case XD_FILEOP_LINK:
        run_transfer(operation);
        break;
    case XD_FILEOP_EXTRACT_HERE:
    case XD_FILEOP_EXTRACT_FOLDER:
        run_extract(operation);
        break;
    case XD_FILEOP_TRASH:
        g_mutex_lock(&operation->lock);
        operation->total_items = operation->sources->len;
        operation->scanning = FALSE;
        g_mutex_unlock(&operation->lock);
        for (guint i = 0; i < operation->sources->len; i++)
            if (!trash_one(operation, g_ptr_array_index(operation->sources, i)))
                break;
        break;
    case XD_FILEOP_DELETE:
        measure_all(operation, operation->sources);
        for (guint i = 0; i < operation->sources->len; i++)
            if (!delete_top(operation, g_ptr_array_index(operation->sources, i)))
                break;
        break;
    }
    operation->created = g_list_reverse(operation->created);
    operation->made = g_list_reverse(operation->made);
    operation->origins = g_list_reverse(operation->origins);
    operation->trashed = g_list_reverse(operation->trashed);
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
    case XD_FILEOP_EXTRACT_HERE:
    case XD_FILEOP_EXTRACT_FOLDER:
        return "Extracting...";
    case XD_FILEOP_TRASH:
    case XD_FILEOP_DELETE:
        break;
    }
    return "Deleting...";
}

static GdkPixbuf *load_icon(GtkWidget *widget, const char *name, int px) {
    GtkIconTheme *theme = gtk_icon_theme_get_for_screen(gtk_widget_get_screen(widget));
    return gtk_icon_theme_load_icon(theme, name, px, GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
}

/* XP's animation: a sheet of paper flying from one folder to the other
 * (or into the Recycle Bin, or away when deleting). */
static gboolean draw_anim(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    job *operation = data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    /* Drawn 20 times a second: the icons are loaded once. */
    if (operation->anim_paper == nullptr) {
        operation->anim_from = load_icon(widget, "folder", 32);
        operation->anim_to = load_icon(
            widget, operation->kind == XD_FILEOP_TRASH ? "user-trash-full" : "folder", 32);
        operation->anim_paper = load_icon(widget, "text-x-generic", 24);
    }
    GdkPixbuf *from = operation->anim_from;
    GdkPixbuf *to = operation->anim_to;
    GdkPixbuf *paper = operation->anim_paper;
    double y0 = (height - 32) / 2.0;
    if (from != nullptr) {
        gdk_cairo_set_source_pixbuf(cairo, from, 4, y0);
        cairo_paint(cairo);
    }
    if (to != nullptr && operation->kind != XD_FILEOP_DELETE) {
        gdk_cairo_set_source_pixbuf(cairo, to, width - 36, y0);
        cairo_paint(cairo);
    }
    if (paper != nullptr) {
        double phase = fmod((double)(g_get_monotonic_time() - operation->started) / 1.2e6, 1.0);
        double x = 24 + phase * (width - 72);
        double arc = sin(phase * G_PI);
        double y = y0 + 4 - arc * (height / 2.0 - 8);
        gdk_cairo_set_source_pixbuf(cairo, paper, x, y);
        cairo_paint_with_alpha(cairo, operation->kind == XD_FILEOP_DELETE ? 1.0 - phase : 1.0);
    }
    return TRUE;
}

static char *remaining_text(guint64 done, guint64 total, double seconds) {
    if (done == 0 || seconds < 1.0 || total <= done)
        return g_strdup("");
    double rate = (double)done / seconds;
    double left = (double)(total - done) / rate;
    if (left < 60.0) {
        double up = ceil(left);
        int seconds_left = (int)up;
        return g_strdup_printf("%d Second%s Remaining", seconds_left, seconds_left == 1 ? "" : "s");
    }
    double up = ceil(left / 60.0);
    int minutes_left = (int)up;
    return g_strdup_printf("%d Minute%s Remaining", minutes_left, minutes_left == 1 ? "" : "s");
}

static gboolean update_dialog(gpointer data) {
    job *operation = data;
    if (operation->dialog == nullptr)
        return G_SOURCE_CONTINUE;
    g_mutex_lock(&operation->lock);
    gboolean scanning = operation->scanning;
    char *current = g_strdup(operation->current != nullptr ? operation->current : "");
    char *from = g_strdup(operation->from != nullptr ? operation->from : "");
    char *to = g_strdup(operation->to);
    guint64 done = operation->done_bytes + operation->file_bytes;
    guint64 total = operation->total_bytes;
    guint items = operation->done_items;
    guint total_items = operation->total_items;
    g_mutex_unlock(&operation->lock);

    if (scanning) {
        const char *what = operation->kind == XD_FILEOP_MOVE   ? "move"
                           : operation->kind == XD_FILEOP_COPY ? "copy"
                                                               : "delete";
        char *text = g_strdup_printf("Preparing to %s...", what);
        gtk_label_set_text(GTK_LABEL(operation->name_label), text);
        g_free(text);
        gtk_label_set_text(GTK_LABEL(operation->from_label), "");
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(operation->bar));
    } else {
        if (operation->rate_since == 0)
            operation->rate_since = g_get_monotonic_time();
        gtk_label_set_text(GTK_LABEL(operation->name_label), current);
        char *line = to != nullptr ? g_strdup_printf("From '%s' to '%s'", from, to)
                                   : g_strdup_printf("From '%s'", from);
        gtk_label_set_text(GTK_LABEL(operation->from_label), line);
        g_free(line);
        /* Bytes when there are any to count, else items. */
        double fraction = total > 0 ? (double)done / (double)total
                          : total_items > 0 ? (double)items / (double)total_items
                                            : 0.0;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(operation->bar), CLAMP(fraction, 0.0, 1.0));
        double seconds = (double)(g_get_monotonic_time() - operation->rate_since) / 1e6;
        char *left = total > 0 ? remaining_text(done, total, seconds) : g_strdup("");
        gtk_label_set_text(GTK_LABEL(operation->time_label), left);
        g_free(left);
    }
    g_free(current);
    g_free(from);
    g_free(to);
    gtk_widget_queue_draw(operation->anim);
    return G_SOURCE_CONTINUE;
}

static void on_cancel([[maybe_unused]] GtkWidget *widget, gpointer data) {
    job *operation = data;
    g_cancellable_cancel(operation->cancel);
    gtk_widget_set_sensitive(operation->cancel_button, FALSE);
}

static gboolean on_dialog_delete([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] GdkEvent *ev,
                                 gpointer data) {
    on_cancel(nullptr, data);
    return TRUE; /* closes when the worker stops */
}

static GtkWidget *left_label(GtkWidget *box) {
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    return label;
}

static gboolean show_dialog(gpointer data) {
    job *operation = data;
    if (operation->asking)
        return G_SOURCE_CONTINUE; /* not over the question */
    operation->show_timer = 0;
    GtkWidget *dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dlg), title_for(operation->kind));
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "system-file-manager");
    if (operation->parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), operation->parent);
    g_signal_connect(dlg, "delete-event", G_CALLBACK(on_dialog_delete), operation);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    operation->anim = gtk_drawing_area_new();
    gtk_widget_set_size_request(operation->anim, 272, 60);
    g_signal_connect(operation->anim, "draw", G_CALLBACK(draw_anim), operation);
    gtk_box_pack_start(GTK_BOX(box), operation->anim, FALSE, FALSE, 0);
    operation->name_label = left_label(box);
    operation->from_label = left_label(box);
    operation->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(operation->bar, 360, -1);
    gtk_box_pack_start(GTK_BOX(box), operation->bar, FALSE, FALSE, 4);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    operation->time_label = left_label(row);
    gtk_box_set_child_packing(GTK_BOX(row), operation->time_label, TRUE, TRUE, 0, GTK_PACK_START);
    operation->cancel_button = gtk_button_new_with_label("Cancel");
    gtk_widget_set_size_request(operation->cancel_button, 75, -1);
    g_signal_connect(operation->cancel_button, "clicked", G_CALLBACK(on_cancel), operation);
    gtk_box_pack_end(GTK_BOX(row), operation->cancel_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(dlg), box);
    operation->dialog = dlg;
    update_dialog(operation);
    gtk_widget_show_all(dlg);
    return G_SOURCE_REMOVE;
}

static void job_free(job *operation) {
    if (operation->show_timer != 0)
        g_source_remove(operation->show_timer);
    if (operation->update_timer != 0)
        g_source_remove(operation->update_timer);
    if (operation->dialog != nullptr)
        gtk_widget_destroy(operation->dialog);
    g_clear_object(&operation->anim_from);
    g_clear_object(&operation->anim_to);
    g_clear_object(&operation->anim_paper);
    if (operation->app != nullptr) {
        g_application_release(operation->app);
        g_object_unref(operation->app);
    }
    if (operation->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(operation->parent), (gpointer *)&operation->parent);
    g_ptr_array_unref(operation->sources);
    g_clear_object(&operation->dest);
    g_clear_object(&operation->cancel);
    g_list_free_full(operation->created, g_object_unref);
    g_list_free_full(operation->made, g_object_unref);
    g_list_free_full(operation->origins, g_object_unref);
    g_list_free_full(operation->trashed, g_object_unref);
    g_mutex_clear(&operation->lock);
    g_cond_clear(&operation->cond);
    g_free(operation->current);
    g_free(operation->from);
    g_free(operation->to);
    g_free(operation->q_title);
    g_free(operation->q_text);
    g_free(operation);
}

static void finished([[maybe_unused]] GObject *source, GAsyncResult *res, gpointer data) {
    job *operation = data;
    (void)g_task_propagate_boolean(G_TASK(res), nullptr);
    if (operation->undoable)
        xd_undo_add_fileop(operation->kind,
                           operation->kind == XD_FILEOP_TRASH ? operation->trashed
                                                              : operation->origins,
                           operation->made);
    /* No one to tell when the window that started it has closed. */
    if (operation->done != nullptr && (operation->parent != nullptr || !operation->had_parent))
        operation->done(operation->created, operation->user);
    job_free(operation);
}

/* Windows asks before anything is deleted. */
static bool confirm_delete(GtkWindow *parent, XdFileOpKind kind, GList *sources) {
    bool trash = kind == XD_FILEOP_TRASH;
    guint count = g_list_length(sources);
    char *text;
    const char *title;
    if (count == 1) {
        GFile *file = sources->data;
        char *name = display_name(file);
        bool dir = g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr) ==
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
                                       count)
                     : g_strdup_printf("Are you sure you want to delete these %u items?", count);
    }
    int response = xd_message_box(parent, title, trash ? XD_MB_QUESTION : XD_MB_WARNING,
                                  XD_MB_YES_NO, FALSE, text);
    g_free(text);
    return response == GTK_RESPONSE_YES;
}

static gboolean start(GtkWindow *parent, XdFileOpKind kind, GList *sources, GFile *dest,
                      XdFileOpDone done, gpointer user, bool undoable);

void xd_fileop_start(GtkWindow *parent, XdFileOpKind kind, GList *sources, GFile *dest,
                     XdFileOpDone done, gpointer user) {
    (void)start(parent, kind, sources, dest, done, user, false);
}

gboolean xd_fileop_start_checked(GtkWindow *parent, XdFileOpKind kind, GList *sources,
                                 GFile *dest, XdFileOpDone done, gpointer user) {
    return start(parent, kind, sources, dest, done, user, false);
}

gboolean xd_fileop_start_undoable(GtkWindow *parent, XdFileOpKind kind, GList *sources,
                                  GFile *dest, XdFileOpDone done, gpointer user) {
    return start(parent, kind, sources, dest, done, user, true);
}

static gboolean start(GtkWindow *parent, XdFileOpKind kind, GList *sources, GFile *dest,
                      XdFileOpDone done, gpointer user, bool undoable) {
    if (sources == nullptr)
        return FALSE;
    if ((kind == XD_FILEOP_TRASH || kind == XD_FILEOP_DELETE) &&
        !confirm_delete(parent, kind, sources))
        return FALSE;
    job *operation = g_new0(job, 1);
    operation->kind = kind;
    operation->undoable = undoable;
    operation->sources = g_ptr_array_new_with_free_func(g_object_unref);
    for (GList *list = sources; list != nullptr; list = list->next)
        g_ptr_array_add(operation->sources, g_object_ref(list->data));
    operation->dest = dest != nullptr ? g_object_ref(dest) : nullptr;
    if (dest == nullptr && kind != XD_FILEOP_TRASH && kind != XD_FILEOP_DELETE) {
        job_free(operation);
        return FALSE;
    }
    operation->parent = parent;
    operation->had_parent = parent != nullptr;
    if (parent != nullptr)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&operation->parent);
    operation->done = done;
    operation->user = user;
    operation->cancel = g_cancellable_new();
    operation->scanning = TRUE;
    g_mutex_init(&operation->lock);
    g_cond_init(&operation->cond);
    operation->started = g_get_monotonic_time();
    /* Closing the window that started it mustn't end the process (and
     * the copy) halfway: the progress dialog isn't one of its windows. */
    operation->app = g_application_get_default();
    if (operation->app != nullptr) {
        g_object_ref(operation->app);
        g_application_hold(operation->app);
    }
    operation->show_timer = g_timeout_add(SHOW_DELAY_MS, show_dialog, operation);
    operation->update_timer = g_timeout_add(UPDATE_MS, update_dialog, operation);

    GTask *task = g_task_new(nullptr, nullptr, finished, operation);
    g_task_set_task_data(task, operation, nullptr);
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
    return TRUE;
}

/* ---- names ---------------------------------------------------------------------------- */

char *xd_unique_name(GFile *dir, const char *base, const char *ext) {
    for (guint count = 1;; count++) {
        char *name = count == 1
                         ? g_strdup_printf("%s%s", base, ext != nullptr ? ext : "")
                         : g_strdup_printf("%s (%u)%s", base, count, ext != nullptr ? ext : "");
        GFile *file = g_file_get_child(dir, name);
        gboolean exists = g_file_query_exists(file, nullptr);
        g_object_unref(file);
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
    clip_data *clip = data;
    switch (info) {
    case TARGET_GNOME: {
        GString *string = g_string_new(clip->cut ? "cut" : "copy");
        for (char **uri = clip->uris; *uri != nullptr; uri++)
            g_string_append_printf(string, "\n%s", *uri);
        gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
                               (const guchar *)string->str, (gint)string->len);
        g_string_free(string, TRUE);
        break;
    }
    case TARGET_KDE_CUT:
        gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
                               (const guchar *)(clip->cut ? "1" : "0"), 1);
        break;
    case TARGET_URIS:
        gtk_selection_data_set_uris(sel, clip->uris);
        break;
    case TARGET_TEXT: {
        GString *string = g_string_new(nullptr);
        for (char **uri = clip->uris; *uri != nullptr; uri++) {
            GFile *file = g_file_new_for_uri(*uri);
            char *name = g_file_get_parse_name(file);
            g_string_append_printf(string, "%s%s", string->len > 0 ? "\n" : "", name);
            g_free(name);
            g_object_unref(file);
        }
        gtk_selection_data_set_text(sel, string->str, (gint)string->len);
        g_string_free(string, TRUE);
        break;
    }
    default:
        break;
    }
}

static void clip_clear([[maybe_unused]] GtkClipboard *cb, gpointer data) {
    clip_data *clip = data;
    if (clip_ours == clip)
        clip_ours = nullptr;
    g_strfreev(clip->uris);
    g_free(clip);
}

static void got_targets([[maybe_unused]] GtkClipboard *cb, GdkAtom *atoms, gint atom_count,
                        [[maybe_unused]] gpointer data) {
    clip_has_files = FALSE;
    for (gint i = 0; i < atom_count; i++) {
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
    clip_data *clip = g_new0(clip_data, 1);
    clip->cut = cut;
    clip->uris = g_new0(char *, g_list_length(files) + 1);
    guint k = 0;
    for (GList *node = files; node != nullptr; node = node->next)
        clip->uris[k++] = g_file_get_uri(node->data);

    GtkTargetList *list = gtk_target_list_new(nullptr, 0);
    gtk_target_list_add(list, gdk_atom_intern_static_string(GNOME_FILES), 0, TARGET_GNOME);
    gtk_target_list_add(list, gdk_atom_intern_static_string(KDE_CUT), 0, TARGET_KDE_CUT);
    gtk_target_list_add_uri_targets(list, TARGET_URIS);
    gtk_target_list_add_text_targets(list, TARGET_TEXT);
    gint count = 0;
    GtkTargetEntry *entries = gtk_target_table_new_from_list(list, &count);
    gtk_target_list_unref(list);
    if (gtk_clipboard_set_with_data(cb, entries, (guint)count, clip_get, clip_clear, clip)) {
        clip_ours = clip;
        clip_has_files = TRUE;
    } else {
        g_strfreev(clip->uris);
        g_free(clip);
    }
    gtk_target_table_free(entries, count);
}

typedef struct clip_request {
    XdClipboardFiles got;
    gpointer user;
} clip_request;

static GList *files_from_uris(char **uris) {
    GList *files = nullptr;
    for (char **uri = uris; uri != nullptr && *uri != nullptr; uri++)
        if (**uri != '\0')
            files = g_list_prepend(files, g_file_new_for_uri(*uri));
    return g_list_reverse(files);
}

static void got_uris([[maybe_unused]] GtkClipboard *cb, char **uris, gpointer data) {
    clip_request *clip_query = data;
    GList *files = files_from_uris(uris);
    clip_query->got(files, FALSE, clip_query->user);
    g_list_free_full(files, g_object_unref);
    g_free(clip_query);
}

static void got_gnome(GtkClipboard *cb, GtkSelectionData *sel, gpointer data) {
    clip_request *clip_query = data;
    gint len = gtk_selection_data_get_length(sel);
    if (len <= 0) {
        gtk_clipboard_request_uris(cb, got_uris, clip_query);
        return;
    }
    char *text = g_strndup((const char *)gtk_selection_data_get_data(sel), (gsize)len);
    char **lines = g_strsplit(text, "\n", -1);
    g_free(text);
    gboolean cut = lines[0] != nullptr && g_strcmp0(g_strstrip(lines[0]), "cut") == 0;
    for (char **line = lines; *line != nullptr; line++)
        g_strstrip(*line);
    GList *files = lines[0] != nullptr ? files_from_uris(lines + 1) : nullptr;
    g_strfreev(lines);
    clip_query->got(files, cut, clip_query->user);
    g_list_free_full(files, g_object_unref);
    g_free(clip_query);
}

void xd_clipboard_get_files(GtkWidget *widget, XdClipboardFiles got, gpointer user) {
    clip_request *clip_query = g_new0(clip_request, 1);
    clip_query->got = got;
    clip_query->user = user;
    gtk_clipboard_request_contents(
        clipboard_for(widget), gdk_atom_intern_static_string(GNOME_FILES), got_gnome, clip_query);
}

gboolean xd_clipboard_has_files(void) { return clip_has_files; }

void xd_clipboard_clear_cut(GtkWidget *widget) {
    if (clip_ours != nullptr && clip_ours->cut)
        gtk_clipboard_clear(clipboard_for(widget));
}
