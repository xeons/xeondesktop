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

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    const guchar *src = gdk_pixbuf_read_pixels(pb);
    unsigned char *dst = cairo_image_surface_get_data(surface);
    int dst_stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < ph; y++) {
        const guchar *in = src + (size_t)y * (size_t)src_stride;
        unsigned char *out_row = dst + (size_t)y * (size_t)dst_stride;
        for (int x = 0; x < pw; x++) {
            const guchar *pixel = in + (size_t)x * (size_t)channels;
            uint32_t pixel_alpha = alpha ? pixel[3] : 255u;
            /* cairo wants premultiplied native-endian ARGB */
            uint32_t red = (pixel[0] * pixel_alpha + 127u) / 255u;
            uint32_t green = (pixel[1] * pixel_alpha + 127u) / 255u;
            uint32_t blue = (pixel[2] * pixel_alpha + 127u) / 255u;
            uint32_t px = (pixel_alpha << 24) | (red << 16) | (green << 8) | blue;
            memcpy(out_row + (size_t)x * 4, &px, sizeof(px));
        }
    }
    cairo_surface_mark_dirty(surface);
    return surface;
}

bool xd_image_size(const char *path, int *width, int *height) {
    *width = 0;
    *height = 0;
    return path != nullptr && path[0] == '/' &&
           gdk_pixbuf_get_file_info(path, width, height) != nullptr && *width > 0 && *height > 0;
}

cairo_surface_t *xd_image_load(const char *path, int max_w, int max_h) {
    if (path == nullptr || path[0] != '/' || max_w <= 0 || max_h <= 0)
        return nullptr;
    int width = 0;
    int height = 0;
    if (gdk_pixbuf_get_file_info(path, &width, &height) == nullptr || width <= 0 || height <= 0)
        return nullptr;

    GError *err = nullptr;
    GdkPixbuf *pb = (width > max_w || height > max_h)
                        ? gdk_pixbuf_new_from_file_at_size(path, max_w, max_h, &err)
                        : gdk_pixbuf_new_from_file(path, &err);
    g_clear_error(&err);
    if (pb == nullptr)
        return nullptr;

    cairo_surface_t *surface = xd_image_from_pixbuf(pb);
    g_object_unref(pb);
    return surface;
}

void xd_image_paint_wallpaper(cairo_t *cairo, cairo_surface_t *image, xd_rect rect, int mode,
                              double pixel_scale) {
    if (image == nullptr || xd_rect_empty(rect))
        return;
    double iw = cairo_image_surface_get_width(image);
    double ih = cairo_image_surface_get_height(image);
    if (iw <= 0 || ih <= 0)
        return;

    cairo_save(cairo);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_clip(cairo);
    double sx = pixel_scale;
    double sy = pixel_scale;
    double x = rect.x;
    double y = rect.y;
    switch ((xd_wallpaper_mode)mode) {
    case XD_WALLPAPER_CENTER:
        x = rect.x + (rect.w - iw * sx) / 2;
        y = rect.y + (rect.h - ih * sy) / 2;
        break;
    case XD_WALLPAPER_TILE:
        break;
    case XD_WALLPAPER_STRETCH:
        sx = rect.w / iw;
        sy = rect.h / ih;
        break;
    case XD_WALLPAPER_FILL:
    case XD_WALLPAPER_FIT: {
        double scale = mode == XD_WALLPAPER_FILL ? fmax(rect.w / iw, rect.h / ih)
                                                 : fmin(rect.w / iw, rect.h / ih);
        sx = scale;
        sy = scale;
        x = rect.x + (rect.w - iw * scale) / 2;
        y = rect.y + (rect.h - ih * scale) / 2;
        break;
    }
    case XD_WALLPAPER_MODE_COUNT:
        break;
    }
    cairo_translate(cairo, x, y);
    cairo_scale(cairo, sx, sy);
    cairo_set_source_surface(cairo, image, 0, 0);
    cairo_pattern_t *pat = cairo_get_source(cairo);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_GOOD);
    if (mode == XD_WALLPAPER_TILE)
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    cairo_paint(cairo);
    cairo_restore(cairo);
}
