/* SPDX-License-Identifier: MIT
 * TEMPORARY: nearest and smooth only, until the scalers land. */
#include "scale.h"

#include <glib.h>
#include <math.h>

static const char *const filter_names[XD_FILTER_COUNT] = {
    "Nearest", "Integer", "Smooth", "Scale2x", "Scale3x", "Scale4x", "2xSaI", "Super2xSaI",
    "SuperEagle",
};

const char *xd_image_filter_name(xd_image_filter f) {
    return (unsigned)f < XD_FILTER_COUNT ? filter_names[f] : "Smooth";
}

bool xd_image_filter_from_name(const char *name, xd_image_filter *out) {
    for (int i = 0; i < XD_FILTER_COUNT; i++)
        if (name != nullptr && g_ascii_strcasecmp(name, filter_names[i]) == 0) {
            *out = (xd_image_filter)i;
            return true;
        }
    return false;
}

static cairo_surface_t *resize(cairo_surface_t *src, int w, int h, bool nearest) {
    int sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src);
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(out);
    cairo_scale(cr, (double)w / sw, (double)h / sh);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(cairo_get_source(cr),
                             nearest ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    return out;
}

cairo_surface_t *xd_image_scale(cairo_surface_t *src, double scale, xd_image_filter filter) {
    int w = MAX(1, (int)lround(cairo_image_surface_get_width(src) * scale));
    int h = MAX(1, (int)lround(cairo_image_surface_get_height(src) * scale));
    return resize(src, w, h, filter != XD_FILTER_SMOOTH);
}

cairo_surface_t *xd_image_scale_frames(cairo_surface_t *src, int count, bool vertical,
                                       double scale, xd_image_filter filter) {
    int sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src);
    int fw = vertical ? sw : sw / count, fh = vertical ? sh / count : sh;
    int ow = MAX(1, (int)lround(fw * scale)), oh = MAX(1, (int)lround(fh * scale));
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                      vertical ? ow : ow * count,
                                                      vertical ? oh * count : oh);
    cairo_t *cr = cairo_create(out);
    for (int i = 0; i < count; i++) {
        cairo_surface_t *f = cairo_surface_create_for_rectangle(src, vertical ? 0 : i * fw,
                                                                vertical ? i * fh : 0, fw, fh);
        cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, fw, fh);
        cairo_t *c2 = cairo_create(img);
        cairo_set_source_surface(c2, f, 0, 0);
        cairo_paint(c2);
        cairo_destroy(c2);
        cairo_surface_t *s = xd_image_scale(img, scale, filter);
        cairo_set_source_surface(cr, s, vertical ? 0 : i * ow, vertical ? i * oh : 0);
        cairo_rectangle(cr, vertical ? 0 : i * ow, vertical ? i * oh : 0, ow, oh);
        cairo_fill(cr);
        cairo_surface_destroy(s);
        cairo_surface_destroy(img);
        cairo_surface_destroy(f);
    }
    cairo_destroy(cr);
    return out;
}
