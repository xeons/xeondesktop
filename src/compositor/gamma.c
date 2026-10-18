/* SPDX-License-Identifier: MIT
 * Each monitor's gamma ramp: its calibration ([ColorProfiles]: the vcgt
 * curves of the ICC profile Color Management made its default), then the
 * night light ([NightLight]), as one LUT the DRM backend loads into the
 * CRTC. It costs no rendering, so it works the same with pixman.
 *
 * Programs that set gamma themselves through wlr-gamma-control (gammastep,
 * wlsunset, redshift) win while they hold it, as on sway and other wlroots
 * compositors: their ramp replaces ours whole (calibration included, as
 * theirs usually carries it), and ours comes back when they let go.
 *
 * The ramps go into the next frame's commit (output.c's frame handler
 * asks xd_gamma_frame), so they never flicker against a frame. A change of
 * settings fades over FADE_MS; the schedule's own changes take
 * XD_NIGHTLIGHT_RAMP_MINUTES, recomputed every TRANSITION_TICK_MS.
 *
 * Outputs without gamma (headless, most nested ones) are left alone.
 * XD_GAMMA_TRACE=FILE logs every ramp decided, applied or not, for tests.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "../settings/nightlight.h"
#include "server.h"
#include "vcgt.h"

static constexpr int FADE_MS = 2000;
static constexpr int FADE_TICK_MS = 50;
/* During the schedule's transitions: 30 minutes from 6500 K to 1700 K
 * moves about 25 K a step, too little to see. */
static constexpr int TRANSITION_TICK_MS = 10000;
/* Otherwise: a look each minute, so a clock change, a resume from sleep
 * or the next transition is never far off. */
static constexpr int IDLE_TICK_MS = 60000;
/* The trace's ramp length on outputs without gamma. */
static constexpr size_t TRACE_RAMP = 256;

struct xd_gamma {
    xd_server *server;
    struct wlr_gamma_control_manager_v1 *manager;
    struct wl_listener set_gamma;
    struct wl_listener manager_destroy;
    struct wl_event_source *timer;
    xd_nightlight_settings night;
    xd_color_profile_rule profiles[XD_COLOR_PROFILES_MAX];
    size_t profile_count;
    bool have_settings;
    double kelvin; /* shown now */
    /* A settings change fading in. */
    bool fading;
    double fade_from;
    struct timespec fade_start;
    FILE *trace;
};

struct xd_gamma_output {
    xd_output *out;
    char profile[1024]; /* the ICC file its calibration came from */
    float *calibration;  /* 3 x calibration_n, or nullptr */
    size_t calibration_n;
    bool dirty;
    bool ramp_set; /* a ramp (ours or a program's) went into a commit */
    struct wl_listener commit;
};

static int64_t ms_since(const struct timespec *t) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)(now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}

static void mark_dirty(xd_output *out) {
    if (out->gamma == nullptr)
        return;
    out->gamma->dirty = true;
    wlr_output_schedule_frame(out->wlr_output);
}

static void mark_all_dirty(xd_gamma *g) {
    XD_LIST_FOR_EACH(out, &g->server->outputs, xd_output, link) {
        mark_dirty(out);
    }
}

/* Loads the output's calibration from its profile in the settings,
 * sampled at its gamma size. */
static void load_calibration(xd_gamma *g, xd_output *out) {
    struct xd_gamma_output *go = out->gamma;
    char key[192];
    xd_output_key(out->wlr_output, key, sizeof(key));
    const char *path = "";
    for (size_t i = 0; i < g->profile_count; i++) {
        if (strcmp(g->profiles[i].monitor, key) == 0)
            path = g->profiles[i].path;
    }
    size_t n = wlr_output_get_gamma_size(out->wlr_output);
    if (n == 0 && g->trace != nullptr)
        n = TRACE_RAMP;
    if (strcmp(go->profile, path) == 0 && (go->calibration == nullptr || go->calibration_n == n))
        return;
    snprintf(go->profile, sizeof(go->profile), "%s", path);
    XD_FREE(&go->calibration);
    go->calibration_n = 0;
    if (path[0] != '\0' && n >= 2) {
        float *curves = xd_alloc_array(3 * n, sizeof(*curves));
        if (xd_vcgt_load(path, n, curves)) {
            go->calibration = curves;
            go->calibration_n = n;
            wlr_log(WLR_INFO, "output %s: calibration from %s", out->wlr_output->name, path);
        } else {
            xd_free(curves);
            wlr_log(WLR_INFO, "output %s: %s has no calibration (vcgt) to load",
                    out->wlr_output->name, path);
        }
    }
    mark_dirty(out);
}

static void trace(xd_gamma *g, xd_output *out, const char *source, const uint16_t *lut,
                  size_t n, bool applied) {
    if (g->trace == nullptr)
        return;
    /* The top of each channel's ramp: what white becomes. */
    fprintf(g->trace, "%s %s %.0f", out->wlr_output->name, source, g->kelvin);
    if (lut != nullptr && n > 0)
        fprintf(g->trace, " r=%.3f g=%.3f b=%.3f", lut[n - 1] / 65535.0,
                lut[2 * n - 1] / 65535.0, lut[3 * n - 1] / 65535.0);
    fprintf(g->trace, " %s\n", applied ? "applied" : "not-applied");
    fflush(g->trace);
}

bool xd_gamma_pending(const xd_output *out) {
    return out->gamma != nullptr && out->gamma->dirty;
}

void xd_gamma_frame(xd_output *out, struct wlr_output_state *state) {
    xd_gamma *g = out->server->gamma;
    struct xd_gamma_output *go = out->gamma;
    if (g == nullptr || go == nullptr || !go->dirty)
        return;
    go->dirty = false;
    struct wlr_output *o = out->wlr_output;
    size_t size = wlr_output_get_gamma_size(o);

    struct wlr_gamma_control_v1 *control = wlr_gamma_control_manager_v1_get_control(g->manager, o);
    if (control != nullptr) {
        /* A program's own ramp. */
        if (!wlr_gamma_control_v1_apply(control, state))
            return;
        bool ok = wlr_output_test_state(o, state);
        if (!ok) {
            /* Its destruction marks the output dirty again, for ours. */
            wlr_gamma_control_v1_send_failed_and_destroy(control);
            wlr_output_state_set_color_transform(state, nullptr);
        }
        go->ramp_set = true;
        trace(g, out, "client", nullptr, 0, ok);
        return;
    }

    bool night = g->kelvin < XD_NIGHTLIGHT_DAY - 0.5;
    if (!night && go->calibration == nullptr) {
        /* Nothing of ours: back to the plain ramp, if one was loaded. */
        if (size > 0 && go->ramp_set) {
            wlr_output_state_set_color_transform(state, nullptr);
            go->ramp_set = false;
        }
        trace(g, out, "none", nullptr, 0, size > 0);
        return;
    }
    size_t n = size > 0 ? size : TRACE_RAMP;
    if (size == 0 && g->trace == nullptr)
        return;
    if (go->calibration != nullptr && go->calibration_n != n)
        return; /* a new mode's gamma size: handle_commit reloads it */
    uint16_t *lut = xd_alloc_array(3 * n, sizeof(*lut));
    double rgb[3];
    xd_blackbody_rgb(g->kelvin, rgb);
    xd_gamma_ramp_fill(lut, lut + n, lut + 2 * n, n, rgb, go->calibration);
    const char *source = !night ? "calibration" : go->calibration ? "calibration+night-light"
                                                                  : "night-light";
    bool applied = false;
    if (size > 0) {
        struct wlr_color_transform *tr =
            wlr_color_transform_init_lut_3x1d(n, lut, lut + n, lut + 2 * n);
        if (tr != nullptr) {
            wlr_output_state_set_color_transform(state, tr);
            wlr_color_transform_unref(tr);
            applied = wlr_output_test_state(o, state);
            go->ramp_set = true;
            if (!applied) {
                wlr_log(WLR_ERROR, "output %s: the gamma ramp was refused", o->name);
                wlr_output_state_set_color_transform(state, nullptr);
            }
        }
    }
    trace(g, out, source, lut, n, applied);
    xd_free(lut);
}

static void update(xd_gamma *g);

static int handle_timer(void *data) {
    update(data);
    return 0;
}

/* Works out the temperature now, shows it if it changed, and sets the
 * timer for the next look. */
static void update(xd_gamma *g) {
    double target = xd_nightlight_kelvin(&g->night, time(nullptr));
    double k = target;
    int next = 0;
    if (g->fading) {
        int64_t ms = ms_since(&g->fade_start);
        double t = (double)ms / FADE_MS;
        if (t >= 1.0) {
            g->fading = false;
        } else {
            /* Smoothstep: no jolt at either end. */
            t = t * t * (3.0 - 2.0 * t);
            k = g->fade_from + (target - g->fade_from) * t;
            next = FADE_TICK_MS;
        }
    }
    if (next == 0 && g->night.enabled && g->night.schedule != XD_NIGHTLIGHT_ALWAYS) {
        bool between = target > g->night.temperature + 0.5 && target < XD_NIGHTLIGHT_DAY - 0.5;
        next = between ? TRANSITION_TICK_MS : IDLE_TICK_MS;
    }
    if (fabs(k - g->kelvin) >= 1.0 || (k != g->kelvin && !g->fading)) {
        g->kelvin = k;
        mark_all_dirty(g);
    }
    wl_event_source_timer_update(g->timer, next);
}

void xd_gamma_apply_settings(xd_server *server, const xd_settings *s) {
    xd_gamma *g = server->gamma;
    if (g == nullptr)
        return;
    bool first = !g->have_settings;
    bool night_changed = memcmp(&g->night, &s->night_light, sizeof(g->night)) != 0;
    g->have_settings = true;
    g->night = s->night_light;
    g->profile_count = s->color_profile_count;
    memcpy(g->profiles, s->color_profiles, sizeof(g->profiles));
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (out->gamma != nullptr)
            load_calibration(g, out);
    }
    if (night_changed && !first) {
        /* Turned on or off, or the strength moved: fade, as Windows 10's
         * night light does. */
        g->fading = true;
        g->fade_from = g->kelvin;
        clock_gettime(CLOCK_MONOTONIC, &g->fade_start);
    }
    update(g);
}

static void handle_commit(struct wl_listener *listener, void *data) {
    struct xd_gamma_output *go = XD_CONTAINER_OF(listener, struct xd_gamma_output, commit);
    const struct wlr_output_event_commit *e = data;
    /* Turned back on (a mode set or the monitor woken): the CRTC may come
     * back without our ramp. A new mode may change the gamma size. */
    if ((e->state->committed & (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE)) &&
        go->out->wlr_output->enabled) {
        xd_gamma *g = go->out->server->gamma;
        if (g != nullptr)
            load_calibration(g, go->out);
        mark_dirty(go->out);
    }
}

void xd_gamma_output_added(xd_output *out) {
    struct xd_gamma_output *go = XD_NEW(struct xd_gamma_output);
    go->out = out;
    out->gamma = go;
    XD_LISTEN(&out->wlr_output->events.commit, go, commit, handle_commit);
    if (out->server->gamma != nullptr)
        load_calibration(out->server->gamma, out);
    mark_dirty(out);
}

void xd_gamma_output_removed(xd_output *out) {
    struct xd_gamma_output *go = out->gamma;
    if (go == nullptr)
        return;
    xd_unlisten(&go->commit);
    xd_free(go->calibration);
    xd_free(go);
    out->gamma = nullptr;
}

static void handle_set_gamma(struct wl_listener *listener, void *data) {
    xd_gamma *g = XD_CONTAINER_OF(listener, xd_gamma, set_gamma);
    const struct wlr_gamma_control_manager_v1_set_gamma_event *e = data;
    xd_output *out = xd_output_from_wlr(g->server, e->output);
    if (out != nullptr)
        mark_dirty(out);
}

static void handle_manager_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_gamma *g = XD_CONTAINER_OF(listener, xd_gamma, manager_destroy);
    xd_unlisten(&g->set_gamma);
    xd_unlisten(&g->manager_destroy);
    g->manager = nullptr;
}

void xd_gamma_init(xd_server *server) {
    xd_gamma *g = XD_NEW(xd_gamma);
    g->server = server;
    g->kelvin = XD_NIGHTLIGHT_DAY;
    g->manager = wlr_gamma_control_manager_v1_create(server->display);
    XD_LISTEN(&g->manager->events.set_gamma, g, set_gamma, handle_set_gamma);
    XD_LISTEN(&g->manager->events.destroy, g, manager_destroy, handle_manager_destroy);
    g->timer = wl_event_loop_add_timer(server->loop, handle_timer, g);
    const char *trace_path = getenv("XD_GAMMA_TRACE");
    if (trace_path != nullptr && trace_path[0] != '\0') {
        g->trace = fopen(trace_path, "ae");
        if (g->trace == nullptr)
            wlr_log_errno(WLR_ERROR, "XD_GAMMA_TRACE %s", trace_path);
    }
    server->gamma = g;
}

void xd_gamma_finish(xd_server *server) {
    xd_gamma *g = server->gamma;
    if (g == nullptr)
        return;
    xd_unlisten(&g->set_gamma);
    xd_unlisten(&g->manager_destroy);
    wl_event_source_remove(g->timer);
    if (g->trace != nullptr)
        fclose(g->trace);
    xd_free(g);
    server->gamma = nullptr;
}
