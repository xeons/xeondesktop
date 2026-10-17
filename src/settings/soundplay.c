/* SPDX-License-Identifier: MIT */
#include <canberra.h>
#include <glib.h>

#include "soundplay.h"

/* One worker, so sounds play in order and only it touches the context. */
static GThreadPool *pool;
static ca_context *player; /* the worker's */
static constexpr uint32_t PLAY_ID = 1;

typedef struct job {
    char *sound, *description;
    xd_sound_done done;
    void *data;
    char *error; /* for `done` */
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

static void play_job(gpointer data, [[maybe_unused]] gpointer user) {
    job *j = data;
    int r = open_player();
    if (r == CA_SUCCESS) {
        ca_context_cancel(player, PLAY_ID);
        /* Files aren't cached by the sound server: they may change. */
        r = xd_sound_kind_of(j->sound) == XD_SOUND_FILE
                ? ca_context_play(player, PLAY_ID, CA_PROP_MEDIA_FILENAME, j->sound,
                                  CA_PROP_EVENT_DESCRIPTION, j->description,
                                  CA_PROP_CANBERRA_CACHE_CONTROL, "never", nullptr)
                : ca_context_play(player, PLAY_ID, CA_PROP_EVENT_ID, j->sound,
                                  CA_PROP_EVENT_DESCRIPTION, j->description, nullptr);
    }
    if (j->done == nullptr) {
        job_free(j);
        return;
    }
    if (r != CA_SUCCESS)
        j->error = g_strdup(ca_strerror(r));
    g_main_context_invoke(nullptr, report, j);
}

void xd_sound_play(const char *sound, const char *description, xd_sound_done done, void *data) {
    if (xd_sound_kind_of(sound) == XD_SOUND_NONE) {
        if (done != nullptr)
            done(nullptr, data);
        return;
    }
    if (pool == nullptr)
        pool = g_thread_pool_new(play_job, nullptr, 1, FALSE, nullptr);
    job *j = g_new0(job, 1);
    j->sound = g_strdup(sound);
    j->description = g_strdup(description != nullptr ? description : sound);
    j->done = done;
    j->data = data;
    g_thread_pool_push(pool, j, nullptr);
}

typedef struct event_job {
    char *label, *sound;
} event_job;

static void event_done(const char *error, void *data) {
    event_job *e = data;
    if (error != nullptr)
        fprintf(stderr, "xeondesktop: cannot play the %s sound (%s): %s\n", e->label, e->sound,
                error);
    g_free(e->label);
    g_free(e->sound);
    g_free(e);
}

void xd_sound_play_event(xd_sound_event ev) {
    xd_sound_scheme s;
    xd_sound_scheme_load(&s);
    event_job *e = g_new0(event_job, 1);
    e->label = g_strdup(xd_sound_event_get(ev)->label);
    e->sound = g_strdup(s.sounds[ev]);
    xd_sound_play(s.sounds[ev], e->label, event_done, e);
    xd_sound_scheme_free(&s);
}

void xd_sound_finish(void) {
    if (pool != nullptr)
        g_thread_pool_free(pool, TRUE, FALSE);
    pool = nullptr;
}
