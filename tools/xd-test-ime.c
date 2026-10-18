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

[[gnu::format(printf, 2, 3)]] static void say(ime *m, const char *fmt, ...) {
    if (m->log == nullptr)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(m->log, fmt, ap);
    va_end(ap);
    fputc('\n', m->log);
    fflush(m->log);
}

/* ---- registry --------------------------------------------------------------- */

static void global_add(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                       uint32_t version) {
    ime *m = data;
    if (strcmp(iface, wl_seat_interface.name) == 0 && m->seat == nullptr)
        m->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (strcmp(iface, wl_compositor_interface.name) == 0)
        m->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        m->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0)
        m->manager = wl_registry_bind(reg, name, &zwp_input_method_manager_v2_interface, 1);
    else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        m->vk_manager = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    (void)version;
}

static void global_remove([[maybe_unused]] void *data, [[maybe_unused]] struct wl_registry *reg,
                          [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global_add, global_remove};

/* ---- the candidate window ------------------------------------------------------ */

static void popup_rectangle(void *data, [[maybe_unused]] struct zwp_input_popup_surface_v2 *p,
                            int32_t x, int32_t y, int32_t w, int32_t h) {
    say(data, "rectangle %d %d %d %d", x, y, w, h);
}

static const struct zwp_input_popup_surface_v2_listener popup_listener = {popup_rectangle};

enum { POPUP_W = 120, POPUP_H = 30 };

static void show_popup(ime *m) {
    if (m->popup != nullptr || m->compositor == nullptr || m->shm == nullptr)
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
    struct wl_shm_pool *pool = wl_shm_create_pool(m->shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, POPUP_W, POPUP_H, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    m->popup_surface = wl_compositor_create_surface(m->compositor);
    m->popup = zwp_input_method_v2_get_input_popup_surface(m->im, m->popup_surface);
    zwp_input_popup_surface_v2_add_listener(m->popup, &popup_listener, m);
    wl_surface_attach(m->popup_surface, buffer, 0, 0);
    wl_surface_damage(m->popup_surface, 0, 0, POPUP_W, POPUP_H);
    wl_surface_commit(m->popup_surface);
}

/* ---- the keyboard grab ------------------------------------------------------- */

static void grab_keymap(void *data, [[maybe_unused]] struct zwp_input_method_keyboard_grab_v2 *g,
                        uint32_t format, int32_t fd, uint32_t size) {
    ime *m = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    /* Our keyboard types with the same keymap as the real one. */
    if (m->vk == nullptr && m->vk_manager != nullptr)
        m->vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(m->vk_manager, m->seat);
    if (m->vk != nullptr) {
        zwp_virtual_keyboard_v1_keymap(m->vk, format, fd, size);
        m->has_keymap = true;
    }
    close(fd);
    say(m, "keymap");
}

static void grab_key(void *data, [[maybe_unused]] struct zwp_input_method_keyboard_grab_v2 *g,
                     [[maybe_unused]] uint32_t serial, uint32_t time, uint32_t key,
                     uint32_t state) {
    ime *m = data;
    if (key == KEY_A) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            /* Composed first, as a real input method shows its preedit;
             * then the result. */
            zwp_input_method_v2_set_preedit_string(m->im, "にほんご", 0, 12);
            zwp_input_method_v2_commit(m->im, m->serial);
            zwp_input_method_v2_commit_string(m->im, "日本語");
            zwp_input_method_v2_commit(m->im, m->serial);
            say(m, "committed");
        }
        return;
    }
    say(m, "forwarded %u %u", key, state);
    if (m->vk != nullptr && m->has_keymap)
        zwp_virtual_keyboard_v1_key(m->vk, time, key, state);
}

static void grab_modifiers(void *data,
                           [[maybe_unused]] struct zwp_input_method_keyboard_grab_v2 *g,
                           [[maybe_unused]] uint32_t serial, uint32_t depressed, uint32_t latched,
                           uint32_t locked, uint32_t group) {
    ime *m = data;
    if (m->vk != nullptr && m->has_keymap)
        zwp_virtual_keyboard_v1_modifiers(m->vk, depressed, latched, locked, group);
}

static void grab_repeat([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwp_input_method_keyboard_grab_v2 *g,
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
    ime *m = data;
    m->serial++;
    if (m->pending_active == m->active)
        return;
    m->active = m->pending_active;
    say(m, "%s", m->active ? "activate" : "deactivate");
    /* As Fcitx 5 does: the keyboard while active, released after. */
    if (m->active && m->grab == nullptr) {
        m->grab = zwp_input_method_v2_grab_keyboard(m->im);
        zwp_input_method_keyboard_grab_v2_add_listener(m->grab, &grab_listener, m);
        show_popup(m);
    } else if (!m->active && m->grab != nullptr) {
        zwp_input_method_keyboard_grab_v2_release(m->grab);
        m->grab = nullptr;
    }
}

static void im_unavailable(void *data, [[maybe_unused]] struct zwp_input_method_v2 *im) {
    ime *m = data;
    say(m, "unavailable");
    m->quit = true;
}

static const struct zwp_input_method_v2_listener im_listener = {
    im_activate, im_deactivate, im_surrounding, im_cause, im_content_type, im_done,
    im_unavailable};

int main(int argc, char **argv) {
    bool probe = argc > 1 && strcmp(argv[1], "--probe") == 0;
    ime m = {0};
    m.display = wl_display_connect(nullptr);
    if (m.display == nullptr) {
        fprintf(stderr, "xd-test-ime: cannot connect to the Wayland display\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(m.display);
    wl_registry_add_listener(reg, &registry_listener, &m);
    wl_display_roundtrip(m.display);
    if (probe) {
        printf("input-method %s\nvirtual-keyboard %s\n", m.manager != nullptr ? "yes" : "no",
               m.vk_manager != nullptr ? "yes" : "no");
        wl_display_disconnect(m.display);
        return EXIT_SUCCESS;
    }
    const char *log = getenv("XD_TEST_IME_LOG");
    if (log != nullptr)
        m.log = fopen(log, "ae");
    if (m.manager == nullptr || m.seat == nullptr) {
        say(&m, "no input method manager");
        fprintf(stderr, "xd-test-ime: the compositor offers no input method manager\n");
        return EXIT_FAILURE;
    }
    m.im = zwp_input_method_manager_v2_get_input_method(m.manager, m.seat);
    zwp_input_method_v2_add_listener(m.im, &im_listener, &m);
    say(&m, "ready%s", m.vk_manager != nullptr ? " with a virtual keyboard" : "");
    while (!m.quit && wl_display_dispatch(m.display) != -1) {
    }
    if (m.log != nullptr)
        fclose(m.log);
    wl_display_disconnect(m.display);
    return m.quit ? EXIT_FAILURE : EXIT_SUCCESS;
}
