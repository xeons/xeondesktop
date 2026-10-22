/* SPDX-License-Identifier: MIT
 * Screen recording, as Windows 11's Snipping Tool (and the Game Bar's
 * Win+Alt+R) records: a part of one monitor, with the pointer, into an
 * MP4 (H.264, and AAC sound from the microphone and/or what the speakers
 * play) in Videos/Screen Recordings.
 *
 * The frames come from wlr-screencopy, on a Wayland connection of our
 * own (GTK's dispatches only its own queue), at most 30 a second, into a
 * GStreamer pipeline (recording.c) through an appsrc. While it records, a
 * dashed red frame stands around the area (a layer-shell surface that
 * takes no input), and a red dot in the notification area (a
 * StatusNotifierItem) says for how long; clicking it, or the shortcut
 * again (org.xeondesktop.ScreenRecorder's Stop), stops it. Once the file
 * is finished, a notification offers to open it.
 */
#include <fcntl.h>
#include <gdk/gdkwayland.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gtk-layer-shell.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../ui/protocols.h"
#include "recording.h"
#include "snip.h"

#define ITEM_PATH "/StatusNotifierItem"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define NOTIFY_NAME "org.freedesktop.Notifications"
#define NOTIFY_PATH "/org/freedesktop/Notifications"

static constexpr gint64 FRAME_INTERVAL_US = G_USEC_PER_SEC / 30;
static constexpr guint RETRY_MS = 100;
static constexpr int FAILURES_MAX = 20;
/* How long the file may take to be finished before giving up on it. */
static constexpr guint FINISH_TIMEOUT_S = 10;
/* Logical px: the frame around the recorded area. */
static constexpr int FRAME_WIDTH = 2;

typedef struct output_place {
    struct wl_output *output;
    struct zxdg_output_v1 *xdg;
    int x, y;
} output_place;

struct snip_recording {
    snip_app *app;
    snip_recording_done done;
    void *done_data;
    xd_snip_box area;     /* in the desktop */
    GdkRectangle monitor; /* its monitor, in the desktop */
    xd_record_sound sound;
    char *path;
    gint64 started_us;

    /* Wayland */
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct zwlr_screencopy_manager_v1 *screencopy;
    uint32_t screencopy_version;
    struct zxdg_output_manager_v1 *xdg_outputs;
    output_place outputs[16];
    size_t output_count;
    struct wl_output *output; /* the monitor's */
    guint display_watch;
    struct zwlr_screencopy_frame_v1 *frame;
    gint64 frame_started_us;
    guint next_frame;
    int failures;
    /* The shared buffer frames are copied into. */
    uint32_t format;
    int width, height, stride;
    bool offered;
    struct wl_buffer *buffer;
    void *pixels;
    size_t size;
    bool y_invert;

    /* GStreamer */
    GstElement *pipeline;
    GstElement *video;
    guint bus_watch;
    guint finish_timeout;
    bool stopping;
    bool finished;

    /* What the user sees */
    GtkWidget *frame_window;
    GDBusConnection *bus;
    guint name_id;
    guint item_object;
    guint control_object;
    guint tick;
};

/* ---- the end ---------------------------------------------------------------------------- */

static void free_buffer(snip_recording *recording) {
    if (recording->buffer != nullptr)
        wl_buffer_destroy(recording->buffer);
    if (recording->pixels != nullptr)
        munmap(recording->pixels, recording->size);
    recording->buffer = nullptr;
    recording->pixels = nullptr;
    recording->offered = false;
}

static void disconnect_display(snip_recording *recording) {
    if (recording->next_frame != 0)
        g_source_remove(recording->next_frame);
    recording->next_frame = 0;
    if (recording->display_watch != 0)
        g_source_remove(recording->display_watch);
    recording->display_watch = 0;
    if (recording->display == nullptr)
        return;
    if (recording->frame != nullptr)
        zwlr_screencopy_frame_v1_destroy(recording->frame);
    recording->frame = nullptr;
    free_buffer(recording);
    for (size_t i = 0; i < recording->output_count; i++) {
        if (recording->outputs[i].xdg != nullptr)
            zxdg_output_v1_destroy(recording->outputs[i].xdg);
        wl_output_destroy(recording->outputs[i].output);
    }
    recording->output_count = 0;
    if (recording->xdg_outputs != nullptr)
        zxdg_output_manager_v1_destroy(recording->xdg_outputs);
    if (recording->screencopy != nullptr)
        zwlr_screencopy_manager_v1_destroy(recording->screencopy);
    if (recording->shm != nullptr)
        wl_shm_destroy(recording->shm);
    if (recording->registry != nullptr)
        wl_registry_destroy(recording->registry);
    wl_display_disconnect(recording->display);
    recording->display = nullptr;
}

static void hide_indicators(snip_recording *recording) {
    if (recording->tick != 0)
        g_source_remove(recording->tick);
    recording->tick = 0;
    if (recording->frame_window != nullptr)
        gtk_widget_destroy(recording->frame_window);
    recording->frame_window = nullptr;
    if (recording->bus != nullptr) {
        if (recording->item_object != 0)
            g_dbus_connection_unregister_object(recording->bus, recording->item_object);
        if (recording->control_object != 0)
            g_dbus_connection_unregister_object(recording->bus, recording->control_object);
    }
    recording->item_object = 0;
    recording->control_object = 0;
    if (recording->name_id != 0)
        g_bus_unown_name(recording->name_id);
    recording->name_id = 0;
    /* The tray drops an item when its connection goes. */
    if (recording->bus != nullptr)
        g_dbus_connection_close_sync(recording->bus, nullptr, nullptr);
    g_clear_object(&recording->bus);
}

/* Everything's done: the file is finished (or isn't, with `error`). */
static void finish(snip_recording *recording, const char *error) {
    if (recording->finished)
        return;
    recording->finished = true;
    disconnect_display(recording);
    hide_indicators(recording);
    if (recording->finish_timeout != 0)
        g_source_remove(recording->finish_timeout);
    recording->finish_timeout = 0;
    if (recording->bus_watch != 0)
        g_source_remove(recording->bus_watch);
    recording->bus_watch = 0;
    if (recording->pipeline != nullptr) {
        gst_element_set_state(recording->pipeline, GST_STATE_NULL);
        gst_object_unref(recording->video);
        gst_object_unref(recording->pipeline);
    }
    recording->pipeline = nullptr;
    recording->video = nullptr;
    if (error != nullptr && recording->path != nullptr)
        g_unlink(recording->path);
    recording->done(recording, error == nullptr ? recording->path : nullptr, error,
                    recording->done_data);
}

void snip_recording_free(snip_recording *recording) {
    if (recording == nullptr)
        return;
    g_free(recording->path);
    g_free(recording);
}

static gboolean finish_timed_out(gpointer data) {
    snip_recording *recording = data;
    recording->finish_timeout = 0;
    finish(recording, "The recording couldn't be finished in time.");
    return G_SOURCE_REMOVE;
}

void snip_recording_stop(snip_recording *recording) {
    if (recording->stopping || recording->finished)
        return;
    recording->stopping = true;
    disconnect_display(recording);
    hide_indicators(recording);
    if (recording->pipeline == nullptr) {
        finish(recording, "Nothing was recorded: the screen couldn't be captured.");
        return;
    }
    /* The sources end, and the muxer writes what an MP4 needs at its end
     * before the EOS reaches the bus. */
    gst_element_send_event(recording->pipeline, gst_event_new_eos());
    recording->finish_timeout = g_timeout_add_seconds(FINISH_TIMEOUT_S, finish_timed_out, recording);
}

static gboolean on_bus_message([[maybe_unused]] GstBus *bus, GstMessage *message, gpointer data) {
    snip_recording *recording = data;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        recording->bus_watch = 0;
        finish(recording, nullptr);
        return G_SOURCE_REMOVE;
    }
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = nullptr;
        g_autofree char *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        g_printerr("xd-snip: recording: %s (%s)\n", error->message, debug != nullptr ? debug : "");
        g_autofree char *text = g_strdup_printf("The recording stopped: %s", error->message);
        g_error_free(error);
        recording->bus_watch = 0;
        finish(recording, text);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the pipeline ----------------------------------------------------------------------- */

static bool element_exists(const char *name) {
    GstElementFactory *factory = gst_element_factory_find(name);
    if (factory == nullptr)
        return false;
    gst_object_unref(factory);
    return true;
}

/* The H.264 encoder there is, with its settings: x264 first (better
 * pictures for the time it takes), then Cisco's OpenH264. */
static const char *video_encoder(void) {
    if (element_exists("x264enc"))
        return "x264enc speed-preset=veryfast pass=qual quantizer=21 key-int-max=60";
    if (element_exists("openh264enc"))
        return "openh264enc bitrate=8000000 complexity=low";
    return nullptr;
}

static const char *audio_encoder(void) {
    if (element_exists("fdkaacenc"))
        return "fdkaacenc";
    if (element_exists("avenc_aac"))
        return "avenc_aac";
    return nullptr;
}

bool snip_recording_possible(xd_record_sound sound, GError **error) {
    if (!gst_is_initialized() && !gst_init_check(nullptr, nullptr, error))
        return false;
    const char *missing = nullptr;
    if (video_encoder() == nullptr)
        missing = "an H.264 encoder (gst-plugins-ugly's x264enc, or gst-plugins-bad's openh264enc)";
    else if (!element_exists("mp4mux"))
        missing = "the MP4 writer (gst-plugins-good)";
    else if (sound != XD_RECORD_SILENT && !element_exists("pulsesrc"))
        missing = "the sound source (gst-plugins-good's pulsesrc)";
    else if (sound != XD_RECORD_SILENT && audio_encoder() == nullptr)
        missing = "an AAC encoder (gst-libav, or gst-plugins-bad's fdkaacenc)";
    if (missing == nullptr)
        return true;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Screen recording needs %s. Install it, then try again.", missing);
    return false;
}

static bool start_pipeline(snip_recording *recording, int width, int height, GError **error) {
    g_autofree char *description = xd_record_pipeline(
        video_encoder(), recording->sound != XD_RECORD_SILENT ? audio_encoder() : nullptr,
        recording->sound);
    GstElement *pipeline = gst_parse_launch(description, error);
    if (pipeline == nullptr)
        return false;
    GstElement *file = gst_bin_get_by_name(GST_BIN(pipeline), "file");
    g_object_set(file, "location", recording->path, nullptr);
    gst_object_unref(file);
    recording->video = gst_bin_get_by_name(GST_BIN(pipeline), "video");
    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, xd_record_video_format(recording->format),
        "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION,
        0, 1, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
    gst_app_src_set_caps(GST_APP_SRC(recording->video), caps);
    gst_caps_unref(caps);
    GstBus *bus = gst_element_get_bus(pipeline);
    recording->bus_watch = gst_bus_add_watch(bus, on_bus_message, recording);
    gst_object_unref(bus);
    recording->pipeline = pipeline;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The recording couldn't start (the sound device, or the file?).");
        return false;
    }
    return true;
}

/* ---- frames ------------------------------------------------------------------------------- */

static void capture_frame(snip_recording *recording);

static gboolean next_frame_due(gpointer data) {
    snip_recording *recording = data;
    recording->next_frame = 0;
    capture_frame(recording);
    return G_SOURCE_REMOVE;
}

/* The next frame, a frame's time after this one started. */
static void schedule_frame(snip_recording *recording, gint64 delay_us) {
    if (recording->stopping || recording->display == nullptr)
        return;
    guint delay_ms = delay_us > 0 ? (guint)(delay_us / 1000) : 0;
    recording->next_frame = g_timeout_add(delay_ms, next_frame_due, recording);
}

static void frame_failed(snip_recording *recording) {
    if (recording->frame != nullptr)
        zwlr_screencopy_frame_v1_destroy(recording->frame);
    recording->frame = nullptr;
    if (++recording->failures > FAILURES_MAX) {
        if (recording->pipeline == nullptr)
            finish(recording, "The screen couldn't be captured.");
        else
            snip_recording_stop(recording);
        return;
    }
    schedule_frame(recording, RETRY_MS * 1000);
}

static void copy_into_buffer(snip_recording *recording) {
    if (!recording->offered || recording->shm == nullptr) {
        frame_failed(recording);
        return;
    }
    if (recording->buffer == nullptr) {
        recording->size = (size_t)recording->stride * (size_t)recording->height;
        int fd = memfd_create("xd-snip-record", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)recording->size) != 0) {
            if (fd >= 0)
                close(fd);
            frame_failed(recording);
            return;
        }
        void *pixels = mmap(nullptr, recording->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (pixels == MAP_FAILED) {
            close(fd);
            frame_failed(recording);
            return;
        }
        recording->pixels = pixels;
        struct wl_shm_pool *pool = wl_shm_create_pool(recording->shm, fd, (int)recording->size);
        recording->buffer = wl_shm_pool_create_buffer(pool, 0, recording->width, recording->height,
                                                      recording->stride, recording->format);
        wl_shm_pool_destroy(pool);
        close(fd);
    }
    zwlr_screencopy_frame_v1_copy(recording->frame, recording->buffer);
}

static void handle_buffer(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                          uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    snip_recording *recording = data;
    if (xd_record_video_format(format) == nullptr || width < 2 || height < 2 ||
        width > INT32_MAX / 4 || stride < width * 4)
        return;
    bool same = recording->offered && recording->format == format &&
                recording->width == (int)width && recording->height == (int)height &&
                recording->stride == (int)stride;
    if (!same && recording->buffer != nullptr) {
        /* The size can't change once the video has one. */
        if (recording->pipeline != nullptr)
            return;
        free_buffer(recording);
    }
    recording->format = format;
    recording->width = (int)width;
    recording->height = (int)height;
    recording->stride = (int)stride;
    recording->offered = true;
    if (recording->screencopy_version < 3)
        copy_into_buffer(recording);
}

static void handle_buffer_done(void *data,
                               [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame) {
    copy_into_buffer(data);
}

static void handle_flags(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t flags) {
    snip_recording *recording = data;
    recording->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

/* The copied frame to the encoder; H.264's 4:2:0 wants even sides. */
static void push_frame(snip_recording *recording) {
    int width = recording->width & ~1, height = recording->height & ~1;
    if (recording->pipeline == nullptr) {
        GError *error = nullptr;
        if (!start_pipeline(recording, width, height, &error)) {
            g_autofree char *text = g_strdup(error->message);
            g_error_free(error);
            finish(recording, text);
            return;
        }
    }
    size_t row_bytes = (size_t)width * 4;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, row_bytes * (size_t)height, nullptr);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return;
    }
    for (int row = 0; row < height; row++) {
        int source_row = recording->y_invert ? recording->height - 1 - row : row;
        memcpy(map.data + row_bytes * (size_t)row,
               (const unsigned char *)recording->pixels + (size_t)source_row * (size_t)recording->stride,
               row_bytes);
    }
    gst_buffer_unmap(buffer, &map);
    gst_app_src_push_buffer(GST_APP_SRC(recording->video), buffer);
}

static void handle_ready(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                         [[maybe_unused]] uint32_t tv_sec_hi, [[maybe_unused]] uint32_t tv_sec_lo,
                         [[maybe_unused]] uint32_t tv_nsec) {
    snip_recording *recording = data;
    zwlr_screencopy_frame_v1_destroy(recording->frame);
    recording->frame = nullptr;
    recording->failures = 0;
    push_frame(recording);
    if (recording->finished)
        return;
    gint64 elapsed = g_get_monotonic_time() - recording->frame_started_us;
    schedule_frame(recording, FRAME_INTERVAL_US - elapsed);
}

static void handle_failed(void *data, [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame) {
    frame_failed(data);
}

static void handle_damage([[maybe_unused]] void *data,
                          [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                          [[maybe_unused]] uint32_t x, [[maybe_unused]] uint32_t y,
                          [[maybe_unused]] uint32_t width, [[maybe_unused]] uint32_t height) {}

static void handle_linux_dmabuf([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zwlr_screencopy_frame_v1 *frame,
                                [[maybe_unused]] uint32_t format, [[maybe_unused]] uint32_t width,
                                [[maybe_unused]] uint32_t height) {}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = handle_buffer,
    .flags = handle_flags,
    .ready = handle_ready,
    .failed = handle_failed,
    .damage = handle_damage,
    .linux_dmabuf = handle_linux_dmabuf,
    .buffer_done = handle_buffer_done,
};

static void capture_frame(snip_recording *recording) {
    if (recording->stopping || recording->display == nullptr || recording->frame != nullptr)
        return;
    recording->frame_started_us = g_get_monotonic_time();
    /* Output-local logical coordinates; the pointer drawn in. */
    double x = recording->area.x - recording->monitor.x;
    double y = recording->area.y - recording->monitor.y;
    recording->frame = zwlr_screencopy_manager_v1_capture_output_region(
        recording->screencopy, 1, recording->output, (int32_t)x, (int32_t)y,
        (int32_t)recording->area.width, (int32_t)recording->area.height);
    zwlr_screencopy_frame_v1_add_listener(recording->frame, &frame_listener, recording);
    wl_display_flush(recording->display);
}

/* ---- the Wayland connection -------------------------------------------------------------- */

static void handle_logical_position(void *data, [[maybe_unused]] struct zxdg_output_v1 *xdg,
                                    int32_t x, int32_t y) {
    output_place *place = data;
    place->x = x;
    place->y = y;
}

static void handle_logical_size([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zxdg_output_v1 *xdg,
                                [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}
static void handle_xdg_done([[maybe_unused]] void *data,
                            [[maybe_unused]] struct zxdg_output_v1 *xdg) {}
static void handle_xdg_name([[maybe_unused]] void *data,
                            [[maybe_unused]] struct zxdg_output_v1 *xdg,
                            [[maybe_unused]] const char *name) {}
static void handle_xdg_description([[maybe_unused]] void *data,
                                   [[maybe_unused]] struct zxdg_output_v1 *xdg,
                                   [[maybe_unused]] const char *description) {}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = handle_logical_position,
    .logical_size = handle_logical_size,
    .done = handle_xdg_done,
    .name = handle_xdg_name,
    .description = handle_xdg_description,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t version) {
    snip_recording *recording = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        recording->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        recording->screencopy_version = MIN(version, 3);
        recording->screencopy = wl_registry_bind(
            registry, name, &zwlr_screencopy_manager_v1_interface, recording->screencopy_version);
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        recording->xdg_outputs =
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, MIN(version, 2));
    } else if (strcmp(interface, wl_output_interface.name) == 0 &&
               recording->output_count < G_N_ELEMENTS(recording->outputs)) {
        recording->outputs[recording->output_count++].output =
            wl_registry_bind(registry, name, &wl_output_interface, 1);
    }
}

static void handle_global_remove([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_registry *registry,
                                 [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static gboolean on_display([[maybe_unused]] gint fd, GIOCondition condition, gpointer data) {
    snip_recording *recording = data;
    if ((condition & (G_IO_ERR | G_IO_HUP)) != 0 || wl_display_dispatch(recording->display) < 0) {
        recording->display_watch = 0;
        if (recording->pipeline != nullptr)
            snip_recording_stop(recording);
        else
            finish(recording, "The connection to the desktop was lost.");
        return G_SOURCE_REMOVE;
    }
    wl_display_flush(recording->display);
    return G_SOURCE_CONTINUE;
}

static bool connect_display(snip_recording *recording, GError **error) {
    recording->display = wl_display_connect(nullptr);
    if (recording->display == nullptr) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The Snipping Tool couldn't connect to the desktop.");
        return false;
    }
    recording->registry = wl_display_get_registry(recording->display);
    wl_registry_add_listener(recording->registry, &registry_listener, recording);
    wl_display_roundtrip(recording->display);
    if (recording->screencopy == nullptr || recording->shm == nullptr ||
        recording->xdg_outputs == nullptr) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "This desktop doesn't let programs capture the screen.");
        return false;
    }
    for (size_t i = 0; i < recording->output_count; i++) {
        output_place *place = &recording->outputs[i];
        place->xdg = zxdg_output_manager_v1_get_xdg_output(recording->xdg_outputs, place->output);
        zxdg_output_v1_add_listener(place->xdg, &xdg_output_listener, place);
    }
    wl_display_roundtrip(recording->display);
    /* The monitor, known by where it is. */
    for (size_t i = 0; i < recording->output_count; i++) {
        if (recording->outputs[i].x == recording->monitor.x &&
            recording->outputs[i].y == recording->monitor.y)
            recording->output = recording->outputs[i].output;
    }
    if (recording->output == nullptr) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "The monitor to record has gone.");
        return false;
    }
    GIOCondition readable = (GIOCondition)(G_IO_IN | G_IO_ERR | G_IO_HUP);
    recording->display_watch =
        g_unix_fd_add(wl_display_get_fd(recording->display), readable, on_display, recording);
    return true;
}

/* ---- the frame around the area ---------------------------------------------------------- */

static gboolean draw_frame(GtkWidget *widget, cairo_t *cairo, [[maybe_unused]] gpointer data) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    static const double dashes[] = {6, 4};
    cairo_set_dash(cairo, dashes, 2, 0);
    cairo_set_line_width(cairo, FRAME_WIDTH);
    cairo_set_source_rgb(cairo, 0.9, 0.1, 0.1);
    double half = FRAME_WIDTH / 2.0;
    cairo_rectangle(cairo, half, half, width - FRAME_WIDTH, height - FRAME_WIDTH);
    cairo_stroke(cairo);
    return TRUE;
}

static void take_no_input(GtkWidget *widget, [[maybe_unused]] gpointer data) {
    cairo_region_t *nothing = cairo_region_create();
    gtk_widget_input_shape_combine_region(widget, nothing);
    cairo_region_destroy(nothing);
}

/* Just outside the area, when the monitor has room for it all round (a
 * whole monitor's recording has none). */
static void show_frame(snip_recording *recording, GdkMonitor *monitor) {
    double left = recording->area.x - recording->monitor.x - FRAME_WIDTH;
    double top = recording->area.y - recording->monitor.y - FRAME_WIDTH;
    double right = left + recording->area.width + 2 * FRAME_WIDTH;
    double bottom = top + recording->area.height + 2 * FRAME_WIDTH;
    if (left < 0 || top < 0 || right > recording->monitor.width ||
        bottom > recording->monitor.height)
        return;
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GdkScreen *screen = gtk_widget_get_screen(window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual != nullptr)
        gtk_widget_set_visual(window, visual);
    gtk_widget_set_app_paintable(window, TRUE);
    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_namespace(GTK_WINDOW(window), "xd-snip-recording");
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_monitor(GTK_WINDOW(window), monitor);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, (int)left);
    gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, (int)top);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_widget_set_size_request(window, (int)(right - left), (int)(bottom - top));
    g_signal_connect(window, "draw", G_CALLBACK(draw_frame), recording);
    g_signal_connect(window, "realize", G_CALLBACK(take_no_input), recording);
    gtk_widget_show(window);
    recording->frame_window = window;
}

/* ---- the tray icon, and Stop over D-Bus ---------------------------------------------------- */

static const char item_xml[] =
    "<node><interface name='" ITEM_IFACE "'>"
    "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<signal name='NewToolTip'/>"
    "</interface></node>";

static const char control_xml[] = "<node><interface name='" SNIP_RECORDER_NAME "'>"
                                  "<method name='Stop'/>"
                                  "</interface></node>";

static char *elapsed_text(const snip_recording *recording) {
    gint64 seconds = (g_get_monotonic_time() - recording->started_us) / G_USEC_PER_SEC;
    return g_strdup_printf("Recording the screen: %d:%02d", (int)(seconds / 60),
                           (int)(seconds % 60));
}

static void item_call([[maybe_unused]] GDBusConnection *connection,
                      [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                      [[maybe_unused]] const char *iface, const char *method,
                      [[maybe_unused]] GVariant *params, GDBusMethodInvocation *invocation,
                      gpointer data) {
    snip_recording *recording = data;
    g_dbus_method_invocation_return_value(invocation, nullptr);
    /* Any click on the red dot stops it; scrolling doesn't. */
    if (strcmp(method, "Scroll") != 0)
        snip_recording_stop(recording);
}

static GVariant *item_get([[maybe_unused]] GDBusConnection *connection,
                          [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                          [[maybe_unused]] const char *iface, const char *property,
                          [[maybe_unused]] GError **error, gpointer data) {
    snip_recording *recording = data;
    if (strcmp(property, "Category") == 0)
        return g_variant_new_string("ApplicationStatus");
    if (strcmp(property, "Id") == 0)
        return g_variant_new_string("xd-snip-recording");
    if (strcmp(property, "Title") == 0)
        return g_variant_new_string("Screen Recording");
    if (strcmp(property, "Status") == 0)
        return g_variant_new_string("Active");
    if (strcmp(property, "IconName") == 0)
        return g_variant_new_string("media-record");
    if (strcmp(property, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);
    if (strcmp(property, "Menu") == 0)
        return g_variant_new_object_path("/");
    if (strcmp(property, "ToolTip") == 0) {
        g_autofree char *title = elapsed_text(recording);
        return g_variant_new("(s@a(iiay)ss)", "media-record",
                             g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0), title,
                             "Click to stop recording");
    }
    return nullptr;
}

static void control_call([[maybe_unused]] GDBusConnection *connection,
                         [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                         [[maybe_unused]] const char *iface, [[maybe_unused]] const char *method,
                         [[maybe_unused]] GVariant *params, GDBusMethodInvocation *invocation,
                         gpointer data) {
    g_dbus_method_invocation_return_value(invocation, nullptr);
    snip_recording_stop(data);
}

static gboolean tick(gpointer data) {
    snip_recording *recording = data;
    if (recording->bus != nullptr)
        g_dbus_connection_emit_signal(recording->bus, nullptr, ITEM_PATH, ITEM_IFACE, "NewToolTip",
                                      nullptr, nullptr);
    return G_SOURCE_CONTINUE;
}

static void show_tray_item(snip_recording *recording) {
    GError *error = nullptr;
    /* A connection of its own, closed when the recording stops: the tray
     * icon goes then, while the program may stay for the notification. */
    g_autofree char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (address != nullptr)
        recording->bus = g_dbus_connection_new_for_address_sync(
            address,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            nullptr, nullptr, &error);
    if (recording->bus == nullptr) {
        g_printerr("xd-snip: no session bus, so no tray icon: %s\n", error->message);
        g_error_free(error);
        return;
    }
    static const GDBusInterfaceVTable item_vtable = {item_call, item_get, nullptr, {nullptr}};
    static const GDBusInterfaceVTable control_vtable = {control_call, nullptr, nullptr, {nullptr}};
    g_autoptr(GDBusNodeInfo) item_info = g_dbus_node_info_new_for_xml(item_xml, nullptr);
    g_autoptr(GDBusNodeInfo) control_info = g_dbus_node_info_new_for_xml(control_xml, nullptr);
    recording->item_object = g_dbus_connection_register_object(
        recording->bus, ITEM_PATH, item_info->interfaces[0], &item_vtable, recording, nullptr,
        nullptr);
    recording->control_object = g_dbus_connection_register_object(
        recording->bus, SNIP_RECORDER_PATH, control_info->interfaces[0], &control_vtable,
        recording, nullptr, nullptr);
    /* The shortcut, pressed again, finds us by this name. */
    recording->name_id = g_bus_own_name_on_connection(
        recording->bus, SNIP_RECORDER_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr,
        nullptr, nullptr);
    g_dbus_connection_call(recording->bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
                           "RegisterStatusNotifierItem", g_variant_new("(s)", ITEM_PATH), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    recording->tick = g_timeout_add_seconds(1, tick, recording);
}

/* ---- starting ------------------------------------------------------------------------------ */

/* A name of its own in the folder, "Screen Recording ... (2).mp4" if the
 * second already has one. */
static char *recording_path(GError **error) {
    const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    g_autofree char *folder = xd_record_folder(videos, g_get_home_dir());
    if (g_mkdir_with_parents(folder, 0755) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s can't be made.", folder);
        return nullptr;
    }
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *name = xd_record_file_name(now);
    char *path = g_build_filename(folder, name, nullptr);
    for (int copy = 2; g_file_test(path, G_FILE_TEST_EXISTS) && copy < 100; copy++) {
        g_free(path);
        g_autofree char *stem = g_strndup(name, strlen(name) - strlen(".mp4"));
        g_autofree char *numbered = g_strdup_printf("%s (%d).mp4", stem, copy);
        path = g_build_filename(folder, numbered, nullptr);
    }
    return path;
}

snip_recording *snip_recording_start(snip_app *app, GdkMonitor *monitor, xd_snip_box area,
                                     xd_record_sound sound, snip_recording_done done,
                                     void *done_data, GError **error) {
    if (!snip_recording_possible(sound, error))
        return nullptr;
    snip_recording *recording = g_new0(snip_recording, 1);
    recording->app = app;
    recording->done = done;
    recording->done_data = done_data;
    recording->area = area;
    recording->sound = sound;
    gdk_monitor_get_geometry(monitor, &recording->monitor);
    recording->path = recording_path(error);
    if (recording->path == nullptr || !connect_display(recording, error)) {
        disconnect_display(recording);
        snip_recording_free(recording);
        return nullptr;
    }
    recording->started_us = g_get_monotonic_time();
    show_frame(recording, monitor);
    show_tray_item(recording);
    capture_frame(recording);
    return recording;
}

/* ---- once it's saved ------------------------------------------------------------------------ */

typedef struct saved_notice {
    GDBusConnection *bus;
    char *path;
    uint32_t id;
    guint action_signal, closed_signal, timeout;
    GSourceFunc gone;
    gpointer gone_data;
} saved_notice;

static void forget_notice(saved_notice *notice) {
    if (notice->action_signal != 0)
        g_dbus_connection_signal_unsubscribe(notice->bus, notice->action_signal);
    if (notice->closed_signal != 0)
        g_dbus_connection_signal_unsubscribe(notice->bus, notice->closed_signal);
    if (notice->timeout != 0)
        g_source_remove(notice->timeout);
    GSourceFunc gone = notice->gone;
    gpointer gone_data = notice->gone_data;
    g_object_unref(notice->bus);
    g_free(notice->path);
    g_free(notice);
    if (gone != nullptr)
        gone(gone_data);
}

static void open_uri(const char *path, bool folder) {
    g_autofree char *target = folder ? g_path_get_dirname(path) : g_strdup(path);
    g_autofree char *uri = g_filename_to_uri(target, nullptr, nullptr);
    GError *error = nullptr;
    if (uri != nullptr && !g_app_info_launch_default_for_uri(uri, nullptr, &error)) {
        g_printerr("xd-snip: cannot open %s: %s\n", uri, error->message);
        g_error_free(error);
    }
}

static void on_notice_signal([[maybe_unused]] GDBusConnection *connection,
                             [[maybe_unused]] const char *sender,
                             [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                             const char *signal, GVariant *params, gpointer data) {
    saved_notice *notice = data;
    uint32_t id = 0;
    if (strcmp(signal, "ActionInvoked") == 0) {
        const char *action = nullptr;
        g_variant_get(params, "(u&s)", &id, &action);
        if (id != notice->id)
            return;
        open_uri(notice->path, strcmp(action, "folder") == 0);
        forget_notice(notice);
        return;
    }
    g_variant_get(params, "(uu)", &id, nullptr);
    if (id == notice->id)
        forget_notice(notice);
}

static gboolean notice_timed_out(gpointer data) {
    saved_notice *notice = data;
    notice->timeout = 0;
    forget_notice(notice);
    return G_SOURCE_REMOVE;
}

static void notified(GObject *source, GAsyncResult *result, gpointer data) {
    saved_notice *notice = data;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, nullptr);
    if (reply == nullptr) {
        forget_notice(notice);
        return;
    }
    g_variant_get(reply, "(u)", &notice->id);
    g_variant_unref(reply);
}

void snip_recording_notify_saved(const char *path, GSourceFunc gone, gpointer gone_data) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus == nullptr) {
        if (gone != nullptr)
            gone(gone_data);
        return;
    }
    saved_notice *notice = g_new0(saved_notice, 1);
    notice->bus = bus;
    notice->path = g_strdup(path);
    notice->gone = gone;
    notice->gone_data = gone_data;
    notice->action_signal = g_dbus_connection_signal_subscribe(
        bus, NOTIFY_NAME, NOTIFY_NAME, "ActionInvoked", NOTIFY_PATH, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_notice_signal, notice, nullptr);
    notice->closed_signal = g_dbus_connection_signal_subscribe(
        bus, NOTIFY_NAME, NOTIFY_NAME, "NotificationClosed", NOTIFY_PATH, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_notice_signal, notice, nullptr);
    /* Kept for a while, then the program may go: the history still has it. */
    notice->timeout = g_timeout_add_seconds(120, notice_timed_out, notice);

    g_autofree char *name = g_path_get_basename(path);
    g_autofree char *body = g_strdup_printf("%s is in Videos, in Screen Recordings.", name);
    static const char *const actions[] = {"default", "Open", "folder", "Show in Folder", nullptr};
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(SNIP_APP_ID));
    g_dbus_connection_call(bus, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME, "Notify",
                           g_variant_new("(susss^asa{sv}i)", "Snipping Tool", 0u, "media-record",
                                         "Screen recording saved", body, actions, &hints, -1),
                           G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, notified,
                           notice);
}

bool snip_recording_stop_running(void) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus == nullptr)
        return false;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, SNIP_RECORDER_NAME, SNIP_RECORDER_PATH, SNIP_RECORDER_NAME, "Stop", nullptr, nullptr,
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, nullptr, nullptr);
    g_object_unref(bus);
    if (reply == nullptr)
        return false;
    g_variant_unref(reply);
    return true;
}
