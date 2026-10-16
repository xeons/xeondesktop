/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../core/ini.h"
#include "apps.h"

/* Windows-style Programs folders for the freedesktop main categories,
 * checked in this order (an entry often lists several). */
static const struct {
    const char *category;
    const char *folder;
    const char *icon;
} folders[] = {
    {"Game", "Games", "applications-games"},
    {"Settings", "Settings", "preferences-desktop"},
    {"Development", "Programming", "applications-development"},
    {"Education", "Education", "applications-education"},
    {"Science", "Education", "applications-science"},
    {"Graphics", "Graphics", "applications-graphics"},
    {"AudioVideo", "Multimedia", "applications-multimedia"},
    {"Audio", "Multimedia", "applications-multimedia"},
    {"Video", "Multimedia", "applications-multimedia"},
    {"Network", "Internet", "applications-internet"},
    {"Office", "Office", "applications-office"},
    {"System", "System Tools", "applications-system"},
    {"Utility", "Accessories", "applications-accessories"},
};

typedef struct entry_fields {
    xd_str type, exec, icon, try_exec, categories, only_show_in, not_show_in;
    xd_str name;
    int name_rank; /* 0 none, 1 Name, 2 Name[lang], 3 Name[lang_COUNTRY] */
    bool no_display, hidden, terminal;
    bool in_main_section, past_main_section;
    xd_str lang, lang_country;
} entry_fields;

static bool is_true(xd_str v) { return xd_str_eq(v, XD_STR("true")); }

/* Does a ';'-separated list contain `item`? */
static bool list_contains(xd_str list, xd_str item) {
    while (list.len > 0) {
        size_t semi = list.len;
        bool more = xd_str_find_char(list, ';', &semi);
        if (xd_str_eq(xd_str_trim(xd_str_sub(list, 0, semi)), item))
            return true;
        list = more ? xd_str_sub(list, semi + 1, list.len) : XD_STR("");
    }
    return false;
}

static bool on_key(void *user, xd_str section, xd_str key, xd_str value) {
    entry_fields *f = user;
    if (!xd_str_eq(section, XD_STR("Desktop Entry"))) {
        f->past_main_section = f->in_main_section || f->past_main_section;
        f->in_main_section = false;
        return true; /* [Desktop Action ...] etc. are ignored */
    }
    f->in_main_section = true;

    if (xd_str_eq(key, XD_STR("Type")))
        f->type = value;
    else if (xd_str_eq(key, XD_STR("Exec")))
        f->exec = value;
    else if (xd_str_eq(key, XD_STR("Icon")))
        f->icon = value;
    else if (xd_str_eq(key, XD_STR("TryExec")))
        f->try_exec = value;
    else if (xd_str_eq(key, XD_STR("Categories")))
        f->categories = value;
    else if (xd_str_eq(key, XD_STR("OnlyShowIn")))
        f->only_show_in = value;
    else if (xd_str_eq(key, XD_STR("NotShowIn")))
        f->not_show_in = value;
    else if (xd_str_eq(key, XD_STR("NoDisplay")))
        f->no_display = is_true(value);
    else if (xd_str_eq(key, XD_STR("Hidden")))
        f->hidden = is_true(value);
    else if (xd_str_eq(key, XD_STR("Terminal")))
        f->terminal = is_true(value);
    else if (xd_str_starts_with(key, XD_STR("Name"))) {
        int rank = 0;
        if (xd_str_eq(key, XD_STR("Name")))
            rank = 1;
        else if (key.len > 6 && key.ptr[4] == '[' && key.ptr[key.len - 1] == ']') {
            xd_str loc = xd_str_sub(key, 5, key.len - 6);
            if (f->lang_country.len > 0 && xd_str_eq(loc, f->lang_country))
                rank = 3;
            else if (f->lang.len > 0 && xd_str_eq(loc, f->lang))
                rank = 2;
        }
        if (rank > f->name_rank) {
            f->name = value;
            f->name_rank = rank;
        }
    }
    return true;
}

/* LC_MESSAGES/LANG "en_US.UTF-8@euro" -> lang "en", lang_country "en_US". */
static void locale_parts(xd_str *lang, xd_str *lang_country) {
    const char *env = getenv("LC_ALL");
    if (env == nullptr || env[0] == '\0')
        env = getenv("LC_MESSAGES");
    if (env == nullptr || env[0] == '\0')
        env = getenv("LANG");
    xd_str s = xd_str_from_cstr(env);
    size_t cut = s.len;
    for (size_t i = 0; i < s.len; i++) {
        if (s.ptr[i] == '.' || s.ptr[i] == '@') {
            cut = i;
            break;
        }
    }
    *lang_country = xd_str_sub(s, 0, cut);
    size_t us;
    *lang = xd_str_find_char(*lang_country, '_', &us) ? xd_str_sub(*lang_country, 0, us)
                                                       : *lang_country;
}

static bool in_path(xd_arena *arena, xd_str prog) {
    if (prog.len == 0)
        return false;
    if (prog.ptr[0] == '/')
        return access(xd_str_arena_cstr(arena, prog), X_OK) == 0;
    const char *path_env = getenv("PATH");
    xd_str rest = xd_str_from_cstr(path_env != nullptr ? path_env : "/usr/bin:/bin");
    xd_strbuf buf = {0};
    bool found = false;
    while (rest.len > 0 && !found) {
        size_t colon = rest.len;
        bool more = xd_str_find_char(rest, ':', &colon);
        xd_strbuf_clear(&buf);
        xd_strbuf_append(&buf, xd_str_sub(rest, 0, colon));
        xd_strbuf_append_char(&buf, '/');
        xd_strbuf_append(&buf, prog);
        found = access(xd_strbuf_cstr(&buf), X_OK) == 0;
        rest = more ? xd_str_sub(rest, colon + 1, rest.len) : XD_STR("");
    }
    xd_strbuf_free(&buf);
    return found;
}

char *xd_app_expand_exec(xd_arena *arena, xd_str exec, const char *name, const char *icon) {
    xd_strbuf out = {0};
    for (size_t i = 0; i < exec.len; i++) {
        char c = exec.ptr[i];
        if (c != '%' || i + 1 == exec.len) {
            xd_strbuf_append_char(&out, c);
            continue;
        }
        char code = exec.ptr[++i];
        switch (code) {
        case '%':
            xd_strbuf_append_char(&out, '%');
            break;
        case 'c':
            /* Quoted for the shell; names can contain spaces or quotes. */
            xd_strbuf_append_char(&out, '\'');
            for (const char *p = name != nullptr ? name : ""; *p != '\0'; p++) {
                if (*p == '\'')
                    xd_strbuf_append(&out, XD_STR("'\\''"));
                else
                    xd_strbuf_append_char(&out, *p);
            }
            xd_strbuf_append_char(&out, '\'');
            break;
        case 'i':
            if (icon != nullptr && icon[0] != '\0' && strchr(icon, '\'') == nullptr)
                xd_strbuf_appendf(&out, "--icon '%s'", icon);
            break;
        default:
            /* %f %F %u %U (no files yet) and deprecated codes: dropped. */
            break;
        }
    }
    char *s = xd_str_arena_cstr(arena, xd_str_trim(xd_strbuf_view(&out)));
    xd_strbuf_free(&out);
    return s;
}

bool xd_app_parse(xd_str text, xd_str id, const char *desktop_name, xd_arena *arena,
                  xd_app *out) {
    entry_fields f = {0};
    locale_parts(&f.lang, &f.lang_country);
    if (!xd_ini_parse(text, on_key, &f, nullptr))
        return false;
    if (!xd_str_eq(f.type, XD_STR("Application")) || f.no_display || f.hidden ||
        f.exec.len == 0 || f.name.len == 0)
        return false;

    xd_str desktop = xd_str_from_cstr(desktop_name);
    if (f.only_show_in.len > 0 && !list_contains(f.only_show_in, desktop))
        return false;
    if (f.not_show_in.len > 0 && list_contains(f.not_show_in, desktop))
        return false;
    if (f.try_exec.len > 0 && !in_path(arena, f.try_exec))
        return false;

    const char *folder = "Other";
    const char *folder_icon = "applications-other";
    for (size_t i = 0; i < XD_ARRAY_LEN(folders); i++) {
        if (list_contains(f.categories, xd_str_from_cstr(folders[i].category))) {
            folder = folders[i].folder;
            folder_icon = folders[i].icon;
            break;
        }
    }

    const char *name = xd_str_arena_cstr(arena, f.name);
    const char *icon = f.icon.len > 0 ? xd_str_arena_cstr(arena, f.icon) : nullptr;
    *out = (xd_app){
        .id = xd_str_arena_cstr(arena, id),
        .name = name,
        .icon = icon,
        .exec = xd_app_expand_exec(arena, f.exec, name, icon),
        .folder = folder,
        .folder_icon = folder_icon,
        .terminal = f.terminal,
    };
    return out->exec[0] != '\0';
}

static bool seen(const xd_app_list *list, xd_str id) {
    for (size_t i = 0; i < list->count; i++) {
        if (xd_str_eq(xd_str_from_cstr(list->items[i].id), id))
            return true;
    }
    return false;
}

static bool id_shadowed(xd_str *shadow, size_t n, xd_str id) {
    for (size_t i = 0; i < n; i++) {
        if (xd_str_eq(shadow[i], id))
            return true;
    }
    return false;
}

/* Scans one applications directory (and subdirectories, whose names become
 * part of the id: kde/foo.desktop -> kde-foo.desktop). Directories earlier
 * in the XDG search order win, so ids already seen are skipped. */
static void scan_apps(xd_app_list *list, xd_strbuf *dir, xd_strbuf *prefix,
                      const char *desktop_name, xd_str **shadow, size_t *shadow_n,
                      size_t *shadow_cap, int depth) {
    DIR *d = opendir(xd_strbuf_cstr(dir));
    if (d == nullptr)
        return;
    size_t dir_len = dir->len;
    size_t prefix_len = prefix->len;
    xd_strbuf text = {0};
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        xd_str name = xd_str_from_cstr(de->d_name);
        if (name.len == 0 || name.ptr[0] == '.')
            continue;
        dir->len = dir_len;
        xd_strbuf_append_char(dir, '/');
        xd_strbuf_append(dir, name);
        prefix->len = prefix_len;
        xd_strbuf_append(prefix, name);

        if (de->d_type == DT_DIR && depth < 2) {
            xd_strbuf_append_char(prefix, '-');
            scan_apps(list, dir, prefix, desktop_name, shadow, shadow_n, shadow_cap,
                      depth + 1);
            continue;
        }
        size_t n = name.len;
        if (n < 9 || !xd_str_eq(xd_str_sub(name, n - 8, 8), XD_STR(".desktop")))
            continue;
        xd_str id = xd_strbuf_view(prefix);
        if (id_shadowed(*shadow, *shadow_n, id) || seen(list, id))
            continue;
        /* Remember the id even if hidden: a user Hidden=true entry must
         * hide the system one. */
        if (*shadow_n == *shadow_cap) {
            *shadow_cap = *shadow_cap == 0 ? 128 : *shadow_cap * 2;
            *shadow = xd_realloc_array(*shadow, *shadow_cap, sizeof(**shadow));
        }
        (*shadow)[(*shadow_n)++] = (xd_str){xd_str_arena_cstr(&list->arena, id), id.len};

        if (!xd_read_file(xd_strbuf_cstr(dir), 256 * 1024, &text))
            continue;
        xd_app app;
        if (!xd_app_parse(xd_strbuf_view(&text), id, desktop_name, &list->arena, &app))
            continue;
        if (list->count == list->cap) {
            list->cap = list->cap == 0 ? 64 : list->cap * 2;
            list->items = xd_realloc_array(list->items, list->cap, sizeof(*list->items));
        }
        list->items[list->count++] = app;
    }
    dir->len = dir_len;
    xd_strbuf_cstr(dir);
    prefix->len = prefix_len;
    xd_strbuf_cstr(prefix);
    xd_strbuf_free(&text);
    closedir(d);
}

static int compare_apps(const void *a, const void *b) {
    const xd_app *x = a;
    const xd_app *y = b;
    int c = strcoll(x->folder, y->folder);
    return c != 0 ? c : strcoll(x->name, y->name);
}

void xd_apps_load(xd_app_list *list, const char *desktop_name) {
    *list = (xd_app_list){0};
    xd_arena_init(&list->arena, 64 * 1024);

    xd_strbuf dirs[8] = {0};
    size_t ndirs = 0;
    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    if (data_home != nullptr && data_home[0] != '\0')
        xd_strbuf_appendf(&dirs[ndirs++], "%s/applications", data_home);
    else if (home != nullptr)
        xd_strbuf_appendf(&dirs[ndirs++], "%s/.local/share/applications", home);
    const char *data_dirs = getenv("XDG_DATA_DIRS");
    xd_str rest = xd_str_from_cstr(data_dirs != nullptr && data_dirs[0] != '\0'
                                       ? data_dirs
                                       : "/usr/local/share:/usr/share");
    while (rest.len > 0 && ndirs < XD_ARRAY_LEN(dirs)) {
        size_t colon = rest.len;
        bool more = xd_str_find_char(rest, ':', &colon);
        xd_str dir = xd_str_sub(rest, 0, colon);
        if (dir.len > 0)
            xd_strbuf_appendf(&dirs[ndirs++], "%.*s/applications", (int)dir.len, dir.ptr);
        rest = more ? xd_str_sub(rest, colon + 1, rest.len) : XD_STR("");
    }

    xd_str *shadow = nullptr;
    size_t shadow_n = 0;
    size_t shadow_cap = 0;
    xd_strbuf prefix = {0};
    for (size_t i = 0; i < ndirs; i++) {
        xd_strbuf_clear(&prefix);
        scan_apps(list, &dirs[i], &prefix, desktop_name, &shadow, &shadow_n, &shadow_cap, 0);
        xd_strbuf_free(&dirs[i]);
    }
    xd_strbuf_free(&prefix);
    xd_free(shadow);

    if (list->count > 1)
        qsort(list->items, list->count, sizeof(*list->items), compare_apps);
}

void xd_apps_free(xd_app_list *list) {
    xd_free(list->items);
    xd_arena_destroy(&list->arena);
    *list = (xd_app_list){0};
}
