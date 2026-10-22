/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "filetypes.h"

/* ---- reading a type's XML ----------------------------------------------------------- */

typedef struct type_reader {
    const char *want; /* the type, or nullptr for the first <mime-type> */
    xd_mime_package *out;
    int depth;      /* the current element's */
    int type_depth; /* the wanted <mime-type>'s while inside it, else 0 */
    bool found;
    GString *comment; /* while inside its untranslated <comment> */
} type_reader;

static void glob_free(gpointer data) {
    xd_mime_glob *mime_glob = data;
    g_free(mime_glob->pattern);
    g_free(mime_glob);
}

static const char *attr(const char **names, const char **values, const char *name) {
    for (size_t i = 0; names[i] != nullptr; i++)
        if (strcmp(names[i], name) == 0)
            return values[i];
    return nullptr;
}

static void on_start([[maybe_unused]] GMarkupParseContext *ctx, const char *element,
                     const char **names, const char **values, gpointer data,
                     [[maybe_unused]] GError **error) {
    type_reader *reader = data;
    reader->depth++;
    if (reader->type_depth == 0) {
        const char *type = attr(names, values, "type");
        if (!reader->found && strcmp(element, "mime-type") == 0 &&
            (reader->want == nullptr || g_strcmp0(type, reader->want) == 0)) {
            reader->type_depth = reader->depth;
            reader->found = true;
            g_free(reader->out->type);
            reader->out->type = g_strdup(type);
        }
        return;
    }
    /* Only the type's own elements, not ones nested in them. */
    if (reader->depth != reader->type_depth + 1)
        return;
    if (strcmp(element, "glob-deleteall") == 0) {
        reader->out->delete_all = true;
        /* What came before it in the same file is gone too. */
        g_ptr_array_set_size(reader->out->globs, 0);
    } else if (strcmp(element, "glob") == 0) {
        const char *pattern = attr(names, values, "pattern");
        const char *weight = attr(names, values, "weight");
        const char *cs = attr(names, values, "case-sensitive");
        if (pattern == nullptr || pattern[0] == '\0')
            return;
        xd_mime_glob *mime_glob = g_new0(xd_mime_glob, 1);
        mime_glob->pattern = g_strdup(pattern);
        mime_glob->weight =
            weight != nullptr ? (int)CLAMP(g_ascii_strtoll(weight, nullptr, 10), 0, 100) : 0;
        mime_glob->case_sensitive = cs != nullptr && strcmp(cs, "true") == 0;
        g_ptr_array_add(reader->out->globs, mime_glob);
    } else if (strcmp(element, "comment") == 0 && attr(names, values, "xml:lang") == nullptr &&
               reader->out->comment == nullptr) {
        reader->comment = g_string_new(nullptr);
    }
}

static void on_text([[maybe_unused]] GMarkupParseContext *ctx, const char *text, gsize len,
                    gpointer data, [[maybe_unused]] GError **error) {
    type_reader *reader = data;
    if (reader->comment != nullptr)
        g_string_append_len(reader->comment, text, (gssize)len);
}

static void on_end([[maybe_unused]] GMarkupParseContext *ctx,
                   [[maybe_unused]] const char *element, gpointer data,
                   [[maybe_unused]] GError **error) {
    type_reader *reader = data;
    if (reader->comment != nullptr && reader->depth == reader->type_depth + 1) {
        reader->out->comment = g_string_free(reader->comment, FALSE);
        reader->comment = nullptr;
    }
    if (reader->depth == reader->type_depth)
        reader->type_depth = 0;
    reader->depth--;
}

static bool read_type(const char *xml, gssize len, const char *type, xd_mime_package *out) {
    static const GMarkupParser parser = {
        .start_element = on_start, .end_element = on_end, .text = on_text};
    *out = (xd_mime_package){.globs = g_ptr_array_new_with_free_func(glob_free)};
    type_reader reader = {.want = type, .out = out};
    GMarkupParseContext *ctx =
        g_markup_parse_context_new(&parser, (GMarkupParseFlags)0, &reader, nullptr);
    bool ok = g_markup_parse_context_parse(ctx, xml, len, nullptr) &&
              g_markup_parse_context_end_parse(ctx, nullptr);
    g_markup_parse_context_free(ctx);
    if (reader.comment != nullptr)
        g_string_free(reader.comment, TRUE);
    return ok && reader.found;
}

bool xd_mime_package_parse(const char *xml, gssize len, const char *type, xd_mime_package *out) {
    return read_type(xml, len, type, out);
}

void xd_mime_package_clear(xd_mime_package *pkg) {
    g_free(pkg->type);
    g_free(pkg->comment);
    if (pkg->globs != nullptr)
        g_ptr_array_unref(pkg->globs);
    *pkg = (xd_mime_package){0};
}

/* "*.png" is an extension; "*.[ch]", "README*" and "*.tar.*" aren't. */
static char *glob_extension(const char *pattern, gboolean case_sensitive) {
    if (!g_str_has_prefix(pattern, "*.") || pattern[2] == '\0')
        return nullptr;
    const char *ext = pattern + 2;
    if (strpbrk(ext, "*?[]/\\") != nullptr || !g_utf8_validate(ext, -1, nullptr))
        return nullptr;
    return case_sensitive ? g_strdup(ext) : g_utf8_strdown(ext, -1);
}

static gboolean has_extension(GPtrArray *out, const char *ext) {
    for (guint i = 0; i < out->len; i++)
        if (strcmp(g_ptr_array_index(out, i), ext) == 0)
            return TRUE;
    return FALSE;
}

bool xd_mime_xml_extensions(const char *xml, gssize len, GPtrArray *out) {
    xd_mime_package pkg;
    read_type(xml, len, nullptr, &pkg);
    for (guint i = 0; i < pkg.globs->len; i++) {
        const xd_mime_glob *mime_glob = g_ptr_array_index(pkg.globs, i);
        char *ext = glob_extension(mime_glob->pattern, mime_glob->case_sensitive);
        if (ext != nullptr && !has_extension(out, ext))
            g_ptr_array_add(out, ext);
        else
            g_free(ext);
    }
    bool delete_all = pkg.delete_all;
    xd_mime_package_clear(&pkg);
    return delete_all;
}

/* MIME types are "media/subtype": nothing that could leave the database
 * directory. */
static bool safe_type(const char *type) {
    const char *slash = strchr(type, '/');
    return type[0] != '\0' && type[0] != '/' && strstr(type, "..") == nullptr &&
           slash != nullptr && slash[1] != '\0' && strchr(slash + 1, '/') == nullptr;
}

/* The per-type file update-mime-database wrote in `data_dir`. */
static char *type_xml(const char *data_dir, const char *type, gsize *len) {
    char *file = g_strconcat(type, ".xml", nullptr);
    char *path = g_build_filename(data_dir, "mime", file, nullptr);
    char *xml = nullptr;
    if (!g_file_get_contents(path, &xml, len, nullptr) || *len >= (1u << 20))
        g_clear_pointer(&xml, g_free);
    g_free(path);
    g_free(file);
    return xml;
}

char **xd_mime_data_dirs(void) {
    GPtrArray *dirs = g_ptr_array_new();
    g_ptr_array_add(dirs, g_strdup(g_get_user_data_dir()));
    for (const char *const *data_dir = g_get_system_data_dirs(); *data_dir != nullptr; data_dir++)
        g_ptr_array_add(dirs, g_strdup(*data_dir));
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

char **xd_content_type_extensions(const char *type) {
    GPtrArray *out = g_ptr_array_new();
    if (safe_type(type)) {
        char **dirs = xd_mime_data_dirs();
        /* A database that deletes the type's globs hides the ones below. */
        for (char **dir = dirs; *dir != nullptr; dir++) {
            gsize len = 0;
            char *xml = type_xml(*dir, type, &len);
            bool delete_all = xml != nullptr && xd_mime_xml_extensions(xml, (gssize)len, out);
            g_free(xml);
            if (delete_all)
                break;
        }
        g_strfreev(dirs);
    }
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

GPtrArray *xd_mime_type_globs(const char *const *data_dirs, const char *type) {
    GPtrArray *out = g_ptr_array_new_with_free_func(glob_free);
    for (const char *const *data_dir = data_dirs; safe_type(type) && *data_dir != nullptr;
         data_dir++) {
        gsize len = 0;
        char *xml = type_xml(*data_dir, type, &len);
        if (xml == nullptr)
            continue;
        xd_mime_package pkg;
        read_type(xml, (gssize)len, type, &pkg);
        g_free(xml);
        for (guint i = 0; i < pkg.globs->len; i++) {
            const xd_mime_glob *glob = g_ptr_array_index(pkg.globs, i);
            bool dup = false;
            for (guint j = 0; j < out->len && !dup; j++) {
                const xd_mime_glob *existing = g_ptr_array_index(out, j);
                dup = strcmp(existing->pattern, glob->pattern) == 0 &&
                      existing->case_sensitive == glob->case_sensitive;
            }
            if (!dup) {
                xd_mime_glob *copy = g_new(xd_mime_glob, 1);
                *copy = *glob;
                copy->pattern = g_strdup(glob->pattern);
                g_ptr_array_add(out, copy);
            }
        }
        bool delete_all = pkg.delete_all;
        xd_mime_package_clear(&pkg);
        if (delete_all)
            break;
    }
    return out;
}

/* ---- the user's changes ---------------------------------------------------------- */

bool xd_mime_extension_valid(const char *ext) {
    if (ext == nullptr || ext[0] == '\0' || strlen(ext) > 32 || !g_utf8_validate(ext, -1, nullptr))
        return false;
    for (const char *cursor = ext; *cursor != '\0'; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        /* A dot would make "*.tar.xyz"-like globs; the rest are glob or
         * type-name syntax. */
        if (!g_unichar_isalnum(character) && character != '-' && character != '_' &&
            character != '+' && character != '~')
            return false;
    }
    return true;
}

char *xd_mime_type_for_new_extension(const char *ext) {
    char *lower = g_utf8_strdown(ext, -1);
    char *type = g_strconcat("application/x-extension-", lower, nullptr);
    g_free(lower);
    return type;
}

static char *package_path(const char *data_dir, const char *type) {
    char *name = g_strdelimit(g_strconcat("xeondesktop-", type, ".xml", nullptr), "/", '-');
    char *path = g_build_filename(data_dir, "mime", "packages", name, nullptr);
    g_free(name);
    return path;
}

char *xd_mime_package_to_xml(const xd_mime_package *pkg) {
    GString *xml = g_string_new("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                "<!-- Folder Options, File Types -->\n"
                                "<mime-info xmlns=\"http://www.freedesktop.org/standards/"
                                "shared-mime-info\">\n");
    char *escaped = g_markup_printf_escaped("  <mime-type type=\"%s\">\n", pkg->type);
    g_string_append(xml, escaped);
    g_free(escaped);
    if (pkg->comment != nullptr) {
        escaped = g_markup_printf_escaped("    <comment>%s</comment>\n", pkg->comment);
        g_string_append(xml, escaped);
        g_free(escaped);
    }
    if (pkg->delete_all)
        g_string_append(xml, "    <glob-deleteall/>\n");
    for (guint i = 0; pkg->globs != nullptr && i < pkg->globs->len; i++) {
        const xd_mime_glob *mime_glob = g_ptr_array_index(pkg->globs, i);
        escaped = g_markup_printf_escaped("    <glob pattern=\"%s\"", mime_glob->pattern);
        g_string_append(xml, escaped);
        g_free(escaped);
        if (mime_glob->weight > 0)
            g_string_append_printf(xml, " weight=\"%d\"", mime_glob->weight);
        if (mime_glob->case_sensitive)
            g_string_append(xml, " case-sensitive=\"true\"");
        g_string_append(xml, "/>\n");
    }
    g_string_append(xml, "  </mime-type>\n</mime-info>\n");
    return g_string_free(xml, FALSE);
}

/* Our package file for `type`, or an empty one. */
static void load_package(const char *path, const char *type, xd_mime_package *pkg) {
    char *xml = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path, &xml, &len, nullptr) ||
        !read_type(xml, (gssize)len, type, pkg)) {
        if (xml != nullptr)
            xd_mime_package_clear(pkg);
        *pkg = (xd_mime_package){.globs = g_ptr_array_new_with_free_func(glob_free)};
    }
    g_free(xml);
    g_free(pkg->type);
    pkg->type = g_strdup(type);
}

static bool save_package(const char *path, const xd_mime_package *pkg, GError **error) {
    char *dir = g_path_get_dirname(path);
    bool ok = g_mkdir_with_parents(dir, 0755) == 0;
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create %s", dir);
    } else {
        char *xml = xd_mime_package_to_xml(pkg);
        ok = g_file_set_contents(path, xml, -1, error);
        g_free(xml);
    }
    g_free(dir);
    return ok;
}

/* Extensions are listed in capitals, as in XP: deleting one deletes
 * every case of it the type has ("*.c" and a case-sensitive "*.C"). */
static bool glob_is_extension(const xd_mime_glob *mime_glob, const char *ext) {
    char *mine = glob_extension(mime_glob->pattern, TRUE);
    char *folded_glob = mine != nullptr ? g_utf8_casefold(mine, -1) : nullptr;
    char *folded_ext = g_utf8_casefold(ext, -1);
    bool is = folded_glob != nullptr && strcmp(folded_glob, folded_ext) == 0;
    g_free(mine);
    g_free(folded_glob);
    g_free(folded_ext);
    return is;
}

bool xd_mime_add_extension(const char *const *data_dirs, const char *type, const char *ext,
                           const char *comment, GError **error) {
    if (!safe_type(type) || !xd_mime_extension_valid(ext)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not a file type: %s (.%s)",
                    type, ext);
        return false;
    }
    char *path = package_path(data_dirs[0], type);
    xd_mime_package pkg;
    load_package(path, type, &pkg);
    if (comment != nullptr && pkg.comment == nullptr)
        pkg.comment = g_strdup(comment);
    bool there = false;
    for (guint i = 0; i < pkg.globs->len && !there; i++)
        there = glob_is_extension(g_ptr_array_index(pkg.globs, i), ext);
    if (!there) {
        /* Not case-sensitive, so in lower case: matching folds the name. */
        char *lower = g_utf8_strdown(ext, -1);
        xd_mime_glob *mime_glob = g_new0(xd_mime_glob, 1);
        mime_glob->pattern = g_strconcat("*.", lower, nullptr);
        g_free(lower);
        g_ptr_array_add(pkg.globs, mime_glob);
    }
    bool ok = save_package(path, &pkg, error);
    xd_mime_package_clear(&pkg);
    g_free(path);
    return ok;
}

bool xd_mime_remove_extension(const char *const *data_dirs, const char *type, const char *ext,
                              bool *type_removed, GError **error) {
    *type_removed = false;
    if (!safe_type(type) || !xd_mime_extension_valid(ext)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not a file type: %s (.%s)",
                    type, ext);
        return false;
    }
    char *path = package_path(data_dirs[0], type);
    xd_mime_package pkg;
    load_package(path, type, &pkg);
    /* All the type's globs but this one's, hiding what the other
     * databases have. */
    GPtrArray *globs = xd_mime_type_globs(data_dirs, type);
    for (guint i = globs->len; i-- > 0;)
        if (glob_is_extension(g_ptr_array_index(globs, i), ext))
            g_ptr_array_remove_index(globs, i);
    g_ptr_array_unref(pkg.globs);
    pkg.globs = globs;
    bool ok;
    /* A type only we define, left with nothing: no type at all. */
    bool ours_only = pkg.comment != nullptr;
    for (const char *const *data_dir = data_dirs + 1; ours_only && *data_dir != nullptr;
         data_dir++) {
        gsize len = 0;
        char *xml = type_xml(*data_dir, type, &len);
        ours_only = xml == nullptr;
        g_free(xml);
    }
    if (ours_only && globs->len == 0) {
        ok = g_remove(path) == 0 || errno == ENOENT;
        if (!ok)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot remove %s: %s",
                        path, g_strerror(errno));
        *type_removed = ok;
    } else {
        pkg.delete_all = true;
        ok = save_package(path, &pkg, error);
    }
    xd_mime_package_clear(&pkg);
    g_free(path);
    return ok;
}

bool xd_mime_update_database(const char *data_dir, GError **error) {
    char *mime = g_build_filename(data_dir, "mime", nullptr);
    const char *argv[] = {"update-mime-database", mime, nullptr};
    int status = 0;
    char *err = nullptr;
    bool ok = g_spawn_sync(nullptr, (char **)(uintptr_t)argv, nullptr,
                           (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL), nullptr,
                           nullptr, nullptr, &err, &status, error) &&
              g_spawn_check_wait_status(status, error);
    if (!ok && err != nullptr && err[0] != '\0' && error != nullptr && *error != nullptr)
        g_prefix_error(error, "%s\n", g_strstrip(err));
    g_free(err);
    g_free(mime);
    return ok;
}
