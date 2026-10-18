/* SPDX-License-Identifier: MIT
 * Makes the wallpapers XeonDesktop installs (data/wallpapers): one for
 * each look, painted from noise, nothing taken from Windows. Run it once
 * after changing it and commit the pictures; the build doesn't run it.
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

static rgb mix(rgb a, rgb b, double t) {
    return (rgb){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
static rgb scale(rgb a, double k) { return (rgb){a.r * k, a.g * k, a.b * k}; }
static rgb add(rgb a, rgb b) { return (rgb){a.r + b.r, a.g + b.g, a.b + b.b}; }
static rgb hex(uint32_t c) {
    return (rgb){((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0};
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static double smooth(double e0, double e1, double x) {
    double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}

/* ---- noise: value noise, its fractal sum, and a warp ----------------------- */

static uint32_t hash3(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

static double lattice(int32_t x, int32_t y, uint32_t seed) {
    return (hash3(x, y, seed) & 0xFFFFFF) / (double)0xFFFFFF;
}

static double vnoise(double x, double y, uint32_t seed) {
    double fx = floor(x);
    double fy = floor(y);
    int32_t ix = (int32_t)fx;
    int32_t iy = (int32_t)fy;
    double tx = x - fx;
    double ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    double a = lattice(ix, iy, seed);
    double b = lattice(ix + 1, iy, seed);
    double c = lattice(ix, iy + 1, seed);
    double d = lattice(ix + 1, iy + 1, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

/* 0..1, about 0.5 on average. */
static double fbm(double x, double y, int octaves, uint32_t seed) {
    double sum = 0;
    double amp = 0.5;
    double norm = 0;
    for (int i = 0; i < octaves; i++) {
        sum += amp * vnoise(x, y, seed + (uint32_t)i * 101u);
        norm += amp;
        x = x * 2.03 + 17.1;
        y = y * 2.01 - 9.3;
        amp *= 0.5;
    }
    return sum / norm;
}

/* fbm of a point moved by two other fbm fields: flowing, washed shapes. */
static double warped(double x, double y, double strength, int octaves, uint32_t seed) {
    double wx = fbm(x + 3.1, y - 1.7, 4, seed + 7u) - 0.5;
    double wy = fbm(x - 5.2, y + 2.9, 4, seed + 13u) - 0.5;
    return fbm(x + strength * wx, y + strength * wy, octaves, seed);
}

/* ---- the picture ------------------------------------------------------------ */

typedef struct canvas {
    int w, h;
    rgb *px;
} canvas;

typedef rgb (*painter)(double u, double v, double aspect, int w, int h);

/* Sky from `top` to `horizon`, with clouds stretched along the horizon. */
static rgb sky(double u, double v, double aspect, double horizon_v, rgb top, rgb low, rgb cloud,
               double cover, uint32_t seed) {
    double t = clamp01(v / horizon_v);
    rgb c = mix(top, low, pow(t, 1.4));
    double n = warped(u * aspect * 1.6, v * 3.6, 0.5, 7, seed);
    double puff = smooth(cover, cover + 0.22, n) * (1 - smooth(0.7, 1.0, t));
    /* Lit from above: a cloud's underside is a little grey. */
    double under = warped(u * aspect * 1.6, v * 3.6 + 0.08, 0.5, 7, seed) - n;
    rgb shade = mix(cloud, scale(cloud, 0.82), clamp01(0.5 + under * 4));
    return mix(c, shade, puff * 0.92);
}

/* Height of a rolling ridge at u (0 left, 1 right), in v. */
static double ridge(double u, double base, double amp, double freq, double phase, uint32_t seed) {
    /* Broad and smooth: two slow waves, and a little noise slower still. */
    return base - amp * (0.55 * sin(u * freq * 6.2832 + phase) +
                         0.20 * sin(u * freq * 1.9 * 6.2832 + phase * 1.7) +
                         0.45 * (fbm(u * freq * 1.2, 0.5 + seed * 0.37, 2, seed) - 0.5));
}

/* A grassy hill below `top_v`: lighter at its crest, lit from the left. */
static rgb hill(double u, double v, double aspect, double top_v, rgb crest, rgb foot, double light,
                uint32_t seed) {
    double depth = clamp01((v - top_v) / (1 - top_v + 1e-6));
    rgb c = mix(crest, foot, pow(depth, 0.7));
    double slope = ridge(u + 0.002, 0, 1, 1, 0, seed) - ridge(u - 0.002, 0, 1, 1, 0, seed);
    c = scale(c, 1 + light * (0.5 - u) * 0.25 + slope * 2.0);
    /* Grass: fine streaks, longer than wide. */
    double g = fbm(u * aspect * 260, v * 70, 3, seed + 99u);
    return scale(c, 0.93 + 0.14 * g);
}

/* Covers a hill's edge over the pixel below it (no jagged horizon). */
static double edge(double v, double top_v, int h) { return smooth(-0.6, 0.6, (v - top_v) * h); }

static rgb paint_rolling_hills(double u, double v, double aspect, [[maybe_unused]] int w, int h) {
    double far_v = ridge(u, 0.60, 0.06, 0.9, 1.2, 41u);
    double near_v = ridge(u, 0.74, 0.10, 0.5, 2.4, 42u) - 0.14 * exp(-pow((u - 0.33) / 0.34, 2));
    rgb c = sky(u, v, aspect, 0.66, hex(0x1F57C3), hex(0xA6CDF3), hex(0xFFFFFF), 0.48, 7u);
    rgb far = hill(u, v, aspect, far_v, hex(0x7FB25A), hex(0x5E9440), 0.5, 41u);
    far = mix(far, hex(0xA6CDF3), 0.18); /* haze */
    c = mix(c, far, edge(v, far_v, h));
    rgb nearc = hill(u, v, aspect, near_v, hex(0x6CC23A), hex(0x2B7417), 1.0, 42u);
    return mix(c, nearc, edge(v, near_v, h));
}

static rgb paint_olive_meadow(double u, double v, double aspect, [[maybe_unused]] int w, int h) {
    double far_v = ridge(u, 0.62, 0.06, 0.8, 0.4, 51u);
    double near_v = ridge(u, 0.75, 0.10, 0.5, 3.1, 52u) - 0.11 * exp(-pow((u - 0.25) / 0.32, 2));
    rgb c = sky(u, v, aspect, 0.66, hex(0x6F8C8A), hex(0xF4DFA0), hex(0xFFF2D6), 0.62, 9u);
    /* The low sun behind the far hill, right of centre. */
    double du = (u - 0.72) * aspect;
    double dv = v - 0.6;
    double sun = exp(-(du * du + dv * dv) / 0.02);
    c = add(c, scale(hex(0xFFE2A0), 0.55 * sun));
    rgb far = hill(u, v, aspect, far_v, hex(0x9C9A5A), hex(0x7D7C40), 0.3, 51u);
    far = mix(far, hex(0xEAD79A), 0.4);
    c = mix(c, far, edge(v, far_v, h));
    rgb nearc = hill(u, v, aspect, near_v, hex(0x8E9A3C), hex(0x434A16), -0.8, 52u);
    /* Warm light along the crest from the low sun. */
    double rim = exp(-(v - near_v) * 90) * smooth(0.2, 1.0, u);
    nearc = add(nearc, scale(hex(0xFFD98A), 0.25 * rim));
    return mix(c, nearc, edge(v, near_v, h));
}

static rgb paint_silver_mist(double u, double v, double aspect, [[maybe_unused]] int w, int h) {
    rgb c = sky(u, v, aspect, 0.6, hex(0xC9D0DA), hex(0xF2F3F5), hex(0xFBFBFC), 0.6, 11u);
    /* Ridges from far to near, each darker and less misty. */
    static const uint32_t ridge_colors[] = {0xC5CBD4, 0xA9B0BB, 0x8A919E, 0x5F6672};
    for (int i = 0; i < 4; i++) {
        double top = ridge(u, 0.50 + i * 0.09, 0.05 + i * 0.015, 0.7 + i * 0.25, i * 1.9,
                           61u + (uint32_t)i);
        rgb r = hex(ridge_colors[i]);
        double depth = clamp01((v - top) * 6);
        r = mix(r, scale(r, 0.9), depth);
        /* Mist lying in the valley below each ridge. */
        double fog = fbm(u * aspect * 1.2 + i * 7.0, v * 5, 5, 71u + (uint32_t)i);
        double mist = smooth(0.0, 0.16, v - top) * (0.30 + 0.35 * fog);
        r = mix(r, hex(0xEEF0F3), mist);
        c = mix(c, r, edge(v, top, h));
    }
    return c;
}

static rgb paint_energy(double u, double v, double aspect, int w, int h) {
    rgb c = mix(hex(0x061A5C), hex(0x0E3F9E), clamp01(0.55 * u + 0.6 * v));
    double gx = (u - 0.3) * aspect;
    double gy = v - 0.55;
    c = add(c, scale(hex(0x1C5BD8), 0.55 * exp(-(gx * gx + gy * gy) / 0.25)));
    /* Ribbons of light flowing across. */
    for (int k = 0; k < 6; k++) {
        double phase = k * 0.9;
        double center = 0.52 + 0.13 * sin(u * 6.2832 * (0.55 + k * 0.05) + phase) +
                        0.05 * sin(u * 6.2832 * 1.7 + phase * 2.3) + (k - 2.5) * 0.025;
        double d = (v - center) * h;
        double width = 40.0 + k * 9.0;
        double glow = exp(-(d * d) / (width * width));
        double core = exp(-(d * d) / 4.0);
        double fade = smooth(0.0, 0.25, u) * smooth(1.0, 0.7, u) * 0.7 + 0.3;
        c = add(c, scale(hex(0x4FA3FF), 0.13 * glow * fade));
        c = add(c, scale(hex(0xCFE8FF), 0.30 * core * fade));
    }
    /* A few soft out-of-focus lights. */
    for (int i = 0; i < 40; i++) {
        double bx = lattice(i, 1, 81u);
        double by = 0.3 + 0.5 * lattice(i, 2, 81u);
        double br = (0.006 + 0.02 * lattice(i, 3, 81u)) * w;
        double dx = (u - bx) * w;
        double dy = (v - by) * h;
        double disc = smooth(br, br * 0.8, sqrt(dx * dx + dy * dy));
        c = add(c, scale(hex(0x7CC0FF), 0.06 * disc));
    }
    return c;
}

/* Watercolor's caption mosaic, large: a light blue gradient at the top
 * left, a broad diagonal band of square tiles with grout where light tiles
 * thin out among dark ones (a few bright ones straying), and a dark blue
 * gradient at the bottom right. Each tile is lit a little at its top. */
static rgb paint_watercolor_blue(double u, double v, double aspect, int w, int h) {
    double s = clamp01((u * aspect * 0.55 + v * 0.75) / (aspect * 0.55 + 0.75));
    rgb light = mix(hex(0x9CC6FD), hex(0x5297F9), v);
    rgb dark = mix(hex(0x3573D6), hex(0x173F95), v * 0.6 + u * 0.4);
    /* Flat gradients outside the band, the mosaic inside it. */
    double band = smooth(0.18, 0.36, s) * (1 - smooth(0.72, 0.9, s));
    rgb flat = mix(light, dark, smooth(0.2, 0.85, s));
    double pitch = h / 24.0;
    double grout = pitch * 0.06;
    double px = u * w;
    double py = v * h;
    double cx = floor(px / pitch);
    double cy = floor(py / pitch);
    int32_t tx = (int32_t)cx;
    int32_t ty = (int32_t)cy;
    double lx = px - cx * pitch;
    double ly = py - cy * pitch;
    /* Each tile decides once, at its centre: light or dark, bright or not. */
    double su = (cx + 0.5) * pitch / w;
    double sv = (cy + 0.5) * pitch / h;
    double ts = clamp01((su * aspect * 0.55 + sv * 0.75) / (aspect * 0.55 + 0.75));
    double r = lattice(tx, ty, 31u);
    double p_light = 1 - smooth(0.25, 0.8, ts);
    rgb tile = r < p_light ? light : dark;
    double tband = smooth(0.18, 0.36, ts) * (1 - smooth(0.72, 0.9, ts));
    if (lattice(tx, ty, 32u) < 0.05 * tband)
        tile = mix(hex(0x78A9FF), hex(0xB7D5FF), lattice(tx, ty, 33u));
    tile = scale(tile, 1.06 - 0.12 * (ly / pitch)); /* lit at the top */
    tile = scale(tile, 0.96 + 0.08 * lattice(tx, ty, 34u));
    bool in_grout = lx < grout || ly < grout;
    rgb mosaic = in_grout ? mix(light, dark, 0.5) : tile;
    rgb c = mix(flat, mosaic, band);
    /* A soft light from the top left, and the corners a little darker. */
    double gx = (u - 0.15) * aspect;
    double gy = v - 0.1;
    c = add(c, scale(hex(0x6FA6F5), 0.25 * exp(-(gx * gx + gy * gy) / 0.5)));
    double vx = (u - 0.5) * aspect;
    double vy = v - 0.5;
    return scale(c, 1 - 0.18 * smooth(0.4, 1.4, vx * vx + vy * vy));
}

static void render(canvas *cv, painter paint) {
    double aspect = (double)cv->w / cv->h;
    for (int y = 0; y < cv->h; y++) {
        double v = (y + 0.5) / cv->h;
        for (int x = 0; x < cv->w; x++) {
            double u = (x + 0.5) / cv->w;
            cv->px[(size_t)y * (size_t)cv->w + (size_t)x] = paint(u, v, aspect, cv->w, cv->h);
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
            rgb c = cv->px[(size_t)y * (size_t)cv->w + (size_t)x];
            double dither = lattice(x, y, 1234u) - 0.5;
            double ch[3] = {c.r, c.g, c.b};
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
        {"Watercolor Blue", paint_watercolor_blue},
        {"Rolling Hills", paint_rolling_hills},
        {"Olive Meadow", paint_olive_meadow},
        {"Silver Mist", paint_silver_mist},
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
