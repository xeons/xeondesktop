/* SPDX-License-Identifier: MIT
 * Windows XP's accessibility features that need the compositor, beyond
 * StickyKeys and BounceKeys (input.c):
 *
 *   SlowKeys    FilterKeys' other half: a key counts only once it has
 *               been held for a while; released sooner it's ignored
 *   ToggleKeys  a high tone as Caps, Num or Scroll Lock goes on, a low one
 *               as it goes off
 *   MouseKeys   the numeric keypad moves the pointer and clicks
 *   ClickLock   a long press of the primary button holds it down until
 *               the next click
 *   shortcuts   Shift five times (StickyKeys), right Shift held 8 seconds
 *               (FilterKeys), Num Lock held 5 seconds (ToggleKeys), Left
 *               Alt+Left Shift+Num Lock (MouseKeys). They turn the feature
 *               on with a rising tone, or off with a falling one, and save
 *               it; turned on, xd-access asks whether to keep it, as XP's
 *               dialogs do.
 *
 *   SoundSentry a sound playing (the compositor's own: window sounds and
 *               the system bell) flashes the active window's caption,
 *               the active window or the desktop
 *   reset       Accessibility Options' "Turn off accessibility features
 *               after idle for": no input for so long turns them off
 *
 * And the system bell (xdg-system-bell-v1): the scheme's Default Beep.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "../core/alloc.h"
#include "../settings/soundplay.h"
#include "server.h"

typedef enum feature : uint8_t {
    FEATURE_STICKY,
    FEATURE_FILTER,
    FEATURE_TOGGLE,
    FEATURE_MOUSE,
} feature;

/* For xd-access --shortcut. */
static const char *const feature_names[] = {"stickykeys", "filterkeys", "togglekeys",
                                            "mousekeys"};

static constexpr int SHIFT_PRESSES = 5;
static constexpr uint32_t SHIFT_GAP_MS = 2000; /* a pause longer than this starts again */
static constexpr int FILTER_HOLD_MS = 8000;
static constexpr int TOGGLE_HOLD_MS = 5000;
static constexpr int MOUSE_TICK_MS = 20;
/* How long SoundSentry's flash lasts. */
static constexpr int SENTRY_FLASH_MS = 200;

struct xd_access {
    xd_server *server;
    struct wlr_xdg_system_bell_v1 *bell;
    struct wl_listener bell_ring;

    /* Shortcuts. */
    int shift_presses;
    uint32_t shift_msec;
    uint32_t hold_keycode; /* right Shift or Num Lock, being held */
    uint32_t swallowed;    /* +1: a shortcut's last key, whose release goes too */
    struct wl_event_source *hold_timer;
    xd_keyboard *numlock_kb; /* Num Lock to put back after MouseKeys' shortcut */
    bool numlock_was;
    struct wl_event_source *numlock_idle;
    bool scroll_lock; /* xkb keymaps rarely lock anything on Scroll Lock */

    /* SlowKeys: the key held back, and the ones accepted. */
    xd_keyboard *slow_kb;
    uint32_t slow_keycode; /* +1; 0 = none */
    struct wl_event_source *slow_timer;
    uint32_t accepted[8];
    size_t accepted_n;

    /* MouseKeys. */
    uint32_t mk_held; /* bit per direction key */
    int64_t mk_start; /* ms, when the first was pressed */
    double mk_fx, mk_fy; /* fractions of a pixel carried over */
    struct wl_event_source *mk_timer;
    bool mk_left, mk_right;        /* the buttons keys 5, + and 0 act on */
    bool mk_held_left, mk_held_right; /* held down by 0 (Ins) */
    uint32_t mk_keys_down[16];     /* keypad keys taken, so their releases are too */
    size_t mk_keys_down_n;

    /* ClickLock. */
    uint32_t press_msec;
    bool click_locked;
    bool unlocking; /* the click that ends the lock is under way */

    /* SoundSentry: the sound player's eventfd, and the flash showing. */
    int sentry_fd;
    struct wl_event_source *sentry_source, *sentry_timer;
    struct wlr_scene_rect *sentry_rect; /* the window or the desktop */
    xd_view *sentry_view;               /* its caption, drawn inactive */

    /* Automatic reset: the last input, and the timer that checks it. */
    int64_t last_input_ms;
    struct wl_event_source *reset_timer;
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool *feature_flag(xd_access_settings *access_settings, feature access_feature) {
    switch (access_feature) {
    case FEATURE_STICKY:
        return &access_settings->sticky_keys;
    case FEATURE_FILTER:
        return &access_settings->filter_keys;
    case FEATURE_TOGGLE:
        return &access_settings->toggle_keys;
    case FEATURE_MOUSE:
        return &access_settings->mouse_keys;
    }
    return &access_settings->sticky_keys;
}

/* Turns a feature on or off, from a shortcut: now, and in desktop.ini
 * (whose reload then changes nothing). */
static void set_feature(xd_server *server, feature access_feature, bool on) {
    xd_access_settings *access_settings = &server->config.access;
    *feature_flag(access_settings, access_feature) = on;
    xd_settings settings;
    xd_settings_load(&settings);
    *feature_flag(&settings.access, access_feature) = on;
    if (!xd_settings_save(&settings))
        wlr_log(WLR_ERROR, "cannot save the accessibility settings");
    wlr_log(WLR_INFO, "accessibility: %s turned %s by its shortcut", feature_names[access_feature],
            on ? "on" : "off");
    xd_input_apply_access(server);
    if (access_settings->sound_on_off)
        xd_sound_play_tone(on ? XD_TONE_RISE : XD_TONE_FALL);
    if (on && access_settings->warn && server->config.accessibility != nullptr) {
        static char opt[] = "--shortcut";
        char *argv[] = {(char *)(uintptr_t)server->config.accessibility, opt,
                        (char *)(uintptr_t)feature_names[access_feature], nullptr};
        xd_spawn_argv(argv);
    }
}

/* True if the shortcut is in use, and acted on. */
static bool shortcut(xd_server *server, feature access_feature) {
    const xd_access_settings *access_settings = &server->config.access;
    bool enabled = access_feature == FEATURE_STICKY   ? access_settings->sticky_shortcut
                   : access_feature == FEATURE_FILTER ? access_settings->filter_shortcut
                   : access_feature == FEATURE_TOGGLE ? access_settings->toggle_shortcut
                                                      : access_settings->mouse_keys_shortcut;
    if (enabled)
        set_feature(server, access_feature, !*feature_flag(&server->config.access, access_feature));
    return enabled;
}

static int hold_fired(void *data) {
    xd_access *acc = data;
    uint32_t key = acc->hold_keycode;
    acc->hold_keycode = 0;
    if (key == KEY_RIGHTSHIFT)
        (void)shortcut(acc->server, FEATURE_FILTER);
    else if (key == KEY_NUMLOCK)
        (void)shortcut(acc->server, FEATURE_TOGGLE);
    return 0;
}

static bool key_down(const xd_keyboard *kb, uint32_t keycode) {
    const struct wlr_keyboard *keyboard = kb->wlr_keyboard;
    for (size_t i = 0; i < keyboard->num_keycodes; i++) {
        if (keyboard->keycodes[i] == keycode)
            return true;
    }
    return false;
}

static uint32_t mod_mask(struct xkb_keymap *keymap, const char *name) {
    xkb_mod_index_t i = keymap != nullptr ? xkb_keymap_mod_get_index(keymap, name)
                                          : XKB_MOD_INVALID;
    return i != XKB_MOD_INVALID && i < 32 ? 1u << i : 0;
}

static bool num_lock_on(const xd_keyboard *kb) {
    const struct wlr_keyboard *keyboard = kb->wlr_keyboard;
    return (keyboard->modifiers.locked & mod_mask(keyboard->keymap, XKB_MOD_NAME_NUM)) != 0;
}

/* MouseKeys' shortcut leaves Num Lock as it was (the key still toggles
 * it in xkb, after we've seen it). */
static void restore_numlock(void *data) {
    xd_access *acc = data;
    acc->numlock_idle = nullptr;
    xd_keyboard *kb = acc->numlock_kb;
    acc->numlock_kb = nullptr;
    if (kb == nullptr)
        return;
    struct wlr_keyboard *keyboard = kb->wlr_keyboard;
    uint32_t num = mod_mask(keyboard->keymap, XKB_MOD_NAME_NUM);
    uint32_t locked =
        acc->numlock_was ? keyboard->modifiers.locked | num : keyboard->modifiers.locked & ~num;
    if (locked != keyboard->modifiers.locked)
        wlr_keyboard_notify_modifiers(keyboard, keyboard->modifiers.depressed,
                                      keyboard->modifiers.latched, locked,
                                      keyboard->modifiers.group);
}

/* Shift five times, the held keys, Left Alt+Left Shift+Num Lock. True if
 * the key is used up. */
static bool shortcut_key(xd_keyboard *kb, uint32_t keycode, bool pressed, uint32_t time_msec) {
    xd_access *acc = kb->server->access;
    if (!pressed) {
        if (acc->hold_keycode != 0 && keycode == acc->hold_keycode) {
            acc->hold_keycode = 0;
            wl_event_source_timer_update(acc->hold_timer, 0);
        }
        if (acc->swallowed == keycode + 1) {
            acc->swallowed = 0;
            return true;
        }
        return false;
    }
    if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
        if (acc->shift_presses > 0 && time_msec - acc->shift_msec > SHIFT_GAP_MS)
            acc->shift_presses = 0;
        acc->shift_msec = time_msec;
        if (++acc->shift_presses == SHIFT_PRESSES) {
            /* The fifth isn't StickyKeys' first. */
            acc->shift_presses = 0;
            if (shortcut(kb->server, FEATURE_STICKY)) {
                acc->swallowed = keycode + 1;
                return true;
            }
        }
    } else {
        acc->shift_presses = 0;
    }
    if (keycode == KEY_NUMLOCK && key_down(kb, KEY_LEFTALT) && key_down(kb, KEY_LEFTSHIFT) &&
        kb->server->config.access.mouse_keys_shortcut) {
        /* The key is ours: the shortcut, not a Num Lock press. */
        acc->hold_keycode = 0;
        wl_event_source_timer_update(acc->hold_timer, 0);
        acc->numlock_kb = kb;
        acc->numlock_was = num_lock_on(kb);
        if (acc->numlock_idle == nullptr)
            acc->numlock_idle =
                wl_event_loop_add_idle(kb->server->loop, restore_numlock, acc);
        acc->swallowed = keycode + 1;
        (void)shortcut(kb->server, FEATURE_MOUSE);
        return true;
    }
    /* Another key stops a hold. */
    if (keycode == KEY_RIGHTSHIFT || keycode == KEY_NUMLOCK) {
        acc->hold_keycode = keycode;
        wl_event_source_timer_update(acc->hold_timer,
                                     keycode == KEY_NUMLOCK ? TOGGLE_HOLD_MS : FILTER_HOLD_MS);
    } else if (acc->hold_keycode != 0) {
        acc->hold_keycode = 0;
        wl_event_source_timer_update(acc->hold_timer, 0);
    }
    return false;
}

/* ---- ToggleKeys -------------------------------------------------------------- */

static void toggle_tone(xd_keyboard *kb, xkb_keysym_t sym) {
    xd_access *acc = kb->server->access;
    const struct wlr_keyboard *keyboard = kb->wlr_keyboard;
    bool on;
    /* The state is still from before this key. */
    if (sym == XKB_KEY_Caps_Lock)
        on = !(keyboard->modifiers.locked & mod_mask(keyboard->keymap, XKB_MOD_NAME_CAPS));
    else if (sym == XKB_KEY_Num_Lock)
        on = !num_lock_on(kb);
    else if (sym == XKB_KEY_Scroll_Lock)
        on = acc->scroll_lock = !acc->scroll_lock;
    else
        return;
    if (kb->server->config.access.toggle_keys)
        xd_sound_play_tone(on ? XD_TONE_HIGH : XD_TONE_LOW);
}

/* ---- SlowKeys -------------------------------------------------------------------- */

static bool is_lock_or_modifier(xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_ISO_Level3_Shift:
    case XKB_KEY_Caps_Lock:
    case XKB_KEY_Num_Lock:
    case XKB_KEY_Scroll_Lock:
        return true;
    default:
        return false;
    }
}

static bool slow_keys_on(const xd_server *server) {
    const xd_access_settings *access_settings = &server->config.access;
    return access_settings->filter_keys && access_settings->slow_keys &&
           access_settings->slow_ms > 0;
}

static void drop_slow(xd_access *acc) {
    acc->slow_kb = nullptr;
    acc->slow_keycode = 0;
    wl_event_source_timer_update(acc->slow_timer, 0);
}

static int slow_accepted(void *data) {
    xd_access *acc = data;
    xd_keyboard *kb = acc->slow_kb;
    uint32_t keycode = acc->slow_keycode - 1;
    drop_slow(acc);
    if (kb == nullptr || acc->accepted_n == XD_ARRAY_LEN(acc->accepted))
        return 0;
    acc->accepted[acc->accepted_n++] = keycode;
    if (kb->server->config.access.filter_beep)
        xd_sound_play_tone(XD_TONE_CLICK);
    struct wlr_keyboard_key_event keyboard_key_event = {
        .time_msec = (uint32_t)now_ms(),
        .keycode = keycode,
        .state = WL_KEYBOARD_KEY_STATE_PRESSED,
    };
    xd_input_key(kb, &keyboard_key_event);
    return 0;
}

/* True if the key is held back or dropped. */
static bool slow_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *keyboard_key_event,
                     xkb_keysym_t sym) {
    xd_access *acc = kb->server->access;
    if (keyboard_key_event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        for (size_t i = 0; i < acc->accepted_n; i++) {
            if (acc->accepted[i] == keyboard_key_event->keycode) {
                acc->accepted[i] = acc->accepted[--acc->accepted_n];
                return false; /* it was let through: so is its release */
            }
        }
        if (acc->slow_kb == kb && acc->slow_keycode == keyboard_key_event->keycode + 1) {
            drop_slow(acc); /* let go too soon */
            return true;
        }
        return false;
    }
    if (!slow_keys_on(kb->server) || is_lock_or_modifier(sym))
        return false;
    /* A new key replaces one still held back. */
    acc->slow_kb = kb;
    acc->slow_keycode = keyboard_key_event->keycode + 1;
    wl_event_source_timer_update(acc->slow_timer, kb->server->config.access.slow_ms);
    if (kb->server->config.access.filter_beep)
        xd_sound_play_tone(XD_TONE_CLICK);
    return true;
}

/* ---- MouseKeys ------------------------------------------------------------------- */

/* The keypad's direction keys: bit, and which way. */
static const struct {
    uint32_t keycode;
    int dx, dy;
} directions[] = {
    {KEY_KP7, -1, -1}, {KEY_KP8, 0, -1}, {KEY_KP9, 1, -1}, {KEY_KP4, -1, 0},
    {KEY_KP6, 1, 0},   {KEY_KP1, -1, 1}, {KEY_KP2, 0, 1},  {KEY_KP3, 1, 1},
};

static bool mouse_key_code(uint32_t keycode) {
    for (size_t i = 0; i < XD_ARRAY_LEN(directions); i++) {
        if (directions[i].keycode == keycode)
            return true;
    }
    switch (keycode) {
    case KEY_KP5:
    case KEY_KPPLUS:
    case KEY_KP0:
    case KEY_KPDOT:
    case KEY_KPSLASH:
    case KEY_KPASTERISK:
    case KEY_KPMINUS:
        return true;
    default:
        return false;
    }
}

static void mk_move(xd_access *acc) {
    xd_server *server = acc->server;
    const xd_access_settings *access_settings = &server->config.access;
    int dx = 0, dy = 0;
    for (size_t i = 0; i < XD_ARRAY_LEN(directions); i++) {
        if (acc->mk_held & (1u << i)) {
            dx += directions[i].dx;
            dy += directions[i].dy;
        }
    }
    dx = XD_CLAMP(dx, -1, 1);
    dy = XD_CLAMP(dy, -1, 1);
    if (dx == 0 && dy == 0)
        return;
    /* From a tenth of the top speed up to it over the acceleration time;
     * Ctrl jumps to it, Shift creeps a pixel at a time. */
    double progress = (double)(now_ms() - acc->mk_start) / access_settings->mouse_keys_accel_ms;
    double speed = access_settings->mouse_keys_speed * XD_CLAMP(progress, 0.1, 1.0);
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    uint32_t mods = keyboard != nullptr ? wlr_keyboard_get_modifiers(keyboard) : 0;
    if (access_settings->mouse_keys_modifiers && (mods & WLR_MODIFIER_CTRL))
        speed = access_settings->mouse_keys_speed;
    double step = speed * MOUSE_TICK_MS / 1000.0;
    if (access_settings->mouse_keys_modifiers && (mods & WLR_MODIFIER_SHIFT))
        step = 1;
    acc->mk_fx += dx * step;
    acc->mk_fy += dy * step;
    double mx = trunc(acc->mk_fx), my = trunc(acc->mk_fy);
    acc->mk_fx -= mx;
    acc->mk_fy -= my;
    if (mx != 0 || my != 0)
        xd_input_move_pointer(server, mx, my, (uint32_t)now_ms());
}

static int mk_tick(void *data) {
    xd_access *acc = data;
    mk_move(acc);
    if (acc->mk_held != 0)
        wl_event_source_timer_update(acc->mk_timer, MOUSE_TICK_MS);
    return 0;
}

static void mk_click(xd_access *acc, uint32_t button, int times) {
    uint32_t time_msec = (uint32_t)now_ms();
    for (int i = 0; i < times; i++) {
        xd_input_button(acc->server, time_msec, button, WL_POINTER_BUTTON_STATE_PRESSED);
        xd_input_button(acc->server, time_msec, button, WL_POINTER_BUTTON_STATE_RELEASED);
    }
}

static void mk_buttons(xd_access *acc, int times) {
    if (acc->mk_left)
        mk_click(acc, BTN_LEFT, times);
    if (acc->mk_right)
        mk_click(acc, BTN_RIGHT, times);
}

static void mk_hold(xd_access *acc, uint32_t button, bool *held, bool down) {
    if (*held == down)
        return;
    *held = down;
    xd_input_button(acc->server, (uint32_t)now_ms(), button,
                    down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
}

/* Windows' keypad: 1-4 and 6-9 move, 5 clicks, + double-clicks, 0 (Ins)
 * holds the button down and . (Del) lets it go; / picks the left
 * button, - the right and * both. True if the key is used up. */
static bool mouse_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *keyboard_key_event) {
    xd_access *acc = kb->server->access;
    bool pressed = keyboard_key_event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (!pressed) {
        bool taken = false;
        for (size_t i = 0; i < acc->mk_keys_down_n; i++) {
            if (acc->mk_keys_down[i] == keyboard_key_event->keycode) {
                acc->mk_keys_down[i] = acc->mk_keys_down[--acc->mk_keys_down_n];
                taken = true;
                break;
            }
        }
        for (size_t i = 0; taken && i < XD_ARRAY_LEN(directions); i++) {
            if (directions[i].keycode == keyboard_key_event->keycode)
                acc->mk_held &= ~(1u << i);
        }
        return taken;
    }
    const xd_access_settings *access_settings = &kb->server->config.access;
    if (!access_settings->mouse_keys || !mouse_key_code(keyboard_key_event->keycode) ||
        num_lock_on(kb) != access_settings->mouse_keys_numlock || kb->server->locked)
        return false;
    bool seen = false;
    for (size_t i = 0; i < acc->mk_keys_down_n; i++)
        seen = seen || acc->mk_keys_down[i] == keyboard_key_event->keycode;
    if (!seen && acc->mk_keys_down_n < XD_ARRAY_LEN(acc->mk_keys_down))
        acc->mk_keys_down[acc->mk_keys_down_n++] = keyboard_key_event->keycode;
    for (size_t i = 0; i < XD_ARRAY_LEN(directions); i++) {
        if (directions[i].keycode != keyboard_key_event->keycode)
            continue;
        if (acc->mk_held == 0) {
            acc->mk_start = now_ms();
            acc->mk_fx = acc->mk_fy = 0;
            wl_event_source_timer_update(acc->mk_timer, MOUSE_TICK_MS);
        }
        acc->mk_held |= 1u << i;
        /* A tap moves a pixel. */
        xd_input_move_pointer(kb->server, directions[i].dx, directions[i].dy,
                              keyboard_key_event->time_msec);
        return true;
    }
    switch (keyboard_key_event->keycode) {
    case KEY_KP5:
        mk_buttons(acc, 1);
        break;
    case KEY_KPPLUS:
        mk_buttons(acc, 2);
        break;
    case KEY_KP0:
        if (acc->mk_left)
            mk_hold(acc, BTN_LEFT, &acc->mk_held_left, true);
        if (acc->mk_right)
            mk_hold(acc, BTN_RIGHT, &acc->mk_held_right, true);
        break;
    case KEY_KPDOT:
        mk_hold(acc, BTN_LEFT, &acc->mk_held_left, false);
        mk_hold(acc, BTN_RIGHT, &acc->mk_held_right, false);
        break;
    case KEY_KPSLASH:
        acc->mk_left = true;
        acc->mk_right = false;
        break;
    case KEY_KPMINUS:
        acc->mk_left = false;
        acc->mk_right = true;
        break;
    case KEY_KPASTERISK:
        acc->mk_left = acc->mk_right = true;
        break;
    default:
        break;
    }
    return true;
}

/* ---- the key hook ------------------------------------------------------------------ */

bool xd_access_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *keyboard_key_event) {
    xd_server *server = kb->server;
    xd_access *acc = server->access;
    if (acc == nullptr)
        return false;
    bool pressed = keyboard_key_event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = kb->wlr_keyboard->xkb_state != nullptr
                    ? xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state,
                                             keyboard_key_event->keycode + 8, &syms)
                    : 0;
    xkb_keysym_t sym = nsyms > 0 ? syms[0] : XKB_KEY_NoSymbol;

    if (shortcut_key(kb, keyboard_key_event->keycode, pressed, keyboard_key_event->time_msec))
        return true;
    const xd_access_settings *access_settings = &server->config.access;
    if (pressed) {
        toggle_tone(kb, sym);
        /* StickyKeys: a tone for each modifier; a modifier used with
         * another key, as without StickyKeys, turns it off. */
        bool modifier = is_lock_or_modifier(sym) && sym != XKB_KEY_Caps_Lock &&
                        sym != XKB_KEY_Num_Lock && sym != XKB_KEY_Scroll_Lock;
        if (access_settings->sticky_keys && access_settings->sticky_sound && modifier)
            xd_sound_play_tone(XD_TONE_CLICK);
        if (access_settings->sticky_keys && access_settings->sticky_two_keys_off && !modifier &&
            kb->sticky_alone != 0)
            set_feature(server, FEATURE_STICKY, false);
    }
    if (mouse_key(kb, keyboard_key_event))
        return true;
    return slow_key(kb, keyboard_key_event, sym);
}

/* ---- ClickLock ------------------------------------------------------------------- */

bool xd_access_button(xd_server *server, uint32_t button, bool pressed, uint32_t time_msec) {
    xd_access *acc = server->access;
    if (acc == nullptr || button != BTN_LEFT)
        return false;
    if (pressed) {
        if (acc->click_locked) {
            /* The click that ends the lock: its press is the held
             * button's, its release lets go. */
            acc->unlocking = true;
            return true;
        }
        acc->press_msec = time_msec;
        return false;
    }
    if (acc->unlocking) {
        acc->unlocking = false;
        acc->click_locked = false;
        return false;
    }
    const xd_mouse_settings *mouse = &server->config.mouse;
    if (mouse->click_lock && time_msec - acc->press_msec >= (uint32_t)mouse->click_lock_ms) {
        acc->click_locked = true;
        return true;
    }
    return false;
}

/* ---- SoundSentry -------------------------------------------------------------------- */

static bool view_alive(xd_server *server, const xd_view *view) {
    XD_LIST_FOR_EACH(candidate, &server->views, xd_view, link) {
        if (candidate == view)
            return true;
    }
    return false;
}

static void sentry_end(xd_access *acc) {
    if (acc->sentry_rect != nullptr) {
        wlr_scene_node_destroy(&acc->sentry_rect->node);
        acc->sentry_rect = nullptr;
    }
    /* The window may have gone in the meantime. */
    if (acc->sentry_view != nullptr && view_alive(acc->server, acc->sentry_view)) {
        acc->sentry_view->sentry_dim = false;
        xd_frame_render(acc->sentry_view, true);
    }
    acc->sentry_view = nullptr;
}

static int sentry_over(void *data) {
    sentry_end(data);
    return 0;
}

/* XP inverted the caption, window or screen; a dark veil stands in for
 * the inversion, which the scene graph can't do. */
static void sentry_veil(xd_access *acc, struct wlr_box box) {
    xd_server *server = acc->server;
    static const float veil[4] = {0, 0, 0, 0.5f};
    struct wlr_scene_tree *top = server->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY];
    acc->sentry_rect = wlr_scene_rect_create(top, box.width, box.height, veil);
    if (acc->sentry_rect == nullptr)
        return;
    wlr_scene_node_set_position(&acc->sentry_rect->node, box.x, box.y);
    wlr_scene_node_raise_to_top(&acc->sentry_rect->node);
}

static void sentry_flash(xd_access *acc) {
    xd_server *server = acc->server;
    const xd_access_settings *access_settings = &server->config.access;
    if (!access_settings->sound_sentry || access_settings->sound_sentry_effect == XD_SENTRY_NONE ||
        server->locked)
        return;
    sentry_end(acc); /* a new sound starts the flash over */
    xd_view *view = xd_view_focused(server);
    xd_sound_sentry effect = access_settings->sound_sentry_effect;
    /* No window (or no caption of ours): what there is. */
    if (effect != XD_SENTRY_DESKTOP && view == nullptr)
        effect = XD_SENTRY_DESKTOP;
    else if (effect == XD_SENTRY_CAPTION && !xd_view_framed(view))
        effect = XD_SENTRY_WINDOW;
    switch (effect) {
    case XD_SENTRY_CAPTION:
        view->sentry_dim = true;
        acc->sentry_view = view;
        xd_frame_render(view, true);
        break;
    case XD_SENTRY_WINDOW:
        sentry_veil(acc, xd_view_outer_box(view));
        break;
    case XD_SENTRY_DESKTOP: {
        struct wlr_box all;
        wlr_output_layout_get_box(server->output_layout, nullptr, &all);
        sentry_veil(acc, all);
        break;
    }
    case XD_SENTRY_NONE:
    case XD_SENTRY_COUNT:
        return;
    }
    wl_event_source_timer_update(acc->sentry_timer, SENTRY_FLASH_MS);
}

static int sentry_readable(int fd, [[maybe_unused]] uint32_t mask, void *data) {
    uint64_t event_count = 0;
    if (read(fd, &event_count, sizeof(event_count)) == (ssize_t)sizeof(event_count) &&
        event_count > 0)
        sentry_flash(data);
    return 0;
}

/* ---- automatic reset ------------------------------------------------------------- */

static bool any_feature_on(const xd_access_settings *access_settings) {
    return access_settings->sticky_keys || access_settings->filter_keys ||
           access_settings->toggle_keys || access_settings->mouse_keys ||
           access_settings->sound_sentry || access_settings->high_contrast;
}

/* XD_ACCESS_RESET_SECONDS shortens it for testing. */
static int64_t reset_after_ms(const xd_access_settings *access_settings) {
    const char *test = getenv("XD_ACCESS_RESET_SECONDS");
    long long secs = test != nullptr ? strtoll(test, nullptr, 10) : 0;
    return secs > 0 ? secs * 1000 : (int64_t)access_settings->auto_reset_minutes * 60 * 1000;
}

/* Sets the timer for when the reset would be due, or stops it. */
static void arm_reset(xd_access *acc) {
    const xd_access_settings *access_settings = &acc->server->config.access;
    if (!access_settings->auto_reset || !any_feature_on(access_settings)) {
        wl_event_source_timer_update(acc->reset_timer, 0);
        return;
    }
    int64_t left = acc->last_input_ms + reset_after_ms(access_settings) - now_ms();
    wl_event_source_timer_update(acc->reset_timer, (int)XD_CLAMP(left, 1, INT32_MAX));
}

/* Turns the features off (as XP's timeout did, High Contrast included),
 * now and in desktop.ini. */
static void reset_features(xd_server *server) {
    xd_settings settings;
    xd_settings_load(&settings);
    xd_access_settings *access_settings = &settings.access;
    access_settings->sticky_keys = access_settings->filter_keys = access_settings->toggle_keys =
        access_settings->mouse_keys = false;
    access_settings->sound_sentry = false;
    if (access_settings->high_contrast)
        xd_settings_set_high_contrast(&settings, false, "");
    if (!xd_settings_save(&settings))
        wlr_log(WLR_ERROR, "cannot save the accessibility settings");
    wlr_log(WLR_INFO, "accessibility: features turned off after %lld idle seconds",
            (long long)(reset_after_ms(&server->config.access) / 1000));
    /* The appearance follows when the compositor reloads the file. */
    xd_access_settings *now = &server->config.access;
    now->sticky_keys = now->filter_keys = now->toggle_keys = now->mouse_keys = false;
    now->sound_sentry = false;
    xd_input_apply_access(server);
    if (now->sound_on_off)
        xd_sound_play_tone(XD_TONE_FALL);
}

static int reset_due(void *data) {
    xd_access *acc = data;
    const xd_access_settings *access_settings = &acc->server->config.access;
    if (access_settings->auto_reset && any_feature_on(access_settings) &&
        now_ms() - acc->last_input_ms >= reset_after_ms(access_settings))
        reset_features(acc->server);
    else
        arm_reset(acc); /* there was input since: later */
    return 0;
}

void xd_access_activity(xd_server *server) {
    /* Only noted: the timer looks at it when it fires. */
    if (server->access != nullptr)
        server->access->last_input_ms = now_ms();
}

/* ---- setup ------------------------------------------------------------------------- */

static void handle_bell_ring([[maybe_unused]] struct wl_listener *listener,
                             [[maybe_unused]] void *data) {
    xd_sound_play_event(XD_SOUND_DEFAULT_BEEP);
}

void xd_access_apply(xd_server *server) {
    xd_access *acc = server->access;
    if (acc == nullptr)
        return;
    const xd_access_settings *access_settings = &server->config.access;
    if (!slow_keys_on(server)) {
        drop_slow(acc);
        acc->accepted_n = 0;
    }
    if (!access_settings->mouse_keys) {
        acc->mk_held = 0;
        mk_hold(acc, BTN_LEFT, &acc->mk_held_left, false);
        mk_hold(acc, BTN_RIGHT, &acc->mk_held_right, false);
    }
    if (!server->config.mouse.click_lock && acc->click_locked) {
        acc->click_locked = false;
        acc->unlocking = false;
        xd_input_button(server, (uint32_t)now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    }
    if (!access_settings->sound_sentry)
        sentry_end(acc);
    arm_reset(acc);
}

void xd_access_keyboard_gone(xd_keyboard *kb) {
    xd_access *acc = kb->server->access;
    if (acc == nullptr)
        return;
    if (acc->slow_kb == kb)
        drop_slow(acc);
    if (acc->numlock_kb == kb)
        acc->numlock_kb = nullptr;
}

void xd_access_init(xd_server *server) {
    xd_access *acc = XD_NEW(xd_access);
    acc->server = server;
    acc->mk_left = true;
    acc->hold_timer = wl_event_loop_add_timer(server->loop, hold_fired, acc);
    acc->slow_timer = wl_event_loop_add_timer(server->loop, slow_accepted, acc);
    acc->mk_timer = wl_event_loop_add_timer(server->loop, mk_tick, acc);
    acc->bell = wlr_xdg_system_bell_v1_create(server->display, 1);
    XD_LISTEN(&acc->bell->events.ring, acc, bell_ring, handle_bell_ring);
    acc->sentry_timer = wl_event_loop_add_timer(server->loop, sentry_over, acc);
    acc->sentry_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (acc->sentry_fd >= 0) {
        acc->sentry_source = wl_event_loop_add_fd(server->loop, acc->sentry_fd, WL_EVENT_READABLE,
                                                  sentry_readable, acc);
        xd_sound_notify_fd(acc->sentry_fd);
    }
    acc->reset_timer = wl_event_loop_add_timer(server->loop, reset_due, acc);
    acc->last_input_ms = now_ms();
    server->access = acc;
    arm_reset(acc);
}

void xd_access_finish(xd_server *server) {
    xd_access *acc = server->access;
    if (acc == nullptr)
        return;
    xd_unlisten(&acc->bell_ring);
    sentry_end(acc);
    xd_sound_notify_fd(-1);
    if (acc->sentry_source != nullptr)
        wl_event_source_remove(acc->sentry_source);
    if (acc->sentry_fd >= 0)
        close(acc->sentry_fd);
    wl_event_source_remove(acc->sentry_timer);
    wl_event_source_remove(acc->reset_timer);
    wl_event_source_remove(acc->hold_timer);
    wl_event_source_remove(acc->slow_timer);
    wl_event_source_remove(acc->mk_timer);
    if (acc->numlock_idle != nullptr)
        wl_event_source_remove(acc->numlock_idle);
    xd_free(acc);
    server->access = nullptr;
}
