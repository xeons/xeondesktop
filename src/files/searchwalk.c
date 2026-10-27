/* SPDX-License-Identifier: MIT
 * xd-files' search: walking the folders on a worker thread. See search.h.
 *
 * The worker takes folders off a queue (breadth first, as XP's results
 * came in), lists each with GFileEnumerator without following links, and
 * puts what matches in `pending`; the main thread takes those a few times
 * a second and hands them to the window, which shows them as they come.
 * A folder's device and inode are remembered, so bind mounts can't make
 * it go round in circles.
 */
#include "search.h"

#include <string.h>

#include "extract.h"

/* What the folder view shows of a file (folderview.c's), and what the
 * walk itself needs. */
#define WALK_ATTRIBUTES                                                                     \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME              \
    "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE                  \
    "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_ICON          \
    "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP        \
    "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK "," G_FILE_ATTRIBUTE_TIME_MODIFIED            \
    "," G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_TIME_ACCESS                      \
    "," G_FILE_ATTRIBUTE_OWNER_USER "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE                  \
    "," G_FILE_ATTRIBUTE_UNIX_DEVICE "," G_FILE_ATTRIBUTE_UNIX_INODE

struct xd_search_job {
    gint refs; /* the main thread's and the worker's */
    GCancellable *cancel;
    xd_search_query *query;
    char **roots;
    bool subfolders, hidden, system, archives;
    xd_search_callbacks callbacks;
    guint drain_source;
    GMutex lock; /* the rest */
    GPtrArray *pending; /* xd_search_hit* */
    char *folder;
    double fraction;
    guint found;
    bool finished;
};

void xd_search_hit_free(gpointer data) {
    xd_search_hit *hit = data;
    if (hit == nullptr)
        return;
    g_clear_object(&hit->file);
    g_clear_object(&hit->info);
    g_free(hit->archive);
    g_free(hit);
}

static void job_unref(xd_search_job *job) {
    if (!g_atomic_int_dec_and_test(&job->refs))
        return;
    g_object_unref(job->cancel);
    xd_search_query_unref(job->query);
    g_strfreev(job->roots);
    g_ptr_array_unref(job->pending);
    g_free(job->folder);
    g_mutex_clear(&job->lock);
    g_free(job);
}

bool xd_search_is_system_folder(const char *path, bool *pseudo) {
    static const char *const pseudo_folders[] = {"/proc", "/sys", "/dev", "/run"};
    static const char *const system_folders[] = {"/usr",  "/etc", "/var",  "/boot", "/opt",
                                                 "/srv",  "/bin", "/sbin", "/lib",  "/lib32",
                                                 "/lib64", "/libx32", "/snap", "/lost+found"};
    *pseudo = false;
    for (size_t i = 0; i < G_N_ELEMENTS(pseudo_folders); i++) {
        if (strcmp(path, pseudo_folders[i]) == 0) {
            *pseudo = true;
            return true;
        }
    }
    for (size_t i = 0; i < G_N_ELEMENTS(system_folders); i++)
        if (strcmp(path, system_folders[i]) == 0)
            return true;
    return false;
}

/* ---- the worker ------------------------------------------------------------------------ */

typedef struct reading {
    GFile *file;
    uint64_t size;
    GCancellable *cancel;
} reading;

static GBytes *read_file(const void *data) {
    const reading *what = data;
    if (what->size > XD_SEARCH_CONTENT_MAX)
        return nullptr;
    char *contents = nullptr;
    gsize length = 0;
    if (!g_file_load_contents(what->file, what->cancel, &contents, &length, nullptr, nullptr))
        return nullptr;
    return g_bytes_new_take(contents, length);
}

static void add_hit(xd_search_job *job, GFile *file, GFileInfo *info, const char *archive) {
    xd_search_hit *hit = g_new0(xd_search_hit, 1);
    hit->file = g_object_ref(file);
    hit->info = g_object_ref(info);
    hit->archive = g_strdup(archive);
    g_mutex_lock(&job->lock);
    g_ptr_array_add(job->pending, hit);
    job->found++;
    g_mutex_unlock(&job->lock);
}

static int64_t info_time(GFileInfo *info, const char *attribute) {
    return g_file_info_has_attribute(info, attribute)
               ? (int64_t)g_file_info_get_attribute_uint64(info, attribute)
               : 0;
}

typedef struct archive_walk {
    xd_search_job *job;
    GFile *archive;
    const char *archive_path;
} archive_walk;

/* An entry inside an archive: shown as if the archive were a folder
 * ("photos.zip/2026/beach.jpg"), its name, size and date its own. */
static gboolean archive_entry(const char *path, gboolean is_dir, guint64 size, gint64 modified,
                              gpointer data) {
    archive_walk *walk = data;
    if (g_cancellable_is_cancelled(walk->job->cancel))
        return FALSE;
    g_autofree char *trimmed = g_strdup(path);
    size_t length = strlen(trimmed);
    while (length > 0 && trimmed[length - 1] == '/')
        trimmed[--length] = '\0';
    if (length == 0)
        return TRUE;
    g_autofree char *name = g_path_get_basename(trimmed);
    g_autofree char *content_type =
        is_dir ? g_strdup("inode/directory") : g_content_type_guess(name, nullptr, 0, nullptr);
    xd_search_item item = {
        .name = name,
        .is_dir = is_dir,
        .size = size,
        .modified = modified,
        .content_type = content_type,
    };
    bool matches = xd_search_query_matches(walk->job->query, &item);
    xd_search_item_clear(&item);
    if (!matches)
        return TRUE;
    g_autoptr(GFileInfo) info = g_file_info_new();
    g_file_info_set_name(info, name);
    g_autofree char *display = g_filename_display_name(name);
    g_file_info_set_display_name(info, display);
    g_file_info_set_file_type(info, is_dir ? G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_REGULAR);
    g_file_info_set_size(info, (goffset)size);
    g_file_info_set_content_type(info, content_type);
    g_autoptr(GIcon) icon = g_content_type_get_icon(content_type);
    g_file_info_set_icon(info, icon);
    g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(modified);
    if (when != nullptr)
        g_file_info_set_modification_date_time(info, when);
    g_autoptr(GFile) file = g_file_resolve_relative_path(walk->archive, trimmed);
    add_hit(walk->job, file, info, walk->archive_path);
    return TRUE;
}

static void search_archive(xd_search_job *job, GFile *file) {
    g_autofree char *path = g_file_get_path(file);
    if (path == nullptr)
        return;
    archive_walk walk = {job, file, path};
    (void)xd_archive_list(path, job->cancel, archive_entry, &walk, nullptr);
}

typedef struct walk_state {
    GQueue folders; /* char* paths */
    GHashTable *seen; /* "device:inode" */
    guint done;
} walk_state;

static void set_progress(xd_search_job *job, walk_state *walk, const char *folder) {
    double fraction = (double)walk->done / (double)(walk->done + walk->folders.length + 1);
    g_mutex_lock(&job->lock);
    if (fraction > job->fraction)
        job->fraction = fraction;
    g_free(job->folder);
    job->folder = g_strdup(folder);
    g_mutex_unlock(&job->lock);
}

/* True the first time a folder (by device and inode) comes up. */
static bool first_visit(walk_state *walk, GFileInfo *info) {
    if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE))
        return true;
    char *key = g_strdup_printf(
        "%u:%" G_GUINT64_FORMAT, g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE),
        g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE));
    return g_hash_table_add(walk->seen, key);
}

static void consider(xd_search_job *job, walk_state *walk, GFile *dir, GFileInfo *info) {
    GFile *child = g_file_get_child(dir, g_file_info_get_name(info));
    bool hidden = g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info);
    if (hidden && !job->hidden) {
        g_object_unref(child);
        return;
    }
    GFileType type = g_file_info_get_file_type(info);
    bool is_link = g_file_info_get_is_symlink(info) || type == G_FILE_TYPE_SYMBOLIC_LINK;
    bool is_dir = type == G_FILE_TYPE_DIRECTORY;
    const char *content_type = g_file_info_get_content_type(info);
    reading what = {child, (uint64_t)g_file_info_get_size(info), job->cancel};
    xd_search_item item = {
        .name = g_file_info_get_name(info),
        .is_dir = is_dir,
        .is_link = is_link,
        .size = (uint64_t)g_file_info_get_size(info),
        .modified = info_time(info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
        .created = info_time(info, G_FILE_ATTRIBUTE_TIME_CREATED),
        .accessed = info_time(info, G_FILE_ATTRIBUTE_TIME_ACCESS),
        .content_type = content_type,
        /* Links' targets aren't read: XP didn't search a shortcut's target. */
        .read_contents = type == G_FILE_TYPE_REGULAR && !is_link ? read_file : nullptr,
        .read_data = &what,
    };
    if (xd_search_query_matches(job->query, &item))
        add_hit(job, child, info, nullptr);
    xd_search_item_clear(&item);
    if (is_dir && !is_link && job->subfolders) {
        char *path = g_file_get_path(child);
        bool pseudo = false;
        bool system = path != nullptr && xd_search_is_system_folder(path, &pseudo);
        if (path != nullptr && (!system || (!pseudo && job->system)) && first_visit(walk, info))
            g_queue_push_tail(&walk->folders, path);
        else
            g_free(path);
    }
    if (job->archives && type == G_FILE_TYPE_REGULAR && !is_link && xd_extract_available() &&
        xd_is_archive_type(content_type))
        search_archive(job, child);
    g_object_unref(child);
}

static gpointer walk_thread(gpointer data) {
    xd_search_job *job = data;
    walk_state walk = {G_QUEUE_INIT, g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr),
                       0};
    for (size_t i = 0; job->roots != nullptr && job->roots[i] != nullptr; i++)
        g_queue_push_tail(&walk.folders, g_strdup(job->roots[i]));
    char *path = nullptr;
    while (!g_cancellable_is_cancelled(job->cancel) &&
           (path = g_queue_pop_head(&walk.folders)) != nullptr) {
        set_progress(job, &walk, path);
        GFile *dir = g_file_new_for_path(path);
        GFileEnumerator *children = g_file_enumerate_children(
            dir, WALK_ATTRIBUTES, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancel, nullptr);
        for (GFileInfo *info = nullptr;
             children != nullptr && !g_cancellable_is_cancelled(job->cancel) &&
             (info = g_file_enumerator_next_file(children, job->cancel, nullptr)) != nullptr;) {
            consider(job, &walk, dir, info);
            g_object_unref(info);
        }
        g_clear_object(&children);
        g_object_unref(dir);
        g_free(path);
        walk.done++;
    }
    g_queue_clear_full(&walk.folders, g_free);
    g_hash_table_unref(walk.seen);
    g_mutex_lock(&job->lock);
    job->finished = true;
    job->fraction = 1.0;
    g_mutex_unlock(&job->lock);
    job_unref(job);
    return nullptr;
}

/* ---- the main thread's side -------------------------------------------------------------- */

static gboolean drain(gpointer data) {
    xd_search_job *job = data;
    /* The callbacks may cancel the job (the window closed, a new search),
     * which lets go of the main thread's reference: hold one meanwhile. */
    g_atomic_int_inc(&job->refs);
    g_mutex_lock(&job->lock);
    GPtrArray *hits = job->pending;
    job->pending = g_ptr_array_new_with_free_func(xd_search_hit_free);
    bool finished = job->finished;
    double fraction = job->fraction;
    char *folder = g_strdup(job->folder);
    guint found = job->found;
    g_mutex_unlock(&job->lock);
    if (hits->len > 0 && job->callbacks.found != nullptr)
        job->callbacks.found(hits, job->callbacks.data);
    g_ptr_array_unref(hits);
    if (!g_cancellable_is_cancelled(job->cancel) && job->callbacks.progress != nullptr)
        job->callbacks.progress(fraction, folder != nullptr ? folder : "", job->callbacks.data);
    g_free(folder);
    gboolean keep = G_SOURCE_CONTINUE;
    if (!g_cancellable_is_cancelled(job->cancel) && finished) {
        job->drain_source = 0;
        keep = G_SOURCE_REMOVE;
        if (job->callbacks.done != nullptr)
            job->callbacks.done(found, job->callbacks.data);
        job_unref(job);
    }
    job_unref(job);
    return keep;
}

xd_search_job *xd_search_job_start(const xd_search_criteria *criteria, xd_search_query *query,
                                   const xd_search_callbacks *callbacks) {
    xd_search_job *job = g_new0(xd_search_job, 1);
    job->refs = 2;
    job->cancel = g_cancellable_new();
    job->query = xd_search_query_ref(query);
    job->roots = g_strdupv(criteria->look_in);
    job->subfolders = criteria->subfolders;
    job->hidden = criteria->hidden;
    job->system = criteria->system;
    job->archives = criteria->archives;
    job->callbacks = *callbacks;
    g_mutex_init(&job->lock);
    job->pending = g_ptr_array_new_with_free_func(xd_search_hit_free);
    job->drain_source = g_timeout_add(100, drain, job);
    g_thread_unref(g_thread_new("xd-search", walk_thread, job));
    return job;
}

void xd_search_job_cancel(xd_search_job *job) {
    if (job == nullptr || g_cancellable_is_cancelled(job->cancel))
        return;
    g_cancellable_cancel(job->cancel);
    if (job->drain_source != 0) {
        /* Called from a callback inside drain() too: the source goes when
         * it returns. */
        GSource *source = g_main_context_find_source_by_id(nullptr, job->drain_source);
        if (source != nullptr && !g_source_is_destroyed(source))
            g_source_destroy(source);
        job->drain_source = 0;
        job_unref(job);
    }
}
