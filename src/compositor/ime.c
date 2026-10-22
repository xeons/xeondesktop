/* SPDX-License-Identifier: MIT
 * Input methods (Fcitx 5, IBus): the relay between programs' text boxes
 * (text-input-v3) and the input method (input-method-v2), as sway's.
 *
 * A text box that wants input says so (enable) with its surrounding text,
 * its kind (content type) and where its caret is; we pass that on to the
 * input method and activate it. The input method grabs the keyboard,
 * turns keys into text, and commits it (and its preedit, the text still
 * being composed) back through us to the text box. Keys it doesn't use
 * come back through its own virtual keyboard. Its candidate window (an
 * input popup surface) is put at the text box's caret.
 *
 * The input method sees every key typed while it's active, and a virtual
 * keyboard can type into any program, so both protocols are offered only
 * to the input method: the framework the compositor started (and what it
 * starts), or a program of an installed framework (ibus-daemon starts
 * ibus-wayland itself). --allow-virtual-input opens virtual keyboards to
 * every client, as before, for the UI tests; never the input method.
 * text-input-v3 is for everyone. The screen locker's password box never
 * goes through the input method.
 *
 * The framework itself: [Keyboard] InputMethod (see
 * settings/inputmethod.h), started with the session, restarted if it
 * crashes, switched when the setting changes.
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "../settings/inputmethod.h"
#include "imeplace.h"
#include "server.h"

typedef struct ime_text_input {
    struct wl_list link; /* xd_ime.text_inputs */
    struct xd_ime *ime;
    struct wlr_text_input_v3 *input;
    /* The focused surface, held back while there's no input method:
     * entering a text input tells its program one is there. */
    struct wlr_surface *pending_focus;
    struct wl_listener pending_focus_destroy;
    struct wl_listener enable;
    struct wl_listener commit;
    struct wl_listener disable;
    struct wl_listener destroy;
} ime_text_input;

typedef struct ime_popup {
    struct wl_list link; /* xd_ime.popups */
    struct xd_ime *ime;
    struct wlr_input_popup_surface_v2 *popup;
    struct wlr_scene_tree *tree;
    /* The caret last told, relative to it: told again only if it moves,
     * as the input method may redraw for it. */
    struct wlr_box told;
    bool has_told;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
} ime_popup;

struct xd_ime {
    xd_server *server;
    struct wlr_text_input_manager_v3 *text_input_manager;
    struct wlr_input_method_manager_v2 *input_method_manager;
    struct wlr_input_method_v2 *im; /* the one input method, if any */
    struct wlr_input_method_keyboard_grab_v2 *grab;
    struct wl_list text_inputs; /* ime_text_input.link */
    struct wl_list popups;      /* ime_popup.link */
    struct wlr_scene_tree *popup_tree;
    struct wl_listener new_text_input;
    struct wl_listener new_input_method;
    struct wl_listener focus_change;
    struct wl_listener im_commit;
    struct wl_listener im_new_popup;
    struct wl_listener im_grab;
    struct wl_listener im_destroy;
    struct wl_listener grab_destroy;
    /* The framework we started. */
    xd_im_framework running;
    pid_t pid;
    uint32_t restarts;
    struct timespec restart_window;
};

/* ---- who may be the input method ---------------------------------------- */

/* The parent of `pid`, from /proc/PID/stat (after the command's closing
 * parenthesis, which may itself contain one); 0 if unknown. */
static pid_t parent_of(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *file = fopen(path, "re");
    if (file == nullptr)
        return 0;
    size_t count = fread(buf, 1, sizeof(buf) - 1, file);
    fclose(file);
    buf[count] = '\0';
    const char *close = strrchr(buf, ')');
    int ppid = 0;
    if (close == nullptr || sscanf(close + 1, " %*c %d", &ppid) != 1)
        return 0;
    return (pid_t)ppid;
}

/* `pid` is `ancestor` or one of its descendants. */
static bool descends_from(pid_t pid, pid_t ancestor) {
    for (int depth = 0; pid > 1 && depth < 64; depth++) {
        if (pid == ancestor)
            return true;
        pid = parent_of(pid);
    }
    return false;
}

static bool is_input_method_client(const xd_server *server, const struct wl_client *client) {
    pid_t pid = 0;
    wl_client_get_credentials((struct wl_client *)(uintptr_t)client, &pid, nullptr, nullptr);
    if (pid <= 0)
        return false;
    const struct xd_ime *ime = server->ime;
    if (ime != nullptr && ime->pid > 0 && descends_from(pid, ime->pid))
        return true;
    /* A framework started by others (ibus-daemon on its own): what the
     * client ran when it connected (trust.c). */
    return xd_trust_input_framework(client);
}

bool xd_ime_global_private(const xd_server *server, const struct wl_global *global) {
    const struct xd_ime *ime = server->ime;
    if (ime != nullptr && global == ime->input_method_manager->global)
        return true;
    return !server->config.virtual_input && server->virtual_keyboards != nullptr &&
           global == server->virtual_keyboards->global;
}

bool xd_ime_client_allowed(const xd_server *server, const struct wl_client *client) {
    return is_input_method_client(server, client);
}

/* ---- the relay ------------------------------------------------------------ */

/* The text input that has the input method: enabled, in the focused
 * surface. */
static ime_text_input *active_text_input(struct xd_ime *ime) {
    XD_LIST_FOR_EACH(ti, &ime->text_inputs, ime_text_input, link) {
        if (ti->input->focused_surface != nullptr && ti->input->current_enabled)
            return ti;
    }
    return nullptr;
}

static void send_state(struct xd_ime *ime, struct wlr_text_input_v3 *input) {
    struct wlr_input_method_v2 *im = ime->im;
    if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
        const char *text = input->current.surrounding.text;
        wlr_input_method_v2_send_surrounding_text(im, text != nullptr ? text : "",
                                                  input->current.surrounding.cursor,
                                                  input->current.surrounding.anchor);
    }
    wlr_input_method_v2_send_text_change_cause(im, input->current.text_change_cause);
    if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
        wlr_input_method_v2_send_content_type(im, input->current.content_type.hint,
                                              input->current.content_type.purpose);
    wlr_input_method_v2_send_done(im);
}

/* Where `surface`'s top left is in the layout: found in the scene, so it
 * works for windows, layer surfaces and popups alike. */
typedef struct surface_search {
    struct wlr_surface *surface;
    int x, y;
    bool found;
} surface_search;

static void find_surface(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    surface_search *finder = data;
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    if (!finder->found && scene_surface != nullptr && scene_surface->surface == finder->surface) {
        finder->found = true;
        finder->x = sx;
        finder->y = sy;
    }
}

static void place_popup(ime_popup *input_popup) {
    struct xd_ime *ime = input_popup->ime;
    ime_text_input *ti = active_text_input(ime);
    struct wlr_surface *surface = input_popup->popup->surface;
    if (ti == nullptr || !surface->mapped || ime->server->locked) {
        wlr_scene_node_set_enabled(&input_popup->tree->node, false);
        return;
    }
    surface_search finder = {.surface = ti->input->focused_surface};
    wlr_scene_node_for_each_buffer(&ime->server->scene->tree.node, find_surface, &finder);
    if (!finder.found) {
        wlr_scene_node_set_enabled(&input_popup->tree->node, false);
        return;
    }
    /* Without a caret rectangle, the text box's top left. */
    struct wlr_box caret = {0};
    if (ti->input->current.features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE)
        caret = ti->input->current.cursor_rectangle;
    xd_ime_rect cursor = {finder.x + caret.x, finder.y + caret.y, caret.width, caret.height};
    struct wlr_output *output = wlr_output_layout_output_at(
        ime->server->output_layout, cursor.x + cursor.width / 2.0, cursor.y + cursor.height / 2.0);
    if (output == nullptr)
        output = wlr_output_layout_output_at(ime->server->output_layout, finder.x, finder.y);
    struct wlr_box screen = {0};
    wlr_output_layout_get_box(ime->server->output_layout, output, &screen);
    xd_ime_rect at = xd_ime_popup_place(
        cursor, surface->current.width, surface->current.height,
        (xd_ime_rect){screen.x, screen.y, screen.width, screen.height});
    wlr_scene_node_set_position(&input_popup->tree->node, at.x, at.y);
    wlr_scene_node_set_enabled(&input_popup->tree->node, true);
    /* The caret, relative to the popup, so it can point at it. */
    struct wlr_box rel = {cursor.x - at.x, cursor.y - at.y, cursor.width, cursor.height};
    if (!input_popup->has_told || !wlr_box_equal(&rel, &input_popup->told)) {
        wlr_input_popup_surface_v2_send_text_input_rectangle(input_popup->popup, &rel);
        input_popup->told = rel;
        input_popup->has_told = true;
    }
}

static void place_popups(struct xd_ime *ime) {
    XD_LIST_FOR_EACH(input_popup, &ime->popups, ime_popup, link) { place_popup(input_popup); }
}

static void handle_enable(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ime_text_input *ti = XD_CONTAINER_OF(listener, ime_text_input, enable);
    struct xd_ime *ime = ti->ime;
    /* Only the text box with the keyboard may have the input method. */
    if (ime->im == nullptr || ti->input->focused_surface == nullptr)
        return;
    wlr_input_method_v2_send_activate(ime->im);
    send_state(ime, ti->input);
    place_popups(ime);
}

static void handle_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ime_text_input *ti = XD_CONTAINER_OF(listener, ime_text_input, commit);
    struct xd_ime *ime = ti->ime;
    if (ime->im == nullptr || !ti->input->current_enabled || ti->input->focused_surface == nullptr)
        return;
    send_state(ime, ti->input);
    place_popups(ime);
}

static void deactivate(struct xd_ime *ime) {
    if (ime->im == nullptr)
        return;
    wlr_input_method_v2_send_deactivate(ime->im);
    wlr_input_method_v2_send_done(ime->im);
}

static void handle_disable(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ime_text_input *ti = XD_CONTAINER_OF(listener, ime_text_input, disable);
    if (ti->input->focused_surface == nullptr)
        return;
    deactivate(ti->ime);
    place_popups(ti->ime);
}

static void set_pending_focus(ime_text_input *ti, struct wlr_surface *surface);

static void handle_pending_focus_destroy(struct wl_listener *listener,
                                         [[maybe_unused]] void *data) {
    ime_text_input *ti = XD_CONTAINER_OF(listener, ime_text_input, pending_focus_destroy);
    set_pending_focus(ti, nullptr);
}

static void set_pending_focus(ime_text_input *ti, struct wlr_surface *surface) {
    xd_unlisten(&ti->pending_focus_destroy);
    ti->pending_focus = surface;
    if (surface != nullptr)
        XD_LISTEN(&surface->events.destroy, ti, pending_focus_destroy,
                  handle_pending_focus_destroy);
}

static void handle_text_input_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ime_text_input *ti = XD_CONTAINER_OF(listener, ime_text_input, destroy);
    struct xd_ime *ime = ti->ime;
    bool was_active = ti->input->current_enabled && ti->input->focused_surface != nullptr;
    xd_unlisten(&ti->enable);
    xd_unlisten(&ti->commit);
    xd_unlisten(&ti->disable);
    xd_unlisten(&ti->destroy);
    set_pending_focus(ti, nullptr);
    wl_list_remove(&ti->link);
    xd_free(ti);
    if (was_active)
        deactivate(ime);
    place_popups(ime);
}

/* Enters `ti` into `surface` if it's its program's, or holds it back
 * until there's an input method. */
static void offer_focus(ime_text_input *ti, struct wlr_surface *surface) {
    if (surface == nullptr ||
        wl_resource_get_client(ti->input->resource) != wl_resource_get_client(surface->resource))
        return;
    if (ti->ime->im != nullptr)
        wlr_text_input_v3_send_enter(ti->input, surface);
    else
        set_pending_focus(ti, surface);
}

static void handle_new_text_input(struct wl_listener *listener, void *data) {
    struct xd_ime *ime = XD_CONTAINER_OF(listener, struct xd_ime, new_text_input);
    struct wlr_text_input_v3 *input = data;
    ime_text_input *ti = XD_NEW(ime_text_input);
    ti->ime = ime;
    ti->input = input;
    wl_list_insert(&ime->text_inputs, &ti->link);
    XD_LISTEN(&input->events.enable, ti, enable, handle_enable);
    XD_LISTEN(&input->events.commit, ti, commit, handle_commit);
    XD_LISTEN(&input->events.disable, ti, disable, handle_disable);
    XD_LISTEN(&input->events.destroy, ti, destroy, handle_text_input_destroy);
    offer_focus(ti, ime->server->seat->keyboard_state.focused_surface);
}

/* The keyboard moved: text inputs leave the old surface (the input method
 * goes inactive) and enter the new one. */
static void handle_focus_change(struct wl_listener *listener, void *data) {
    struct xd_ime *ime = XD_CONTAINER_OF(listener, struct xd_ime, focus_change);
    const struct wlr_seat_keyboard_focus_change_event *seat_keyboard_focus_change_event = data;
    if (seat_keyboard_focus_change_event->old_surface ==
        seat_keyboard_focus_change_event->new_surface)
        return;
    XD_LIST_FOR_EACH(ti, &ime->text_inputs, ime_text_input, link) {
        set_pending_focus(ti, nullptr);
        if (ti->input->focused_surface != nullptr) {
            if (ti->input->current_enabled)
                deactivate(ime);
            wlr_text_input_v3_send_leave(ti->input);
        }
        offer_focus(ti, seat_keyboard_focus_change_event->new_surface);
    }
    place_popups(ime);
}

/* ---- the input method ------------------------------------------------------ */

static void handle_im_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    struct xd_ime *ime = XD_CONTAINER_OF(listener, struct xd_ime, im_commit);
    ime_text_input *ti = active_text_input(ime);
    if (ti == nullptr)
        return;
    const struct wlr_input_method_v2_state *st = &ime->im->current;
    if (st->preedit.text != nullptr)
        wlr_text_input_v3_send_preedit_string(ti->input, st->preedit.text,
                                              st->preedit.cursor_begin, st->preedit.cursor_end);
    if (st->commit_text != nullptr)
        wlr_text_input_v3_send_commit_string(ti->input, st->commit_text);
    if (st->delete.before_length != 0 || st->delete.after_length != 0)
        wlr_text_input_v3_send_delete_surrounding_text(ti->input, st->delete.before_length,
                                                       st->delete.after_length);
    wlr_text_input_v3_send_done(ti->input);
}

static void handle_popup_map(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ime_popup *input_popup = XD_CONTAINER_OF(listener, ime_popup, map);
    place_popup(input_popup);
}

static void handle_popup_unmap(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ime_popup *input_popup = XD_CONTAINER_OF(listener, ime_popup, unmap);
    wlr_scene_node_set_enabled(&input_popup->tree->node, false);
}

/* A new size (a longer candidate list) may need it moved to fit. */
static void handle_popup_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ime_popup *input_popup = XD_CONTAINER_OF(listener, ime_popup, commit);
    if (input_popup->popup->surface->mapped)
        place_popup(input_popup);
}

static void handle_popup_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ime_popup *input_popup = XD_CONTAINER_OF(listener, ime_popup, destroy);
    xd_unlisten(&input_popup->map);
    xd_unlisten(&input_popup->unmap);
    xd_unlisten(&input_popup->commit);
    xd_unlisten(&input_popup->destroy);
    /* The scene's subsurface tree goes with the surface by itself; ours
     * holds it. */
    wlr_scene_node_destroy(&input_popup->tree->node);
    wl_list_remove(&input_popup->link);
    xd_free(input_popup);
}

static void handle_im_new_popup(struct wl_listener *listener, void *data) {
    struct xd_ime *ime = XD_CONTAINER_OF(listener, struct xd_ime, im_new_popup);
    struct wlr_input_popup_surface_v2 *popup = data;
    ime_popup *input_popup = XD_NEW(ime_popup);
    input_popup->ime = ime;
    input_popup->popup = popup;
    input_popup->tree = wlr_scene_tree_create(ime->popup_tree);
    wlr_scene_subsurface_tree_create(input_popup->tree, popup->surface);
    wlr_scene_node_set_enabled(&input_popup->tree->node, false);
    wl_list_insert(&ime->popups, &input_popup->link);
    XD_LISTEN(&popup->surface->events.map, input_popup, map, handle_popup_map);
    XD_LISTEN(&popup->surface->events.unmap, input_popup, unmap, handle_popup_unmap);
    XD_LISTEN(&popup->surface->events.commit, input_popup, commit, handle_popup_commit);
    XD_LISTEN(&popup->events.destroy, input_popup, destroy, handle_popup_destroy);
    if (popup->surface->mapped)
        place_popup(input_popup);
}

/* The keyboard the input method hears first: the one last typed on,
 * unless that's its own (whose keys it sent itself) or has no keymap yet
 * (a virtual keyboard just connected). The first key sets it anyway. */
static bool grab_may_use(const xd_keyboard *kb) {
    return kb->wlr_keyboard->keymap != nullptr && !xd_ime_own_keyboard(kb);
}

static struct wlr_keyboard *grab_keyboard_for(struct xd_ime *ime) {
    struct wlr_keyboard *seat_kb = wlr_seat_get_keyboard(ime->server->seat);
    XD_LIST_FOR_EACH(kb, &ime->server->keyboards, xd_keyboard, link) {
        if (kb->wlr_keyboard == seat_kb && grab_may_use(kb))
            return seat_kb;
    }
    XD_LIST_FOR_EACH(kb, &ime->server->keyboards, xd_keyboard, link) {
        if (grab_may_use(kb))
            return kb->wlr_keyboard;
    }
    return nullptr;
}

static void handle_grab_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    struct xd_ime *ime = XD_CONTAINER_OF(listener, struct xd_ime, grab_destroy);
    xd_unlisten(&ime->grab_destroy);
    struct wlr_keyboard *kb = ime->grab->keyboard;
    ime->grab = nullptr;
    /* The program had the modifiers from the input method's keyboard;
     * it gets the real ones back. */
    if (kb != nullptr) {
        wlr_seat_set_keyboard(ime->server->seat, kb);
        wlr_seat_keyboard_notify_modifiers(ime->server->seat, &kb->modifiers);
    }
}

static void handle_im_grab(struct wl_listener *listener, void *data) {
    struct xd_ime *ime = XD_CONTAINER_OF(listener, struct xd_ime, im_grab);
    struct wlr_input_method_keyboard_grab_v2 *grab = data;
    if (ime->grab != nullptr) /* one at a time; the protocol allows only one */
        return;
    ime->grab = grab;
    XD_LISTEN(&grab->events.destroy, ime, grab_destroy, handle_grab_destroy);
    struct wlr_keyboard *kb = grab_keyboard_for(ime);
    if (kb != nullptr) {
        wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb);
        wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &kb->modifiers);
    }
}

static void handle_im_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    struct xd_ime *ime = XD_CONTAINER_OF(listener, struct xd_ime, im_destroy);
    xd_unlisten(&ime->im_commit);
    xd_unlisten(&ime->im_new_popup);
    xd_unlisten(&ime->im_grab);
    xd_unlisten(&ime->im_destroy);
    ime->im = nullptr;
    /* Programs are told there's no input method any more, and get it
     * back when another comes. */
    XD_LIST_FOR_EACH(ti, &ime->text_inputs, ime_text_input, link) {
        struct wlr_surface *surface = ti->input->focused_surface;
        if (surface != nullptr) {
            wlr_text_input_v3_send_leave(ti->input);
            set_pending_focus(ti, surface);
        }
    }
    wlr_log(WLR_INFO, "input method gone");
}

static void handle_new_input_method(struct wl_listener *listener, void *data) {
    struct xd_ime *ime = XD_CONTAINER_OF(listener, struct xd_ime, new_input_method);
    struct wlr_input_method_v2 *im = data;
    if (ime->im != nullptr || im->seat != ime->server->seat) {
        wlr_input_method_v2_send_unavailable(im);
        return;
    }
    ime->im = im;
    XD_LISTEN(&im->events.commit, ime, im_commit, handle_im_commit);
    XD_LISTEN(&im->events.new_popup_surface, ime, im_new_popup, handle_im_new_popup);
    XD_LISTEN(&im->events.grab_keyboard, ime, im_grab, handle_im_grab);
    XD_LISTEN(&im->events.destroy, ime, im_destroy, handle_im_destroy);
    wlr_log(WLR_INFO, "input method connected");
    /* Text boxes already focused learn of it, and one already enabled
     * activates it at once. */
    XD_LIST_FOR_EACH(ti, &ime->text_inputs, ime_text_input, link) {
        if (ti->pending_focus != nullptr) {
            struct wlr_surface *surface = ti->pending_focus;
            set_pending_focus(ti, nullptr);
            wlr_text_input_v3_send_enter(ti->input, surface);
        }
    }
    ime_text_input *ti = active_text_input(ime);
    if (ti != nullptr) {
        wlr_input_method_v2_send_activate(im);
        send_state(ime, ti->input);
    }
}

/* ---- keys ------------------------------------------------------------------- */

bool xd_ime_own_keyboard(const xd_keyboard *kb) {
    const struct xd_ime *ime = kb->server->ime;
    if (ime == nullptr || ime->im == nullptr)
        return false;
    struct wlr_virtual_keyboard_v1 *vk =
        wlr_input_device_get_virtual_keyboard(&kb->wlr_keyboard->base);
    return vk != nullptr &&
           wl_resource_get_client(vk->resource) == wl_resource_get_client(ime->im->resource);
}

/* The grab gets the keys, unless the screen is locked (passwords never
 * go through the input method) or they're its own. */
static struct wlr_input_method_keyboard_grab_v2 *grab_for(const xd_keyboard *kb) {
    struct xd_ime *ime = kb->server->ime;
    if (ime == nullptr || ime->grab == nullptr || kb->server->locked || xd_ime_own_keyboard(kb))
        return nullptr;
    return ime->grab;
}

bool xd_ime_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *keyboard_key_event) {
    struct wlr_input_method_keyboard_grab_v2 *grab = grab_for(kb);
    if (grab == nullptr)
        return false;
    wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb->wlr_keyboard);
    wlr_input_method_keyboard_grab_v2_send_key(grab, keyboard_key_event->time_msec,
                                               keyboard_key_event->keycode,
                                               keyboard_key_event->state);
    return true;
}

bool xd_ime_modifiers(xd_keyboard *kb) {
    struct wlr_input_method_keyboard_grab_v2 *grab = grab_for(kb);
    if (grab == nullptr)
        return false;
    wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb->wlr_keyboard);
    wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &kb->wlr_keyboard->modifiers);
    return true;
}

void xd_ime_lock_changed(xd_server *server) {
    if (server->ime != nullptr)
        place_popups(server->ime);
}

/* ---- the framework ----------------------------------------------------------- */

/* Restarts at most 5 times in 30 seconds, as the shell. */
static bool may_restart(struct xd_ime *ime) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - ime->restart_window.tv_sec > 30) {
        ime->restart_window = now;
        ime->restarts = 0;
    }
    return ++ime->restarts <= 5;
}

static void start_framework(struct xd_ime *ime, xd_im_framework fw) {
    ime->running = fw;
    if (fw == XD_IM_NONE || ime->server->terminating)
        return;
    /* XD_IME_COMMAND stands in for the framework's program: the UI tests'
     * input method. */
    const char *test = getenv("XD_IME_COMMAND");
    if (test != nullptr && test[0] != '\0') {
        char *argv[] = {(char *)(uintptr_t)test, nullptr};
        ime->pid = xd_spawn_argv(argv);
    } else if (xd_im_installed(fw)) {
        const char *const *argv = xd_im_argv(fw);
        ime->pid = xd_spawn_argv((char *const *)(uintptr_t)argv);
    } else {
        wlr_log(WLR_ERROR, "input method %s is not installed", xd_im_display_name(fw));
        return;
    }
    wlr_log(WLR_INFO, "started input method %s (pid %d)", xd_im_display_name(fw), (int)ime->pid);
}

static void stop_framework(struct xd_ime *ime) {
    if (ime->pid > 0)
        kill(ime->pid, SIGTERM);
    ime->pid = 0;
    /* "ibus start" leaves ibus-daemon running on its own. */
    if (ime->running == XD_IM_IBUS && getenv("XD_IME_COMMAND") == nullptr &&
        xd_im_installed(XD_IM_IBUS)) {
        static char ibus[] = "ibus", exit_cmd[] = "exit";
        char *argv[] = {ibus, exit_cmd, nullptr};
        xd_spawn_argv(argv);
    }
    ime->running = XD_IM_NONE;
}

bool xd_ime_child_exited(xd_server *server, pid_t pid, int status) {
    struct xd_ime *ime = server->ime;
    if (ime == nullptr || pid <= 0 || pid != ime->pid)
        return false;
    ime->pid = 0;
    /* A crash restarts it; a clean exit was meant (another copy runs, or
     * it went on in the background). */
    if (WIFSIGNALED(status)) {
        if (may_restart(ime)) {
            wlr_log(WLR_ERROR, "input method killed by signal %d; restarting",
                    WTERMSIG(status));
            start_framework(ime, ime->running);
        } else {
            wlr_log(WLR_ERROR, "input method keeps crashing; not restarting it");
        }
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        wlr_log(WLR_ERROR, "input method exited with status %d", WEXITSTATUS(status));
    }
    return true;
}

bool xd_ime_apply(xd_server *server) {
    struct xd_ime *ime = server->ime;
    xd_im_framework fw = xd_im_from_name(server->config.keyboard.input_method);
    bool env_changed = xd_im_env_apply(fw);
    if (ime != nullptr && fw != ime->running) {
        stop_framework(ime);
        start_framework(ime, fw);
    }
    return env_changed;
}

/* ---- setup ---------------------------------------------------------------------- */

void xd_ime_init(xd_server *server) {
    struct xd_ime *ime = XD_NEW(struct xd_ime);
    server->ime = ime;
    ime->server = server;
    wl_list_init(&ime->text_inputs);
    wl_list_init(&ime->popups);
    ime->text_input_manager = wlr_text_input_manager_v3_create(server->display);
    ime->input_method_manager = wlr_input_method_manager_v2_create(server->display);
    /* Candidate windows: over windows, the taskbar and menus; under the
     * screen locker. */
    ime->popup_tree = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_place_below(&ime->popup_tree->node, &server->layer_lock->node);
    XD_LISTEN(&ime->text_input_manager->events.new_text_input, ime, new_text_input,
              handle_new_text_input);
    XD_LISTEN(&ime->input_method_manager->events.new_input_method, ime, new_input_method,
              handle_new_input_method);
    XD_LISTEN(&server->seat->keyboard_state.events.focus_change, ime, focus_change,
              handle_focus_change);
}

void xd_ime_finish(xd_server *server) {
    struct xd_ime *ime = server->ime;
    if (ime == nullptr)
        return;
    stop_framework(ime);
    xd_unlisten(&ime->new_text_input);
    xd_unlisten(&ime->new_input_method);
    xd_unlisten(&ime->focus_change);
    xd_unlisten(&ime->im_commit);
    xd_unlisten(&ime->im_new_popup);
    xd_unlisten(&ime->im_grab);
    xd_unlisten(&ime->im_destroy);
    xd_unlisten(&ime->grab_destroy);
    XD_LIST_FOR_EACH_SAFE(ti, tmp, &ime->text_inputs, ime_text_input, link) {
        xd_unlisten(&ti->enable);
        xd_unlisten(&ti->commit);
        xd_unlisten(&ti->disable);
        xd_unlisten(&ti->destroy);
        set_pending_focus(ti, nullptr);
        wl_list_remove(&ti->link);
        xd_free(ti);
    }
    XD_LIST_FOR_EACH_SAFE(input_popup, ptmp, &ime->popups, ime_popup, link) {
        xd_unlisten(&input_popup->map);
        xd_unlisten(&input_popup->unmap);
        xd_unlisten(&input_popup->commit);
        xd_unlisten(&input_popup->destroy);
        wl_list_remove(&input_popup->link);
        xd_free(input_popup);
    }
    server->ime = nullptr;
    xd_free(ime);
}
