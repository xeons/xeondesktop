/* SPDX-License-Identifier: MIT
 * Edit > Undo for xd-files. See undo.h.
 */
#include "undo.h"

#include "../gtkapp/gtkapp.h"
#include "places.h"
#include "rename.h"

enum { UNDO_MAX = 10 }; /* as many as Windows keeps */

typedef enum { UNDO_RENAME, UNDO_MOVE, UNDO_COPY, UNDO_DELETE } undo_kind;

typedef struct undo_entry {
    undo_kind kind;
    GList *from;    /* move: where each came from; delete: what was sent */
    GList *to;      /* move, copy: the files made; rename: the file now */
    char *old_text; /* rename */
} undo_entry;

static GQueue stack = G_QUEUE_INIT; /* latest first */

static void entry_free(gpointer p) {
    undo_entry *u = p;
    g_list_free_full(u->from, g_object_unref);
    g_list_free_full(u->to, g_object_unref);
    g_free(u->old_text);
    g_free(u);
}

static void push(undo_entry *u) {
    g_queue_push_head(&stack, u);
    while (g_queue_get_length(&stack) > UNDO_MAX)
        entry_free(g_queue_pop_tail(&stack));
}

static GList *copy_files(GList *files) {
    GList *out = nullptr;
    for (GList *l = files; l != nullptr; l = l->next)
        out = g_list_prepend(out, g_object_ref(l->data));
    return g_list_reverse(out);
}

/* Moves out of the Recycle Bin are Restore's business: not undone. */
static gboolean in_trash(GList *files) {
    GPtrArray *dirs = xd_trash_dirs();
    gboolean found = FALSE;
    for (GList *l = files; l != nullptr && !found; l = l->next)
        for (guint i = 0; i < dirs->len && !found; i++)
            found = g_file_has_prefix(l->data, g_ptr_array_index(dirs, i));
    g_ptr_array_unref(dirs);
    return found;
}

void xd_undo_add_fileop(XdFileOpKind kind, GList *from, GList *created) {
    undo_kind k;
    switch (kind) {
    case XD_FILEOP_COPY:
        k = UNDO_COPY;
        break;
    case XD_FILEOP_MOVE:
        k = UNDO_MOVE;
        break;
    case XD_FILEOP_TRASH:
        k = UNDO_DELETE;
        break;
    case XD_FILEOP_LINK:
    case XD_FILEOP_DELETE:
    default:
        return;
    }
    if (k == UNDO_DELETE ? from == nullptr : created == nullptr)
        return; /* nothing was done */
    if (k == UNDO_MOVE && in_trash(from))
        return;
    undo_entry *u = g_new0(undo_entry, 1);
    u->kind = k;
    u->from = copy_files(from);
    u->to = copy_files(created);
    push(u);
}

void xd_undo_add_rename(GFile *renamed, const char *old_text) {
    undo_entry *u = g_new0(undo_entry, 1);
    u->kind = UNDO_RENAME;
    u->to = g_list_append(nullptr, g_object_ref(renamed));
    u->old_text = g_strdup(old_text);
    push(u);
}

const char *xd_undo_label(void) {
    const undo_entry *u = g_queue_peek_head(&stack);
    if (u == nullptr)
        return nullptr;
    switch (u->kind) {
    case UNDO_RENAME:
        return "_Undo Rename";
    case UNDO_MOVE:
        return "_Undo Move";
    case UNDO_COPY:
        return "_Undo Copy";
    case UNDO_DELETE:
        return "_Undo Delete";
    }
    return nullptr;
}

static void undo_rename(GtkWindow *parent, undo_entry *u, XdFileOpDone done, gpointer user) {
    GFile *now = u->to->data;
    GError *err = nullptr;
    GFile *back = xd_rename(now, u->old_text, &err);
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
static void undo_move(GtkWindow *parent, undo_entry *u, XdFileOpDone done, gpointer user) {
    GHashTable *by_dir = g_hash_table_new_full(g_file_hash, (GEqualFunc)g_file_equal,
                                               g_object_unref, nullptr);
    GList *dirs = nullptr;
    for (GList *f = u->from, *t = u->to; f != nullptr && t != nullptr; f = f->next, t = t->next) {
        GFile *dir = g_file_get_parent(f->data);
        if (dir == nullptr)
            continue;
        GList *files = g_hash_table_lookup(by_dir, dir);
        if (files == nullptr)
            dirs = g_list_append(dirs, g_object_ref(dir));
        files = g_list_append(files, t->data);
        g_hash_table_replace(by_dir, dir, files);
    }
    for (GList *d = dirs; d != nullptr; d = d->next) {
        GList *files = g_hash_table_lookup(by_dir, d->data);
        xd_fileop_start(parent, XD_FILEOP_MOVE, files, d->data, done, user);
        g_list_free(files);
    }
    g_list_free_full(dirs, g_object_unref);
    g_hash_table_unref(by_dir);
}

/* What was sent to the Recycle Bin comes back from it. */
static void undo_delete(GtkWindow *parent, undo_entry *u, XdFileOpDone done, gpointer user) {
    GList *trashed = xd_trash_find(u->from);
    if (trashed == nullptr) {
        xd_message_box(parent, "Undo Delete", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The files are no longer in the Recycle Bin.");
        return;
    }
    xd_trash_restore(parent, trashed);
    g_list_free_full(trashed, g_object_unref);
    if (done != nullptr)
        done(u->from, user);
}

void xd_undo(GtkWindow *parent, XdFileOpDone done, gpointer user) {
    undo_entry *u = g_queue_pop_head(&stack);
    if (u == nullptr)
        return;
    switch (u->kind) {
    case UNDO_RENAME:
        undo_rename(parent, u, done, user);
        break;
    case UNDO_MOVE:
        undo_move(parent, u, done, user);
        break;
    case UNDO_COPY:
        /* The copies go to the Recycle Bin, after the usual question;
         * answered No, there's still that to undo. */
        if (!xd_fileop_start_checked(parent, XD_FILEOP_TRASH, u->to, nullptr, nullptr, nullptr)) {
            g_queue_push_head(&stack, u);
            return;
        }
        break;
    case UNDO_DELETE:
        undo_delete(parent, u, done, user);
        break;
    }
    entry_free(u);
}
