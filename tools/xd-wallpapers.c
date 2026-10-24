/* SPDX-License-Identifier: MIT
 * Paints Energy, one of the wallpapers XeonDesktop installs
 * (data/wallpapers), from noise, nothing taken from Windows; the others
 * are pictures made for the project. Run it once after changing it and
 * commit the picture; the build doesn't run it.
 *
 *   xd-wallpapers DIR [WIDTH HEIGHT]
 */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rgb {
    double r, g, b;
} rgb;

static rgb mix(rgb from, rgb to, double amount) {
    return (rgb){from.r + (to.r - from.r) * amount, from.g + (to.g - from.g) * amount,
                 from.b + (to.b - from.b) * amount};
}
static rgb scale(rgb color, double k) { return (rgb){color.r * k, color.g * k, color.b * k}; }
static rgb add(rgb first, rgb second) {
    return (rgb){first.r + second.r, first.g + second.g, first.b + second.b};
}
static rgb hex(uint32_t value) {
    return (rgb){((value >> 16) & 0xFF) / 255.0, ((value >> 8) & 0xFF) / 255.0,
                 (value & 0xFF) / 255.0};
}

static double clamp01(double value) { return value < 0 ? 0 : value > 1 ? 1 : value; }
static double smooth(double e0, double e1, double x) {
    double fraction = clamp01((x - e0) / (e1 - e0));
    return fraction * fraction * (3 - 2 * fraction);
}

/* ---- noise: value noise, its fractal sum, and a warp ----------------------- */

static uint32_t hash3(int32_t x, int32_t y, uint32_t seed) {
    uint32_t hash = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    hash ^= hash >> 13;
    hash *= 0x5bd1e995u;
    hash ^= hash >> 15;
    return hash;
}

static double lattice(int32_t x, int32_t y, uint32_t seed) {
    return (hash3(x, y, seed) & 0xFFFFFF) / (double)0xFFFFFF;
}

/* ---- the picture ------------------------------------------------------------ */

typedef struct canvas {
    int w, h;
    rgb *px;
} canvas;

typedef rgb (*painter)(double horizontal, double vertical, double aspect, int width, int height);

static rgb paint_energy(double horizontal, double vertical, double aspect, int image_width,
                        int image_height) {
    rgb color = mix(hex(0x061A5C), hex(0x0E3F9E), clamp01(0.55 * horizontal + 0.6 * vertical));
    double gx = (horizontal - 0.3) * aspect;
    double gy = vertical - 0.55;
    color = add(color, scale(hex(0x1C5BD8), 0.55 * exp(-(gx * gx + gy * gy) / 0.25)));
    /* Ribbons of light flowing across. */
    for (int k = 0; k < 6; k++) {
        double phase = k * 0.9;
        double center = 0.52 + 0.13 * sin(horizontal * 6.2832 * (0.55 + k * 0.05) + phase) +
                        0.05 * sin(horizontal * 6.2832 * 1.7 + phase * 2.3) + (k - 2.5) * 0.025;
        double distance = (vertical - center) * image_height;
        double width = 40.0 + k * 9.0;
        double glow = exp(-(distance * distance) / (width * width));
        double core = exp(-(distance * distance) / 4.0);
        double fade = smooth(0.0, 0.25, horizontal) * smooth(1.0, 0.7, horizontal) * 0.7 + 0.3;
        color = add(color, scale(hex(0x4FA3FF), 0.13 * glow * fade));
        color = add(color, scale(hex(0xCFE8FF), 0.30 * core * fade));
    }
    /* A few soft out-of-focus lights. */
    for (int i = 0; i < 40; i++) {
        double bx = lattice(i, 1, 81u);
        double by = 0.3 + 0.5 * lattice(i, 2, 81u);
        double br = (0.006 + 0.02 * lattice(i, 3, 81u)) * image_width;
        double dx = (horizontal - bx) * image_width;
        double dy = (vertical - by) * image_height;
        double disc = smooth(br, br * 0.8, sqrt(dx * dx + dy * dy));
        color = add(color, scale(hex(0x7CC0FF), 0.06 * disc));
    }
    return color;
}


static void render(canvas *cv, painter paint) {
    double aspect = (double)cv->w / cv->h;
    for (int y = 0; y < cv->h; y++) {
        double vertical = (y + 0.5) / cv->h;
        for (int x = 0; x < cv->w; x++) {
            double horizontal = (x + 0.5) / cv->w;
            cv->px[(size_t)y * (size_t)cv->w + (size_t)x] =
                paint(horizontal, vertical, aspect, cv->w, cv->h);
        }
    }
}

/* To JPEG, dithered so the long gradients don't band. */
static bool save(const canvas *cv, const char *path) {
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, cv->w, cv->h);
    if (pb == nullptr)
        return false;
    guchar *data = gdk_pixbuf_get_pixels(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    for (int y = 0; y < cv->h; y++) {
        for (int x = 0; x < cv->w; x++) {
            rgb color = cv->px[(size_t)y * (size_t)cv->w + (size_t)x];
            double dither = lattice(x, y, 1234u) - 0.5;
            double ch[3] = {color.r, color.g, color.b};
            for (int i = 0; i < 3; i++) {
                double level = clamp01(ch[i]) * 255.0 + dither + 0.5;
                level = level < 0 ? 0 : level > 255 ? 255 : level;
                data[(size_t)y * (size_t)stride + (size_t)x * 3 + (size_t)i] = (guchar)level;
            }
        }
    }
    GError *err = nullptr;
    bool ok = gdk_pixbuf_save(pb, path, "jpeg", &err, "quality", "92", nullptr);
    if (!ok) {
        fprintf(stderr, "xd-wallpapers: %s: %s\n", path, err->message);
        g_error_free(err);
    }
    g_object_unref(pb);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 4) {
        fprintf(stderr, "usage: xd-wallpapers DIR [WIDTH HEIGHT]\n");
        return 2;
    }
    canvas cv = {.w = 3840, .h = 2160};
    if (argc == 4) {
        cv.w = atoi(argv[2]);
        cv.h = atoi(argv[3]);
        if (cv.w < 16 || cv.h < 16 || cv.w > 16384 || cv.h > 16384) {
            fprintf(stderr, "xd-wallpapers: bad size\n");
            return 2;
        }
    }
    static const struct {
        const char *name;
        painter paint;
    } pictures[] = {
        {"Energy", paint_energy},
    };
    cv.px = calloc((size_t)cv.w * (size_t)cv.h, sizeof(rgb));
    if (cv.px == nullptr)
        return 1;
    int status = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(pictures); i++) {
        render(&cv, pictures[i].paint);
        char *path = g_strdup_printf("%s/%s.jpg", argv[1], pictures[i].name);
        if (!save(&cv, path))
            status = 1;
        else
            printf("%s\n", path);
        g_free(path);
    }
    free(cv.px);
    return status;
}
