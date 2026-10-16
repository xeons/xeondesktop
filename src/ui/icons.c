/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <librsvg/rsvg.h>
#include <stdlib.h>

#include "../core/alloc.h"
#include "../core/str.h"
#include "icons.h"

enum { XD_ICON_SCALABLE = 0, XD_ICON_MAX_THEMES = 6, XD_ICON_MAX_DEPTH = 3 };

/* One file that provides an icon. size 0 means scalable (SVG). */
typedef struct icon_file {
    struct icon_file *next;
    const char *path;
    int size;
    uint8_t theme; /* index into xd_icons.themes; lower wins */
} icon_file;

typedef struct icon_entry {
    const char *name; /* nullptr = empty slot */
    icon_file *files;
} icon_entry;

typedef struct cache_entry {
    const char *name;
    int px;
    cairo_surface_t *surface; /* nullptr = known missing */
} cache_entry;

struct xd_icons {
    xd_arena arena;
    const char *themes[XD_ICON_MAX_THEMES];
    size_t theme_count;

    icon_entry *index;
    size_t index_cap; /* power of two */
    size_t index_len;

    cache_entry *cache;
    size_t cache_len, cache_cap;
};

static uint64_t hash_str(xd_str s) {
    /* FNV-1a */
    uint64_t h = 0xcbf29ce484222325u;
    for (size_t i = 0; i < s.len; i++) {
        h ^= (unsigned char)s.ptr[i];
        h *= 0x100000001b3u;
    }
    return h;
}

static icon_entry *index_slot(xd_icons *ic, xd_str name) {
    size_t mask = ic->index_cap - 1;
    for (size_t i = (size_t)hash_str(name) & mask;; i = (i + 1) & mask) {
        icon_entry *e = &ic->index[i];
        if (e->name == nullptr || xd_str_eq(xd_str_from_cstr(e->name), name))
            return e;
    }
}

static void index_grow(xd_icons *ic) {
    size_t old_cap = ic->index_cap;
    icon_entry *old = ic->index;
    ic->index_cap = old_cap == 0 ? 1024 : old_cap * 2;
    ic->index = XD_NEW_ARRAY(icon_entry, ic->index_cap);
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].name != nullptr)
            *index_slot(ic, xd_str_from_cstr(old[i].name)) = old[i];
    }
    xd_free(old);
}

static void index_add(xd_icons *ic, xd_str name, const char *path, int size, uint8_t theme) {
    if ((ic->index_len + 1) * 2 > ic->index_cap)
        index_grow(ic);
    icon_entry *e = index_slot(ic, name);
    if (e->name == nullptr) {
        e->name = xd_str_arena_cstr(&ic->arena, name);
        ic->index_len++;
    }
    icon_file *f = XD_ARENA_NEW(&ic->arena, icon_file);
    f->path = path;
    f->size = size;
    f->theme = theme;
    f->next = e->files;
    e->files = f;
}

/* Directory names that give an icon size: "48x48", "48x48@2", "48",
 * "scalable". Returns -1 for anything else. */
static int size_from_dir(xd_str dir) {
    if (xd_str_eq(dir, XD_STR("scalable")))
        return XD_ICON_SCALABLE;
    size_t x;
    xd_str num = xd_str_find_char(dir, 'x', &x) ? xd_str_sub(dir, 0, x) : dir;
    int64_t v;
    if (!xd_str_parse_i64(num, 8, 1024, &v))
        return -1;
    if (num.len != dir.len) {
        /* "48x48" or "48x48@2": the part after 'x' must start with digits */
        xd_str rest = xd_str_sub(dir, x + 1, dir.len);
        if (rest.len == 0 || rest.ptr[0] < '0' || rest.ptr[0] > '9')
            return -1;
    }
    return (int)v;
}

static void scan_dir(xd_icons *ic, xd_strbuf *path, int size, uint8_t theme, int depth) {
    DIR *d = opendir(xd_strbuf_cstr(path));
    if (d == nullptr)
        return;
    size_t base_len = path->len;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        xd_str name = xd_str_from_cstr(de->d_name);
        if (name.len == 0 || name.ptr[0] == '.')
            continue;
        path->len = base_len;
        xd_strbuf_append_char(path, '/');
        xd_strbuf_append(path, name);

        bool is_dir = de->d_type == DT_DIR || de->d_type == DT_LNK || de->d_type == DT_UNKNOWN;
        size_t dot = name.len;
        while (dot > 0 && name.ptr[dot - 1] != '.')
            dot--;
        xd_str ext = dot > 0 ? xd_str_sub(name, dot, name.len) : XD_STR("");
        bool png = xd_str_eq(ext, XD_STR("png"));
        bool svg = xd_str_eq(ext, XD_STR("svg")) || xd_str_eq(ext, XD_STR("svgz"));

        if ((png || svg) && size >= 0) {
            int file_size = svg ? XD_ICON_SCALABLE : size;
            if (svg && size > 0)
                file_size = size; /* fixed-size SVG (breeze's apps/48) */
            index_add(ic, xd_str_sub(name, 0, dot - 1),
                      xd_str_arena_cstr(&ic->arena, xd_strbuf_view(path)), file_size, theme);
        } else if (is_dir && !png && !svg && depth < XD_ICON_MAX_DEPTH) {
            int sub = size_from_dir(name);
            scan_dir(ic, path, sub >= 0 ? sub : size, theme, depth + 1);
        }
    }
    path->len = base_len;
    xd_strbuf_cstr(path);
    closedir(d);
}

static void add_base_dirs(size_t *count, xd_strbuf out[static 8]) {
    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    if (data_home != nullptr && data_home[0] != '\0')
        xd_strbuf_appendf(&out[(*count)++], "%s/icons", data_home);
    else if (home != nullptr)
        xd_strbuf_appendf(&out[(*count)++], "%s/.local/share/icons", home);
    if (home != nullptr)
        xd_strbuf_appendf(&out[(*count)++], "%s/.icons", home);

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    xd_str rest = xd_str_from_cstr(data_dirs != nullptr && data_dirs[0] != '\0'
                                       ? data_dirs
                                       : "/usr/local/share:/usr/share");
    while (rest.len > 0 && *count < 7) {
        size_t colon = rest.len;
        bool more = xd_str_find_char(rest, ':', &colon);
        xd_str dir = xd_str_sub(rest, 0, colon);
        if (dir.len > 0)
            xd_strbuf_appendf(&out[(*count)++], "%.*s/icons", (int)dir.len, dir.ptr);
        rest = more ? xd_str_sub(rest, colon + 1, rest.len) : XD_STR("");
    }
}

xd_icons *xd_icons_create(const char *theme) {
    xd_icons *ic = XD_NEW(xd_icons);
    xd_arena_init(&ic->arena, 64 * 1024);
    index_grow(ic);

    /* Requested theme, common fallbacks, hicolor last (the spec's
     * required fallback); pixmaps are scanned as a final pseudo-theme. */
    const char *candidates[] = {theme, "breeze", "Adwaita", "AdwaitaLegacy", "hicolor"};
    for (size_t i = 0; i < XD_ARRAY_LEN(candidates); i++) {
        if (candidates[i] == nullptr || ic->theme_count >= XD_ICON_MAX_THEMES - 1)
            continue;
        bool dup = false;
        for (size_t j = 0; j < ic->theme_count; j++)
            dup = dup || strcmp(ic->themes[j], candidates[i]) == 0;
        if (!dup)
            ic->themes[ic->theme_count++] = xd_str_arena_cstr(&ic->arena,
                                                              xd_str_from_cstr(candidates[i]));
    }

    xd_strbuf bases[8] = {0};
    size_t base_count = 0;
    add_base_dirs(&base_count, bases);
    xd_strbuf path = {0};
    for (size_t t = 0; t < ic->theme_count; t++) {
        for (size_t b = 0; b < base_count; b++) {
            xd_strbuf_clear(&path);
            xd_strbuf_appendf(&path, "%s/%s", xd_strbuf_cstr(&bases[b]), ic->themes[t]);
            scan_dir(ic, &path, -1, (uint8_t)t, 0);
        }
    }
    /* /usr/share/pixmaps: unthemed, any size. */
    xd_strbuf_clear(&path);
    xd_strbuf_append(&path, XD_STR("/usr/share/pixmaps"));
    scan_dir(ic, &path, 48, (uint8_t)ic->theme_count, XD_ICON_MAX_DEPTH);

    xd_strbuf_free(&path);
    for (size_t b = 0; b < base_count; b++)
        xd_strbuf_free(&bases[b]);
    return ic;
}

void xd_icons_destroy(xd_icons *ic) {
    if (ic == nullptr)
        return;
    for (size_t i = 0; i < ic->cache_len; i++) {
        if (ic->cache[i].surface != nullptr)
            cairo_surface_destroy(ic->cache[i].surface);
    }
    xd_free(ic->cache);
    xd_free(ic->index);
    xd_arena_destroy(&ic->arena);
    xd_free(ic);
}

/* Best file: earliest theme, then scalable, then the smallest size >= px,
 * then the largest smaller one. */
static const icon_file *pick(const icon_entry *e, int px) {
    const icon_file *best = nullptr;
    for (const icon_file *f = e->files; f != nullptr; f = f->next) {
        if (best == nullptr || f->theme < best->theme) {
            best = f;
            continue;
        }
        if (f->theme > best->theme)
            continue;
        bool f_fits = f->size == XD_ICON_SCALABLE || f->size >= px;
        bool b_fits = best->size == XD_ICON_SCALABLE || best->size >= px;
        if (f->size == px && best->size != px)
            best = f;
        else if (best->size == px)
            continue;
        else if (f_fits && !b_fits)
            best = f;
        else if (f_fits && b_fits) {
            /* both fit: prefer the closer bitmap, then scalable */
            if (best->size == XD_ICON_SCALABLE && f->size != XD_ICON_SCALABLE)
                best = f;
            else if (f->size != XD_ICON_SCALABLE && f->size < best->size)
                best = f;
        } else if (!f_fits && !b_fits && f->size > best->size) {
            best = f;
        }
    }
    return best;
}

static cairo_surface_t *load_png(const char *path, int px) {
    cairo_surface_t *src = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(src);
        return nullptr;
    }
    int w = cairo_image_surface_get_width(src);
    int h = cairo_image_surface_get_height(src);
    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(dst);
    if (w > 0 && h > 0) {
        double s = (double)px / XD_MAX(w, h);
        cairo_translate(cr, (px - w * s) / 2, (px - h * s) / 2);
        cairo_scale(cr, s, s);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(src);
    return dst;
}

static cairo_surface_t *load_svg(const char *path, int px) {
    GError *err = nullptr;
    RsvgHandle *h = rsvg_handle_new_from_file(path, &err);
    if (h == nullptr) {
        g_clear_error(&err);
        return nullptr;
    }
    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(dst);
    RsvgRectangle viewport = {0, 0, px, px};
    bool ok = rsvg_handle_render_document(h, cr, &viewport, &err);
    cairo_destroy(cr);
    g_object_unref(h);
    g_clear_error(&err);
    if (!ok) {
        cairo_surface_destroy(dst);
        return nullptr;
    }
    return dst;
}

static cairo_surface_t *load_file(const char *path, int px) {
    xd_str p = xd_str_from_cstr(path);
    size_t n = p.len;
    if (n >= 4 && xd_str_eq(xd_str_sub(p, n - 4, 4), XD_STR(".png")))
        return load_png(path, px);
    return load_svg(path, px);
}

cairo_surface_t *xd_icons_get(xd_icons *ic, const char *name, int px) {
    if (name == nullptr || name[0] == '\0' || px <= 0 || px > 1024)
        return nullptr;
    for (size_t i = 0; i < ic->cache_len; i++) {
        if (ic->cache[i].px == px && strcmp(ic->cache[i].name, name) == 0)
            return ic->cache[i].surface;
    }

    cairo_surface_t *surface = nullptr;
    if (name[0] == '/') {
        surface = load_file(name, px);
    } else {
        icon_entry *e = index_slot(ic, xd_str_from_cstr(name));
        const icon_file *f = e->name != nullptr ? pick(e, px) : nullptr;
        if (f != nullptr)
            surface = load_file(f->path, px);
    }

    if (ic->cache_len == ic->cache_cap) {
        ic->cache_cap = ic->cache_cap == 0 ? 64 : ic->cache_cap * 2;
        ic->cache = xd_realloc_array(ic->cache, ic->cache_cap, sizeof(*ic->cache));
    }
    ic->cache[ic->cache_len++] = (cache_entry){
        .name = xd_str_arena_cstr(&ic->arena, xd_str_from_cstr(name)),
        .px = px,
        .surface = surface,
    };
    return surface;
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
