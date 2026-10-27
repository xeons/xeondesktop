/* SPDX-License-Identifier: MIT
 * General Options in the compositor (both builds): Desktop Size made the
 * desktops' grid (desktops.c), and the keys that switch desktops or carry
 * the active window along. With one desktop the keys are left alone, for
 * the programs.
 */
#include "../desktops_grid.h"
#include "effects.h"

typedef enum general_direction : uint8_t {
    GENERAL_LEFT,
    GENERAL_RIGHT,
    GENERAL_UP,
    GENERAL_DOWN,
    GENERAL_DIRECTIONS,
} general_direction;

static const char *const switch_keys[GENERAL_DIRECTIONS] = {"SwitchLeft", "SwitchRight",
                                                            "SwitchUp", "SwitchDown"};
static const char *const move_keys[GENERAL_DIRECTIONS] = {"MoveLeft", "MoveRight", "MoveUp",
                                                          "MoveDown"};
static const int direction_columns[GENERAL_DIRECTIONS] = {-1, 1, 0, 0};
static const int direction_rows[GENERAL_DIRECTIONS] = {0, 0, -1, 1};

typedef struct general_state {
    xd_effect_binding switch_bindings[GENERAL_DIRECTIONS];
    xd_effect_binding move_bindings[GENERAL_DIRECTIONS];
} general_state;

static void *general_enable([[maybe_unused]] xd_server *server) {
    return XD_NEW(general_state);
}

static void general_disable(xd_server *server, void *data) {
    xd_desktops_set_grid(server, 1, 1, false);
    xd_free(data);
}

static void general_configure(xd_server *server, void *data, const xd_effects_settings *settings,
                              size_t effect) {
    general_state *state = data;
    for (size_t i = 0; i < GENERAL_DIRECTIONS; i++) {
        state->switch_bindings[i] = xd_effects_get_binding(settings, effect, switch_keys[i]);
        state->move_bindings[i] = xd_effects_get_binding(settings, effect, move_keys[i]);
    }
    xd_desktops_set_grid(server, xd_effects_get_int(settings, effect, "HorizontalSize"),
                         xd_effects_get_int(settings, effect, "VerticalSize"),
                         xd_effects_get_bool(settings, effect, "WrapAround"));
}

static bool general_handle_key(xd_server *server, void *data, uint32_t modifiers,
                               uint32_t keysym) {
    general_state *state = data;
    if (xd_desktops_get_count(server) < 2)
        return false;
    for (size_t i = 0; i < GENERAL_DIRECTIONS; i++) {
        bool switching = xd_effects_binding_matches_key(&state->switch_bindings[i], modifiers,
                                                        keysym);
        bool moving = xd_effects_binding_matches_key(&state->move_bindings[i], modifiers, keysym);
        if (!switching && !moving)
            continue;
        xd_view *carried = moving ? xd_view_focused(server) : nullptr;
        /* At the last desktop that way (no wrap-around), nothing more. */
        xd_desktops_switch_by(server, direction_columns[i], direction_rows[i], carried);
        return true;
    }
    return false;
}

const xd_effect_impl xd_effect_impl_general_options = {
    .id = "GeneralOptions",
    .enable = general_enable,
    .disable = general_disable,
    .configure = general_configure,
    .key = general_handle_key,
};
