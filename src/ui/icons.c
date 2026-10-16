/* SPDX-License-Identifier: MIT
 * Icon lookup through GTK3's GtkIconTheme: the freedesktop icon theme spec
 * (theme inheritance, hicolor fallback, size matching) plus loading
 * PNG/SVG via gdk-pixbuf. Only the icon theme is used; GTK isn't
 * initialized and no GTK widgets are created.
 */
#include <gtk/gtk.h>

#include "../core/alloc.h"
#include "icons.h"
#include "image.h"

typedef struct cache_entry {
    char *name;
    int px;
    cairo_surface_t *surface; /* nullptr = known missing */
} cache_entry;

struct xd_icons {
    GtkIconTheme *theme;
    cache_entry *cache;
    size_t cache_len, cache_cap;
};

/* Is `name` an installed icon theme (has an index.theme on the search
 * path)? */
static bool theme_installed(GtkIconTheme *theme, const char *name) {
    gchar **paths = nullptr;
    gint n = 0;
    gtk_icon_theme_get_search_path(theme, &paths, &n);
    bool found = false;
    for (gint i = 0; i < n && !found; i++) {
        char *index = g_build_filename(paths[i], name, "index.theme", nullptr);
        found = g_file_test(index, G_FILE_TEST_IS_REGULAR);
        g_free(index);
    }
    g_strfreev(paths);
    return found;
}

xd_icons *xd_icons_create(const char *theme_name) {
    xd_icons *ic = XD_NEW(xd_icons);
    ic->theme = gtk_icon_theme_new();
    /* The requested theme if installed, else common defaults; GTK follows
     * each theme's Inherits= chain and ends at hicolor. */
    const char *candidates[] = {theme_name, "breeze", "Adwaita"};
    for (size_t i = 0; i < XD_ARRAY_LEN(candidates); i++) {
        if (candidates[i] != nullptr && theme_installed(ic->theme, candidates[i])) {
            gtk_icon_theme_set_custom_theme(ic->theme, candidates[i]);
            break;
        }
    }
    return ic;
}

void xd_icons_destroy(xd_icons *ic) {
    if (ic == nullptr)
        return;
    for (size_t i = 0; i < ic->cache_len; i++) {
        if (ic->cache[i].surface != nullptr)
            cairo_surface_destroy(ic->cache[i].surface);
        xd_free(ic->cache[i].name);
    }
    xd_free(ic->cache);
    g_object_unref(ic->theme);
    xd_free(ic);
}

static cairo_surface_t *load(xd_icons *ic, const char *name, int px) {
    GdkPixbuf *pb = nullptr;
    if (name[0] == '/') {
        /* Desktop entries may name an icon file directly. */
        pb = gdk_pixbuf_new_from_file_at_size(name, px, px, nullptr);
    } else {
        GtkIconInfo *info = gtk_icon_theme_lookup_icon_for_scale(ic->theme, name, px, 1,
                                                                 GTK_ICON_LOOKUP_FORCE_SIZE);
        if (info != nullptr) {
            pb = gtk_icon_info_load_icon(info, nullptr);
            g_object_unref(info);
        }
    }
    if (pb == nullptr)
        return nullptr;
    cairo_surface_t *s = xd_image_from_pixbuf(pb);
    g_object_unref(pb);
    return s;
}

cairo_surface_t *xd_icons_get(xd_icons *ic, const char *name, int px) {
    if (name == nullptr || name[0] == '\0' || px <= 0 || px > 1024)
        return nullptr;
    for (size_t i = 0; i < ic->cache_len; i++) {
        if (ic->cache[i].px == px && strcmp(ic->cache[i].name, name) == 0)
            return ic->cache[i].surface;
    }
    cairo_surface_t *s = load(ic, name, px);
    if (ic->cache_len == ic->cache_cap) {
        ic->cache_cap = ic->cache_cap == 0 ? 64 : ic->cache_cap * 2;
        ic->cache = xd_realloc_array(ic->cache, ic->cache_cap, sizeof(*ic->cache));
    }
    ic->cache[ic->cache_len++] = (cache_entry){
        .name = xd_str_dup_cstr(xd_str_from_cstr(name)),
        .px = px,
        .surface = s,
    };
    return s;
}

cairo_surface_t *xd_icons_get_any(xd_icons *ic, const char *const *names, size_t count,
                                  int px) {
    for (size_t i = 0; i < count; i++) {
        cairo_surface_t *s = xd_icons_get(ic, names[i], px);
        if (s != nullptr)
            return s;
    }
    return nullptr;
}
