/* SPDX-License-Identifier: MIT
 * Scales a PNG with every theme image filter (src/theme/scale.h), one
 * output PNG per filter, to compare them by eye.
 *
 *   xd-scale-demo IN.png SCALE OUT_DIR
 *
 * writes OUT_DIR/<Filter>.png (Nearest.png, Integer.png, ... SuperEagle.png).
 */
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/theme/scale.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: xd-scale-demo IN.png SCALE OUT_DIR\n");
        return EXIT_FAILURE;
    }
    char *end = nullptr;
    double scale = g_ascii_strtod(argv[2], &end);
    if (end == argv[2] || *end || !(scale > 0)) {
        fprintf(stderr, "xd-scale-demo: bad scale: %s\n", argv[2]);
        return EXIT_FAILURE;
    }
    cairo_surface_t *in = cairo_image_surface_create_from_png(argv[1]);
    if (cairo_surface_status(in) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "xd-scale-demo: %s: %s\n", argv[1],
                cairo_status_to_string(cairo_surface_status(in)));
        cairo_surface_destroy(in);
        return EXIT_FAILURE;
    }
    if (g_mkdir_with_parents(argv[3], 0755) != 0) {
        fprintf(stderr, "xd-scale-demo: %s: %s\n", argv[3], g_strerror(errno));
        cairo_surface_destroy(in);
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;
    for (int filter = 0; filter < XD_FILTER_COUNT; filter++) {
        const char *name = xd_image_filter_name((xd_image_filter)filter);
        cairo_surface_t *out = xd_image_scale(in, scale, (xd_image_filter)filter);
        if (!out) {
            fprintf(stderr, "xd-scale-demo: %s: scaling failed\n", name);
            status = EXIT_FAILURE;
            continue;
        }
        char *file = g_strdup_printf("%s.png", name);
        char *path = g_build_filename(argv[3], file, nullptr);
        cairo_status_t st = cairo_surface_write_to_png(out, path);
        if (st != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "xd-scale-demo: %s: %s\n", path, cairo_status_to_string(st));
            status = EXIT_FAILURE;
        } else {
            printf("%s (%dx%d)\n", path, cairo_image_surface_get_width(out),
                   cairo_image_surface_get_height(out));
        }
        g_free(path);
        g_free(file);
        cairo_surface_destroy(out);
    }
    cairo_surface_destroy(in);
    return status;
}
