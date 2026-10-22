/* SPDX-License-Identifier: MIT */
#include "recording.h"

#include <math.h>
#include <string.h>
#include <wayland-client-protocol.h>

/* The video's frame rate: the screen is captured this often at most, and
 * still stretches repeat the last frame. */
#define FRAME_RATE "30"

char *xd_record_file_name(GDateTime *time) {
    g_autofree char *stamp = g_date_time_format(time, "%Y-%m-%d %H%M%S");
    return g_strdup_printf("Screen Recording %s.mp4", stamp);
}

char *xd_record_folder(const char *videos, const char *home) {
    if (videos == nullptr || strcmp(videos, home) == 0)
        return g_build_filename(home, "Videos", "Screen Recordings", nullptr);
    return g_build_filename(videos, "Screen Recordings", nullptr);
}

bool xd_record_fit(xd_snip_box selection, xd_snip_box monitor, xd_snip_box *area) {
    double left = round(fmax(selection.x, monitor.x));
    double top = round(fmax(selection.y, monitor.y));
    double right = round(fmin(selection.x + selection.width, monitor.x + monitor.width));
    double bottom = round(fmin(selection.y + selection.height, monitor.y + monitor.height));
    if (right <= left || bottom <= top)
        return false;
    *area = (xd_snip_box){left, top, right - left, bottom - top};
    return true;
}

const char *xd_record_video_format(uint32_t shm_format) {
    /* wl_shm's are little-endian words, GStreamer's are bytes in order. */
    switch (shm_format) {
    case WL_SHM_FORMAT_XRGB8888:
        return "BGRx";
    case WL_SHM_FORMAT_ARGB8888:
        return "BGRA";
    case WL_SHM_FORMAT_XBGR8888:
        return "RGBx";
    case WL_SHM_FORMAT_ABGR8888:
        return "RGBA";
    default:
        return nullptr;
    }
}

/* One sound source, ready to mix or encode: all the same (a mono
 * microphone and stereo speakers mix). */
static void append_source(GString *pipeline, bool speakers) {
    g_string_append(pipeline, "pulsesrc");
    if (speakers)
        g_string_append(pipeline, " device=@DEFAULT_MONITOR@");
    g_string_append(pipeline, " ! audioconvert ! audioresample "
                              "! audio/x-raw,rate=48000,channels=2 ! queue");
}

char *xd_record_pipeline(const char *video_encoder, const char *audio_encoder,
                         xd_record_sound sound) {
    GString *pipeline = g_string_new(nullptr);
    /* Frames are stamped as they come, against the clock the sound
     * sources keep. A slow encoder drops old frames, not the sound. */
    g_string_append_printf(pipeline,
                           "appsrc name=video is-live=true do-timestamp=true format=time "
                           "max-buffers=4 leaky-type=downstream "
                           "! videorate ! video/x-raw,framerate=" FRAME_RATE "/1 "
                           "! videoconvert ! video/x-raw,format=I420 "
                           "! queue ! %s ! h264parse "
                           "! queue ! mp4mux name=mux ! filesink name=file",
                           video_encoder);
    if (audio_encoder == nullptr || sound == XD_RECORD_SILENT)
        return g_string_free(pipeline, FALSE);
    g_string_append(pipeline, " ");
    if (sound == XD_RECORD_MICROPHONE_AND_COMPUTER) {
        g_string_append_printf(pipeline,
                               "audiomixer name=mix ! audioconvert ! %s ! queue ! mux. ",
                               audio_encoder);
        append_source(pipeline, false);
        g_string_append(pipeline, " ! mix. ");
        append_source(pipeline, true);
        g_string_append(pipeline, " ! mix.");
    } else {
        append_source(pipeline, sound == XD_RECORD_COMPUTER);
        g_string_append_printf(pipeline, " ! %s ! queue ! mux.", audio_encoder);
    }
    return g_string_free(pipeline, FALSE);
}
