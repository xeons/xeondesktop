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

/* Windows 7's "Taskbar buttons", for the unified taskbar (XP's task list
 * groups as XP did). */
typedef enum xd_taskbar_buttons : uint8_t {
    XD_BUTTONS_ALWAYS_COMBINE, /* "Always combine, hide labels": one icon per program */
    XD_BUTTONS_WHEN_FULL,      /* "Combine when taskbar is full" */
    XD_BUTTONS_NEVER_COMBINE,  /* "Never combine" */
    XD_BUTTONS_COUNT,
} xd_taskbar_buttons;

/* The taskbar's buttons ([Taskbar] Layout): Windows 7's unified row, the
 * pinned and running programs together, or XP's, a button for each window
 * with Quick Launch for programs. Automatic: Aero's is unified. */
typedef enum xd_taskbar_layout : uint8_t {
    XD_TASKBAR_LAYOUT_AUTO,
    XD_TASKBAR_LAYOUT_UNIFIED,
    XD_TASKBAR_LAYOUT_TASKS,
    XD_TASKBAR_LAYOUT_COUNT,
} xd_taskbar_layout;

/* Windows 7's "Use small icons" ([Taskbar] Icons): large icons make the
 * taskbar 40px tall, as 7's; small ones keep the style's own (XP's 30px).
 * Automatic: large on a unified taskbar, small on XP's. */
typedef enum xd_taskbar_icons : uint8_t {
    XD_TASKBAR_ICONS_AUTO,
    XD_TASKBAR_ICONS_LARGE,
    XD_TASKBAR_ICONS_SMALL,
    XD_TASKBAR_ICONS_COUNT,
} xd_taskbar_icons;

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

/* Power Options' Advanced "Power buttons": what closing the lid, the power
 * button and the sleep button do, for this user. The shell carries them
 * out, holding logind's block inhibitor for each one set; unset, logind's
 * own HandleLidSwitch, HandlePowerKey and HandleSuspendKey act. */
typedef enum xd_power_button : uint8_t {
    XD_BUTTON_LID,   /* [Power] LidAction */
    XD_BUTTON_POWER, /* [Power] PowerButtonAction */
    XD_BUTTON_SLEEP, /* [Power] SleepButtonAction (XF86Sleep and XF86Suspend) */
    XD_POWER_BUTTON_COUNT,
} xd_power_button;
typedef enum xd_button_action : uint8_t {
    XD_BUTTON_LOGIND, /* unset: logind's setting */
    XD_BUTTON_NOTHING,
    XD_BUTTON_ASK, /* the Shut Down dialog */
    XD_BUTTON_STANDBY,
    XD_BUTTON_HIBERNATE,
    XD_BUTTON_SHUTDOWN,
    XD_BUTTON_LOCK,
    XD_BUTTON_ACTION_COUNT,
} xd_button_action;

/* [Updates]: Automatic Updates (xd-wuaucpl). On the schedule the shell
 * checks for software and firmware updates and, in Download mode, downloads
 * them; nothing is ever installed without the user's click. */
typedef enum xd_updates_mode : uint8_t {
    XD_UPDATES_DOWNLOAD, /* "Download updates for me, but let me choose when to install them" */
    XD_UPDATES_NOTIFY,   /* "Notify me but don't automatically download or install them" */
    XD_UPDATES_OFF,      /* "Turn off Automatic Updates" */
    XD_UPDATES_MODE_COUNT,
} xd_updates_mode;
typedef struct xd_update_settings {
    xd_updates_mode mode; /* Mode */
    int day;              /* Day: 0 every day, 1..7 Sunday..Saturday */
    int hour;             /* Hour: 0..23, local time */
} xd_update_settings;

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
    /* InputMethod: the input method framework the compositor starts
     * ("fcitx5", "ibus"; "" = none), see inputmethod.h. */
    char input_method[16];
} xd_keyboard_settings;

/* [Folders]: Folder Options (xd-folders), obeyed by xd-files. */
typedef struct xd_folder_settings {
    bool new_window;      /* open each folder in its own window */
    bool new_tabs;        /* ...in a new tab instead (xd-files' tabs, as Windows 11) */
    bool single_click;    /* single-click to open an item (point to select) */
    bool show_hidden;     /* show hidden files and folders */
    bool hide_extensions; /* hide extensions for known file types */
    bool full_path_address; /* the full path in the address bar */
    bool full_path_title;   /* the full path in the title bar */
    bool remember_views;    /* remember each folder's view settings */
    bool common_tasks;      /* XP's task pane where the Folders bar isn't; false = Windows
                               classic folders, the view the whole width */
    char default_view[16];  /* "Apply to All Folders": thumbnails, tiles, icons, list or
                               details; "" = the file manager's own default */
} xd_folder_settings;

/* [Touchpad]: what a touchpad's driver added to Mouse Properties (the
 * Synaptics and ALPS tabs of XP and 7), applied by the compositor to
 * libinput touchpads; mice keep [Mouse]'s speed and acceleration. */
typedef enum xd_touchpad_scroll : uint8_t {
    XD_TOUCHPAD_SCROLL_TWO_FINGER, /* two fingers moved together */
    XD_TOUCHPAD_SCROLL_EDGE, /* one finger along the right or bottom edge */
    XD_TOUCHPAD_SCROLL_NONE,
    XD_TOUCHPAD_SCROLL_COUNT,
} xd_touchpad_scroll;

/* A press of the touchpad's button (a clickpad's whole surface): */
typedef enum xd_touchpad_click : uint8_t {
    XD_TOUCHPAD_CLICK_DEFAULT, /* as the touchpad comes */
    XD_TOUCHPAD_CLICK_AREAS, /* its bottom right corner is the right button */
    XD_TOUCHPAD_CLICK_FINGERS, /* two fingers are the right button, three the middle */
    XD_TOUCHPAD_CLICK_COUNT,
} xd_touchpad_click;

typedef struct xd_touchpad_settings {
    bool tap; /* a tap is a click; two fingers right, three middle */
    bool tap_drag; /* tap, then touch and move: a drag */
    bool drag_lock; /* ...which lasts a moment past lifting the finger */
    xd_touchpad_scroll scroll;
    bool natural_scroll; /* the content moves with the fingers */
    int scroll_speed; /* percent of the touchpad's own */
    xd_touchpad_click click;
    bool disable_typing; /* ignored while typing (a palm on it) */
    bool disable_with_mouse; /* off while a mouse is plugged in */
    int speed; /* -10 .. 10, as [Mouse]'s, for the touchpad alone */
    bool acceleration;
    /* Three fingers swiped: down shows the desktop, up brings the
     * windows back, sideways switches windows as Alt+Tab does. */
    bool gestures;
} xd_touchpad_settings;

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
    /* [Touchpad], kept with the mouse's: the compositor takes them
     * together, Mouse Properties sets them together. */
    xd_touchpad_settings touchpad;
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

/* [ScreenSaver.*]: each screen saver's Settings dialog, as Windows kept
 * them under "Control Panel\Screen Saver.<name>". Speeds and sizes are
 * trackbar positions, 0 (slow, small) to XD_SAVER_SLIDER_MAX. */
enum { XD_SAVER_SLIDER_MAX = 10, XD_MYSTIFY_SHAPES = 2 };
typedef enum xd_pipes_joint : uint8_t {
    XD_PIPES_ELBOW,
    XD_PIPES_BALL,
    XD_PIPES_MIXED,
    XD_PIPES_CYCLE, /* elbows, balls, then mixed: another kind each time the screen fills */
    XD_PIPES_JOINT_COUNT,
} xd_pipes_joint;
typedef enum xd_text3d_rotation : uint8_t {
    XD_TEXT3D_NONE,
    XD_TEXT3D_SPIN,
    XD_TEXT3D_SEESAW,
    XD_TEXT3D_WOBBLE,
    XD_TEXT3D_RANDOM,
    XD_TEXT3D_ROTATION_COUNT,
} xd_text3d_rotation;
typedef enum xd_text3d_surface : uint8_t {
    XD_TEXT3D_SOLID,      /* Solid Color: lit faces in the chosen colour */
    XD_TEXT3D_REFLECTION, /* Reflection: chrome, reflecting a sky and the ground */
    XD_TEXT3D_SURFACE_COUNT,
} xd_text3d_surface;
typedef struct xd_marquee_settings {
    char text[256];
    xd_font_spec font;
    xd_rgb color, background;
    bool random_position; /* Position: Random, else Centered */
    int speed;
} xd_marquee_settings;
typedef struct xd_mystify_shape {
    bool active;
    int lines;       /* outlines in its trail */
    bool two_colors; /* else Multiple random colors */
    xd_rgb colors[2];
} xd_mystify_shape;
typedef struct xd_text3d_settings {
    bool show_time; /* Display: Time, else the text */
    char text[64];
    xd_text3d_rotation rotation;
    int speed;
    int size;
    xd_text3d_surface surface;
    xd_rgb color; /* Solid Color's colour */
} xd_text3d_settings;
typedef struct xd_saver_settings {
    xd_marquee_settings marquee;
    struct {
        int speed;   /* Warp speed */
        int density; /* stars */
    } starfield;
    struct {
        xd_mystify_shape shapes[XD_MYSTIFY_SHAPES]; /* Polygon 1, Polygon 2 */
        bool clear_screen; /* off: the trails stay on the screen */
    } mystify;
    struct {
        int curves; /* loops at once */
        int lines;  /* outlines in each loop's trail */
        int speed;
    } beziers;
    struct {
        int speed;   /* Warp speed */
        int density; /* how many fly at once */
    } flying;
    struct {
        bool multiple; /* several pipes grow at once */
        xd_pipes_joint joints;
    } pipes;
    xd_text3d_settings text3d;
} xd_saver_settings;

/* Limits of the [ScreenSaver.*] counts, for the Settings dialogs. */
enum {
    XD_STARFIELD_DENSITY_MIN = 10,
    XD_STARFIELD_DENSITY_MAX = 200,
    XD_MYSTIFY_LINES_MAX = 15,
    XD_BEZIERS_CURVES_MAX = 10,
    XD_BEZIERS_LINES_MAX = 50,
    XD_FLYING_DENSITY_MIN = 10,
    XD_FLYING_DENSITY_MAX = 200,
};

/* [NightLight]: warmer colours in the evening, done by the compositor
 * through each monitor's gamma ramp (nightlight.h has the arithmetic). */
typedef enum xd_nightlight_schedule : uint8_t {
    XD_NIGHTLIGHT_ALWAYS, /* on all the time while turned on */
    XD_NIGHTLIGHT_SUNSET, /* Sunset to sunrise, at Latitude/Longitude */
    XD_NIGHTLIGHT_MANUAL, /* Set hours: TurnOn to TurnOff */
    XD_NIGHTLIGHT_SCHEDULE_COUNT,
} xd_nightlight_schedule;
typedef struct xd_nightlight_settings {
    bool enabled;
    xd_nightlight_schedule schedule;
    int temperature;            /* K at night: XD_NIGHTLIGHT_K_MIN..XD_NIGHTLIGHT_K_MAX */
    int on_minute, off_minute;  /* Set hours, minutes after local midnight */
    bool auto_location;         /* find Latitude/Longitude with GeoClue */
    bool has_location;          /* Latitude/Longitude are set */
    double latitude, longitude; /* degrees, north and east positive */
} xd_nightlight_settings;
enum {
    XD_NIGHTLIGHT_K_MIN = 1700,
    XD_NIGHTLIGHT_K_MAX = 4700,
    XD_NIGHTLIGHT_K_DAY = 6500, /* no change: sRGB's white point */
};

/* [ColorProfiles]: the ICC profile whose calibration curves (vcgt) the
 * compositor loads into a monitor's gamma ramp, by the monitor's
 * monitors.ini key. Color Management (xd-color) writes it as a display's
 * default profile changes in colord. */
typedef struct xd_color_profile_rule {
    char monitor[192];
    char path[1024];
} xd_color_profile_rule;
enum { XD_COLOR_PROFILES_MAX = 8 };

/* XP's "Use the following transition effect for menus and tooltips". */
typedef enum xd_menu_transition : uint8_t {
    XD_TRANSITION_FADE = 0,
    XD_TRANSITION_SCROLL, /* the menu slides out from where it opens */
} xd_menu_transition;

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
    /* A theme package drawing over the style (docs/THEMES.md, themepkg.h):
     * ThemePackage, its name under xeondesktop/themes (a folder, or the
     * stem of a .xdtheme; never a path; "" = none); ThemeVariant, its
     * variant's id ("" or unknown = its first); ImageScaling, the filter
     * its images are scaled with (XD_FILTER_COUNT: Automatic, the
     * package's own, else Smooth); IntegerScaling, whole-number scales
     * done exactly. */
    char theme_package[128];
    char theme_variant[64];
    xd_image_filter image_scaling;
    bool integer_scaling;
    /* [CustomScheme]: Advanced Appearance's changes to the scheme. Choosing
     * another scheme or font size drops them, as in Windows. */
    xd_scheme_custom custom_scheme;

    /* [Desktop] */
    char wallpaper[1024]; /* absolute path, "" = none */
    xd_wallpaper_mode wallpaper_mode;
    xd_desktop_icon_settings desktop_icons; /* [DesktopIcons] */

    /* [Effects] */
    bool menu_transitions;
    xd_menu_transition menu_transition; /* MenuTransition: which one */
    bool animate_windows; /* XP's "Animate windows when minimizing and maximizing" */
    bool full_window_drag; /* off: moving and sizing drag an outline */
    bool large_icons;       /* XP's Shell Icon Size 48: desktop and Start menu */
    bool hide_keyboard_cues;

    /* [Windows] */
    bool always_frames; /* draw our frame even on client-decorated windows */

    /* [Taskbar] */
    bool quick_launch;
    bool locked; /* Lock the Taskbar: no toolbar grips */
    bool auto_hide;
    bool on_top;       /* Keep the taskbar on top of other windows */
    bool group;        /* Group similar taskbar buttons (when crowded) */
    xd_taskbar_buttons buttons; /* unified: icons only, or labels */
    xd_taskbar_layout layout;
    xd_taskbar_icons icons;
    bool previews; /* hovering a task button shows its windows' thumbnails */
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
    xd_saver_settings savers; /* [ScreenSaver.*] */
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
    xd_button_action power_buttons[XD_POWER_BUTTON_COUNT]; /* Advanced's Power buttons */

    /* [Bluetooth] */
    bool bluetooth_icon; /* "Show the Bluetooth icon in the notification area" */

    xd_update_settings updates; /* [Updates] */

    xd_nightlight_settings night_light; /* [NightLight] */
    xd_color_profile_rule color_profiles[XD_COLOR_PROFILES_MAX]; /* [ColorProfiles] */
    size_t color_profile_count;
} xd_settings;

/* The ICC profile of a monitor (its monitors.ini key), or nullptr. */
const char *xd_settings_color_profile(const xd_settings *settings, const char *monitor);
/* Sets it; nullptr or "" removes it. False if the key or path can't be
 * stored (too long, a key GKeyFile can't hold, a relative path) or the
 * list is full. */
bool xd_settings_set_color_profile(xd_settings *settings, const char *monitor, const char *path);
/* "always", "sunset", "manual": [NightLight] Schedule's values. */
const char *xd_nightlight_schedule_name(xd_nightlight_schedule schedule);

/* An icon's rule ([Notifications]), XD_TRAY_AUTO if it has none. */
xd_tray_rule xd_settings_tray_rule(const xd_settings *settings, const char *id);
/* Sets one (XD_TRAY_AUTO removes it). */
void xd_settings_set_tray_rule(xd_settings *settings, const char *id, xd_tray_rule rule);

void xd_settings_defaults(xd_settings *settings);
/* The savers' settings before their Settings dialogs change anything
 * (part of xd_settings_defaults). */
void xd_saver_settings_defaults(xd_saver_settings *saver_settings);
/* Parses settings text on top of the defaults. Never fails: bad values
 * keep their defaults. */
void xd_settings_parse(xd_str text, xd_settings *settings);
/* Loads the user's file, or defaults if there is none. */
void xd_settings_load(xd_settings *settings);
[[nodiscard]] bool xd_settings_save(const xd_settings *settings);
/* Applies to `now` (the file as it is) the values `pending` changed from
 * `base` (the file as a dialog loaded it), so that saving `now` keeps what
 * other programs wrote in the meantime. */
void xd_settings_merge(const xd_settings *base, const xd_settings *pending, xd_settings *now);
/* Same settings (compares what would be written, not struct bytes). */
bool xd_settings_equal(const xd_settings *left, const xd_settings *right);
void xd_settings_path(xd_strbuf *out);
void xd_settings_serialize(const xd_settings *settings, xd_strbuf *out);

const char *xd_wallpaper_mode_name(xd_wallpaper_mode wallpaper_mode);
/* A desktop icon's theme icon name, before any Change Icon. */
const char *xd_desk_icon_default(xd_desk_icon icon);
/* The desktop's icon size in logical px: Small 16, Large 48, Medium the
 * shell's icon size, 32 (48 with "Use large icons", XP's Shell Icon Size). */
int xd_desk_icon_px(const xd_settings *settings);
/* The icon theme to use: IconTheme if set; else $XD_ICON_THEME (for
 * testing a theme without touching the settings); else "breeze".
 * xd_icons_create falls back further if it isn't installed. */
const char *xd_settings_icon_theme(const xd_settings *settings);
/* The GTK theme programs get (GTK_THEME): GtkTheme if set, else the one
 * generated from the appearance (XD_GTK_THEME_NAME). */
const char *xd_settings_gtk_theme(const xd_settings *settings);
/* The Qt widget style for `style` when QtStyle is automatic: Qt's own
 * Windows (2000's bevels) for Classic, Fusion for the others. */
const char *xd_qt_style_auto(xd_visual_style style);
/* Whether the taskbar is unified, and has large icons, in `style`, the
 * automatic choices resolved. */
bool xd_settings_taskbar_unified(const xd_settings *settings, xd_visual_style style);
bool xd_settings_taskbar_large_icons(const xd_settings *settings, xd_visual_style style);
/* The Qt widget style programs get (QT_STYLE_OVERRIDE): QtStyle if set,
 * else xd_qt_style_auto's. */
const char *xd_settings_qt_style(const xd_settings *settings);
/* Qt programs started from now on take their palette, fonts, icon theme
 * and file dialogs from the GTK theme (Qt's own "gtk3" platform theme,
 * QT_QPA_PLATFORMTHEME), with our GTK theme its Qt palette file
 * (QT_GUI_GTK_JSON, see qtpalette.h), and the widget style from QtStyle
 * (QT_STYLE_OVERRIDE). A variable the user set in the environment is left
 * alone: XEONDESKTOP_<variable> remembers what we set, so that our
 * children, which inherit ours, can still tell the two apart. Returns
 * whether anything changed. */
bool xd_settings_qt_env(const xd_settings *settings);
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
xd_alarm_fired xd_battery_alarm_check(const xd_settings *settings, xd_alarm_done *done,
                                      double percent, bool discharging);
/* "download", "notify", "off": [Updates] Mode's values. */
const char *xd_updates_mode_name(xd_updates_mode mode);
/* "standby", ...: [Power] *AlarmAction's values. */
const char *xd_alarm_action_name(xd_alarm_action action);

/* "nothing", "ask", ...: [Power] LidAction's and the buttons' values
 * ("logind" for unset, which isn't written). */
const char *xd_button_action_name(xd_button_action action);
/* By that name; false if it's none. */
bool xd_button_action_from_name(const char *name, xd_button_action *out);
/* logind's HandleLidSwitch-style value in our terms ("suspend" is
 * XD_BUTTON_STANDBY); false for those Power Options doesn't offer
 * ("reboot", "suspend-then-hibernate"...). */
bool xd_button_action_from_logind(const char *handle, xd_button_action *out);
/* What the shell inhibits in logind ("handle-lid-switch:handle-power-key"):
 * the buttons set; "" if none. The sleep button is logind's suspend and
 * hibernate keys both. */
void xd_power_buttons_inhibit_what(const xd_settings *settings, char *out, size_t cap);
/* What the shell does for `button` (pressed, or the lid closed): its
 * action; nothing for one left to logind, and for the lid while docked or
 * with another monitor connected (as logind's HandleLidSwitchDocked=ignore).
 * The lid never asks. */
xd_button_action xd_power_button_action(const xd_settings *settings, xd_power_button button,
                                        bool docked);

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
    XD_TOUCHPAD_SCROLL_SPEED_MIN = 25, /* percent */
    XD_TOUCHPAD_SCROLL_SPEED_MAX = 400,
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
void xd_settings_scheme(const xd_settings *settings, xd_scheme *out);

/* Accessibility Options' High Contrast: on, switches to the Classic style
 * with `scheme` (a high-contrast scheme's stem), remembering the look it
 * replaces; off, switches back to that. */
void xd_settings_set_high_contrast(xd_settings *settings, bool on, const char *scheme);

/* Watches the settings file. Returns an fd to poll (or -1); call
 * xd_settings_watch_changed when it's readable. */
int xd_settings_watch(void);
/* Drains the fd; true if desktop.ini was (re)written. */
bool xd_settings_watch_changed(int fd);

#endif
