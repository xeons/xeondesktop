/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "filetypes.h"

typedef struct glob_reader {
    GPtrArray *out;
    int depth; /* <mime-type> is 1 */
} glob_reader;

static gboolean has_extension(GPtrArray *out, const char *ext) {
    for (guint i = 0; i < out->len; i++)
        if (strcmp(g_ptr_array_index(out, i), ext) == 0)
            return TRUE;
    return FALSE;
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

static void on_start([[maybe_unused]] GMarkupParseContext *ctx, const char *element,
                     const char **names, const char **values, gpointer data,
                     [[maybe_unused]] GError **error) {
    glob_reader *r = data;
    /* Only the type's own globs, not ones in nested elements. */
    if (++r->depth != 2 || strcmp(element, "glob") != 0)
        return;
    const char *pattern = nullptr;
    gboolean case_sensitive = FALSE;
    for (size_t i = 0; names[i] != nullptr; i++) {
        if (strcmp(names[i], "pattern") == 0)
            pattern = values[i];
        else if (strcmp(names[i], "case-sensitive") == 0)
            case_sensitive = strcmp(values[i], "true") == 0;
    }
    char *ext = pattern != nullptr ? glob_extension(pattern, case_sensitive) : nullptr;
    if (ext != nullptr && !has_extension(r->out, ext))
        g_ptr_array_add(r->out, ext);
    else
        g_free(ext);
}

static void on_end([[maybe_unused]] GMarkupParseContext *ctx,
                   [[maybe_unused]] const char *element, gpointer data,
                   [[maybe_unused]] GError **error) {
    ((glob_reader *)data)->depth--;
}

void xd_mime_xml_extensions(const char *xml, gssize len, GPtrArray *out) {
    static const GMarkupParser parser = {.start_element = on_start, .end_element = on_end};
    glob_reader r = {out, 0};
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, G_MARKUP_IGNORE_QUALIFIED, &r,
                                                          nullptr);
    if (g_markup_parse_context_parse(ctx, xml, len, nullptr))
        g_markup_parse_context_end_parse(ctx, nullptr);
    g_markup_parse_context_free(ctx);
}

static void read_type_xml(const char *data_dir, const char *type, GPtrArray *out) {
    char *file = g_strconcat(type, ".xml", nullptr);
    char *path = g_build_filename(data_dir, "mime", file, nullptr);
    char *xml = nullptr;
    gsize len = 0;
    if (g_file_get_contents(path, &xml, &len, nullptr) && len < (1u << 20))
        xd_mime_xml_extensions(xml, (gssize)len, out);
    g_free(xml);
    g_free(path);
    g_free(file);
}

char **xd_content_type_extensions(const char *type) {
    GPtrArray *out = g_ptr_array_new();
    /* MIME types are "media/subtype": nothing that could leave the
     * database directory. */
    if (strstr(type, "..") == nullptr && type[0] != '/' && strchr(type, '/') != nullptr) {
        read_type_xml(g_get_user_data_dir(), type, out);
        for (const char *const *d = g_get_system_data_dirs(); *d != nullptr; d++)
            read_type_xml(*d, type, out);
    }
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}
