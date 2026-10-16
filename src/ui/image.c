/* SPDX-License-Identifier: MIT */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>

#include "../settings/settings.h"
#include "image.h"

cairo_surface_t *xd_image_from_pixbuf(GdkPixbuf *pb) {
    int pw = gdk_pixbuf_get_width(pb);
    int ph = gdk_pixbuf_get_height(pb);
    int channels = gdk_pixbuf_get_n_channels(pb);
    int src_stride = gdk_pixbuf_get_rowstride(pb);
    bool alpha = gdk_pixbuf_get_has_alpha(pb);
    if (gdk_pixbuf_get_colorspace(pb) != GDK_COLORSPACE_RGB ||
        gdk_pixbuf_get_bits_per_sample(pb) != 8 || channels < 3 || channels > 4 ||
        src_stride <= 0) {
        return nullptr;
    }

    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    const guchar *src = gdk_pixbuf_read_pixels(pb);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int dst_stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < ph; y++) {
        const guchar *in = src + (size_t)y * (size_t)src_stride;
        unsigned char *out_row = dst + (size_t)y * (size_t)dst_stride;
        for (int x = 0; x < pw; x++) {
            const guchar *p = in + (size_t)x * (size_t)channels;
            uint32_t a = alpha ? p[3] : 255u;
            /* cairo wants premultiplied native-endian ARGB */
            uint32_t r = (p[0] * a + 127u) / 255u;
            uint32_t g = (p[1] * a + 127u) / 255u;
            uint32_t b = (p[2] * a + 127u) / 255u;
            uint32_t px = (a << 24) | (r << 16) | (g << 8) | b;
            memcpy(out_row + (size_t)x * 4, &px, sizeof(px));
        }
    }
    cairo_surface_mark_dirty(s);
    return s;
}

bool xd_image_size(const char *path, int *w, int *h) {
    *w = 0;
    *h = 0;
    return path != nullptr && path[0] == '/' && gdk_pixbuf_get_file_info(path, w, h) != nullptr &&
           *w > 0 && *h > 0;
}

cairo_surface_t *xd_image_load(const char *path, int max_w, int max_h) {
    if (path == nullptr || path[0] != '/' || max_w <= 0 || max_h <= 0)
        return nullptr;
    int w = 0;
    int h = 0;
    if (gdk_pixbuf_get_file_info(path, &w, &h) == nullptr || w <= 0 || h <= 0)
        return nullptr;

    GError *err = nullptr;
    GdkPixbuf *pb = (w > max_w || h > max_h)
                        ? gdk_pixbuf_new_from_file_at_size(path, max_w, max_h, &err)
                        : gdk_pixbuf_new_from_file(path, &err);
    g_clear_error(&err);
    if (pb == nullptr)
        return nullptr;

    cairo_surface_t *s = xd_image_from_pixbuf(pb);
    g_object_unref(pb);
    return s;
}

void xd_image_paint_wallpaper(cairo_t *cr, cairo_surface_t *image, xd_rect r, int mode,
                              double pixel_scale) {
    if (image == nullptr || xd_rect_empty(r))
        return;
    double iw = cairo_image_surface_get_width(image);
    double ih = cairo_image_surface_get_height(image);
    if (iw <= 0 || ih <= 0)
        return;

    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    double sx = pixel_scale;
    double sy = pixel_scale;
    double x = r.x;
    double y = r.y;
    switch ((xd_wallpaper_mode)mode) {
    case XD_WALLPAPER_CENTER:
        x = r.x + (r.w - iw * sx) / 2;
        y = r.y + (r.h - ih * sy) / 2;
        break;
    case XD_WALLPAPER_TILE:
        break;
    case XD_WALLPAPER_STRETCH:
        sx = r.w / iw;
        sy = r.h / ih;
        break;
    case XD_WALLPAPER_FILL:
    case XD_WALLPAPER_FIT: {
        double s = mode == XD_WALLPAPER_FILL ? fmax(r.w / iw, r.h / ih) : fmin(r.w / iw, r.h / ih);
        sx = s;
        sy = s;
        x = r.x + (r.w - iw * s) / 2;
        y = r.y + (r.h - ih * s) / 2;
        break;
    }
    case XD_WALLPAPER_MODE_COUNT:
        break;
    }
    cairo_translate(cr, x, y);
    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_pattern_t *pat = cairo_get_source(cr);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_GOOD);
    if (mode == XD_WALLPAPER_TILE)
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    cairo_paint(cr);
    cairo_restore(cr);
}
