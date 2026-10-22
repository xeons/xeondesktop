/* SPDX-License-Identifier: MIT
 * xd-test-ime: a stand-in input method for the UI tests, where neither
 * Fcitx 5 nor IBus need be installed. It speaks input-method-v2 as they
 * do: grabs the keyboard while a text box is active, types "日本語" for
 * the A key (a commit, after a preedit), passes every other key back
 * through its own virtual keyboard, and shows a candidate window (a
 * plain 120x30 box) at the caret. What it hears goes, a line each, to
 * $XD_TEST_IME_LOG.
 *
 *   xd-test-ime           be the input method (the compositor starts it:
 *                         XD_IME_COMMAND)
 *   xd-test-ime --probe   print which of the input method's globals this
 *                         client is offered, and exit
 */
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "../src/ui/protocols.h"

typedef struct ime {
    struct wl_display *display;
    struct wl_seat *seat;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwp_input_method_manager_v2 *manager;
    struct zwp_virtual_keyboard_manager_v1 *vk_manager;
    struct zwp_input_method_v2 *im;
    struct zwp_input_method_keyboard_grab_v2 *grab;
    struct zwp_virtual_keyboard_v1 *vk;
    struct wl_surface *popup_surface;
    struct zwp_input_popup_surface_v2 *popup;
    FILE *log;
    uint32_t serial; /* done events so far */
    bool pending_active, active;
    bool has_keymap;
    bool quit;
} ime;

[[gnu::format(printf, 2, 3)]] static void say(ime *input_method, const char *fmt, ...) {
    if (input_method->log == nullptr)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(input_method->log, fmt, ap);
    va_end(ap);
    fputc('\n', input_method->log);
    fflush(input_method->log);
}

/* ---- registry --------------------------------------------------------------- */

static void global_add(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                       uint32_t version) {
    ime *input_method = data;
    if (strcmp(iface, wl_seat_interface.name) == 0 && input_method->seat == nullptr)
        input_method->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (strcmp(iface, wl_compositor_interface.name) == 0)
        input_method->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        input_method->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0)
        input_method->manager =
            wl_registry_bind(reg, name, &zwp_input_method_manager_v2_interface, 1);
    else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        input_method->vk_manager =
            wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    (void)version;
}

static void global_remove([[maybe_unused]] void *data, [[maybe_unused]] struct wl_registry *reg,
                          [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global_add, global_remove};

/* ---- the candidate window ------------------------------------------------------ */

static void popup_rectangle(void *data, [[maybe_unused]] struct zwp_input_popup_surface_v2 *popup,
                            int32_t x, int32_t y, int32_t width, int32_t height) {
    say(data, "rectangle %d %d %d %d", x, y, width, height);
}

static const struct zwp_input_popup_surface_v2_listener popup_listener = {popup_rectangle};

enum { POPUP_W = 120, POPUP_H = 30 };

static void show_popup(ime *input_method) {
    if (input_method->popup != nullptr || input_method->compositor == nullptr ||
        input_method->shm == nullptr)
        return;
    int stride = POPUP_W * 4, size = stride * POPUP_H;
    int fd = memfd_create("xd-test-ime", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    uint32_t *px = mmap(nullptr, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return;
    }
    for (int i = 0; i < POPUP_W * POPUP_H; i++)
        px[i] = 0xffff00ff; /* magenta: easy to find in a screenshot */
    munmap(px, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(input_method->shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, POPUP_W, POPUP_H, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    input_method->popup_surface = wl_compositor_create_surface(input_method->compositor);
    input_method->popup =
        zwp_input_method_v2_get_input_popup_surface(input_method->im, input_method->popup_surface);
    zwp_input_popup_surface_v2_add_listener(input_method->popup, &popup_listener, input_method);
    wl_surface_attach(input_method->popup_surface, buffer, 0, 0);
    wl_surface_damage(input_method->popup_surface, 0, 0, POPUP_W, POPUP_H);
    wl_surface_commit(input_method->popup_surface);
}

/* ---- the keyboard grab ------------------------------------------------------- */

static void grab_keymap(void *data,
                        [[maybe_unused]] struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
                        uint32_t format, int32_t fd, uint32_t size) {
    ime *input_method = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    /* Our keyboard types with the same keymap as the real one. */
    if (input_method->vk == nullptr && input_method->vk_manager != nullptr)
        input_method->vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
            input_method->vk_manager, input_method->seat);
    if (input_method->vk != nullptr) {
        zwp_virtual_keyboard_v1_keymap(input_method->vk, format, fd, size);
        input_method->has_keymap = true;
    }
    close(fd);
    say(input_method, "keymap");
}

static void grab_key(void *data,
                     [[maybe_unused]] struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
                     [[maybe_unused]] uint32_t serial, uint32_t time, uint32_t key,
                     uint32_t state) {
    ime *input_method = data;
    if (key == KEY_A) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            /* Composed first, as a real input method shows its preedit;
             * then the result. */
            zwp_input_method_v2_set_preedit_string(input_method->im, "にほんご", 0, 12);
            zwp_input_method_v2_commit(input_method->im, input_method->serial);
            zwp_input_method_v2_commit_string(input_method->im, "日本語");
            zwp_input_method_v2_commit(input_method->im, input_method->serial);
            say(input_method, "committed");
        }
        return;
    }
    say(input_method, "forwarded %u %u", key, state);
    if (input_method->vk != nullptr && input_method->has_keymap)
        zwp_virtual_keyboard_v1_key(input_method->vk, time, key, state);
}

static void grab_modifiers(void *data,
                           [[maybe_unused]] struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
                           [[maybe_unused]] uint32_t serial, uint32_t depressed, uint32_t latched,
                           uint32_t locked, uint32_t group) {
    ime *input_method = data;
    if (input_method->vk != nullptr && input_method->has_keymap)
        zwp_virtual_keyboard_v1_modifiers(input_method->vk, depressed, latched, locked, group);
}

static void grab_repeat([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
                        [[maybe_unused]] int32_t rate, [[maybe_unused]] int32_t delay) {}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
    grab_keymap, grab_key, grab_modifiers, grab_repeat};

/* ---- the input method --------------------------------------------------------- */

static void im_activate(void *data, [[maybe_unused]] struct zwp_input_method_v2 *im) {
    ((ime *)data)->pending_active = true;
}

static void im_deactivate(void *data, [[maybe_unused]] struct zwp_input_method_v2 *im) {
    ((ime *)data)->pending_active = false;
}

static void im_surrounding(void *data, [[maybe_unused]] struct zwp_input_method_v2 *im,
                           const char *text, uint32_t cursor, uint32_t anchor) {
    say(data, "surrounding \"%s\" %u %u", text, cursor, anchor);
}

static void im_cause(void *data, [[maybe_unused]] struct zwp_input_method_v2 *im,
                     uint32_t cause) {
    say(data, "cause %u", cause);
}

static void im_content_type(void *data, [[maybe_unused]] struct zwp_input_method_v2 *im,
                            uint32_t hint, uint32_t purpose) {
    say(data, "content-type %u %u", hint, purpose);
}

static void im_done(void *data, [[maybe_unused]] struct zwp_input_method_v2 *im) {
    ime *input_method = data;
    input_method->serial++;
    if (input_method->pending_active == input_method->active)
        return;
    input_method->active = input_method->pending_active;
    say(input_method, "%s", input_method->active ? "activate" : "deactivate");
    /* As Fcitx 5 does: the keyboard while active, released after. */
    if (input_method->active && input_method->grab == nullptr) {
        input_method->grab = zwp_input_method_v2_grab_keyboard(input_method->im);
        zwp_input_method_keyboard_grab_v2_add_listener(input_method->grab, &grab_listener,
                                                       input_method);
        show_popup(input_method);
    } else if (!input_method->active && input_method->grab != nullptr) {
        zwp_input_method_keyboard_grab_v2_release(input_method->grab);
        input_method->grab = nullptr;
    }
}

static void im_unavailable(void *data, [[maybe_unused]] struct zwp_input_method_v2 *im) {
    ime *input_method = data;
    say(input_method, "unavailable");
    input_method->quit = true;
}

static const struct zwp_input_method_v2_listener im_listener = {
    im_activate, im_deactivate, im_surrounding, im_cause, im_content_type, im_done,
    im_unavailable};

int main(int argc, char **argv) {
    bool probe = argc > 1 && strcmp(argv[1], "--probe") == 0;
    ime input_method = {0};
    input_method.display = wl_display_connect(nullptr);
    if (input_method.display == nullptr) {
        fprintf(stderr, "xd-test-ime: cannot connect to the Wayland display\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(input_method.display);
    wl_registry_add_listener(reg, &registry_listener, &input_method);
    wl_display_roundtrip(input_method.display);
    if (probe) {
        printf("input-method %s\nvirtual-keyboard %s\n",
               input_method.manager != nullptr ? "yes" : "no",
               input_method.vk_manager != nullptr ? "yes" : "no");
        wl_display_disconnect(input_method.display);
        return EXIT_SUCCESS;
    }
    const char *log = getenv("XD_TEST_IME_LOG");
    if (log != nullptr)
        input_method.log = fopen(log, "ae");
    if (input_method.manager == nullptr || input_method.seat == nullptr) {
        say(&input_method, "no input method manager");
        fprintf(stderr, "xd-test-ime: the compositor offers no input method manager\n");
        return EXIT_FAILURE;
    }
    input_method.im =
        zwp_input_method_manager_v2_get_input_method(input_method.manager, input_method.seat);
    zwp_input_method_v2_add_listener(input_method.im, &im_listener, &input_method);
    say(&input_method, "ready%s",
        input_method.vk_manager != nullptr ? " with a virtual keyboard" : "");
    while (!input_method.quit && wl_display_dispatch(input_method.display) != -1) {
    }
    if (input_method.log != nullptr)
        fclose(input_method.log);
    wl_display_disconnect(input_method.display);
    return input_method.quit ? EXIT_FAILURE : EXIT_SUCCESS;
}
