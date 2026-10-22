/* SPDX-License-Identifier: MIT
 * The screen, the windows on it and the clipboard, through Wayland
 * protocols GTK doesn't speak, on GTK's own connection.
 *
 * Capturing runs on a queue of our own, waited on there, so GTK's events
 * keep waiting for GTK. Each monitor is copied with wlr-screencopy into
 * shared memory, then drawn into one picture of the whole desktop at the
 * largest monitor scale.
 *
 * The clipboard is ext-data-control's, which a program may set without a
 * window or keyboard focus: Print Screen's xd-snip has neither, and a snip
 * copied in the Mark-up window stays on the clipboard after it closes, as
 * in Windows, because a separate xd-snip keeps serving it.
 */
#include <errno.h>
#include <fcntl.h>
#include <gdk/gdkwayland.h>
#include <gio/gunixoutputstream.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../ui/protocols.h"
#include "snip.h"

#define PNG_TYPE "image/png"

typedef struct globals {
    struct wl_shm *shm;
    struct zwlr_screencopy_manager_v1 *screencopy;
    uint32_t screencopy_version;
    struct xd_window_rectangles_manager_v1 *rectangles;
    struct ext_data_control_manager_v1 *data_control;
} globals;

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t version) {
    globals *found = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        found->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        found->screencopy_version = MIN(version, 3);
        found->screencopy = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface,
                                             found->screencopy_version);
    } else if (strcmp(interface, xd_window_rectangles_manager_v1_interface.name) == 0) {
        found->rectangles =
            wl_registry_bind(registry, name, &xd_window_rectangles_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
        found->data_control =
            wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1);
    }
}

static void handle_global_remove([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_registry *registry,
                                 [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

/* Our connection to the compositor, with a queue of our own. */
typedef struct connection {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    globals globals;
} connection;

static bool connect_globals(GdkDisplay *gdk_display, connection *conn, GError **error) {
    *conn = (connection){0};
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "The Snipping Tool needs a Wayland desktop.");
        return false;
    }
    conn->display = gdk_wayland_display_get_wl_display(gdk_display);
    conn->queue = wl_display_create_queue(conn->display);
    struct wl_display *wrapper = wl_proxy_create_wrapper(conn->display);
    wl_proxy_set_queue((struct wl_proxy *)wrapper, conn->queue);
    conn->registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(conn->registry, &registry_listener, &conn->globals);
    if (wl_display_roundtrip_queue(conn->display, conn->queue) < 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The connection to the desktop was lost.");
        return false;
    }
    return true;
}

static void disconnect_globals(connection *conn) {
    globals *found = &conn->globals;
    if (found->shm != nullptr)
        wl_shm_destroy(found->shm);
    if (found->screencopy != nullptr)
        zwlr_screencopy_manager_v1_destroy(found->screencopy);
    if (found->rectangles != nullptr)
        xd_window_rectangles_manager_v1_destroy(found->rectangles);
    if (found->data_control != nullptr)
        ext_data_control_manager_v1_destroy(found->data_control);
    if (conn->registry != nullptr)
        wl_registry_destroy(conn->registry);
    if (conn->queue != nullptr)
        wl_event_queue_destroy(conn->queue);
    *conn = (connection){0};
}

/* ---- one monitor ------------------------------------------------------------------------- */

typedef struct monitor_copy {
    connection *conn;
    struct zwlr_screencopy_frame_v1 *frame;
    GdkRectangle geometry; /* logical, in the desktop */
    /* The buffer the compositor asked for. */
    uint32_t format;
    int width, height, stride;
    bool offered;
    struct wl_buffer *buffer;
    void *pixels;
    size_t size;
    bool y_invert;
    bool done, failed;
    cairo_surface_t *image; /* once done */
} monitor_copy;

/* Formats we can read, little-endian: cairo's own two, and the same with
 * red and blue the other way round. */
static bool format_known(uint32_t format) {
    return format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 ||
           format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888;
}

static void copy_frame(monitor_copy *copy) {
    if (!copy->offered) {
        copy->failed = true;
        return;
    }
    copy->size = (size_t)copy->stride * (size_t)copy->height;
    int fd = memfd_create("xd-snip", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)copy->size) != 0) {
        if (fd >= 0)
            close(fd);
        copy->failed = true;
        return;
    }
    copy->pixels = mmap(nullptr, copy->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (copy->pixels == MAP_FAILED) {
        copy->pixels = nullptr;
        close(fd);
        copy->failed = true;
        return;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(copy->conn->globals.shm, fd, (int)copy->size);
    copy->buffer = wl_shm_pool_create_buffer(pool, 0, copy->width, copy->height, copy->stride,
                                             copy->format);
    wl_shm_pool_destroy(pool);
    close(fd);
    zwlr_screencopy_frame_v1_copy(copy->frame, copy->buffer);
}

static void handle_buffer(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                          uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    monitor_copy *copy = data;
    if (copy->offered || !format_known(format) || width == 0 || height == 0 ||
        width > INT32_MAX / 4 || stride < width * 4)
        return;
    copy->format = format;
    copy->width = (int)width;
    copy->height = (int)height;
    copy->stride = (int)stride;
    copy->offered = true;
    /* Before version 3 there's no buffer_done: this is the only offer. */
    if (copy->conn->globals.screencopy_version < 3)
        copy_frame(copy);
}

static void handle_flags(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t flags) {
    monitor_copy *copy = data;
    copy->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

/* The copied pixels as a cairo picture, the right way up. */
static cairo_surface_t *picture_of(const monitor_copy *copy) {
    cairo_format_t format = copy->format == WL_SHM_FORMAT_ARGB8888 ||
                                    copy->format == WL_SHM_FORMAT_ABGR8888
                                ? CAIRO_FORMAT_ARGB32
                                : CAIRO_FORMAT_RGB24;
    cairo_surface_t *image = cairo_image_surface_create(format, copy->width, copy->height);
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS)
        return image;
    unsigned char *target = cairo_image_surface_get_data(image);
    int target_stride = cairo_image_surface_get_stride(image);
    bool swap = copy->format == WL_SHM_FORMAT_ABGR8888 || copy->format == WL_SHM_FORMAT_XBGR8888;
    for (int row = 0; row < copy->height; row++) {
        int source_row = copy->y_invert ? copy->height - 1 - row : row;
        const uint32_t *source =
            (const uint32_t *)(const void *)((const unsigned char *)copy->pixels +
                                             (size_t)source_row * (size_t)copy->stride);
        uint32_t *line = (uint32_t *)(void *)(target + (size_t)row * (size_t)target_stride);
        if (!swap) {
            memcpy(line, source, (size_t)copy->width * 4);
            continue;
        }
        for (int column = 0; column < copy->width; column++) {
            uint32_t pixel = source[column];
            line[column] = (pixel & 0xff00ff00u) | ((pixel & 0xffu) << 16) | ((pixel >> 16) & 0xffu);
        }
    }
    cairo_surface_mark_dirty(image);
    return image;
}

static void handle_ready(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                         [[maybe_unused]] uint32_t tv_sec_hi, [[maybe_unused]] uint32_t tv_sec_lo,
                         [[maybe_unused]] uint32_t tv_nsec) {
    monitor_copy *copy = data;
    copy->image = picture_of(copy);
    copy->done = true;
}

static void handle_failed(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame) {
    monitor_copy *copy = data;
    copy->failed = true;
}

static void handle_damage([[maybe_unused]] void *data,
                          [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                          [[maybe_unused]] uint32_t x, [[maybe_unused]] uint32_t y,
                          [[maybe_unused]] uint32_t width, [[maybe_unused]] uint32_t height) {}

static void handle_linux_dmabuf([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                                [[maybe_unused]] uint32_t format, [[maybe_unused]] uint32_t width,
                                [[maybe_unused]] uint32_t height) {}

static void handle_buffer_done(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame) {
    copy_frame(data);
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = handle_buffer,
    .flags = handle_flags,
    .ready = handle_ready,
    .failed = handle_failed,
    .damage = handle_damage,
    .linux_dmabuf = handle_linux_dmabuf,
    .buffer_done = handle_buffer_done,
};

static void free_copy(monitor_copy *copy) {
    if (copy->frame != nullptr)
        zwlr_screencopy_frame_v1_destroy(copy->frame);
    if (copy->buffer != nullptr)
        wl_buffer_destroy(copy->buffer);
    if (copy->pixels != nullptr)
        munmap(copy->pixels, copy->size);
    if (copy->image != nullptr)
        cairo_surface_destroy(copy->image);
}

/* ---- the windows ---------------------------------------------------------------------------- */

typedef struct window_list {
    GArray *windows; /* xd_snip_window */
    bool done;
} window_list;

static void handle_window(void *data, [[maybe_unused]] struct xd_window_rectangles_v1 *list,
                          int32_t x, int32_t y, int32_t width, int32_t height, uint32_t flags) {
    window_list *found = data;
    xd_snip_window window = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .activated = (flags & XD_WINDOW_RECTANGLES_V1_FLAGS_ACTIVATED) != 0,
    };
    g_array_append_val(found->windows, window);
}

static void handle_windows_done(void *data,
                                [[maybe_unused]] struct xd_window_rectangles_v1 *list) {
    window_list *found = data;
    found->done = true;
}

static const struct xd_window_rectangles_v1_listener window_listener = {
    .window = handle_window,
    .done = handle_windows_done,
};

/* ---- the desktop ---------------------------------------------------------------------------- */

xd_snip_screen *snip_capture_screen(GdkDisplay *display, GError **error) {
    connection conn;
    if (!connect_globals(display, &conn, error)) {
        disconnect_globals(&conn);
        return nullptr;
    }
    if (conn.globals.screencopy == nullptr || conn.globals.shm == nullptr) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "This desktop doesn't let programs capture the screen.");
        disconnect_globals(&conn);
        return nullptr;
    }

    int monitor_count = gdk_display_get_n_monitors(display);
    monitor_copy *copies = g_new0(monitor_copy, (size_t)MAX(monitor_count, 1));
    int started = 0;
    for (int i = 0; i < monitor_count; i++) {
        GdkMonitor *monitor = gdk_display_get_monitor(display, i);
        struct wl_output *output = gdk_wayland_monitor_get_wl_output(monitor);
        if (output == nullptr)
            continue;
        monitor_copy *copy = &copies[started++];
        copy->conn = &conn;
        gdk_monitor_get_geometry(monitor, &copy->geometry);
        copy->frame = zwlr_screencopy_manager_v1_capture_output(conn.globals.screencopy, 0, output);
        zwlr_screencopy_frame_v1_add_listener(copy->frame, &frame_listener, copy);
    }
    window_list found = {.windows = g_array_new(FALSE, FALSE, sizeof(xd_snip_window))};
    struct xd_window_rectangles_v1 *list = nullptr;
    if (conn.globals.rectangles != nullptr) {
        list = xd_window_rectangles_manager_v1_get_rectangles(conn.globals.rectangles);
        xd_window_rectangles_v1_add_listener(list, &window_listener, &found);
    } else {
        found.done = true;
    }

    for (;;) {
        bool waiting = !found.done;
        for (int i = 0; i < started; i++)
            waiting = waiting || (!copies[i].done && !copies[i].failed);
        if (!waiting)
            break;
        if (wl_display_dispatch_queue(conn.display, conn.queue) < 0)
            break;
    }

    /* The desktop's extent, and the most pixels any monitor has to a
     * logical pixel. */
    GdkRectangle bounds = {0};
    double scale = 1;
    bool any = false;
    for (int i = 0; i < started; i++) {
        monitor_copy *copy = &copies[i];
        if (!copy->done || copy->geometry.width <= 0)
            continue;
        if (!any)
            bounds = copy->geometry;
        else
            gdk_rectangle_union(&bounds, &copy->geometry, &bounds);
        scale = fmax(scale, (double)copy->width / copy->geometry.width);
        any = true;
    }

    xd_snip_screen *screen = nullptr;
    if (any) {
        screen = g_new0(xd_snip_screen, 1);
        screen->origin_x = bounds.x;
        screen->origin_y = bounds.y;
        screen->scale = scale;
        double width = ceil(bounds.width * scale), height = ceil(bounds.height * scale);
        int image_width = (int)width, image_height = (int)height;
        screen->image = cairo_image_surface_create(CAIRO_FORMAT_RGB24, image_width, image_height);
        cairo_t *cairo = cairo_create(screen->image);
        cairo_set_source_rgb(cairo, 0, 0, 0);
        cairo_paint(cairo);
        for (int i = 0; i < started; i++) {
            monitor_copy *copy = &copies[i];
            if (!copy->done || copy->image == nullptr)
                continue;
            cairo_save(cairo);
            cairo_translate(cairo, (copy->geometry.x - bounds.x) * scale,
                            (copy->geometry.y - bounds.y) * scale);
            double monitor_scale = copy->geometry.width * scale / copy->width;
            cairo_scale(cairo, monitor_scale, monitor_scale);
            cairo_set_source_surface(cairo, copy->image, 0, 0);
            if (monitor_scale == 1)
                cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_NEAREST);
            cairo_paint(cairo);
            cairo_restore(cairo);
        }
        cairo_destroy(cairo);
        cairo_surface_flush(screen->image);
        screen->window_count = found.windows->len;
        screen->windows = (xd_snip_window *)(void *)g_array_free(found.windows, FALSE);
        found.windows = nullptr;
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The screen couldn't be captured.");
    }

    if (found.windows != nullptr)
        g_array_unref(found.windows);
    if (list != nullptr)
        xd_window_rectangles_v1_destroy(list);
    for (int i = 0; i < started; i++)
        free_copy(&copies[i]);
    g_free(copies);
    disconnect_globals(&conn);
    return screen;
}

/* ---- the clipboard ---------------------------------------------------------------------------- */

static cairo_status_t append_png(void *closure, const unsigned char *bytes, unsigned int length) {
    g_byte_array_append(closure, bytes, length);
    return CAIRO_STATUS_SUCCESS;
}

GBytes *snip_png_bytes(cairo_surface_t *snip) {
    GByteArray *data = g_byte_array_new();
    cairo_surface_write_to_png_stream(snip, append_png, data);
    return g_byte_array_free_to_bytes(data);
}

typedef struct clipboard {
    connection conn;
    struct ext_data_control_device_v1 *device;
    struct ext_data_control_source_v1 *source;
    GBytes *png;
} clipboard;

static void written(GObject *stream, GAsyncResult *result, [[maybe_unused]] gpointer data) {
    g_output_stream_write_all_finish(G_OUTPUT_STREAM(stream), result, nullptr, nullptr);
    g_object_unref(stream);
}

static void handle_send(void *data, [[maybe_unused]] struct ext_data_control_source_v1 *source,
                        const char *mime_type, int32_t fd) {
    clipboard *board = data;
    if (strcmp(mime_type, PNG_TYPE) != 0) {
        close(fd);
        return;
    }
    /* Written as the main loop runs, however long the reader takes. */
    GOutputStream *stream = g_unix_output_stream_new(fd, TRUE);
    gsize size;
    const void *bytes = g_bytes_get_data(board->png, &size);
    g_output_stream_write_all_async(stream, bytes, size, G_PRIORITY_DEFAULT, nullptr, written,
                                    nullptr);
}

static void handle_cancelled(void *data,
                             [[maybe_unused]] struct ext_data_control_source_v1 *source) {
    clipboard *board = data;
    ext_data_control_source_v1_destroy(board->source);
    board->source = nullptr;
    gtk_main_quit();
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = handle_send,
    .cancelled = handle_cancelled,
};

bool snip_clipboard_serve(GdkDisplay *display, GBytes *png, GError **error) {
    static clipboard board;
    if (!connect_globals(display, &board.conn, error))
        return false;
    GdkSeat *seat = gdk_display_get_default_seat(display);
    struct wl_seat *wl_seat = seat != nullptr ? gdk_wayland_seat_get_wl_seat(seat) : nullptr;
    if (board.conn.globals.data_control == nullptr || wl_seat == nullptr) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "This desktop doesn't let programs use the clipboard without a "
                            "window.");
        disconnect_globals(&board.conn);
        return false;
    }
    board.png = g_bytes_ref(png);
    struct ext_data_control_manager_v1 *manager = board.conn.globals.data_control;
    board.device = ext_data_control_manager_v1_get_data_device(manager, wl_seat);
    board.source = ext_data_control_manager_v1_create_data_source(manager);
    /* The source's events come as GTK's do, on the default queue: GTK's
     * main loop runs them. */
    wl_proxy_set_queue((struct wl_proxy *)board.source, nullptr);
    wl_proxy_set_queue((struct wl_proxy *)board.device, nullptr);
    ext_data_control_source_v1_add_listener(board.source, &source_listener, &board);
    ext_data_control_source_v1_offer(board.source, PNG_TYPE);
    ext_data_control_device_v1_set_selection(board.device, board.source);
    wl_display_flush(board.conn.display);
    return true;
}

void snip_clipboard_copy(cairo_surface_t *snip) {
    g_autofree char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *argv[] = {self != nullptr ? self : (char *)(uintptr_t)"xd-snip",
                    (char *)(uintptr_t)"--copy-stdin", nullptr};
    GError *error = nullptr;
    int input = -1;
    if (!g_spawn_async_with_pipes(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                                  nullptr, &input, nullptr, nullptr, &error)) {
        g_warning("cannot copy the snip: %s", error->message);
        g_error_free(error);
        return;
    }
    g_autoptr(GBytes) png = snip_png_bytes(snip);
    GOutputStream *stream = g_unix_output_stream_new(input, TRUE);
    gsize size;
    const void *bytes = g_bytes_get_data(png, &size);
    /* A snip is a few MB at most; the child reads it all at once. */
    g_output_stream_write_all(stream, bytes, size, nullptr, nullptr, nullptr);
    g_object_unref(stream);
}
