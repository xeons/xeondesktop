/* SPDX-License-Identifier: MIT
 * Loading pictures (wallpapers, previews) into cairo surfaces.
 */
#ifndef XD_UI_IMAGE_H
#define XD_UI_IMAGE_H

#include <cairo.h>

#include "../core/common.h"
#include "../theme/draw.h"

/* Loads PNG/JPEG/WebP/... via gdk-pixbuf, scaled down (keeping the aspect
 * ratio) to fit max_w x max_h. nullptr on failure. The caller owns the
 * surface. Only for files the user picked: wallpapers. */
[[nodiscard]] cairo_surface_t *xd_image_load(const char *path, int max_w, int max_h);

/* Converts a GdkPixbuf (8-bit RGB/RGBA) to a cairo image surface. */
typedef struct _GdkPixbuf GdkPixbuf;
[[nodiscard]] cairo_surface_t *xd_image_from_pixbuf(GdkPixbuf *pb);

/* Pixel size of an image file without decoding it. */
[[nodiscard]] bool xd_image_size(const char *path, int *width, int *height);

/* Paints `image` into `rect` the way Windows paints wallpaper in each mode
 * (enum xd_wallpaper_mode values: center, tile, stretch, fill, fit).
 * `pixel_scale` maps image pixels to device pixels for center/tile. */
void xd_image_paint_wallpaper(cairo_t *cairo, cairo_surface_t *image, xd_rect rect, int mode,
                              double pixel_scale);

#endif
