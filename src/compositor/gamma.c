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

static int64_t ms_since(const struct timespec *time) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)(now.tv_sec - time->tv_sec) * 1000 + (now.tv_nsec - time->tv_nsec) / 1000000;
}

static void mark_dirty(xd_output *out) {
    if (out->gamma == nullptr)
        return;
    out->gamma->dirty = true;
    wlr_output_schedule_frame(out->wlr_output);
}

static void mark_all_dirty(xd_gamma *gamma) {
    XD_LIST_FOR_EACH(out, &gamma->server->outputs, xd_output, link) { mark_dirty(out); }
}

/* Loads the output's calibration from its profile in the settings,
 * sampled at its gamma size. */
static void load_calibration(xd_gamma *gamma, xd_output *out) {
    struct xd_gamma_output *go = out->gamma;
    char key[192];
    xd_output_key(out->wlr_output, key, sizeof(key));
    const char *path = "";
    for (size_t i = 0; i < gamma->profile_count; i++) {
        if (strcmp(gamma->profiles[i].monitor, key) == 0)
            path = gamma->profiles[i].path;
    }
    size_t count = wlr_output_get_gamma_size(out->wlr_output);
    if (count == 0 && gamma->trace != nullptr)
        count = TRACE_RAMP;
    if (strcmp(go->profile, path) == 0 &&
        (go->calibration == nullptr || go->calibration_n == count))
        return;
    snprintf(go->profile, sizeof(go->profile), "%s", path);
    XD_FREE(&go->calibration);
    go->calibration_n = 0;
    if (path[0] != '\0' && count >= 2) {
        float *curves = xd_alloc_array(3 * count, sizeof(*curves));
        if (xd_vcgt_load(path, count, curves)) {
            go->calibration = curves;
            go->calibration_n = count;
            wlr_log(WLR_INFO, "output %s: calibration from %s", out->wlr_output->name, path);
        } else {
            xd_free(curves);
            wlr_log(WLR_INFO, "output %s: %s has no calibration (vcgt) to load",
                    out->wlr_output->name, path);
        }
    }
    mark_dirty(out);
}

static void trace(xd_gamma *gamma, xd_output *out, const char *source, const uint16_t *lut,
                  size_t count, bool applied) {
    if (gamma->trace == nullptr)
        return;
    /* The top of each channel's ramp: what white becomes. */
    fprintf(gamma->trace, "%s %s %.0f", out->wlr_output->name, source, gamma->kelvin);
    if (lut != nullptr && count > 0)
        fprintf(gamma->trace, " r=%.3f g=%.3f b=%.3f", lut[count - 1] / 65535.0,
                lut[2 * count - 1] / 65535.0, lut[3 * count - 1] / 65535.0);
    fprintf(gamma->trace, " %s\n", applied ? "applied" : "not-applied");
    fflush(gamma->trace);
}

bool xd_gamma_pending(const xd_output *out) {
    return out->gamma != nullptr && out->gamma->dirty;
}

void xd_gamma_frame(xd_output *out, struct wlr_output_state *state) {
    xd_gamma *gamma = out->server->gamma;
    struct xd_gamma_output *go = out->gamma;
    if (gamma == nullptr || go == nullptr || !go->dirty)
        return;
    go->dirty = false;
    struct wlr_output *output = out->wlr_output;
    size_t size = wlr_output_get_gamma_size(output);

    struct wlr_gamma_control_v1 *control =
        wlr_gamma_control_manager_v1_get_control(gamma->manager, output);
    if (control != nullptr) {
        /* A program's own ramp. */
        if (!wlr_gamma_control_v1_apply(control, state))
            return;
        bool ok = wlr_output_test_state(output, state);
        if (!ok) {
            /* Its destruction marks the output dirty again, for ours. */
            wlr_gamma_control_v1_send_failed_and_destroy(control);
            wlr_output_state_set_color_transform(state, nullptr);
        }
        go->ramp_set = true;
        trace(gamma, out, "client", nullptr, 0, ok);
        return;
    }

    bool night = gamma->kelvin < XD_NIGHTLIGHT_DAY - 0.5;
    if (!night && go->calibration == nullptr) {
        /* Nothing of ours: back to the plain ramp, if one was loaded. */
        if (size > 0 && go->ramp_set) {
            wlr_output_state_set_color_transform(state, nullptr);
            go->ramp_set = false;
        }
        trace(gamma, out, "none", nullptr, 0, size > 0);
        return;
    }
    size_t count = size > 0 ? size : TRACE_RAMP;
    if (size == 0 && gamma->trace == nullptr)
        return;
    if (go->calibration != nullptr && go->calibration_n != count)
        return; /* a new mode's gamma size: handle_commit reloads it */
    uint16_t *lut = xd_alloc_array(3 * count, sizeof(*lut));
    double rgb[3];
    xd_blackbody_rgb(gamma->kelvin, rgb);
    xd_gamma_ramp_fill(lut, lut + count, lut + 2 * count, count, rgb, go->calibration);
    const char *source = !night ? "calibration" : go->calibration ? "calibration+night-light"
                                                                  : "night-light";
    bool applied = false;
    if (size > 0) {
        struct wlr_color_transform *tr =
            wlr_color_transform_init_lut_3x1d(count, lut, lut + count, lut + 2 * count);
        if (tr != nullptr) {
            wlr_output_state_set_color_transform(state, tr);
            wlr_color_transform_unref(tr);
            applied = wlr_output_test_state(output, state);
            go->ramp_set = true;
            if (!applied) {
                wlr_log(WLR_ERROR, "output %s: the gamma ramp was refused", output->name);
                wlr_output_state_set_color_transform(state, nullptr);
            }
        }
    }
    trace(gamma, out, source, lut, count, applied);
    xd_free(lut);
}

static void update(xd_gamma *gamma);

static int handle_timer(void *data) {
    update(data);
    return 0;
}

/* Works out the temperature now, shows it if it changed, and sets the
 * timer for the next look. */
static void update(xd_gamma *gamma) {
    double target = xd_nightlight_kelvin(&gamma->night, time(nullptr));
    double k = target;
    int next = 0;
    if (gamma->fading) {
        int64_t ms = ms_since(&gamma->fade_start);
        double progress = (double)ms / FADE_MS;
        if (progress >= 1.0) {
            gamma->fading = false;
        } else {
            /* Smoothstep: no jolt at either end. */
            progress = progress * progress * (3.0 - 2.0 * progress);
            k = gamma->fade_from + (target - gamma->fade_from) * progress;
            next = FADE_TICK_MS;
        }
    }
    if (next == 0 && gamma->night.enabled && gamma->night.schedule != XD_NIGHTLIGHT_ALWAYS) {
        bool between = target > gamma->night.temperature + 0.5 && target < XD_NIGHTLIGHT_DAY - 0.5;
        next = between ? TRANSITION_TICK_MS : IDLE_TICK_MS;
    }
    if (fabs(k - gamma->kelvin) >= 1.0 || (k != gamma->kelvin && !gamma->fading)) {
        gamma->kelvin = k;
        mark_all_dirty(gamma);
    }
    wl_event_source_timer_update(gamma->timer, next);
}

void xd_gamma_apply_settings(xd_server *server, const xd_settings *settings) {
    xd_gamma *gamma = server->gamma;
    if (gamma == nullptr)
        return;
    bool first = !gamma->have_settings;
    bool night_changed = memcmp(&gamma->night, &settings->night_light, sizeof(gamma->night)) != 0;
    gamma->have_settings = true;
    gamma->night = settings->night_light;
    gamma->profile_count = settings->color_profile_count;
    memcpy(gamma->profiles, settings->color_profiles, sizeof(gamma->profiles));
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (out->gamma != nullptr)
            load_calibration(gamma, out);
    }
    if (night_changed && !first) {
        /* Turned on or off, or the strength moved: fade, as Windows 10's
         * night light does. */
        gamma->fading = true;
        gamma->fade_from = gamma->kelvin;
        clock_gettime(CLOCK_MONOTONIC, &gamma->fade_start);
    }
    update(gamma);
}

static void handle_commit(struct wl_listener *listener, void *data) {
    struct xd_gamma_output *go = XD_CONTAINER_OF(listener, struct xd_gamma_output, commit);
    const struct wlr_output_event_commit *output_event_commit = data;
    /* Turned back on (a mode set or the monitor woken): the CRTC may come
     * back without our ramp. A new mode may change the gamma size. */
    if ((output_event_commit->state->committed &
         (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE)) &&
        go->out->wlr_output->enabled) {
        xd_gamma *gamma = go->out->server->gamma;
        if (gamma != nullptr)
            load_calibration(gamma, go->out);
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
    xd_gamma *gamma = XD_CONTAINER_OF(listener, xd_gamma, set_gamma);
    const struct wlr_gamma_control_manager_v1_set_gamma_event
        *gamma_control_manager_v1_set_gamma_event = data;
    xd_output *out =
        xd_output_from_wlr(gamma->server, gamma_control_manager_v1_set_gamma_event->output);
    if (out != nullptr)
        mark_dirty(out);
}

static void handle_manager_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_gamma *gamma = XD_CONTAINER_OF(listener, xd_gamma, manager_destroy);
    xd_unlisten(&gamma->set_gamma);
    xd_unlisten(&gamma->manager_destroy);
    gamma->manager = nullptr;
}

void xd_gamma_init(xd_server *server) {
    xd_gamma *gamma = XD_NEW(xd_gamma);
    gamma->server = server;
    gamma->kelvin = XD_NIGHTLIGHT_DAY;
    gamma->manager = wlr_gamma_control_manager_v1_create(server->display);
    xd_trust_privileged(server, gamma->manager->global);
    XD_LISTEN(&gamma->manager->events.set_gamma, gamma, set_gamma, handle_set_gamma);
    XD_LISTEN(&gamma->manager->events.destroy, gamma, manager_destroy, handle_manager_destroy);
    gamma->timer = wl_event_loop_add_timer(server->loop, handle_timer, gamma);
    const char *trace_path = getenv("XD_GAMMA_TRACE");
    if (trace_path != nullptr && trace_path[0] != '\0') {
        gamma->trace = fopen(trace_path, "ae");
        if (gamma->trace == nullptr)
            wlr_log_errno(WLR_ERROR, "XD_GAMMA_TRACE %s", trace_path);
    }
    server->gamma = gamma;
}

void xd_gamma_finish(xd_server *server) {
    xd_gamma *gamma = server->gamma;
    if (gamma == nullptr)
        return;
    xd_unlisten(&gamma->set_gamma);
    xd_unlisten(&gamma->manager_destroy);
    wl_event_source_remove(gamma->timer);
    if (gamma->trace != nullptr)
        fclose(gamma->trace);
    xd_free(gamma);
    server->gamma = nullptr;
}
