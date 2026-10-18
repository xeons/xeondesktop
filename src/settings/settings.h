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

/* Windows 7's "Taskbar buttons" (Aero; the other styles group as XP did). */
typedef enum xd_taskbar_buttons : uint8_t {
    XD_BUTTONS_ALWAYS_COMBINE, /* "Always combine, hide labels": one icon per program */
    XD_BUTTONS_WHEN_FULL,      /* "Combine when taskbar is full" */
    XD_BUTTONS_NEVER_COMBINE,  /* "Never combine" */
    XD_BUTTONS_COUNT,
} xd_taskbar_buttons;

/* [Notifications]: one notification area icon's behavior, by its id. */
typedef enum xd_tray_rule : uint8_t {
    XD_TRAY_AUTO,        /* hide when inactive */
    XD_TRAY_ALWAYS_HIDE,
    XD_TRAY_ALWAYS_SHOW,
} xd_tray_rule;
typedef struct xd_tray_behavior {
    char id[64];
    xd_tray_rule rule;
} xd_tray_behavior;
enum { XD_TRAY_RULES_MAX = 48 };
/* The most "Number of programs on Start menu" goes to. */
enum { XD_START_PROGRAMS_MAX = 30 };

/* [Power] Scheme: XP's power schemes. Choosing one in Power Options fills
 * in its timeouts; the timeouts themselves are what the shell uses. */
typedef enum xd_power_scheme : uint8_t {
    XD_POWER_HOME,         /* Home/Office Desk */
    XD_POWER_PORTABLE,     /* Portable/Laptop */
    XD_POWER_PRESENTATION, /* Presentation */
    XD_POWER_ALWAYS_ON,    /* Always On */
    XD_POWER_MINIMAL,      /* Minimal Power Management */
    XD_POWER_MAX_BATTERY,  /* Max Battery */
    XD_POWER_SCHEME_COUNT,
} xd_power_scheme;

/* Power Options' Alarms: what a battery alarm does when the charge falls
 * to its level (Alarm Action...). The shell acts on them. */
typedef enum xd_alarm_action : uint8_t {
    XD_ALARM_NOTHING,   /* "When the alarm goes off" unchecked */
    XD_ALARM_STANDBY,
    XD_ALARM_HIBERNATE,
    XD_ALARM_SHUTDOWN,
    XD_ALARM_ACTION_COUNT,
} xd_alarm_action;
typedef struct xd_battery_alarm {
    bool on;      /* "Activate ... battery alarm when power level reaches:" */
    int percent;  /* that level */
    bool sound;   /* Notification: Sound alarm (the scheme's battery sound) */
    bool message; /* Notification: Display message */
    xd_alarm_action action;
} xd_battery_alarm;

/* [Keyboard]: XKB names ("" = XKB_DEFAULT_* or the system default) and
 * key repeat, applied by the compositor to every keyboard. */
typedef struct xd_keyboard_settings {
    char layout[64];   /* e.g. "us,de" */
    char variant[64];  /* e.g. "dvorak" */
    char options[256]; /* e.g. "grp:alt_shift_toggle,caps:ctrl_modifier" */
    char model[64];
    int repeat_delay; /* ms before a held key repeats */
    int repeat_rate;  /* repeats per second; 0 = no repeat */
    int cursor_blink_ms; /* text caret on (and off) time, as in Windows; 0 = steady */
} xd_keyboard_settings;

/* [Folders]: Folder Options (xd-folders), obeyed by xd-files. */
typedef struct xd_folder_settings {
    bool new_window;      /* open each folder in its own window */
    bool single_click;    /* single-click to open an item (point to select) */
    bool show_hidden;     /* show hidden files and folders */
    bool hide_extensions; /* hide extensions for known file types */
    bool full_path_address; /* the full path in the address bar */
    bool full_path_title;   /* the full path in the title bar */
    bool remember_views;    /* remember each folder's view settings */
    char default_view[16];  /* "Apply to All Folders": thumbnails, tiles, icons, list or
                               details; "" = the file manager's own default */
} xd_folder_settings;

/* [Mouse]: the compositor applies the buttons, speed and acceleration to
 * every libinput pointer and the wheel to what it forwards; the cursor
 * theme to its own cursor and, through XCURSOR_THEME/XCURSOR_SIZE, to
 * programs started afterwards. Our GTK apps follow the double-click time
 * and cursor live (gtkapp.c), the shell its double-click time. */
typedef struct xd_mouse_settings {
    bool left_handed;      /* Switch primary and secondary buttons */
    int speed;             /* -10 (slow) .. 10 (fast): libinput's speed x 10 */
    bool acceleration;     /* Enhance pointer precision: adaptive, else flat */
    int double_click_ms;   /* 200 .. 900 */
    int wheel_lines;       /* lines a wheel notch scrolls (clients' own step is 3) */
    char cursor_theme[64]; /* XCursor theme directory name; "" = the default */
    int cursor_size;       /* logical px */
    bool click_lock;       /* ClickLock: a long press holds the button until the next click */
    int click_lock_ms;     /* how long that press is */
} xd_mouse_settings;

/* SoundSentry's "Choose the visual warning". */
typedef enum xd_sound_sentry : uint8_t {
    XD_SENTRY_NONE,
    XD_SENTRY_CAPTION, /* Flash active caption bar */
    XD_SENTRY_WINDOW,  /* Flash active window */
    XD_SENTRY_DESKTOP, /* Flash desktop */
    XD_SENTRY_COUNT,
} xd_sound_sentry;

/* [Accessibility]: StickyKeys, FilterKeys, ToggleKeys and MouseKeys,
 * done by the compositor (with the keyboard shortcuts that turn them on);
 * High Contrast switches [Appearance] and remembers what to switch back
 * to. */
typedef struct xd_access_settings {
    bool sticky_keys;          /* a modifier pressed alone applies to the next key */
    bool sticky_lock;          /* ...and pressed twice stays on until pressed again */
    bool sticky_shortcut;      /* Shift pressed 5 times turns it on or off */
    bool sticky_two_keys_off;  /* a modifier held with another key turns it off */
    bool sticky_sound;         /* a tone when a modifier is pressed */
    bool filter_keys;          /* FilterKeys: BounceKeys, or SlowKeys if slow_keys */
    bool filter_shortcut;      /* right Shift held 8 seconds */
    bool slow_keys;            /* ignore keys released within slow_ms (else BounceKeys) */
    bool filter_beep;          /* a tone when a key is pressed or accepted */
    bool filter_no_repeat;     /* SlowKeys: no key repeat at all, else the rates below */
    int bounce_ms;             /* BounceKeys: ignore a key pressed again within this */
    int slow_ms;               /* SlowKeys: how long a key must be held */
    int filter_repeat_delay;   /* SlowKeys' repeat delay and interval, ms */
    int filter_repeat_ms;
    bool toggle_keys;          /* tones on Caps, Num and Scroll Lock */
    bool toggle_shortcut;      /* Num Lock held 5 seconds */
    bool mouse_keys;           /* the numeric keypad moves the pointer */
    bool mouse_keys_shortcut;  /* Left Alt+Left Shift+Num Lock */
    bool mouse_keys_modifiers; /* Ctrl speeds it up, Shift slows it down */
    bool mouse_keys_numlock;   /* MouseKeys work while Num Lock is on (else off) */
    int mouse_keys_speed;      /* top speed, px/s */
    int mouse_keys_accel_ms;   /* time to reach it */
    bool warn;                 /* a shortcut asks whether to keep a feature it turned on */
    bool sound_on_off;         /* a rising or falling tone as a feature goes on or off */
    bool high_contrast;
    /* XP's "Show ... status on screen": an icon in the notification area
     * while the feature is on (the shell's). */
    bool sticky_indicator, filter_indicator, mouse_keys_indicator;
    /* Sound tab: SoundSentry flashes something whenever the compositor
     * plays a sound (its window sounds and the system bell); ShowSounds
     * asks programs for captions (only stored: no toolkit reads it). */
    bool sound_sentry;
    xd_sound_sentry sound_sentry_effect;
    bool show_sounds;
    /* General: turn the features off after so long with no input. */
    bool auto_reset;
    int auto_reset_minutes;
    xd_visual_style saved_style; /* the look from before High Contrast */
    char saved_scheme[64];
} xd_access_settings;

/* [DesktopIcons]: Display Properties' Desktop Items dialog (XP's
 * Customize Desktop) and the desktop's own menu (Arrange Icons By, or
 * Windows 7's View). Followed live by the shell. */
typedef enum xd_desk_icon : uint8_t {
    /* In the order of XP's Desktop Items list. */
    XD_DESK_ICON_COMPUTER,
    XD_DESK_ICON_DOCUMENTS,
    XD_DESK_ICON_NETWORK,
    XD_DESK_ICON_TRASH_FULL,
    XD_DESK_ICON_TRASH_EMPTY,
    XD_DESK_ICON_COUNT,
} xd_desk_icon;
typedef enum xd_desk_icon_size : uint8_t {
    XD_DESK_ICONS_SMALL,  /* Windows 7's View > Small icons */
    XD_DESK_ICONS_MEDIUM, /* the shell's icon size: XP's "Use large icons" makes it 48 */
    XD_DESK_ICONS_LARGE,
} xd_desk_icon_size;
typedef struct xd_desktop_icon_settings {
    /* Which of the special icons show (Desktop Items' checkboxes; the
     * default web browser stands in for XP's Internet Explorer). */
    bool documents, computer, network, browser;
    bool show;          /* Show Desktop Icons */
    bool auto_arrange;  /* kept packed in columns, in order */
    bool align_to_grid; /* off: icons stay where they're dropped */
    xd_desk_icon_size size;
    /* Change Icon...: an image file replacing the theme's icon; "" = default. */
    char icons[XD_DESK_ICON_COUNT][512];
} xd_desktop_icon_settings;

typedef struct xd_settings {
    /* [Appearance] */
    xd_visual_style style;
    char scheme[64]; /* scheme file stem, e.g. "windows-standard"; "" = built in */
    xd_font_size font_size;
    xd_font_smoothing font_smoothing;
    /* Aero's Window Color and Appearance: GlassColor, AARRGGBB in hex as
     * DWM's ColorizationColor (the alpha is the colour intensity), and
     * GlassTransparency ("Enable transparency"). */
    uint32_t glass_color;
    bool glass_transparency;
    /* Theme directory names; "" = the default (see xd_settings_icon_theme
     * and xd_settings_gtk_theme). */
    char icon_theme[64]; /* IconTheme: a freedesktop icon theme under icons/ */
    char gtk_theme[64];  /* GtkTheme: an installed GTK3 theme instead of ours */
    char qt_style[64];   /* QtStyle: a Qt widget style's key; "" = automatic */

    /* [Desktop] */
    char wallpaper[1024]; /* absolute path, "" = none */
    xd_wallpaper_mode wallpaper_mode;
    xd_desktop_icon_settings desktop_icons; /* [DesktopIcons] */

    /* [Effects] (stored; not all are implemented yet) */
    bool menu_transitions;
    bool animate_windows; /* XP's "Animate windows when minimizing and maximizing" */
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
    xd_taskbar_buttons buttons; /* Aero: icons only, or labels */
    bool show_clock;
    bool hide_inactive; /* fold tray icons that have been quiet behind a chevron */
    bool all_displays; /* a taskbar on every monitor, not just the first */
    xd_edge edge;      /* where the taskbar is */
    int rows;          /* a top or bottom taskbar's height, in rows */
    int side_width;    /* a left or right taskbar's width, logical px */
    /* Quick Launch's own View menu, as XP's toolbars have. */
    bool quick_large;      /* View > Large Icons (else Small Icons) */
    bool quick_show_text;  /* Show Text: each shortcut's name beside its icon */
    bool quick_show_title; /* Show Title: "Quick Launch" before the buttons */

    /* [StartMenu] */
    xd_start_style start_style;
    /* XP's Customize Start Menu: the program list's icon size, how many
     * most-used programs it shows, and the highlight for new programs. */
    bool start_large_icons;
    int start_programs;
    bool start_highlight_new;

    /* [ScreenSaver] */
    char screensaver[64];
    int screensaver_wait; /* minutes */
    bool screensaver_password;
    int monitor_off; /* minutes of idle before monitors turn off; 0 = never */
    xd_keyboard_settings keyboard;
    xd_folder_settings folders;
    xd_mouse_settings mouse;
    xd_access_settings access;
    xd_tray_behavior tray_rules[XD_TRAY_RULES_MAX];
    size_t tray_rule_count;

    /* [Sound] */
    bool volume_icon; /* "Place volume icon in the taskbar" */

    /* [Power] (monitor off is [ScreenSaver] MonitorOff, shared with
     * Display Properties) */
    xd_power_scheme power_scheme;
    int standby;          /* minutes of idle before standby (suspend); 0 = never */
    int hibernate;        /* minutes of idle before hibernating; 0 = never */
    /* The same three while running on batteries (XP's laptop columns);
     * the shell switches when the power cord does. */
    int monitor_off_battery, standby_battery, hibernate_battery;
    /* The user's own scheme (Power Options' Save As), by name; "" = the
     * built-in power_scheme. */
    char power_scheme_custom[64];
    bool battery_icon;    /* "Always show icon on the taskbar"; else only on battery */
    bool resume_password; /* lock the screen before sleeping */
    xd_battery_alarm low_alarm, critical_alarm; /* Alarms */

    /* [Bluetooth] */
    bool bluetooth_icon; /* "Show the Bluetooth icon in the notification area" */
} xd_settings;

/* An icon's rule ([Notifications]), XD_TRAY_AUTO if it has none. */
xd_tray_rule xd_settings_tray_rule(const xd_settings *s, const char *id);
/* Sets one (XD_TRAY_AUTO removes it). */
void xd_settings_set_tray_rule(xd_settings *s, const char *id, xd_tray_rule rule);

void xd_settings_defaults(xd_settings *s);
/* Parses settings text on top of the defaults. Never fails: bad values
 * keep their defaults. */
void xd_settings_parse(xd_str text, xd_settings *s);
/* Loads the user's file, or defaults if there is none. */
void xd_settings_load(xd_settings *s);
[[nodiscard]] bool xd_settings_save(const xd_settings *s);
/* Applies to `now` (the file as it is) the values `pending` changed from
 * `base` (the file as a dialog loaded it), so that saving `now` keeps what
 * other programs wrote in the meantime. */
void xd_settings_merge(const xd_settings *base, const xd_settings *pending, xd_settings *now);
/* Same settings (compares what would be written, not struct bytes). */
bool xd_settings_equal(const xd_settings *a, const xd_settings *b);
void xd_settings_path(xd_strbuf *out);
void xd_settings_serialize(const xd_settings *s, xd_strbuf *out);

const char *xd_wallpaper_mode_name(xd_wallpaper_mode m);
/* A desktop icon's theme icon name, before any Change Icon. */
const char *xd_desk_icon_default(xd_desk_icon icon);
/* The desktop's icon size in logical px: Small 16, Large 48, Medium the
 * shell's icon size, 32 (48 with "Use large icons", XP's Shell Icon Size). */
int xd_desk_icon_px(const xd_settings *s);
/* The icon theme to use: IconTheme if set; else $XD_ICON_THEME (for
 * testing a theme without touching the settings); else "breeze".
 * xd_icons_create falls back further if it isn't installed. */
const char *xd_settings_icon_theme(const xd_settings *s);
/* The GTK theme programs get (GTK_THEME): GtkTheme if set, else the one
 * generated from the appearance (XD_GTK_THEME_NAME). */
const char *xd_settings_gtk_theme(const xd_settings *s);
/* The Qt widget style for `style` when QtStyle is automatic: Qt's own
 * Windows (2000's bevels) for Classic, Fusion for the others. */
const char *xd_qt_style_auto(xd_visual_style style);
/* The Qt widget style programs get (QT_STYLE_OVERRIDE): QtStyle if set,
 * else xd_qt_style_auto's. */
const char *xd_settings_qt_style(const xd_settings *s);
/* Qt programs started from now on take their palette, fonts, icon theme
 * and file dialogs from the GTK theme (Qt's own "gtk3" platform theme,
 * QT_QPA_PLATFORMTHEME), with our GTK theme its Qt palette file
 * (QT_GUI_GTK_JSON, see qtpalette.h), and the widget style from QtStyle
 * (QT_STYLE_OVERRIDE). A variable the user set in the environment is left
 * alone: XEONDESKTOP_<variable> remembers what we set, so that our
 * children, which inherit ours, can still tell the two apart. Returns
 * whether anything changed. */
bool xd_settings_qt_env(const xd_settings *s);
/* Whether `var` (one of those) holds what xd_settings_qt_env set. */
bool xd_qt_env_ours(const char *var);
/* "home", "portable", ...: [Power] Scheme's values. */
const char *xd_power_scheme_name(xd_power_scheme scheme);

/* The battery alarms: which one goes off now, as the charge reaches
 * `percent`. Each goes off once on the way down (the critical one marks
 * the low one done too); `discharging` false (plugged in) starts them
 * again. `done` holds that between calls: zero it to start. */
typedef enum xd_alarm_fired : uint8_t {
    XD_ALARM_FIRED_NONE,
    XD_ALARM_FIRED_LOW,
    XD_ALARM_FIRED_CRITICAL,
} xd_alarm_fired;
typedef struct xd_alarm_done {
    bool low, critical;
} xd_alarm_done;
xd_alarm_fired xd_battery_alarm_check(const xd_settings *s, xd_alarm_done *done, double percent,
                                      bool discharging);
/* "standby", ...: [Power] *AlarmAction's values. */
const char *xd_alarm_action_name(xd_alarm_action action);

/* Limits of the [Mouse] values, for the applets' sliders. */
enum {
    XD_MOUSE_SPEED_MIN = -10,
    XD_MOUSE_SPEED_MAX = 10,
    XD_DOUBLE_CLICK_MIN = 200,
    XD_DOUBLE_CLICK_MAX = 900,
    XD_CURSOR_SIZE_MIN = 16,
    XD_CURSOR_SIZE_MAX = 128,
    XD_CLICK_LOCK_MIN = 200,
    XD_CLICK_LOCK_MAX = 2200,
    XD_MOUSE_KEYS_SPEED_MIN = 10,
    XD_MOUSE_KEYS_SPEED_MAX = 360,
    XD_MOUSE_KEYS_ACCEL_MIN = 1000,
    XD_MOUSE_KEYS_ACCEL_MAX = 5000,
    XD_AUTO_RESET_MIN = 5, /* Accessibility's automatic reset, minutes */
    XD_AUTO_RESET_MAX = 30,
};

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

/* Accessibility Options' High Contrast: on, switches to the Classic style
 * with `scheme` (a high-contrast scheme's stem), remembering the look it
 * replaces; off, switches back to that. */
void xd_settings_set_high_contrast(xd_settings *s, bool on, const char *scheme);

/* Watches the settings file. Returns an fd to poll (or -1); call
 * xd_settings_watch_changed when it's readable. */
int xd_settings_watch(void);
/* Drains the fd; true if desktop.ini was (re)written. */
bool xd_settings_watch_changed(int fd);

#endif
