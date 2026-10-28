/* SPDX-License-Identifier: MIT
 * xd-diskbench's engine: runs a plan of tests on a worker thread with
 * io_uring, on a test file of its own in a hidden folder of the chosen
 * directory, and tells the main loop how it's going. No GTK, so it can be
 * driven and tested on its own.
 *
 * The file is opened with O_DIRECT so the page cache doesn't flatter the
 * results; where the file system refuses that (tmpfs, some FUSE), it's
 * buffered I/O with fdatasync after writes, and the results say
 * "cached". It's preallocated and filled before the first test, has a
 * new name each time, and is deleted afterwards, whether the plan
 * finished, was stopped or failed; a leftover from a crash goes the next
 * time a plan runs there.
 */
#ifndef XD_DISKBENCH_ENGINE_H
#define XD_DISKBENCH_ENGINE_H

#include <glib.h>

#include "benchlib.h"

typedef enum xd_bench_direction : uint8_t {
    XD_BENCH_READ,
    XD_BENCH_WRITE,
    XD_BENCH_MIXED,
} xd_bench_direction;

typedef struct xd_bench_step {
    size_t row;
    xd_bench_direction direction;
    xd_bench_test test;
} xd_bench_step;

#define XD_BENCH_MAX_STEPS (XD_BENCH_MAX_ROWS * 2)

typedef struct xd_bench_plan {
    const char *directory; /* where the hidden folder for the test file goes */
    uint64_t file_size;
    unsigned runs;
    unsigned measure_ms;  /* how long each run measures */
    unsigned interval_ms; /* the pause between tests */
    bool random_data;
    size_t step_count;
    xd_bench_step steps[XD_BENCH_MAX_STEPS];
} xd_bench_plan;

typedef enum xd_bench_event_kind : uint8_t {
    XD_BENCH_EVENT_PREPARING, /* filling the test file: fraction */
    XD_BENCH_EVENT_RUNNING,   /* step_index, run */
    XD_BENCH_EVENT_WAITING,   /* the interval before step_index */
    XD_BENCH_EVENT_RESULT,    /* step_index's best so far */
    XD_BENCH_EVENT_FINISHED,  /* error, or cancelled; the last event */
} xd_bench_event_kind;

typedef struct xd_bench_event {
    xd_bench_event_kind kind;
    size_t step_index;
    unsigned run;
    double fraction;
    bool cached; /* buffered I/O: the file system refused O_DIRECT */
    xd_bench_measure read_result, write_result;
    bool cancelled;
    const char *error; /* nullptr when it went well */
} xd_bench_event;

/* Called in the main context of the thread that started the engine. */
typedef void (*xd_bench_event_fn)(const xd_bench_event *event, void *user_data);

typedef struct xd_bench_engine xd_bench_engine;

/* Starts the plan (copied) on a worker thread. */
[[nodiscard]] xd_bench_engine *xd_bench_start(const xd_bench_plan *plan,
                                              xd_bench_event_fn callback, void *user_data);
/* Asks it to stop as soon as it can; FINISHED follows, cancelled. */
void xd_bench_stop(xd_bench_engine *engine);
/* Waits for the worker and frees it: after FINISHED (from its callback
 * too). */
void xd_bench_free_engine(xd_bench_engine *engine);

/* The hidden folder test files go in. Free with g_free. */
char *xd_bench_get_work_directory(const char *directory);
/* Deletes the test files in `work_directory` left by programs no longer
 * running (a crash, a power cut); returns how many. */
size_t xd_bench_remove_leftovers(const char *work_directory);

#endif
