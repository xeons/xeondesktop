/* SPDX-License-Identifier: MIT */
#include <canberra.h>
#include <glib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "soundplay.h"

/* One worker, so sounds play in order and only it touches the context. */
static GThreadPool *pool;
static ca_context *player; /* the worker's */
static char *trace_path;   /* XD_SOUND_TRACE, read on the caller's thread */
static constexpr uint32_t PLAY_ID = 1;
static atomic_int notify_fd = -1; /* xd_sound_notify_fd's */

/* xd_sound_play_event_wait's: the caller and the sound's end both hold
 * it, as the caller may stop waiting first. */
typedef struct waiter {
    GMutex lock;
    GCond cond;
    bool finished;
    int refs;
} waiter;

static void waiter_unref(waiter *waiter_state) {
    g_mutex_lock(&waiter_state->lock);
    bool last = --waiter_state->refs == 0;
    g_mutex_unlock(&waiter_state->lock);
    if (last) {
        g_mutex_clear(&waiter_state->lock);
        g_cond_clear(&waiter_state->cond);
        g_free(waiter_state);
    }
}

static void waiter_finish(waiter *waiter_state) {
    g_mutex_lock(&waiter_state->lock);
    waiter_state->finished = true;
    g_cond_signal(&waiter_state->cond);
    g_mutex_unlock(&waiter_state->lock);
    waiter_unref(waiter_state);
}

typedef enum job_kind : uint8_t { JOB_SOUND, JOB_EVENT, JOB_TONE } job_kind;

typedef struct job {
    job_kind kind;
    xd_sound_event event; /* JOB_EVENT */
    xd_sound_tone tone;   /* JOB_TONE */
    char *sound, *description;
    xd_sound_done done;
    void *data;
    char *error;   /* for `done` */
    waiter *wait;  /* told when the sound ends; may be nullptr */
} job;

static void job_free(job *sound_job) {
    g_free(sound_job->sound);
    g_free(sound_job->description);
    g_free(sound_job->error);
    g_free(sound_job);
}

static gboolean report(gpointer data) {
    job *sound_job = data;
    sound_job->done(sound_job->error, sound_job->data);
    job_free(sound_job);
    return G_SOURCE_REMOVE;
}

static int open_player(void) {
    if (player != nullptr)
        return CA_SUCCESS;
    int status = ca_context_create(&player);
    if (status == CA_SUCCESS)
        status = ca_context_change_props(player, CA_PROP_APPLICATION_NAME, "XeonDesktop",
                                         CA_PROP_APPLICATION_ID, "org.xeondesktop", nullptr);
    if (status != CA_SUCCESS && player != nullptr) {
        ca_context_destroy(player);
        player = nullptr;
    }
    return status;
}

static void played([[maybe_unused]] ca_context *context, [[maybe_unused]] uint32_t id,
                   [[maybe_unused]] int error, void *data) {
    waiter_finish(data);
}

/* The tone as a file in the cache, written the first time this process
 * plays it (so a changed tone replaces an old file). */
static char *tone_file(xd_sound_tone sound_tone) {
    static bool written[XD_TONE_COUNT];
    char *name = g_strconcat(xd_sound_tone_name(sound_tone), ".wav", nullptr);
    char *dir = g_build_filename(g_get_user_cache_dir(), "xeondesktop", "tones", nullptr);
    char *path = g_build_filename(dir, name, nullptr);
    g_free(name);
    if (!written[sound_tone]) {
        GBytes *wav = xd_sound_tone_wav(sound_tone);
        gsize len = 0;
        const char *data = g_bytes_get_data(wav, &len);
        GError *err = nullptr;
        g_mkdir_with_parents(dir, 0700);
        if (g_file_set_contents(path, data, (gssize)len, &err))
            written[sound_tone] = true;
        else
            fprintf(stderr, "xeondesktop: cannot write %s: %s\n", path, err->message);
        g_clear_error(&err);
        g_bytes_unref(wav);
    }
    g_free(dir);
    return path;
}

/* XD_SOUND_TRACE=FILE: what would play is written to FILE instead, one
 * line a sound ("Minimize\twindow-minimized"), for tests. */
static void trace(const job *sound_job) {
    FILE *file = fopen(trace_path, "ae");
    if (file == nullptr)
        return;
    fprintf(file, "%s\t%s\n", sound_job->description, sound_job->sound);
    fclose(file);
}

/* Plays the job's sound; CA_SUCCESS when there was nothing to play. */
static int play(job *sound_job) {
    if (sound_job->kind == JOB_EVENT) {
        xd_sound_scheme sound_scheme;
        xd_sound_scheme_load(&sound_scheme);
        sound_job->sound = g_strdup(sound_scheme.sounds[sound_job->event]);
        sound_job->description = g_strdup(xd_sound_event_get(sound_job->event)->label);
        xd_sound_scheme_free(&sound_scheme);
    } else if (sound_job->kind == JOB_TONE) {
        sound_job->sound = tone_file(sound_job->tone);
        sound_job->description = g_strdup(xd_sound_tone_name(sound_job->tone));
    }
    if (xd_sound_kind_of(sound_job->sound) == XD_SOUND_NONE)
        return CA_SUCCESS;
    int fd = atomic_load(&notify_fd);
    if (sound_job->kind == JOB_EVENT && fd >= 0) {
        uint64_t one = 1;
        /* Failing (closed, or full) only loses a flash. */
        [[maybe_unused]] ssize_t written = write(fd, &one, sizeof(one));
    }
    if (trace_path != nullptr) {
        trace(sound_job);
        return CA_SUCCESS;
    }
    int status = open_player();
    if (status != CA_SUCCESS)
        return status;
    ca_context_cancel(player, PLAY_ID);
    ca_proplist *props = nullptr;
    status = ca_proplist_create(&props);
    if (status != CA_SUCCESS)
        return status;
    ca_proplist_sets(props, CA_PROP_EVENT_DESCRIPTION, sound_job->description);
    if (xd_sound_kind_of(sound_job->sound) == XD_SOUND_FILE) {
        ca_proplist_sets(props, CA_PROP_MEDIA_FILENAME, sound_job->sound);
        /* Files aren't cached by the sound server: they may change. */
        ca_proplist_sets(props, CA_PROP_CANBERRA_CACHE_CONTROL, "never");
    } else {
        ca_proplist_sets(props, CA_PROP_EVENT_ID, sound_job->sound);
    }
    status = ca_context_play_full(player, PLAY_ID, props,
                                  sound_job->wait != nullptr ? played : nullptr, sound_job->wait);
    ca_proplist_destroy(props);
    if (status == CA_SUCCESS)
        sound_job->wait = nullptr; /* `played` has it */
    return status;
}

static void play_job(gpointer data, [[maybe_unused]] gpointer user) {
    job *sound_job = data;
    int status = play(sound_job);
    if (sound_job->wait != nullptr)
        waiter_finish(sound_job->wait); /* nothing played */
    if (status != CA_SUCCESS)
        sound_job->error = g_strdup(ca_strerror(status));
    if (sound_job->done != nullptr) {
        g_main_context_invoke(nullptr, report, sound_job);
        return;
    }
    /* Events and tones have nobody to tell. */
    if (sound_job->error != nullptr)
        fprintf(stderr, "xeondesktop: cannot play the %s sound (%s): %s\n", sound_job->description,
                sound_job->sound, sound_job->error);
    job_free(sound_job);
}

static void push(job *sound_job) {
    if (pool == nullptr) {
        const char *trace_env = getenv("XD_SOUND_TRACE");
        trace_path = trace_env != nullptr && trace_env[0] == '/' ? g_strdup(trace_env) : nullptr;
        pool = g_thread_pool_new(play_job, nullptr, 1, FALSE, nullptr);
    }
    g_thread_pool_push(pool, sound_job, nullptr);
}

void xd_sound_play(const char *sound, const char *description, xd_sound_done done, void *data) {
    if (xd_sound_kind_of(sound) == XD_SOUND_NONE) {
        if (done != nullptr)
            done(nullptr, data);
        return;
    }
    job *sound_job = g_new0(job, 1);
    sound_job->kind = JOB_SOUND;
    sound_job->sound = g_strdup(sound);
    sound_job->description = g_strdup(description != nullptr ? description : sound);
    sound_job->done = done;
    sound_job->data = data;
    push(sound_job);
}

void xd_sound_play_event(xd_sound_event ev) {
    job *sound_job = g_new0(job, 1);
    sound_job->kind = JOB_EVENT;
    sound_job->event = ev;
    push(sound_job);
}

void xd_sound_play_event_wait(xd_sound_event ev, int timeout_ms) {
    waiter *waiter_state = g_new0(waiter, 1);
    g_mutex_init(&waiter_state->lock);
    g_cond_init(&waiter_state->cond);
    waiter_state->refs = 2;
    job *sound_job = g_new0(job, 1);
    sound_job->kind = JOB_EVENT;
    sound_job->event = ev;
    sound_job->wait = waiter_state;
    push(sound_job);
    gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * G_TIME_SPAN_MILLISECOND;
    g_mutex_lock(&waiter_state->lock);
    while (!waiter_state->finished &&
           g_cond_wait_until(&waiter_state->cond, &waiter_state->lock, end))
        ;
    g_mutex_unlock(&waiter_state->lock);
    waiter_unref(waiter_state);
}

void xd_sound_play_tone(xd_sound_tone tone) {
    job *sound_job = g_new0(job, 1);
    sound_job->kind = JOB_TONE;
    sound_job->tone = tone;
    push(sound_job);
}

void xd_sound_notify_fd(int fd) { atomic_store(&notify_fd, fd); }

void xd_sound_finish(void) {
    if (pool != nullptr)
        g_thread_pool_free(pool, TRUE, FALSE);
    pool = nullptr;
    g_free(trace_path);
    trace_path = nullptr;
}
