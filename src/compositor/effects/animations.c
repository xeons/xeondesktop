/* SPDX-License-Identifier: MIT
 * Animations (both builds): Compiz's animation plugin for windows
 * opening, closing, minimizing and coming back, each with its own effect
 * (animations_model.h), duration and windows.
 *
 * The window's mirror (mirror.h) is drawn in its place in the stacking
 * order and animated, the window itself hidden meanwhile when it's
 * appearing. A closing window's mirror keeps its last picture after the
 * window has gone. Minimizing goes to the window's taskbar button, which
 * the shell tells view.c (as XP's animation in animate.c uses), and when
 * Animations animates it, XP's bar doesn't fly. The Animations Add-On's
 * effects (addon.c) are choices too: the Add-On draws those, and when it
 * doesn't (off, or no OpenGL), Animations zooms instead.
 */
#include <time.h>

#include "../../core/alloc.h"
#include "animations_model.h"
#include "effects.h"
#include "mirror.h"

typedef enum animation_event : uint8_t {
    ANIMATION_EVENT_OPEN,
    ANIMATION_EVENT_CLOSE,
    ANIMATION_EVENT_MINIMIZE,
    ANIMATION_EVENT_UNMINIMIZE,
    ANIMATION_EVENT_COUNT,
} animation_event;

static const char *const animation_event_prefixes[ANIMATION_EVENT_COUNT] = {
    "Open", "Close", "Minimize", "Unminimize"};

typedef struct animation_options {
    xd_animation_effect effect;
    double seconds;
    char windows[512];
} animation_options;

typedef struct animations_state {
    xd_server *server;
    animation_options options[ANIMATION_EVENT_COUNT];
    struct wl_list running; /* running_animation.link */
} animations_state;

typedef struct running_animation {
    struct wl_list link; /* animations_state.running */
    xd_view *view;       /* nullptr once it's gone */
    xd_mirror *mirror;
    xd_animation_effect effect;
    double seconds, start; /* start: 0 until its first frame */
    bool appearing;
    bool hides_view; /* the window is hidden till it's over */
    xd_effect_rect box;
    xd_effect_rect target;
    bool has_target;
} running_animation;

static bool animation_get_event(xd_window_event_kind kind, animation_event *event) {
    switch (kind) {
    case XD_WINDOW_EVENT_MAP:
        *event = ANIMATION_EVENT_OPEN;
        return true;
    case XD_WINDOW_EVENT_UNMAP:
        *event = ANIMATION_EVENT_CLOSE;
        return true;
    case XD_WINDOW_EVENT_MINIMIZE:
        *event = ANIMATION_EVENT_MINIMIZE;
        return true;
    case XD_WINDOW_EVENT_UNMINIMIZE:
        *event = ANIMATION_EVENT_UNMINIMIZE;
        return true;
    case XD_WINDOW_EVENT_DESTROY:
    case XD_WINDOW_EVENT_FOCUS:
    case XD_WINDOW_EVENT_MAXIMIZE:
    case XD_WINDOW_EVENT_TITLE:
        return false;
    }
    return false;
}

/* Whether the window's `event` is animated: an effect is set for it, the
 * window is one of its windows, and it's on the current desktop. A
 * minimized window closing (from its taskbar button) isn't on the screen
 * to animate, as Magic Lamp and the Add-On have it too. */
static bool animation_is_wanted(const animations_state *state, animation_event event,
                                const xd_view *view) {
    const animation_options *options = &state->options[event];
    return options->effect != XD_ANIMATION_NONE && !view->fullscreen &&
           xd_desktops_is_view_shown(view) &&
           !(event == ANIMATION_EVENT_CLOSE && view->minimized) &&
           xd_effects_match_view(options->windows, view);
}

static void animation_free(running_animation *animation) {
    xd_view *view = animation->view;
    /* The desktop may have changed meanwhile: shown only if it's this one's. */
    if (animation->hides_view && view != nullptr && view->mapped && !view->minimized &&
        xd_desktops_is_view_shown(view))
        wlr_scene_node_set_enabled(&view->tree->node, true);
    xd_mirror_destroy(animation->mirror);
    wl_list_remove(&animation->link);
    xd_free(animation);
}

static void animation_cancel_view(animations_state *state, const xd_view *view) {
    running_animation *animation, *next;
    wl_list_for_each_safe(animation, next, &state->running, link) {
        if (animation->view == view)
            animation_free(animation);
    }
}

static void animation_start(animations_state *state, animation_event event, xd_view *view) {
    animation_cancel_view(state, view);
    const animation_options *options = &state->options[event];
    running_animation *animation = XD_NEW(running_animation);
    animation->view = view;
    /* An Add-On effect the Add-On (addon.c, before us in the list) didn't
     * take: it's off, or there's no OpenGL. */
    animation->effect = xd_animation_get_fallback(options->effect);
    animation->seconds = options->seconds;
    animation->appearing = event == ANIMATION_EVENT_OPEN || event == ANIMATION_EVENT_UNMINIMIZE;
    struct wlr_box box = xd_view_outer_box(view);
    animation->box = (xd_effect_rect){box.x, box.y, box.width, box.height};
    bool minimizing = event == ANIMATION_EVENT_MINIMIZE || event == ANIMATION_EVENT_UNMINIMIZE;
    if (minimizing && view->has_button_box) {
        const struct wlr_box *button = &view->button_box;
        animation->target = (xd_effect_rect){button->x, button->y, button->width, button->height};
        animation->has_target = true;
    }
    /* Drawn where the window is in the stacking order. */
    struct wlr_scene_tree *parent = view->tree->node.parent;
    animation->mirror = xd_mirror_create(view, parent);
    wlr_scene_node_place_above(&xd_mirror_get_tree(animation->mirror)->node, &view->tree->node);
    if (event == ANIMATION_EVENT_CLOSE) {
        xd_mirror_detach(animation->mirror); /* its last picture */
        animation->view = nullptr;
    }
    if (animation->appearing) {
        wlr_scene_node_set_enabled(&view->tree->node, false);
        animation->hides_view = true;
    }
    wl_list_insert(&state->running, &animation->link);
    xd_effects_schedule_frames(state->server);
}

static void animations_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                           const xd_window_event *event) {
    animations_state *state = data;
    xd_view *view = event->view;
    if (event->kind == XD_WINDOW_EVENT_DESTROY) {
        running_animation *animation;
        wl_list_for_each(animation, &state->running, link) {
            if (animation->view == view) {
                xd_mirror_detach(animation->mirror);
                animation->view = nullptr;
            }
        }
        return;
    }
    animation_event kind;
    if (!animation_get_event(event->kind, &kind))
        return;
    /* An effect before it in the list (Magic Lamp, one of Compiz's
     * Animation's own choices) has this one. */
    if (!animation_is_wanted(state, kind, view) ||
        xd_effects_is_animated_before(&xd_effect_impl_animations, view, event->kind)) {
        /* Whatever it was doing is over. */
        animation_cancel_view(state, view);
        return;
    }
    animation_start(state, kind, view);
}

static void animations_handle_frame(xd_server *server, void *data,
                                    [[maybe_unused]] xd_output *output, double now) {
    animations_state *state = data;
    if (wl_list_empty(&state->running))
        return;
    struct timespec when;
    clock_gettime(CLOCK_MONOTONIC, &when);
    running_animation *animation, *next;
    wl_list_for_each_safe(animation, next, &state->running, link) {
        /* Its window went to another desktop (switched away from): its
         * picture would stay on this one. */
        if (animation->view != nullptr && !xd_desktops_is_view_shown(animation->view)) {
            animation_free(animation);
            continue;
        }
        if (animation->start == 0)
            animation->start = now;
        double progress = (now - animation->start) / XD_MAX(animation->seconds, 0.001);
        if (progress >= 1.0) {
            animation_free(animation);
            continue;
        }
        double presence = xd_animation_get_presence(animation->appearing, progress);
        xd_mirror_update(animation->mirror);
        /* A window opening may still be settling on its size. */
        if (animation->view != nullptr && animation->appearing) {
            struct wlr_box box = xd_mirror_get_box(animation->mirror);
            animation->box = (xd_effect_rect){box.x, box.y, box.width, box.height};
        }
        xd_animation_frame frame =
            xd_animation_get_frame(animation->effect, animation->box,
                                   animation->has_target ? &animation->target : nullptr,
                                   presence);
        xd_mirror_place(animation->mirror, frame.x, frame.y, frame.scale_x, frame.scale_y);
        xd_mirror_set_opacity(animation->mirror, (float)frame.opacity);
        if (animation->hides_view)
            xd_mirror_send_frame_done(animation->mirror, &when);
    }
    if (!wl_list_empty(&state->running))
        xd_effects_schedule_frames(server);
}

static bool animations_animates([[maybe_unused]] xd_server *server, void *data,
                                const xd_view *view, xd_window_event_kind kind) {
    animations_state *state = data;
    animation_event event;
    return (kind == XD_WINDOW_EVENT_MINIMIZE || kind == XD_WINDOW_EVENT_UNMINIMIZE) &&
           animation_get_event(kind, &event) && animation_is_wanted(state, event, view);
}

static void *animations_enable(xd_server *server) {
    animations_state *state = XD_NEW(animations_state);
    state->server = server;
    wl_list_init(&state->running);
    return state;
}

static void animations_disable([[maybe_unused]] xd_server *server, void *data) {
    animations_state *state = data;
    running_animation *animation, *next;
    wl_list_for_each_safe(animation, next, &state->running, link) {
        animation_free(animation);
    }
    xd_free(state);
}

static void animations_configure([[maybe_unused]] xd_server *server, void *data,
                                 const xd_effects_settings *settings, size_t effect) {
    animations_state *state = data;
    for (size_t i = 0; i < ANIMATION_EVENT_COUNT; i++) {
        animation_options *options = &state->options[i];
        char key[32];
        snprintf(key, sizeof(key), "%sEffect", animation_event_prefixes[i]);
        /* By name: each event lists its own effects. */
        options->effect =
            xd_animation_find_effect(xd_effects_get_choice_name(settings, effect, key));
        snprintf(key, sizeof(key), "%sDuration", animation_event_prefixes[i]);
        options->seconds = xd_effects_get_int(settings, effect, key) / 1000.0;
        snprintf(key, sizeof(key), "%sWindows", animation_event_prefixes[i]);
        snprintf(options->windows, sizeof(options->windows), "%s",
                 xd_effects_get_text(settings, effect, key));
    }
}

const xd_effect_impl xd_effect_impl_animations = {
    .id = "Animations",
    .enable = animations_enable,
    .disable = animations_disable,
    .configure = animations_configure,
    .window_event = animations_handle_window_event,
    .frame = animations_handle_frame,
    .animates = animations_animates,
};
