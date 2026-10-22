/* SPDX-License-Identifier: MIT
 * Title bar icons, loaded in a thread: see appicons.h.
 */
#include "appicons.h"

#include <errno.h>
#include <glib.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../ui/appid.h"
#include "../ui/icons.h"

/* A lookup, there and back. `px` 0 tells the thread to finish. */
typedef struct request {
    char *key;
    char *app_id;
    int px;
    cairo_surface_t *surface;
} request;

/* A cache entry: `surface` is nullptr while it loads, or if there's none. */
typedef struct entry {
    cairo_surface_t *surface;
} entry;

struct xd_appicons {
    GHashTable *cache; /* "px:app_id" -> entry */
    GAsyncQueue *todo, *done;
    GThread *thread;
    int efd; /* the thread's "done" signal */
    struct wl_event_source *source;
    char *theme;
    xd_appicons_ready_fn ready;
    void *data;
};

static void request_free(request *icon_request) {
    if (icon_request->surface != nullptr)
        cairo_surface_destroy(icon_request->surface);
    g_free(icon_request->key);
    g_free(icon_request->app_id);
    g_free(icon_request);
}

static void entry_free(gpointer user_data) {
    entry *cached = user_data;
    if (cached->surface != nullptr)
        cairo_surface_destroy(cached->surface);
    g_free(cached);
}

/* The thread's own copy: the icon cache's surfaces stay in the thread. */
static cairo_surface_t *copy_surface(cairo_surface_t *icon_surface, int px) {
    int width = px, height = px;
    if (cairo_surface_get_type(icon_surface) == CAIRO_SURFACE_TYPE_IMAGE) {
        width = cairo_image_surface_get_width(icon_surface);
        height = cairo_image_surface_get_height(icon_surface);
    }
    cairo_surface_t *copy = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cairo = cairo_create(copy);
    cairo_set_source_surface(cairo, icon_surface, 0, 0);
    cairo_paint(cairo);
    cairo_destroy(cairo);
    if (cairo_surface_status(copy) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(copy);
        return nullptr;
    }
    return copy;
}

static gpointer worker(gpointer data) {
    xd_appicons *ai = data;
    xd_icons *icons = xd_icons_create(ai->theme);
    for (;;) {
        request *icon_request = g_async_queue_pop(ai->todo);
        if (icon_request->px == 0) {
            request_free(icon_request);
            break;
        }
        char *icon = nullptr, *name = nullptr;
        xd_app_for_window(icon_request->app_id, &icon, &name);
        char *lower = g_ascii_strdown(icon_request->app_id, -1);
        /* The application's Icon key; else, as many programs name their
         * icon, the app id itself; else Windows' generic program icon. */
        const char *names[4];
        size_t count = 0;
        if (icon != nullptr)
            names[count++] = icon;
        if (icon_request->app_id[0] != '\0') {
            names[count++] = icon_request->app_id;
            names[count++] = lower;
        }
        names[count++] = "application-x-executable";
        cairo_surface_t *surface =
            icons != nullptr ? xd_icons_get_any(icons, names, count, icon_request->px) : nullptr;
        if (surface != nullptr)
            icon_request->surface = copy_surface(surface, icon_request->px);
        g_free(lower);
        g_free(icon);
        g_free(name);
        g_async_queue_push(ai->done, icon_request);
        uint64_t one = 1;
        while (write(ai->efd, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
    }
    if (icons != nullptr)
        xd_icons_destroy(icons);
    return nullptr;
}

static int handle_done(int fd, [[maybe_unused]] uint32_t mask, void *data) {
    xd_appicons *ai = data;
    uint64_t count;
    while (read(fd, &count, sizeof(count)) < 0 && errno == EINTR) {
    }
    request *icon_request;
    while ((icon_request = g_async_queue_try_pop(ai->done)) != nullptr) {
        entry *cached = g_hash_table_lookup(ai->cache, icon_request->key);
        if (cached != nullptr) {
            cached->surface = icon_request->surface;
            icon_request->surface = nullptr;
        }
        if (ai->ready != nullptr)
            ai->ready(ai->data, icon_request->app_id);
        request_free(icon_request);
    }
    return 0;
}

xd_appicons *xd_appicons_create(struct wl_event_loop *loop, const char *theme,
                                xd_appicons_ready_fn ready, void *data) {
    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0)
        return nullptr;
    xd_appicons *ai = XD_NEW(xd_appicons);
    ai->efd = efd;
    ai->theme = g_strdup(theme);
    ai->ready = ready;
    ai->data = data;
    ai->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
    ai->todo = g_async_queue_new();
    ai->done = g_async_queue_new();
    ai->source = wl_event_loop_add_fd(loop, efd, WL_EVENT_READABLE, handle_done, ai);
    ai->thread = g_thread_new("xd-appicons", worker, ai);
    return ai;
}

void xd_appicons_destroy(xd_appicons *ai) {
    if (ai == nullptr)
        return;
    request *quit = g_new0(request, 1);
    g_async_queue_push(ai->todo, quit);
    g_thread_join(ai->thread);
    request *icon_request;
    while ((icon_request = g_async_queue_try_pop(ai->todo)) != nullptr)
        request_free(icon_request);
    while ((icon_request = g_async_queue_try_pop(ai->done)) != nullptr)
        request_free(icon_request);
    g_async_queue_unref(ai->todo);
    g_async_queue_unref(ai->done);
    if (ai->source != nullptr)
        wl_event_source_remove(ai->source);
    close(ai->efd);
    g_hash_table_destroy(ai->cache);
    g_free(ai->theme);
    xd_free(ai);
}

const char *xd_appicons_theme(const xd_appicons *ai) { return ai->theme; }

cairo_surface_t *xd_appicons_get(xd_appicons *ai, const char *app_id, int px) {
    if (ai == nullptr || px <= 0)
        return nullptr;
    if (app_id == nullptr)
        app_id = "";
    char *key = g_strdup_printf("%d:%s", px, app_id);
    entry *cached = g_hash_table_lookup(ai->cache, key);
    if (cached != nullptr) {
        g_free(key);
        return cached->surface;
    }
    g_hash_table_insert(ai->cache, key, g_new0(entry, 1));
    request *icon_request = g_new0(request, 1);
    icon_request->key = g_strdup(key);
    icon_request->app_id = g_strdup(app_id);
    icon_request->px = px;
    g_async_queue_push(ai->todo, icon_request);
    return nullptr;
}
