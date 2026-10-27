/* SPDX-License-Identifier: MIT
 * Desktop Effects in the compositor: the settings (effects.ini, watched
 * and applied live), what each effect is told (window events, frames),
 * and the support file telling Desktop Effects what can be drawn.
 *
 * Each effect is an xd_effect_impl in a file of its own here, listed in
 * impl_list.h, with the same id as its entry in the settings table
 * (src/settings/effects.h). The OpenGL ones are compiled into
 * xd-compositor-fx alone and draw through glfx.h; the plain
 * xd-compositor reports them unavailable.
 */
#ifndef XD_COMPOSITOR_EFFECTS_EFFECTS_H
#define XD_COMPOSITOR_EFFECTS_EFFECTS_H

#include <xkbcommon/xkbcommon.h>

#include "../../settings/effects.h"
#include "../server.h"

/* What happened to a window, told to every effect turned on. */
typedef enum xd_window_event_kind : uint8_t {
    XD_WINDOW_EVENT_MAP,        /* shown for the first time (after it got the focus) */
    XD_WINDOW_EVENT_UNMAP,      /* about to go: its content is still there */
    XD_WINDOW_EVENT_DESTROY,    /* about to be freed */
    XD_WINDOW_EVENT_FOCUS,      /* became the active window */
    XD_WINDOW_EVENT_MAXIMIZE,   /* jumped to a new size: maximized, restored, snapped */
    XD_WINDOW_EVENT_MINIMIZE,   /* about to be hidden */
    XD_WINDOW_EVENT_UNMINIMIZE, /* shown again */
    XD_WINDOW_EVENT_TITLE,      /* its title changed */
} xd_window_event_kind;

typedef struct xd_window_event {
    xd_window_event_kind kind;
    xd_view *view;
    struct wlr_box from; /* MAXIMIZE: its outer box before */
} xd_window_event;

/* Input offered to the effects that are on (input.c), before the
 * compositor's own bindings and the programs: keys, buttons and the
 * vertical wheel while no window is being moved or sized, the pointer's
 * moves and the modifiers always. */
typedef enum xd_effect_input_kind : uint8_t {
    XD_EFFECT_INPUT_KEY,       /* pressed or released */
    XD_EFFECT_INPUT_BUTTON,    /* pressed or released */
    XD_EFFECT_INPUT_AXIS,      /* a step of the vertical wheel */
    XD_EFFECT_INPUT_MOTION,    /* the pointer moved */
    XD_EFFECT_INPUT_MODIFIERS, /* the modifiers held changed */
} xd_effect_input_kind;

typedef struct xd_effect_input {
    xd_effect_input_kind kind;
    uint32_t modifiers; /* xd_binding_modifier bits held */
    uint32_t keysym;    /* KEY: lower case, Shift+Tab as Tab */
    uint32_t button;    /* BUTTON: BTN_LEFT... (linux/input-event-codes.h) */
    bool pressed;       /* KEY, BUTTON */
    double delta;       /* AXIS: positive is down (towards the user) */
    int steps;          /* AXIS: how many of the wheel's notches (at least 1) */
    double x, y;        /* the pointer, layout coordinates */
    uint32_t time_msec; /* KEY, BUTTON, MOTION: the event's time */
} xd_effect_input;

/* A KEY or BUTTON option, read once (xd_effects_get_binding) to match
 * input against. Button4 and Button5 are X11's wheel up and down. */
typedef struct xd_effect_binding {
    uint32_t modifiers; /* xd_binding_modifier bits */
    uint32_t keysym;    /* lower case; 0 for a button or none */
    uint32_t button;    /* BTN_LEFT...; 0 for a key or none */
    int wheel;          /* -1 up, 1 down, 0 neither */
} xd_effect_binding;

/* An effect's code. Every hook is optional. `state` is what enable
 * returned. */
typedef struct xd_effect_impl {
    const char *id; /* its xd_effect_info's */
    /* Turned on (at startup, or in Desktop Effects): its state. */
    void *(*enable)(xd_server *server);
    void (*disable)(xd_server *server, void *state);
    /* Its options, right after enable and whenever they change. */
    void (*configure)(xd_server *server, void *state, const xd_effects_settings *settings,
                      size_t effect);
    void (*window_event)(xd_server *server, void *state, const xd_window_event *event);
    /* Before each frame of each output is drawn (Compiz's
     * preparePaintScreen): `now` in seconds, CLOCK_MONOTONIC. */
    void (*frame)(xd_server *server, void *state, xd_output *output, double now);
    /* Input (xd_effect_input), the effects' only way in: true if the
     * effect used it up. Keys, buttons and wheel steps go no further once
     * one has (a button's release then goes nowhere else either, whoever
     * claims it); every effect sees each move and modifier change. Keys
     * and buttons never come while the screen is locked. */
    bool (*input)(xd_server *server, void *state, const xd_effect_input *input);
    /* Whether it animates this window event itself. For MINIMIZE and
     * UNMINIMIZE, XP's animation (animate.c, a bar flying to the taskbar
     * button) is left out then; for any event, effects later in
     * impl_list.h that ask (xd_effects_is_animated_before) leave the
     * window to it: Magic Lamp's minimize, not Animations' too. */
    bool (*animates)(xd_server *server, void *state, const xd_view *view,
                     xd_window_event_kind kind);
} xd_effect_impl;

#define XD_EFFECT_IMPL(name) extern const xd_effect_impl xd_effect_impl_##name;
#include "impl_list.h"
#undef XD_EFFECT_IMPL

/* At startup, once the renderer and the scene exist. */
void xd_effects_init(xd_server *server);
/* Once the Wayland socket is up: the support file, for that display. */
void xd_effects_publish(xd_server *server, const char *display);
void xd_effects_finish(xd_server *server);
void xd_effects_window_event(xd_view *view, xd_window_event_kind kind,
                             const struct wlr_box *from);
/* output.c: before building the output's frame (the effects' frame
 * hooks, in impl_list.h's order, then the OpenGL window effects), and
 * after committing it. */
void xd_effects_output_frame(xd_output *output);
void xd_effects_output_frame_done(xd_output *output);
/* Whether `effect` (the settings' index) is turned on and running. */
bool xd_effects_is_running(const xd_server *server, const char *id);
/* output.c: whether an output filter (Water) draws the output's frames
 * (every frame is drawn then), and the frame's state, as
 * wlr_scene_output_build_state makes it, filtered: a filter works on the
 * picture the scene graph drew, whole-output effects' (Cube, Expo,
 * Shift) and Zoom's included. */
bool xd_effects_is_output_filtered(const xd_output *output);
bool xd_effects_build_output_state(xd_output *output, struct wlr_scene_output *scene_output,
                                   struct wlr_output_state *state);
/* input.c: offers input to the effects (the modifiers and pointer filled
 * in here); true if one used it up. */
bool xd_effects_input(xd_server *server, xd_effect_input *input);
/* view.c: whether an effect animates the window's minimize or
 * unminimize, in place of XP's animation. */
bool xd_effects_animates_window(xd_view *view, xd_window_event_kind kind);
/* Whether an effect on and listed before `impl` animates the window
 * event (its `animates`): `impl` then leaves the window alone. */
bool xd_effects_is_animated_before(const xd_effect_impl *impl, xd_view *view,
                                   xd_window_event_kind kind);

/* Helpers for the effects. */
/* A KEY or BUTTON option of `effect`; all zeros for none. */
void xd_effects_get_binding(const xd_effects_settings *settings, size_t effect, const char *key,
                            xd_effect_binding *binding);
/* Whether a key or button press (or a wheel step) is that binding:
 * exactly its modifiers held. */
bool xd_effects_binding_matches(const xd_effect_binding *binding, const xd_effect_input *input);
/* Whether every modifier of the binding is still held (a switcher its
 * key opened ends when one is let go). */
bool xd_effects_is_binding_held(const xd_effect_binding *binding, const xd_effect_input *input);
/* Whether a window matches an XD_EFFECT_OPTION_MATCH value. */
bool xd_effects_match_view(const char *match, const xd_view *view);
/* A tree above every window and panel and below the lock screen, for an
 * effect's own pictures; destroy it with wlr_scene_node_destroy. */
struct wlr_scene_tree *xd_effects_create_overlay(xd_server *server);
/* Marks a tree (a child of the scene's root) as an effect's own picture:
 * scene captures (glfx.h) leave it out, so a desktop's picture in Expo or
 * on the cube is the desktop's, not what an effect draws over it. */
void xd_effects_tag_picture(struct wlr_scene_tree *tree);
/* Asks every output for another frame (an effect is animating). */
void xd_effects_schedule_frames(xd_server *server);

#endif
