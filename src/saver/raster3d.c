/* SPDX-License-Identifier: MIT
 * The screen savers' software 3D renderer. See raster3d.h.
 *
 * Triangles are filled by edge functions over their bounding box, with
 * perspective-correct interpolation (weights divided by depth); each
 * pixel nearer than what's there is lit with the interpolated normal
 * (Phong shading), which keeps a pipe's highlight smooth however few
 * facets it has.
 */
#include "raster3d.h"

#include <math.h>

#include "../core/common.h"

static constexpr double NEAR_Z = 0.05;

static xd_r3_vec vec_add(xd_r3_vec a, xd_r3_vec b) {
    return (xd_r3_vec){a.x + b.x, a.y + b.y, a.z + b.z};
}
static xd_r3_vec vec_sub(xd_r3_vec a, xd_r3_vec b) {
    return (xd_r3_vec){a.x - b.x, a.y - b.y, a.z - b.z};
}
static xd_r3_vec vec_scale(xd_r3_vec a, double k) {
    return (xd_r3_vec){a.x * k, a.y * k, a.z * k};
}

/* A whole number of pixels, clamped to [lo, hi]. */
static int clamp_px(double value, int lo, int hi) {
    return value <= lo ? lo : value >= hi ? hi : (int)value;
}
static double vec_dot(xd_r3_vec a, xd_r3_vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static xd_r3_vec vec_cross(xd_r3_vec a, xd_r3_vec b) {
    return (xd_r3_vec){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static xd_r3_vec vec_normalize(xd_r3_vec a) {
    double length = sqrt(vec_dot(a, a));
    return length > 0 ? vec_scale(a, 1.0 / length) : a;
}

void xd_r3_clear(const xd_r3_target *target, uint32_t rgb) {
    for (int y = 0; y < target->height; y++) {
        uint32_t *row = target->pixels + (size_t)y * (size_t)target->stride;
        float *depth = target->depth + (size_t)y * (size_t)target->width;
        for (int x = 0; x < target->width; x++) {
            row[x] = rgb;
            depth[x] = INFINITY;
        }
    }
}

static uint32_t channel(double value) {
    double scaled = value * 255.0 + 0.5;
    return scaled <= 0 ? 0u : scaled >= 255 ? 255u : (uint32_t)scaled;
}

/* The lit colour at a point (camera space) with normal `n`. */
static uint32_t shade(const xd_r3_target *target, const xd_r3_material *material,
                      xd_r3_vec point, xd_r3_vec n) {
    xd_r3_vec view = vec_normalize(vec_scale(point, -1));
    n = vec_normalize(n);
    if (vec_dot(n, view) < 0) /* seen from inside: lit as its other face */
        n = vec_scale(n, -1);
    double diffuse = fmax(0.0, vec_dot(n, target->light));
    double light = target->ambient + (1.0 - target->ambient) * diffuse;
    double highlight = 0;
    if (material->specular > 0 && diffuse > 0) {
        xd_r3_vec half = vec_normalize(vec_add(target->light, view));
        highlight = material->specular * pow(fmax(0.0, vec_dot(n, half)), material->shininess);
    }
    return channel(material->r * light + highlight) << 16 |
           channel(material->g * light + highlight) << 8 | channel(material->b * light + highlight);
}

void xd_r3_triangle(const xd_r3_target *target, const xd_r3_vec position[3],
                    const xd_r3_vec normal[3], const xd_r3_material *material) {
    double sx[3], sy[3], inv_z[3];
    for (int i = 0; i < 3; i++) {
        if (position[i].z < NEAR_Z)
            return;
        inv_z[i] = 1.0 / position[i].z;
        sx[i] = target->cx + position[i].x * target->focal * inv_z[i];
        sy[i] = target->cy + position[i].y * target->focal * inv_z[i];
    }
    double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabs(area) < 1e-9)
        return;
    int x0 = clamp_px(floor(fmin(sx[0], fmin(sx[1], sx[2]))), 0, target->width - 1);
    int y0 = clamp_px(floor(fmin(sy[0], fmin(sy[1], sy[2]))), 0, target->height - 1);
    int x1 = clamp_px(ceil(fmax(sx[0], fmax(sx[1], sx[2]))), -1, target->width - 1);
    int y1 = clamp_px(ceil(fmax(sy[0], fmax(sy[1], sy[2]))), -1, target->height - 1);
    xd_r3_vec face =
        vec_cross(vec_sub(position[1], position[0]), vec_sub(position[2], position[0]));
    for (int y = y0; y <= y1; y++) {
        double py = y + 0.5;
        uint32_t *row = target->pixels + (size_t)y * (size_t)target->stride;
        float *depth = target->depth + (size_t)y * (size_t)target->width;
        for (int x = x0; x <= x1; x++) {
            double px = x + 0.5;
            /* Screen-space weights: each corner's share, by the edge
             * opposite it. */
            double w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
            double w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
            double w2 = 1.0 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            double inv = w0 * inv_z[0] + w1 * inv_z[1] + w2 * inv_z[2];
            double z = 1.0 / inv;
            if (z >= (double)depth[x])
                continue;
            /* Perspective-correct: weights over depth, renormalised. */
            double b0 = w0 * inv_z[0] * z, b1 = w1 * inv_z[1] * z, b2 = w2 * inv_z[2] * z;
            xd_r3_vec point = vec_add(
                vec_add(vec_scale(position[0], b0), vec_scale(position[1], b1)),
                vec_scale(position[2], b2));
            xd_r3_vec n = material->flat
                              ? face
                              : vec_add(vec_add(vec_scale(normal[0], b0), vec_scale(normal[1], b1)),
                                        vec_scale(normal[2], b2));
            depth[x] = (float)z;
            row[x] = shade(target, material, point, n);
        }
    }
}

static void quad(const xd_r3_target *target, const xd_r3_vec p[4], const xd_r3_vec n[4],
                 const xd_r3_material *material) {
    xd_r3_triangle(target, (const xd_r3_vec[]){p[0], p[1], p[2]},
                   (const xd_r3_vec[]){n[0], n[1], n[2]}, material);
    xd_r3_triangle(target, (const xd_r3_vec[]){p[0], p[2], p[3]},
                   (const xd_r3_vec[]){n[0], n[2], n[3]}, material);
}

/* Two unit vectors perpendicular to `axis` (a unit vector) and each other. */
static void basis(xd_r3_vec axis, xd_r3_vec *u, xd_r3_vec *v) {
    xd_r3_vec ref = fabs(axis.x) < 0.9 ? (xd_r3_vec){1, 0, 0} : (xd_r3_vec){0, 1, 0};
    *u = vec_normalize(vec_cross(axis, ref));
    *v = vec_cross(axis, *u);
}

void xd_r3_cylinder(const xd_r3_target *target, xd_r3_vec a, xd_r3_vec b, double radius,
                    int segments, const xd_r3_material *material) {
    xd_r3_vec axis = vec_normalize(vec_sub(b, a));
    xd_r3_vec u, v;
    basis(axis, &u, &v);
    for (int i = 0; i < segments; i++) {
        double t0 = 2 * M_PI * i / segments, t1 = 2 * M_PI * (i + 1) / segments;
        xd_r3_vec n0 = vec_add(vec_scale(u, cos(t0)), vec_scale(v, sin(t0)));
        xd_r3_vec n1 = vec_add(vec_scale(u, cos(t1)), vec_scale(v, sin(t1)));
        xd_r3_vec p[4] = {vec_add(a, vec_scale(n0, radius)), vec_add(a, vec_scale(n1, radius)),
                          vec_add(b, vec_scale(n1, radius)), vec_add(b, vec_scale(n0, radius))};
        quad(target, p, (const xd_r3_vec[]){n0, n1, n1, n0}, material);
    }
}

void xd_r3_elbow(const xd_r3_target *target, xd_r3_vec center, xd_r3_vec start_dir,
                 xd_r3_vec end_dir, double bend_radius, double radius, int segments, int steps,
                 const xd_r3_material *material) {
    xd_r3_vec side = vec_cross(start_dir, end_dir); /* across the bend's plane */
    for (int s = 0; s < steps; s++) {
        double a0 = M_PI / 2 * s / steps, a1 = M_PI / 2 * (s + 1) / steps;
        xd_r3_vec r0 = vec_add(vec_scale(start_dir, cos(a0)), vec_scale(end_dir, sin(a0)));
        xd_r3_vec r1 = vec_add(vec_scale(start_dir, cos(a1)), vec_scale(end_dir, sin(a1)));
        xd_r3_vec c0 = vec_add(center, vec_scale(r0, bend_radius));
        xd_r3_vec c1 = vec_add(center, vec_scale(r1, bend_radius));
        for (int i = 0; i < segments; i++) {
            double t0 = 2 * M_PI * i / segments, t1 = 2 * M_PI * (i + 1) / segments;
            xd_r3_vec n[4] = {vec_add(vec_scale(r0, cos(t0)), vec_scale(side, sin(t0))),
                              vec_add(vec_scale(r0, cos(t1)), vec_scale(side, sin(t1))),
                              vec_add(vec_scale(r1, cos(t1)), vec_scale(side, sin(t1))),
                              vec_add(vec_scale(r1, cos(t0)), vec_scale(side, sin(t0)))};
            xd_r3_vec p[4] = {vec_add(c0, vec_scale(n[0], radius)),
                              vec_add(c0, vec_scale(n[1], radius)),
                              vec_add(c1, vec_scale(n[2], radius)),
                              vec_add(c1, vec_scale(n[3], radius))};
            quad(target, p, n, material);
        }
    }
}

void xd_r3_sphere(const xd_r3_target *target, xd_r3_vec center, double radius, int segments,
                  const xd_r3_material *material) {
    int rings = XD_MAX(segments / 2, 2);
    for (int r = 0; r < rings; r++) {
        double p0 = M_PI * r / rings - M_PI / 2, p1 = M_PI * (r + 1) / rings - M_PI / 2;
        for (int i = 0; i < segments; i++) {
            double t0 = 2 * M_PI * i / segments, t1 = 2 * M_PI * (i + 1) / segments;
            xd_r3_vec n[4] = {{cos(p0) * cos(t0), sin(p0), cos(p0) * sin(t0)},
                              {cos(p0) * cos(t1), sin(p0), cos(p0) * sin(t1)},
                              {cos(p1) * cos(t1), sin(p1), cos(p1) * sin(t1)},
                              {cos(p1) * cos(t0), sin(p1), cos(p1) * sin(t0)}};
            xd_r3_vec p[4];
            for (int k = 0; k < 4; k++)
                p[k] = vec_add(center, vec_scale(n[k], radius));
            quad(target, p, n, material);
        }
    }
}

void xd_r3_bounds(const xd_r3_target *target, xd_r3_vec center, double radius, int *x0, int *y0,
                  int *x1, int *y1) {
    double z = fmax(center.z - radius, NEAR_Z);
    double sx = target->cx + center.x * target->focal / center.z;
    double sy = target->cy + center.y * target->focal / center.z;
    double reach = radius * target->focal / z + 2;
    *x0 = clamp_px(floor(sx - reach), 0, target->width - 1);
    *y0 = clamp_px(floor(sy - reach), 0, target->height - 1);
    *x1 = clamp_px(ceil(sx + reach), -1, target->width - 1);
    *y1 = clamp_px(ceil(sy + reach), -1, target->height - 1);
}
