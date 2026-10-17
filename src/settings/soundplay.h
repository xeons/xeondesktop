/* SPDX-License-Identifier: MIT
 * Playing the sound scheme's sounds, through libcanberra (which plays
 * through PulseAudio or PipeWire's pulse server, and finds named sounds
 * in the XDG sound theme). Not part of libxdsettings: only the programs
 * that play sounds link libcanberra.
 *
 * Sounds play on a worker thread: libcanberra can block until a sound
 * has been handed to the server, or for good when the server is stuck,
 * and the shell's main loop must never wait for that.
 */
#ifndef XD_SETTINGS_SOUNDPLAY_H
#define XD_SETTINGS_SOUNDPLAY_H

#include "sounds.h"

/* Called in the main context (GLib's default) when a sound has started,
 * or failed with `error`. */
typedef void (*xd_sound_done)(const char *error, void *data);

/* Plays a scheme value ("" plays nothing). A new sound stops the one
 * playing. `done` may be nullptr. */
void xd_sound_play(const char *sound, const char *description, xd_sound_done done, void *data);
/* Plays an event's sound from the user's scheme (read now); failures are
 * printed. */
void xd_sound_play_event(xd_sound_event ev);
/* At exit: drops sounds not started yet. One still playing is left to
 * end with the process. */
void xd_sound_finish(void);

#endif
