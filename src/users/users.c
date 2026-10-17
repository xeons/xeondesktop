/* SPDX-License-Identifier: MIT */
#include <crypt.h>
#include <string.h>

#include "users.h"

bool xd_users_valid_name(const char *name) {
    size_t n = name != nullptr ? strlen(name) : 0;
    if (n == 0 || n > 32 || !g_ascii_islower(name[0]))
        return false;
    for (const char *p = name; *p != '\0'; p++) {
        if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p != '_' && *p != '-')
            return false;
    }
    return true;
}

char *xd_users_suggest_name(const char *full_name) {
    /* GLib transliterates in the current locale ("ü" -> "u"). */
    char *ascii = g_str_to_ascii(full_name != nullptr ? full_name : "", nullptr);
    GString *s = g_string_new(nullptr);
    for (const char *p = ascii; *p != '\0' && s->len < 32; p++) {
        char c = g_ascii_tolower(*p);
        if (g_ascii_islower(c) || (s->len > 0 && g_ascii_isdigit(c)))
            g_string_append_c(s, c);
    }
    g_free(ascii);
    return g_string_free(s, FALSE);
}

char *xd_users_hash_password(const char *password) {
    if (password == nullptr)
        return nullptr;
    /* nullptr prefix: the method libxcrypt prefers (yescrypt today). */
    char setting[CRYPT_GENSALT_OUTPUT_SIZE];
    if (crypt_gensalt_rn(nullptr, 0, nullptr, 0, setting, sizeof(setting)) == nullptr)
        return nullptr;
    struct crypt_data *data = g_new0(struct crypt_data, 1);
    const char *hash = crypt_rn(password, setting, data, (int)sizeof(*data));
    /* Failures are nullptr or a string starting with "*". */
    char *out = hash != nullptr && hash[0] == '$' ? g_strdup(hash) : nullptr;
    explicit_bzero(data, sizeof(*data));
    g_free(data);
    return out;
}

bool xd_users_make_picture(const char *source, const char *dest, int size, GError **error) {
    GdkPixbuf *src = gdk_pixbuf_new_from_file(source, error);
    if (src == nullptr)
        return false;
    GdkPixbuf *oriented = gdk_pixbuf_apply_embedded_orientation(src);
    g_object_unref(src);
    int w = gdk_pixbuf_get_width(oriented), h = gdk_pixbuf_get_height(oriented);
    int side = MIN(w, h);
    GdkPixbuf *square = gdk_pixbuf_new_subpixbuf(oriented, (w - side) / 2, (h - side) / 2, side,
                                                 side);
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(square, size, size, GDK_INTERP_HYPER);
    bool ok = scaled != nullptr && gdk_pixbuf_save(scaled, dest, "png", error, nullptr);
    if (scaled != nullptr)
        g_object_unref(scaled);
    g_object_unref(square);
    g_object_unref(oriented);
    return ok;
}
