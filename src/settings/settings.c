/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/alloc.h"
#include <glib.h>
#include "settings.h"

static const char *const mode_names[XD_WALLPAPER_MODE_COUNT] = {
    [XD_WALLPAPER_CENTER] = "center", [XD_WALLPAPER_TILE] = "tile",
    [XD_WALLPAPER_STRETCH] = "stretch", [XD_WALLPAPER_FILL] = "fill",
    [XD_WALLPAPER_FIT] = "fit",
};

static const char *const font_size_names[] = {"normal", "large", "extralarge"};
static const char *const start_style_names[] = {"classic", "xp", "win7"};
static const char *const smoothing_names[] = {"standard", "none", "cleartype"};
static const char *const edge_names[] = {"bottom", "top", "left", "right"};
static const char *const tray_rule_names[] = {"auto", "hide", "show"};

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

void xd_settings_defaults(xd_settings *s) {
    *s = (xd_settings){
        .style = XD_STYLE_CLASSIC,
        .wallpaper_mode = XD_WALLPAPER_FILL,
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
        .screensaver_wait = 15,
        /* Windows' defaults: about 30 repeats a second after half a second. */
        .keyboard = {.repeat_delay = 500, .repeat_rate = 30},
    };
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
            /* A stem, not a path: keep it to safe file-name characters. */
            bool ok = v.len < sizeof(s->scheme);
            for (size_t i = 0; ok && i < v.len; i++) {
                char c = v.ptr[i];
                ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_';
            }
            if (ok)
                copy_into(s->scheme, sizeof(s->scheme), v);
        } else if (KEY("FontSize")) {
            int i = index_of(font_size_names, XD_ARRAY_LEN(font_size_names), v);
            if (i >= 0)
                s->font_size = (xd_font_size)i;
        } else if (KEY("FontSmoothing")) {
            int i = index_of(smoothing_names, XD_ARRAY_LEN(smoothing_names), v);
            if (i >= 0)
                s->font_smoothing = (xd_font_smoothing)i;
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
    } else if (SEC("Effects")) {
        if (KEY("MenuTransitions"))
            parse_bool(v, &s->menu_transitions);
        else if (KEY("FullWindowDrag"))
            parse_bool(v, &s->full_window_drag);
        else if (KEY("AnimateWindows"))
            parse_bool(v, &s->animate_windows);
        else if (KEY("LargeIcons"))
            parse_bool(v, &s->large_icons);
        else if (KEY("HideKeyboardCues"))
            parse_bool(v, &s->hide_keyboard_cues);
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
        }
    } else if (SEC("StartMenu")) {
        if (KEY("Style")) {
            int i = index_of(start_style_names, XD_ARRAY_LEN(start_style_names), v);
            if (i >= 0)
                s->start_style = (xd_start_style)i;
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

static GKeyFile *to_keyfile(const xd_settings *s) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Appearance", "Style", xd_visual_style_name(s->style));
    g_key_file_set_string(kf, "Appearance", "Scheme", s->scheme);
    g_key_file_set_string(kf, "Appearance", "FontSize", font_size_names[s->font_size]);
    g_key_file_set_string(kf, "Appearance", "FontSmoothing", smoothing_names[s->font_smoothing]);
    g_key_file_set_comment(kf, "Appearance", nullptr,
                           " XeonDesktop settings. Written by Display Properties; edits are\n"
                           " picked up immediately.",
                           nullptr);
    g_key_file_set_string(kf, "Desktop", "Wallpaper", s->wallpaper);
    g_key_file_set_string(kf, "Desktop", "WallpaperMode", mode_names[s->wallpaper_mode]);
    g_key_file_set_boolean(kf, "Effects", "MenuTransitions", s->menu_transitions);
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
    g_key_file_set_boolean(kf, "Taskbar", "ShowClock", s->show_clock);
    g_key_file_set_boolean(kf, "Taskbar", "HideInactiveIcons", s->hide_inactive);
    g_key_file_set_boolean(kf, "Taskbar", "AllDisplays", s->all_displays);
    g_key_file_set_string(kf, "Taskbar", "Edge", edge_names[s->edge]);
    g_key_file_set_integer(kf, "Taskbar", "Rows", s->rows);
    g_key_file_set_integer(kf, "Taskbar", "SideWidth", s->side_width);
    g_key_file_set_string(kf, "StartMenu", "Style", start_style_names[s->start_style]);
    g_key_file_set_string(kf, "ScreenSaver", "Name", s->screensaver);
    g_key_file_set_integer(kf, "ScreenSaver", "Wait", s->screensaver_wait);
    g_key_file_set_boolean(kf, "ScreenSaver", "Password", s->screensaver_password);
    g_key_file_set_integer(kf, "ScreenSaver", "MonitorOff", s->monitor_off);
    g_key_file_set_string(kf, "Keyboard", "Layout", s->keyboard.layout);
    g_key_file_set_string(kf, "Keyboard", "Variant", s->keyboard.variant);
    g_key_file_set_string(kf, "Keyboard", "Options", s->keyboard.options);
    g_key_file_set_string(kf, "Keyboard", "Model", s->keyboard.model);
    g_key_file_set_integer(kf, "Keyboard", "RepeatDelay", s->keyboard.repeat_delay);
    g_key_file_set_integer(kf, "Keyboard", "RepeatRate", s->keyboard.repeat_rate);
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
        static const xd_metric scaled[] = {XD_METRIC_CAPTION_HEIGHT,
                                           XD_METRIC_CAPTION_BUTTON_WIDTH, XD_METRIC_MENU_HEIGHT,
                                           XD_METRIC_TASKBAR_HEIGHT};
        for (size_t i = 0; i < XD_ARRAY_LEN(scaled); i++)
            out->metrics[scaled[i]] = (int)lround(out->metrics[scaled[i]] * f);
    }
    out->font_smoothing = s->font_smoothing;
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
