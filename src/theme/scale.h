/* SPDX-License-Identifier: MIT
 * Scaling theme images for high DPI (docs/THEMES.md, "Scaling images for
 * high DPI"): the filters a theme package's images can be scaled with.
 */
#ifndef XD_THEME_SCALE_H
#define XD_THEME_SCALE_H

#include <cairo.h>
#include <stdbool.h>

typedef enum xd_image_filter {
    XD_FILTER_NEAREST,     /* hard pixels at any scale */
    XD_FILTER_INTEGER,     /* exact NxN blocks; other scales: integer prescale */
    XD_FILTER_SMOOTH,      /* bilinear */
    XD_FILTER_SCALE2X,     /* EPX: 2x */
    XD_FILTER_SCALE3X,     /* 3x */
    XD_FILTER_SCALE4X,     /* Scale2x twice */
    XD_FILTER_2XSAI,       /* 2x (twice for 4x) */
    XD_FILTER_SUPER2XSAI,  /* 2x (twice for 4x) */
    XD_FILTER_SUPEREAGLE,  /* 2x (twice for 4x) */
    XD_FILTER_COUNT
} xd_image_filter;

/* "Nearest", "Integer", "Smooth", "Scale2x", "Scale3x", "Scale4x",
 * "2xSaI", "Super2xSaI", "SuperEagle" (matched case-insensitively). */
const char *xd_image_filter_name(xd_image_filter f);
bool xd_image_filter_from_name(const char *name, xd_image_filter *out);

/* A new ARGB32 image surface: `src` (an ARGB32 image surface, cairo's
 * premultiplied alpha) scaled by `scale` (> 0) with `filter`. The size is
 * the source's times the scale, rounded to the nearest pixel (at least
 * 1). A scale of 1 copies.
 *
 * Pixel-art filters at their own factors (Scale2x at 2, Scale3x at 3,
 * Scale4x at 4, the SaI family at 2 and 4) give their output exactly. At
 * other scales they run at the smallest of their factors that reaches the
 * scale (or their largest, repeated), then scale down smoothly to the
 * size, as INTEGER does with whole-number blocks. Below 1, every filter
 * but NEAREST scales down smoothly.
 *
 * The pixel-art filters compare whole pixels, alpha included, so a
 * transparent edge is treated as its own colour and doesn't bleed. They
 * blend in premultiplied ARGB, all four channels alike, and read
 * neighbours past the image's edge as the nearest edge pixel. Smooth
 * reductions average the area each output pixel covers.
 *
 * Returns nullptr if `src` isn't an image surface, `scale` isn't a
 * positive number, `filter` isn't one of the above, or a size would pass
 * cairo's limit of 32767 pixels a side. Another image format than ARGB32
 * is converted first. */
cairo_surface_t *xd_image_scale(cairo_surface_t *src, double scale, xd_image_filter filter);

/* Scales a strip of `count` equal frames (laid out vertically or
 * horizontally) frame by frame and puts them back together, so no filter
 * reads across a frame's edge into the next one. Each frame's size is
 * scaled as xd_image_scale() scales a whole image. Returns nullptr as
 * xd_image_scale() does, and if the strip doesn't divide into `count`
 * equal frames. */
cairo_surface_t *xd_image_scale_frames(cairo_surface_t *src, int count, bool vertical,
                                       double scale, xd_image_filter filter);

#endif
