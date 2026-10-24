/* SPDX-License-Identifier: MIT
 * A small software 3D renderer for the screen savers (3D Pipes, 3D Maze),
 * which have no GPU to draw with: triangles in camera space (x right, y
 * down, z away from the eye), projected in perspective, depth-tested per
 * pixel and lit per pixel as the OpenGL savers were: ambient light, one
 * directional light and a white specular highlight.
 */
#ifndef XD_SAVER_RASTER3D_H
#define XD_SAVER_RASTER3D_H

#include <stdint.h>

typedef struct xd_r3_vec {
    double x, y, z;
} xd_r3_vec;

/* A surface's colour (0..1) and how it shines. */
typedef struct xd_r3_material {
    double r, g, b;
    double specular;  /* the highlight's strength, 0..1 */
    double shininess; /* its tightness: OpenGL's GL_SHININESS */
    bool flat;        /* one normal per triangle: faceted, as a polyhedron */
} xd_r3_material;

/* Where triangles go: pixels (0x00RRGGBB, `stride` pixels a row) and a
 * depth per pixel (camera-space z; larger is farther), both the caller's.
 * The projection: focal length `focal` (px) and the centre (cx, cy). */
typedef struct xd_r3_target {
    uint32_t *pixels;
    float *depth;
    int width, height, stride;
    double focal, cx, cy;
    xd_r3_vec light; /* towards the light, normalised */
    double ambient;
} xd_r3_target;

/* Clears the pixels to `rgb` and the depth to infinity. */
void xd_r3_clear(const xd_r3_target *target, uint32_t rgb);

/* One triangle, its corners' positions and normals in camera space (the
 * normals need not be normalised). Corners nearer than z = 0.05 drop it. */
void xd_r3_triangle(const xd_r3_target *target, const xd_r3_vec position[3],
                    const xd_r3_vec normal[3], const xd_r3_material *material);

/* Meshes, as triangles. A cylinder from `a` to `b`; a torus segment, the
 * pipe's centre line running round `center` from `start_dir` towards
 * `end_dir` (perpendicular unit vectors) at `bend_radius`, a quarter turn;
 * a sphere. `segments` round the circumference. */
void xd_r3_cylinder(const xd_r3_target *target, xd_r3_vec a, xd_r3_vec b, double radius,
                    int segments, const xd_r3_material *material);
void xd_r3_elbow(const xd_r3_target *target, xd_r3_vec center, xd_r3_vec start_dir,
                 xd_r3_vec end_dir, double bend_radius, double radius, int segments, int steps,
                 const xd_r3_material *material);
void xd_r3_sphere(const xd_r3_target *target, xd_r3_vec center, double radius, int segments,
                  const xd_r3_material *material);

/* The screen rectangle a sphere round `center` covers (for repainting). */
void xd_r3_bounds(const xd_r3_target *target, xd_r3_vec center, double radius, int *x0, int *y0,
                  int *x1, int *y1);

#endif
