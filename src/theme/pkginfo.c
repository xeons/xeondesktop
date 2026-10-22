/* SPDX-License-Identifier: MIT
 * What a theme package offers, from its theme.xml alone: Display
 * Properties lists every installed package, and loading each one's style
 * and images for that would be slow.
 */
#include <string.h>

#if XD_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include "pkg.h"

/* theme.xml is a few kilobytes; anything near this isn't one. */
enum { THEME_XML_MAX = 1 << 20 };

static GBytes *theme_xml(const char *path, GError **error) {
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        char *file = g_build_filename(path, "theme.xml", nullptr);
        char *data = nullptr;
        gsize len = 0;
        bool ok = g_file_get_contents(file, &data, &len, error);
        g_free(file);
        return ok ? g_bytes_new_take(data, len) : nullptr;
    }
#if XD_HAVE_LIBARCHIVE
    struct archive *reader = archive_read_new();
    archive_read_support_format_zip(reader);
    GBytes *out = nullptr;
    bool opened = archive_read_open_filename(reader, path, 64 * 1024) == ARCHIVE_OK;
    struct archive_entry *archive_member;
    while (opened && out == nullptr &&
           archive_read_next_header(reader, &archive_member) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(archive_member);
        la_int64_t size = archive_entry_size(archive_member);
        if (name == nullptr || strcmp(name, "theme.xml") != 0 ||
            archive_entry_filetype(archive_member) != AE_IFREG || size < 0 ||
            size > THEME_XML_MAX) {
            archive_read_data_skip(reader);
            continue;
        }
        char *buf = g_malloc((gsize)size + 1);
        if (archive_read_data(reader, buf, (size_t)size) == size)
            out = g_bytes_new_take(buf, (gsize)size);
        else
            g_free(buf);
        break;
    }
    if (out == nullptr)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s", path,
                    !opened && archive_error_string(reader) != nullptr
                        ? archive_error_string(reader)
                        : "no theme.xml");
    archive_read_free(reader);
    return out;
#else
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSYS,
                "%s: theme packages must be folders (built without libarchive)", path);
    return nullptr;
#endif
}

static const char *attr(const char **names, const char **values, const char *name) {
    for (size_t i = 0; names[i] != nullptr; i++)
        if (g_ascii_strcasecmp(names[i], name) == 0)
            return values[i];
    return nullptr;
}

/* A path in the package, as the loader takes them: relative, no "..". */
static void set_member(char **out, const char *path) {
    g_free(*out);
    *out = path != nullptr && path[0] != '\0' && path[0] != '/' && strstr(path, "..") == nullptr
               ? g_strdup(path)
               : nullptr;
}

typedef struct info_ctx {
    xd_pkg_info *info;
    GArray *variants; /* xd_pkg_variant_info */
} info_ctx;

static void info_start(G_GNUC_UNUSED GMarkupParseContext *mc, const char *element,
                       const char **names, const char **values, gpointer data,
                       G_GNUC_UNUSED GError **error) {
    info_ctx *info = data;
    if (strcmp(element, "xdtheme") == 0) {
        const char *name = attr(names, values, "name");
        g_free(info->info->name);
        info->info->name = g_strdup(name != nullptr ? name : "");
        const char *base = attr(names, values, "base");
        if (base == nullptr ||
            !xd_visual_style_from_name(xd_str_from_cstr(base), &info->info->base))
            info->info->base = XD_STYLE_CLASSIC;
    } else if (strcmp(element, "variant") == 0) {
        /* As xd_pkg_load: a variant without a style isn't one. */
        const char *id = attr(names, values, "id");
        if (id == nullptr || attr(names, values, "style") == nullptr)
            return;
        const char *name = attr(names, values, "name");
        const char *size = attr(names, values, "size");
        xd_pkg_variant_info variant_info = {
            .id = g_strdup(id),
            .name = g_strdup(name != nullptr && *name != '\0' ? name : id),
            .size = g_strdup(size != nullptr && *size != '\0' ? size : "Normal"),
        };
        g_array_append_val(info->variants, variant_info);
    } else if (strcmp(element, "scaling") == 0) {
        const char *filter_name = attr(names, values, "filter");
        info->info->has_filter =
            filter_name != nullptr && xd_image_filter_from_name(filter_name, &info->info->filter);
    } else if (strcmp(element, "wallpaper") == 0) {
        set_member(&info->info->wallpaper, attr(names, values, "file"));
        const char *style = attr(names, values, "style");
        g_free(info->info->wallpaper_style);
        info->info->wallpaper_style = info->info->wallpaper != nullptr && style != nullptr
                                          ? g_ascii_strdown(style, -1)
                                          : nullptr;
    } else if (strcmp(element, "sounds") == 0) {
        set_member(&info->info->sounds, attr(names, values, "scheme"));
    } else if (strcmp(element, "cursors") == 0) {
        set_member(&info->info->cursors, attr(names, values, "theme"));
    }
}

bool xd_pkg_read_info(const char *path, xd_pkg_info *out, GError **error) {
    *out = (xd_pkg_info){.name = g_strdup("")};
    GBytes *bytes = theme_xml(path, error);
    if (bytes == nullptr) {
        xd_pkg_info_clear(out);
        return false;
    }
    info_ctx info = {.info = out,
                     .variants = g_array_new(FALSE, TRUE, sizeof(xd_pkg_variant_info))};
    static const GMarkupParser parser = {.start_element = info_start};
    gsize len = 0;
    const char *text = g_bytes_get_data(bytes, &len);
    GMarkupParseContext *mc = g_markup_parse_context_new(&parser, 0, &info, nullptr);
    bool ok = g_markup_parse_context_parse(mc, text, (gssize)len, error) &&
              g_markup_parse_context_end_parse(mc, error);
    g_markup_parse_context_free(mc);
    g_bytes_unref(bytes);
    out->variant_count = info.variants->len;
    out->variants = (xd_pkg_variant_info *)(void *)g_array_free(info.variants, FALSE);
    if (ok && out->variant_count == 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: no variant", path);
        ok = false;
    } else if (!ok) {
        g_prefix_error(error, "%s: theme.xml: ", path);
    }
    if (!ok)
        xd_pkg_info_clear(out);
    return ok;
}

void xd_pkg_info_clear(xd_pkg_info *info) {
    for (size_t i = 0; i < info->variant_count; i++) {
        g_free(info->variants[i].id);
        g_free(info->variants[i].name);
        g_free(info->variants[i].size);
    }
    g_free(info->variants);
    g_free(info->name);
    g_free(info->wallpaper);
    g_free(info->wallpaper_style);
    g_free(info->sounds);
    g_free(info->cursors);
    *info = (xd_pkg_info){0};
}
