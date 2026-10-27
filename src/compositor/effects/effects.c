/* SPDX-License-Identifier: MIT
 * Desktop Effects in the compositor (effects.h): effects.ini loaded at
 * startup and whenever it's saved, each effect turned on or off and told
 * its options, window events and frames passed on to the effects that are
 * on, and the support file written for Desktop Effects.
 */
#include "effects.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "../../core/alloc.h"
#ifdef XD_SCENEFX
#include "glfx.h"
#endif

static const xd_effect_impl *const impls[] = {
#define XD_EFFECT_IMPL(name) &xd_effect_impl_##name,
#include "impl_list.h"
    nullptr, /* so the list is never empty */
};
/* Tested, so it counts as used where the list is empty. */
#ifdef XD_EFFECT_IMPL
#undef XD_EFFECT_IMPL
#endif

/* A function, not a constant: the plain build's loops over none would
 * be "always false" comparisons. */
static size_t effects_count_impls(void) {
    return XD_ARRAY_LEN(impls) - 1;
}

typedef struct running_effect {
    size_t index; /* in the settings */
    void *state;
    bool on;
} running_effect;

struct xd_effects {
    xd_server *server;
    xd_effects_settings *settings;
    running_effect running[XD_ARRAY_LEN(impls)];
    int watch_fd;
    struct wl_event_source *watch;
    xd_effects_support support;
    char display[64]; /* the support file's, once published */
    /* Buttons whose press an effect used up: their releases are used up
     * too, whichever effect hears them. */
    uint32_t held_buttons[8];
    size_t held_count;
};

static double effects_get_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Turns effects on and off as the settings say, and tells those on their
 * options. */
static void effects_apply_settings(xd_effects *effects) {
    xd_server *server = effects->server;
    for (size_t i = 0; i < effects_count_impls(); i++) {
        const xd_effect_impl *impl = impls[i];
        running_effect *running = &effects->running[i];
        const xd_effect_info *info = xd_effects_get_info(effects->settings, running->index);
        bool want = xd_effects_is_enabled(effects->settings, running->index) &&
                    (!info->needs_opengl || effects->support.opengl);
        if (running->on && !want) {
            if (impl->disable != nullptr)
                impl->disable(server, running->state);
            running->on = false;
            running->state = nullptr;
            wlr_log(WLR_INFO, "effects: %s off", info->name);
            continue;
        }
        if (!want)
            continue;
        if (!running->on) {
            running->state = impl->enable != nullptr ? impl->enable(server) : nullptr;
            running->on = true;
            wlr_log(WLR_INFO, "effects: %s on", info->name);
        }
        if (impl->configure != nullptr)
            impl->configure(server, running->state, effects->settings, running->index);
    }
}

static int effects_handle_settings_changed([[maybe_unused]] int fd, [[maybe_unused]] uint32_t mask,
                                   void *data) {
    xd_effects *effects = data;
    if (!xd_effects_has_changed(effects->watch_fd))
        return 0;
    xd_effects_load(effects->settings);
    effects_apply_settings(effects);
    return 0;
}

#ifndef XD_SCENEFX
/* Why the plain build can't draw OpenGL effects, in the user's words. */
static void effects_explain_plain_build(const xd_server *server, xd_effects_support *support) {
    const char *reason =
        wlr_renderer_is_pixman(server->renderer)
            ? "The desktop is drawn in software on this computer (no graphics card the "
              "effects can use, or a virtual machine's), so effects drawn with OpenGL are "
              "unavailable."
            : "Effects drawn with OpenGL need XeonDesktop's compositor built with SceneFX "
              "(xd-compositor-fx), which isn't installed or was turned off (XD_NO_FX).";
    snprintf(support->reason, sizeof(support->reason), "%s", reason);
    if (wlr_renderer_is_gles2(server->renderer))
        snprintf(support->renderer, sizeof(support->renderer), "%s", "OpenGL ES (plain build)");
    else
        snprintf(support->renderer, sizeof(support->renderer), "%s", "Software (pixman)");
}
#endif

void xd_effects_init(xd_server *server) {
    xd_effects *effects = XD_NEW(xd_effects);
    effects->server = server;
    effects->settings = xd_effects_create_settings();
    effects->watch_fd = -1;
    server->effects = effects;
    for (size_t i = 0; i < effects_count_impls(); i++) {
        bool known = xd_effects_find(effects->settings, impls[i]->id, &effects->running[i].index);
        XD_ASSERT(known); /* every effect has its table entry */
    }
#ifdef XD_SCENEFX
    effects->support.opengl = xd_glfx_init(server, &effects->support);
#else
    effects_explain_plain_build(server, &effects->support);
#endif
    wlr_log(WLR_INFO, "effects: OpenGL effects %s (%s)",
            effects->support.opengl ? "available" : "unavailable", effects->support.renderer);

    xd_effects_load(effects->settings);
    effects->watch_fd = xd_effects_watch();
    if (effects->watch_fd >= 0)
        effects->watch = wl_event_loop_add_fd(server->loop, effects->watch_fd, WL_EVENT_READABLE,
                                              effects_handle_settings_changed, effects);
    effects_apply_settings(effects);
}

void xd_effects_publish(xd_server *server, const char *display) {
    xd_effects *effects = server->effects;
    if (effects == nullptr || display == nullptr)
        return;
    snprintf(effects->display, sizeof(effects->display), "%s", display);
    if (!xd_effects_write_support(effects->display, &effects->support))
        wlr_log(WLR_ERROR, "effects: Desktop Effects won't know what can be drawn");
}

void xd_effects_finish(xd_server *server) {
    xd_effects *effects = server->effects;
    if (effects == nullptr)
        return;
    for (size_t i = 0; i < effects_count_impls(); i++) {
        running_effect *running = &effects->running[i];
        if (running->on && impls[i]->disable != nullptr)
            impls[i]->disable(server, running->state);
        running->on = false;
    }
#ifdef XD_SCENEFX
    xd_glfx_finish(server);
#endif
    if (effects->watch != nullptr)
        wl_event_source_remove(effects->watch);
    if (effects->watch_fd >= 0)
        close(effects->watch_fd);
    if (effects->display[0] != '\0')
        xd_effects_remove_support(effects->display);
    xd_effects_destroy_settings(effects->settings);
    server->effects = nullptr;
    xd_free(effects);
}

void xd_effects_window_event(xd_view *view, xd_window_event_kind kind,
                             const struct wlr_box *from) {
    xd_server *server = view->server;
    xd_effects *effects = server->effects;
    if (effects == nullptr)
        return;
    xd_window_event event = {.kind = kind, .view = view};
    if (from != nullptr)
        event.from = *from;
    for (size_t i = 0; i < effects_count_impls(); i++) {
        if (effects->running[i].on && impls[i]->window_event != nullptr)
            impls[i]->window_event(server, effects->running[i].state, &event);
    }
#ifdef XD_SCENEFX
    /* After the effects: one could still take a last picture of it. */
    if (kind == XD_WINDOW_EVENT_UNMAP || kind == XD_WINDOW_EVENT_DESTROY ||
        kind == XD_WINDOW_EVENT_MINIMIZE)
        xd_glfx_forget_view(view);
    if (kind == XD_WINDOW_EVENT_DESTROY)
        xd_glfx_drop_view(view);
    /* Scene captures (glfx_output.c) keep its frame's pixels no more. */
    if (kind == XD_WINDOW_EVENT_UNMAP || kind == XD_WINDOW_EVENT_DESTROY)
        xd_glfx_capture_forget_view(view);
#endif
}

void xd_effects_output_frame(xd_output *output) {
    xd_server *server = output->server;
    xd_effects *effects = server->effects;
    if (effects == nullptr)
        return;
    double now = effects_get_now();
    for (size_t i = 0; i < effects_count_impls(); i++) {
        if (effects->running[i].on && impls[i]->frame != nullptr)
            impls[i]->frame(server, effects->running[i].state, output, now);
    }
#ifdef XD_SCENEFX
    xd_glfx_frame(server, output, now);
#endif
}

void xd_effects_output_frame_done([[maybe_unused]] xd_output *output) {
#ifdef XD_SCENEFX
    xd_glfx_frame_done(output->server, output);
#endif
}

bool xd_effects_is_running(const xd_server *server, const char *id) {
    const xd_effects *effects = server->effects;
    if (effects == nullptr)
        return false;
    for (size_t i = 0; i < effects_count_impls(); i++) {
        if (strcmp(impls[i]->id, id) == 0)
            return effects->running[i].on;
    }
    return false;
}

bool xd_effects_animates_minimize(const xd_server *server) {
    const xd_effects *effects = server->effects;
    if (effects == nullptr)
        return false;
    for (size_t i = 0; i < effects_count_impls(); i++) {
        if (effects->running[i].on && impls[i]->animates_minimize)
            return true;
    }
    return false;
}

/* ---- input ---------------------------------------------------------------------- */

static uint32_t effects_get_modifiers(xd_server *server) {
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard == nullptr)
        return 0;
    uint32_t held = wlr_keyboard_get_modifiers(keyboard);
    uint32_t modifiers = 0;
    if (held & WLR_MODIFIER_SHIFT)
        modifiers |= XD_BINDING_SHIFT;
    if (held & WLR_MODIFIER_CTRL)
        modifiers |= XD_BINDING_CONTROL;
    if (held & WLR_MODIFIER_ALT)
        modifiers |= XD_BINDING_ALT;
    if (held & WLR_MODIFIER_LOGO)
        modifiers |= XD_BINDING_SUPER;
    return modifiers;
}

/* A press used up keeps its release from the programs too (they never
 * had the press), even if no effect claims it: whether the button's
 * release is used up. */
static bool effects_track_button(xd_effects *effects, const xd_effect_input *input, bool used) {
    bool held = false;
    for (size_t i = 0; i < effects->held_count; i++) {
        if (effects->held_buttons[i] == input->button) {
            /* Pressed again: its release went elsewhere (the screen
             * locked meanwhile), so this press starts afresh. */
            effects->held_buttons[i] = effects->held_buttons[--effects->held_count];
            held = true;
            break;
        }
    }
    if (!input->pressed)
        return used || held;
    if (used && effects->held_count < XD_ARRAY_LEN(effects->held_buttons))
        effects->held_buttons[effects->held_count++] = input->button;
    return used;
}

bool xd_effects_input(xd_server *server, xd_effect_input *input) {
    xd_effects *effects = server->effects;
    if (effects == nullptr)
        return false;
    input->modifiers = effects_get_modifiers(server);
    input->keysym = xkb_keysym_to_lower(input->keysym);
    /* Shift+Tab is Tab, as bindings name it. */
    if (input->keysym == XKB_KEY_ISO_Left_Tab)
        input->keysym = XKB_KEY_Tab;
    input->x = server->cursor->x;
    input->y = server->cursor->y;
    /* Moves and modifier changes are everyone's to see. */
    bool shared = input->kind == XD_EFFECT_INPUT_MOTION || input->kind == XD_EFFECT_INPUT_MODIFIERS;
    bool used = false;
    for (size_t i = 0; i < effects_count_impls(); i++) {
        if (!effects->running[i].on || impls[i]->input == nullptr)
            continue;
        if (impls[i]->input(server, effects->running[i].state, input)) {
            used = true;
            if (!shared)
                break;
        }
    }
    if (input->kind == XD_EFFECT_INPUT_BUTTON)
        used = effects_track_button(effects, input, used);
    return used;
}

bool xd_effects_animates_window(xd_view *view, xd_window_event_kind kind) {
    xd_server *server = view->server;
    xd_effects *effects = server->effects;
    if (effects == nullptr)
        return false;
    for (size_t i = 0; i < effects_count_impls(); i++) {
        if (effects->running[i].on && impls[i]->animates != nullptr &&
            impls[i]->animates(server, effects->running[i].state, view, kind))
            return true;
    }
    return false;
}

void xd_effects_get_binding(const xd_effects_settings *settings, size_t effect, const char *key,
                            xd_effect_binding *binding) {
    *binding = (xd_effect_binding){0};
    char name[64];
    uint32_t modifiers = 0;
    if (!xd_effects_parse_binding(xd_effects_get_text(settings, effect, key), &modifiers, name,
                                  sizeof(name)))
        return;
    binding->modifiers = modifiers;
    /* X11's numbering, as Compiz and the panel have it. */
    static const uint32_t buttons[] = {0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, 0, 0, 0, 0, BTN_SIDE,
                                       BTN_EXTRA};
    if (strncasecmp(name, "Button", 6) == 0 && name[6] >= '1' && name[6] <= '9' &&
        name[7] == '\0') {
        int number = name[6] - '0';
        if (number == 4 || number == 5)
            binding->wheel = number == 4 ? -1 : 1;
        else
            binding->button = buttons[number];
        return;
    }
    binding->keysym = xkb_keysym_to_lower(xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE));
}

bool xd_effects_binding_matches(const xd_effect_binding *binding, const xd_effect_input *input) {
    if (input->modifiers != binding->modifiers)
        return false;
    switch (input->kind) {
    case XD_EFFECT_INPUT_KEY:
        return input->pressed && binding->keysym != XKB_KEY_NoSymbol &&
               input->keysym == binding->keysym;
    case XD_EFFECT_INPUT_BUTTON:
        return input->pressed && binding->button != 0 && input->button == binding->button;
    case XD_EFFECT_INPUT_AXIS:
        return binding->wheel != 0 && (binding->wheel < 0) == (input->delta < 0) &&
               input->delta != 0;
    case XD_EFFECT_INPUT_MOTION:
    case XD_EFFECT_INPUT_MODIFIERS:
        return false;
    }
    return false;
}

bool xd_effects_is_binding_held(const xd_effect_binding *binding, const xd_effect_input *input) {
    bool none = binding->keysym == XKB_KEY_NoSymbol && binding->button == 0 && binding->wheel == 0;
    return !none && (input->modifiers & binding->modifiers) == binding->modifiers;
}

bool xd_effects_is_output_filtered([[maybe_unused]] const xd_output *output) {
#ifdef XD_SCENEFX
    return xd_glfx_is_output_filtered(output);
#else
    return false;
#endif
}

bool xd_effects_build_output_state([[maybe_unused]] xd_output *output,
                                   struct wlr_scene_output *scene_output,
                                   struct wlr_output_state *state) {
#ifdef XD_SCENEFX
    return xd_glfx_build_output_state(output, scene_output, state);
#else
    return wlr_scene_output_build_state(scene_output, state, nullptr);
#endif
}

bool xd_effects_match_view(const char *match, const xd_view *view) {
    return xd_effects_match_window(match, xd_view_app_id(view), xd_view_title(view),
                                   view->dialog);
}

struct wlr_scene_tree *xd_effects_create_overlay(xd_server *server) {
    struct wlr_scene_tree *tree = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_place_below(&tree->node, &server->layer_lock->node);
    return tree;
}

void xd_effects_schedule_frames(xd_server *server) {
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (out->wlr_output->enabled)
            wlr_output_schedule_frame(out->wlr_output);
    }
}
