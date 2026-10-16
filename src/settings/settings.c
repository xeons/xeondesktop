/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../core/ini.h"
#include "settings.h"

static const char *const mode_names[XD_WALLPAPER_MODE_COUNT] = {
    [XD_WALLPAPER_CENTER] = "center", [XD_WALLPAPER_TILE] = "tile",
    [XD_WALLPAPER_STRETCH] = "stretch", [XD_WALLPAPER_FILL] = "fill",
    [XD_WALLPAPER_FIT] = "fit",
};

static const char *const font_size_names[] = {"normal", "large", "extralarge"};
static const char *const smoothing_names[] = {"standard", "none", "cleartype"};

const char *xd_wallpaper_mode_name(xd_wallpaper_mode m) {
    XD_ASSERT((unsigned)m < XD_WALLPAPER_MODE_COUNT);
    return mode_names[m];
}

void xd_settings_defaults(xd_settings *s) {
    *s = (xd_settings){
        .style = XD_STYLE_CLASSIC,
        .wallpaper_mode = XD_WALLPAPER_FILL,
        .menu_transitions = true,
        .full_window_drag = true,
        .quick_launch = true,
        .screensaver_wait = 15,
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
        else if (KEY("LargeIcons"))
            parse_bool(v, &s->large_icons);
        else if (KEY("HideKeyboardCues"))
            parse_bool(v, &s->hide_keyboard_cues);
    } else if (SEC("Taskbar")) {
        if (KEY("QuickLaunch"))
            parse_bool(v, &s->quick_launch);
    } else if (SEC("ScreenSaver")) {
        if (KEY("Name")) {
            copy_into(s->screensaver, sizeof(s->screensaver), v);
        } else if (KEY("Wait")) {
            int64_t m;
            if (xd_str_parse_i64(v, 1, 600, &m))
                s->screensaver_wait = (int)m;
        } else if (KEY("Password")) {
            parse_bool(v, &s->screensaver_password);
        }
    }
#undef SEC
#undef KEY
    return true; /* unknown keys are ignored */
}

void xd_settings_parse(xd_str text, xd_settings *s) {
    xd_settings_defaults(s);
    xd_settings tmp = *s;
    if (xd_ini_parse(text, on_entry, &tmp, nullptr))
        *s = tmp;
    /* A syntax error leaves the defaults rather than a half-read file. */
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

static const char *b(bool v) { return v ? "true" : "false"; }

void xd_settings_serialize(const xd_settings *s, xd_strbuf *out) {
    xd_strbuf_appendf(out,
                      "; XeonDesktop settings. Written by Display Properties; edits are\n"
                      "; picked up immediately.\n\n"
                      "[Appearance]\nStyle=%s\nScheme=%s\nFontSize=%s\nFontSmoothing=%s\n\n"
                      "[Desktop]\nWallpaper=%s\nWallpaperMode=%s\n\n"
                      "[Effects]\nMenuTransitions=%s\nFullWindowDrag=%s\nLargeIcons=%s\n"
                      "HideKeyboardCues=%s\n\n"
                      "[Taskbar]\nQuickLaunch=%s\n\n"
                      "[ScreenSaver]\nName=%s\nWait=%d\nPassword=%s\n",
                      xd_visual_style_name(s->style), s->scheme,
                      font_size_names[s->font_size], smoothing_names[s->font_smoothing],
                      s->wallpaper, mode_names[s->wallpaper_mode], b(s->menu_transitions),
                      b(s->full_window_drag), b(s->large_icons), b(s->hide_keyboard_cues),
                      b(s->quick_launch), s->screensaver, s->screensaver_wait, b(s->screensaver_password));
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
    xd_strbuf tmp = {0};
    xd_strbuf text = {0};
    xd_settings_path(&path);
    mkdirs_for(&path);
    xd_strbuf_appendf(&tmp, "%s.tmp", xd_strbuf_cstr(&path));
    xd_settings_serialize(s, &text);

    /* Write then rename, so watchers never see a partial file. */
    FILE *f = fopen(xd_strbuf_cstr(&tmp), "w");
    bool ok = f != nullptr && fwrite(text.data, 1, text.len, f) == text.len;
    if (f != nullptr)
        ok = fclose(f) == 0 && ok;
    ok = ok && rename(xd_strbuf_cstr(&tmp), xd_strbuf_cstr(&path)) == 0;
    if (!ok)
        unlink(xd_strbuf_cstr(&tmp));
    xd_strbuf_free(&text);
    xd_strbuf_free(&tmp);
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
