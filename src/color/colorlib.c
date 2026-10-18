/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <glib/gstdio.h>

#include "colorlib.h"

/* Profiles are tens of kilobytes; a big one with many tables a few MB. */
static constexpr goffset MAX_PROFILE_BYTES = 64 * 1024 * 1024;

static bool present(const char *s) { return s != nullptr && s[0] != '\0'; }

char *xd_color_display_id(const char *vendor, const char *model, const char *serial,
                          const char *connector) {
    GString *id = g_string_new("xrandr");
    if (present(vendor))
        g_string_append_printf(id, "-%s", vendor);
    if (present(model))
        g_string_append_printf(id, "-%s", model);
    if (present(serial))
        g_string_append_printf(id, "-%s", serial);
    if (!present(vendor) && !present(model) && !present(serial))
        g_string_append_printf(id, "-%s", present(connector) ? connector : "unknown");
    return g_string_free(id, FALSE);
}

const char *xd_color_kind_label(const char *kind) {
    static const struct {
        const char *kind, *label;
    } kinds[] = {
        {"display", "Display"}, {"printer", "Printer"}, {"scanner", "Scanner"},
        {"camera", "Camera"},   {"webcam", "Webcam"},
    };
    for (size_t i = 0; kind != nullptr && i < G_N_ELEMENTS(kinds); i++) {
        if (strcmp(kinds[i].kind, kind) == 0)
            return kinds[i].label;
    }
    return "Device";
}

char *xd_color_device_label(const char *kind, const char *vendor, const char *model,
                            const char *id) {
    const char *what = xd_color_kind_label(kind);
    /* Model names often repeat the vendor ("HP HP LaserJet"). */
    if (present(vendor) && present(model) && !g_str_has_prefix(model, vendor))
        return g_strdup_printf("%s: %s %s", what, vendor, model);
    if (present(model))
        return g_strdup_printf("%s: %s", what, model);
    if (present(vendor))
        return g_strdup_printf("%s: %s", what, vendor);
    return g_strdup_printf("%s: %s", what, present(id) ? id : "Unknown");
}

bool xd_color_looks_like_icc(const guint8 *data, size_t len) {
    if (len < 128)
        return false;
    /* The header's first field is the profile's size, big-endian. */
    guint32 size = (guint32)data[0] << 24 | (guint32)data[1] << 16 | (guint32)data[2] << 8 |
                   (guint32)data[3];
    return size >= 128 && size <= len && memcmp(data + 36, "acsp", 4) == 0;
}

char *xd_color_profile_id(const guint8 *data, size_t len) {
    char *md5 = g_compute_checksum_for_data(G_CHECKSUM_MD5, data, len);
    char *id = g_strdup_printf("icc-%s", md5);
    g_free(md5);
    return id;
}

static bool same_contents(const char *path, const char *data, gsize len) {
    char *other = nullptr;
    gsize other_len = 0;
    bool same = g_file_get_contents(path, &other, &other_len, nullptr) && other_len == len &&
                memcmp(other, data, len) == 0;
    g_free(other);
    return same;
}

char *xd_color_import(const char *path, const char *store, char **id, GError **error) {
    GStatBuf st;
    if (g_stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s can't be read.", path);
        return nullptr;
    }
    if (st.st_size > MAX_PROFILE_BYTES) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "%s is too large to be a color profile.", path);
        return nullptr;
    }
    char *data = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, error))
        return nullptr;
    if (!xd_color_looks_like_icc((const guint8 *)data, len)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "%s is not a valid color profile.", path);
        g_free(data);
        return nullptr;
    }
    if (g_mkdir_with_parents(store, 0755) != 0) {
        GIOErrorEnum code = g_io_error_from_errno(errno);
        g_set_error(error, G_IO_ERROR, (gint)code, "%s can't be created.",
                    store);
        g_free(data);
        return nullptr;
    }
    char *base = g_path_get_basename(path);
    const char *dot = strrchr(base, '.');
    size_t stem_len = dot != nullptr && dot != base ? (size_t)(dot - base) : strlen(base);
    const char *ext = dot != nullptr && dot != base ? dot : ".icc";
    char *dest = nullptr;
    for (int n = 1; dest == nullptr && n < 100; n++) {
        char *name = n == 1 ? g_strdup_printf("%.*s%s", (int)stem_len, base, ext)
                            : g_strdup_printf("%.*s-%d%s", (int)stem_len, base, n, ext);
        char *candidate = g_build_filename(store, name, nullptr);
        g_free(name);
        if (!g_file_test(candidate, G_FILE_TEST_EXISTS) || same_contents(candidate, data, len))
            dest = candidate;
        else
            g_free(candidate);
    }
    g_free(base);
    bool ok = dest != nullptr;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Too many profiles named like %s.",
                    path);
    /* The original is left where it was; the store gets a copy. */
    else if (!g_file_test(dest, G_FILE_TEST_EXISTS))
        ok = g_file_set_contents_full(dest, data, (gssize)len, G_FILE_SET_CONTENTS_CONSISTENT,
                                      0644, error);
    if (ok && id != nullptr)
        *id = xd_color_profile_id((const guint8 *)data, len);
    g_free(data);
    if (!ok)
        g_clear_pointer(&dest, g_free);
    return dest;
}
