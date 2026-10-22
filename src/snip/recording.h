/* SPDX-License-Identifier: MIT
 * Screen recording's workings apart from Wayland and GStreamer, for the
 * unit tests: where a recording goes, what it covers, and the GStreamer
 * pipeline that encodes it.
 */
#ifndef XD_SNIP_RECORDING_H
#define XD_SNIP_RECORDING_H

#include <stdint.h>

#include "snipping.h"

/* "Screen Recording 2026-10-22 153704.mp4", as Windows 11 names them. */
char *xd_record_file_name(GDateTime *time);
/* Videos/Screen Recordings, as in Windows 11. `videos` is the Videos
 * folder GLib knows, or nullptr; GLib says the home folder when
 * user-dirs.dirs names none, and then it's ~/Videos. */
char *xd_record_folder(const char *videos, const char *home);

/* The part of a monitor recorded: the selection within the monitor, in
 * whole logical pixels; false when nothing of it is. */
bool xd_record_fit(xd_snip_box selection, xd_snip_box monitor, xd_snip_box *area);

/* GStreamer's name for a wl_shm pixel format we can record, or nullptr. */
const char *xd_record_video_format(uint32_t shm_format);

/* The pipeline, as gst_parse_launch takes it: an appsrc named "video"
 * (screen frames, its caps set once the first frame's size is known)
 * through `video_encoder` (an H.264 encoder and its properties) into an
 * MP4 file through a filesink named "file"; with sound, PulseAudio's
 * (PipeWire's) microphone and/or the speakers' monitor through
 * `audio_encoder` (AAC). Silent when `audio_encoder` is nullptr. */
char *xd_record_pipeline(const char *video_encoder, const char *audio_encoder,
                         xd_record_sound sound);

#endif
