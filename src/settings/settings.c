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
static const char *const layout_names[] = {"auto", "unified", "tasks"};
static const char *const taskbar_icons_names[] = {"auto", "large", "small"};
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
static const char *const updates_mode_names[XD_UPDATES_MODE_COUNT] = {
    [XD_UPDATES_DOWNLOAD] = "download",
    [XD_UPDATES_NOTIFY] = "notify",
    [XD_UPDATES_OFF] = "off",
};
/* [Updates] Day: XP's "Every day", "Every Sunday", ... */
static const char *const updates_day_names[] = {"daily",     "sunday",   "monday", "tuesday",
                                                "wednesday", "thursday", "friday", "saturday"};
static const char *const button_action_names[XD_BUTTON_ACTION_COUNT] = {
    [XD_BUTTON_LOGIND] = "logind",       [XD_BUTTON_NOTHING] = "nothing",
    [XD_BUTTON_ASK] = "ask",             [XD_BUTTON_STANDBY] = "standby",
    [XD_BUTTON_HIBERNATE] = "hibernate", [XD_BUTTON_SHUTDOWN] = "shutdown",
    [XD_BUTTON_LOCK] = "lock",
};
static int index_of(const char *const *names, size_t count, xd_str value);
static const char *const button_keys[XD_POWER_BUTTON_COUNT] = {
    [XD_BUTTON_LID] = "LidAction",
    [XD_BUTTON_POWER] = "PowerButtonAction",
    [XD_BUTTON_SLEEP] = "SleepButtonAction",
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

xd_tray_rule xd_settings_tray_rule(const xd_settings *settings, const char *id) {
    for (size_t i = 0; id != nullptr && i < settings->tray_rule_count; i++) {
        if (strcmp(settings->tray_rules[i].id, id) == 0)
            return settings->tray_rules[i].rule;
    }
    return XD_TRAY_AUTO;
}

void xd_settings_set_tray_rule(xd_settings *settings, const char *id, xd_tray_rule rule) {
    for (size_t i = 0; i < settings->tray_rule_count; i++) {
        if (strcmp(settings->tray_rules[i].id, id) != 0)
            continue;
        if (rule == XD_TRAY_AUTO)
            settings->tray_rules[i] = settings->tray_rules[--settings->tray_rule_count];
        else
            settings->tray_rules[i].rule = rule;
        return;
    }
    if (rule == XD_TRAY_AUTO || settings->tray_rule_count == XD_TRAY_RULES_MAX || id[0] == '\0' ||
        strlen(id) >= sizeof(settings->tray_rules[0].id))
        return;
    xd_tray_behavior *tray_behavior = &settings->tray_rules[settings->tray_rule_count++];
    *tray_behavior = (xd_tray_behavior){.rule = rule};
    memcpy(tray_behavior->id, id, strlen(id) + 1);
}

static const char *const nightlight_schedule_names[XD_NIGHTLIGHT_SCHEDULE_COUNT] = {
    [XD_NIGHTLIGHT_ALWAYS] = "always",
    [XD_NIGHTLIGHT_SUNSET] = "sunset",
    [XD_NIGHTLIGHT_MANUAL] = "manual",
};

const char *xd_nightlight_schedule_name(xd_nightlight_schedule schedule) {
    XD_ASSERT((unsigned)schedule < XD_NIGHTLIGHT_SCHEDULE_COUNT);
    return nightlight_schedule_names[schedule];
}

const char *xd_settings_color_profile(const xd_settings *settings, const char *monitor) {
    for (size_t i = 0; monitor != nullptr && i < settings->color_profile_count; i++) {
        if (strcmp(settings->color_profiles[i].monitor, monitor) == 0)
            return settings->color_profiles[i].path;
    }
    return nullptr;
}

/* A monitor key as a GKeyFile key: no brackets, '=' or control characters,
 * and no space at either end (GKeyFile trims them). */
static bool profile_key_ok(const char *key) {
    size_t count = strlen(key);
    if (count == 0 || count >= sizeof(((xd_color_profile_rule *)nullptr)->monitor) ||
        key[0] == ' ' || key[count - 1] == ' ')
        return false;
    for (size_t i = 0; i < count; i++) {
        if ((unsigned char)key[i] < ' ' || strchr("[]=", key[i]) != nullptr)
            return false;
    }
    return true;
}

static bool profile_path_ok(const char *path) {
    size_t count = strlen(path);
    if (path[0] != '/' || count >= sizeof(((xd_color_profile_rule *)nullptr)->path))
        return false;
    for (size_t i = 0; i < count; i++) {
        if ((unsigned char)path[i] < ' ')
            return false;
    }
    return true;
}

bool xd_settings_set_color_profile(xd_settings *settings, const char *monitor, const char *path) {
    bool remove = path == nullptr || path[0] == '\0';
    for (size_t i = 0; i < settings->color_profile_count; i++) {
        if (strcmp(settings->color_profiles[i].monitor, monitor) != 0)
            continue;
        if (remove) {
            settings->color_profiles[i] = settings->color_profiles[--settings->color_profile_count];
            return true;
        }
        if (!profile_path_ok(path))
            return false;
        snprintf(settings->color_profiles[i].path, sizeof(settings->color_profiles[i].path), "%s",
                 path);
        return true;
    }
    if (remove)
        return true;
    if (settings->color_profile_count == XD_COLOR_PROFILES_MAX || !profile_key_ok(monitor) ||
        !profile_path_ok(path))
        return false;
    xd_color_profile_rule *color_profile_rule =
        &settings->color_profiles[settings->color_profile_count++];
    snprintf(color_profile_rule->monitor, sizeof(color_profile_rule->monitor), "%s", monitor);
    snprintf(color_profile_rule->path, sizeof(color_profile_rule->path), "%s", path);
    return true;
}

const char *xd_wallpaper_mode_name(xd_wallpaper_mode wallpaper_mode) {
    XD_ASSERT((unsigned)wallpaper_mode < XD_WALLPAPER_MODE_COUNT);
    return mode_names[wallpaper_mode];
}

const char *xd_desk_icon_default(xd_desk_icon icon) {
    XD_ASSERT((unsigned)icon < XD_DESK_ICON_COUNT);
    return desk_icon_defaults[icon];
}

int xd_desk_icon_px(const xd_settings *settings) {
    switch (settings->desktop_icons.size) {
    case XD_DESK_ICONS_SMALL:
        return 16;
    case XD_DESK_ICONS_LARGE:
        return 48;
    case XD_DESK_ICONS_MEDIUM:
        break;
    }
    return settings->large_icons ? 48 : 32;
}

const char *xd_settings_icon_theme(const xd_settings *settings) {
    if (settings->icon_theme[0] != '\0')
        return settings->icon_theme;
    const char *env = getenv("XD_ICON_THEME");
    return env != nullptr && env[0] != '\0' ? env : "breeze";
}

const char *xd_settings_gtk_theme(const xd_settings *settings) {
    return settings->gtk_theme[0] != '\0' ? settings->gtk_theme : XD_GTK_THEME_NAME;
}

const char *xd_qt_style_auto(xd_visual_style style) {
    /* Windows draws 2000's raised and sunken bevels; Fusion's flat,
     * rounded and lightly shaded controls are the closest Qt has to XP's
     * and 7's. */
    return style == XD_STYLE_CLASSIC ? "Windows" : "Fusion";
}

bool xd_settings_taskbar_unified(const xd_settings *settings, xd_visual_style style) {
    switch (settings->layout) {
    case XD_TASKBAR_LAYOUT_UNIFIED:
        return true;
    case XD_TASKBAR_LAYOUT_TASKS:
        return false;
    case XD_TASKBAR_LAYOUT_AUTO:
    case XD_TASKBAR_LAYOUT_COUNT:
        break;
    }
    return style == XD_STYLE_AERO;
}

bool xd_settings_taskbar_large_icons(const xd_settings *settings, xd_visual_style style) {
    switch (settings->icons) {
    case XD_TASKBAR_ICONS_LARGE:
        return true;
    case XD_TASKBAR_ICONS_SMALL:
        return false;
    case XD_TASKBAR_ICONS_AUTO:
    case XD_TASKBAR_ICONS_COUNT:
        break;
    }
    return xd_settings_taskbar_unified(settings, style);
}

const char *xd_settings_qt_style(const xd_settings *settings) {
    return settings->qt_style[0] != '\0' ? settings->qt_style : xd_qt_style_auto(settings->style);
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

bool xd_settings_qt_env(const xd_settings *settings) {
    /* All are evaluated: no short circuit. */
    bool theme = qt_env_set("QT_QPA_PLATFORMTHEME", "gtk3");
    bool style = qt_env_set("QT_STYLE_OVERRIDE", xd_settings_qt_style(settings));
    /* Our palette goes with our GTK theme; with another, Qt takes that
     * theme's colours its own way. Empty rather than unset, so that the
     * D-Bus and systemd environments, which can only be given values,
     * follow too. */
    char *palette = settings->gtk_theme[0] == '\0' ? xd_gtk_theme_qt_palette_path() : g_strdup("");
    bool colours = qt_env_set("QT_GUI_GTK_JSON", palette);
    g_free(palette);
    return theme || style || colours;
}

const char *xd_alarm_action_name(xd_alarm_action action) {
    return action < XD_ALARM_ACTION_COUNT ? alarm_action_names[action] : alarm_action_names[0];
}

xd_alarm_fired xd_battery_alarm_check(const xd_settings *settings, xd_alarm_done *done,
                                      double percent, bool discharging) {
    if (!discharging) {
        *done = (xd_alarm_done){0};
        return XD_ALARM_FIRED_NONE;
    }
    const xd_battery_alarm *low = &settings->low_alarm, *critical = &settings->critical_alarm;
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

const char *xd_button_action_name(xd_button_action action) {
    return action < XD_BUTTON_ACTION_COUNT ? button_action_names[action] : button_action_names[0];
}

bool xd_button_action_from_name(const char *name, xd_button_action *out) {
    int i = name != nullptr ? index_of(button_action_names, XD_ARRAY_LEN(button_action_names),
                                       xd_str_from_cstr(name))
                            : -1;
    if (i < 0)
        return false;
    *out = (xd_button_action)i;
    return true;
}

bool xd_button_action_from_logind(const char *handle, xd_button_action *out) {
    static const struct {
        const char *handle;
        xd_button_action action;
    } map[] = {
        {"ignore", XD_BUTTON_NOTHING},     {"suspend", XD_BUTTON_STANDBY},
        {"hibernate", XD_BUTTON_HIBERNATE}, {"poweroff", XD_BUTTON_SHUTDOWN},
        {"lock", XD_BUTTON_LOCK},
    };
    for (size_t i = 0; handle != nullptr && i < XD_ARRAY_LEN(map); i++) {
        if (strcmp(handle, map[i].handle) == 0) {
            *out = map[i].action;
            return true;
        }
    }
    return false;
}

void xd_power_buttons_inhibit_what(const xd_settings *settings, char *out, size_t cap) {
    static const char *const what[XD_POWER_BUTTON_COUNT] = {
        [XD_BUTTON_LID] = "handle-lid-switch",
        [XD_BUTTON_POWER] = "handle-power-key",
        /* XF86Sleep is logind's suspend key, XF86Suspend its hibernate
         * key: both are XP's one sleep button. */
        [XD_BUTTON_SLEEP] = "handle-suspend-key:handle-hibernate-key",
    };
    XD_ASSERT(cap > 0);
    out[0] = '\0';
    for (size_t button = 0; button < XD_POWER_BUTTON_COUNT; button++) {
        if (settings->power_buttons[button] == XD_BUTTON_LOGIND)
            continue;
        if (out[0] != '\0')
            g_strlcat(out, ":", cap);
        g_strlcat(out, what[button], cap);
    }
}

xd_button_action xd_power_button_action(const xd_settings *settings, xd_power_button button,
                                        bool docked) {
    XD_ASSERT((unsigned)button < XD_POWER_BUTTON_COUNT);
    xd_button_action button_action = settings->power_buttons[button];
    if (button_action == XD_BUTTON_LOGIND)
        return XD_BUTTON_NOTHING; /* logind acts */
    if (button == XD_BUTTON_LID && (docked || button_action == XD_BUTTON_ASK))
        return XD_BUTTON_NOTHING;
    return button_action;
}

const char *xd_updates_mode_name(xd_updates_mode mode) {
    XD_ASSERT((unsigned)mode < XD_UPDATES_MODE_COUNT);
    return updates_mode_names[mode];
}

const char *xd_power_scheme_name(xd_power_scheme scheme) {
    XD_ASSERT((unsigned)scheme < XD_POWER_SCHEME_COUNT);
    return power_scheme_names[scheme];
}

void xd_settings_defaults(xd_settings *settings) {
    *settings = (xd_settings){
        .style = XD_STYLE_CLASSIC,
        .glass_color = XD_GLASS_DEFAULT,
        .glass_transparency = true,
        .image_scaling = XD_FILTER_COUNT,
        .integer_scaling = true,
        .wallpaper_mode = XD_WALLPAPER_FILL,
        /* Windows 2000's icons but Internet Explorer; Align to Grid on. */
        .desktop_icons = {.documents = true,
                          .computer = true,
                          .network = true,
                          .show = true,
                          .align_to_grid = true,
                          .size = XD_DESK_ICONS_MEDIUM},
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
        .mouse = {.acceleration = true,
                  .double_click_ms = 500,
                  .wheel_lines = 3,
                  .cursor_size = 24,
                  .click_lock_ms = 1200},
        /* XP's: every shortcut on, and the warnings and sounds with them;
         * FilterKeys' shortcut turns on SlowKeys. */
        .access = {.sticky_lock = true,
                   .sticky_shortcut = true,
                   .sticky_two_keys_off = true,
                   .sticky_sound = true,
                   .filter_shortcut = true,
                   .slow_keys = true,
                   .filter_beep = true,
                   .bounce_ms = 500,
                   .slow_ms = 1000,
                   .filter_repeat_delay = 1000,
                   .filter_repeat_ms = 500,
                   .toggle_shortcut = true,
                   .mouse_keys_shortcut = true,
                   .mouse_keys_modifiers = true,
                   .mouse_keys_numlock = true,
                   .mouse_keys_speed = 80,
                   .mouse_keys_accel_ms = 3000,
                   .warn = true,
                   .sound_on_off = true,
                   .sticky_indicator = true,
                   .filter_indicator = true,
                   .mouse_keys_indicator = true,
                   .sound_sentry_effect = XD_SENTRY_WINDOW,
                   .auto_reset_minutes = XD_AUTO_RESET_MIN},
        .volume_icon = true,
        .battery_icon = true,
        .resume_password = true, /* as in XP */
        /* XP's levels. No action by default: UPower takes its own
         * CriticalPowerAction just below. */
        .low_alarm = {.on = true, .percent = 10, .sound = true, .message = true},
        .critical_alarm = {.on = true, .percent = 3, .sound = true, .message = true},
        .bluetooth_icon = true,
        /* XP SP2's every day at 3:00 AM, downloading but never installing. */
        .updates = {.mode = XD_UPDATES_DOWNLOAD, .day = 0, .hour = 3},
        /* Off, as in Windows 10; turned on, from sunset to sunrise (the
         * set hours until there is a location) at about Windows 10's
         * middle strength. */
        .night_light = {.schedule = XD_NIGHTLIGHT_SUNSET,
                        .temperature = 3400,
                        .on_minute = 21 * 60,
                        .off_minute = 7 * 60,
                        .auto_location = true},
        /* XP's, except extensions, which stay visible: renaming edits the
         * whole name. */
        .folders = {.full_path_address = true, .remember_views = true, .common_tasks = true},
    };
    xd_saver_settings_defaults(&settings->savers);
}

static void copy_into(char *dst, size_t cap, xd_str value) {
    size_t length = XD_MIN(value.len, cap - 1);
    memcpy(dst, value.ptr, length);
    dst[length] = '\0';
}

static int index_of(const char *const *names, size_t count, xd_str value) {
    for (size_t i = 0; i < count; i++) {
        if (xd_str_eq_nocase(value, xd_str_from_cstr(names[i])))
            return (int)i;
    }
    return -1;
}

static void parse_bool(xd_str value, bool *out) {
    if (xd_str_eq_nocase(value, XD_STR("true")) || xd_str_eq(value, XD_STR("1")))
        *out = true;
    else if (xd_str_eq_nocase(value, XD_STR("false")) || xd_str_eq(value, XD_STR("0")))
        *out = false;
}

/* A scheme is a file stem, not a path: safe file-name characters only. */
static void parse_stem(xd_str value, char *dst, size_t cap) {
    bool ok = value.len < cap;
    for (size_t i = 0; ok && i < value.len; i++) {
        char character = value.ptr[i];
        ok = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
             (character >= '0' && character <= '9') || character == '-' || character == '_';
    }
    if (ok)
        copy_into(dst, cap, value);
}

/* A colour as AARRGGBB in hex, as Windows writes ColorizationColor. */
static void parse_argb(xd_str value, uint32_t *out) {
    char buf[9];
    if (value.len != 8)
        return;
    memcpy(buf, value.ptr, 8);
    buf[8] = '\0';
    guint64 parsed;
    if (g_ascii_string_to_unsigned(buf, 16, 0, UINT32_MAX, &parsed, nullptr))
        *out = (uint32_t)parsed;
}

static void parse_int(xd_str value, int64_t lo, int64_t hi, int *out) {
    int64_t parsed;
    if (xd_str_parse_i64(value, lo, hi, &parsed))
        *out = (int)parsed;
}

/* A theme's directory name under icons/ or themes/: no paths, nothing
 * hidden, nothing that needs escaping in an environment variable. GtkTheme
 * may carry GTK_THEME's variant (":dark"). */
static void parse_theme_dir(xd_str value, char *dst, size_t cap) {
    bool ok = value.len < cap && (value.len == 0 || value.ptr[0] != '.');
    for (size_t i = 0; ok && i < value.len; i++)
        ok = value.ptr[i] != '/' && (unsigned char)value.ptr[i] >= ' ';
    if (ok)
        copy_into(dst, cap, value);
}

/* A Qt style's key ("Fusion", "kvantum-dark"): it goes into the
 * environment, so nothing else. */
static void parse_qt_style(xd_str value, char *dst, size_t cap) {
    bool ok = value.len < cap;
    for (size_t i = 0; ok && i < value.len; i++)
        ok = g_ascii_isalnum(value.ptr[i]) || memchr("-_.+", value.ptr[i], 4) != nullptr;
    if (ok)
        copy_into(dst, cap, value);
}

/* ImageScaling: "Automatic" or a filter's name (scale.h). */
static void parse_image_scaling(xd_str value, xd_image_filter *out) {
    if (xd_str_eq_nocase(value, XD_STR("Automatic"))) {
        *out = XD_FILTER_COUNT;
        return;
    }
    char name[32];
    if (value.len >= sizeof(name))
        return;
    copy_into(name, sizeof(name), value);
    xd_image_filter image_filter;
    if (xd_image_filter_from_name(name, &image_filter))
        *out = image_filter;
}

static void parse_mouse(xd_mouse_settings *mouse, xd_str key, xd_str value) {
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (KEY("LeftHanded")) {
        parse_bool(value, &mouse->left_handed);
    } else if (KEY("Speed")) {
        parse_int(value, XD_MOUSE_SPEED_MIN, XD_MOUSE_SPEED_MAX, &mouse->speed);
    } else if (KEY("Acceleration")) {
        parse_bool(value, &mouse->acceleration);
    } else if (KEY("DoubleClickMs")) {
        parse_int(value, XD_DOUBLE_CLICK_MIN, XD_DOUBLE_CLICK_MAX, &mouse->double_click_ms);
    } else if (KEY("WheelLines")) {
        parse_int(value, 1, 30, &mouse->wheel_lines);
    } else if (KEY("CursorTheme")) {
        /* A directory name under icons/: no paths, nothing hidden. */
        if (value.len < sizeof(mouse->cursor_theme) &&
            memchr(value.ptr, '/', value.len) == nullptr && (value.len == 0 || value.ptr[0] != '.'))
            copy_into(mouse->cursor_theme, sizeof(mouse->cursor_theme), value);
    } else if (KEY("CursorSize")) {
        parse_int(value, XD_CURSOR_SIZE_MIN, XD_CURSOR_SIZE_MAX, &mouse->cursor_size);
    } else if (KEY("ClickLock")) {
        parse_bool(value, &mouse->click_lock);
    } else if (KEY("ClickLockMs")) {
        parse_int(value, XD_CLICK_LOCK_MIN, XD_CLICK_LOCK_MAX, &mouse->click_lock_ms);
    }
#undef KEY
}

static void parse_access(xd_access_settings *access_settings, xd_str key, xd_str value) {
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
            parse_bool(value, (bool *)((char *)access_settings + flags[i].offset));
            return;
        }
    }
    if (KEY("BounceMs")) {
        parse_int(value, 100, 2000, &access_settings->bounce_ms);
    } else if (KEY("SlowKeysMs")) {
        parse_int(value, 0, 20000, &access_settings->slow_ms);
    } else if (KEY("FilterKeysRepeatDelay")) {
        parse_int(value, 300, 20000, &access_settings->filter_repeat_delay);
    } else if (KEY("FilterKeysRepeatMs")) {
        parse_int(value, 300, 20000, &access_settings->filter_repeat_ms);
    } else if (KEY("MouseKeysSpeed")) {
        parse_int(value, XD_MOUSE_KEYS_SPEED_MIN, XD_MOUSE_KEYS_SPEED_MAX,
                  &access_settings->mouse_keys_speed);
    } else if (KEY("MouseKeysAccelMs")) {
        parse_int(value, XD_MOUSE_KEYS_ACCEL_MIN, XD_MOUSE_KEYS_ACCEL_MAX,
                  &access_settings->mouse_keys_accel_ms);
    } else if (KEY("SoundSentryEffect")) {
        int i = index_of(sentry_names, XD_ARRAY_LEN(sentry_names), value);
        if (i >= 0)
            access_settings->sound_sentry_effect = (xd_sound_sentry)i;
    } else if (KEY("AutoResetMinutes")) {
        parse_int(value, XD_AUTO_RESET_MIN, XD_AUTO_RESET_MAX,
                  &access_settings->auto_reset_minutes);
    } else if (KEY("HighContrast")) {
        parse_bool(value, &access_settings->high_contrast);
    } else if (KEY("SavedStyle")) {
        xd_visual_style st;
        if (xd_visual_style_from_name(value, &st))
            access_settings->saved_style = st;
    } else if (KEY("SavedScheme")) {
        parse_stem(value, access_settings->saved_scheme, sizeof(access_settings->saved_scheme));
    }
#undef KEY
}

/* [Power] LowAlarm*, CriticalAlarm*: `key` without its prefix. */
static void parse_alarm(xd_battery_alarm *al, xd_str key, xd_str value) {
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (KEY("")) {
        parse_bool(value, &al->on);
    } else if (KEY("Level")) {
        parse_int(value, 0, 100, &al->percent);
    } else if (KEY("Sound")) {
        parse_bool(value, &al->sound);
    } else if (KEY("Message")) {
        parse_bool(value, &al->message);
    } else if (KEY("Action")) {
        int i = index_of(alarm_action_names, XD_ARRAY_LEN(alarm_action_names), value);
        if (i >= 0)
            al->action = (xd_alarm_action)i;
    }
#undef KEY
}

static void parse_desktop_icons(xd_desktop_icon_settings *desktop_icon_settings, xd_str key,
                                xd_str value) {
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (KEY("MyDocuments")) {
        parse_bool(value, &desktop_icon_settings->documents);
    } else if (KEY("MyComputer")) {
        parse_bool(value, &desktop_icon_settings->computer);
    } else if (KEY("MyNetworkPlaces")) {
        parse_bool(value, &desktop_icon_settings->network);
    } else if (KEY("WebBrowser")) {
        parse_bool(value, &desktop_icon_settings->browser);
    } else if (KEY("Show")) {
        parse_bool(value, &desktop_icon_settings->show);
    } else if (KEY("AutoArrange")) {
        parse_bool(value, &desktop_icon_settings->auto_arrange);
    } else if (KEY("AlignToGrid")) {
        parse_bool(value, &desktop_icon_settings->align_to_grid);
    } else if (KEY("Size")) {
        int i = index_of(desk_icon_size_names, XD_ARRAY_LEN(desk_icon_size_names), value);
        if (i >= 0)
            desktop_icon_settings->size = (xd_desk_icon_size)i;
    } else {
        /* Change Icon: an image file, by its absolute path. */
        for (size_t i = 0; i < XD_DESK_ICON_COUNT; i++) {
            if (xd_str_eq_nocase(key, xd_str_from_cstr(desk_icon_keys[i])) &&
                (value.len == 0 ||
                 (value.ptr[0] == '/' && value.len < sizeof(desktop_icon_settings->icons[i]))))
                copy_into(desktop_icon_settings->icons[i], sizeof(desktop_icon_settings->icons[i]),
                          value);
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

void xd_saver_settings_defaults(xd_saver_settings *saver_settings) {
    *saver_settings = (xd_saver_settings){
        .marquee = {.text = "XeonDesktop",
                    .font = {.family = "Times New Roman", .size_pt = 48},
                    .color = {255, 0, 0},
                    .random_position = true,
                    .speed = 5},
        .starfield = {.speed = 5, .density = 200},
        .mystify =
            {.shapes = {{.active = true, .lines = 8, .colors = {{255, 0, 0}, {255, 255, 0}}},
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
static void parse_rgb_value(xd_str value, xd_rgb *out) {
    gchar *text = g_strndup(value.ptr, value.len);
    gchar **parts = g_strsplit_set(g_strstrip(text), " \t", -1);
    int64_t channels[3];
    size_t count = 0;
    bool ok = true;
    for (gchar **part = parts; ok && *part != nullptr; part++) {
        if (**part == '\0')
            continue; /* runs of spaces */
        ok = count < 3 && xd_str_parse_i64(xd_str_from_cstr(*part), 0, 255, &channels[count++]);
    }
    if (ok && count == 3)
        *out = (xd_rgb){(uint8_t)channels[0], (uint8_t)channels[1], (uint8_t)channels[2]};
    g_strfreev(parts);
    g_free(text);
}

static void set_rgb(GKeyFile *kf, const char *group, const char *key, xd_rgb color) {
    char buf[16];
    g_snprintf(buf, sizeof(buf), "%u %u %u", color.r, color.g, color.b);
    g_key_file_set_string(kf, group, key, buf);
}

/* Free text shown on the screen: anything but control characters. */
static void parse_saver_text(xd_str value, char *dst, size_t cap) {
    bool ok = value.len < cap;
    for (size_t i = 0; ok && i < value.len; i++)
        ok = (unsigned char)value.ptr[i] >= ' ';
    if (ok)
        copy_into(dst, cap, value);
}

static void parse_saver_entry(xd_saver_settings *sv, xd_str section, xd_str key, xd_str value) {
#define SEC(n) xd_str_eq_nocase(section, XD_STR("ScreenSaver." n))
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (SEC("Marquee")) {
        xd_marquee_settings *marquee_settings = &sv->marquee;
        if (KEY("Text"))
            parse_saver_text(value, marquee_settings->text, sizeof(marquee_settings->text));
        else if (KEY("Font"))
            parse_saver_text(value, marquee_settings->font.family,
                             sizeof(marquee_settings->font.family));
        else if (KEY("FontSize")) {
            int pt = (int)marquee_settings->font.size_pt;
            parse_int(value, 6, 200, &pt);
            marquee_settings->font.size_pt = pt;
        } else if (KEY("Bold"))
            parse_bool(value, &marquee_settings->font.bold);
        else if (KEY("Italic"))
            parse_bool(value, &marquee_settings->font.italic);
        else if (KEY("TextColor"))
            parse_rgb_value(value, &marquee_settings->color);
        else if (KEY("BackgroundColor"))
            parse_rgb_value(value, &marquee_settings->background);
        else if (KEY("Position")) {
            if (xd_str_eq_nocase(value, XD_STR("random")))
                marquee_settings->random_position = true;
            else if (xd_str_eq_nocase(value, XD_STR("centered")))
                marquee_settings->random_position = false;
        } else if (KEY("Speed"))
            parse_int(value, 0, XD_SAVER_SLIDER_MAX, &marquee_settings->speed);
    } else if (SEC("Starfield")) {
        if (KEY("WarpSpeed"))
            parse_int(value, 0, XD_SAVER_SLIDER_MAX, &sv->starfield.speed);
        else if (KEY("Density"))
            parse_int(value, XD_STARFIELD_DENSITY_MIN, XD_STARFIELD_DENSITY_MAX,
                      &sv->starfield.density);
    } else if (SEC("Mystify")) {
        if (KEY("ClearScreen")) {
            parse_bool(value, &sv->mystify.clear_screen);
            return;
        }
        /* Polygon1Active, Polygon2Lines, ... */
        for (int i = 0; i < XD_MYSTIFY_SHAPES; i++) {
            char prefix[16];
            g_snprintf(prefix, sizeof(prefix), "Polygon%d", i + 1);
            xd_str key_prefix = xd_str_from_cstr(prefix);
            if (key.len <= key_prefix.len ||
                !xd_str_eq_nocase(xd_str_sub(key, 0, key_prefix.len), key_prefix))
                continue;
            xd_str rest = xd_str_sub(key, key_prefix.len, key.len);
            xd_mystify_shape *sh = &sv->mystify.shapes[i];
            if (xd_str_eq_nocase(rest, XD_STR("Active")))
                parse_bool(value, &sh->active);
            else if (xd_str_eq_nocase(rest, XD_STR("Lines")))
                parse_int(value, 1, XD_MYSTIFY_LINES_MAX, &sh->lines);
            else if (xd_str_eq_nocase(rest, XD_STR("Colors"))) {
                if (xd_str_eq_nocase(value, XD_STR("two")))
                    sh->two_colors = true;
                else if (xd_str_eq_nocase(value, XD_STR("random")))
                    sh->two_colors = false;
            } else if (xd_str_eq_nocase(rest, XD_STR("Color1")))
                parse_rgb_value(value, &sh->colors[0]);
            else if (xd_str_eq_nocase(rest, XD_STR("Color2")))
                parse_rgb_value(value, &sh->colors[1]);
        }
    } else if (SEC("Beziers")) {
        if (KEY("Curves"))
            parse_int(value, 1, XD_BEZIERS_CURVES_MAX, &sv->beziers.curves);
        else if (KEY("Lines"))
            parse_int(value, 1, XD_BEZIERS_LINES_MAX, &sv->beziers.lines);
        else if (KEY("Speed"))
            parse_int(value, 0, XD_SAVER_SLIDER_MAX, &sv->beziers.speed);
    } else if (SEC("Flying")) {
        if (KEY("WarpSpeed"))
            parse_int(value, 0, XD_SAVER_SLIDER_MAX, &sv->flying.speed);
        else if (KEY("Density"))
            parse_int(value, XD_FLYING_DENSITY_MIN, XD_FLYING_DENSITY_MAX, &sv->flying.density);
    } else if (SEC("Pipes")) {
        if (KEY("Pipes")) {
            if (xd_str_eq_nocase(value, XD_STR("multiple")))
                sv->pipes.multiple = true;
            else if (xd_str_eq_nocase(value, XD_STR("single")))
                sv->pipes.multiple = false;
        } else if (KEY("Joints")) {
            int i = index_of(pipes_joint_names, XD_ARRAY_LEN(pipes_joint_names), value);
            if (i >= 0)
                sv->pipes.joints = (xd_pipes_joint)i;
        }
    } else if (SEC("Text3D")) {
        xd_text3d_settings *text3d_settings = &sv->text3d;
        if (KEY("Display")) {
            if (xd_str_eq_nocase(value, XD_STR("time")))
                text3d_settings->show_time = true;
            else if (xd_str_eq_nocase(value, XD_STR("text")))
                text3d_settings->show_time = false;
        } else if (KEY("Text"))
            parse_saver_text(value, text3d_settings->text, sizeof(text3d_settings->text));
        else if (KEY("Rotation")) {
            int i = index_of(text3d_rotation_names, XD_ARRAY_LEN(text3d_rotation_names), value);
            if (i >= 0)
                text3d_settings->rotation = (xd_text3d_rotation)i;
        } else if (KEY("Speed"))
            parse_int(value, 0, XD_SAVER_SLIDER_MAX, &text3d_settings->speed);
        else if (KEY("Size"))
            parse_int(value, 0, XD_SAVER_SLIDER_MAX, &text3d_settings->size);
        else if (KEY("Surface")) {
            int i = index_of(text3d_surface_names, XD_ARRAY_LEN(text3d_surface_names), value);
            if (i >= 0)
                text3d_settings->surface = (xd_text3d_surface)i;
        } else if (KEY("Color"))
            parse_rgb_value(value, &text3d_settings->color);
    }
#undef SEC
#undef KEY
}

static void saver_settings_to_keyfile(GKeyFile *kf, const xd_saver_settings *sv) {
    const char *group_name = "ScreenSaver.Marquee";
    g_key_file_set_string(kf, group_name, "Text", sv->marquee.text);
    g_key_file_set_string(kf, group_name, "Font", sv->marquee.font.family);
    g_key_file_set_integer(kf, group_name, "FontSize", (int)sv->marquee.font.size_pt);
    g_key_file_set_boolean(kf, group_name, "Bold", sv->marquee.font.bold);
    g_key_file_set_boolean(kf, group_name, "Italic", sv->marquee.font.italic);
    set_rgb(kf, group_name, "TextColor", sv->marquee.color);
    set_rgb(kf, group_name, "BackgroundColor", sv->marquee.background);
    g_key_file_set_string(kf, group_name, "Position",
                          sv->marquee.random_position ? "random" : "centered");
    g_key_file_set_integer(kf, group_name, "Speed", sv->marquee.speed);
    group_name = "ScreenSaver.Starfield";
    g_key_file_set_integer(kf, group_name, "WarpSpeed", sv->starfield.speed);
    g_key_file_set_integer(kf, group_name, "Density", sv->starfield.density);
    group_name = "ScreenSaver.Mystify";
    for (int i = 0; i < XD_MYSTIFY_SHAPES; i++) {
        const xd_mystify_shape *sh = &sv->mystify.shapes[i];
        char key[32];
        g_snprintf(key, sizeof(key), "Polygon%dActive", i + 1);
        g_key_file_set_boolean(kf, group_name, key, sh->active);
        g_snprintf(key, sizeof(key), "Polygon%dLines", i + 1);
        g_key_file_set_integer(kf, group_name, key, sh->lines);
        g_snprintf(key, sizeof(key), "Polygon%dColors", i + 1);
        g_key_file_set_string(kf, group_name, key, sh->two_colors ? "two" : "random");
        g_snprintf(key, sizeof(key), "Polygon%dColor1", i + 1);
        set_rgb(kf, group_name, key, sh->colors[0]);
        g_snprintf(key, sizeof(key), "Polygon%dColor2", i + 1);
        set_rgb(kf, group_name, key, sh->colors[1]);
    }
    g_key_file_set_boolean(kf, group_name, "ClearScreen", sv->mystify.clear_screen);
    group_name = "ScreenSaver.Beziers";
    g_key_file_set_integer(kf, group_name, "Curves", sv->beziers.curves);
    g_key_file_set_integer(kf, group_name, "Lines", sv->beziers.lines);
    g_key_file_set_integer(kf, group_name, "Speed", sv->beziers.speed);
    group_name = "ScreenSaver.Flying";
    g_key_file_set_integer(kf, group_name, "WarpSpeed", sv->flying.speed);
    g_key_file_set_integer(kf, group_name, "Density", sv->flying.density);
    group_name = "ScreenSaver.Pipes";
    g_key_file_set_string(kf, group_name, "Pipes", sv->pipes.multiple ? "multiple" : "single");
    g_key_file_set_string(kf, group_name, "Joints", pipes_joint_names[sv->pipes.joints]);
    group_name = "ScreenSaver.Text3D";
    g_key_file_set_string(kf, group_name, "Display", sv->text3d.show_time ? "time" : "text");
    g_key_file_set_string(kf, group_name, "Text", sv->text3d.text);
    g_key_file_set_string(kf, group_name, "Rotation", text3d_rotation_names[sv->text3d.rotation]);
    g_key_file_set_integer(kf, group_name, "Speed", sv->text3d.speed);
    g_key_file_set_integer(kf, group_name, "Size", sv->text3d.size);
    g_key_file_set_string(kf, group_name, "Surface", text3d_surface_names[sv->text3d.surface]);
    set_rgb(kf, group_name, "Color", sv->text3d.color);
}

/* "21:30": minutes after midnight. */
static void parse_clock(xd_str value, int *out) {
    size_t colon;
    int64_t hour, minute;
    if (xd_str_find_char(value, ':', &colon) &&
        xd_str_parse_i64(xd_str_sub(value, 0, colon), 0, 23, &hour) &&
        xd_str_parse_i64(xd_str_sub(value, colon + 1, value.len), 0, 59, &minute))
        *out = (int)(hour * 60 + minute);
}

/* Degrees in C notation whatever the locale; false if not one, or not
 * within ±limit. */
static bool parse_degrees(xd_str value, double limit, double *out) {
    char buf[32];
    if (value.len == 0 || value.len >= sizeof(buf))
        return false;
    copy_into(buf, sizeof(buf), value);
    char *end = nullptr;
    double degrees = g_ascii_strtod(buf, &end);
    if (end == buf || *end != '\0' || !(fabs(degrees) <= limit))
        return false;
    *out = degrees;
    return true;
}

static void parse_nightlight(xd_nightlight_settings *nightlight_settings, xd_str key,
                             xd_str value) {
#define KEY(k) xd_str_eq_nocase(key, XD_STR(k))
    if (KEY("Enabled")) {
        parse_bool(value, &nightlight_settings->enabled);
    } else if (KEY("Schedule")) {
        int i = index_of(nightlight_schedule_names, XD_ARRAY_LEN(nightlight_schedule_names), value);
        if (i >= 0)
            nightlight_settings->schedule = (xd_nightlight_schedule)i;
    } else if (KEY("Temperature")) {
        parse_int(value, XD_NIGHTLIGHT_K_MIN, XD_NIGHTLIGHT_K_MAX,
                  &nightlight_settings->temperature);
    } else if (KEY("TurnOn")) {
        parse_clock(value, &nightlight_settings->on_minute);
    } else if (KEY("TurnOff")) {
        parse_clock(value, &nightlight_settings->off_minute);
    } else if (KEY("AutomaticLocation")) {
        parse_bool(value, &nightlight_settings->auto_location);
    } else if (KEY("Latitude") || KEY("Longitude")) {
        /* Both or neither; an empty value clears them. */
        bool lat = KEY("Latitude");
        double degrees;
        if (parse_degrees(value, lat ? 90.0 : 180.0, &degrees)) {
            *(lat ? &nightlight_settings->latitude : &nightlight_settings->longitude) = degrees;
            nightlight_settings->has_location = true;
        } else if (value.len == 0) {
            nightlight_settings->has_location = false;
        }
    }
#undef KEY
}

static bool on_entry(void *user, xd_str section, xd_str key, xd_str value) {
    xd_settings *settings = user;
#define SEC(n) xd_str_eq_nocase(section, XD_STR(n))
#define KEY(n) xd_str_eq_nocase(key, XD_STR(n))
    if (SEC("Appearance")) {
        if (KEY("Style")) {
            xd_visual_style st;
            if (xd_visual_style_from_name(value, &st))
                settings->style = st;
        } else if (KEY("Scheme")) {
            parse_stem(value, settings->scheme, sizeof(settings->scheme));
        } else if (KEY("FontSize")) {
            int i = index_of(font_size_names, XD_ARRAY_LEN(font_size_names), value);
            if (i >= 0)
                settings->font_size = (xd_font_size)i;
        } else if (KEY("FontSmoothing")) {
            int i = index_of(smoothing_names, XD_ARRAY_LEN(smoothing_names), value);
            if (i < 0)
                i = index_of(smoothing_old_names, XD_ARRAY_LEN(smoothing_old_names), value);
            if (i >= 0)
                settings->font_smoothing.antialias = (xd_font_antialias)i;
        } else if (KEY("FontSubpixelOrder")) {
            int i = index_of(subpixel_names, XD_ARRAY_LEN(subpixel_names), value);
            if (i >= 0)
                settings->font_smoothing.order = (xd_subpixel_order)i;
        } else if (KEY("FontHinting")) {
            int i = index_of(hinting_names, XD_ARRAY_LEN(hinting_names), value);
            if (i >= 0)
                settings->font_smoothing.hinting = (xd_font_hinting)i;
        } else if (KEY("GlassColor")) {
            parse_argb(value, &settings->glass_color);
        } else if (KEY("GlassTransparency")) {
            parse_bool(value, &settings->glass_transparency);
        } else if (KEY("IconTheme")) {
            parse_theme_dir(value, settings->icon_theme, sizeof(settings->icon_theme));
        } else if (KEY("GtkTheme")) {
            parse_theme_dir(value, settings->gtk_theme, sizeof(settings->gtk_theme));
        } else if (KEY("QtStyle")) {
            parse_qt_style(value, settings->qt_style, sizeof(settings->qt_style));
        } else if (KEY("ThemePackage")) {
            parse_theme_dir(value, settings->theme_package, sizeof(settings->theme_package));
        } else if (KEY("ThemeVariant")) {
            parse_theme_dir(value, settings->theme_variant, sizeof(settings->theme_variant));
        } else if (KEY("ImageScaling")) {
            parse_image_scaling(value, &settings->image_scaling);
        } else if (KEY("IntegerScaling")) {
            parse_bool(value, &settings->integer_scaling);
        }
    } else if (SEC("Desktop")) {
        if (KEY("Wallpaper")) {
            if (value.len == 0 || (value.ptr[0] == '/' && value.len < sizeof(settings->wallpaper)))
                copy_into(settings->wallpaper, sizeof(settings->wallpaper), value);
        } else if (KEY("WallpaperMode")) {
            int i = index_of(mode_names, XD_ARRAY_LEN(mode_names), value);
            if (i >= 0)
                settings->wallpaper_mode = (xd_wallpaper_mode)i;
        }
    } else if (SEC("CustomScheme")) {
        (void)xd_scheme_custom_set(&settings->custom_scheme, key,
                                   value); /* a bad value is left out */
    } else if (SEC("DesktopIcons")) {
        parse_desktop_icons(&settings->desktop_icons, key, value);
    } else if (SEC("Effects")) {
        if (KEY("MenuTransitions")) {
            parse_bool(value, &settings->menu_transitions);
        } else if (KEY("MenuTransition")) {
            int i = index_of(transition_names, XD_ARRAY_LEN(transition_names), value);
            if (i >= 0)
                settings->menu_transition = (xd_menu_transition)i;
        } else if (KEY("FullWindowDrag")) {
            parse_bool(value, &settings->full_window_drag);
        } else if (KEY("AnimateWindows")) {
            parse_bool(value, &settings->animate_windows);
        } else if (KEY("LargeIcons")) {
            parse_bool(value, &settings->large_icons);
        } else if (KEY("HideKeyboardCues")) {
            parse_bool(value, &settings->hide_keyboard_cues);
        }
    } else if (SEC("Windows")) {
        if (KEY("AlwaysDrawFrames"))
            parse_bool(value, &settings->always_frames);
    } else if (SEC("Taskbar")) {
        if (KEY("QuickLaunch"))
            parse_bool(value, &settings->quick_launch);
        else if (KEY("Locked"))
            parse_bool(value, &settings->locked);
        else if (KEY("AutoHide"))
            parse_bool(value, &settings->auto_hide);
        else if (KEY("OnTop"))
            parse_bool(value, &settings->on_top);
        else if (KEY("GroupButtons"))
            parse_bool(value, &settings->group);
        else if (KEY("Buttons")) {
            int i = index_of(buttons_names, XD_ARRAY_LEN(buttons_names), value);
            if (i >= 0)
                settings->buttons = (xd_taskbar_buttons)i;
        } else if (KEY("Layout")) {
            int i = index_of(layout_names, XD_ARRAY_LEN(layout_names), value);
            if (i >= 0)
                settings->layout = (xd_taskbar_layout)i;
        } else if (KEY("Icons")) {
            int i = index_of(taskbar_icons_names, XD_ARRAY_LEN(taskbar_icons_names), value);
            if (i >= 0)
                settings->icons = (xd_taskbar_icons)i;
        } else if (KEY("ShowClock"))
            parse_bool(value, &settings->show_clock);
        else if (KEY("HideInactiveIcons"))
            parse_bool(value, &settings->hide_inactive);
        else if (KEY("AllDisplays"))
            parse_bool(value, &settings->all_displays);
        else if (KEY("Edge")) {
            int i = index_of(edge_names, XD_ARRAY_LEN(edge_names), value);
            if (i >= 0)
                settings->edge = (xd_edge)i;
        } else if (KEY("Rows")) {
            int64_t number;
            if (xd_str_parse_i64(value, 1, 8, &number))
                settings->rows = (int)number;
        } else if (KEY("SideWidth")) {
            int64_t number;
            if (xd_str_parse_i64(value, 40, 800, &number))
                settings->side_width = (int)number;
        } else if (KEY("QuickLaunchLargeIcons")) {
            parse_bool(value, &settings->quick_large);
        } else if (KEY("QuickLaunchShowText")) {
            parse_bool(value, &settings->quick_show_text);
        } else if (KEY("QuickLaunchShowTitle")) {
            parse_bool(value, &settings->quick_show_title);
        }
    } else if (SEC("StartMenu")) {
        if (KEY("Style")) {
            int i = index_of(start_style_names, XD_ARRAY_LEN(start_style_names), value);
            if (i >= 0)
                settings->start_style = (xd_start_style)i;
        } else if (KEY("LargeIcons")) {
            parse_bool(value, &settings->start_large_icons);
        } else if (KEY("Programs")) {
            int64_t number;
            if (xd_str_parse_i64(value, 0, XD_START_PROGRAMS_MAX, &number))
                settings->start_programs = (int)number;
        } else if (KEY("HighlightNewPrograms")) {
            parse_bool(value, &settings->start_highlight_new);
        }
    } else if (SEC("ScreenSaver")) {
        if (KEY("Name")) {
            copy_into(settings->screensaver, sizeof(settings->screensaver), value);
        } else if (KEY("Wait")) {
            int64_t minute_count;
            if (xd_str_parse_i64(value, 1, 600, &minute_count))
                settings->screensaver_wait = (int)minute_count;
        } else if (KEY("Password")) {
            parse_bool(value, &settings->screensaver_password);
        } else if (KEY("MonitorOff")) {
            int64_t minute_count;
            if (xd_str_parse_i64(value, 0, 1440, &minute_count))
                settings->monitor_off = (int)minute_count;
        }
    } else if (xd_str_eq_nocase(xd_str_sub(section, 0, 12), XD_STR("ScreenSaver."))) {
        parse_saver_entry(&settings->savers, section, key, value);
    } else if (SEC("NightLight")) {
        parse_nightlight(&settings->night_light, key, value);
    } else if (SEC("ColorProfiles")) {
        char monitor[sizeof(settings->color_profiles[0].monitor)];
        char path[sizeof(settings->color_profiles[0].path)];
        if (key.len < sizeof(monitor) && value.len < sizeof(path)) {
            copy_into(monitor, sizeof(monitor), key);
            copy_into(path, sizeof(path), value);
            (void)xd_settings_set_color_profile(settings, monitor, path);
        }
    } else if (SEC("Notifications")) {
        int i = index_of(tray_rule_names, XD_ARRAY_LEN(tray_rule_names), value);
        if (i > 0 && key.len > 0 && key.len < sizeof(settings->tray_rules[0].id)) {
            char id[sizeof(settings->tray_rules[0].id)];
            copy_into(id, sizeof(id), key);
            xd_settings_set_tray_rule(settings, id, (xd_tray_rule)i);
        }
    } else if (SEC("Keyboard")) {
        xd_keyboard_settings *keyboard = &settings->keyboard;
        int64_t number;
        if (KEY("Layout"))
            copy_into(keyboard->layout, sizeof(keyboard->layout), value);
        else if (KEY("Variant"))
            copy_into(keyboard->variant, sizeof(keyboard->variant), value);
        else if (KEY("Options"))
            copy_into(keyboard->options, sizeof(keyboard->options), value);
        else if (KEY("Model"))
            copy_into(keyboard->model, sizeof(keyboard->model), value);
        else if (KEY("RepeatDelay") && xd_str_parse_i64(value, 100, 2000, &number))
            keyboard->repeat_delay = (int)number;
        else if (KEY("RepeatRate") && xd_str_parse_i64(value, 0, 100, &number))
            keyboard->repeat_rate = (int)number;
        else if (KEY("CursorBlinkMs") && xd_str_parse_i64(value, 0, 2000, &number))
            keyboard->cursor_blink_ms = (int)number;
        else if (KEY("InputMethod"))
            copy_into(keyboard->input_method, sizeof(keyboard->input_method), value);
    } else if (SEC("Mouse")) {
        parse_mouse(&settings->mouse, key, value);
    } else if (SEC("Accessibility")) {
        parse_access(&settings->access, key, value);
    } else if (SEC("Sound")) {
        if (KEY("VolumeIcon"))
            parse_bool(value, &settings->volume_icon);
    } else if (SEC("Bluetooth")) {
        if (KEY("ShowIcon"))
            parse_bool(value, &settings->bluetooth_icon);
    } else if (SEC("Updates")) {
        int i = -1;
        if (KEY("Mode") &&
            (i = index_of(updates_mode_names, XD_ARRAY_LEN(updates_mode_names), value)) >= 0)
            settings->updates.mode = (xd_updates_mode)i;
        else if (KEY("Day") &&
                 (i = index_of(updates_day_names, XD_ARRAY_LEN(updates_day_names), value)) >= 0)
            settings->updates.day = i;
        else if (KEY("Hour"))
            parse_int(value, 0, 23, &settings->updates.hour);
    } else if (SEC("Power")) {
        int64_t minute_count;
        if (KEY("Scheme")) {
            int i = index_of(power_scheme_names, XD_ARRAY_LEN(power_scheme_names), value);
            if (i >= 0)
                settings->power_scheme = (xd_power_scheme)i;
        } else if (KEY("Standby") && xd_str_parse_i64(value, 0, 1440, &minute_count)) {
            settings->standby = (int)minute_count;
        } else if (KEY("Hibernate") && xd_str_parse_i64(value, 0, 1440, &minute_count)) {
            settings->hibernate = (int)minute_count;
        } else if (KEY("MonitorOffOnBattery")) {
            parse_int(value, 0, 1440, &settings->monitor_off_battery);
        } else if (KEY("StandbyOnBattery")) {
            parse_int(value, 0, 1440, &settings->standby_battery);
        } else if (KEY("HibernateOnBattery")) {
            parse_int(value, 0, 1440, &settings->hibernate_battery);
        } else if (KEY("CustomScheme")) {
            copy_into(settings->power_scheme_custom, sizeof(settings->power_scheme_custom), value);
        } else if (KEY("AlwaysShowIcon")) {
            parse_bool(value, &settings->battery_icon);
        } else if (KEY("PasswordOnResume")) {
            parse_bool(value, &settings->resume_password);
        } else if (key.len >= 8 && xd_str_eq_nocase(xd_str_sub(key, 0, 8), XD_STR("LowAlarm"))) {
            parse_alarm(&settings->low_alarm, xd_str_sub(key, 8, key.len), value);
        } else if (key.len >= 13 &&
                   xd_str_eq_nocase(xd_str_sub(key, 0, 13), XD_STR("CriticalAlarm"))) {
            parse_alarm(&settings->critical_alarm, xd_str_sub(key, 13, key.len), value);
        } else {
            int button = index_of(button_keys, XD_ARRAY_LEN(button_keys), key);
            int i = index_of(button_action_names, XD_ARRAY_LEN(button_action_names), value);
            if (button >= 0 && i >= 0)
                settings->power_buttons[button] = (xd_button_action)i;
        }
    } else if (SEC("Folders")) {
        xd_folder_settings *folder_settings = &settings->folders;
        if (KEY("NewWindow"))
            parse_bool(value, &folder_settings->new_window);
        else if (KEY("NewTabs"))
            parse_bool(value, &folder_settings->new_tabs);
        else if (KEY("SingleClick"))
            parse_bool(value, &folder_settings->single_click);
        else if (KEY("ShowHidden"))
            parse_bool(value, &folder_settings->show_hidden);
        else if (KEY("HideExtensions"))
            parse_bool(value, &folder_settings->hide_extensions);
        else if (KEY("FullPathAddress"))
            parse_bool(value, &folder_settings->full_path_address);
        else if (KEY("FullPathTitle"))
            parse_bool(value, &folder_settings->full_path_title);
        else if (KEY("RememberViews"))
            parse_bool(value, &folder_settings->remember_views);
        else if (KEY("CommonTasks"))
            parse_bool(value, &folder_settings->common_tasks);
        else if (KEY("DefaultView")) {
            int i = index_of(view_names, XD_ARRAY_LEN(view_names), value);
            if (i >= 0 || value.len == 0)
                copy_into(folder_settings->default_view, sizeof(folder_settings->default_view),
                          i >= 0 ? xd_str_from_cstr(view_names[i]) : value);
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
    for (gsize group_index = 0; ok && group_index < ngroups; group_index++) {
        gsize nkeys = 0;
        gchar **keys = g_key_file_get_keys(kf, groups[group_index], &nkeys, nullptr);
        for (gsize k = 0; ok && k < nkeys; k++) {
            /* Unescaped string; the raw value if it has a bad escape. */
            gchar *text = g_key_file_get_string(kf, groups[group_index], keys[k], nullptr);
            if (text == nullptr)
                text = g_key_file_get_value(kf, groups[group_index], keys[k], nullptr);
            ok = fn(user, xd_str_from_cstr(groups[group_index]), xd_str_from_cstr(keys[k]),
                    xd_str_from_cstr(text));
            g_free(text);
        }
        g_strfreev(keys);
    }
    g_strfreev(groups);
    return ok;
}

void xd_settings_parse(xd_str text, xd_settings *settings) {
    xd_settings_defaults(settings);
    xd_settings tmp = *settings;
    GKeyFile *kf = g_key_file_new();
    /* A syntax error leaves the defaults rather than a half-read file. */
    if (g_key_file_load_from_data(kf, text.ptr, text.len, G_KEY_FILE_NONE, nullptr) &&
        keyfile_foreach(kf, on_entry, &tmp))
        *settings = tmp;
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

void xd_settings_load(xd_settings *settings) {
    xd_strbuf path = {0};
    xd_strbuf text = {0};
    xd_settings_path(&path);
    if (xd_read_file(xd_strbuf_cstr(&path), 1u << 20, &text))
        xd_settings_parse(xd_strbuf_view(&text), settings);
    else
        xd_settings_defaults(settings);
    xd_strbuf_free(&text);
    xd_strbuf_free(&path);
}

static void set_custom_key(void *user, const char *key, const char *value) {
    g_key_file_set_string(user, "CustomScheme", key, value);
}

static GKeyFile *to_keyfile(const xd_settings *settings) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Appearance", "Style", xd_visual_style_name(settings->style));
    g_key_file_set_string(kf, "Appearance", "Scheme", settings->scheme);
    g_key_file_set_string(kf, "Appearance", "FontSize", font_size_names[settings->font_size]);
    g_key_file_set_string(kf, "Appearance", "FontSmoothing",
                          smoothing_names[settings->font_smoothing.antialias]);
    g_key_file_set_string(kf, "Appearance", "FontSubpixelOrder",
                          subpixel_names[settings->font_smoothing.order]);
    g_key_file_set_string(kf, "Appearance", "FontHinting",
                          hinting_names[settings->font_smoothing.hinting]);
    char glass[16];
    snprintf(glass, sizeof(glass), "%08X", (unsigned)settings->glass_color);
    g_key_file_set_string(kf, "Appearance", "GlassColor", glass);
    g_key_file_set_boolean(kf, "Appearance", "GlassTransparency", settings->glass_transparency);
    g_key_file_set_string(kf, "Appearance", "IconTheme", settings->icon_theme);
    g_key_file_set_string(kf, "Appearance", "GtkTheme", settings->gtk_theme);
    g_key_file_set_string(kf, "Appearance", "QtStyle", settings->qt_style);
    g_key_file_set_string(kf, "Appearance", "ThemePackage", settings->theme_package);
    g_key_file_set_string(kf, "Appearance", "ThemeVariant", settings->theme_variant);
    g_key_file_set_string(kf, "Appearance", "ImageScaling",
                          settings->image_scaling == XD_FILTER_COUNT
                              ? "Automatic"
                              : xd_image_filter_name(settings->image_scaling));
    g_key_file_set_boolean(kf, "Appearance", "IntegerScaling", settings->integer_scaling);
    g_key_file_set_comment(kf, "Appearance", nullptr,
                           " XeonDesktop settings. Written by Display Properties; edits are\n"
                           " picked up immediately.",
                           nullptr);
    xd_scheme_custom_foreach(&settings->custom_scheme, set_custom_key, kf);
    g_key_file_set_string(kf, "Desktop", "Wallpaper", settings->wallpaper);
    g_key_file_set_string(kf, "Desktop", "WallpaperMode", mode_names[settings->wallpaper_mode]);
    const xd_desktop_icon_settings *di = &settings->desktop_icons;
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
    g_key_file_set_boolean(kf, "Effects", "MenuTransitions", settings->menu_transitions);
    g_key_file_set_string(kf, "Effects", "MenuTransition",
                          transition_names[settings->menu_transition]);
    g_key_file_set_boolean(kf, "Effects", "FullWindowDrag", settings->full_window_drag);
    g_key_file_set_boolean(kf, "Effects", "AnimateWindows", settings->animate_windows);
    g_key_file_set_boolean(kf, "Effects", "LargeIcons", settings->large_icons);
    g_key_file_set_boolean(kf, "Effects", "HideKeyboardCues", settings->hide_keyboard_cues);
    g_key_file_set_boolean(kf, "Windows", "AlwaysDrawFrames", settings->always_frames);
    g_key_file_set_boolean(kf, "Taskbar", "QuickLaunch", settings->quick_launch);
    g_key_file_set_boolean(kf, "Taskbar", "Locked", settings->locked);
    g_key_file_set_boolean(kf, "Taskbar", "AutoHide", settings->auto_hide);
    g_key_file_set_boolean(kf, "Taskbar", "OnTop", settings->on_top);
    g_key_file_set_boolean(kf, "Taskbar", "GroupButtons", settings->group);
    g_key_file_set_string(kf, "Taskbar", "Buttons", buttons_names[settings->buttons]);
    g_key_file_set_string(kf, "Taskbar", "Layout", layout_names[settings->layout]);
    g_key_file_set_string(kf, "Taskbar", "Icons", taskbar_icons_names[settings->icons]);
    g_key_file_set_boolean(kf, "Taskbar", "ShowClock", settings->show_clock);
    g_key_file_set_boolean(kf, "Taskbar", "HideInactiveIcons", settings->hide_inactive);
    g_key_file_set_boolean(kf, "Taskbar", "AllDisplays", settings->all_displays);
    g_key_file_set_string(kf, "Taskbar", "Edge", edge_names[settings->edge]);
    g_key_file_set_integer(kf, "Taskbar", "Rows", settings->rows);
    g_key_file_set_integer(kf, "Taskbar", "SideWidth", settings->side_width);
    g_key_file_set_boolean(kf, "Taskbar", "QuickLaunchLargeIcons", settings->quick_large);
    g_key_file_set_boolean(kf, "Taskbar", "QuickLaunchShowText", settings->quick_show_text);
    g_key_file_set_boolean(kf, "Taskbar", "QuickLaunchShowTitle", settings->quick_show_title);
    g_key_file_set_string(kf, "StartMenu", "Style", start_style_names[settings->start_style]);
    g_key_file_set_boolean(kf, "StartMenu", "LargeIcons", settings->start_large_icons);
    g_key_file_set_integer(kf, "StartMenu", "Programs", settings->start_programs);
    g_key_file_set_boolean(kf, "StartMenu", "HighlightNewPrograms", settings->start_highlight_new);
    g_key_file_set_string(kf, "ScreenSaver", "Name", settings->screensaver);
    g_key_file_set_integer(kf, "ScreenSaver", "Wait", settings->screensaver_wait);
    g_key_file_set_boolean(kf, "ScreenSaver", "Password", settings->screensaver_password);
    g_key_file_set_integer(kf, "ScreenSaver", "MonitorOff", settings->monitor_off);
    saver_settings_to_keyfile(kf, &settings->savers);
    g_key_file_set_string(kf, "Keyboard", "Layout", settings->keyboard.layout);
    g_key_file_set_string(kf, "Keyboard", "Variant", settings->keyboard.variant);
    g_key_file_set_string(kf, "Keyboard", "Options", settings->keyboard.options);
    g_key_file_set_string(kf, "Keyboard", "Model", settings->keyboard.model);
    g_key_file_set_integer(kf, "Keyboard", "RepeatDelay", settings->keyboard.repeat_delay);
    g_key_file_set_integer(kf, "Keyboard", "RepeatRate", settings->keyboard.repeat_rate);
    g_key_file_set_boolean(kf, "Sound", "VolumeIcon", settings->volume_icon);
    g_key_file_set_string(kf, "Power", "Scheme", power_scheme_names[settings->power_scheme]);
    g_key_file_set_integer(kf, "Power", "Standby", settings->standby);
    g_key_file_set_integer(kf, "Power", "Hibernate", settings->hibernate);
    g_key_file_set_integer(kf, "Power", "MonitorOffOnBattery", settings->monitor_off_battery);
    g_key_file_set_integer(kf, "Power", "StandbyOnBattery", settings->standby_battery);
    g_key_file_set_integer(kf, "Power", "HibernateOnBattery", settings->hibernate_battery);
    g_key_file_set_string(kf, "Power", "CustomScheme", settings->power_scheme_custom);
    g_key_file_set_boolean(kf, "Power", "AlwaysShowIcon", settings->battery_icon);
    g_key_file_set_boolean(kf, "Power", "PasswordOnResume", settings->resume_password);
    const struct {
        const char *prefix;
        const xd_battery_alarm *alarm;
    } alarms[] = {{"LowAlarm", &settings->low_alarm}, {"CriticalAlarm", &settings->critical_alarm}};
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
    /* Unset leaves them to logind, which a written "logind" would too;
     * left out, desktop.ini says nothing about them. */
    for (size_t button = 0; button < XD_POWER_BUTTON_COUNT; button++) {
        if (settings->power_buttons[button] != XD_BUTTON_LOGIND)
            g_key_file_set_string(kf, "Power", button_keys[button],
                                  button_action_names[settings->power_buttons[button]]);
    }
    g_key_file_set_boolean(kf, "Bluetooth", "ShowIcon", settings->bluetooth_icon);
    g_key_file_set_string(kf, "Updates", "Mode", updates_mode_names[settings->updates.mode]);
    g_key_file_set_string(kf, "Updates", "Day", updates_day_names[settings->updates.day]);
    g_key_file_set_integer(kf, "Updates", "Hour", settings->updates.hour);
    const xd_nightlight_settings *nl = &settings->night_light;
    g_key_file_set_boolean(kf, "NightLight", "Enabled", nl->enabled);
    g_key_file_set_string(kf, "NightLight", "Schedule", nightlight_schedule_names[nl->schedule]);
    g_key_file_set_integer(kf, "NightLight", "Temperature", nl->temperature);
    char clock[8];
    g_snprintf(clock, sizeof(clock), "%02d:%02d", nl->on_minute / 60, nl->on_minute % 60);
    g_key_file_set_string(kf, "NightLight", "TurnOn", clock);
    g_snprintf(clock, sizeof(clock), "%02d:%02d", nl->off_minute / 60, nl->off_minute % 60);
    g_key_file_set_string(kf, "NightLight", "TurnOff", clock);
    g_key_file_set_boolean(kf, "NightLight", "AutomaticLocation", nl->auto_location);
    char deg[G_ASCII_DTOSTR_BUF_SIZE];
    g_key_file_set_string(kf, "NightLight", "Latitude",
                          nl->has_location ? g_ascii_formatd(deg, sizeof(deg), "%.4f", nl->latitude)
                                           : "");
    g_key_file_set_string(kf, "NightLight", "Longitude",
                          nl->has_location
                              ? g_ascii_formatd(deg, sizeof(deg), "%.4f", nl->longitude)
                              : "");
    for (size_t i = 0; i < settings->color_profile_count; i++)
        g_key_file_set_string(kf, "ColorProfiles", settings->color_profiles[i].monitor,
                              settings->color_profiles[i].path);
    const xd_folder_settings *folder_settings = &settings->folders;
    g_key_file_set_boolean(kf, "Folders", "NewWindow", folder_settings->new_window);
    g_key_file_set_boolean(kf, "Folders", "NewTabs", folder_settings->new_tabs);
    g_key_file_set_boolean(kf, "Folders", "SingleClick", folder_settings->single_click);
    g_key_file_set_boolean(kf, "Folders", "ShowHidden", folder_settings->show_hidden);
    g_key_file_set_boolean(kf, "Folders", "HideExtensions", folder_settings->hide_extensions);
    g_key_file_set_boolean(kf, "Folders", "FullPathAddress", folder_settings->full_path_address);
    g_key_file_set_boolean(kf, "Folders", "FullPathTitle", folder_settings->full_path_title);
    g_key_file_set_boolean(kf, "Folders", "RememberViews", folder_settings->remember_views);
    g_key_file_set_boolean(kf, "Folders", "CommonTasks", folder_settings->common_tasks);
    g_key_file_set_string(kf, "Folders", "DefaultView", folder_settings->default_view);
    g_key_file_set_integer(kf, "Keyboard", "CursorBlinkMs", settings->keyboard.cursor_blink_ms);
    g_key_file_set_string(kf, "Keyboard", "InputMethod", settings->keyboard.input_method);
    const xd_mouse_settings *mouse = &settings->mouse;
    g_key_file_set_boolean(kf, "Mouse", "LeftHanded", mouse->left_handed);
    g_key_file_set_integer(kf, "Mouse", "Speed", mouse->speed);
    g_key_file_set_boolean(kf, "Mouse", "Acceleration", mouse->acceleration);
    g_key_file_set_integer(kf, "Mouse", "DoubleClickMs", mouse->double_click_ms);
    g_key_file_set_integer(kf, "Mouse", "WheelLines", mouse->wheel_lines);
    g_key_file_set_string(kf, "Mouse", "CursorTheme", mouse->cursor_theme);
    g_key_file_set_integer(kf, "Mouse", "CursorSize", mouse->cursor_size);
    g_key_file_set_boolean(kf, "Mouse", "ClickLock", mouse->click_lock);
    g_key_file_set_integer(kf, "Mouse", "ClickLockMs", mouse->click_lock_ms);
    const xd_access_settings *access_settings = &settings->access;
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeys", access_settings->sticky_keys);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysLock", access_settings->sticky_lock);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysShortcut",
                           access_settings->sticky_shortcut);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysTwoKeysOff",
                           access_settings->sticky_two_keys_off);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysSound", access_settings->sticky_sound);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeys", access_settings->filter_keys);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeysShortcut",
                           access_settings->filter_shortcut);
    g_key_file_set_boolean(kf, "Accessibility", "SlowKeys", access_settings->slow_keys);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeysBeep", access_settings->filter_beep);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeysNoRepeat",
                           access_settings->filter_no_repeat);
    g_key_file_set_integer(kf, "Accessibility", "BounceMs", access_settings->bounce_ms);
    g_key_file_set_integer(kf, "Accessibility", "SlowKeysMs", access_settings->slow_ms);
    g_key_file_set_integer(kf, "Accessibility", "FilterKeysRepeatDelay",
                           access_settings->filter_repeat_delay);
    g_key_file_set_integer(kf, "Accessibility", "FilterKeysRepeatMs",
                           access_settings->filter_repeat_ms);
    g_key_file_set_boolean(kf, "Accessibility", "ToggleKeys", access_settings->toggle_keys);
    g_key_file_set_boolean(kf, "Accessibility", "ToggleKeysShortcut",
                           access_settings->toggle_shortcut);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeys", access_settings->mouse_keys);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeysShortcut",
                           access_settings->mouse_keys_shortcut);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeysModifiers",
                           access_settings->mouse_keys_modifiers);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeysNumLock",
                           access_settings->mouse_keys_numlock);
    g_key_file_set_integer(kf, "Accessibility", "MouseKeysSpeed",
                           access_settings->mouse_keys_speed);
    g_key_file_set_integer(kf, "Accessibility", "MouseKeysAccelMs",
                           access_settings->mouse_keys_accel_ms);
    g_key_file_set_boolean(kf, "Accessibility", "WarnOnShortcut", access_settings->warn);
    g_key_file_set_boolean(kf, "Accessibility", "SoundOnOff", access_settings->sound_on_off);
    g_key_file_set_boolean(kf, "Accessibility", "StickyKeysIndicator",
                           access_settings->sticky_indicator);
    g_key_file_set_boolean(kf, "Accessibility", "FilterKeysIndicator",
                           access_settings->filter_indicator);
    g_key_file_set_boolean(kf, "Accessibility", "MouseKeysIndicator",
                           access_settings->mouse_keys_indicator);
    g_key_file_set_boolean(kf, "Accessibility", "SoundSentry", access_settings->sound_sentry);
    g_key_file_set_string(kf, "Accessibility", "SoundSentryEffect",
                          sentry_names[access_settings->sound_sentry_effect]);
    g_key_file_set_boolean(kf, "Accessibility", "ShowSounds", access_settings->show_sounds);
    g_key_file_set_boolean(kf, "Accessibility", "AutoReset", access_settings->auto_reset);
    g_key_file_set_integer(kf, "Accessibility", "AutoResetMinutes",
                           access_settings->auto_reset_minutes);
    g_key_file_set_boolean(kf, "Accessibility", "HighContrast", access_settings->high_contrast);
    g_key_file_set_string(kf, "Accessibility", "SavedStyle",
                          xd_visual_style_name(access_settings->saved_style));
    g_key_file_set_string(kf, "Accessibility", "SavedScheme", access_settings->saved_scheme);
    for (size_t i = 0; i < settings->tray_rule_count; i++)
        g_key_file_set_string(kf, "Notifications", settings->tray_rules[i].id,
                              tray_rule_names[settings->tray_rules[i].rule]);
    return kf;
}

void xd_settings_serialize(const xd_settings *settings, xd_strbuf *out) {
    GKeyFile *kf = to_keyfile(settings);
    gsize len = 0;
    gchar *data = g_key_file_to_data(kf, &len, nullptr);
    xd_strbuf_append(out, (xd_str){data, len});
    g_free(data);
    g_key_file_free(kf);
}

bool xd_settings_equal(const xd_settings *left, const xd_settings *right) {
    xd_strbuf left_text = {0};
    xd_strbuf right_text = {0};
    xd_settings_serialize(left, &left_text);
    xd_settings_serialize(right, &right_text);
    bool eq = xd_str_eq(xd_strbuf_view(&left_text), xd_strbuf_view(&right_text));
    xd_strbuf_free(&left_text);
    xd_strbuf_free(&right_text);
    return eq;
}

void xd_settings_merge(const xd_settings *base, const xd_settings *pending, xd_settings *now) {
    GKeyFile *base_file = to_keyfile(base);
    GKeyFile *pending_file = to_keyfile(pending);
    GKeyFile *now_file = to_keyfile(now);
    /* Keys `pending` set or changed... */
    gchar **groups = g_key_file_get_groups(pending_file, nullptr);
    for (gchar **group_name = groups; *group_name != nullptr; group_name++) {
        gchar **keys = g_key_file_get_keys(pending_file, *group_name, nullptr, nullptr);
        for (gchar **key = keys; keys != nullptr && *key != nullptr; key++) {
            gchar *pv = g_key_file_get_value(pending_file, *group_name, *key, nullptr);
            gchar *bv = g_key_file_get_value(base_file, *group_name, *key, nullptr);
            if (pv != nullptr && g_strcmp0(pv, bv) != 0)
                g_key_file_set_value(now_file, *group_name, *key, pv);
            g_free(pv);
            g_free(bv);
        }
        g_strfreev(keys);
    }
    g_strfreev(groups);
    /* ...and the ones it removed (notification rules back to the default). */
    groups = g_key_file_get_groups(base_file, nullptr);
    for (gchar **group_name = groups; *group_name != nullptr; group_name++) {
        gchar **keys = g_key_file_get_keys(base_file, *group_name, nullptr, nullptr);
        for (gchar **key = keys; keys != nullptr && *key != nullptr; key++) {
            if (!g_key_file_has_key(pending_file, *group_name, *key, nullptr))
                g_key_file_remove_key(now_file, *group_name, *key, nullptr);
        }
        g_strfreev(keys);
    }
    g_strfreev(groups);
    gsize len = 0;
    gchar *data = g_key_file_to_data(now_file, &len, nullptr);
    xd_settings_parse((xd_str){data, len}, now);
    g_free(data);
    g_key_file_free(now_file);
    g_key_file_free(pending_file);
    g_key_file_free(base_file);
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

bool xd_settings_save(const xd_settings *settings) {
    xd_strbuf path = {0};
    xd_settings_path(&path);
    mkdirs_for(&path);
    /* g_key_file_save_to_file writes a temporary file and renames it, so
     * watchers never see a partial file. */
    GKeyFile *kf = to_keyfile(settings);
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

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
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
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
        self[length] = '\0';
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

static int compare_names(const void *left, const void *b2) {
    return strcoll(*(char *const *)left, *(char *const *)b2);
}

void xd_scheme_list_load(xd_scheme_list *list) {
    *list = (xd_scheme_list){0};
    xd_strbuf dir = {0};
    if (!xd_data_dir(&dir)) {
        xd_strbuf_free(&dir);
        return;
    }
    xd_strbuf_append(&dir, XD_STR("/schemes"));
    DIR *folder_handle = opendir(xd_strbuf_cstr(&dir));
    xd_strbuf_free(&dir);
    if (folder_handle == nullptr)
        return;

    /* Collect "Name\tstem" pairs, sort by name, then split. */
    char **pairs = nullptr;
    size_t count = 0;
    size_t cap = 0;
    struct dirent *de;
    while ((de = readdir(folder_handle)) != nullptr) {
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
            if (count == cap) {
                cap = cap == 0 ? 16 : cap * 2;
                pairs = xd_realloc_array(pairs, cap, sizeof(*pairs));
            }
            xd_strbuf pair = {0};
            xd_strbuf_appendf(&pair, "%s\t%s", scheme.name, stem_c);
            pairs[count++] = xd_str_dup_cstr(xd_strbuf_view(&pair));
            xd_strbuf_free(&pair);
        }
        xd_free(stem_c);
    }
    closedir(folder_handle);
    if (count > 1)
        qsort(pairs, count, sizeof(*pairs), compare_names);

    list->stems = xd_alloc_array(XD_MAX(count, 1), sizeof(char *));
    list->names = xd_alloc_array(XD_MAX(count, 1), sizeof(char *));
    for (size_t i = 0; i < count; i++) {
        xd_str pair_text = xd_str_from_cstr(pairs[i]);
        size_t tab = 0;
        XD_ASSERT(xd_str_find_char(pair_text, '\t', &tab));
        list->names[i] = xd_str_dup_cstr(xd_str_sub(pair_text, 0, tab));
        list->stems[i] = xd_str_dup_cstr(xd_str_sub(pair_text, tab + 1, pair_text.len));
        xd_free(pairs[i]);
    }
    xd_free(pairs);
    list->count = count;
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

void xd_settings_scheme(const xd_settings *settings, xd_scheme *out) {
    xd_scheme_init_default(out);
    if (settings->scheme[0] != '\0') {
        xd_strbuf path = {0};
        scheme_file(settings->scheme, &path);
        if (path.len == 0 || !xd_scheme_load(xd_strbuf_cstr(&path), out, nullptr))
            xd_scheme_init_default(out);
        xd_strbuf_free(&path);
    }
    /* Large / Extra Large: Windows' (large) schemes scale fonts and the
     * caption and menu metrics together. */
    static const double factors[] = {1.0, 1.25, 1.5};
    double factor = factors[settings->font_size];
    if (factor != 1.0) {
        for (size_t i = 0; i < XD_FONT_COUNT; i++)
            out->fonts[i].size_pt *= factor;
        static const xd_metric scaled[] = {
            XD_METRIC_CAPTION_HEIGHT, XD_METRIC_CAPTION_BUTTON_WIDTH, XD_METRIC_MENU_HEIGHT,
            XD_METRIC_TASKBAR_HEIGHT, XD_METRIC_SM_CAPTION_HEIGHT};
        for (size_t i = 0; i < XD_ARRAY_LEN(scaled); i++)
            out->metrics[scaled[i]] = (int)lround(out->metrics[scaled[i]] * factor);
    }
    /* Advanced Appearance's changes, as they were made: over the font
     * size. High Contrast's scheme goes without them (they come back when
     * it's turned off). */
    if (!settings->access.high_contrast)
        xd_scheme_apply_custom(out, &settings->custom_scheme);
    out->font_smoothing = settings->font_smoothing;
    out->glass_color = settings->glass_color;
    out->glass_opaque = !settings->glass_transparency;
}

void xd_settings_set_high_contrast(xd_settings *settings, bool on, const char *scheme) {
    xd_access_settings *access_settings = &settings->access;
    if (on) {
        if (!access_settings->high_contrast) {
            access_settings->saved_style = settings->style;
            memcpy(access_settings->saved_scheme, settings->scheme,
                   sizeof(access_settings->saved_scheme));
        }
        settings->style = XD_STYLE_CLASSIC;
        parse_stem(xd_str_from_cstr(scheme), settings->scheme, sizeof(settings->scheme));
    } else if (access_settings->high_contrast) {
        settings->style = access_settings->saved_style;
        memcpy(settings->scheme, access_settings->saved_scheme, sizeof(settings->scheme));
        access_settings->saved_style = XD_STYLE_CLASSIC;
        access_settings->saved_scheme[0] = '\0';
    }
    access_settings->high_contrast = on;
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
        ssize_t bytes_read = read(fd, buf, sizeof(buf));
        if (bytes_read <= 0)
            break;
        size_t off = 0;
        while (off + sizeof(struct inotify_event) <= (size_t)bytes_read) {
            struct inotify_event ev;
            memcpy(&ev, buf + off, sizeof(ev));
            size_t total = sizeof(ev) + ev.len;
            if (total > (size_t)bytes_read - off)
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
