/* SPDX-License-Identifier: MIT */
#include "engine.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <linux/magic.h>
#include <liburing.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include "../core/common.h"

#define WORK_DIRECTORY_NAME ".xd-diskbench"
#define FILE_PREFIX "xd-diskbench-"
#define FILE_SUFFIX ".tmp"

/* O_DIRECT wants buffers aligned to the logical block size; a page is
 * enough for every disk. */
static constexpr size_t BUFFER_ALIGNMENT = 4096;
static constexpr uint64_t FILL_BLOCK = 1024 * 1024;
/* Room the test file leaves free, so a test never fills the disk. */
static constexpr uint64_t FREE_SPACE_MARGIN = 64 * 1024 * 1024;
static constexpr long long WAIT_SLICE_NS = 100 * 1000 * 1000;

struct xd_bench_engine {
    xd_bench_plan plan;
    char *directory;
    xd_bench_event_fn callback;
    void *user_data;
    GMainContext *context;
    GThread *thread;
    atomic_bool cancelled;
    GMutex mutex;
    GCond cond;
    /* The worker's, until it ends. */
    char *file_path;
    int file;
    bool cached;
};

static uint64_t get_monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static bool is_cancelled(xd_bench_engine *engine) {
    return atomic_load_explicit(&engine->cancelled, memory_order_relaxed);
}

/* ---- events ------------------------------------------------------------------------ */

typedef struct posted_event {
    xd_bench_engine *engine;
    xd_bench_event event;
    char *error;
} posted_event;

static gboolean deliver_event(gpointer data) {
    posted_event *posted = data;
    posted->event.error = posted->error;
    posted->engine->callback(&posted->event, posted->engine->user_data);
    return G_SOURCE_REMOVE;
}

static void free_posted_event(gpointer data) {
    posted_event *posted = data;
    g_free(posted->error);
    g_free(posted);
}

static void post_event(xd_bench_engine *engine, const xd_bench_event *event, const char *error) {
    posted_event *posted = g_new0(posted_event, 1);
    posted->engine = engine;
    posted->event = *event;
    posted->event.cached = engine->cached;
    posted->error = g_strdup(error);
    g_main_context_invoke_full(engine->context, G_PRIORITY_DEFAULT, deliver_event, posted,
                               free_posted_event);
}

/* Waits `ms`, or until stopped. */
static void wait_cancellable(xd_bench_engine *engine, unsigned ms) {
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    g_mutex_lock(&engine->mutex);
    while (!is_cancelled(engine))
        if (!g_cond_wait_until(&engine->cond, &engine->mutex, end))
            break;
    g_mutex_unlock(&engine->mutex);
}

/* ---- the test file ----------------------------------------------------------------- */

char *xd_bench_get_work_directory(const char *directory) {
    return g_build_filename(directory, WORK_DIRECTORY_NAME, nullptr);
}

/* The process a test file was made by, from its name, or 0. */
static pid_t get_file_owner_pid(const char *name) {
    if (!g_str_has_prefix(name, FILE_PREFIX) || !g_str_has_suffix(name, FILE_SUFFIX))
        return 0;
    const char *digits = name + strlen(FILE_PREFIX);
    char *end = nullptr;
    gint64 pid = g_ascii_strtoll(digits, &end, 10);
    if (end == digits || *end != '-' || pid <= 0 || pid > INT32_MAX)
        return 0;
    return (pid_t)pid;
}

size_t xd_bench_remove_leftovers(const char *work_directory) {
    GDir *dir = g_dir_open(work_directory, 0, nullptr);
    if (dir == nullptr)
        return 0;
    size_t removed = 0;
    const char *name;
    while ((name = g_dir_read_name(dir)) != nullptr) {
        pid_t pid = get_file_owner_pid(name);
        if (pid == 0 || pid == getpid())
            continue;
        /* EPERM: it's running, as someone else. */
        if (kill(pid, 0) == 0 || errno != ESRCH)
            continue;
        g_autofree char *path = g_build_filename(work_directory, name, nullptr);
        if (g_unlink(path) == 0)
            removed++;
    }
    g_dir_close(dir);
    return removed;
}

static void fill_test_data(uint8_t *buffer, size_t size, bool random_data, uint64_t seed) {
    if (!random_data) {
        memset(buffer, 0, size);
        return;
    }
    uint64_t state = seed;
    for (size_t i = 0; i + sizeof(uint64_t) <= size; i += sizeof(uint64_t)) {
        uint64_t value = xd_bench_get_next_random(&state);
        memcpy(buffer + i, &value, sizeof(value));
    }
}

static char *describe_errno(const char *what, int error) {
    return g_strdup_printf("%s: %s", what, g_strerror(error));
}

/* Opens the test file buffered, after the file system refused O_DIRECT. */
static bool reopen_buffered(xd_bench_engine *engine, char **error) {
    close(engine->file);
    engine->file = open(engine->file_path, O_RDWR | O_CLOEXEC);
    if (engine->file < 0) {
        *error = describe_errno("The test file can't be opened", errno);
        return false;
    }
    engine->cached = true;
    return true;
}

static bool create_test_file(xd_bench_engine *engine, const char *work_directory, char **error) {
    struct statvfs fs;
    if (statvfs(work_directory, &fs) == 0) {
        uint64_t free_bytes = (uint64_t)fs.f_bavail * fs.f_frsize;
        if (free_bytes < engine->plan.file_size + FREE_SPACE_MARGIN) {
            g_autofree char *need = xd_bench_format_size(engine->plan.file_size);
            g_autofree char *have = xd_bench_format_size(free_bytes);
            *error = g_strdup_printf("There isn't enough free space for the test file: it needs "
                                     "%s and 64 MiB to spare, and %s is free.",
                                     need, have);
            return false;
        }
    }
    engine->file_path = g_strdup_printf("%s/" FILE_PREFIX "%d-%08x" FILE_SUFFIX, work_directory,
                                        (int)getpid(), g_random_int());
    int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
    /* tmpfs takes O_DIRECT since Linux 6.6, but its files are memory all
     * the same: the results are the page cache's. */
    struct statfs memory_fs;
    if (statfs(work_directory, &memory_fs) == 0 &&
        (memory_fs.f_type == TMPFS_MAGIC || memory_fs.f_type == RAMFS_MAGIC))
        engine->cached = true;
    engine->file = open(engine->file_path, flags | (engine->cached ? 0 : O_DIRECT), 0600);
    if (engine->file < 0 && errno == EINVAL) {
        engine->cached = true;
        engine->file = open(engine->file_path, flags, 0600);
    }
    if (engine->file < 0) {
        *error = describe_errno("The test file can't be made", errno);
        g_clear_pointer(&engine->file_path, g_free);
        return false;
    }
    if (fallocate(engine->file, 0, 0, (off_t)engine->plan.file_size) != 0 &&
        errno != EOPNOTSUPP) {
        *error = describe_errno("The test file can't be made", errno);
        return false;
    }

    /* Filled before the read tests, so they read data rather than holes
     * (which some file systems answer without touching the disk). */
    uint64_t block = XD_MIN(FILL_BLOCK, engine->plan.file_size);
    void *memory = nullptr;
    if (posix_memalign(&memory, BUFFER_ALIGNMENT, block) != 0)
        XD_PANIC("out of memory");
    uint8_t *buffer = memory;
    bool ok = true;
    uint64_t seed = 1;
    xd_bench_event event = {.kind = XD_BENCH_EVENT_PREPARING};
    double last_posted = -1;
    for (uint64_t offset = 0; offset < engine->plan.file_size && ok; offset += block) {
        if (is_cancelled(engine))
            break;
        fill_test_data(buffer, block, engine->plan.random_data, seed++);
        ssize_t written = pwrite(engine->file, buffer, block, (off_t)offset);
        if (written < 0 && errno == EINVAL && !engine->cached) {
            /* Some FUSE file systems open with O_DIRECT, then refuse it. */
            ok = reopen_buffered(engine, error);
            offset -= block;
            continue;
        }
        if (written != (ssize_t)block) {
            *error = describe_errno("The test file can't be written", written < 0 ? errno : ENOSPC);
            ok = false;
        }
        event.fraction = (double)(offset + block) / (double)engine->plan.file_size;
        if (event.fraction - last_posted >= 0.02) {
            post_event(engine, &event, nullptr);
            last_posted = event.fraction;
        }
    }
    free(buffer);
    if (ok && fdatasync(engine->file) != 0) {
        *error = describe_errno("The test file can't be written", errno);
        ok = false;
    }
    return ok;
}

/* Closes and deletes the test file, and the hidden folder if it's empty. */
static void remove_test_file(xd_bench_engine *engine, const char *work_directory) {
    if (engine->file >= 0)
        close(engine->file);
    engine->file = -1;
    if (engine->file_path != nullptr)
        g_unlink(engine->file_path);
    g_clear_pointer(&engine->file_path, g_free);
    g_rmdir(work_directory);
}

/* ---- measuring --------------------------------------------------------------------- */

/* One thread's share of a run: its own ring, queue_depth operations in
 * flight, until the deadline. */
typedef struct thread_job {
    xd_bench_engine *engine;
    const xd_bench_test *test;
    xd_bench_direction direction;
    xd_bench_offsets offsets;
    uint64_t random_state;
    uint64_t deadline_ns;
    xd_bench_measure reads, writes;
    int error; /* an errno, or 0 */
} thread_job;

typedef struct slot {
    uint64_t submitted_ns;
    bool is_read;
} slot;

static bool prepare_operation(thread_job *job, struct io_uring *ring, uint8_t *buffers,
                              slot *slots, unsigned index) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr)
        return false;
    uint32_t block = job->test->block_size;
    uint8_t *buffer = buffers + (size_t)index * block;
    uint64_t offset = xd_bench_get_next_offset(&job->offsets);
    bool is_read = job->direction == XD_BENCH_READ ||
                   (job->direction == XD_BENCH_MIXED &&
                    xd_bench_choose_read(&job->random_state, job->test->read_percent));
    if (is_read) {
        io_uring_prep_read(sqe, job->engine->file, buffer, block, offset);
    } else {
        /* A new stamp at the front of each block, so a drive that
         * deduplicates or compresses can't skip writing it. */
        if (job->engine->plan.random_data) {
            uint64_t stamp = xd_bench_get_next_random(&job->random_state);
            memcpy(buffer, &stamp, sizeof(stamp));
        }
        io_uring_prep_write(sqe, job->engine->file, buffer, block, offset);
    }
    io_uring_sqe_set_data64(sqe, index);
    slots[index] = (slot){.submitted_ns = get_monotonic_ns(), .is_read = is_read};
    return true;
}

static void run_thread_job(thread_job *job) {
    unsigned depth = job->test->queue_depth;
    struct io_uring ring;
    int status = io_uring_queue_init(depth, &ring, 0);
    if (status < 0) {
        job->error = -status;
        return;
    }
    size_t buffer_bytes = (size_t)depth * job->test->block_size;
    void *memory = nullptr;
    if (posix_memalign(&memory, BUFFER_ALIGNMENT, buffer_bytes) != 0)
        XD_PANIC("out of memory");
    uint8_t *buffers = memory;
    fill_test_data(buffers, buffer_bytes, job->engine->plan.random_data, job->random_state);
    slot *slots = g_new0(slot, depth);

    unsigned in_flight = 0;
    for (unsigned i = 0; i < depth; i++)
        if (prepare_operation(job, &ring, buffers, slots, i))
            in_flight++;
    io_uring_submit(&ring);
    bool stopping = false;
    unsigned ring_failures = 0;
    while (in_flight > 0) {
        struct io_uring_cqe *cqe = nullptr;
        struct __kernel_timespec timeout = {.tv_sec = 0, .tv_nsec = WAIT_SLICE_NS};
        status = io_uring_wait_cqe_timeout(&ring, &cqe, &timeout);
        uint64_t now = get_monotonic_ns();
        stopping = stopping || now >= job->deadline_ns || is_cancelled(job->engine);
        if (status == -ETIME || status == -EINTR)
            continue;
        if (status < 0) {
            /* The ring itself failed: stop, and wait for what's in flight,
             * which still writes into the buffers. */
            if (job->error == 0)
                job->error = -status;
            stopping = true;
            if (++ring_failures < 50)
                continue;
            /* It won't say: the buffers stay allocated rather than risk
             * the kernel writing into freed memory. */
            io_uring_queue_exit(&ring);
            g_free(slots);
            return;
        }
        ring_failures = 0;
        unsigned resubmit = 0;
        unsigned head;
        unsigned seen = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            seen++;
            in_flight--;
            unsigned index = (unsigned)io_uring_cqe_get_data64(cqe);
            if (cqe->res < 0) {
                if (job->error == 0)
                    job->error = -cqe->res;
                stopping = true;
                continue;
            }
            xd_bench_measure *measure = slots[index].is_read ? &job->reads : &job->writes;
            measure->bytes += (uint64_t)cqe->res;
            measure->operations++;
            measure->latency_ns += now - slots[index].submitted_ns;
            if (!stopping && prepare_operation(job, &ring, buffers, slots, index)) {
                in_flight++;
                resubmit++;
            }
        }
        io_uring_cq_advance(&ring, seen);
        if (resubmit > 0)
            io_uring_submit(&ring);
    }
    io_uring_queue_exit(&ring);
    g_free(slots);
    free(buffers);
}

static gpointer run_thread_job_thread(gpointer data) {
    run_thread_job(data);
    return nullptr;
}

/* One run of a step: every thread for measure_ms. */
static bool measure_run(xd_bench_engine *engine, const xd_bench_step *step, unsigned run,
                        xd_bench_measure *reads, xd_bench_measure *writes, char **error) {
    const xd_bench_test *test = &step->test;
    uint64_t block_count = engine->plan.file_size / test->block_size;
    if (block_count == 0) {
        *error = g_strdup("The test file is smaller than one block.");
        return false;
    }
    if (engine->cached && step->direction != XD_BENCH_WRITE)
        posix_fadvise(engine->file, 0, 0, POSIX_FADV_DONTNEED);
    unsigned threads = test->threads;
    thread_job *jobs = g_new0(thread_job, threads);
    /* Sequential threads each get a slice of the file; random ones roam
     * all of it, each with its own seed. */
    uint64_t slice = block_count / threads;
    uint64_t start = get_monotonic_ns();
    for (unsigned i = 0; i < threads; i++) {
        thread_job *job = &jobs[i];
        job->engine = engine;
        job->test = test;
        job->direction = step->direction;
        job->random_state = ((uint64_t)run << 32) ^ ((uint64_t)step->row << 16) ^ i;
        job->deadline_ns = start + (uint64_t)engine->plan.measure_ms * 1000000u;
        bool sliced = test->pattern == XD_BENCH_SEQUENTIAL && slice > 0;
        xd_bench_init_offsets(&job->offsets, test->pattern, test->block_size,
                              sliced ? slice * i : 0, sliced ? slice : block_count,
                              job->random_state + 1);
    }
    GThread **workers = g_new0(GThread *, threads);
    for (unsigned i = 1; i < threads; i++)
        workers[i] = g_thread_new("xd-diskbench-io", run_thread_job_thread, &jobs[i]);
    run_thread_job(&jobs[0]);
    for (unsigned i = 1; i < threads; i++)
        g_thread_join(workers[i]);
    g_free(workers);
    /* Buffered writes count only once they're on the disk. */
    if (engine->cached && step->direction != XD_BENCH_READ)
        fdatasync(engine->file);
    uint64_t elapsed = get_monotonic_ns() - start;

    *reads = (xd_bench_measure){.elapsed_ns = elapsed};
    *writes = (xd_bench_measure){.elapsed_ns = elapsed};
    int failure = 0;
    for (unsigned i = 0; i < threads; i++) {
        reads->bytes += jobs[i].reads.bytes;
        reads->operations += jobs[i].reads.operations;
        reads->latency_ns += jobs[i].reads.latency_ns;
        writes->bytes += jobs[i].writes.bytes;
        writes->operations += jobs[i].writes.operations;
        writes->latency_ns += jobs[i].writes.latency_ns;
        if (failure == 0)
            failure = jobs[i].error;
    }
    g_free(jobs);
    if (failure == EPERM || failure == ENOSYS) {
        *error = g_strdup_printf("The test can't run: this system doesn't allow io_uring (%s).",
                                 g_strerror(failure));
        return false;
    }
    if (failure != 0) {
        *error = describe_errno(step->direction == XD_BENCH_READ ? "Reading the test file failed"
                                                                 : "Writing the test file failed",
                                failure);
        return false;
    }
    return true;
}

static void run_plan(xd_bench_engine *engine, char **error) {
    g_autofree char *work_directory = xd_bench_get_work_directory(engine->directory);
    if (g_mkdir(work_directory, 0700) != 0 && errno != EEXIST) {
        *error = describe_errno("The folder for the test file can't be made", errno);
        return;
    }
    xd_bench_remove_leftovers(work_directory);
    bool ok = create_test_file(engine, work_directory, error);
    for (size_t index = 0; ok && index < engine->plan.step_count; index++) {
        const xd_bench_step *step = &engine->plan.steps[index];
        if (index > 0 && engine->plan.interval_ms > 0 && !is_cancelled(engine)) {
            xd_bench_event waiting = {.kind = XD_BENCH_EVENT_WAITING, .step_index = index};
            post_event(engine, &waiting, nullptr);
            wait_cancellable(engine, engine->plan.interval_ms);
        }
        xd_bench_event result = {.kind = XD_BENCH_EVENT_RESULT, .step_index = index};
        for (unsigned run = 1; ok && run <= engine->plan.runs && !is_cancelled(engine); run++) {
            xd_bench_event running = {
                .kind = XD_BENCH_EVENT_RUNNING, .step_index = index, .run = run};
            post_event(engine, &running, nullptr);
            xd_bench_measure reads, writes;
            ok = measure_run(engine, step, run, &reads, &writes, error);
            if (!ok || is_cancelled(engine))
                break;
            /* A mixed row's best run is the one that moved most in all. */
            xd_bench_measure total = reads, best_total = result.read_result;
            total.bytes += writes.bytes;
            total.operations += writes.operations;
            best_total.bytes += result.write_result.bytes;
            best_total.operations += result.write_result.operations;
            if (xd_bench_is_better(&total, &best_total)) {
                result.read_result = reads;
                result.write_result = writes;
                post_event(engine, &result, nullptr);
            }
        }
        if (is_cancelled(engine))
            break;
    }
    remove_test_file(engine, work_directory);
}

static gpointer run_worker(gpointer data) {
    xd_bench_engine *engine = data;
    char *error = nullptr;
    run_plan(engine, &error);
    xd_bench_event finished = {.kind = XD_BENCH_EVENT_FINISHED,
                               .cancelled = is_cancelled(engine) && error == nullptr};
    post_event(engine, &finished, error);
    g_free(error);
    return nullptr;
}

xd_bench_engine *xd_bench_start(const xd_bench_plan *plan, xd_bench_event_fn callback,
                                void *user_data) {
    XD_ASSERT(plan->step_count <= XD_BENCH_MAX_STEPS && plan->runs > 0);
    xd_bench_engine *engine = g_new0(xd_bench_engine, 1);
    engine->plan = *plan;
    engine->directory = g_strdup(plan->directory);
    engine->plan.directory = engine->directory;
    engine->callback = callback;
    engine->user_data = user_data;
    engine->context = g_main_context_ref_thread_default();
    engine->file = -1;
    atomic_init(&engine->cancelled, false);
    g_mutex_init(&engine->mutex);
    g_cond_init(&engine->cond);
    engine->thread = g_thread_new("xd-diskbench", run_worker, engine);
    return engine;
}

void xd_bench_stop(xd_bench_engine *engine) {
    g_mutex_lock(&engine->mutex);
    atomic_store(&engine->cancelled, true);
    g_cond_broadcast(&engine->cond);
    g_mutex_unlock(&engine->mutex);
}

void xd_bench_free_engine(xd_bench_engine *engine) {
    if (engine == nullptr)
        return;
    g_thread_join(engine->thread);
    g_mutex_clear(&engine->mutex);
    g_cond_clear(&engine->cond);
    g_main_context_unref(engine->context);
    g_free(engine->directory);
    g_free(engine);
}
