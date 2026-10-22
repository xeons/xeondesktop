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
    FILE *file = log_file != nullptr ? log_file : stderr;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(file, fmt, ap);
    va_end(ap);
    fputc('\n', file);
    fflush(file);
}

static void set_string(xcb_connection_t *connection, xcb_window_t xcb_window, xcb_atom_t prop,
                       const char *text, size_t len) {
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcb_window, prop, XCB_ATOM_STRING, 8,
                        (uint32_t)len, text);
}

static void grab_keyboard(xcb_connection_t *connection, xcb_window_t xcb_window) {
    xcb_grab_keyboard_reply_t *reply =
        xcb_grab_keyboard_reply(connection,
                                xcb_grab_keyboard(connection, 0, xcb_window, XCB_CURRENT_TIME,
                                                  XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
                                nullptr);
    say("grab keyboard %d", reply != nullptr ? reply->status : -1);
    free(reply);
}

static void grab_pointer(xcb_connection_t *connection, xcb_window_t xcb_window) {
    uint16_t mask = XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS |
                    XCB_EVENT_MASK_BUTTON_RELEASE;
    xcb_grab_pointer_reply_t *reply = xcb_grab_pointer_reply(
        connection,
        xcb_grab_pointer(connection, 0, xcb_window, mask, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         xcb_window, XCB_NONE, XCB_CURRENT_TIME),
        nullptr);
    say("grab pointer %d", reply != nullptr ? reply->status : -1);
    free(reply);
}

int main(int argc, char **argv) {
    bool confine = argc > 1 && strcmp(argv[1], "--confine") == 0;
    const char *log = getenv("XD_TEST_GRAB_LOG");
    if (log != nullptr)
        log_file = fopen(log, "ae");
    xcb_connection_t *connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "xd-test-x11grab: cannot connect to the X server\n");
        return EXIT_FAILURE;
    }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    xcb_window_t xcb_window = xcb_generate_id(connection);
    uint32_t values[] = {
        0x20a040,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_FOCUS_CHANGE |
            XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_BUTTON_PRESS,
    };
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, xcb_window, screen->root, 0, 0, 400, 300, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    static const char title[] = "X11 grab test";
    static const char wm_class[] = "xd-test-x11grab\0XdTestX11Grab";
    set_string(connection, xcb_window, XCB_ATOM_WM_NAME, title, sizeof(title) - 1);
    set_string(connection, xcb_window, XCB_ATOM_WM_CLASS, wm_class, sizeof(wm_class));
    xcb_map_window(connection, xcb_window);
    xcb_flush(connection);

    bool keyboard = false, pointer = !confine;
    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(connection)) != nullptr) {
        switch (event->response_type & 0x7f) {
        case XCB_MAP_NOTIFY:
            say("ready");
            break;
        case XCB_FOCUS_IN:
            say("focus in");
            /* The keyboard once it has the focus. */
            if (!keyboard) {
                keyboard = true;
                grab_keyboard(connection, xcb_window);
            }
            break;
        case XCB_FOCUS_OUT:
            say("focus out");
            break;
        case XCB_KEY_PRESS: {
            const xcb_key_press_event_t *key_press = (const xcb_key_press_event_t *)event;
            say("key %u pressed", key_press->detail);
            break;
        }
        case XCB_KEY_RELEASE: {
            const xcb_key_release_event_t *key_release = (const xcb_key_release_event_t *)event;
            say("key %u released", key_release->detail);
            break;
        }
        case XCB_MOTION_NOTIFY: {
            const xcb_motion_notify_event_t *motion = (const xcb_motion_notify_event_t *)event;
            say("motion %d %d", motion->event_x, motion->event_y);
            break;
        }
        case XCB_BUTTON_PRESS: {
            const xcb_button_press_event_t *button_press = (const xcb_button_press_event_t *)event;
            say("button %u pressed", button_press->detail);
            /* The pointer when clicked, as VirtualBox captures the mouse:
             * Xwayland confines only a pointer that's over the window. */
            if (!pointer) {
                pointer = true;
                grab_pointer(connection, xcb_window);
            }
            break;
        }
        default:
            break;
        }
        free(event);
        xcb_flush(connection);
    }
    xcb_disconnect(connection);
    if (log_file != nullptr)
        fclose(log_file);
    return EXIT_SUCCESS;
}
