/* SPDX-License-Identifier: MIT */
#include <gdk/gdkwayland.h>
#include <string.h>

#include "../ui/protocols.h"
#include "tasks.h"

typedef struct task_impl {
    xd_task pub; /* first: xd_task* and task_impl* convert */
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    struct xd_tasklist *list;
    gboolean announced;
} task_impl;

struct xd_tasklist {
    GdkDisplay *display;
    struct wl_registry *registry;
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    struct xd_window_arranger_v1 *arranger; /* version 4+, or nullptr */
    GList *tasks; /* task_impl* */
    xd_task_cb changed, closed;
    gpointer data;
};

static task_impl *impl(xd_task *task) { return (task_impl *)(void *)task; }

static void handle_title(void *data,
                         [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
                         const char *title) {
    task_impl *toplevel = data;
    g_free(toplevel->pub.title);
    toplevel->pub.title = g_strdup(title);
}

static void handle_app_id(void *data,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
                          const char *app_id) {
    task_impl *toplevel = data;
    g_free(toplevel->pub.app_id);
    toplevel->pub.app_id = g_strdup(app_id);
}

static void handle_output_enter(
    [[maybe_unused]] void *data,
    [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
    [[maybe_unused]] struct wl_output *output) {}

static void handle_output_leave(
    [[maybe_unused]] void *data,
    [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
    [[maybe_unused]] struct wl_output *output) {}

static void handle_state(void *data,
                         [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
                         struct wl_array *states) {
    task_impl *toplevel = data;
    toplevel->pub.minimized = toplevel->pub.maximized = toplevel->pub.activated = FALSE;
    size_t count = states->size / sizeof(uint32_t);
    const uint32_t *state_values = states->data;
    for (size_t i = 0; i < count; i++) {
        if (state_values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
            toplevel->pub.minimized = TRUE;
        else if (state_values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED)
            toplevel->pub.maximized = TRUE;
        else if (state_values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            toplevel->pub.activated = TRUE;
    }
}

static void handle_done(void *data,
                        [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle) {
    task_impl *toplevel = data;
    toplevel->announced = TRUE;
    if (toplevel->list->changed != nullptr)
        toplevel->list->changed(&toplevel->pub, toplevel->list->data);
}

static void task_free(task_impl *toplevel) {
    zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
    g_free(toplevel->pub.title);
    g_free(toplevel->pub.app_id);
    g_free(toplevel);
}

static void handle_closed(
    void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle) {
    task_impl *toplevel = data;
    xd_tasklist *list = toplevel->list;
    list->tasks = g_list_remove(list->tasks, toplevel);
    if (toplevel->announced && list->closed != nullptr)
        list->closed(&toplevel->pub, list->data);
    task_free(toplevel);
}

static void handle_parent([[maybe_unused]] void *data,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *toplevel_handle,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *parent) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = handle_title,
    .app_id = handle_app_id,
    .output_enter = handle_output_enter,
    .output_leave = handle_output_leave,
    .state = handle_state,
    .done = handle_done,
    .closed = handle_closed,
    .parent = handle_parent,
};

static void manager_toplevel(
    void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager,
    struct zwlr_foreign_toplevel_handle_v1 *handle) {
    xd_tasklist *list = data;
    task_impl *toplevel = g_new0(task_impl, 1);
    toplevel->handle = handle;
    toplevel->list = list;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, toplevel);
    list->tasks = g_list_append(list->tasks, toplevel);
}

static void manager_finished(void *data,
                             struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager) {
    xd_tasklist *list = data;
    zwlr_foreign_toplevel_manager_v1_destroy(toplevel_manager);
    list->manager = nullptr;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = manager_toplevel,
    .finished = manager_finished,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
    xd_tasklist *list = data;
    if (list->manager == nullptr &&
        strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        list->manager = wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface,
                                         MIN(version, 3u));
        zwlr_foreign_toplevel_manager_v1_add_listener(list->manager, &manager_listener, list);
    } else if (list->arranger == nullptr && version >= 4 &&
               strcmp(interface, xd_window_arranger_v1_interface.name) == 0) {
        /* Its events are for the shell; we only send requests. */
        list->arranger = wl_registry_bind(reg, name, &xd_window_arranger_v1_interface,
                                          MIN(version, 5u));
    }
}

static void registry_global_remove([[maybe_unused]] void *data,
                                   [[maybe_unused]] struct wl_registry *reg,
                                   [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

xd_tasklist *xd_tasklist_new(GdkDisplay *display, xd_task_cb changed, xd_task_cb closed,
                             gpointer data) {
    if (!GDK_IS_WAYLAND_DISPLAY(display))
        return nullptr;
    xd_tasklist *list = g_new0(xd_tasklist, 1);
    list->display = display;
    list->changed = changed;
    list->closed = closed;
    list->data = data;
    struct wl_display *wl = gdk_wayland_display_get_wl_display(display);
    list->registry = wl_display_get_registry(wl);
    wl_registry_add_listener(list->registry, &registry_listener, list);
    /* GDK dispatches the default queue; one roundtrip binds the manager. */
    wl_display_roundtrip(wl);
    return list;
}

gboolean xd_tasklist_available(const xd_tasklist *list) {
    return list != nullptr && list->manager != nullptr;
}

void xd_tasklist_free(xd_tasklist *list) {
    if (list == nullptr)
        return;
    while (list->tasks != nullptr) {
        task_impl *toplevel = list->tasks->data;
        list->tasks = g_list_delete_link(list->tasks, list->tasks);
        task_free(toplevel);
    }
    if (list->manager != nullptr)
        zwlr_foreign_toplevel_manager_v1_stop(list->manager);
    if (list->arranger != nullptr)
        xd_window_arranger_v1_destroy(list->arranger);
    wl_registry_destroy(list->registry);
    g_free(list);
}

void xd_task_activate(xd_task *task) {
    task_impl *toplevel = impl(task);
    GdkSeat *seat = gdk_display_get_default_seat(toplevel->list->display);
    if (seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle,
                                                 gdk_wayland_seat_get_wl_seat(seat));
}

void xd_task_close(xd_task *task) { zwlr_foreign_toplevel_handle_v1_close(impl(task)->handle); }

void xd_task_set_minimized(xd_task *task, gboolean minimized) {
    if (minimized)
        zwlr_foreign_toplevel_handle_v1_set_minimized(impl(task)->handle);
    else
        zwlr_foreign_toplevel_handle_v1_unset_minimized(impl(task)->handle);
}

void xd_task_set_maximized(xd_task *task, gboolean maximized) {
    if (maximized)
        zwlr_foreign_toplevel_handle_v1_set_maximized(impl(task)->handle);
    else
        zwlr_foreign_toplevel_handle_v1_unset_maximized(impl(task)->handle);
}

gboolean xd_tasklist_can_arrange(const xd_tasklist *list) {
    return list != nullptr && list->arranger != nullptr;
}

void xd_tasklist_arrange(xd_tasklist *list, GList *tasks, xd_arrange_mode mode) {
    if (!xd_tasklist_can_arrange(list) || tasks == nullptr)
        return;
    for (GList *link = tasks; link != nullptr; link = link->next)
        xd_window_arranger_v1_mark(list->arranger, impl(link->data)->handle);
    uint32_t arranger_mode =
        mode == XD_ARRANGE_TILE_HORIZONTALLY ? XD_WINDOW_ARRANGER_V1_MODE_TILE_HORIZONTALLY
        : mode == XD_ARRANGE_TILE_VERTICALLY ? XD_WINDOW_ARRANGER_V1_MODE_TILE_VERTICALLY
                                             : XD_WINDOW_ARRANGER_V1_MODE_CASCADE;
    xd_window_arranger_v1_arrange_marked(list->arranger, arranger_mode);
}

gboolean xd_tasklist_can_set_on_top(const xd_tasklist *list) {
    return list != nullptr && list->arranger != nullptr &&
           xd_window_arranger_v1_get_version(list->arranger) >=
               XD_WINDOW_ARRANGER_V1_SET_ALWAYS_ON_TOP_SINCE_VERSION;
}

void xd_tasklist_set_on_top(xd_tasklist *list, GtkWidget *window, gboolean on_top) {
    GdkWindow *gw = gtk_widget_get_window(window);
    if (!xd_tasklist_can_set_on_top(list) || gw == nullptr || !GDK_IS_WAYLAND_WINDOW(gw))
        return;
    struct wl_surface *surface = gdk_wayland_window_get_wl_surface(gw);
    if (surface != nullptr)
        xd_window_arranger_v1_set_always_on_top(list->arranger, surface, on_top ? 1 : 0);
}
