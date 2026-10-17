/* SPDX-License-Identifier: MIT */
#include <canberra.h>
#include <glib.h>
#include <stdio.h>

#include "soundplay.h"

/* One worker, so sounds play in order and only it touches the context. */
static GThreadPool *pool;
static ca_context *player; /* the worker's */
static char *trace_path;   /* XD_SOUND_TRACE, read on the caller's thread */
static constexpr uint32_t PLAY_ID = 1;

/* xd_sound_play_event_wait's: the caller and the sound's end both hold
 * it, as the caller may stop waiting first. */
typedef struct waiter {
    GMutex lock;
    GCond cond;
    bool finished;
    int refs;
} waiter;

static void waiter_unref(waiter *w) {
    g_mutex_lock(&w->lock);
    bool last = --w->refs == 0;
    g_mutex_unlock(&w->lock);
    if (last) {
        g_mutex_clear(&w->lock);
        g_cond_clear(&w->cond);
        g_free(w);
    }
}

static void waiter_finish(waiter *w) {
    g_mutex_lock(&w->lock);
    w->finished = true;
    g_cond_signal(&w->cond);
    g_mutex_unlock(&w->lock);
    waiter_unref(w);
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

static void job_free(job *j) {
    g_free(j->sound);
    g_free(j->description);
    g_free(j->error);
    g_free(j);
}

static gboolean report(gpointer data) {
    job *j = data;
    j->done(j->error, j->data);
    job_free(j);
    return G_SOURCE_REMOVE;
}

static int open_player(void) {
    if (player != nullptr)
        return CA_SUCCESS;
    int r = ca_context_create(&player);
    if (r == CA_SUCCESS)
        r = ca_context_change_props(player, CA_PROP_APPLICATION_NAME, "XeonDesktop",
                                    CA_PROP_APPLICATION_ID, "org.xeondesktop", nullptr);
    if (r != CA_SUCCESS && player != nullptr) {
        ca_context_destroy(player);
        player = nullptr;
    }
    return r;
}

static void played([[maybe_unused]] ca_context *c, [[maybe_unused]] uint32_t id,
                   [[maybe_unused]] int error, void *data) {
    waiter_finish(data);
}

/* The tone as a file in the cache, written the first time this process
 * plays it (so a changed tone replaces an old file). */
static char *tone_file(xd_sound_tone t) {
    static bool written[XD_TONE_COUNT];
    char *name = g_strconcat(xd_sound_tone_name(t), ".wav", nullptr);
    char *dir = g_build_filename(g_get_user_cache_dir(), "xeondesktop", "tones", nullptr);
    char *path = g_build_filename(dir, name, nullptr);
    g_free(name);
    if (!written[t]) {
        GBytes *wav = xd_sound_tone_wav(t);
        gsize len = 0;
        const char *data = g_bytes_get_data(wav, &len);
        GError *err = nullptr;
        g_mkdir_with_parents(dir, 0700);
        if (g_file_set_contents(path, data, (gssize)len, &err))
            written[t] = true;
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
static void trace(const job *j) {
    FILE *f = fopen(trace_path, "ae");
    if (f == nullptr)
        return;
    fprintf(f, "%s\t%s\n", j->description, j->sound);
    fclose(f);
}

/* Plays the job's sound; CA_SUCCESS when there was nothing to play. */
static int play(job *j) {
    if (j->kind == JOB_EVENT) {
        xd_sound_scheme s;
        xd_sound_scheme_load(&s);
        j->sound = g_strdup(s.sounds[j->event]);
        j->description = g_strdup(xd_sound_event_get(j->event)->label);
        xd_sound_scheme_free(&s);
    } else if (j->kind == JOB_TONE) {
        j->sound = tone_file(j->tone);
        j->description = g_strdup(xd_sound_tone_name(j->tone));
    }
    if (xd_sound_kind_of(j->sound) == XD_SOUND_NONE)
        return CA_SUCCESS;
    if (trace_path != nullptr) {
        trace(j);
        return CA_SUCCESS;
    }
    int r = open_player();
    if (r != CA_SUCCESS)
        return r;
    ca_context_cancel(player, PLAY_ID);
    ca_proplist *props = nullptr;
    r = ca_proplist_create(&props);
    if (r != CA_SUCCESS)
        return r;
    ca_proplist_sets(props, CA_PROP_EVENT_DESCRIPTION, j->description);
    if (xd_sound_kind_of(j->sound) == XD_SOUND_FILE) {
        ca_proplist_sets(props, CA_PROP_MEDIA_FILENAME, j->sound);
        /* Files aren't cached by the sound server: they may change. */
        ca_proplist_sets(props, CA_PROP_CANBERRA_CACHE_CONTROL, "never");
    } else {
        ca_proplist_sets(props, CA_PROP_EVENT_ID, j->sound);
    }
    r = ca_context_play_full(player, PLAY_ID, props, j->wait != nullptr ? played : nullptr,
                             j->wait);
    ca_proplist_destroy(props);
    if (r == CA_SUCCESS)
        j->wait = nullptr; /* `played` has it */
    return r;
}

static void play_job(gpointer data, [[maybe_unused]] gpointer user) {
    job *j = data;
    int r = play(j);
    if (j->wait != nullptr)
        waiter_finish(j->wait); /* nothing played */
    if (r != CA_SUCCESS)
        j->error = g_strdup(ca_strerror(r));
    if (j->done != nullptr) {
        g_main_context_invoke(nullptr, report, j);
        return;
    }
    /* Events and tones have nobody to tell. */
    if (j->error != nullptr)
        fprintf(stderr, "xeondesktop: cannot play the %s sound (%s): %s\n", j->description,
                j->sound, j->error);
    job_free(j);
}

static void push(job *j) {
    if (pool == nullptr) {
        const char *t = getenv("XD_SOUND_TRACE");
        trace_path = t != nullptr && t[0] == '/' ? g_strdup(t) : nullptr;
        pool = g_thread_pool_new(play_job, nullptr, 1, FALSE, nullptr);
    }
    g_thread_pool_push(pool, j, nullptr);
}

void xd_sound_play(const char *sound, const char *description, xd_sound_done done, void *data) {
    if (xd_sound_kind_of(sound) == XD_SOUND_NONE) {
        if (done != nullptr)
            done(nullptr, data);
        return;
    }
    job *j = g_new0(job, 1);
    j->kind = JOB_SOUND;
    j->sound = g_strdup(sound);
    j->description = g_strdup(description != nullptr ? description : sound);
    j->done = done;
    j->data = data;
    push(j);
}

void xd_sound_play_event(xd_sound_event ev) {
    job *j = g_new0(job, 1);
    j->kind = JOB_EVENT;
    j->event = ev;
    push(j);
}

void xd_sound_play_event_wait(xd_sound_event ev, int timeout_ms) {
    waiter *w = g_new0(waiter, 1);
    g_mutex_init(&w->lock);
    g_cond_init(&w->cond);
    w->refs = 2;
    job *j = g_new0(job, 1);
    j->kind = JOB_EVENT;
    j->event = ev;
    j->wait = w;
    push(j);
    gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * G_TIME_SPAN_MILLISECOND;
    g_mutex_lock(&w->lock);
    while (!w->finished && g_cond_wait_until(&w->cond, &w->lock, end))
        ;
    g_mutex_unlock(&w->lock);
    waiter_unref(w);
}

void xd_sound_play_tone(xd_sound_tone tone) {
    job *j = g_new0(job, 1);
    j->kind = JOB_TONE;
    j->tone = tone;
    push(j);
}

void xd_sound_finish(void) {
    if (pool != nullptr)
        g_thread_pool_free(pool, TRUE, FALSE);
    pool = nullptr;
    g_free(trace_path);
    trace_path = nullptr;
}
