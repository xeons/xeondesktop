/* SPDX-License-Identifier: MIT */
#include "queue.h"

/* A backstop for a thumbnailer that never ends (it has its own limit). */
static constexpr guint TIMEOUT_SECONDS = 60;

typedef struct request {
    char *uri;
    XdThumbSize size;
    guint64 batch; /* the latest batch it was asked for in */
    guint64 order; /* first asked, first done within a batch */
} request;

typedef struct job {
    XdThumbQueue *q; /* nullptr once cancelled: it only cleans up */
    char *uri;
    GSubprocess *proc;
    GCancellable *cancel;
    guint timeout;
} job;

struct XdThumbQueue {
    XdThumbDone done;
    gpointer data;
    GHashTable *pending; /* uri -> request */
    GHashTable *running; /* uri (the job's) -> job */
    guint64 batch, order;
    guint pump;
    guint max;
};

static void request_free(gpointer user_data) {
    request *queued = user_data;
    g_free(queued->uri);
    g_free(queued);
}

static void job_free(job *running_job) {
    g_clear_object(&running_job->proc);
    g_clear_object(&running_job->cancel);
    g_free(running_job->uri);
    g_free(running_job);
}

XdThumbQueue *xd_thumb_queue_new(XdThumbDone done, gpointer data) {
    XdThumbQueue *thumb_queue = g_new0(XdThumbQueue, 1);
    thumb_queue->done = done;
    thumb_queue->data = data;
    thumb_queue->pending = g_hash_table_new_full(g_str_hash, g_str_equal, nullptr, request_free);
    thumb_queue->running = g_hash_table_new(g_str_hash, g_str_equal);
    /* Each decode is one core's work; leave the rest to the user. */
    thumb_queue->max = (guint)CLAMP(g_get_num_processors() / 2, 1, 4);
    return thumb_queue;
}

static void schedule(XdThumbQueue *thumb_queue);

static void job_finished(GObject *source, GAsyncResult *res, gpointer data) {
    job *running_job = data;
    char *out = nullptr;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, nullptr,
                                                       nullptr);
    XdThumbQueue *thumb_queue = running_job->q;
    if (thumb_queue != nullptr) {
        g_hash_table_remove(thumb_queue->running, running_job->uri);
        if (running_job->timeout != 0)
            g_source_remove(running_job->timeout);
        const char *path = ok && g_subprocess_get_successful(running_job->proc) && out != nullptr
                               ? g_strstrip(out)
                               : nullptr;
        thumb_queue->done(running_job->uri, path != nullptr && *path != '\0' ? path : nullptr,
                          thumb_queue->data);
        schedule(thumb_queue);
    }
    g_free(out);
    job_free(running_job);
}

static gboolean job_timeout(gpointer data) {
    job *running_job = data;
    running_job->timeout = 0;
    g_subprocess_force_exit(running_job->proc);
    return G_SOURCE_REMOVE;
}

static void start(XdThumbQueue *thumb_queue, request *queued) {
    char *size = g_strconcat("--size=", xd_thumb_size_name(queued->size), nullptr);
    const char *argv[] = {"xd-thumbnailer", size, queued->uri, nullptr};
    GSubprocess *proc = g_subprocess_newv(
        argv, (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
        nullptr);
    g_free(size);
    if (proc == nullptr) {
        thumb_queue->done(queued->uri, nullptr, thumb_queue->data);
        return;
    }
    job *running_job = g_new0(job, 1);
    running_job->q = thumb_queue;
    running_job->uri = g_strdup(queued->uri);
    running_job->proc = proc;
    running_job->cancel = g_cancellable_new();
    running_job->timeout = g_timeout_add_seconds(TIMEOUT_SECONDS, job_timeout, running_job);
    g_hash_table_insert(thumb_queue->running, running_job->uri, running_job);
    g_subprocess_communicate_utf8_async(proc, nullptr, running_job->cancel, job_finished,
                                        running_job);
}

/* The next request: from the latest batch, the first asked. */
static request *next(XdThumbQueue *thumb_queue) {
    request *best = nullptr;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, thumb_queue->pending);
    while (g_hash_table_iter_next(&iter, nullptr, &value)) {
        request *queued = value;
        if (best == nullptr || queued->batch > best->batch ||
            (queued->batch == best->batch && queued->order < best->order))
            best = queued;
    }
    return best;
}

static gboolean pump(gpointer data) {
    XdThumbQueue *thumb_queue = data;
    thumb_queue->pump = 0;
    thumb_queue->batch++; /* what's asked for from now on is newer */
    while (g_hash_table_size(thumb_queue->running) < thumb_queue->max) {
        request *queued = next(thumb_queue);
        if (queued == nullptr)
            break;
        g_hash_table_steal(thumb_queue->pending, queued->uri);
        start(thumb_queue, queued);
        request_free(queued);
    }
    return G_SOURCE_REMOVE;
}

/* After the current main loop iteration: a view's whole redraw is one
 * batch. */
static void schedule(XdThumbQueue *thumb_queue) {
    if (thumb_queue->pump == 0)
        thumb_queue->pump = g_idle_add(pump, thumb_queue);
}

void xd_thumb_queue_request(XdThumbQueue *thumb_queue, const char *uri, XdThumbSize size) {
    if (g_hash_table_contains(thumb_queue->running, uri))
        return;
    request *queued = g_hash_table_lookup(thumb_queue->pending, uri);
    if (queued == nullptr) {
        queued = g_new0(request, 1);
        queued->uri = g_strdup(uri);
        queued->size = size;
        queued->order = thumb_queue->order++;
        g_hash_table_insert(thumb_queue->pending, queued->uri, queued);
    }
    queued->size = MAX(queued->size, size);
    queued->batch = thumb_queue->batch;
    schedule(thumb_queue);
}

void xd_thumb_queue_cancel(XdThumbQueue *thumb_queue) {
    g_hash_table_remove_all(thumb_queue->pending);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, thumb_queue->running);
    while (g_hash_table_iter_next(&iter, nullptr, &value)) {
        job *running_job = value;
        running_job->q = nullptr;
        if (running_job->timeout != 0)
            g_source_remove(running_job->timeout);
        running_job->timeout = 0;
        g_cancellable_cancel(running_job->cancel);
        /* xd-thumbnailer's sandbox dies with it (bwrap --die-with-parent). */
        g_subprocess_force_exit(running_job->proc);
    }
    g_hash_table_remove_all(thumb_queue->running);
}

void xd_thumb_queue_free(XdThumbQueue *thumb_queue) {
    if (thumb_queue == nullptr)
        return;
    xd_thumb_queue_cancel(thumb_queue);
    if (thumb_queue->pump != 0)
        g_source_remove(thumb_queue->pump);
    g_hash_table_unref(thumb_queue->pending);
    g_hash_table_unref(thumb_queue->running);
    g_free(thumb_queue);
}
