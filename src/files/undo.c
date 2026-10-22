/* SPDX-License-Identifier: MIT
 * Edit > Undo for xd-files. See undo.h.
 */
#include "undo.h"

#include "../gtkapp/gtkapp.h"
#include "places.h"
#include "rename.h"

enum { UNDO_MAX = 10 }; /* as many as Windows keeps */

typedef enum { UNDO_RENAME, UNDO_MOVE, UNDO_COPY, UNDO_EXTRACT, UNDO_DELETE } undo_kind;

typedef struct undo_entry {
    undo_kind kind;
    GList *from;    /* move: where each came from; delete: what was sent */
    GList *to;      /* move, copy: the files made; rename: the file now */
    char *old_text; /* rename */
} undo_entry;

static GQueue stack = G_QUEUE_INIT; /* latest first */

static void entry_free(gpointer user_data) {
    undo_entry *undo_step = user_data;
    g_list_free_full(undo_step->from, g_object_unref);
    g_list_free_full(undo_step->to, g_object_unref);
    g_free(undo_step->old_text);
    g_free(undo_step);
}

static void push(undo_entry *undo_step) {
    g_queue_push_head(&stack, undo_step);
    while (g_queue_get_length(&stack) > UNDO_MAX)
        entry_free(g_queue_pop_tail(&stack));
}

static GList *copy_files(GList *files) {
    GList *out = nullptr;
    for (GList *list = files; list != nullptr; list = list->next)
        out = g_list_prepend(out, g_object_ref(list->data));
    return g_list_reverse(out);
}

/* Moves out of the Recycle Bin are Restore's business: not undone. */
static gboolean in_trash(GList *files) {
    GPtrArray *dirs = xd_trash_dirs();
    gboolean found = FALSE;
    for (GList *list = files; list != nullptr && !found; list = list->next)
        for (guint i = 0; i < dirs->len && !found; i++)
            found = g_file_has_prefix(list->data, g_ptr_array_index(dirs, i));
    g_ptr_array_unref(dirs);
    return found;
}

void xd_undo_add_fileop(XdFileOpKind kind, GList *from, GList *created) {
    undo_kind step_kind;
    switch (kind) {
    case XD_FILEOP_COPY:
        step_kind = UNDO_COPY;
        break;
    case XD_FILEOP_MOVE:
        step_kind = UNDO_MOVE;
        break;
    case XD_FILEOP_EXTRACT_HERE:
    case XD_FILEOP_EXTRACT_FOLDER:
        step_kind = UNDO_EXTRACT;
        break;
    case XD_FILEOP_TRASH:
        step_kind = UNDO_DELETE;
        break;
    case XD_FILEOP_LINK:
    case XD_FILEOP_DELETE:
    default:
        return;
    }
    if (step_kind == UNDO_DELETE ? from == nullptr : created == nullptr)
        return; /* nothing was done */
    if (step_kind == UNDO_MOVE && in_trash(from))
        return;
    undo_entry *undo_step = g_new0(undo_entry, 1);
    undo_step->kind = step_kind;
    undo_step->from = copy_files(from);
    undo_step->to = copy_files(created);
    push(undo_step);
}

void xd_undo_add_rename(GFile *renamed, const char *old_text) {
    undo_entry *undo_step = g_new0(undo_entry, 1);
    undo_step->kind = UNDO_RENAME;
    undo_step->to = g_list_append(nullptr, g_object_ref(renamed));
    undo_step->old_text = g_strdup(old_text);
    push(undo_step);
}

const char *xd_undo_label(void) {
    const undo_entry *undo_step = g_queue_peek_head(&stack);
    if (undo_step == nullptr)
        return nullptr;
    switch (undo_step->kind) {
    case UNDO_RENAME:
        return "_Undo Rename";
    case UNDO_MOVE:
        return "_Undo Move";
    case UNDO_COPY:
        return "_Undo Copy";
    case UNDO_EXTRACT:
        return "_Undo Extract";
    case UNDO_DELETE:
        return "_Undo Delete";
    }
    return nullptr;
}

static void undo_rename(GtkWindow *parent, undo_entry *undo_step, XdFileOpDone done,
                        gpointer user) {
    GFile *now = undo_step->to->data;
    GError *err = nullptr;
    GFile *back = xd_rename(now, undo_step->old_text, &err);
    if (back == nullptr) {
        char *name = xd_rename_text(now);
        char *msg = g_strdup_printf("Cannot rename %s: %s", name, err->message);
        xd_message_box(parent, "Error Renaming File or Folder", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
        g_free(name);
        g_error_free(err);
        return;
    }
    GList *one = g_list_append(nullptr, back);
    if (done != nullptr)
        done(one, user);
    g_list_free_full(one, g_object_unref);
}

/* Each file goes back to the folder it came from: one move per folder. */
static void undo_move(GtkWindow *parent, undo_entry *undo_step, XdFileOpDone done, gpointer user) {
    GHashTable *by_dir = g_hash_table_new_full(g_file_hash, (GEqualFunc)g_file_equal,
                                               g_object_unref, nullptr);
    GList *dirs = nullptr;
    for (GList *from = undo_step->from, *to = undo_step->to; from != nullptr && to != nullptr;
         from = from->next, to = to->next) {
        GFile *dir = g_file_get_parent(from->data);
        if (dir == nullptr)
            continue;
        GList *files = g_hash_table_lookup(by_dir, dir);
        if (files == nullptr)
            dirs = g_list_append(dirs, g_object_ref(dir));
        files = g_list_append(files, to->data);
        g_hash_table_replace(by_dir, dir, files);
    }
    for (GList *dir_node = dirs; dir_node != nullptr; dir_node = dir_node->next) {
        GList *files = g_hash_table_lookup(by_dir, dir_node->data);
        /* Files moved out of a folder merged into another came from a
         * folder the move then removed: it's made again. */
        g_file_make_directory_with_parents(dir_node->data, nullptr, nullptr);
        xd_fileop_start(parent, XD_FILEOP_MOVE, files, dir_node->data, done, user);
        g_list_free(files);
    }
    g_list_free_full(dirs, g_object_unref);
    g_hash_table_unref(by_dir);
}

/* What was sent to the Recycle Bin comes back from it. */
static void undo_delete(GtkWindow *parent, undo_entry *undo_step, XdFileOpDone done,
                        gpointer user) {
    GList *trashed = xd_trash_find(undo_step->from);
    if (trashed == nullptr) {
        xd_message_box(parent, "Undo Delete", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The files are no longer in the Recycle Bin.");
        return;
    }
    xd_trash_restore(parent, trashed);
    g_list_free_full(trashed, g_object_unref);
    if (done != nullptr)
        done(undo_step->from, user);
}

void xd_undo(GtkWindow *parent, XdFileOpDone done, gpointer user) {
    undo_entry *undo_step = g_queue_pop_head(&stack);
    if (undo_step == nullptr)
        return;
    switch (undo_step->kind) {
    case UNDO_RENAME:
        undo_rename(parent, undo_step, done, user);
        break;
    case UNDO_MOVE:
        undo_move(parent, undo_step, done, user);
        break;
    case UNDO_COPY:
    case UNDO_EXTRACT:
        /* The copies (or what was extracted) go to the Recycle Bin, after the usual question;
         * answered No, there's still that to undo. */
        if (!xd_fileop_start_checked(parent, XD_FILEOP_TRASH, undo_step->to, nullptr, nullptr,
                                     nullptr)) {
            g_queue_push_head(&stack, undo_step);
            return;
        }
        break;
    case UNDO_DELETE:
        undo_delete(parent, undo_step, done, user);
        break;
    }
    entry_free(undo_step);
}
