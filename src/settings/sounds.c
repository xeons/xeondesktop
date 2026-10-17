/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <math.h>

#include "settings.h"
#include "sounds.h"

/* Windows Default, in sounds of the freedesktop sound theme (what XP's
 * Media folder had, as near as it goes). XP left the window and menu
 * events silent, and Question too. */
static const xd_sound_event_info events[XD_SOUND_EVENT_COUNT] = {
    [XD_SOUND_ASTERISK] = {"Asterisk", "Asterisk", "Windows", "dialog-information"},
    [XD_SOUND_CLOSE_PROGRAM] = {"CloseProgram", "Close program", "Windows", ""},
    [XD_SOUND_CRITICAL_BATTERY] = {"CriticalBatteryAlarm", "Critical Battery Alarm", "Windows",
                                   "dialog-error"},
    [XD_SOUND_CRITICAL_STOP] = {"CriticalStop", "Critical Stop", "Windows", "dialog-error"},
    [XD_SOUND_DEFAULT_BEEP] = {"DefaultBeep", "Default Beep", "Windows", "bell"},
    [XD_SOUND_DEVICE_CONNECT] = {"DeviceConnect", "Device Connect", "Windows", "device-added"},
    [XD_SOUND_DEVICE_DISCONNECT] = {"DeviceDisconnect", "Device Disconnect", "Windows",
                                    "device-removed"},
    [XD_SOUND_EXCLAMATION] = {"Exclamation", "Exclamation", "Windows", "dialog-warning"},
    [XD_SOUND_EXIT] = {"ExitWindows", "Exit Windows", "Windows", "service-logout"},
    [XD_SOUND_LOW_BATTERY] = {"LowBatteryAlarm", "Low Battery Alarm", "Windows",
                              "dialog-warning"},
    [XD_SOUND_MAXIMIZE] = {"Maximize", "Maximize", "Windows", ""},
    [XD_SOUND_MENU_COMMAND] = {"MenuCommand", "Menu command", "Windows", ""},
    [XD_SOUND_MENU_POPUP] = {"MenuPopup", "Menu popup", "Windows", ""},
    [XD_SOUND_MINIMIZE] = {"Minimize", "Minimize", "Windows", ""},
    [XD_SOUND_NEW_MAIL] = {"NewMail", "New Mail Notification", "Windows", "message"},
    [XD_SOUND_OPEN_PROGRAM] = {"OpenProgram", "Open program", "Windows", ""},
    [XD_SOUND_PROGRAM_ERROR] = {"ProgramError", "Program error", "Windows", "dialog-error"},
    [XD_SOUND_QUESTION] = {"Question", "Question", "Windows", ""},
    [XD_SOUND_RESTORE_DOWN] = {"RestoreDown", "Restore Down", "Windows", ""},
    [XD_SOUND_RESTORE_UP] = {"RestoreUp", "Restore Up", "Windows", ""},
    [XD_SOUND_NOTIFICATION] = {"SystemNotification", "System Notification", "Windows",
                               "message-new-instant"},
    [XD_SOUND_LOGOFF] = {"WindowsLogoff", "Windows Logoff", "Windows", "service-logout"},
    [XD_SOUND_LOGON] = {"WindowsLogon", "Windows Logon", "Windows", "service-login"},
    [XD_SOUND_EMPTY_RECYCLE_BIN] = {"EmptyRecycleBin", "Empty Recycle Bin", "Windows Explorer",
                                    "trash-empty"},
};

static const char *const base_names[] = {"default", "none"};

const xd_sound_event_info *xd_sound_event_get(xd_sound_event ev) {
    XD_ASSERT((unsigned)ev < XD_SOUND_EVENT_COUNT);
    return &events[ev];
}

/* Sound theme names are file stems: "dialog-error". */
static bool theme_name_ok(const char *s) {
    if (s[0] == '\0' || s[0] == '.' || strlen(s) > 128)
        return false;
    for (const char *c = s; *c != '\0'; c++) {
        if (!g_ascii_isalnum(*c) && *c != '-' && *c != '_' && *c != '.')
            return false;
    }
    return true;
}

xd_sound_kind xd_sound_kind_of(const char *sound) {
    if (sound == nullptr || sound[0] == '\0')
        return XD_SOUND_NONE;
    return sound[0] == '/' ? XD_SOUND_FILE : XD_SOUND_THEMED;
}

static const char *base_sound(xd_sound_base base, xd_sound_event ev) {
    return base == XD_SOUNDS_NONE ? "" : events[ev].sound;
}

void xd_sound_scheme_init(xd_sound_scheme *s, xd_sound_base base) {
    *s = (xd_sound_scheme){.base = base};
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++)
        s->sounds[i] = g_strdup(base_sound(base, (xd_sound_event)i));
}

void xd_sound_scheme_free(xd_sound_scheme *s) {
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++)
        g_free(s->sounds[i]);
    *s = (xd_sound_scheme){0};
}

bool xd_sound_scheme_set(xd_sound_scheme *s, xd_sound_event ev, const char *sound) {
    XD_ASSERT((unsigned)ev < XD_SOUND_EVENT_COUNT);
    if (sound == nullptr)
        sound = "";
    bool ok = sound[0] == '\0' || (sound[0] == '/' && strlen(sound) < 4096) ||
              theme_name_ok(sound);
    if (ok) {
        g_free(s->sounds[ev]);
        s->sounds[ev] = g_strdup(sound);
    }
    return ok;
}

void xd_sound_scheme_parse(xd_str text, xd_sound_scheme *s) {
    xd_sound_scheme_init(s, XD_SOUNDS_DEFAULT);
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_data(kf, text.ptr, text.len, G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(kf);
        return; /* a syntax error leaves Windows Default */
    }
    char *base = g_key_file_get_string(kf, "Sounds", "Scheme", nullptr);
    for (size_t i = 0; base != nullptr && i < XD_ARRAY_LEN(base_names); i++) {
        if (g_ascii_strcasecmp(base, base_names[i]) == 0) {
            xd_sound_scheme_free(s);
            xd_sound_scheme_init(s, (xd_sound_base)i);
        }
    }
    g_free(base);
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++) {
        char *v = g_key_file_get_string(kf, "Events", events[i].key, nullptr);
        if (v != nullptr)
            (void)xd_sound_scheme_set(s, (xd_sound_event)i, g_strstrip(v));
        g_free(v);
    }
    g_key_file_free(kf);
}

static GKeyFile *to_keyfile(const xd_sound_scheme *s) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Sounds", "Scheme", base_names[s->base]);
    g_key_file_set_comment(kf, "Sounds", nullptr,
                           " XeonDesktop's sound scheme. Written by Sounds and Audio Devices.\n"
                           " A value is a sound theme name, a file path, or empty for none.",
                           nullptr);
    /* Every event, so a later change of defaults doesn't change them. */
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++)
        g_key_file_set_string(kf, "Events", events[i].key,
                              s->sounds[i] != nullptr ? s->sounds[i] : "");
    return kf;
}

void xd_sound_scheme_serialize(const xd_sound_scheme *s, xd_strbuf *out) {
    GKeyFile *kf = to_keyfile(s);
    gsize len = 0;
    gchar *data = g_key_file_to_data(kf, &len, nullptr);
    xd_strbuf_append(out, (xd_str){data, len});
    g_free(data);
    g_key_file_free(kf);
}

bool xd_sound_scheme_modified(const xd_sound_scheme *s) {
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++) {
        const char *v = s->sounds[i] != nullptr ? s->sounds[i] : "";
        if (strcmp(v, base_sound(s->base, (xd_sound_event)i)) != 0)
            return true;
    }
    return false;
}

void xd_sound_scheme_path(xd_strbuf *out) {
    /* Next to desktop.ini. */
    xd_settings_path(out);
    char *slash = strrchr(out->data, '/');
    XD_ASSERT(slash != nullptr);
    out->len = (size_t)(slash - out->data) + 1;
    out->data[out->len] = '\0';
    xd_strbuf_append(out, XD_STR("sounds.ini"));
}

void xd_sound_scheme_load(xd_sound_scheme *s) {
    xd_strbuf path = {0};
    xd_sound_scheme_path(&path);
    char *text = nullptr;
    gsize len = 0;
    if (g_file_get_contents(xd_strbuf_cstr(&path), &text, &len, nullptr) && len < (1u << 20))
        xd_sound_scheme_parse((xd_str){text, len}, s);
    else
        xd_sound_scheme_init(s, XD_SOUNDS_DEFAULT);
    g_free(text);
    xd_strbuf_free(&path);
}

bool xd_sound_scheme_save(const xd_sound_scheme *s) {
    xd_strbuf path = {0};
    xd_sound_scheme_path(&path);
    char *dir = g_path_get_dirname(xd_strbuf_cstr(&path));
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GKeyFile *kf = to_keyfile(s);
    GError *err = nullptr;
    /* Written to a temporary file and renamed into place. */
    bool ok = g_key_file_save_to_file(kf, xd_strbuf_cstr(&path), &err);
    if (!ok) {
        fprintf(stderr, "xeondesktop: cannot save the sound scheme: %s\n",
                err != nullptr ? err->message : "unknown error");
        g_clear_error(&err);
    }
    g_key_file_free(kf);
    xd_strbuf_free(&path);
    return ok;
}

/* ---- tones ---------------------------------------------------------------------- */

/* Windows beeps these; we hold or sweep a sine wave, quietly, with a few
 * milliseconds' fade at each end so it doesn't click. */
static const struct {
    const char *name;
    double from_hz, to_hz; /* a sweep when they differ */
    int ms;
} tones[XD_TONE_COUNT] = {
    [XD_TONE_HIGH] = {"access-high", 1200, 1200, 100},
    [XD_TONE_LOW] = {"access-low", 500, 500, 100},
    [XD_TONE_CLICK] = {"access-click", 2000, 2000, 15},
    [XD_TONE_RISE] = {"access-on", 500, 1600, 300},
    [XD_TONE_FALL] = {"access-off", 1600, 500, 300},
};

const char *xd_sound_tone_name(xd_sound_tone t) {
    XD_ASSERT((unsigned)t < XD_TONE_COUNT);
    return tones[t].name;
}

static void put_le(GByteArray *a, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        g_byte_array_append(a, &b, 1);
    }
}

GBytes *xd_sound_tone_wav(xd_sound_tone t) {
    XD_ASSERT((unsigned)t < XD_TONE_COUNT);
    constexpr uint32_t RATE = 22050;
    constexpr uint32_t FADE = RATE * 3 / 1000; /* samples */
    uint32_t n = RATE * (uint32_t)tones[t].ms / 1000;
    GByteArray *a = g_byte_array_sized_new(44 + n * 2);
    /* RIFF/WAVE: one channel of 16-bit PCM. */
    g_byte_array_append(a, (const guint8 *)"RIFF", 4);
    put_le(a, 36 + n * 2, 4);
    g_byte_array_append(a, (const guint8 *)"WAVEfmt ", 8);
    put_le(a, 16, 4);
    put_le(a, 1, 2); /* PCM */
    put_le(a, 1, 2); /* channels */
    put_le(a, RATE, 4);
    put_le(a, RATE * 2, 4); /* bytes a second */
    put_le(a, 2, 2);        /* bytes a sample */
    put_le(a, 16, 2);
    g_byte_array_append(a, (const guint8 *)"data", 4);
    put_le(a, n * 2, 4);
    double phase = 0;
    for (uint32_t i = 0; i < n; i++) {
        double hz = tones[t].from_hz + (tones[t].to_hz - tones[t].from_hz) * i / n;
        phase += 2 * G_PI * hz / RATE;
        double env = i < FADE ? (double)i / FADE : n - i < FADE ? (double)(n - i) / FADE : 1.0;
        int16_t v = (int16_t)(sin(phase) * env * 0.3 * INT16_MAX);
        put_le(a, (uint16_t)v, 2);
    }
    return g_byte_array_free_to_bytes(a);
}
