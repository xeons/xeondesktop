/* SPDX-License-Identifier: MIT
 * Playing the sound scheme's sounds, through libcanberra (which plays
 * through PulseAudio or PipeWire's pulse server, and finds named sounds
 * in the XDG sound theme). Not part of libxdsettings: only the programs
 * that play sounds link libcanberra.
 *
 * Sounds play on a worker thread, which also reads the scheme: libcanberra
 * can block until a sound has been handed to the server, or for good when
 * the server is stuck, and neither the compositor's loop nor the shell's
 * may wait for that.
 *
 * XD_SOUND_TRACE=/FILE appends what would play to FILE instead, a line a
 * sound ("Minimize<TAB>window-minimized"), for tests.
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
/* Plays an event's sound from the user's scheme (read by the worker);
 * failures are printed. Needs no main loop. */
void xd_sound_play_event(xd_sound_event ev);
/* The same, waiting until the sound has played, or for at most
 * `timeout_ms`: for Log Off and Shut Down, which end the session. */
void xd_sound_play_event_wait(xd_sound_event ev, int timeout_ms);
/* Plays an accessibility tone. */
void xd_sound_play_tone(xd_sound_tone tone);
/* From now on, each event sound that plays (or is traced) adds 1 to the
 * eventfd `fd`, from the worker thread: the compositor's SoundSentry
 * flashes then. Events whose scheme has no sound don't count. -1 stops. */
void xd_sound_notify_fd(int fd);
/* At exit: drops sounds not started yet. One still playing is left to
 * end with the process. */
void xd_sound_finish(void);

#endif
