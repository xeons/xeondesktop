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
} xd_window_event_kind;

typedef struct xd_window_event {
    xd_window_event_kind kind;
    xd_view *view;
    struct wlr_box from; /* MAXIMIZE: its outer box before */
} xd_window_event;

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
    /* Input, before the desktop's own shortcuts and clicks (Compiz's key
     * and button bindings): true if the effect used it up. `modifiers`
     * are the keyboard's WLR_MODIFIER_* bits; never while the screen is
     * locked. */
    bool (*key)(xd_server *server, void *state, uint32_t modifiers, uint32_t keysym);
    /* The modifiers held changed (a switcher ends when they're let go). */
    void (*modifiers)(xd_server *server, void *state, uint32_t modifiers);
    bool (*button)(xd_server *server, void *state, uint32_t button, bool pressed,
                   uint32_t modifiers);
    /* The pointer moved (server->cursor is where to): true if it's used
     * up, and nothing under it hears of it. */
    bool (*motion)(xd_server *server, void *state, uint32_t time_msec);
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
/* output.c: before building the output's frame, and after committing it. */
void xd_effects_output_frame(xd_output *output);
void xd_effects_output_frame_done(xd_output *output);
/* Whether `effect` (the settings' index) is turned on and running. */
bool xd_effects_is_running(const xd_server *server, const char *id);
/* input.c: a key pressed, the modifiers changed, a button, the pointer
 * moved; true if an effect used it up. */
bool xd_effects_key(xd_server *server, uint32_t modifiers, uint32_t keysym);
void xd_effects_modifiers(xd_server *server, uint32_t modifiers);
bool xd_effects_button(xd_server *server, uint32_t button, bool pressed, uint32_t modifiers);
bool xd_effects_motion(xd_server *server, uint32_t time_msec);

/* A KEY or BUTTON option's value, ready to compare with input:
 * WLR_MODIFIER_* bits (Shift, Ctrl, Alt, Logo) and the keysym, or the
 * button (BTN_LEFT...). Empty: the binding is none. */
typedef struct xd_effect_binding {
    uint32_t modifiers;
    uint32_t keysym; /* KEY: lower case */
    uint32_t button; /* BUTTON */
    bool empty;
} xd_effect_binding;

xd_effect_binding xd_effects_get_binding(const xd_effects_settings *settings, size_t effect,
                                         const char *key);
/* Whether the key, with exactly those modifiers held (Caps Lock, Num
 * Lock and the like aside), is the binding's. Shift+Tab's ISO_Left_Tab
 * counts as Tab. */
bool xd_effects_binding_matches_key(const xd_effect_binding *binding, uint32_t modifiers,
                                    uint32_t keysym);
bool xd_effects_binding_matches_button(const xd_effect_binding *binding, uint32_t modifiers,
                                       uint32_t button);
/* Whether every modifier of the binding is still held. */
bool xd_effects_binding_is_held(const xd_effect_binding *binding, uint32_t modifiers);

#endif
