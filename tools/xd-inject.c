/* SPDX-License-Identifier: MIT
 * xd-inject: types and clicks into an XeonDesktop session, for UI tests.
 *
 * Needs a compositor started with --allow-virtual-input. Steps run in
 * order:
 *
 *   move X Y          pointer to logical desktop coordinates
 *   rmove DX DY       pointer moved by DX, DY, as a mouse moves it (relative)
 *   click [BUTTON]    press and release (left, right, middle)
 *   down|up [BUTTON]  press or release only
 *   wheel N           scroll N notches (positive = down)
 *   type TEXT         type characters using the session's keymap
 *   key COMBO         e.g. Return, ctrl+alt+t, alt+F4, super
 *   hold|release MOD  keep ctrl, alt, shift or super down across steps
 *   press|lift KEY    hold any key down (key repeat), e.g. press Down
 *   sleep MS          wait
 *   lid close|open    the laptop lid (xd-window-arranger-v1's test_lid)
 *
 * And, through xd-test-input-v1, a touchscreen, a drawing tablet and a
 * touchpad, plugged in while xd-inject runs:
 *
 *   touch down|move ID X Y   a finger (ID: any number) lands or moves
 *   touch up|cancel ID       it lifts, or the screen gives up on it
 *   pen in|move X Y [P [TX TY]]  the pen comes near or moves; P pressure
 *                     (0 to 1), TX TY tilt in degrees (kept for later moves)
 *   pen down|up|out   its tip touches or lifts; it goes out of reach
 *   pen press|lift N  barrel button 1 or 2
 *   pad press|lift N  a pad button (from 0)
 *   pad ring|strip N POS  a finger on a ring (degrees) or strip (0 to 1)
 *   swipe|pinch|rest begin FINGERS  a touchpad gesture starts (rest:
 *                     fingers held still, a hold gesture)
 *   swipe update DX DY, pinch update DX DY SCALE DEGREES
 *   swipe|pinch|rest end [cancel]
 *
 * Example: xd-inject move 20 728 click sleep 500 type alacritty key Return
 */
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "../src/core/alloc.h"
#include "../src/core/str.h"
#include "../src/ui/protocols.h"

typedef struct output {
    struct wl_output *wl;
    struct zxdg_output_v1 *xdg;
    int x, y, w, h;
} output;

typedef struct key_entry {
    xkb_keysym_t sym;
    xkb_keycode_t code;
    bool shift;
} key_entry;

typedef struct inject {
    struct wl_display *display;
    struct wl_seat *seat;
    struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
    struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct xd_window_arranger_v1 *arranger; /* for the lid, which has no virtual device */
    struct xd_test_input_v1 *test_input; /* touch, pens, gestures: bound only for those */
    uint32_t test_input_name;
    double pen_pressure, pen_tilt_x, pen_tilt_y;
    struct zwlr_virtual_pointer_v1 *pointer;
    struct zwp_virtual_keyboard_v1 *keyboard;
    output outputs[16];
    size_t output_count;

    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    /* Follows our own keys, for the layout (group) the switching keys
     * select when the keymap has several ([Keyboard] Layout=us,de). */
    struct xkb_state *state;
    key_entry *keys;
    size_t key_count;
    uint32_t mod_shift, mod_ctrl, mod_alt, mod_super;
    uint32_t held; /* modifiers kept down by "hold" */
} inject;

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

/* Lets the compositor process what we sent before the next step. */
static void settle(inject *in) {
    wl_display_roundtrip(in->display);
    sleep_ms(25);
}

/* ---- globals and outputs ------------------------------------------------------ */

static void xdg_pos(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y) {
    (void)o;
    output *out = data;
    out->x = x;
    out->y = y;
}
static void xdg_size(void *data, struct zxdg_output_v1 *o, int32_t w, int32_t h) {
    (void)o;
    output *out = data;
    out->w = w;
    out->h = h;
}
static void xdg_done([[maybe_unused]] void *d, [[maybe_unused]] struct zxdg_output_v1 *o) {}
static void xdg_name([[maybe_unused]] void *d, [[maybe_unused]] struct zxdg_output_v1 *o,
                     [[maybe_unused]] const char *n) {}
static void xdg_desc([[maybe_unused]] void *d, [[maybe_unused]] struct zxdg_output_v1 *o,
                     [[maybe_unused]] const char *n) {}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_pos,
    .logical_size = xdg_size,
    .done = xdg_done,
    .name = xdg_name,
    .description = xdg_desc,
};

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                   uint32_t version) {
    inject *in = data;
    xd_str i = xd_str_from_cstr(iface);
    if (xd_str_eq(i, xd_str_from_cstr(wl_seat_interface.name)) && in->seat == nullptr) {
        in->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (xd_str_eq(i, xd_str_from_cstr(zwlr_virtual_pointer_manager_v1_interface.name))) {
        in->pointer_manager = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface,
                                               XD_MIN(version, 2u));
    } else if (xd_str_eq(i, xd_str_from_cstr(zwp_virtual_keyboard_manager_v1_interface.name))) {
        in->keyboard_manager =
            wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (xd_str_eq(i, xd_str_from_cstr(zxdg_output_manager_v1_interface.name))) {
        in->xdg_output_manager =
            wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, XD_MIN(version, 3u));
    } else if (xd_str_eq(i, xd_str_from_cstr(xd_window_arranger_v1_interface.name)) &&
               version >= XD_WINDOW_ARRANGER_V1_TEST_LID_SINCE_VERSION) {
        in->arranger = wl_registry_bind(reg, name, &xd_window_arranger_v1_interface,
                                        XD_WINDOW_ARRANGER_V1_TEST_LID_SINCE_VERSION);
    } else if (xd_str_eq(i, xd_str_from_cstr(xd_test_input_v1_interface.name))) {
        in->test_input_name = name;
    } else if (xd_str_eq(i, xd_str_from_cstr(wl_output_interface.name)) &&
               in->output_count < XD_ARRAY_LEN(in->outputs)) {
        in->outputs[in->output_count++].wl = wl_registry_bind(reg, name, &wl_output_interface, 1);
    }
}

static void global_remove([[maybe_unused]] void *data, [[maybe_unused]] struct wl_registry *reg,
                          [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global, global_remove};

/* Bounding box of all outputs: virtual pointer absolute coordinates are
 * relative to it. */
static void layout_extent(const inject *in, int *x0, int *y0, int *w, int *h) {
    int minx = 0;
    int miny = 0;
    int maxx = 1;
    int maxy = 1;
    for (size_t i = 0; i < in->output_count; i++) {
        const output *o = &in->outputs[i];
        if (i == 0 || o->x < minx)
            minx = o->x;
        if (i == 0 || o->y < miny)
            miny = o->y;
        if (i == 0 || o->x + o->w > maxx)
            maxx = o->x + o->w;
        if (i == 0 || o->y + o->h > maxy)
            maxy = o->y + o->h;
    }
    *x0 = minx;
    *y0 = miny;
    *w = XD_MAX(maxx - minx, 1);
    *h = XD_MAX(maxy - miny, 1);
}

/* ---- keyboard ----------------------------------------------------------------- */

static bool setup_keyboard(inject *in) {
    in->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    in->keymap = in->xkb != nullptr
                     ? xkb_keymap_new_from_names(in->xkb, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS)
                     : nullptr;
    if (in->keymap == nullptr)
        return false;
    in->state = xkb_state_new(in->keymap);
    if (in->state == nullptr)
        return false;

    /* Send the keymap so the compositor interprets our keycodes as we do. */
    char *text = xkb_keymap_get_as_string(in->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (text == nullptr)
        return false;
    size_t size = strlen(text) + 1;
    int fd = memfd_create("xd-inject-keymap", MFD_CLOEXEC);
    bool ok = fd >= 0 && write(fd, text, size) == (ssize_t)size;
    free(text);
    if (!ok) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    zwp_virtual_keyboard_v1_keymap(in->keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                   (uint32_t)size);
    close(fd);

    /* keysym -> (keycode, needs Shift) from levels 0 and 1 of group 0. */
    xkb_keycode_t lo = xkb_keymap_min_keycode(in->keymap);
    xkb_keycode_t hi = xkb_keymap_max_keycode(in->keymap);
    in->keys = xd_alloc_array((size_t)(hi - lo + 1) * 2, sizeof(*in->keys));
    for (xkb_keycode_t code = lo; code <= hi; code++) {
        for (xkb_level_index_t level = 0; level < 2; level++) {
            const xkb_keysym_t *syms = nullptr;
            int n = xkb_keymap_key_get_syms_by_level(in->keymap, code, 0, level, &syms);
            if (n == 1 && syms[0] != XKB_KEY_NoSymbol)
                in->keys[in->key_count++] = (key_entry){syms[0], code, level == 1};
        }
    }
    xkb_mod_index_t idx;
    if ((idx = xkb_keymap_mod_get_index(in->keymap, XKB_MOD_NAME_SHIFT)) != XKB_MOD_INVALID)
        in->mod_shift = 1u << idx;
    if ((idx = xkb_keymap_mod_get_index(in->keymap, XKB_MOD_NAME_CTRL)) != XKB_MOD_INVALID)
        in->mod_ctrl = 1u << idx;
    if ((idx = xkb_keymap_mod_get_index(in->keymap, XKB_MOD_NAME_ALT)) != XKB_MOD_INVALID)
        in->mod_alt = 1u << idx;
    if ((idx = xkb_keymap_mod_get_index(in->keymap, XKB_MOD_NAME_LOGO)) != XKB_MOD_INVALID)
        in->mod_super = 1u << idx;
    return true;
}

static const key_entry *find_key(const inject *in, xkb_keysym_t sym) {
    /* Prefer the unshifted level when both have it. */
    const key_entry *best = nullptr;
    for (size_t i = 0; i < in->key_count; i++) {
        if (in->keys[i].sym == sym && (best == nullptr || (best->shift && !in->keys[i].shift)))
            best = &in->keys[i];
    }
    return best;
}

static void send_key(inject *in, xkb_keycode_t code, bool pressed) {
    /* The protocol takes evdev codes: XKB keycodes are offset by 8. */
    zwp_virtual_keyboard_v1_key(in->keyboard, now_ms(), code - 8,
                                pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                        : WL_KEYBOARD_KEY_STATE_RELEASED);
    xkb_state_update_key(in->state, code, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

static void send_mods(inject *in, uint32_t depressed) {
    zwp_virtual_keyboard_v1_modifiers(
        in->keyboard, depressed, 0, 0,
        xkb_state_serialize_layout(in->state, XKB_STATE_LAYOUT_LOCKED));
}

/* Presses a key with modifiers held, then releases everything. */
static bool press_combo(inject *in, uint32_t mods, xkb_keysym_t sym) {
    const key_entry *k = find_key(in, sym);
    if (k == nullptr)
        return false;
    if (k->shift)
        mods |= in->mod_shift;
    mods &= ~in->held; /* already down */
    struct {
        uint32_t mask;
        xkb_keysym_t sym;
    } held[] = {
        {in->mod_ctrl, XKB_KEY_Control_L},
        {in->mod_alt, XKB_KEY_Alt_L},
        {in->mod_shift, XKB_KEY_Shift_L},
        {in->mod_super, XKB_KEY_Super_L},
    };
    uint32_t state = in->held;
    for (size_t i = 0; i < XD_ARRAY_LEN(held); i++) {
        if (!(mods & held[i].mask))
            continue;
        const key_entry *m = find_key(in, held[i].sym);
        if (m != nullptr)
            send_key(in, m->code, true);
        state |= held[i].mask;
        send_mods(in, state);
    }
    send_key(in, k->code, true);
    settle(in);
    send_key(in, k->code, false);
    for (size_t i = XD_ARRAY_LEN(held); i-- > 0;) {
        if (!(mods & held[i].mask))
            continue;
        const key_entry *m = find_key(in, held[i].sym);
        if (m != nullptr)
            send_key(in, m->code, false);
        state &= ~held[i].mask;
        send_mods(in, state);
    }
    settle(in);
    return true;
}

static bool type_text(inject *in, const char *text) {
    xd_str s = xd_str_from_cstr(text);
    for (size_t i = 0; i < s.len;) {
        /* Decode one UTF-8 character. */
        unsigned char c = (unsigned char)s.ptr[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.len) {
            cp = (uint32_t)(c & 0x1F) << 6 | ((unsigned char)s.ptr[i + 1] & 0x3F);
            n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.len) {
            cp = (uint32_t)(c & 0x0F) << 12 | (uint32_t)((unsigned char)s.ptr[i + 1] & 0x3F) << 6 |
                 ((unsigned char)s.ptr[i + 2] & 0x3F);
            n = 3;
        } else {
            fprintf(stderr, "xd-inject: can't type byte 0x%02x\n", c);
            return false;
        }
        xkb_keysym_t sym = xkb_utf32_to_keysym(cp);
        if (!press_combo(in, 0, sym)) {
            fprintf(stderr, "xd-inject: no key types U+%04X in this keymap\n", cp);
            return false;
        }
        i += n;
    }
    return true;
}

/* "hold ctrl" / "release ctrl": keeps a modifier down across later steps
 * (Ctrl+click, Alt+drag). */
static bool hold_modifier(inject *in, const char *name, bool down) {
    static const struct {
        const char *name;
        xkb_keysym_t sym;
    } mods[] = {
        {"ctrl", XKB_KEY_Control_L},
        {"alt", XKB_KEY_Alt_L},
        {"shift", XKB_KEY_Shift_L},
        {"super", XKB_KEY_Super_L},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(mods); i++) {
        if (!xd_str_eq_nocase(xd_str_from_cstr(name), xd_str_from_cstr(mods[i].name)))
            continue;
        uint32_t mask = i == 0 ? in->mod_ctrl : i == 1 ? in->mod_alt : i == 2 ? in->mod_shift
                                                                      : in->mod_super;
        const key_entry *k = find_key(in, mods[i].sym);
        if (k == nullptr)
            return false;
        if (down == ((in->held & mask) != 0))
            return true;
        send_key(in, k->code, down);
        in->held = down ? in->held | mask : in->held & ~mask;
        send_mods(in, in->held);
        settle(in);
        return true;
    }
    fprintf(stderr, "xd-inject: unknown modifier %s\n", name);
    return false;
}

/* "press Down" / "lift Down": one key held across later steps. */
static bool press_key(inject *in, const char *name, bool down) {
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    const key_entry *k = sym != XKB_KEY_NoSymbol ? find_key(in, sym) : nullptr;
    if (k == nullptr) {
        fprintf(stderr, "xd-inject: unknown key %s\n", name);
        return false;
    }
    send_key(in, k->code, down);
    settle(in);
    return true;
}

/* "ctrl+alt+F4" -> modifiers and keysym. */
static bool key_combo(inject *in, const char *spec) {
    uint32_t mods = 0;
    xd_str rest = xd_str_from_cstr(spec);
    for (;;) {
        size_t plus;
        bool more = xd_str_find_char(rest, '+', &plus) && plus + 1 < rest.len;
        xd_str part = more ? xd_str_sub(rest, 0, plus) : rest;
        if (more) {
            if (xd_str_eq_nocase(part, XD_STR("ctrl")))
                mods |= in->mod_ctrl;
            else if (xd_str_eq_nocase(part, XD_STR("alt")))
                mods |= in->mod_alt;
            else if (xd_str_eq_nocase(part, XD_STR("shift")))
                mods |= in->mod_shift;
            else if (xd_str_eq_nocase(part, XD_STR("super")))
                mods |= in->mod_super;
            else {
                fprintf(stderr, "xd-inject: unknown modifier in %s\n", spec);
                return false;
            }
            rest = xd_str_sub(rest, plus + 1, rest.len);
            continue;
        }
        char *name = xd_str_dup_cstr(part);
        xkb_keysym_t sym = xd_str_eq_nocase(part, XD_STR("super"))
                               ? XKB_KEY_Super_L
                               : xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
        xd_free(name);
        if (sym == XKB_KEY_NoSymbol) {
            fprintf(stderr, "xd-inject: unknown key in %s\n", spec);
            return false;
        }
        return press_combo(in, mods, sym);
    }
}

/* ---- pointer ------------------------------------------------------------------- */

static uint32_t button_code(const char *name) {
    if (name == nullptr || strcmp(name, "left") == 0)
        return BTN_LEFT;
    if (strcmp(name, "right") == 0)
        return BTN_RIGHT;
    if (strcmp(name, "middle") == 0)
        return BTN_MIDDLE;
    return 0;
}

static void button(inject *in, uint32_t code, bool pressed) {
    zwlr_virtual_pointer_v1_button(in->pointer, now_ms(), code,
                                   pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                           : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(in->pointer);
    settle(in);
}

static void move(inject *in, int x, int y) {
    int x0;
    int y0;
    int w;
    int h;
    layout_extent(in, &x0, &y0, &w, &h);
    uint32_t px = (uint32_t)XD_CLAMP(x - x0, 0, w - 1);
    uint32_t py = (uint32_t)XD_CLAMP(y - y0, 0, h - 1);
    zwlr_virtual_pointer_v1_motion_absolute(in->pointer, now_ms(), px, py, (uint32_t)w,
                                            (uint32_t)h);
    zwlr_virtual_pointer_v1_frame(in->pointer);
    settle(in);
}

/* As a mouse moves (what a locked or confined pointer hears). */
static void move_by(inject *in, int dx, int dy) {
    zwlr_virtual_pointer_v1_motion(in->pointer, now_ms(), wl_fixed_from_int(dx),
                                   wl_fixed_from_int(dy));
    zwlr_virtual_pointer_v1_frame(in->pointer);
    settle(in);
}

static bool parse_int(const char *s, int64_t lo, int64_t hi, int64_t *out) {
    return s != nullptr && xd_str_parse_i64(xd_str_from_cstr(s), lo, hi, out);
}

static bool parse_num(const char *s, double *out) {
    return s != nullptr && xd_str_parse_double(xd_str_from_cstr(s), out);
}

/* ---- touch, pen and touchpad (xd-test-input-v1) ----------------------------------- */

static const char *const test_input_steps[] = {"touch", "pen", "pad", "swipe", "pinch", "rest"};

static bool is_test_input_step(const char *cmd) {
    for (size_t i = 0; i < XD_ARRAY_LEN(test_input_steps); i++) {
        if (strcmp(cmd, test_input_steps[i]) == 0)
            return true;
    }
    return false;
}

static bool needs_test_input(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (is_test_input_step(argv[i]))
            return true;
    }
    return false;
}

static wl_fixed_t fx(double v) { return wl_fixed_from_double(v); }

/* One touch, pen, pad or gesture step at argv[*i]; false if it's bad. */
static bool test_input_step(inject *in, int argc, char **argv, int *i) {
    struct xd_test_input_v1 *t = in->test_input;
    const char *cmd = argv[*i];
    const char *what = *i + 1 < argc ? argv[*i + 1] : "";
    /* The numbers after the step's words. */
    double n[5] = {0};
    int count = 0;
    int first = *i + 2;
    while (count < (int)XD_ARRAY_LEN(n) && first + count < argc &&
           parse_num(argv[first + count], &n[count]))
        count++;
    int used = 0;
    if (strcmp(cmd, "touch") == 0) {
        int32_t id = (int32_t)n[0];
        if ((strcmp(what, "down") == 0 || strcmp(what, "move") == 0) && count >= 3) {
            if (what[0] == 'd')
                xd_test_input_v1_touch_down(t, id, fx(n[1]), fx(n[2]));
            else
                xd_test_input_v1_touch_motion(t, id, fx(n[1]), fx(n[2]));
            used = 3;
        } else if ((strcmp(what, "up") == 0 || strcmp(what, "cancel") == 0) && count >= 1) {
            if (what[0] == 'u')
                xd_test_input_v1_touch_up(t, id);
            else
                xd_test_input_v1_touch_cancel(t, id);
            used = 1;
        } else {
            return false;
        }
    } else if (strcmp(cmd, "pen") == 0) {
        if ((strcmp(what, "in") == 0 || strcmp(what, "move") == 0) && count >= 2) {
            used = count >= 5 ? 5 : count >= 3 ? 3 : 2;
            if (used >= 3)
                in->pen_pressure = n[2];
            if (used >= 5) {
                in->pen_tilt_x = n[3];
                in->pen_tilt_y = n[4];
            }
            if (what[0] == 'i')
                xd_test_input_v1_tool_proximity(t, fx(n[0]), fx(n[1]), 1);
            xd_test_input_v1_tool_motion(t, fx(n[0]), fx(n[1]), fx(in->pen_pressure),
                                         fx(in->pen_tilt_x), fx(in->pen_tilt_y));
        } else if (strcmp(what, "down") == 0 || strcmp(what, "up") == 0) {
            xd_test_input_v1_tool_tip(t, what[0] == 'd');
        } else if (strcmp(what, "out") == 0) {
            xd_test_input_v1_tool_proximity(t, 0, 0, 0);
        } else if ((strcmp(what, "press") == 0 || strcmp(what, "lift") == 0) && count >= 1 &&
                   (n[0] == 1 || n[0] == 2)) {
            xd_test_input_v1_tool_button(t, n[0] == 1 ? BTN_STYLUS : BTN_STYLUS2,
                                         what[0] == 'p');
            used = 1;
        } else {
            return false;
        }
    } else if (strcmp(cmd, "pad") == 0) {
        if ((strcmp(what, "press") == 0 || strcmp(what, "lift") == 0) && count >= 1) {
            xd_test_input_v1_pad_button(t, (uint32_t)n[0], what[0] == 'p');
            used = 1;
        } else if ((strcmp(what, "ring") == 0 || strcmp(what, "strip") == 0) && count >= 2) {
            if (what[0] == 'r')
                xd_test_input_v1_pad_ring(t, (uint32_t)n[0], fx(n[1]));
            else
                xd_test_input_v1_pad_strip(t, (uint32_t)n[0], fx(n[1]));
            used = 2;
        } else {
            return false;
        }
    } else {
        /* swipe, pinch, rest (a hold) */
        char kind = cmd[0];
        if (strcmp(what, "begin") == 0 && count >= 1) {
            uint32_t fingers = (uint32_t)n[0];
            if (kind == 's')
                xd_test_input_v1_swipe_begin(t, fingers);
            else if (kind == 'p')
                xd_test_input_v1_pinch_begin(t, fingers);
            else
                xd_test_input_v1_hold_begin(t, fingers);
            used = 1;
        } else if (strcmp(what, "update") == 0 && kind == 's' && count >= 2) {
            xd_test_input_v1_swipe_update(t, fx(n[0]), fx(n[1]));
            used = 2;
        } else if (strcmp(what, "update") == 0 && kind == 'p' && count >= 4) {
            xd_test_input_v1_pinch_update(t, fx(n[0]), fx(n[1]), fx(n[2]), fx(n[3]));
            used = 4;
        } else if (strcmp(what, "end") == 0) {
            bool cancel = *i + 2 < argc && strcmp(argv[*i + 2], "cancel") == 0;
            if (kind == 's')
                xd_test_input_v1_swipe_end(t, cancel);
            else if (kind == 'p')
                xd_test_input_v1_pinch_end(t, cancel);
            else
                xd_test_input_v1_hold_end(t, cancel);
            *i += cancel ? 1 : 0;
        } else {
            return false;
        }
    }
    *i += 1 + used;
    settle(in);
    return true;
}

static int run_steps(inject *in, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *cmd = argv[i];
        const char *a1 = i + 1 < argc ? argv[i + 1] : nullptr;
        const char *a2 = i + 2 < argc ? argv[i + 2] : nullptr;
        int64_t v1;
        int64_t v2;
        if (strcmp(cmd, "move") == 0 && parse_int(a1, -100000, 100000, &v1) &&
            parse_int(a2, -100000, 100000, &v2)) {
            move(in, (int)v1, (int)v2);
            i += 2;
        } else if (strcmp(cmd, "rmove") == 0 && parse_int(a1, -100000, 100000, &v1) &&
                   parse_int(a2, -100000, 100000, &v2)) {
            move_by(in, (int)v1, (int)v2);
            i += 2;
        } else if (strcmp(cmd, "click") == 0 || strcmp(cmd, "down") == 0 ||
                   strcmp(cmd, "up") == 0) {
            uint32_t code = button_code(a1);
            if (code != 0 && a1 != nullptr)
                i++;
            else
                code = BTN_LEFT;
            if (strcmp(cmd, "up") != 0)
                button(in, code, true);
            if (strcmp(cmd, "down") != 0)
                button(in, code, false);
        } else if (strcmp(cmd, "wheel") == 0 && parse_int(a1, -100, 100, &v1)) {
            zwlr_virtual_pointer_v1_axis_source(in->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
            zwlr_virtual_pointer_v1_axis_discrete(in->pointer, now_ms(),
                                                  WL_POINTER_AXIS_VERTICAL_SCROLL,
                                                  wl_fixed_from_int((int)v1 * 15), (int)v1);
            zwlr_virtual_pointer_v1_frame(in->pointer);
            settle(in);
            i++;
        } else if (strcmp(cmd, "type") == 0 && a1 != nullptr) {
            if (!type_text(in, a1))
                return EXIT_FAILURE;
            i++;
        } else if (strcmp(cmd, "key") == 0 && a1 != nullptr) {
            if (!key_combo(in, a1))
                return EXIT_FAILURE;
            i++;
        } else if ((strcmp(cmd, "hold") == 0 || strcmp(cmd, "release") == 0) && a1 != nullptr) {
            if (!hold_modifier(in, a1, strcmp(cmd, "hold") == 0))
                return EXIT_FAILURE;
            i++;
        } else if ((strcmp(cmd, "press") == 0 || strcmp(cmd, "lift") == 0) && a1 != nullptr) {
            if (!press_key(in, a1, strcmp(cmd, "press") == 0))
                return EXIT_FAILURE;
            i++;
        } else if (strcmp(cmd, "lid") == 0 && a1 != nullptr &&
                   (strcmp(a1, "close") == 0 || strcmp(a1, "open") == 0)) {
            if (in->arranger == nullptr) {
                fprintf(stderr, "xd-inject: the compositor offers no lid to toggle\n");
                return EXIT_FAILURE;
            }
            xd_window_arranger_v1_test_lid(in->arranger, strcmp(a1, "close") == 0);
            settle(in);
            i++;
        } else if (is_test_input_step(cmd) && in->test_input != nullptr) {
            if (!test_input_step(in, argc, argv, &i)) {
                fprintf(stderr, "xd-inject: bad %s step\n", cmd);
                return EXIT_FAILURE;
            }
        } else if (strcmp(cmd, "sleep") == 0 && parse_int(a1, 0, 600000, &v1)) {
            sleep_ms((long)v1);
            i++;
        } else {
            fprintf(stderr, "xd-inject: bad step at '%s'\n", cmd);
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    static inject in;
    in.display = wl_display_connect(nullptr);
    if (in.display == nullptr) {
        fprintf(stderr, "xd-inject: no Wayland compositor (is WAYLAND_DISPLAY set?)\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(in.display);
    wl_registry_add_listener(reg, &registry_listener, &in);
    wl_display_roundtrip(in.display);
    if (in.pointer_manager == nullptr || in.keyboard_manager == nullptr || in.seat == nullptr) {
        fprintf(stderr, "xd-inject: the compositor doesn't allow virtual input "
                        "(start xd-compositor with --allow-virtual-input)\n");
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < in.output_count && in.xdg_output_manager != nullptr; i++) {
        in.outputs[i].xdg =
            zxdg_output_manager_v1_get_xdg_output(in.xdg_output_manager, in.outputs[i].wl);
        zxdg_output_v1_add_listener(in.outputs[i].xdg, &xdg_output_listener, &in.outputs[i]);
    }
    if (needs_test_input(argc, argv)) {
        if (in.test_input_name == 0) {
            fprintf(stderr, "xd-inject: the compositor offers no test touchscreen, tablet or "
                            "touchpad (start xd-compositor with --allow-virtual-input)\n");
            return EXIT_FAILURE;
        }
        in.test_input = wl_registry_bind(reg, in.test_input_name, &xd_test_input_v1_interface, 1);
    }
    in.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(in.pointer_manager, in.seat);
    in.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(in.keyboard_manager,
                                                                           in.seat);
    wl_display_roundtrip(in.display);
    if (!setup_keyboard(&in)) {
        fprintf(stderr, "xd-inject: cannot set up a keymap\n");
        return EXIT_FAILURE;
    }
    settle(&in);
    /* Programs take the seat's new touch capability and the new tablet
     * before the first touch. */
    if (in.test_input != nullptr)
        sleep_ms(300);

    int rc = run_steps(&in, argc, argv);

    zwlr_virtual_pointer_v1_destroy(in.pointer);
    zwp_virtual_keyboard_v1_destroy(in.keyboard);
    if (in.arranger != nullptr)
        xd_window_arranger_v1_destroy(in.arranger);
    if (in.test_input != nullptr)
        xd_test_input_v1_destroy(in.test_input);
    wl_display_roundtrip(in.display);
    xd_free(in.keys);
    xkb_state_unref(in.state);
    xkb_keymap_unref(in.keymap);
    xkb_context_unref(in.xkb);
    wl_display_disconnect(in.display);
    return rc;
}
