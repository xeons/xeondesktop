/* SPDX-License-Identifier: MIT
 * Icon lookup through GTK3's GtkIconTheme: the freedesktop icon theme spec
 * (theme inheritance, hicolor fallback, size matching) plus loading
 * PNG/SVG via gdk-pixbuf. Only the icon theme is used; GTK isn't
 * initialized and no GTK widgets are created.
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
    gint path_count = 0;
    gtk_icon_theme_get_search_path(theme, &paths, &path_count);
    bool found = false;
    for (gint i = 0; i < path_count && !found; i++) {
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

/* An icon named by its file (a desktop entry's Icon=/path, a tray icon's
 * IconName or a window's app id may be one): only a regular file, read
 * without waiting (a FIFO would hold the shell up for ever), and not a
 * huge one. */
static GdkPixbuf *load_file(const char *path, int px) {
    enum { MAX_BYTES = 8 * 1024 * 1024 };
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    struct stat st;
    GdkPixbuf *pb = nullptr;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= MAX_BYTES) {
        gsize size = (gsize)st.st_size;
        guchar *data = g_malloc(size);
        gsize got = 0;
        while (got < size) {
            ssize_t count = read(fd, data + got, size - got);
            if (count <= 0)
                break;
            got += (gsize)count;
        }
        if (got == size) {
            GInputStream *stream = g_memory_input_stream_new_from_data(data, (gssize)size, g_free);
            data = nullptr;
            pb = gdk_pixbuf_new_from_stream_at_scale(stream, px, px, TRUE, nullptr, nullptr);
            g_object_unref(stream);
        }
        g_free(data);
    }
    close(fd);
    return pb;
}

static cairo_surface_t *load(xd_icons *ic, const char *name, int px) {
    GdkPixbuf *pb = nullptr;
    if (name[0] == '/') {
        /* Desktop entries may name an icon file directly. */
        pb = load_file(name, px);
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
    cairo_surface_t *surface = xd_image_from_pixbuf(pb);
    g_object_unref(pb);
    return surface;
}

cairo_surface_t *xd_icons_get(xd_icons *ic, const char *name, int px) {
    if (name == nullptr || name[0] == '\0' || px <= 0 || px > 1024)
        return nullptr;
    for (size_t i = 0; i < ic->cache_len; i++) {
        if (ic->cache[i].px == px && strcmp(ic->cache[i].name, name) == 0)
            return ic->cache[i].surface;
    }
    cairo_surface_t *surface = load(ic, name, px);
    if (ic->cache_len == ic->cache_cap) {
        ic->cache_cap = ic->cache_cap == 0 ? 64 : ic->cache_cap * 2;
        ic->cache = xd_realloc_array(ic->cache, ic->cache_cap, sizeof(*ic->cache));
    }
    ic->cache[ic->cache_len++] = (cache_entry){
        .name = xd_str_dup_cstr(xd_str_from_cstr(name)),
        .px = px,
        .surface = surface,
    };
    return surface;
}

cairo_surface_t *xd_icons_get_any(xd_icons *ic, const char *const *names, size_t count,
                                  int px) {
    for (size_t i = 0; i < count; i++) {
        cairo_surface_t *surface = xd_icons_get(ic, names[i], px);
        if (surface != nullptr)
            return surface;
    }
    return nullptr;
}
