/* SPDX-License-Identifier: MIT
 * Desktop settings: ~/.config/xeondesktop/desktop.ini.
 *
 * The file is the single source of truth. Display Properties writes it
 * (atomically); the compositor and shell watch it with inotify and apply
 * changes live. Unknown keys are ignored so older builds can read newer
 * files.
 */
#ifndef XD_SETTINGS_SETTINGS_H
#define XD_SETTINGS_SETTINGS_H

#include "../theme/theme.h"

typedef enum xd_font_size : uint8_t {
    XD_FONT_NORMAL,
    XD_FONT_LARGE,
    XD_FONT_EXTRA_LARGE,
} xd_font_size;

typedef enum xd_wallpaper_mode : uint8_t {
    XD_WALLPAPER_CENTER,
    XD_WALLPAPER_TILE,
    XD_WALLPAPER_STRETCH,
    XD_WALLPAPER_FILL, /* scale to cover, cropping (Windows 7's Fill) */
    XD_WALLPAPER_FIT,  /* scale to fit, letterboxed */
    XD_WALLPAPER_MODE_COUNT,
} xd_wallpaper_mode;

typedef enum xd_start_style : uint8_t {
    XD_START_CLASSIC, /* Windows 2000: cascading menu with side banner */
    XD_START_XP,      /* Windows XP: two columns, user header, All Programs cascades */
    XD_START_WIN7,    /* Windows 7: two columns, search, All Programs in place */
    XD_START_STYLE_COUNT,
} xd_start_style;

/* [Keyboard]: XKB names ("" = XKB_DEFAULT_* or the system default) and
 * key repeat, applied by the compositor to every keyboard. */
typedef struct xd_keyboard_settings {
    char layout[64];   /* e.g. "us,de" */
    char variant[64];  /* e.g. "dvorak" */
    char options[256]; /* e.g. "grp:alt_shift_toggle,caps:ctrl_modifier" */
    char model[64];
    int repeat_delay; /* ms before a held key repeats */
    int repeat_rate;  /* repeats per second; 0 = no repeat */
} xd_keyboard_settings;

typedef struct xd_settings {
    /* [Appearance] */
    xd_visual_style style;
    char scheme[64]; /* scheme file stem, e.g. "windows-standard"; "" = built in */
    xd_font_size font_size;
    xd_font_smoothing font_smoothing;

    /* [Desktop] */
    char wallpaper[1024]; /* absolute path, "" = none */
    xd_wallpaper_mode wallpaper_mode;

    /* [Effects] (stored; not all are implemented yet) */
    bool menu_transitions;
    bool full_window_drag;
    bool large_icons;
    bool hide_keyboard_cues;

    /* [Windows] */
    bool always_frames; /* draw our frame even on client-decorated windows */

    /* [Taskbar] */
    bool quick_launch;
    bool locked; /* Lock the Taskbar: no toolbar grips */
    bool auto_hide;
    bool on_top;       /* Keep the taskbar on top of other windows */
    bool group;        /* Group similar taskbar buttons (when crowded) */
    bool show_clock;
    bool all_displays; /* a taskbar on every monitor, not just the first */

    /* [StartMenu] */
    xd_start_style start_style;

    /* [ScreenSaver] */
    char screensaver[64];
    int screensaver_wait; /* minutes */
    bool screensaver_password;
    int monitor_off; /* minutes of idle before monitors turn off; 0 = never */
    xd_keyboard_settings keyboard;
} xd_settings;

void xd_settings_defaults(xd_settings *s);
/* Parses settings text on top of the defaults. Never fails: bad values
 * keep their defaults. */
void xd_settings_parse(xd_str text, xd_settings *s);
/* Loads the user's file, or defaults if there is none. */
void xd_settings_load(xd_settings *s);
[[nodiscard]] bool xd_settings_save(const xd_settings *s);
/* Same settings (compares what would be written, not struct bytes). */
bool xd_settings_equal(const xd_settings *a, const xd_settings *b);
void xd_settings_path(xd_strbuf *out);
void xd_settings_serialize(const xd_settings *s, xd_strbuf *out);

const char *xd_wallpaper_mode_name(xd_wallpaper_mode m);

/* Directory holding data/ (schemes, ...): $XD_DATA_DIR, the source tree
 * next to a build directory, or the install prefix. */
bool xd_data_dir(xd_strbuf *out);

/* Available colour schemes: file stems and display names. */
typedef struct xd_scheme_list {
    size_t count;
    char **stems;
    char **names;
} xd_scheme_list;
void xd_scheme_list_load(xd_scheme_list *list);
void xd_scheme_list_free(xd_scheme_list *list);

/* The scheme the settings select, with font size applied. */
void xd_settings_scheme(const xd_settings *s, xd_scheme *out);

/* Watches the settings file. Returns an fd to poll (or -1); call
 * xd_settings_watch_changed when it's readable. */
int xd_settings_watch(void);
/* Drains the fd; true if desktop.ini was (re)written. */
bool xd_settings_watch_changed(int fd);

#endif
