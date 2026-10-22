/* SPDX-License-Identifier: MIT */
#include <crypt.h>
#include <string.h>

#include "users.h"

bool xd_users_valid_name(const char *name) {
    size_t count = name != nullptr ? strlen(name) : 0;
    if (count == 0 || count > 32 || !g_ascii_islower(name[0]))
        return false;
    for (const char *letter = name; *letter != '\0'; letter++) {
        if (!g_ascii_islower(*letter) && !g_ascii_isdigit(*letter) && *letter != '_' &&
            *letter != '-')
            return false;
    }
    return true;
}

char *xd_users_suggest_name(const char *full_name) {
    /* GLib transliterates in the current locale ("ü" -> "u"). */
    char *ascii = g_str_to_ascii(full_name != nullptr ? full_name : "", nullptr);
    GString *string = g_string_new(nullptr);
    for (const char *letter = ascii; *letter != '\0' && string->len < 32; letter++) {
        char lower = g_ascii_tolower(*letter);
        if (g_ascii_islower(lower) || (string->len > 0 && g_ascii_isdigit(lower)))
            g_string_append_c(string, lower);
    }
    g_free(ascii);
    return g_string_free(string, FALSE);
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
    int width = gdk_pixbuf_get_width(oriented), height = gdk_pixbuf_get_height(oriented);
    int side = MIN(width, height);
    GdkPixbuf *square =
        gdk_pixbuf_new_subpixbuf(oriented, (width - side) / 2, (height - side) / 2, side, side);
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(square, size, size, GDK_INTERP_HYPER);
    bool ok = scaled != nullptr && gdk_pixbuf_save(scaled, dest, "png", error, nullptr);
    if (scaled != nullptr)
        g_object_unref(scaled);
    g_object_unref(square);
    g_object_unref(oriented);
    return ok;
}
