/* SPDX-License-Identifier: MIT
 * xd-test-x11grab: an X11 program grabbing the keyboard and pointer, for
 * the UI tests, as X11 virtual machines and remote desktops do (VirtualBox,
 * VMware, rdesktop): a 400x300 green window that, once it has the focus,
 * grabs the keyboard (XGrabKeyboard, which Xwayland passes on as its
 * keyboard grab) and with --confine, once clicked, the pointer, confined
 * to itself (XGrabPointer's confine_to, which Xwayland turns into a
 * confined pointer). What it hears goes, a line each, to $XD_TEST_GRAB_LOG (else
 * stderr): X11 keycodes are evdev's plus 8.
 *
 *   xd-test-x11grab [--confine]
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static FILE *log_file;

[[gnu::format(printf, 1, 2)]] static void say(const char *fmt, ...) {
    FILE *f = log_file != nullptr ? log_file : stderr;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

static void set_string(xcb_connection_t *c, xcb_window_t w, xcb_atom_t prop, const char *s,
                       size_t len) {
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, prop, XCB_ATOM_STRING, 8, (uint32_t)len, s);
}

static void grab_keyboard(xcb_connection_t *c, xcb_window_t w) {
    xcb_grab_keyboard_reply_t *k = xcb_grab_keyboard_reply(
        c,
        xcb_grab_keyboard(c, 0, w, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
        nullptr);
    say("grab keyboard %d", k != nullptr ? k->status : -1);
    free(k);
}

static void grab_pointer(xcb_connection_t *c, xcb_window_t w) {
    uint16_t mask = XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS |
                    XCB_EVENT_MASK_BUTTON_RELEASE;
    xcb_grab_pointer_reply_t *p =
        xcb_grab_pointer_reply(c,
                               xcb_grab_pointer(c, 0, w, mask, XCB_GRAB_MODE_ASYNC,
                                                XCB_GRAB_MODE_ASYNC, w, XCB_NONE,
                                                XCB_CURRENT_TIME),
                               nullptr);
    say("grab pointer %d", p != nullptr ? p->status : -1);
    free(p);
}

int main(int argc, char **argv) {
    bool confine = argc > 1 && strcmp(argv[1], "--confine") == 0;
    const char *log = getenv("XD_TEST_GRAB_LOG");
    if (log != nullptr)
        log_file = fopen(log, "ae");
    xcb_connection_t *c = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "xd-test-x11grab: cannot connect to the X server\n");
        return EXIT_FAILURE;
    }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t w = xcb_generate_id(c);
    uint32_t values[] = {
        0x20a040,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_FOCUS_CHANGE |
            XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_BUTTON_PRESS,
    };
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, 400, 300, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    static const char title[] = "X11 grab test";
    static const char wm_class[] = "xd-test-x11grab\0XdTestX11Grab";
    set_string(c, w, XCB_ATOM_WM_NAME, title, sizeof(title) - 1);
    set_string(c, w, XCB_ATOM_WM_CLASS, wm_class, sizeof(wm_class));
    xcb_map_window(c, w);
    xcb_flush(c);

    bool keyboard = false, pointer = !confine;
    xcb_generic_event_t *e;
    while ((e = xcb_wait_for_event(c)) != nullptr) {
        switch (e->response_type & 0x7f) {
        case XCB_MAP_NOTIFY:
            say("ready");
            break;
        case XCB_FOCUS_IN:
            say("focus in");
            /* The keyboard once it has the focus. */
            if (!keyboard) {
                keyboard = true;
                grab_keyboard(c, w);
            }
            break;
        case XCB_FOCUS_OUT:
            say("focus out");
            break;
        case XCB_KEY_PRESS: {
            const xcb_key_press_event_t *k = (const xcb_key_press_event_t *)e;
            say("key %u pressed", k->detail);
            break;
        }
        case XCB_KEY_RELEASE: {
            const xcb_key_release_event_t *k = (const xcb_key_release_event_t *)e;
            say("key %u released", k->detail);
            break;
        }
        case XCB_MOTION_NOTIFY: {
            const xcb_motion_notify_event_t *m = (const xcb_motion_notify_event_t *)e;
            say("motion %d %d", m->event_x, m->event_y);
            break;
        }
        case XCB_BUTTON_PRESS: {
            const xcb_button_press_event_t *b = (const xcb_button_press_event_t *)e;
            say("button %u pressed", b->detail);
            /* The pointer when clicked, as VirtualBox captures the mouse:
             * Xwayland confines only a pointer that's over the window. */
            if (!pointer) {
                pointer = true;
                grab_pointer(c, w);
            }
            break;
        }
        default:
            break;
        }
        free(e);
        xcb_flush(c);
    }
    xcb_disconnect(c);
    if (log_file != nullptr)
        fclose(log_file);
    return EXIT_SUCCESS;
}
