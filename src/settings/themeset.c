/* SPDX-License-Identifier: MIT */
#include "themeset.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

bool xd_theme_set_has_extras(const xd_pkg_info *info) {
    return info->wallpaper != nullptr || info->sounds != nullptr || info->cursors != nullptr;
}

bool xd_theme_set_cursor_theme(const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, "xdtheme-%s", name);
    /* What [Mouse] CursorTheme takes: a directory name. */
    return n > 0 && (size_t)n < cap && n < 64 && strchr(name, '/') == nullptr;
}

static char *files_dir(const char *data, const char *name) {
    return g_build_filename(data != nullptr ? data : g_get_user_data_dir(), "xeondesktop",
                            "theme-files", name, nullptr);
}

/* Unpacks `member` (a file) of `p` to dir/member. */
static bool unpack_file(const xd_installed_package *p, const char *member, const char *dir,
                        GError **error) {
    char *to = g_build_filename(dir, member, nullptr);
    char *parent = g_path_get_dirname(to);
    bool ok = xd_pkg_extract(p->path, member, parent, error);
    g_free(parent);
    g_free(to);
    return ok;
}

static bool wallpaper_mode(const char *style, xd_wallpaper_mode *out) {
    for (int i = 0; i < XD_WALLPAPER_MODE_COUNT; i++) {
        if (g_ascii_strcasecmp(style, xd_wallpaper_mode_name((xd_wallpaper_mode)i)) == 0) {
            *out = (xd_wallpaper_mode)i;
            return true;
        }
    }
    return false;
}

/* The package's sound scheme with its files' paths (package paths: they
 * have a "/" but don't start with one) made the unpacked files'. */
static bool package_sounds(const xd_installed_package *p, const char *dir, bool unpack,
                           xd_sound_scheme *out, GError **error) {
    GBytes *b = xd_pkg_read_member(p->path, p->info.sounds, error);
    if (b == nullptr)
        return false;
    gsize len = 0;
    const char *text = g_bytes_get_data(b, &len);
    GKeyFile *kf = g_key_file_new();
    bool ok = g_key_file_load_from_data(kf, text != nullptr ? text : "", len, G_KEY_FILE_NONE,
                                        error);
    g_bytes_unref(b);
    char **keys = ok ? g_key_file_get_keys(kf, "Events", nullptr, nullptr) : nullptr;
    for (char **k = keys; ok && k != nullptr && *k != nullptr; k++) {
        char *v = g_key_file_get_string(kf, "Events", *k, nullptr);
        if (v != nullptr && strchr(g_strstrip(v), '/') != nullptr && v[0] != '/' &&
            strstr(v, "..") != nullptr) {
            /* Nothing outside the package: the event keeps the scheme's. */
            g_key_file_remove_key(kf, "Events", *k, nullptr);
        } else if (v != nullptr && strchr(v, '/') != nullptr && v[0] != '/') {
            ok = !unpack || unpack_file(p, v, dir, error);
            char *full = g_build_filename(dir, v, nullptr);
            g_key_file_set_string(kf, "Events", *k, full);
            g_free(full);
        }
        g_free(v);
    }
    g_strfreev(keys);
    if (ok) {
        gsize n = 0;
        char *data = g_key_file_to_data(kf, &n, nullptr);
        xd_sound_scheme_parse((xd_str){data, n}, out);
        g_free(data);
    }
    g_key_file_free(kf);
    return ok;
}

bool xd_theme_set_extras(const xd_installed_package *p, const char *data, bool unpack,
                         xd_settings *s, xd_sound_scheme *sounds, bool *sounds_set,
                         GError **error) {
    const xd_pkg_info *info = &p->info;
    char *dir = files_dir(data, p->name);
    xd_settings next = *s;
    xd_sound_scheme scheme = {0};
    bool ok = true;
    if (info->wallpaper != nullptr) {
        ok = !unpack || unpack_file(p, info->wallpaper, dir, error);
        char *full = g_build_filename(dir, info->wallpaper, nullptr);
        if (strlen(full) < sizeof(next.wallpaper))
            snprintf(next.wallpaper, sizeof(next.wallpaper), "%s", full);
        g_free(full);
        if (info->wallpaper_style == nullptr ||
            !wallpaper_mode(info->wallpaper_style, &next.wallpaper_mode))
            next.wallpaper_mode = XD_WALLPAPER_STRETCH;
    }
    if (ok && info->cursors != nullptr) {
        char theme[64];
        if (xd_theme_set_cursor_theme(p->name, theme, sizeof(theme))) {
            char *to = g_build_filename(data != nullptr ? data : g_get_user_data_dir(), "icons",
                                        theme, nullptr);
            ok = !unpack || xd_pkg_extract(p->path, info->cursors, to, error);
            g_free(to);
            snprintf(next.mouse.cursor_theme, sizeof(next.mouse.cursor_theme), "%s", theme);
        }
    }
    bool has_sounds = ok && info->sounds != nullptr;
    if (has_sounds)
        ok = package_sounds(p, dir, unpack, &scheme, error);
    g_free(dir);
    if (!ok) {
        if (has_sounds)
            xd_sound_scheme_free(&scheme);
        return false;
    }
    *s = next;
    *sounds_set = has_sounds;
    if (has_sounds) {
        xd_sound_scheme_free(sounds);
        *sounds = scheme;
    }
    return true;
}

static bool same_sounds(const xd_sound_scheme *a, const xd_sound_scheme *b) {
    xd_strbuf x = {0}, y = {0};
    xd_sound_scheme_serialize(a, &x);
    xd_sound_scheme_serialize(b, &y);
    bool same = xd_str_eq(xd_strbuf_view(&x), xd_strbuf_view(&y));
    xd_strbuf_free(&x);
    xd_strbuf_free(&y);
    return same;
}

bool xd_theme_set_matches(const xd_installed_package *p, const char *data, const xd_settings *s,
                          const xd_sound_scheme *sounds) {
    if (strcmp(s->theme_package, p->name) != 0)
        return false;
    xd_settings want = *s;
    xd_sound_scheme scheme;
    xd_sound_scheme_init(&scheme, XD_SOUNDS_DEFAULT);
    bool sounds_set = false;
    bool same = xd_theme_set_extras(p, data, false, &want, &scheme, &sounds_set, nullptr) &&
                strcmp(want.wallpaper, s->wallpaper) == 0 &&
                want.wallpaper_mode == s->wallpaper_mode &&
                strcmp(want.mouse.cursor_theme, s->mouse.cursor_theme) == 0 &&
                (!sounds_set || same_sounds(&scheme, sounds));
    xd_sound_scheme_free(&scheme);
    return same;
}

/* ---- Save As... ---------------------------------------------------------------- */

typedef struct variants_ctx {
    const char *current;
    GString *first;  /* the current variant's element */
    GString *others; /* the rest, and <scaling> */
} variants_ctx;

/* The source theme.xml's variants and scaling, written again. */
static void variants_start(G_GNUC_UNUSED GMarkupParseContext *mc, const char *element,
                           const char **names, const char **values, gpointer user,
                           G_GNUC_UNUSED GError **error) {
    variants_ctx *c = user;
    if (strcmp(element, "variant") != 0 && strcmp(element, "scaling") != 0)
        return;
    bool current = false;
    GString *out = g_string_new("  <");
    g_string_append(out, element);
    for (size_t i = 0; names[i] != nullptr; i++) {
        char *v = g_markup_escape_text(values[i], -1);
        g_string_append_printf(out, " %s=\"%s\"", names[i], v);
        g_free(v);
        current |= strcmp(element, "variant") == 0 && strcmp(names[i], "id") == 0 &&
                   c->current != nullptr && strcmp(values[i], c->current) == 0;
    }
    g_string_append(out, "/>\n");
    g_string_append(current && c->first->len == 0 ? c->first : c->others, out->str);
    g_string_free(out, TRUE);
}

static bool add_bytes(GHashTable *files, const char *member, GBytes *b) {
    g_hash_table_insert(files, g_strdup(member), b);
    return true;
}

/* A file from disk into the package. */
static bool add_file(GHashTable *files, const char *member, const char *path) {
    char *data = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, nullptr))
        return false;
    return add_bytes(files, member, g_bytes_new_take(data, len));
}

/* `want` in the package, or with " (2)"... before its extension if taken. */
static char *free_member(GHashTable *files, const char *folder, const char *want) {
    char *member = g_strconcat(folder, "/", want, nullptr);
    const char *dot = strrchr(want, '.');
    size_t stem = dot != nullptr && dot != want ? (size_t)(dot - want) : strlen(want);
    for (int n = 2; g_hash_table_contains(files, member) && n < 1000; n++) {
        g_free(member);
        member = g_strdup_printf("%s/%.*s (%d)%s", folder, (int)stem, want, n, want + stem);
    }
    return member;
}

static void save_sounds(GHashTable *files, const xd_sound_scheme *sounds) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Sounds", "Scheme", sounds->base == XD_SOUNDS_NONE ? "none" : "default");
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++) {
        const char *v = sounds->sounds[i] != nullptr ? sounds->sounds[i] : "";
        char *value = g_strdup(v);
        if (xd_sound_kind_of(v) == XD_SOUND_FILE) {
            /* Copied in, unless it can't be read: then where it is. */
            char *base = g_path_get_basename(v);
            char *member = free_member(files, "sounds", base);
            if (add_file(files, member, v)) {
                g_free(value);
                value = g_strdup(member);
            }
            g_free(member);
            g_free(base);
        }
        g_key_file_set_string(kf, "Events", xd_sound_event_get((xd_sound_event)i)->key, value);
        g_free(value);
    }
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, nullptr);
    add_bytes(files, "sounds/scheme.ini", g_bytes_new_take(data, len));
    g_key_file_free(kf);
}

static char **default_icon_dirs(void) {
    GPtrArray *dirs = g_ptr_array_new();
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "icons", nullptr));
    g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".icons", nullptr));
    for (const char *const *d = g_get_system_data_dirs(); *d != nullptr; d++)
        g_ptr_array_add(dirs, g_build_filename(*d, "icons", nullptr));
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

/* The XCursor theme `theme` (its cursors/ folder and index.theme), from
 * the first of `dirs` that has it, into cursors/. Links are followed:
 * a zip holds files. */
static bool save_cursors(GHashTable *files, const char *theme, const char *const *dirs) {
    for (; dirs != nullptr && *dirs != nullptr; dirs++) {
        char *root = g_build_filename(*dirs, theme, nullptr);
        char *cursors = g_build_filename(root, "cursors", nullptr);
        GDir *d = g_dir_open(cursors, 0, nullptr);
        bool any = false;
        const char *e;
        while (d != nullptr && (e = g_dir_read_name(d)) != nullptr) {
            char *path = g_build_filename(cursors, e, nullptr);
            char *member = g_strconcat("cursors/cursors/", e, nullptr);
            if (e[0] != '.' && g_file_test(path, G_FILE_TEST_IS_REGULAR))
                any |= add_file(files, member, path);
            g_free(member);
            g_free(path);
        }
        if (d != nullptr)
            g_dir_close(d);
        if (any) {
            char *index = g_build_filename(root, "index.theme", nullptr);
            (void)add_file(files, "cursors/index.theme", index);
            g_free(index);
        }
        g_free(cursors);
        g_free(root);
        if (any)
            return true;
    }
    return false;
}

/* Package paths of what a theme brings: the source package's own are
 * replaced by the current settings'. */
static gboolean is_extra(gpointer key, [[maybe_unused]] gpointer value,
                         [[maybe_unused]] gpointer user) {
    const char *m = key;
    return strcmp(m, "theme.xml") == 0 || g_str_has_prefix(m, "wallpaper/") ||
           g_str_has_prefix(m, "sounds/") || g_str_has_prefix(m, "cursors/");
}

bool xd_theme_set_save(const char *dest, const char *name, const xd_settings *s,
                       const xd_sound_scheme *sounds, const char *package, const char *variant,
                       const char *const *icon_dirs, GError **error) {
    GHashTable *files = nullptr;
    variants_ctx vc = {.current = variant, .first = g_string_new(""), .others = g_string_new("")};
    bool ok = true;
    if (package != nullptr) {
        files = xd_pkg_read_members(package, error);
        GBytes *xml = files != nullptr ? g_hash_table_lookup(files, "theme.xml") : nullptr;
        ok = xml != nullptr;
        if (ok) {
            static const GMarkupParser parser = {.start_element = variants_start};
            gsize len = 0;
            const char *text = g_bytes_get_data(xml, &len);
            GMarkupParseContext *mc = g_markup_parse_context_new(&parser, 0, &vc, nullptr);
            ok = g_markup_parse_context_parse(mc, text, (gssize)len, error) &&
                 g_markup_parse_context_end_parse(mc, error);
            g_markup_parse_context_free(mc);
        } else if (files != nullptr) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "%s: no theme.xml", package);
        }
        if (files != nullptr)
            g_hash_table_foreach_remove(files, is_extra, nullptr);
    } else {
        /* Everything left to the style. */
        files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify)g_bytes_unref);
        add_bytes(files, "styles/default.xml", g_bytes_new_static("<style/>\n", 9));
        g_string_append(vc.first, "  <variant id=\"default\" name=\"Default\" size=\"Normal\" "
                                  "style=\"styles/default.xml\"/>\n");
    }
    if (!ok) {
        if (files != nullptr)
            g_hash_table_unref(files);
        g_string_free(vc.first, TRUE);
        g_string_free(vc.others, TRUE);
        return false;
    }
    GString *xml = g_string_new("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    char *head = g_markup_printf_escaped("<xdtheme version=\"1\" name=\"%s\" base=\"%s\">\n"
                                         "  <description>Saved in Display Properties</description>\n",
                                         name, xd_visual_style_name(s->style));
    g_string_append(xml, head);
    g_free(head);
    g_string_append(xml, vc.first->str);
    g_string_append(xml, vc.others->str);
    if (s->wallpaper[0] != '\0') {
        char *base = g_path_get_basename(s->wallpaper);
        char *member = free_member(files, "wallpaper", base);
        if (add_file(files, member, s->wallpaper)) {
            char *el = g_markup_printf_escaped("  <wallpaper file=\"%s\" style=\"%s\"/>\n", member,
                                               xd_wallpaper_mode_name(s->wallpaper_mode));
            g_string_append(xml, el);
            g_free(el);
        }
        g_free(member);
        g_free(base);
    }
    save_sounds(files, sounds);
    g_string_append(xml, "  <sounds scheme=\"sounds/scheme.ini\"/>\n");
    if (s->mouse.cursor_theme[0] != '\0') {
        char **dirs = icon_dirs == nullptr ? default_icon_dirs() : nullptr;
        if (save_cursors(files, s->mouse.cursor_theme,
                         icon_dirs != nullptr ? icon_dirs : (const char *const *)dirs))
            g_string_append(xml, "  <cursors theme=\"cursors\"/>\n");
        g_strfreev(dirs);
    }
    g_string_append(xml, "</xdtheme>\n");
    gsize len = xml->len;
    add_bytes(files, "theme.xml", g_bytes_new_take(g_string_free(xml, FALSE), len));
    ok = xd_pkg_write_zip(dest, files, error);
    g_hash_table_unref(files);
    g_string_free(vc.first, TRUE);
    g_string_free(vc.others, TRUE);
    return ok;
}
