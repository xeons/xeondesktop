/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <math.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/alloc.h"
#include <glib.h>
#include "gtktheme.h"
#include "settings.h"

static const char *const mode_names[XD_WALLPAPER_MODE_COUNT] = {
    [XD_WALLPAPER_CENTER] = "center", [XD_WALLPAPER_TILE] = "tile",
    [XD_WALLPAPER_STRETCH] = "stretch", [XD_WALLPAPER_FILL] = "fill",
    [XD_WALLPAPER_FIT] = "fit",
};

static const char *const font_size_names[] = {"normal", "large", "extralarge"};
static const char *const start_style_names[] = {"classic", "xp", "win7"};
/* [Appearance] FontSmoothing, FontSubpixelOrder and FontHinting, in the
 * enums' order (fontconfig's and GNOME's words). */
static const char *const smoothing_names[] = {"grayscale", "none", "subpixel"};
/* What FontSmoothing said before: Windows' names. */
static const char *const smoothing_old_names[] = {"standard", "none", "cleartype"};
static const char *const subpixel_names[] = {"rgb", "bgr", "vrgb", "vbgr"};
static const char *const hinting_names[] = {"slight", "none", "medium", "full"};
static const char *const transition_names[] = {"fade", "scroll"};
static const char *const edge_names[] = {"bottom", "top", "left", "right"};
static const char *const buttons_names[] = {"always", "whenfull", "never"};
static const char *const tray_rule_names[] = {"auto", "hide", "show"};
static const char *const power_scheme_names[XD_POWER_SCHEME_COUNT] = {
    [XD_POWER_HOME] = "home",
    [XD_POWER_PORTABLE] = "portable",
    [XD_POWER_PRESENTATION] = "presentation",
    [XD_POWER_ALWAYS_ON] = "alwayson",
    [XD_POWER_MINIMAL] = "minimal",
    [XD_POWER_MAX_BATTERY] = "maxbattery",
};
static const char *const alarm_action_names[XD_ALARM_ACTION_COUNT] = {
    [XD_ALARM_NOTHING] = "none",
    [XD_ALARM_STANDBY] = "standby",
    [XD_ALARM_HIBERNATE] = "hibernate",
    [XD_ALARM_SHUTDOWN] = "shutdown",
};
static const char *const sentry_names[XD_SENTRY_COUNT] = {
    [XD_SENTRY_NONE] = "none",
    [XD_SENTRY_CAPTION] = "caption",
    [XD_SENTRY_WINDOW] = "window",
    [XD_SENTRY_DESKTOP] = "desktop",
};
static const char *const desk_icon_size_names[] = {"small", "medium", "large"};
/* [DesktopIcons]' Change Icon keys, by xd_desk_icon. */
static const char *const desk_icon_keys[XD_DESK_ICON_COUNT] = {
    [XD_DESK_ICON_COMPUTER] = "MyComputerIcon",
    [XD_DESK_ICON_DOCUMENTS] = "MyDocumentsIcon",
    [XD_DESK_ICON_NETWORK] = "MyNetworkPlacesIcon",
    [XD_DESK_ICON_TRASH_FULL] = "RecycleBinFullIcon",
    [XD_DESK_ICON_TRASH_EMPTY] = "RecycleBinEmptyIcon",
};
static const char *const desk_icon_defaults[XD_DESK_ICON_COUNT] = {
    [XD_DESK_ICON_COMPUTER] = "computer",
    [XD_DESK_ICON_DOCUMENTS] = "folder-documents",
    [XD_DESK_ICON_NETWORK] = "network-workgroup",
    [XD_DESK_ICON_TRASH_FULL] = "user-trash-full",
    [XD_DESK_ICON_TRASH_EMPTY] = "user-trash",
};
static const char *const view_names[] = {"thumbnails", "tiles", "icons", "list", "details"};

xd_tray_rule xd_settings_tray_rule(const xd_settings *s, const char *id) {
    for (size_t i = 0; id != nullptr && i < s->tray_rule_count; i++) {
        if (strcmp(s->tray_rules[i].id, id) == 0)
            return s->tray_rules[i].rule;
    }
    return XD_TRAY_AUTO;
}

void xd_settings_set_tray_rule(xd_settings *s, const char *id, xd_tray_rule rule) {
    for (size_t i = 0; i < s->tray_rule_count; i++) {
        if (strcmp(s->tray_rules[i].id, id) != 0)
            continue;
        if (rule == XD_TRAY_AUTO)
            s->tray_rules[i] = s->tray_rules[--s->tray_rule_count];
        else
            s->tray_rules[i].rule = rule;
        return;
    }
    if (rule == XD_TRAY_AUTO || s->tray_rule_count == XD_TRAY_RULES_MAX || id[0] == '\0' ||
        strlen(id) >= sizeof(s->tray_rules[0].id))
        return;
    xd_tray_behavior *b = &s->tray_rules[s->tray_rule_count++];
    *b = (xd_tray_behavior){.rule = rule};
    memcpy(b->id, id, strlen(id) + 1);
}

const char *xd_wallpaper_mode_name(xd_wallpaper_mode m) {
    XD_ASSERT((unsigned)m < XD_WALLPAPER_MODE_COUNT);
    return mode_names[m];
}

const char *xd_desk_icon_default(xd_desk_icon icon) {
    XD_ASSERT((unsigned)icon < XD_DESK_ICON_COUNT);
    return desk_icon_defaults[icon];
}

int xd_desk_icon_px(const xd_settings *s) {
    switch (s->desktop_icons.size) {
    case XD_DESK_ICONS_SMALL:
        return 16;
    case XD_DESK_ICONS_LARGE:
        return 48;
    case XD_DESK_ICONS_MEDIUM:
        break;
    }
    return s->large_icons ? 48 : 32;
}

const char *xd_settings_icon_theme(const xd_settings *s) {
    if (s->icon_theme[0] != '\0')
        return s->icon_theme;
    const char *env = getenv("XD_ICON_THEME");
    return env != nullptr && env[0] != '\0' ? env : "breeze";
}

const char *xd_settings_gtk_theme(const xd_settings *s) {
    return s->gtk_theme[0] != '\0' ? s->gtk_theme : XD_GTK_THEME_NAME;
}

const char *xd_qt_style_auto(xd_visual_style style) {
    /* Windows draws 2000's raised and sunken bevels; Fusion's flat,
     * rounded and lightly shaded controls are the closest Qt has to XP's
     * and 7's. */
    return style == XD_STYLE_CLASSIC ? "Windows" : "Fusion";
}

const char *xd_settings_qt_style(const xd_settings *s) {
    return s->qt_style[0] != '\0' ? s->qt_style : xd_qt_style_auto(s->style);
}

/* Sets `var` to `value` unless the user set it: it isn't set, or holds
 * what we set before (XEONDESKTOP_<var>). */
static bool qt_env_set(const char *var, const char *value) {
    const char *old = getenv(var);
    if (old != nullptr && !xd_qt_env_ours(var))
        return false;
    bool changed = old == nullptr || strcmp(old, value) != 0;
    char *mark = g_strconcat("XEONDESKTOP_", var, nullptr);
    setenv(var, value, true);
    setenv(mark, value, true);
    g_free(mark);
    return changed;
}

bool xd_qt_env_ours(const char *var) {
    char *mark = g_strconcat("XEONDESKTOP_", var, nullptr);
    const char *ours = getenv(mark);
    const char *now = getenv(var);
    g_free(mark);
    return ours != nullptr && now != nullptr && strcmp(ours, now) == 0;
}

bool xd_settings_qt_env(const xd_settings *s) {
    /* All are evaluated: no short circuit. */
    bool theme = qt_env_set("QT_QPA_PLATFORMTHEME", "gtk3");
    bool style = qt_env_set("QT_STYLE_OVERRIDE", xd_settings_qt_style(s));
    /* Our palette goes with our GTK theme; with another, Qt takes that
     * theme's colours its own way. Empty rather than unset, so that the
     * D-Bus and systemd environments, which can only be given values,
     * follow too. */
    char *palette = s->gtk_theme[0] == '\0' ? xd_gtk_theme_qt_palette_path() : g_strdup("");
    bool colours = qt_env_set("QT_GUI_GTK_JSON", palette);
    g_free(palette);
    return theme || style || colours;
}

const char *xd_alarm_action_name(xd_alarm_action action) {
    return action < XD_ALARM_ACTION_COUNT ? alarm_action_names[action] : alarm_action_names[0];
}

xd_alarm_fired xd_battery_alarm_check(const xd_settings *s, xd_alarm_done *done, double percent,
                                      bool discharging) {
    if (!discharging) {
        *done = (xd_alarm_done){0};
        return XD_ALARM_FIRED_NONE;
    }
    const xd_battery_alarm *low = &s->low_alarm, *critical = &s->critical_alarm;
    if (critical->on && percent <= critical->percent && !done->critical) {
        done->critical = done->low = true;
        return XD_ALARM_FIRED_CRITICAL;
    }
    if (low->on && percent <= low->percent && !done->low) {
        done->low = true;
        return XD_ALARM_FIRED_LOW;
    }
    return XD_ALARM_FIRED_NONE;
}

const char *xd_power_scheme_name(xd_power_scheme scheme) {
    XD_ASSERT((unsigned)scheme < XD_POWER_SCHEME_COUNT);
    return power_scheme_names[scheme];
}

void xd_settings_defaults(xd_settings *s) {
    *s = (xd_settings){
        .style = XD_STYLE_CLASSIC,
        .glass_color = XD_GLASS_DEFAULT,
        .glass_transparency = true,
        .wallpaper_mode = XD_WALLPAPER_FILL,
        /* Windows 2000's icons but Internet Explorer; Align to Grid on. */
        .desktop_icons = {.documents = true, .computer = true, .network = true, .show = true,
                          .align_to_grid = true, .size = XD_DESK_ICONS_MEDIUM},
        .menu_transitions = true,
        .animate_windows = true,
        .full_window_drag = true,
        .quick_launch = true,
        .locked = true,
        .on_top = true,
        .group = true,
        .show_clock = true,
        .hide_inactive = true,
        .all_displays = true,
        .edge = XD_EDGE_BOTTOM,
        .rows = 1,
        .side_width = 150,
        .start_large_icons = true,
        .start_programs = 6, /* XP's */
        .start_highlight_new = true,
        .screensaver_wait = 15,
        /* Windows' defaults: about 30 repeats a second after half a second. */
        .keyboard = {.repeat_delay = 500, .repeat_rate = 30, .cursor_blink_ms = 530},
        .mouse = {.acceleration = true, .double_click_ms = 500, .wheel_lines = 3,
                  .cursor_size = 24, .click_lock_ms = 1200},
        /* XP's: every shortcut on, and the warnings and sounds with them;
         * FilterKeys' shortcut turns on SlowKeys. */
        .access = {.sticky_lock = true, .sticky_shortcut = true, .sticky_two_keys_off = true,
                   .sticky_sound = true, .filter_shortcut = true, .slow_keys = true,
                   .filter_beep = true, .bounce_ms = 500, .slow_ms = 1000,
                   .filter_repeat_delay = 1000, .filter_repeat_ms = 500,
                   .toggle_shortcut = true, .mouse_keys_shortcut = true,
                   .mouse_keys_modifiers = true, .mouse_keys_numlock = true,
                   .mouse_keys_speed = 80, .mouse_keys_accel_ms = 3000, .warn = true,
                   .sound_on_off = true, .sticky_indicator = true, .filter_indicator = true,
                   .mouse_keys_indicator = true, .sound_sentry_effect = XD_SENTRY_WINDOW,
                   .auto_reset_minutes = XD_AUTO_RESET_MIN},
        .volume_icon = true,
        .battery_icon = true,
        .resume_password = true, /* as in XP */
        /* XP's levels. No action by default: UPower takes its own
         * CriticalPowerAction just below. */
        .low_alarm = {.on = true, .percent = 10, .sound = true, .message = true},
        .critical_alarm = {.on = true, .percent = 3, .sound = true, .message = true},
        .bluetooth_icon = true,
        /* XP's, except extensions, which stay visible: renaming edits the
         * whole name. */
        .folders = {.full_path_address = true, .remember_views = true},
    };
    xd_saver_settings_defaults(&s->savers);
}

static void copy_into(char *dst, size_t cap, xd_str v) {
    size_t n = XD_MIN(v.len, cap - 1);
    memcpy(dst, v.ptr, n);
    dst[n] = '\0';
}

static int index_of(const char *const *names, size_t n, xd_str v) {
    for (size_t i = 0; i < n; i++) {
        if (xd_str_eq_nocase(v, xd_str_from_cstr(names[i])))
            return (int)i;
    }
    return -1;
}

static void parse_bool(xd_str v, bool *out) {
    if (xd_str_eq_nocase(v, XD_STR("true")) || xd_str_eq(v, XD_STR("1")))
        *out = true;
    else if (xd_str_eq_nocase(v, XD_STR("false")) || xd_str_eq(v, XD_STR("0")))
        *out = false;
}

/* A scheme is a file stem, not a path: safe file-name characters only. */
static void parse_stem(xd_str v, char *dst, size_t cap) {
    bool ok = v.len < cap;
    for (size_t i = 0; ok && i < v.len; i++) {
        char c = v.ptr[i];
        ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             c == '-' || c == '_';
    }
    if (ok)
        copy_into(dst, cap, v);
}

/* A colour as AARRGGBB in hex, as Windows writes ColorizationColor. */
static void parse_argb(xd_str v, uint32_t *out) {
    char buf[9];
    if (v.len != 8)
        return;
    memcpy(buf, v.ptr, 8);
    buf[8] = '\0';
    guint64 n;
    if (g_ascii_string_to_unsigned(buf, 16, 0, UINT32_MAX, &n, nullptr))
        *out = (uint32_t)n;
}

static void parse_int(xd_str v, int64_t lo, int64_t hi, int *out) {
    int64_t n;
    if (xd_str_parse_i64(v, lo, hi, &n))
        *out = (int)n;
}

/* A theme's directory name under icons/ or themes/: no paths, nothing
 * hidden, nothing that needs escaping in an environment variable. GtkTheme
 * may carry GTK_THEME's variant (":dark"). */
static void parse_theme_dir(xd_str v, char *dst, size_t cap) {
    bool ok = v.len < cap && (v.len == 0 || v.ptr[0] != '.');
    for (size_t i = 0; ok && i < v.len; i++)
        ok = v.ptr[i] != '/' && (unsigned char)v.ptr[i] >= ' ';
    if (ok)
        copy_into(dst, cap, v);
}

/* A Qt style's key ("Fusion", "kvantum-dark"): it goes into the
 * environment, so nothing else. */
static void parse_qt_style(xd_str v, char *dst, size_t cap) {
    bool ok = v.len < cap;
    for (size_t i = 0; ok && i < v.len; i++)
        ok = g_ascii_isalnum(v.ptr[i]) || memchr("-_.+", v.ptr[i], 4) != nullptr;
    if (ok)
        copy_into(dst, cap, v);
}

static void parse_mouse(xd_mouse_settings *m, xd_str key, xd_str v) {
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (KEY("LeftHanded")) {
        parse_bool(v, &m->left_handed);
    } else if (KEY("Speed")) {
        parse_int(v, XD_MOUSE_SPEED_MIN, XD_MOUSE_SPEED_MAX, &m->speed);
    } else if (KEY("Acceleration")) {
        parse_bool(v, &m->acceleration);
    } else if (KEY("DoubleClickMs")) {
        parse_int(v, XD_DOUBLE_CLICK_MIN, XD_DOUBLE_CLICK_MAX, &m->double_click_ms);
    } else if (KEY("WheelLines")) {
        parse_int(v, 1, 30, &m->wheel_lines);
    } else if (KEY("CursorTheme")) {
        /* A directory name under icons/: no paths, nothing hidden. */
        if (v.len < sizeof(m->cursor_theme) && memchr(v.ptr, '/', v.len) == nullptr &&
            (v.len == 0 || v.ptr[0] != '.'))
            copy_into(m->cursor_theme, sizeof(m->cursor_theme), v);
    } else if (KEY("CursorSize")) {
        parse_int(v, XD_CURSOR_SIZE_MIN, XD_CURSOR_SIZE_MAX, &m->cursor_size);
    } else if (KEY("ClickLock")) {
        parse_bool(v, &m->click_lock);
    } else if (KEY("ClickLockMs")) {
        parse_int(v, XD_CLICK_LOCK_MIN, XD_CLICK_LOCK_MAX, &m->click_lock_ms);
    }
#undef KEY
}

static void parse_access(xd_access_settings *a, xd_str key, xd_str v) {
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    static const struct {
        const char *key;
        size_t offset;
    } flags[] = {
        {"StickyKeys", offsetof(xd_access_settings, sticky_keys)},
        {"StickyKeysLock", offsetof(xd_access_settings, sticky_lock)},
        {"StickyKeysShortcut", offsetof(xd_access_settings, sticky_shortcut)},
        {"StickyKeysTwoKeysOff", offsetof(xd_access_settings, sticky_two_keys_off)},
        {"StickyKeysSound", offsetof(xd_access_settings, sticky_sound)},
        {"FilterKeys", offsetof(xd_access_settings, filter_keys)},
        /* FilterKeys' name from before SlowKeys was done. */
        {"BounceKeys", offsetof(xd_access_settings, filter_keys)},
        {"FilterKeysShortcut", offsetof(xd_access_settings, filter_shortcut)},
        {"SlowKeys", offsetof(xd_access_settings, slow_keys)},
        {"FilterKeysBeep", offsetof(xd_access_settings, filter_beep)},
        {"FilterKeysNoRepeat", offsetof(xd_access_settings, filter_no_repeat)},
        {"ToggleKeys", offsetof(xd_access_settings, toggle_keys)},
        {"ToggleKeysShortcut", offsetof(xd_access_settings, toggle_shortcut)},
        {"MouseKeys", offsetof(xd_access_settings, mouse_keys)},
        {"MouseKeysShortcut", offsetof(xd_access_settings, mouse_keys_shortcut)},
        {"MouseKeysModifiers", offsetof(xd_access_settings, mouse_keys_modifiers)},
        {"MouseKeysNumLock", offsetof(xd_access_settings, mouse_keys_numlock)},
        {"WarnOnShortcut", offsetof(xd_access_settings, warn)},
        {"SoundOnOff", offsetof(xd_access_settings, sound_on_off)},
        {"StickyKeysIndicator", offsetof(xd_access_settings, sticky_indicator)},
        {"FilterKeysIndicator", offsetof(xd_access_settings, filter_indicator)},
        {"MouseKeysIndicator", offsetof(xd_access_settings, mouse_keys_indicator)},
        {"SoundSentry", offsetof(xd_access_settings, sound_sentry)},
        {"ShowSounds", offsetof(xd_access_settings, show_sounds)},
        {"AutoReset", offsetof(xd_access_settings, auto_reset)},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(flags); i++) {
        if (xd_str_eq_nocase(key, xd_str_from_cstr(flags[i].key))) {
            parse_bool(v, (bool *)((char *)a + flags[i].offset));
            return;
        }
    }
    if (KEY("BounceMs")) {
        parse_int(v, 100, 2000, &a->bounce_ms);
    } else if (KEY("SlowKeysMs")) {
        parse_int(v, 0, 20000, &a->slow_ms);
    } else if (KEY("FilterKeysRepeatDelay")) {
        parse_int(v, 300, 20000, &a->filter_repeat_delay);
    } else if (KEY("FilterKeysRepeatMs")) {
        parse_int(v, 300, 20000, &a->filter_repeat_ms);
    } else if (KEY("MouseKeysSpeed")) {
        parse_int(v, XD_MOUSE_KEYS_SPEED_MIN, XD_MOUSE_KEYS_SPEED_MAX, &a->mouse_keys_speed);
    } else if (KEY("MouseKeysAccelMs")) {
        parse_int(v, XD_MOUSE_KEYS_ACCEL_MIN, XD_MOUSE_KEYS_ACCEL_MAX, &a->mouse_keys_accel_ms);
    } else if (KEY("SoundSentryEffect")) {
        int i = index_of(sentry_names, XD_ARRAY_LEN(sentry_names), v);
        if (i >= 0)
            a->sound_sentry_effect = (xd_sound_sentry)i;
    } else if (KEY("AutoResetMinutes")) {
        parse_int(v, XD_AUTO_RESET_MIN, XD_AUTO_RESET_MAX, &a->auto_reset_minutes);
    } else if (KEY("HighContrast")) {
        parse_bool(v, &a->high_contrast);
    } else if (KEY("SavedStyle")) {
        xd_visual_style st;
        if (xd_visual_style_from_name(v, &st))
            a->saved_style = st;
    } else if (KEY("SavedScheme")) {
        parse_stem(v, a->saved_scheme, sizeof(a->saved_scheme));
    }
#undef KEY
}

/* [Power] LowAlarm*, CriticalAlarm*: `key` without its prefix. */
static void parse_alarm(xd_battery_alarm *al, xd_str key, xd_str v) {
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (KEY("")) {
        parse_bool(v, &al->on);
    } else if (KEY("Level")) {
        parse_int(v, 0, 100, &al->percent);
    } else if (KEY("Sound")) {
        parse_bool(v, &al->sound);
    } else if (KEY("Message")) {
        parse_bool(v, &al->message);
    } else if (KEY("Action")) {
        int i = index_of(alarm_action_names, XD_ARRAY_LEN(alarm_action_names), v);
        if (i >= 0)
            al->action = (xd_alarm_action)i;
    }
#undef KEY
}

static void parse_desktop_icons(xd_desktop_icon_settings *d, xd_str key, xd_str v) {
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (KEY("MyDocuments")) {
        parse_bool(v, &d->documents);
    } else if (KEY("MyComputer")) {
        parse_bool(v, &d->computer);
    } else if (KEY("MyNetworkPlaces")) {
        parse_bool(v, &d->network);
    } else if (KEY("WebBrowser")) {
        parse_bool(v, &d->browser);
    } else if (KEY("Show")) {
        parse_bool(v, &d->show);
    } else if (KEY("AutoArrange")) {
        parse_bool(v, &d->auto_arrange);
    } else if (KEY("AlignToGrid")) {
        parse_bool(v, &d->align_to_grid);
    } else if (KEY("Size")) {
        int i = index_of(desk_icon_size_names, XD_ARRAY_LEN(desk_icon_size_names), v);
        if (i >= 0)
            d->size = (xd_desk_icon_size)i;
    } else {
        /* Change Icon: an image file, by its absolute path. */
        for (size_t i = 0; i < XD_DESK_ICON_COUNT; i++) {
            if (xd_str_eq_nocase(key, xd_str_from_cstr(desk_icon_keys[i])) &&
                (v.len == 0 || (v.ptr[0] == '/' && v.len < sizeof(d->icons[i]))))
                copy_into(d->icons[i], sizeof(d->icons[i]), v);
        }
    }
#undef KEY
}

/* ---- [ScreenSaver.*] --------------------------------------------------------- */

static const char *const pipes_joint_names[XD_PIPES_JOINT_COUNT] = {
    [XD_PIPES_ELBOW] = "elbow", [XD_PIPES_BALL] = "ball", [XD_PIPES_MIXED] = "mixed",
    [XD_PIPES_CYCLE] = "cycle"};
static const char *const text3d_rotation_names[XD_TEXT3D_ROTATION_COUNT] = {
    [XD_TEXT3D_NONE] = "none", [XD_TEXT3D_SPIN] = "spin", [XD_TEXT3D_SEESAW] = "seesaw",
    [XD_TEXT3D_WOBBLE] = "wobble", [XD_TEXT3D_RANDOM] = "random"};
static const char *const text3d_surface_names[XD_TEXT3D_SURFACE_COUNT] = {
    [XD_TEXT3D_SOLID] = "solid", [XD_TEXT3D_REFLECTION] = "reflection"};

void xd_saver_settings_defaults(xd_saver_settings *s) {
    *s = (xd_saver_settings){
        .marquee = {.text = "XeonDesktop",
                    .font = {.family = "Times New Roman", .size_pt = 48},
                    .color = {255, 0, 0},
                    .random_position = true,
                    .speed = 5},
        .starfield = {.speed = 5, .density = 200},
        .mystify = {.shapes = {{.active = true, .lines = 8, .colors = {{255, 0, 0}, {255, 255, 0}}},
                               {.active = true, .lines = 8, .colors = {{0, 0, 255}, {0, 255, 255}}}},
                    .clear_screen = true},
        .beziers = {.curves = 1, .lines = 24, .speed = 5},
        .flying = {.speed = 5, .density = 20},
        .pipes = {.joints = XD_PIPES_MIXED},
        .text3d = {.text = "XeonDesktop",
                   .rotation = XD_TEXT3D_SPIN,
                   .speed = 5,
                   .size = 5,
                   .surface = XD_TEXT3D_REFLECTION,
                   .color = {255, 0, 0}},
    };
}

/* "R G B", as the registry keeps Windows' colours (and our schemes do). */
static void parse_rgb_value(xd_str v, xd_rgb *out) {
    gchar *text = g_strndup(v.ptr, v.len);
    gchar **parts = g_strsplit_set(g_strstrip(text), " \t", -1);
    int64_t c[3];
    size_t n = 0;
    bool ok = true;
    for (gchar **p = parts; ok && *p != nullptr; p++) {
        if (**p == '\0')
            continue; /* runs of spaces */
        ok = n < 3 && xd_str_parse_i64(xd_str_from_cstr(*p), 0, 255, &c[n++]);
    }
    if (ok && n == 3)
        *out = (xd_rgb){(uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]};
    g_strfreev(parts);
    g_free(text);
}

static void set_rgb(GKeyFile *kf, const char *group, const char *key, xd_rgb c) {
    char buf[16];
    g_snprintf(buf, sizeof(buf), "%u %u %u", c.r, c.g, c.b);
    g_key_file_set_string(kf, group, key, buf);
}

/* Free text shown on the screen: anything but control characters. */
static void parse_saver_text(xd_str v, char *dst, size_t cap) {
    bool ok = v.len < cap;
    for (size_t i = 0; ok && i < v.len; i++)
        ok = (unsigned char)v.ptr[i] >= ' ';
    if (ok)
        copy_into(dst, cap, v);
}

static void parse_saver_entry(xd_saver_settings *sv, xd_str section, xd_str key, xd_str v) {
#define SEC(n) xd_str_eq_nocase(section, XD_STR("ScreenSaver." n))
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (SEC("Marquee")) {
        xd_marquee_settings *m = &sv->marquee;
        if (KEY("Text"))
            parse_saver_text(v, m->text, sizeof(m->text));
        else if (KEY("Font"))
            parse_saver_text(v, m->font.family, sizeof(m->font.family));
        else if (KEY("FontSize")) {
            int pt = (int)m->font.size_pt;
            parse_int(v, 6, 200, &pt);
            m->font.size_pt = pt;
        } else if (KEY("Bold"))
            parse_bool(v, &m->font.bold);
        else if (KEY("Italic"))
            parse_bool(v, &m->font.italic);
        else if (KEY("TextColor"))
            parse_rgb_value(v, &m->color);
        else if (KEY("BackgroundColor"))
            parse_rgb_value(v, &m->background);
        else if (KEY("Position")) {
            if (xd_str_eq_nocase(v, XD_STR("random")))
                m->random_position = true;
            else if (xd_str_eq_nocase(v, XD_STR("centered")))
                m->random_position = false;
        } else if (KEY("Speed"))
            parse_int(v, 0, XD_SAVER_SLIDER_MAX, &m->speed);
    } else if (SEC("Starfield")) {
        if (KEY("WarpSpeed"))
            parse_int(v, 0, XD_SAVER_SLIDER_MAX, &sv->starfield.speed);
        else if (KEY("Density"))
            parse_int(v, XD_STARFIELD_DENSITY_MIN, XD_STARFIELD_DENSITY_MAX,
                      &sv->starfield.density);
    } else if (SEC("Mystify")) {
        if (KEY("ClearScreen")) {
            parse_bool(v, &sv->mystify.clear_screen);
            return;
        }
        /* Polygon1Active, Polygon2Lines, ... */
        for (int i = 0; i < XD_MYSTIFY_SHAPES; i++) {
            char prefix[16];
            g_snprintf(prefix, sizeof(prefix), "Polygon%d", i + 1);
            xd_str p = xd_str_from_cstr(prefix);
            if (key.len <= p.len || !xd_str_eq_nocase(xd_str_sub(key, 0, p.len), p))
                continue;
            xd_str rest = xd_str_sub(key, p.len, key.len);
            xd_mystify_shape *sh = &sv->mystify.shapes[i];
            if (xd_str_eq_nocase(rest, XD_STR("Active")))
                parse_bool(v, &sh->active);
            else if (xd_str_eq_nocase(rest, XD_STR("Lines")))
                parse_int(v, 1, XD_MYSTIFY_LINES_MAX, &sh->lines);
            else if (xd_str_eq_nocase(rest, XD_STR("Colors"))) {
                if (xd_str_eq_nocase(v, XD_STR("two")))
                    sh->two_colors = true;
                else if (xd_str_eq_nocase(v, XD_STR("random")))
                    sh->two_colors = false;
            } else if (xd_str_eq_nocase(rest, XD_STR("Color1")))
                parse_rgb_value(v, &sh->colors[0]);
            else if (xd_str_eq_nocase(rest, XD_STR("Color2")))
                parse_rgb_value(v, &sh->colors[1]);
        }
    } else if (SEC("Beziers")) {
        if (KEY("Curves"))
            parse_int(v, 1, XD_BEZIERS_CURVES_MAX, &sv->beziers.curves);
        else if (KEY("Lines"))
            parse_int(v, 1, XD_BEZIERS_LINES_MAX, &sv->beziers.lines);
        else if (KEY("Speed"))
            parse_int(v, 0, XD_SAVER_SLIDER_MAX, &sv->beziers.speed);
    } else if (SEC("Flying")) {
        if (KEY("WarpSpeed"))
            parse_int(v, 0, XD_SAVER_SLIDER_MAX, &sv->flying.speed);
        else if (KEY("Density"))
            parse_int(v, XD_FLYING_DENSITY_MIN, XD_FLYING_DENSITY_MAX, &sv->flying.density);
    } else if (SEC("Pipes")) {
        if (KEY("Pipes")) {
            if (xd_str_eq_nocase(v, XD_STR("multiple")))
                sv->pipes.multiple = true;
            else if (xd_str_eq_nocase(v, XD_STR("single")))
                sv->pipes.multiple = false;
        } else if (KEY("Joints")) {
            int i = index_of(pipes_joint_names, XD_ARRAY_LEN(pipes_joint_names), v);
            if (i >= 0)
                sv->pipes.joints = (xd_pipes_joint)i;
        }
    } else if (SEC("Text3D")) {
        xd_text3d_settings *t = &sv->text3d;
        if (KEY("Display")) {
            if (xd_str_eq_nocase(v, XD_STR("time")))
                t->show_time = true;
            else if (xd_str_eq_nocase(v, XD_STR("text")))
                t->show_time = false;
        } else if (KEY("Text"))
            parse_saver_text(v, t->text, sizeof(t->text));
        else if (KEY("Rotation")) {
            int i = index_of(text3d_rotation_names, XD_ARRAY_LEN(text3d_rotation_names), v);
            if (i >= 0)
                t->rotation = (xd_text3d_rotation)i;
        } else if (KEY("Speed"))
            parse_int(v, 0, XD_SAVER_SLIDER_MAX, &t->speed);
        else if (KEY("Size"))
            parse_int(v, 0, XD_SAVER_SLIDER_MAX, &t->size);
        else if (KEY("Surface")) {
            int i = index_of(text3d_surface_names, XD_ARRAY_LEN(text3d_surface_names), v);
            if (i >= 0)
                t->surface = (xd_text3d_surface)i;
        } else if (KEY("Color"))
            parse_rgb_value(v, &t->color);
    }
#undef SEC
#undef KEY
}

static void saver_settings_to_keyfile(GKeyFile *kf, const xd_saver_settings *sv) {
    const char *g = "ScreenSaver.Marquee";
    g_key_file_set_string(kf, g, "Text", sv->marquee.text);
    g_key_file_set_string(kf, g, "Font", sv->marquee.font.family);
    g_key_file_set_integer(kf, g, "FontSize", (int)sv->marquee.font.size_pt);
    g_key_file_set_boolean(kf, g, "Bold", sv->marquee.font.bold);
    g_key_file_set_boolean(kf, g, "Italic", sv->marquee.font.italic);
    set_rgb(kf, g, "TextColor", sv->marquee.color);
    set_rgb(kf, g, "BackgroundColor", sv->marquee.background);
    g_key_file_set_string(kf, g, "Position", sv->marquee.random_position ? "random" : "centered");
    g_key_file_set_integer(kf, g, "Speed", sv->marquee.speed);
    g = "ScreenSaver.Starfield";
    g_key_file_set_integer(kf, g, "WarpSpeed", sv->starfield.speed);
    g_key_file_set_integer(kf, g, "Density", sv->starfield.density);
    g = "ScreenSaver.Mystify";
    for (int i = 0; i < XD_MYSTIFY_SHAPES; i++) {
        const xd_mystify_shape *sh = &sv->mystify.shapes[i];
        char key[32];
        g_snprintf(key, sizeof(key), "Polygon%dActive", i + 1);
        g_key_file_set_boolean(kf, g, key, sh->active);
        g_snprintf(key, sizeof(key), "Polygon%dLines", i + 1);
        g_key_file_set_integer(kf, g, key, sh->lines);
        g_snprintf(key, sizeof(key), "Polygon%dColors", i + 1);
        g_key_file_set_string(kf, g, key, sh->two_colors ? "two" : "random");
        g_snprintf(key, sizeof(key), "Polygon%dColor1", i + 1);
        set_rgb(kf, g, key, sh->colors[0]);
        g_snprintf(key, sizeof(key), "Polygon%dColor2", i + 1);
        set_rgb(kf, g, key, sh->colors[1]);
    }
    g_key_file_set_boolean(kf, g, "ClearScreen", sv->mystify.clear_screen);
    g = "ScreenSaver.Beziers";
    g_key_file_set_integer(kf, g, "Curves", sv->beziers.curves);
    g_key_file_set_integer(kf, g, "Lines", sv->beziers.lines);
    g_key_file_set_integer(kf, g, "Speed", sv->beziers.speed);
    g = "ScreenSaver.Flying";
    g_key_file_set_integer(kf, g, "WarpSpeed", sv->flying.speed);
    g_key_file_set_integer(kf, g, "Density", sv->flying.density);
    g = "ScreenSaver.Pipes";
    g_key_file_set_string(kf, g, "Pipes", sv->pipes.multiple ? "multiple" : "single");
    g_key_file_set_string(kf, g, "Joints", pipes_joint_names[sv->pipes.joints]);
    g = "ScreenSaver.Text3D";
    g_key_file_set_string(kf, g, "Display", sv->text3d.show_time ? "time" : "text");
    g_key_file_set_string(kf, g, "Text", sv->text3d.text);
    g_key_file_set_string(kf, g, "Rotation", text3d_rotation_names[sv->text3d.rotation]);
    g_key_file_set_integer(kf, g, "Speed", sv->text3d.speed);
    g_key_file_set_integer(kf, g, "Size", sv->text3d.size);
    g_key_file_set_string(kf, g, "Surface", text3d_surface_names[sv->text3d.surface]);
    set_rgb(kf, g, "Color", sv->text3d.color);
}

static bool on_entry(void *user, xd_str section, xd_str key, xd_str v) {
    xd_settings *s = user;
#define SEC(n) xd_str_eq_nocase(section, XD_STR(n))
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (SEC("Appearance")) {
        if (KEY("Style")) {
            xd_visual_style st;
            if (xd_visual_style_from_name(v, &st))
                s->style = st;
        } else if (KEY("Scheme")) {
            parse_stem(v, s->scheme, sizeof(s->scheme));
        } else if (KEY("FontSize")) {
            int i = index_of(font_size_names, XD_ARRAY_LEN(font_size_names), v);
            if (i >= 0)
                s->font_size = (xd_font_size)i;
        } else if (KEY("FontSmoothing")) {
            int i = index_of(smoothing_names, XD_ARRAY_LEN(smoothing_names), v);
            if (i < 0)
                i = index_of(smoothing_old_names, XD_ARRAY_LEN(smoothing_old_names), v);
            if (i >= 0)
                s->font_smoothing.antialias = (xd_font_antialias)i;
        } else if (KEY("FontSubpixelOrder")) {
            int i = index_of(subpixel_names, XD_ARRAY_LEN(subpixel_names), v);
            if (i >= 0)
                s->font_smoothing.order = (xd_subpixel_order)i;
        } else if (KEY("FontHinting")) {
            int i = index_of(hinting_names, XD_ARRAY_LEN(hinting_names), v);
            if (i >= 0)
                s->font_smoothing.hinting = (xd_font_hinting)i;
        } else if (KEY("GlassColor")) {
            parse_argb(v, &s->glass_color);
        } else if (KEY("GlassTransparency")) {
            parse_bool(v, &s->glass_transparency);
        } else if (KEY("IconTheme")) {
            parse_theme_dir(v, s->icon_theme, sizeof(s->icon_theme));
        } else if (KEY("GtkTheme")) {
            parse_theme_dir(v, s->gtk_theme, sizeof(s->gtk_theme));
        } else if (KEY("QtStyle")) {
            parse_qt_style(v, s->qt_style, sizeof(s->qt_style));
        }
    } else if (SEC("Desktop")) {
        if (KEY("Wallpaper")) {
            if (v.len == 0 || (v.ptr[0] == '/' && v.len < sizeof(s->wallpaper)))
                copy_into(s->wallpaper, sizeof(s->wallpaper), v);
        } else if (KEY("WallpaperMode")) {
            int i = index_of(mode_names, XD_ARRAY_LEN(mode_names), v);
            if (i >= 0)
                s->wallpaper_mode = (xd_wallpaper_mode)i;
        }
    } else if (SEC("CustomScheme")) {
        (void)xd_scheme_custom_set(&s->custom_scheme, key, v); /* a bad value is left out */
    } else if (SEC("DesktopIcons")) {
        parse_desktop_icons(&s->desktop_icons, key, v);
    } else if (SEC("Effects")) {
        if (KEY("MenuTransitions")) {
            parse_bool(v, &s->menu_transitions);
        } else if (KEY("MenuTransition")) {
            int i = index_of(transition_names, XD_ARRAY_LEN(transition_names), v);
            if (i >= 0)
                s->menu_transition = (xd_menu_transition)i;
        } else if (KEY("FullWindowDrag")) {
            parse_bool(v, &s->full_window_drag);
        } else if (KEY("AnimateWindows")) {
            parse_bool(v, &s->animate_windows);
        } else if (KEY("LargeIcons")) {
            parse_bool(v, &s->large_icons);
        } else if (KEY("HideKeyboardCues")) {
            parse_bool(v, &s->hide_keyboard_cues);
        }
    } else if (SEC("Windows")) {
        if (KEY("AlwaysDrawFrames"))
            parse_bool(v, &s->always_frames);
    } else if (SEC("Taskbar")) {
        if (KEY("QuickLaunch"))
            parse_bool(v, &s->quick_launch);
        else if (KEY("Locked"))
            parse_bool(v, &s->locked);
        else if (KEY("AutoHide"))
            parse_bool(v, &s->auto_hide);
        else if (KEY("OnTop"))
            parse_bool(v, &s->on_top);
        else if (KEY("GroupButtons"))
            parse_bool(v, &s->group);
        else if (KEY("Buttons")) {
            int i = index_of(buttons_names, XD_ARRAY_LEN(buttons_names), v);
            if (i >= 0)
                s->buttons = (xd_taskbar_buttons)i;
        }
        else if (KEY("ShowClock"))
            parse_bool(v, &s->show_clock);
        else if (KEY("HideInactiveIcons"))
            parse_bool(v, &s->hide_inactive);
        else if (KEY("AllDisplays"))
            parse_bool(v, &s->all_displays);
        else if (KEY("Edge")) {
            int i = index_of(edge_names, XD_ARRAY_LEN(edge_names), v);
            if (i >= 0)
                s->edge = (xd_edge)i;
        } else if (KEY("Rows")) {
            int64_t n;
            if (xd_str_parse_i64(v, 1, 8, &n))
                s->rows = (int)n;
        } else if (KEY("SideWidth")) {
            int64_t n;
            if (xd_str_parse_i64(v, 40, 800, &n))
                s->side_width = (int)n;
        } else if (KEY("QuickLaunchLargeIcons")) {
            parse_bool(v, &s->quick_large);
        } else if (KEY("QuickLaunchShowText")) {
            parse_bool(v, &s->quick_show_text);
        } else if (KEY("QuickLaunchShowTitle")) {
            parse_bool(v, &s->quick_show_title);
        }
    } else if (SEC("StartMenu")) {
        if (KEY("Style")) {
            int i = index_of(start_style_names, XD_ARRAY_LEN(start_style_names), v);
            if (i >= 0)
                s->start_style = (xd_start_style)i;
        } else if (KEY("LargeIcons")) {
            parse_bool(v, &s->start_large_icons);
        } else if (KEY("Programs")) {
            int64_t n;
            if (xd_str_parse_i64(v, 0, XD_START_PROGRAMS_MAX, &n))
                s->start_programs = (int)n;
        } else if (KEY("HighlightNewPrograms")) {
            parse_bool(v, &s->start_highlight_new);
        }
    } else if (SEC("ScreenSaver")) {
        if (KEY("Name")) {
            copy_into(s->screensaver, sizeof(s->screensaver), v);
        } else if (KEY("Wait")) {
            int64_t m;
            if (xd_str_parse_i64(v, 1, 600, &m))
                s->screensaver_wait = (int)m;
        } else if (KEY("Password")) {
            parse_bool(v, &s->screensaver_password);
        } else if (KEY("MonitorOff")) {
            int64_t m;
            if (xd_str_parse_i64(v, 0, 1440, &m))
                s->monitor_off = (int)m;
        }
    } else if (xd_str_eq_nocase(xd_str_sub(section, 0, 12), XD_STR("ScreenSaver."))) {
        parse_saver_entry(&s->savers, section, key, v);
    } else if (SEC("Notifications")) {
        int i = index_of(tray_rule_names, XD_ARRAY_LEN(tray_rule_names), v);
        if (i > 0 && key.len > 0 && key.len < sizeof(s->tray_rules[0].id)) {
            char id[sizeof(s->tray_rules[0].id)];
            copy_into(id, sizeof(id), key);
            xd_settings_set_tray_rule(s, id, (xd_tray_rule)i);
        }
    } else if (SEC("Keyboard")) {
        xd_keyboard_settings *k = &s->keyboard;
        int64_t n;
        if (KEY("Layout"))
            copy_into(k->layout, sizeof(k->layout), v);
        else if (KEY("Variant"))
            copy_into(k->variant, sizeof(k->variant), v);
        else if (KEY("Options"))
            copy_into(k->options, sizeof(k->options), v);
        else if (KEY("Model"))
            copy_into(k->model, sizeof(k->model), v);
        else if (KEY("RepeatDelay") && xd_str_parse_i64(v, 100, 2000, &n))
            k->repeat_delay = (int)n;
        else if (KEY("RepeatRate") && xd_str_parse_i64(v, 0, 100, &n))
            k->repeat_rate = (int)n;
        else if (KEY("CursorBlinkMs") && xd_str_parse_i64(v, 0, 2000, &n))
            k->cursor_blink_ms = (int)n;
    } else if (SEC("Mouse")) {
        parse_mouse(&s->mouse, key, v);
    } else if (SEC("Accessibility")) {
        parse_access(&s->access, key, v);
    } else if (SEC("Sound")) {
        if (KEY("VolumeIcon"))
            parse_bool(v, &s->volume_icon);
    } else if (SEC("Bluetooth")) {
        if (KEY("ShowIcon"))
            parse_bool(v, &s->bluetooth_icon);
    } else if (SEC("Power")) {
        int64_t m;
        if (KEY("Scheme")) {
            int i = index_of(power_scheme_names, XD_ARRAY_LEN(power_scheme_names), v);
            if (i >= 0)
                s->power_scheme = (xd_power_scheme)i;
        } else if (KEY("Standby") && xd_str_parse_i64(v, 0, 1440, &m)) {
            s->standby = (int)m;
        } else if (KEY("Hibernate") && xd_str_parse_i64(v, 0, 1440, &m)) {
            s->hibernate = (int)m;
        } else if (KEY("MonitorOffOnBattery")) {
            parse_int(v, 0, 1440, &s->monitor_off_battery);
        } else if (KEY("StandbyOnBattery")) {
            parse_int(v, 0, 1440, &s->standby_battery);
        } else if (KEY("HibernateOnBattery")) {
            parse_int(v, 0, 1440, &s->hibernate_battery);
        } else if (KEY("CustomScheme")) {
            copy_into(s->power_scheme_custom, sizeof(s->power_scheme_custom), v);
        } else if (KEY("AlwaysShowIcon")) {
            parse_bool(v, &s->battery_icon);
        } else if (KEY("PasswordOnResume")) {
            parse_bool(v, &s->resume_password);
        } else if (key.len >= 8 && xd_str_eq_nocase(xd_str_sub(key, 0, 8), XD_STR("LowAlarm"))) {
            parse_alarm(&s->low_alarm, xd_str_sub(key, 8, key.len), v);
        } else if (key.len >= 13 &&
                   xd_str_eq_nocase(xd_str_sub(key, 0, 13), XD_STR("CriticalAlarm"))) {
            parse_alarm(&s->critical_alarm, xd_str_sub(key, 13, key.len), v);
        }
    } else if (SEC("Folders")) {
        xd_folder_settings *f = &s->folders;
        if (KEY("NewWindow"))
            parse_bool(v, &f->new_window);
        else if (KEY("SingleClick"))
            parse_bool(v, &f->single_click);
        else if (KEY("ShowHidden"))
            parse_bool(v, &f->show_hidden);
        else if (KEY("HideExtensions"))
            parse_bool(v, &f->hide_extensions);
        else if (KEY("FullPathAddress"))
            parse_bool(v, &f->full_path_address);
        else if (KEY("FullPathTitle"))
            parse_bool(v, &f->full_path_title);
        else if (KEY("RememberViews"))
            parse_bool(v, &f->remember_views);
        else if (KEY("DefaultView")) {
            int i = index_of(view_names, XD_ARRAY_LEN(view_names), v);
            if (i >= 0 || v.len == 0)
                copy_into(f->default_view, sizeof(f->default_view),
                          i >= 0 ? xd_str_from_cstr(view_names[i]) : v);
        }
    }
#undef SEC
#undef KEY
    return true; /* unknown keys are ignored */
}

/* Feeds every key of a key file to `fn`, group by group, in file order. */
static bool keyfile_foreach(GKeyFile *kf,
                            bool (*fn)(void *user, xd_str group, xd_str key, xd_str value),
                            void *user) {
    gsize ngroups = 0;
    gchar **groups = g_key_file_get_groups(kf, &ngroups);
    bool ok = true;
    for (gsize g = 0; ok && g < ngroups; g++) {
        gsize nkeys = 0;
        gchar **keys = g_key_file_get_keys(kf, groups[g], &nkeys, nullptr);
        for (gsize k = 0; ok && k < nkeys; k++) {
            /* Unescaped string; the raw value if it has a bad escape. */
            gchar *v = g_key_file_get_string(kf, groups[g], keys[k], nullptr);
            if (v == nullptr)
                v = g_key_file_get_value(kf, groups[g], keys[k], nullptr);
            ok = fn(user, xd_str_from_cstr(groups[g]), xd_str_from_cstr(keys[k]),
                    xd_str_from_cstr(v));
            g_free(v);
        }
        g_strfreev(keys);
    }
    g_strfreev(groups);
    return ok;
}

void xd_settings_parse(xd_str text, xd_settings *s) {
    xd_settings_defaults(s);
    xd_settings tmp = *s;
    GKeyFile *kf = g_key_file_new();
    /* A syntax error leaves the defaults rather than a half-read file. */
    if (g_key_file_load_from_data(kf, text.ptr, text.len, G_KEY_FILE_NONE, nullptr) &&
        keyfile_foreach(kf, on_entry, &tmp))
        *s = tmp;
    g_key_file_free(kf);
}

void xd_settings_path(xd_strbuf *out) {
    xd_strbuf_clear(out);
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (cfg != nullptr && cfg[0] == '/')
        xd_strbuf_appendf(out, "%s/xeondesktop/desktop.ini", cfg);
    else
        xd_strbuf_appendf(out, "%s/.config/xeondesktop/desktop.ini",
                          home != nullptr ? home : "/tmp");
}

void xd_settings_load(xd_settings *s) {
    xd_strbuf path = {0};
    xd_strbuf text = {0};
    xd_settings_path(&path);
    if (xd_read_file(xd_strbuf_cstr(&path), 1u << 20, &text))
        xd_settings_parse(xd_strbuf_view(&text), s);
    else
        xd_settings_defaults(s);
    xd_strbuf_free(&text);
    xd_strbuf_free(&path);
}

static void set_custom_key(void *user, const char *key, const char *value) {
    g_key_file_set_string(user, "CustomScheme", key, value);
}

static GKeyFile *to_keyfile(const xd_settings *s) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Appearance", "Style", xd_visual_style_name(s->style));
    g_key_file_set_string(kf, "Appearance", "Scheme", s->scheme);
    g_key_file_set_string(kf, "Appearance", "FontSize", font_size_names[s->font_size]);
    g_key_file_set_string(kf, "Appearance", "FontSmoothing",
                          smoothing_names[s->font_smoothing.antialias]);
    g_key_file_set_string(kf, "Appearance", "FontSubpixelOrder",
                          subpixel_names[s->font_smoothing.order]);
    g_key_file_set_string(kf, "Appearance", "FontHinting",
                          hinting_names[s->font_smoothing.hinting]);
    char glass[16];
    snprintf(glass, sizeof(glass), "%08X", (unsigned)s->glass_color);
    g_key_file_set_string(kf, "Appearance", "GlassColor", glass);
    g_key_file_set_boolean(kf, "Appearance", "GlassTransparency", s->glass_transparency);
    g_key_file_set_string(kf, "Appearance", "IconTheme", s->icon_theme);
    g_key_file_set_string(kf, "Appearance", "GtkTheme", s->gtk_theme);
    g_key_file_set_string(kf, "Appearance", "QtStyle", s->qt_style);
    g_key_file_set_comment(kf, "Appearance", nullptr,
                           " XeonDesktop settings. Written by Display Properties; edits are\n"
                           " picked up immediately.",
                           nullptr);
    xd_scheme_custom_foreach(&s->custom_scheme, set_custom_key, kf);
    g_key_file_set_string(kf, "Desktop", "Wallpaper", s->wallpaper);
    g_key_file_set_string(kf, "Desktop", "WallpaperMode", mode_names[s->wallpaper_mode]);
    const xd_desktop_icon_settings *di = &s->desktop_icons;
    g_key_file_set_boolean(kf, "DesktopIcons", "MyDocuments", di->documents);
    g_key_file_set_boolean(kf, "DesktopIcons", "MyComputer", di->computer);
    g_key_file_set_boolean(kf, "DesktopIcons", "MyNetworkPlaces", di->network);
    g_key_file_set_boolean(kf, "DesktopIcons", "WebBrowser", di->browser);
    g_key_file_set_boolean(kf, "DesktopIcons", "Show", di->show);
    g_key_file_set_boolean(kf, "DesktopIcons", "AutoArrange", di->auto_arrange);
    g_key_file_set_boolean(kf, "DesktopIcons", "AlignToGrid", di->align_to_grid);
    g_key_file_set_string(kf, "DesktopIcons", "Size", desk_icon_size_names[di->size]);
    for (size_t i = 0; i < XD_DESK_ICON_COUNT; i++)
        g_key_file_set_string(kf, "DesktopIcons", desk_icon_keys[i], di->icons[i]);
    g_key_file_set_boolean(kf, "Effects", "MenuTransitions", s->menu_transitions);
    g_key_file_set_string(kf, "Effects", "MenuTransition", transition_names[s->menu_transition]);
    g_key_file_set_boolean(kf, "Effects", "FullWindowDrag", s->full_window_drag);
    g_key_file_set_boolean(kf, "Effects", "AnimateWindows", s->animate_windows);
    g_key_file_set_boolean(kf, "Effects", "LargeIcons", s->large_icons);
    g_key_file_set_boolean(kf, "Effects", "HideKeyboardCues", s->hide_keyboard_cues);
    g_key_file_set_boolean(kf, "Windows", "AlwaysDrawFrames", s->always_frames);
    g_key_file_set_boolean(kf, "Taskbar", "QuickLaunch", s->quick_launch);
    g_key_file_set_boolean(kf, "Taskbar", "Locked", s->locked);
    g_key_file_set_boolean(kf, "Taskbar", "AutoHide", s->auto_hide);
    g_key_file_set_boolean(kf, "Taskbar", "OnTop", s->on_top);
    g_key_file_set_boolean(kf, "Taskbar", "GroupButtons", s->group);
    g_key_file_set_string(kf, "Taskbar", "Buttons", buttons_names[s->buttons]);
    g_key_file_set_boolean(kf, "Taskbar", "ShowClock", s->show_clock);
    g_key_file_set_boolean(kf, "Taskbar", "HideInactiveIcons", s->hide_inactive);
    g_key_file_set_boolean(kf, "Taskbar", "AllDisplays", s->all_displays);
    g_key_file_set_string(kf, "Taskbar", "Edge", edge_names[s->edge]);
    g_key_file_set_integer(kf, "Taskbar", "Rows", s->rows);
    g_key_file_set_integer(kf, "Taskbar", "SideWidth", s->side_width);
    g_key_file_set_boolean(kf, "Taskbar", "QuickLaunchLargeIcons", s->quick_large);
    g_key_file_set_boolean(kf, "Taskbar", "QuickLaunchShowText", s->quick_show_text);
    g_key_file_set_boolean(kf, "Taskbar", "QuickLaunchShowTitle", s->quick_show_title);
    g_key_file_set_string(kf, "StartMenu", "Style", start_style_names[s->start_style]);
    g_key_file_set_boolean(kf, "StartMenu", "LargeIcons", s->start_large_icons);
    g_key_file_set_integer(kf, "StartMenu", "Programs", s->start_programs);
    g_key_file_set_boolean(kf, "StartMenu", "HighlightNewPrograms", s->start_highlight_new);
    g_key_file_set_string(kf, "ScreenSaver", "Name", s->screensaver);
    g_key_file_set_integer(kf, "ScreenSaver", "Wait", s->screensaver_wait);
    g_key_file_set_boolean(kf, "ScreenSaver", "Password", s->screensaver_password);
    g_key_file_set_integer(kf, "ScreenSaver", "MonitorOff", s->monitor_off);
    saver_settings_to_keyfile(kf, &s->savers);
    g_key_file_set_string(kf, "Keyboard", "Layout", s->keyboard.layout);
    g_key_file_set_string(kf, "Keyboard", "Variant", s->keyboard.variant);
    g_key_file_set_string(kf, "Keyboard", "Options", s->keyboard.options);
    g_key_file_set_string(kf, "Keyboard", "Model", s->keyboard.model);
    g_key_file_set_integer(kf, "Keyboard", "RepeatDelay", s->keyboard.repeat_delay);
    g_key_file_set_integer(kf, "Keyboard", "RepeatRate", s->keyboard.repeat_rate);
    g_key_file_set_boolean(kf, "Sound", "VolumeIcon", s->volume_icon);
    g_key_file_set_string(kf, "Power", "Scheme", power_scheme_names[s->power_scheme]);
    g_key_file_set_integer(kf, "Power", "Standby", s->standby);
    g_key_file_set_integer(kf, "Power", "Hibernate", s->hibernate);
    g_key_file_set_integer(kf, "Power", "MonitorOffOnBattery", s->monitor_off_battery);
    g_key_file_set_integer(kf, "Power", "StandbyOnBattery", s->standby_battery);
    g_key_file_set_integer(kf, "Power", "HibernateOnBattery", s->hibernate_battery);
    g_key_file_set_string(kf, "Power", "CustomScheme", s->power_scheme_custom);
    g_key_file_set_boolean(kf, "Power", "AlwaysShowIcon", s->battery_icon);
    g_key_file_set_boolean(kf, "Power", "PasswordOnResume", s->resume_password);
    const struct {
        const char *prefix;
        const xd_battery_alarm *alarm;
    } alarms[] = {{"LowAlarm", &s->low_alarm}, {"CriticalAlarm", &s->critical_alarm}};
    for (size_t i = 0; i < XD_ARRAY_LEN(alarms); i++) {
        const xd_battery_alarm *al = alarms[i].alarm;
        char key[32];
        g_key_file_set_boolean(kf, "Power", alarms[i].prefix, al->on);
        g_snprintf(key, sizeof(key), "%sLevel", alarms[i].prefix);
        g_key_file_set_integer(kf, "Power", key, al->percent);
        g_snprintf(key, sizeof(key), "%sSound", alarms[i].prefix);
        g_key_file_set_boolean(kf, "Power", key, al->sound);
        g_snprintf(key, sizeof(key), "%sMessage", alarms[i].prefix);
        g_key_file_set_boolean(kf, "Power", key, al->message);
        g_snprintf(key, sizeof(key), "%sAction", alarms[i].prefix);
        g_key_file_set_string(kf, "Power", key, alarm_action_names[al->action]);
    }
    g_key_file_set_boolean(kf, "Bluetooth", "ShowIcon", s->bluetooth_icon);
    const xd_folder_settings *f = &s->folders;
    g_key_file_set_boolean(kf, "Folders", "NewWindow", f->new_window);
    g_key_file_set_boolean(kf, "Folders", "SingleClick", f->single_click);
    g_key_file_set_boolean(kf, "Folders", "ShowHidden", f->show_hidden);
    g_key_file_set_boolean(kf, "Folders", "HideExtensions", f->hide_extensions);
    g_key_file_set_boolean(kf, "Folders", "FullPathAddress", f->full_path_address);
    g_key_file_set_boolean(kf, "Folders", "FullPathTitle", f->full_path_title);
    g_key_file_set_boolean(kf, "Folders", "RememberViews", f->remember_views);
    g_key_file_set_string(kf, "Folders", "DefaultView", f->default_view);
    g_key_file_set_integer(kf, "Keyboard", "CursorBlinkMs", s->keyboard.cursor_blink_ms);
    const xd_mouse_settings *m = &s->mouse;
    g_key_file_set_boolean(kf, "Mouse", "LeftHanded", m->left_handed);
    g_key_file_set_integer(kf, "Mouse", "Speed", m->speed);
    g_key_file_set_boolean(kf, "Mouse", "Acceleration", m->acceleration);
    g_key_file_set_integer(kf, "Mouse", "DoubleClickMs", m->double_click_ms);
    g_key_file_set_integer(kf, "Mouse", "WheelLines", m->wheel_lines);
    g_key_file_set_string(kf, "Mouse", "CursorTheme", m->cursor_theme);
    g_key_file_set_integer(kf, "Mouse", "CursorSize", m->cursor_size);
    g_key_file_set_boolean(kf, "Mouse", "ClickLock", m->click_lock);
    g_key_file_set_integer(kf, "Mouse", "ClickLockMs", m->click_lock_ms);
    const xd_access_settings *a = &s->access;
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeys", a->sticky_keys);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysLock", a->sticky_lock);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysShortcut", a->sticky_shortcut);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysTwoKeysOff", a->sticky_two_keys_off);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysSound", a->sticky_sound);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeys", a->filter_keys);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeysShortcut", a->filter_shortcut);
    g_key_file_set_boolean(kf, "Accessibility", "SlowKeys", a->slow_keys);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeysBeep", a->filter_beep);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeysNoRepeat", a->filter_no_repeat);
    g_key_file_set_integer(kf, "Accessibility", "BounceMs", a->bounce_ms);
    g_key_file_set_integer(kf, "Accessibility", "SlowKeysMs", a->slow_ms);
    g_key_file_set_integer(kf, "Accessibility", "FilterKeysRepeatDelay", a->filter_repeat_delay);
    g_key_file_set_integer(kf, "Accessibility", "FilterKeysRepeatMs", a->filter_repeat_ms);
    g_key_file_set_boolean(kf, "Accessibility", "ToggleKeys", a->toggle_keys);
    g_key_file_set_boolean(kf, "Accessibility", "ToggleKeysShortcut", a->toggle_shortcut);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeys", a->mouse_keys);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeysShortcut", a->mouse_keys_shortcut);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeysModifiers", a->mouse_keys_modifiers);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeysNumLock", a->mouse_keys_numlock);
    g_key_file_set_integer(kf, "Accessibility", "MouseKeysSpeed", a->mouse_keys_speed);
    g_key_file_set_integer(kf, "Accessibility", "MouseKeysAccelMs", a->mouse_keys_accel_ms);
    g_key_file_set_boolean(kf, "Accessibility", "WarnOnShortcut", a->warn);
    g_key_file_set_boolean(kf, "Accessibility", "SoundOnOff", a->sound_on_off);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysIndicator", a->sticky_indicator);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeysIndicator", a->filter_indicator);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeysIndicator", a->mouse_keys_indicator);
    g_key_file_set_boolean(kf, "Accessibility", "SoundSentry", a->sound_sentry);
    g_key_file_set_string(kf, "Accessibility", "SoundSentryEffect",
                          sentry_names[a->sound_sentry_effect]);
    g_key_file_set_boolean(kf, "Accessibility", "ShowSounds", a->show_sounds);
    g_key_file_set_boolean(kf, "Accessibility", "AutoReset", a->auto_reset);
    g_key_file_set_integer(kf, "Accessibility", "AutoResetMinutes", a->auto_reset_minutes);
    g_key_file_set_boolean(kf, "Accessibility", "HighContrast", a->high_contrast);
    g_key_file_set_string(kf, "Accessibility", "SavedStyle", xd_visual_style_name(a->saved_style));
    g_key_file_set_string(kf, "Accessibility", "SavedScheme", a->saved_scheme);
    for (size_t i = 0; i < s->tray_rule_count; i++)
        g_key_file_set_string(kf, "Notifications", s->tray_rules[i].id,
                              tray_rule_names[s->tray_rules[i].rule]);
    return kf;
}

void xd_settings_serialize(const xd_settings *s, xd_strbuf *out) {
    GKeyFile *kf = to_keyfile(s);
    gsize len = 0;
    gchar *data = g_key_file_to_data(kf, &len, nullptr);
    xd_strbuf_append(out, (xd_str){data, len});
    g_free(data);
    g_key_file_free(kf);
}

bool xd_settings_equal(const xd_settings *a, const xd_settings *b) {
    xd_strbuf x = {0};
    xd_strbuf y = {0};
    xd_settings_serialize(a, &x);
    xd_settings_serialize(b, &y);
    bool eq = xd_str_eq(xd_strbuf_view(&x), xd_strbuf_view(&y));
    xd_strbuf_free(&x);
    xd_strbuf_free(&y);
    return eq;
}

void xd_settings_merge(const xd_settings *base, const xd_settings *pending, xd_settings *now) {
    GKeyFile *b = to_keyfile(base);
    GKeyFile *p = to_keyfile(pending);
    GKeyFile *n = to_keyfile(now);
    /* Keys `pending` set or changed... */
    gchar **groups = g_key_file_get_groups(p, nullptr);
    for (gchar **g = groups; *g != nullptr; g++) {
        gchar **keys = g_key_file_get_keys(p, *g, nullptr, nullptr);
        for (gchar **k = keys; keys != nullptr && *k != nullptr; k++) {
            gchar *pv = g_key_file_get_value(p, *g, *k, nullptr);
            gchar *bv = g_key_file_get_value(b, *g, *k, nullptr);
            if (pv != nullptr && g_strcmp0(pv, bv) != 0)
                g_key_file_set_value(n, *g, *k, pv);
            g_free(pv);
            g_free(bv);
        }
        g_strfreev(keys);
    }
    g_strfreev(groups);
    /* ...and the ones it removed (notification rules back to the default). */
    groups = g_key_file_get_groups(b, nullptr);
    for (gchar **g = groups; *g != nullptr; g++) {
        gchar **keys = g_key_file_get_keys(b, *g, nullptr, nullptr);
        for (gchar **k = keys; keys != nullptr && *k != nullptr; k++) {
            if (!g_key_file_has_key(p, *g, *k, nullptr))
                g_key_file_remove_key(n, *g, *k, nullptr);
        }
        g_strfreev(keys);
    }
    g_strfreev(groups);
    gsize len = 0;
    gchar *data = g_key_file_to_data(n, &len, nullptr);
    xd_settings_parse((xd_str){data, len}, now);
    g_free(data);
    g_key_file_free(n);
    g_key_file_free(p);
    g_key_file_free(b);
}

static void mkdirs_for(xd_strbuf *path) {
    for (size_t i = 1; i < path->len; i++) {
        if (path->data[i] != '/')
            continue;
        path->data[i] = '\0';
        mkdir(path->data, 0700);
        path->data[i] = '/';
    }
}

bool xd_settings_save(const xd_settings *s) {
    xd_strbuf path = {0};
    xd_settings_path(&path);
    mkdirs_for(&path);
    /* g_key_file_save_to_file writes a temporary file and renames it, so
     * watchers never see a partial file. */
    GKeyFile *kf = to_keyfile(s);
    GError *err = nullptr;
    bool ok = g_key_file_save_to_file(kf, xd_strbuf_cstr(&path), &err);
    if (!ok) {
        fprintf(stderr, "xeondesktop: cannot save settings: %s\n",
                err != nullptr ? err->message : "unknown error");
        g_clear_error(&err);
    }
    g_key_file_free(kf);
    xd_strbuf_free(&path);
    return ok;
}

/* ---- data files ---------------------------------------------------------------- */

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool xd_data_dir(xd_strbuf *out) {
    xd_strbuf_clear(out);
    const char *env = getenv("XD_DATA_DIR");
    if (env != nullptr && env[0] == '/') {
        xd_strbuf_append(out, xd_str_from_cstr(env));
        return is_dir(xd_strbuf_cstr(out));
    }
    /* Build tree: <src>/build/xd-foo -> <src>/data */
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        char *slash = strrchr(self, '/');
        if (slash != nullptr) {
            *slash = '\0';
            xd_strbuf_appendf(out, "%s/../data", self);
            if (is_dir(xd_strbuf_cstr(out)))
                return true;
            xd_strbuf_clear(out);
            xd_strbuf_appendf(out, "%s/../share/xeondesktop", self);
            if (is_dir(xd_strbuf_cstr(out)))
                return true;
        }
    }
    static const char *const prefixes[] = {"/usr/local/share/xeondesktop",
                                           "/usr/share/xeondesktop"};
    for (size_t i = 0; i < XD_ARRAY_LEN(prefixes); i++) {
        xd_strbuf_clear(out);
        xd_strbuf_append(out, xd_str_from_cstr(prefixes[i]));
        if (is_dir(prefixes[i]))
            return true;
    }
    return false;
}

static void scheme_file(const char *stem, xd_strbuf *out) {
    xd_strbuf_clear(out);
    xd_strbuf dir = {0};
    if (xd_data_dir(&dir))
        xd_strbuf_appendf(out, "%s/schemes/%s.ini", xd_strbuf_cstr(&dir), stem);
    xd_strbuf_free(&dir);
}

static int compare_names(const void *a, const void *b2) {
    return strcoll(*(char *const *)a, *(char *const *)b2);
}

void xd_scheme_list_load(xd_scheme_list *list) {
    *list = (xd_scheme_list){0};
    xd_strbuf dir = {0};
    if (!xd_data_dir(&dir)) {
        xd_strbuf_free(&dir);
        return;
    }
    xd_strbuf_append(&dir, XD_STR("/schemes"));
    DIR *d = opendir(xd_strbuf_cstr(&dir));
    xd_strbuf_free(&dir);
    if (d == nullptr)
        return;

    /* Collect "Name\tstem" pairs, sort by name, then split. */
    char **pairs = nullptr;
    size_t n = 0;
    size_t cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        xd_str name = xd_str_from_cstr(de->d_name);
        if (name.len < 5 || !xd_str_eq(xd_str_sub(name, name.len - 4, 4), XD_STR(".ini")))
            continue;
        xd_str stem = xd_str_sub(name, 0, name.len - 4);
        char *stem_c = xd_str_dup_cstr(stem);
        xd_strbuf path = {0};
        scheme_file(stem_c, &path);
        xd_scheme scheme;
        bool ok = xd_scheme_load(xd_strbuf_cstr(&path), &scheme, nullptr);
        xd_strbuf_free(&path);
        if (ok) {
            if (n == cap) {
                cap = cap == 0 ? 16 : cap * 2;
                pairs = xd_realloc_array(pairs, cap, sizeof(*pairs));
            }
            xd_strbuf pair = {0};
            xd_strbuf_appendf(&pair, "%s\t%s", scheme.name, stem_c);
            pairs[n++] = xd_str_dup_cstr(xd_strbuf_view(&pair));
            xd_strbuf_free(&pair);
        }
        xd_free(stem_c);
    }
    closedir(d);
    if (n > 1)
        qsort(pairs, n, sizeof(*pairs), compare_names);

    list->stems = xd_alloc_array(XD_MAX(n, 1), sizeof(char *));
    list->names = xd_alloc_array(XD_MAX(n, 1), sizeof(char *));
    for (size_t i = 0; i < n; i++) {
        xd_str p = xd_str_from_cstr(pairs[i]);
        size_t tab = 0;
        XD_ASSERT(xd_str_find_char(p, '\t', &tab));
        list->names[i] = xd_str_dup_cstr(xd_str_sub(p, 0, tab));
        list->stems[i] = xd_str_dup_cstr(xd_str_sub(p, tab + 1, p.len));
        xd_free(pairs[i]);
    }
    xd_free(pairs);
    list->count = n;
}

void xd_scheme_list_free(xd_scheme_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        xd_free(list->stems[i]);
        xd_free(list->names[i]);
    }
    xd_free(list->stems);
    xd_free(list->names);
    *list = (xd_scheme_list){0};
}

void xd_settings_scheme(const xd_settings *s, xd_scheme *out) {
    xd_scheme_init_default(out);
    if (s->scheme[0] != '\0') {
        xd_strbuf path = {0};
        scheme_file(s->scheme, &path);
        if (path.len == 0 || !xd_scheme_load(xd_strbuf_cstr(&path), out, nullptr))
            xd_scheme_init_default(out);
        xd_strbuf_free(&path);
    }
    /* Large / Extra Large: Windows' (large) schemes scale fonts and the
     * caption and menu metrics together. */
    static const double factors[] = {1.0, 1.25, 1.5};
    double f = factors[s->font_size];
    if (f != 1.0) {
        for (size_t i = 0; i < XD_FONT_COUNT; i++)
            out->fonts[i].size_pt *= f;
        static const xd_metric scaled[] = {
            XD_METRIC_CAPTION_HEIGHT, XD_METRIC_CAPTION_BUTTON_WIDTH, XD_METRIC_MENU_HEIGHT,
            XD_METRIC_TASKBAR_HEIGHT, XD_METRIC_SM_CAPTION_HEIGHT};
        for (size_t i = 0; i < XD_ARRAY_LEN(scaled); i++)
            out->metrics[scaled[i]] = (int)lround(out->metrics[scaled[i]] * f);
    }
    /* Advanced Appearance's changes, as they were made: over the font
     * size. High Contrast's scheme goes without them (they come back when
     * it's turned off). */
    if (!s->access.high_contrast)
        xd_scheme_apply_custom(out, &s->custom_scheme);
    out->font_smoothing = s->font_smoothing;
    out->glass_color = s->glass_color;
    out->glass_opaque = !s->glass_transparency;
}

void xd_settings_set_high_contrast(xd_settings *s, bool on, const char *scheme) {
    xd_access_settings *a = &s->access;
    if (on) {
        if (!a->high_contrast) {
            a->saved_style = s->style;
            memcpy(a->saved_scheme, s->scheme, sizeof(a->saved_scheme));
        }
        s->style = XD_STYLE_CLASSIC;
        parse_stem(xd_str_from_cstr(scheme), s->scheme, sizeof(s->scheme));
    } else if (a->high_contrast) {
        s->style = a->saved_style;
        memcpy(s->scheme, a->saved_scheme, sizeof(s->scheme));
        a->saved_style = XD_STYLE_CLASSIC;
        a->saved_scheme[0] = '\0';
    }
    a->high_contrast = on;
}

/* ---- watching ------------------------------------------------------------------ */

int xd_settings_watch(void) {
    xd_strbuf path = {0};
    xd_settings_path(&path);
    mkdirs_for(&path);
    /* Watch the directory: an atomic save replaces the file, which a
     * watch on the file itself would lose. */
    char *slash = strrchr(path.data, '/');
    if (slash != nullptr)
        *slash = '\0';
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0 && inotify_add_watch(fd, path.data, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        close(fd);
        fd = -1;
    }
    xd_strbuf_free(&path);
    return fd;
}

bool xd_settings_watch_changed(int fd) {
    bool changed = false;
    alignas(struct inotify_event) char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        size_t off = 0;
        while (off + sizeof(struct inotify_event) <= (size_t)n) {
            struct inotify_event ev;
            memcpy(&ev, buf + off, sizeof(ev));
            size_t total = sizeof(ev) + ev.len;
            if (total > (size_t)n - off)
                break;
            if (ev.len > 0) {
                /* The name is NUL-padded within ev.len bytes. */
                xd_str name = {buf + off + sizeof(ev), strnlen(buf + off + sizeof(ev), ev.len)};
                if (xd_str_eq(name, XD_STR("desktop.ini")))
                    changed = true;
            }
            off += total;
        }
    }
    return changed;
}
