/* SPDX-License-Identifier: MIT
 * The sound scheme: which sound each program event plays, as XP's
 * Sounds tab sets it. ~/.config/xeondesktop/sounds.ini:
 *
 *   [Sounds]
 *   Scheme=default            the scheme events not listed below follow
 *   [Events]
 *   CriticalStop=dialog-error a sound from the XDG sound theme, by name
 *   WindowsLogon=/home/u/a.wav a sound file
 *   Maximize=                 no sound
 *
 * Written by Sounds and Audio Devices (xd-sound); read when a sound is
 * played.
 */
#ifndef XD_SETTINGS_SOUNDS_H
#define XD_SETTINGS_SOUNDS_H

#include <glib.h>

#include "../core/str.h"

typedef enum xd_sound_event : uint8_t {
    XD_SOUND_ASTERISK,
    XD_SOUND_CLOSE_PROGRAM,
    XD_SOUND_CRITICAL_BATTERY,
    XD_SOUND_CRITICAL_STOP,
    XD_SOUND_DEFAULT_BEEP,
    XD_SOUND_DEVICE_CONNECT,
    XD_SOUND_DEVICE_DISCONNECT,
    XD_SOUND_EXCLAMATION,
    XD_SOUND_EXIT,
    XD_SOUND_LOW_BATTERY,
    XD_SOUND_MAXIMIZE,
    XD_SOUND_MENU_COMMAND,
    XD_SOUND_MENU_POPUP,
    XD_SOUND_MINIMIZE,
    XD_SOUND_NEW_MAIL,
    XD_SOUND_OPEN_PROGRAM,
    XD_SOUND_PROGRAM_ERROR,
    XD_SOUND_QUESTION,
    XD_SOUND_RESTORE_DOWN,
    XD_SOUND_RESTORE_UP,
    XD_SOUND_NOTIFICATION,
    XD_SOUND_LOGOFF,
    XD_SOUND_LOGON,
    XD_SOUND_EMPTY_RECYCLE_BIN,
    XD_SOUND_EVENT_COUNT,
} xd_sound_event;

/* The schemes events start from: XP's "Windows Default" and "No Sounds". */
typedef enum xd_sound_base : uint8_t {
    XD_SOUNDS_DEFAULT,
    XD_SOUNDS_NONE,
} xd_sound_base;

typedef struct xd_sound_event_info {
    const char *key;     /* in [Events] */
    const char *label;   /* XP's name for it */
    const char *program; /* XP's group: "Windows", "Windows Explorer" */
    const char *sound;   /* Windows Default's sound (a theme name), "" = none */
} xd_sound_event_info;

const xd_sound_event_info *xd_sound_event_get(xd_sound_event ev);

typedef struct xd_sound_scheme {
    xd_sound_base base;
    /* g_malloc'd: "" = none, "/path" = a file, else a sound theme name. */
    char *sounds[XD_SOUND_EVENT_COUNT];
} xd_sound_scheme;

typedef enum xd_sound_kind : uint8_t {
    XD_SOUND_NONE,
    XD_SOUND_FILE,
    XD_SOUND_THEMED,
} xd_sound_kind;

xd_sound_kind xd_sound_kind_of(const char *sound);

/* Every event set to `base`'s sound. */
void xd_sound_scheme_init(xd_sound_scheme *sound_scheme, xd_sound_base base);
void xd_sound_scheme_free(xd_sound_scheme *sound_scheme);
/* Replaces one event's sound (copied). Bad values (a relative path, a
 * theme name with odd characters) are refused. */
bool xd_sound_scheme_set(xd_sound_scheme *sound_scheme, xd_sound_event ev, const char *sound);
/* Parses sounds.ini text; never fails (bad values keep the scheme's). */
void xd_sound_scheme_parse(xd_str text, xd_sound_scheme *sound_scheme);
void xd_sound_scheme_serialize(const xd_sound_scheme *sound_scheme, xd_strbuf *out);
/* Differs from its base scheme: XP shows "(modified)". */
bool xd_sound_scheme_modified(const xd_sound_scheme *sound_scheme);
void xd_sound_scheme_path(xd_strbuf *out);
void xd_sound_scheme_load(xd_sound_scheme *sound_scheme);
[[nodiscard]] bool xd_sound_scheme_save(const xd_sound_scheme *sound_scheme);

/* The accessibility features' tones (not part of the scheme, as in
 * Windows), made here as short WAV files. */
typedef enum xd_sound_tone : uint8_t {
    XD_TONE_HIGH,  /* ToggleKeys: a lock turned on; StickyKeys: a modifier locked */
    XD_TONE_LOW,   /* ToggleKeys: a lock turned off; StickyKeys: a modifier released */
    XD_TONE_CLICK, /* StickyKeys and FilterKeys: a key pressed or accepted */
    XD_TONE_RISE,  /* a feature turned on */
    XD_TONE_FALL,  /* ...and off */
    XD_TONE_COUNT,
} xd_sound_tone;

/* The tone's file stem ("access-on"), and the tone as a 16-bit mono WAV. */
const char *xd_sound_tone_name(xd_sound_tone sound_tone);
GBytes *xd_sound_tone_wav(xd_sound_tone sound_tone);

#endif
