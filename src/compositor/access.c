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
 * And the system bell (xdg-system-bell-v1): the scheme's Default Beep.
 */
#include <linux/input-event-codes.h>
#include <math.h>
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
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool *feature_flag(xd_access_settings *a, feature f) {
    switch (f) {
    case FEATURE_STICKY:
        return &a->sticky_keys;
    case FEATURE_FILTER:
        return &a->filter_keys;
    case FEATURE_TOGGLE:
        return &a->toggle_keys;
    case FEATURE_MOUSE:
        return &a->mouse_keys;
    }
    return &a->sticky_keys;
}

/* Turns a feature on or off, from a shortcut: now, and in desktop.ini
 * (whose reload then changes nothing). */
static void set_feature(xd_server *server, feature f, bool on) {
    xd_access_settings *a = &server->config.access;
    *feature_flag(a, f) = on;
    xd_settings s;
    xd_settings_load(&s);
    *feature_flag(&s.access, f) = on;
    if (!xd_settings_save(&s))
        wlr_log(WLR_ERROR, "cannot save the accessibility settings");
    wlr_log(WLR_INFO, "accessibility: %s turned %s by its shortcut", feature_names[f],
            on ? "on" : "off");
    xd_input_apply_access(server);
    if (a->sound_on_off)
        xd_sound_play_tone(on ? XD_TONE_RISE : XD_TONE_FALL);
    if (on && a->warn && server->config.accessibility != nullptr) {
        static char opt[] = "--shortcut";
        char *argv[] = {(char *)(uintptr_t)server->config.accessibility, opt,
                        (char *)(uintptr_t)feature_names[f], nullptr};
        xd_spawn_argv(argv);
    }
}

/* True if the shortcut is in use, and acted on. */
static bool shortcut(xd_server *server, feature f) {
    const xd_access_settings *a = &server->config.access;
    bool enabled = f == FEATURE_STICKY   ? a->sticky_shortcut
                   : f == FEATURE_FILTER ? a->filter_shortcut
                   : f == FEATURE_TOGGLE ? a->toggle_shortcut
                                         : a->mouse_keys_shortcut;
    if (enabled)
        set_feature(server, f, !*feature_flag(&server->config.access, f));
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
    const struct wlr_keyboard *k = kb->wlr_keyboard;
    for (size_t i = 0; i < k->num_keycodes; i++) {
        if (k->keycodes[i] == keycode)
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
    const struct wlr_keyboard *k = kb->wlr_keyboard;
    return (k->modifiers.locked & mod_mask(k->keymap, XKB_MOD_NAME_NUM)) != 0;
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
    struct wlr_keyboard *k = kb->wlr_keyboard;
    uint32_t num = mod_mask(k->keymap, XKB_MOD_NAME_NUM);
    uint32_t locked = acc->numlock_was ? k->modifiers.locked | num : k->modifiers.locked & ~num;
    if (locked != k->modifiers.locked)
        wlr_keyboard_notify_modifiers(k, k->modifiers.depressed, k->modifiers.latched, locked,
                                      k->modifiers.group);
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
    const struct wlr_keyboard *k = kb->wlr_keyboard;
    bool on;
    /* The state is still from before this key. */
    if (sym == XKB_KEY_Caps_Lock)
        on = !(k->modifiers.locked & mod_mask(k->keymap, XKB_MOD_NAME_CAPS));
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
    const xd_access_settings *a = &server->config.access;
    return a->filter_keys && a->slow_keys && a->slow_ms > 0;
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
    struct wlr_keyboard_key_event e = {
        .time_msec = (uint32_t)now_ms(),
        .keycode = keycode,
        .state = WL_KEYBOARD_KEY_STATE_PRESSED,
    };
    xd_input_key(kb, &e);
    return 0;
}

/* True if the key is held back or dropped. */
static bool slow_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *e, xkb_keysym_t sym) {
    xd_access *acc = kb->server->access;
    if (e->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        for (size_t i = 0; i < acc->accepted_n; i++) {
            if (acc->accepted[i] == e->keycode) {
                acc->accepted[i] = acc->accepted[--acc->accepted_n];
                return false; /* it was let through: so is its release */
            }
        }
        if (acc->slow_kb == kb && acc->slow_keycode == e->keycode + 1) {
            drop_slow(acc); /* let go too soon */
            return true;
        }
        return false;
    }
    if (!slow_keys_on(kb->server) || is_lock_or_modifier(sym))
        return false;
    /* A new key replaces one still held back. */
    acc->slow_kb = kb;
    acc->slow_keycode = e->keycode + 1;
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
    const xd_access_settings *a = &server->config.access;
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
    double t = (double)(now_ms() - acc->mk_start) / a->mouse_keys_accel_ms;
    double speed = a->mouse_keys_speed * XD_CLAMP(t, 0.1, 1.0);
    struct wlr_keyboard *k = wlr_seat_get_keyboard(server->seat);
    uint32_t mods = k != nullptr ? wlr_keyboard_get_modifiers(k) : 0;
    if (a->mouse_keys_modifiers && (mods & WLR_MODIFIER_CTRL))
        speed = a->mouse_keys_speed;
    double step = speed * MOUSE_TICK_MS / 1000.0;
    if (a->mouse_keys_modifiers && (mods & WLR_MODIFIER_SHIFT))
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
    uint32_t t = (uint32_t)now_ms();
    for (int i = 0; i < times; i++) {
        xd_input_button(acc->server, t, button, WL_POINTER_BUTTON_STATE_PRESSED);
        xd_input_button(acc->server, t, button, WL_POINTER_BUTTON_STATE_RELEASED);
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
static bool mouse_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *e) {
    xd_access *acc = kb->server->access;
    bool pressed = e->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (!pressed) {
        bool taken = false;
        for (size_t i = 0; i < acc->mk_keys_down_n; i++) {
            if (acc->mk_keys_down[i] == e->keycode) {
                acc->mk_keys_down[i] = acc->mk_keys_down[--acc->mk_keys_down_n];
                taken = true;
                break;
            }
        }
        for (size_t i = 0; taken && i < XD_ARRAY_LEN(directions); i++) {
            if (directions[i].keycode == e->keycode)
                acc->mk_held &= ~(1u << i);
        }
        return taken;
    }
    const xd_access_settings *a = &kb->server->config.access;
    if (!a->mouse_keys || !mouse_key_code(e->keycode) ||
        num_lock_on(kb) != a->mouse_keys_numlock || kb->server->locked)
        return false;
    bool seen = false;
    for (size_t i = 0; i < acc->mk_keys_down_n; i++)
        seen = seen || acc->mk_keys_down[i] == e->keycode;
    if (!seen && acc->mk_keys_down_n < XD_ARRAY_LEN(acc->mk_keys_down))
        acc->mk_keys_down[acc->mk_keys_down_n++] = e->keycode;
    for (size_t i = 0; i < XD_ARRAY_LEN(directions); i++) {
        if (directions[i].keycode != e->keycode)
            continue;
        if (acc->mk_held == 0) {
            acc->mk_start = now_ms();
            acc->mk_fx = acc->mk_fy = 0;
            wl_event_source_timer_update(acc->mk_timer, MOUSE_TICK_MS);
        }
        acc->mk_held |= 1u << i;
        /* A tap moves a pixel. */
        xd_input_move_pointer(kb->server, directions[i].dx, directions[i].dy, e->time_msec);
        return true;
    }
    switch (e->keycode) {
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

bool xd_access_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *e) {
    xd_server *server = kb->server;
    xd_access *acc = server->access;
    if (acc == nullptr)
        return false;
    bool pressed = e->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = kb->wlr_keyboard->xkb_state != nullptr
                    ? xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, e->keycode + 8, &syms)
                    : 0;
    xkb_keysym_t sym = nsyms > 0 ? syms[0] : XKB_KEY_NoSymbol;

    if (shortcut_key(kb, e->keycode, pressed, e->time_msec))
        return true;
    const xd_access_settings *a = &server->config.access;
    if (pressed) {
        toggle_tone(kb, sym);
        /* StickyKeys: a tone for each modifier; a modifier used with
         * another key, as without StickyKeys, turns it off. */
        bool modifier = is_lock_or_modifier(sym) && sym != XKB_KEY_Caps_Lock &&
                        sym != XKB_KEY_Num_Lock && sym != XKB_KEY_Scroll_Lock;
        if (a->sticky_keys && a->sticky_sound && modifier)
            xd_sound_play_tone(XD_TONE_CLICK);
        if (a->sticky_keys && a->sticky_two_keys_off && !modifier && kb->sticky_alone != 0)
            set_feature(server, FEATURE_STICKY, false);
    }
    if (mouse_key(kb, e))
        return true;
    return slow_key(kb, e, sym);
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
    const xd_mouse_settings *m = &server->config.mouse;
    if (m->click_lock && time_msec - acc->press_msec >= (uint32_t)m->click_lock_ms) {
        acc->click_locked = true;
        return true;
    }
    return false;
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
    const xd_access_settings *a = &server->config.access;
    if (!slow_keys_on(server)) {
        drop_slow(acc);
        acc->accepted_n = 0;
    }
    if (!a->mouse_keys) {
        acc->mk_held = 0;
        mk_hold(acc, BTN_LEFT, &acc->mk_held_left, false);
        mk_hold(acc, BTN_RIGHT, &acc->mk_held_right, false);
    }
    if (!server->config.mouse.click_lock && acc->click_locked) {
        acc->click_locked = false;
        acc->unlocking = false;
        xd_input_button(server, (uint32_t)now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    }
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
    server->access = acc;
}

void xd_access_finish(xd_server *server) {
    xd_access *acc = server->access;
    if (acc == nullptr)
        return;
    xd_unlisten(&acc->bell_ring);
    wl_event_source_remove(acc->hold_timer);
    wl_event_source_remove(acc->slow_timer);
    wl_event_source_remove(acc->mk_timer);
    if (acc->numlock_idle != nullptr)
        wl_event_source_remove(acc->numlock_idle);
    xd_free(acc);
    server->access = nullptr;
}
