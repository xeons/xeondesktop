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

static void request_free(gpointer p) {
    request *r = p;
    g_free(r->uri);
    g_free(r);
}

static void job_free(job *j) {
    g_clear_object(&j->proc);
    g_clear_object(&j->cancel);
    g_free(j->uri);
    g_free(j);
}

XdThumbQueue *xd_thumb_queue_new(XdThumbDone done, gpointer data) {
    XdThumbQueue *q = g_new0(XdThumbQueue, 1);
    q->done = done;
    q->data = data;
    q->pending = g_hash_table_new_full(g_str_hash, g_str_equal, nullptr, request_free);
    q->running = g_hash_table_new(g_str_hash, g_str_equal);
    /* Each decode is one core's work; leave the rest to the user. */
    q->max = (guint)CLAMP(g_get_num_processors() / 2, 1, 4);
    return q;
}

static void schedule(XdThumbQueue *q);

static void job_finished(GObject *source, GAsyncResult *res, gpointer data) {
    job *j = data;
    char *out = nullptr;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, nullptr,
                                                       nullptr);
    XdThumbQueue *q = j->q;
    if (q != nullptr) {
        g_hash_table_remove(q->running, j->uri);
        if (j->timeout != 0)
            g_source_remove(j->timeout);
        const char *path = ok && g_subprocess_get_successful(j->proc) && out != nullptr
                               ? g_strstrip(out)
                               : nullptr;
        q->done(j->uri, path != nullptr && *path != '\0' ? path : nullptr, q->data);
        schedule(q);
    }
    g_free(out);
    job_free(j);
}

static gboolean job_timeout(gpointer data) {
    job *j = data;
    j->timeout = 0;
    g_subprocess_force_exit(j->proc);
    return G_SOURCE_REMOVE;
}

static void start(XdThumbQueue *q, request *r) {
    char *size = g_strconcat("--size=", xd_thumb_size_name(r->size), nullptr);
    const char *argv[] = {"xd-thumbnailer", size, r->uri, nullptr};
    GSubprocess *proc = g_subprocess_newv(
        argv, (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
        nullptr);
    g_free(size);
    if (proc == nullptr) {
        q->done(r->uri, nullptr, q->data);
        return;
    }
    job *j = g_new0(job, 1);
    j->q = q;
    j->uri = g_strdup(r->uri);
    j->proc = proc;
    j->cancel = g_cancellable_new();
    j->timeout = g_timeout_add_seconds(TIMEOUT_SECONDS, job_timeout, j);
    g_hash_table_insert(q->running, j->uri, j);
    g_subprocess_communicate_utf8_async(proc, nullptr, j->cancel, job_finished, j);
}

/* The next request: from the latest batch, the first asked. */
static request *next(XdThumbQueue *q) {
    request *best = nullptr;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, q->pending);
    while (g_hash_table_iter_next(&iter, nullptr, &value)) {
        request *r = value;
        if (best == nullptr || r->batch > best->batch ||
            (r->batch == best->batch && r->order < best->order))
            best = r;
    }
    return best;
}

static gboolean pump(gpointer data) {
    XdThumbQueue *q = data;
    q->pump = 0;
    q->batch++; /* what's asked for from now on is newer */
    while (g_hash_table_size(q->running) < q->max) {
        request *r = next(q);
        if (r == nullptr)
            break;
        g_hash_table_steal(q->pending, r->uri);
        start(q, r);
        request_free(r);
    }
    return G_SOURCE_REMOVE;
}

/* After the current main loop iteration: a view's whole redraw is one
 * batch. */
static void schedule(XdThumbQueue *q) {
    if (q->pump == 0)
        q->pump = g_idle_add(pump, q);
}

void xd_thumb_queue_request(XdThumbQueue *q, const char *uri, XdThumbSize size) {
    if (g_hash_table_contains(q->running, uri))
        return;
    request *r = g_hash_table_lookup(q->pending, uri);
    if (r == nullptr) {
        r = g_new0(request, 1);
        r->uri = g_strdup(uri);
        r->size = size;
        r->order = q->order++;
        g_hash_table_insert(q->pending, r->uri, r);
    }
    r->size = MAX(r->size, size);
    r->batch = q->batch;
    schedule(q);
}

void xd_thumb_queue_cancel(XdThumbQueue *q) {
    g_hash_table_remove_all(q->pending);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, q->running);
    while (g_hash_table_iter_next(&iter, nullptr, &value)) {
        job *j = value;
        j->q = nullptr;
        if (j->timeout != 0)
            g_source_remove(j->timeout);
        j->timeout = 0;
        g_cancellable_cancel(j->cancel);
        /* xd-thumbnailer's sandbox dies with it (bwrap --die-with-parent). */
        g_subprocess_force_exit(j->proc);
    }
    g_hash_table_remove_all(q->running);
}

void xd_thumb_queue_free(XdThumbQueue *q) {
    if (q == nullptr)
        return;
    xd_thumb_queue_cancel(q);
    if (q->pump != 0)
        g_source_remove(q->pump);
    g_hash_table_unref(q->pending);
    g_hash_table_unref(q->running);
    g_free(q);
}
