/* SPDX-License-Identifier: MIT */
#include "chooserlib.h"

#include <fnmatch.h>
#include <gio/gio.h>
#include <string.h>

/* ---- file filters ------------------------------------------------------------------ */

/* The portal's rule kinds (org.freedesktop.portal.FileChooser). */
enum { RULE_GLOB = 0, RULE_MIME = 1 };

void xd_chooser_filter_free(xd_chooser_filter *filter) {
    if (filter == nullptr)
        return;
    g_free(filter->name);
    g_ptr_array_unref(filter->globs);
    g_ptr_array_unref(filter->mimes);
    if (filter->source != nullptr)
        g_variant_unref(filter->source);
    g_free(filter);
}

xd_chooser_filter *xd_chooser_filter_parse(GVariant *variant) {
    if (variant == nullptr || !g_variant_is_of_type(variant, G_VARIANT_TYPE("(sa(us))")))
        return nullptr;
    xd_chooser_filter *filter = g_new0(xd_chooser_filter, 1);
    filter->globs = g_ptr_array_new_with_free_func(g_free);
    filter->mimes = g_ptr_array_new_with_free_func(g_free);
    filter->source = g_variant_ref_sink(variant);
    const char *name = nullptr;
    GVariantIter *rules = nullptr;
    g_variant_get(variant, "(&sa(us))", &name, &rules);
    filter->name = g_strdup(name);
    guint32 kind = 0;
    const char *rule = nullptr;
    while (g_variant_iter_next(rules, "(u&s)", &kind, &rule)) {
        if (kind == RULE_GLOB)
            g_ptr_array_add(filter->globs, g_strdup(rule));
        else if (kind == RULE_MIME)
            g_ptr_array_add(filter->mimes, g_strdup(rule));
    }
    g_variant_iter_free(rules);
    return filter;
}

static void free_filter(gpointer data) { xd_chooser_filter_free(data); }

GPtrArray *xd_chooser_filters_parse(GVariant *filters) {
    GPtrArray *out = g_ptr_array_new_with_free_func(free_filter);
    if (filters == nullptr || !g_variant_is_of_type(filters, G_VARIANT_TYPE("a(sa(us))")))
        return out;
    GVariantIter iter;
    g_variant_iter_init(&iter, filters);
    GVariant *child = nullptr;
    while ((child = g_variant_iter_next_value(&iter)) != nullptr) {
        xd_chooser_filter *filter = xd_chooser_filter_parse(child);
        if (filter != nullptr)
            g_ptr_array_add(out, filter);
        g_variant_unref(child);
    }
    return out;
}

int xd_chooser_filters_find(const GPtrArray *filters, GVariant *current) {
    if (current == nullptr || !g_variant_is_of_type(current, G_VARIANT_TYPE("(sa(us))")))
        return -1;
    for (guint i = 0; i < filters->len; i++) {
        const xd_chooser_filter *filter = g_ptr_array_index(filters, i);
        if (g_variant_equal(filter->source, current))
            return (int)i;
    }
    const char *name = nullptr;
    g_variant_get_child(current, 0, "&s", &name);
    for (guint i = 0; i < filters->len; i++) {
        const xd_chooser_filter *filter = g_ptr_array_index(filters, i);
        if (g_strcmp0(filter->name, name) == 0)
            return (int)i;
    }
    return -1;
}

bool xd_chooser_filter_matches(const xd_chooser_filter *filter, const char *name,
                               const char *content_type) {
    if (filter == nullptr || (filter->globs->len == 0 && filter->mimes->len == 0))
        return true;
    for (guint i = 0; i < filter->globs->len; i++) {
        if (fnmatch(g_ptr_array_index(filter->globs, i), name, FNM_CASEFOLD) == 0)
            return true;
    }
    if (filter->mimes->len == 0)
        return false;
    char *guessed = content_type == nullptr ? g_content_type_guess(name, nullptr, 0, nullptr)
                                            : nullptr;
    const char *type = content_type != nullptr ? content_type : guessed;
    bool matched = false;
    for (guint i = 0; i < filter->mimes->len && !matched; i++) {
        const char *mime = g_ptr_array_index(filter->mimes, i);
        /* A bare star, or the any-type MIME rule, means anything, as GTK
         * takes them. */
        matched = strcmp(mime, "*") == 0 || strcmp(mime, "*/*") == 0 ||
                  g_content_type_is_mime_type(type, mime);
    }
    g_free(guessed);
    return matched;
}

char *xd_chooser_filter_get_extension(const xd_chooser_filter *filter) {
    if (filter == nullptr || filter->globs->len == 0)
        return nullptr;
    const char *glob = g_ptr_array_index(filter->globs, 0);
    if (!g_str_has_prefix(glob, "*.") || glob[2] == '\0')
        return nullptr;
    GString *extension = g_string_new(".");
    for (const char *cursor = glob + 2; *cursor != '\0'; cursor++) {
        if (*cursor == '*' || *cursor == '?' || *cursor == '.' || *cursor == '/') {
            g_string_free(extension, TRUE);
            return nullptr;
        }
        if (*cursor == '[') {
            /* A bracket of a letter's two cases, [Tt]: the letter in lower case. */
            const char *close = strchr(cursor, ']');
            if (close == nullptr || close == cursor + 1) {
                g_string_free(extension, TRUE);
                return nullptr;
            }
            g_string_append_c(extension, g_ascii_tolower(cursor[1]));
            cursor = close;
            continue;
        }
        g_string_append_c(extension, *cursor);
    }
    return g_string_free(extension, FALSE);
}

char *xd_chooser_add_extension(const char *name, const xd_chooser_filter *filter) {
    size_t length = strlen(name);
    if (length >= 2 && name[0] == '"' && name[length - 1] == '"')
        return g_strndup(name + 1, length - 2);
    if (length > 0 && name[length - 1] == '.')
        return g_strndup(name, length - 1);
    const char *slash = strrchr(name, '/');
    const char *base = slash != nullptr ? slash + 1 : name;
    char *extension = xd_chooser_filter_get_extension(filter);
    if (extension == nullptr || strchr(base, '.') != nullptr || *base == '\0') {
        g_free(extension);
        return g_strdup(name);
    }
    char *out = g_strconcat(name, extension, nullptr);
    g_free(extension);
    return out;
}

bool xd_chooser_is_pattern(const char *text) {
    return text != nullptr && strpbrk(text, "*?") != nullptr;
}

/* ---- the File name box ------------------------------------------------------------- */

void xd_typed_name_free(xd_typed_name *typed) {
    g_strfreev(typed->paths);
    g_free(typed->pattern);
    *typed = (xd_typed_name){0};
}

/* One name typed: a path, absolute and canonical. */
static char *resolve_name(const char *folder, const char *name) {
    if (g_str_has_prefix(name, "file://")) {
        char *local = g_filename_from_uri(name, nullptr, nullptr);
        if (local != nullptr) {
            char *canonical = g_canonicalize_filename(local, nullptr);
            g_free(local);
            return canonical;
        }
    }
    if (strcmp(name, "~") == 0)
        return g_strdup(g_get_home_dir());
    if (g_str_has_prefix(name, "~/"))
        return g_canonicalize_filename(name + 2, g_get_home_dir());
    return g_canonicalize_filename(name, folder);
}

static char **one_path(char *path) {
    char **paths = g_new0(char *, 2);
    paths[0] = path;
    return paths;
}

xd_typed_name xd_chooser_parse_name(const char *folder, const char *text, bool multiple) {
    xd_typed_name typed = {0};
    char *trimmed = g_strstrip(g_strdup(text != nullptr ? text : ""));
    if (*trimmed == '\0') {
        g_free(trimmed);
        return typed;
    }
    /* Several names, each in quotes, as XP's Open took them. */
    size_t length = strlen(trimmed);
    bool quoted = trimmed[0] == '"';
    if (multiple && quoted && strchr(trimmed + 1, '"') != nullptr &&
        strchr(trimmed + 1, '"') != trimmed + length - 1) {
        char **names = nullptr;
        if (g_shell_parse_argv(trimmed, nullptr, &names, nullptr)) {
            GPtrArray *paths = g_ptr_array_new();
            for (char **name = names; *name != nullptr; name++) {
                if (**name != '\0')
                    g_ptr_array_add(paths, resolve_name(folder, *name));
            }
            g_ptr_array_add(paths, nullptr);
            typed.kind = XD_TYPED_FILE;
            typed.paths = (char **)g_ptr_array_free(paths, FALSE);
            g_strfreev(names);
            g_free(trimmed);
            if (typed.paths[0] == nullptr)
                xd_typed_name_free(&typed);
            return typed;
        }
    }
    if (quoted && length >= 2 && trimmed[length - 1] == '"') {
        trimmed[length - 1] = '\0';
        memmove(trimmed, trimmed + 1, length - 1);
    }
    char *path = resolve_name(folder, trimmed);
    char *base = g_path_get_basename(path);
    if (xd_chooser_is_pattern(base)) {
        typed.kind = XD_TYPED_PATTERN;
        typed.pattern = base;
        typed.paths = one_path(g_path_get_dirname(path));
        g_free(path);
    } else {
        g_free(base);
        typed.kind = g_file_test(path, G_FILE_TEST_IS_DIR) ? XD_TYPED_FOLDER : XD_TYPED_FILE;
        typed.paths = one_path(path);
    }
    g_free(trimmed);
    return typed;
}
