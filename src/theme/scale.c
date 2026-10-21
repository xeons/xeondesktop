/* SPDX-License-Identifier: MIT
 * Scaling theme images for high DPI: nearest, whole-number blocks,
 * bilinear, and the pixel-art scalers emulators use (EPX/Scale2x, Scale3x,
 * 2xSaI, Super2xSaI, SuperEagle).
 *
 * The pixel-art scalers are written here from their published
 * descriptions (Andrea Mazzoleni's Scale2x/Scale3x rule pages, Derek Liauw
 * Kie Fa's notes on the SaI family), not from any existing source.
 *
 * Everything works on a packed copy of the image (`img`), one uint32_t per
 * pixel in cairo's premultiplied ARGB32, so the filters can compare whole
 * pixels and blend all four channels the same way. Out-of-range neighbours
 * are the nearest edge pixel. Cairo does the smooth (bilinear and area)
 * scaling.
 */
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../core/alloc.h"
#include "scale.h"

/* Cairo's own limit on an image surface's side. */
constexpr int MAX_SIDE = 32767;

static const char *const filter_names[XD_FILTER_COUNT] = {
    [XD_FILTER_NEAREST] = "Nearest",       [XD_FILTER_INTEGER] = "Integer",
    [XD_FILTER_SMOOTH] = "Smooth",         [XD_FILTER_SCALE2X] = "Scale2x",
    [XD_FILTER_SCALE3X] = "Scale3x",       [XD_FILTER_SCALE4X] = "Scale4x",
    [XD_FILTER_2XSAI] = "2xSaI",           [XD_FILTER_SUPER2XSAI] = "Super2xSaI",
    [XD_FILTER_SUPEREAGLE] = "SuperEagle",
};

const char *xd_image_filter_name(xd_image_filter f) {
    if ((unsigned)f >= XD_FILTER_COUNT)
        return nullptr;
    return filter_names[f];
}

bool xd_image_filter_from_name(const char *name, xd_image_filter *out) {
    if (!name)
        return false;
    for (int i = 0; i < XD_FILTER_COUNT; i++) {
        if (g_ascii_strcasecmp(name, filter_names[i]) == 0) {
            if (out)
                *out = (xd_image_filter)i;
            return true;
        }
    }
    return false;
}

/* ---- Packed images ---------------------------------------------------- */

typedef struct img {
    uint32_t *px;
    int w, h;
} img;

static img img_new(int w, int h) {
    return (img){xd_alloc_array((size_t)w * (size_t)h, sizeof(uint32_t)), w, h};
}

static void img_free(img *m) {
    XD_FREE(&m->px);
}

static inline uint32_t px_at(const img *m, int x, int y) {
    x = x < 0 ? 0 : x >= m->w ? m->w - 1 : x;
    y = y < 0 ? 0 : y >= m->h ? m->h - 1 : y;
    return m->px[(size_t)y * (size_t)m->w + (size_t)x];
}

static inline void px_set(img *m, int x, int y, uint32_t v) {
    if (x >= 0 && x < m->w && y >= 0 && y < m->h)
        m->px[(size_t)y * (size_t)m->w + (size_t)x] = v;
}

/* A packed copy of the surface's pixels, or {nullptr} if it isn't a usable
 * image surface. Anything other than ARGB32 is painted into ARGB32 first. */
static img img_from_surface(cairo_surface_t *s) {
    if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE)
        return (img){};
    int w = cairo_image_surface_get_width(s);
    int h = cairo_image_surface_get_height(s);
    if (w <= 0 || h <= 0)
        return (img){};
    cairo_surface_t *argb = s;
    if (cairo_image_surface_get_format(s) != CAIRO_FORMAT_ARGB32) {
        argb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(argb);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cr);
        cairo_destroy(cr);
    }
    cairo_surface_flush(argb);
    const unsigned char *data = cairo_image_surface_get_data(argb);
    int stride = cairo_image_surface_get_stride(argb);
    img m = {};
    if (data) {
        m = img_new(w, h);
        for (int y = 0; y < h; y++)
            memcpy(m.px + (size_t)y * (size_t)w, data + (size_t)y * (size_t)stride,
                   (size_t)w * sizeof(uint32_t));
    }
    if (argb != s)
        cairo_surface_destroy(argb);
    return m;
}

static cairo_surface_t *img_to_surface(const img *m) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m->w, m->h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    cairo_surface_flush(s);
    unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < m->h; y++)
        memcpy(data + (size_t)y * (size_t)stride, m->px + (size_t)y * (size_t)m->w,
               (size_t)m->w * sizeof(uint32_t));
    cairo_surface_mark_dirty(s);
    return s;
}

/* ---- Blending --------------------------------------------------------- */

/* Weighted mean of four premultiplied pixels, per channel (alpha
 * included), the weights in eighths (they sum to 8), rounded half up.
 * Being a weighted mean, no channel can exceed the largest input's, and
 * colour can't exceed alpha, so translucent edges stay valid. */
static inline uint32_t mix8(uint32_t p, unsigned wp, uint32_t q, unsigned wq, uint32_t r,
                            unsigned wr, uint32_t s, unsigned ws) {
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        unsigned v = ((p >> sh) & 0xff) * wp + ((q >> sh) & 0xff) * wq +
                     ((r >> sh) & 0xff) * wr + ((s >> sh) & 0xff) * ws + 4;
        out |= (uint32_t)(v / 8) << sh;
    }
    return out;
}

/* 1/2 each. */
static inline uint32_t half(uint32_t p, uint32_t q) {
    return mix8(p, 4, q, 4, 0, 0, 0, 0);
}

/* 3/4 p, 1/4 q. */
static inline uint32_t three_quarters(uint32_t p, uint32_t q) {
    return mix8(p, 6, q, 2, 0, 0, 0, 0);
}

/* 1/4 each. */
static inline uint32_t quarter4(uint32_t p, uint32_t q, uint32_t r, uint32_t s) {
    return mix8(p, 2, q, 2, r, 2, s, 2);
}

/* ---- Nearest and whole-number blocks ---------------------------------- */

/* Each output pixel takes the source pixel under its centre. */
static void scale_nearest(const img *s, img *d) {
    for (int y = 0; y < d->h; y++) {
        int sy = (int)(((int64_t)y * 2 + 1) * s->h / ((int64_t)d->h * 2));
        for (int x = 0; x < d->w; x++) {
            int sx = (int)(((int64_t)x * 2 + 1) * s->w / ((int64_t)d->w * 2));
            d->px[(size_t)y * (size_t)d->w + (size_t)x] = px_at(s, sx, sy);
        }
    }
}

/* d is s->w * n by s->h * n: every pixel an exact n x n block. */
static void scale_blocks(const img *s, img *d, int n) {
    for (int y = 0; y < d->h; y++)
        for (int x = 0; x < d->w; x++)
            d->px[(size_t)y * (size_t)d->w + (size_t)x] = px_at(s, x / n, y / n);
}

/* ---- Smooth ----------------------------------------------------------- */

/* One pass of an area average along one axis: `n` lines of `slen` source
 * samples become `dlen` samples each, every one the mean of the stretch of
 * source it covers (partly covered samples by the part covered). Samples
 * are 4 channels; `sstep`/`dstep` are the distances between samples along
 * the line and `sline`/`dline` between lines, in channels. */
static void area_pass(const double *src, double *dst, int n, int slen, int dlen, size_t sstep,
                      size_t sline, size_t dstep, size_t dline) {
    double f = (double)slen / dlen;
    for (int line = 0; line < n; line++) {
        const double *sl = src + (size_t)line * sline;
        double *dl = dst + (size_t)line * dline;
        for (int i = 0; i < dlen; i++) {
            double x0 = i * f, x1 = (i + 1) * f;
            double acc[4] = {};
            for (int k = (int)x0; k < slen && k < x1; k++) {
                double w = fmin(k + 1, x1) - fmax(k, x0);
                for (int c = 0; c < 4; c++)
                    acc[c] += w * sl[(size_t)k * sstep + (size_t)c];
            }
            for (int c = 0; c < 4; c++)
                dl[(size_t)i * dstep + (size_t)c] = acc[c] / f;
        }
    }
}

/* s shrunk to d's size (no larger either way) by averaging the area each
 * output pixel covers, in premultiplied ARGB: the right way to bring an
 * enlarged pixel-art image down to a size in between, every source pixel
 * weighing what it covers (cairo's GOOD filter samples bilinearly at mild
 * reductions, which drops and doubles rows). */
static void scale_area(const img *s, img *d) {
    double *a = xd_alloc_array((size_t)s->w * (size_t)s->h * 4, sizeof(double));
    double *b = xd_alloc_array((size_t)d->w * (size_t)s->h * 4, sizeof(double));
    double *c = xd_alloc_array((size_t)d->w * (size_t)d->h * 4, sizeof(double));
    size_t np = (size_t)s->w * (size_t)s->h;
    for (size_t p = 0; p < np; p++)
        for (int ch = 0; ch < 4; ch++)
            a[p * 4 + (size_t)ch] = (double)((s->px[p] >> (8 * ch)) & 0xff);
    /* Rows first (s->h lines of s->w), then columns (d->w lines of s->h). */
    area_pass(a, b, s->h, s->w, d->w, 4, (size_t)s->w * 4, 4, (size_t)d->w * 4);
    area_pass(b, c, d->w, s->h, d->h, (size_t)d->w * 4, 4, (size_t)d->w * 4, 4);
    size_t nd = (size_t)d->w * (size_t)d->h;
    for (size_t p = 0; p < nd; p++) {
        unsigned v[4];
        for (int ch = 0; ch < 4; ch++) {
            double x = floor(c[p * 4 + (size_t)ch] + 0.5);
            v[ch] = x < 0 ? 0 : x > 255 ? 255 : (unsigned)x;
        }
        /* Keep it valid premultiplied colour despite rounding. */
        for (int ch = 0; ch < 3; ch++)
            if (v[ch] > v[3])
                v[ch] = v[3];
        d->px[p] = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
    }
    xd_free(c);
    xd_free(b);
    xd_free(a);
}

/* s scaled to d's size: an area average going down, otherwise cairo's
 * bilinear filter. EXTEND_PAD repeats the edge pixels outwards, so the
 * edges don't fade into transparency. */
static void scale_smooth(const img *s, img *d) {
    if (d->w <= s->w && d->h <= s->h) {
        scale_area(s, d);
        return;
    }
    cairo_surface_t *ss = cairo_image_surface_create_for_data(
        (unsigned char *)s->px, CAIRO_FORMAT_ARGB32, s->w, s->h, s->w * 4);
    cairo_surface_t *ds = cairo_image_surface_create_for_data(
        (unsigned char *)d->px, CAIRO_FORMAT_ARGB32, d->w, d->h, d->w * 4);
    cairo_t *cr = cairo_create(ds);
    cairo_scale(cr, (double)d->w / s->w, (double)d->h / s->h);
    cairo_set_source_surface(cr, ss, 0, 0);
    cairo_pattern_t *pat = cairo_get_source(cr);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(ds);
    cairo_surface_finish(ds);
    cairo_surface_destroy(ds);
    cairo_surface_destroy(ss);
}

/* ---- Scale2x (EPX) and Scale3x ---------------------------------------- */

/* Scale2x, as Mazzoleni describes it: with
 *       B
 *     D E F
 *       H
 * each quarter of E takes the colour of its two outer neighbours when they
 * agree with each other and the other two neighbours don't continue the
 * edge (B != H and D != F), so a staircase becomes a diagonal. */
static void scale2x(const img *s, img *d) {
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            uint32_t b = px_at(s, x, y - 1), dd = px_at(s, x - 1, y);
            uint32_t e = px_at(s, x, y), f = px_at(s, x + 1, y);
            uint32_t h = px_at(s, x, y + 1);
            uint32_t e0 = e, e1 = e, e2 = e, e3 = e;
            if (b != h && dd != f) {
                if (dd == b)
                    e0 = dd;
                if (b == f)
                    e1 = f;
                if (dd == h)
                    e2 = dd;
                if (h == f)
                    e3 = f;
            }
            px_set(d, 2 * x, 2 * y, e0);
            px_set(d, 2 * x + 1, 2 * y, e1);
            px_set(d, 2 * x, 2 * y + 1, e2);
            px_set(d, 2 * x + 1, 2 * y + 1, e3);
        }
    }
}

/* Scale3x, as Mazzoleni describes it: with
 *     A B C
 *     D E F
 *     G H I
 * and the same B != H, D != F guard, the corners of the 3x3 block take the
 * agreeing outer neighbours as in Scale2x, and each edge middle takes its
 * neighbour when an adjacent corner agrees and E differs from the far
 * corner (which would otherwise make a lump). The centre is always E. */
static void scale3x(const img *s, img *d) {
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            uint32_t a = px_at(s, x - 1, y - 1), b = px_at(s, x, y - 1);
            uint32_t c = px_at(s, x + 1, y - 1), dd = px_at(s, x - 1, y);
            uint32_t e = px_at(s, x, y), f = px_at(s, x + 1, y);
            uint32_t g = px_at(s, x - 1, y + 1), h = px_at(s, x, y + 1);
            uint32_t i = px_at(s, x + 1, y + 1);
            uint32_t o[9] = {e, e, e, e, e, e, e, e, e};
            if (b != h && dd != f) {
                if (dd == b)
                    o[0] = dd;
                if ((dd == b && e != c) || (b == f && e != a))
                    o[1] = b;
                if (b == f)
                    o[2] = f;
                if ((dd == b && e != g) || (dd == h && e != a))
                    o[3] = dd;
                if ((b == f && e != i) || (h == f && e != c))
                    o[5] = f;
                if (dd == h)
                    o[6] = dd;
                if ((dd == h && e != i) || (h == f && e != g))
                    o[7] = h;
                if (h == f)
                    o[8] = f;
            }
            for (int k = 0; k < 9; k++)
                px_set(d, 3 * x + k % 3, 3 * y + k / 3, o[k]);
        }
    }
}

/* ---- The SaI family --------------------------------------------------- */

/* The 4x4 neighbourhood the SaI scalers read, around the 2x2 square whose
 * top-left pixel is (x, y):
 *
 *     i  e  f  j        row y-1
 *     g [a  b] k        row y
 *     h [c  d] l        row y+1
 *     m  n  o  p        row y+2
 */
typedef struct sai_nb {
    uint32_t i, e, f, j, g, a, b, k, h, c, d, l, m, n, o, p;
} sai_nb;

static inline sai_nb sai_read(const img *s, int x, int y) {
    return (sai_nb){
        px_at(s, x - 1, y - 1), px_at(s, x, y - 1), px_at(s, x + 1, y - 1), px_at(s, x + 2, y - 1),
        px_at(s, x - 1, y),     px_at(s, x, y),     px_at(s, x + 1, y),     px_at(s, x + 2, y),
        px_at(s, x - 1, y + 1), px_at(s, x, y + 1), px_at(s, x + 1, y + 1), px_at(s, x + 2, y + 1),
        px_at(s, x - 1, y + 2), px_at(s, x, y + 2), px_at(s, x + 1, y + 2), px_at(s, x + 2, y + 2),
    };
}

/* When both diagonals of the square are single colours (a == d, b == c),
 * they cross, and one must be drawn over the other. The two pixels outside
 * each corner of the square (g,e at a; f,k at b; h,n at c; l,o at d) vote:
 * a pair that is all of one diagonal's colour is background, which says
 * the other diagonal is the thin line to keep joined up.
 * > 0: keep a's diagonal; < 0: keep b's; 0: undecided. */
static int sai_vote(const sai_nb *n) {
    const uint32_t pairs[4][2] = {{n->g, n->e}, {n->f, n->k}, {n->h, n->n}, {n->l, n->o}};
    int r = 0;
    for (int k = 0; k < 4; k++) {
        if (pairs[k][0] == n->b && pairs[k][1] == n->b)
            r++;
        else if (pairs[k][0] == n->a && pairs[k][1] == n->a)
            r--;
    }
    return r;
}

/* 2xSaI. Each source pixel a becomes a 2x2 block: a itself, then the
 * points halfway to b (right), to c (below), and the square's centre.
 * Those in-between points are normally the halfway blends (and the centre
 * the four-way blend), but:
 * - When one diagonal of the square is a single colour and the other
 *   isn't, the centre takes the diagonal's colour, and each halfway point
 *   takes it too where the surrounding pixels show the diagonal's edge
 *   running on through that point.
 * - When both diagonals are, sai_vote() picks the one to keep at the
 *   centre (the four-way blend if undecided).
 * - With no diagonal, a halfway point snaps to one side where that side
 *   forms an L with its neighbours that the other side doesn't break.
 *
 * Sampling at a and halfway on puts the picture a quarter of a source
 * pixel up and left of centre, which no whole-pixel shift can undo: a
 * 1-pixel border comes out as one pure and one blended pixel on the left
 * and top, two pure ones on the right and bottom. (Super2xSaI shares this
 * horizontally.) */
static void sai2x(const img *s, img *dst) {
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            sai_nb n = sai_read(s, x, y);
            uint32_t right, below, centre;
            if (n.a == n.d && n.b != n.c) {
                bool r = (n.a == n.e && n.b == n.l) ||
                         (n.a == n.c && n.a == n.f && n.b != n.e && n.b == n.j);
                bool b = (n.a == n.g && n.c == n.o) ||
                         (n.a == n.b && n.a == n.h && n.g != n.c && n.c == n.m);
                right = r ? n.a : half(n.a, n.b);
                below = b ? n.a : half(n.a, n.c);
                centre = n.a;
            } else if (n.b == n.c && n.a != n.d) {
                bool r = (n.b == n.f && n.a == n.h) ||
                         (n.b == n.e && n.b == n.d && n.a != n.f && n.a == n.i);
                bool b = (n.c == n.h && n.a == n.f) ||
                         (n.c == n.g && n.c == n.d && n.a != n.h && n.a == n.i);
                right = r ? n.b : half(n.a, n.b);
                below = b ? n.c : half(n.a, n.c);
                centre = n.b;
            } else if (n.a == n.d && n.b == n.c) {
                if (n.a == n.b) {
                    right = below = centre = n.a;
                } else {
                    right = half(n.a, n.b);
                    below = half(n.a, n.c);
                    int v = sai_vote(&n);
                    centre = v > 0 ? n.a : v < 0 ? n.b : quarter4(n.a, n.b, n.c, n.d);
                }
            } else {
                centre = quarter4(n.a, n.b, n.c, n.d);
                if (n.a == n.c && n.a == n.f && n.b != n.e && n.b == n.j)
                    right = n.a;
                else if (n.b == n.e && n.b == n.d && n.a != n.f && n.a == n.i)
                    right = n.b;
                else
                    right = half(n.a, n.b);
                if (n.a == n.b && n.a == n.h && n.g != n.c && n.c == n.m)
                    below = n.a;
                else if (n.c == n.g && n.c == n.d && n.a != n.h && n.a == n.i)
                    below = n.c;
                else
                    below = half(n.a, n.c);
            }
            px_set(dst, 2 * x, 2 * y, n.a);
            px_set(dst, 2 * x + 1, 2 * y, right);
            px_set(dst, 2 * x, 2 * y + 1, below);
            px_set(dst, 2 * x + 1, 2 * y + 1, centre);
        }
    }
}

/* Super2xSaI. Horizontally it samples as 2xSaI does (a, then halfway to
 * b); vertically it splits the square between rows y and y+1 into a top
 * half, nearer a and b, and a bottom half, nearer c and d. The right
 * column follows the square's diagonals (sai_vote() when they cross),
 * otherwise snapping to 3/4 of one side where it forms an edge running
 * downwards or upwards; the left column, normally a and c themselves,
 * softens to a half blend at the ends of a diagonal.
 *
 * The square for rows y, y+1 fills output rows 2y+1 and 2y+2, so the
 * picture stays centred (output row 0 comes from the clamped square above
 * the image). */
static void super2xsai(const img *s, img *dst) {
    for (int y = -1; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            sai_nb n = sai_read(s, x, y);
            uint32_t tl = n.a, tr, bl = n.c, br;
            if (n.b == n.c && n.a != n.d) {
                tr = br = n.b;
            } else if (n.a == n.d && n.b != n.c) {
                tr = br = n.a;
            } else if (n.a == n.d && n.b == n.c) {
                int v = sai_vote(&n);
                tr = br = v > 0 ? n.a : v < 0 ? n.b : half(n.a, n.b);
            } else {
                if (n.d == n.b && n.d == n.n && n.c != n.o && n.d != n.m)
                    br = three_quarters(n.d, n.c);
                else if (n.a == n.c && n.c == n.o && n.n != n.d && n.c != n.p)
                    br = three_quarters(n.c, n.d);
                else
                    br = half(n.c, n.d);
                if (n.b == n.d && n.b == n.e && n.a != n.f && n.b != n.i)
                    tr = three_quarters(n.b, n.a);
                else if (n.a == n.c && n.a == n.f && n.e != n.b && n.a != n.j)
                    tr = three_quarters(n.a, n.b);
                else
                    tr = half(n.a, n.b);
            }
            if ((n.a == n.d && n.b != n.c && n.g == n.a && n.a != n.o) ||
                (n.a == n.h && n.b == n.a && n.g != n.c && n.a != n.m))
                bl = half(n.c, n.a);
            if ((n.c == n.b && n.a != n.d && n.h == n.c && n.c != n.f) ||
                (n.g == n.c && n.d == n.c && n.h != n.a && n.c != n.i))
                tl = half(n.c, n.a);
            px_set(dst, 2 * x, 2 * y + 1, tl);
            px_set(dst, 2 * x + 1, 2 * y + 1, tr);
            px_set(dst, 2 * x, 2 * y + 2, bl);
            px_set(dst, 2 * x + 1, 2 * y + 2, br);
        }
    }
}

/* SuperEagle. Each 2x2 square of source pixels a b / c d becomes the four
 * output pixels around its centre, each nearest one corner:
 * - A diagonal of one colour (the other not) claims its two corners, and
 *   the other two corners become half their own colour, half the
 *   diagonal's, or 1/4 their own where the pixels beyond show the diagonal
 *   running on.
 * - Crossing diagonals: sai_vote() picks the one kept whole, and the
 *   other's corners blend half and half; undecided keeps all four pixels.
 * - Otherwise each corner keeps 3/4 of its colour and takes 1/8 from each
 *   of its two neighbouring corners, which softens straight edges.
 *
 * The square at (x, y) fills output (2x+1..2x+2, 2y+1..2y+2), so every
 * source pixel's block is made of the four squares around it and the
 * picture stays centred. */
static void supereagle(const img *s, img *dst) {
    for (int y = -1; y < s->h; y++) {
        for (int x = -1; x < s->w; x++) {
            sai_nb n = sai_read(s, x, y);
            uint32_t qa, qb, qc, qd;
            if (n.b == n.c && n.a != n.d) {
                qb = qc = n.b;
                qa = (n.h == n.c || n.f == n.b) ? three_quarters(n.b, n.a) : half(n.a, n.b);
                qd = (n.k == n.b || n.n == n.c) ? three_quarters(n.c, n.d) : half(n.c, n.d);
            } else if (n.a == n.d && n.b != n.c) {
                qa = qd = n.a;
                qb = (n.l == n.d || n.e == n.a) ? three_quarters(n.a, n.b) : half(n.b, n.a);
                qc = (n.g == n.a || n.o == n.d) ? three_quarters(n.d, n.c) : half(n.d, n.c);
            } else if (n.a == n.d && n.b == n.c) {
                int v = n.a == n.b ? 0 : sai_vote(&n);
                if (v < 0) {
                    qb = qc = n.b;
                    qa = qd = half(n.a, n.b);
                } else if (v > 0) {
                    qa = qd = n.a;
                    qb = qc = half(n.a, n.b);
                } else {
                    qa = qd = n.a;
                    qb = qc = n.b;
                }
            } else {
                qa = mix8(n.a, 6, n.b, 1, n.c, 1, 0, 0);
                qb = mix8(n.b, 6, n.a, 1, n.d, 1, 0, 0);
                qc = mix8(n.c, 6, n.a, 1, n.d, 1, 0, 0);
                qd = mix8(n.d, 6, n.b, 1, n.c, 1, 0, 0);
            }
            px_set(dst, 2 * x + 1, 2 * y + 1, qa);
            px_set(dst, 2 * x + 2, 2 * y + 1, qb);
            px_set(dst, 2 * x + 1, 2 * y + 2, qc);
            px_set(dst, 2 * x + 2, 2 * y + 2, qd);
        }
    }
}

/* ---- Putting it together ---------------------------------------------- */

/* A pixel-art filter's own factor: one pass enlarges by this much. */
static int filter_step(xd_image_filter filter) {
    return filter == XD_FILTER_SCALE3X ? 3 : filter == XD_FILTER_SCALE4X ? 4 : 2;
}

/* One pass of a pixel-art filter, replacing *m. */
static void pixel_art_step(img *m, xd_image_filter filter) {
    int f = filter_step(filter);
    img d = img_new(m->w * f, m->h * f);
    switch (filter) {
    case XD_FILTER_SCALE2X:
        scale2x(m, &d);
        break;
    case XD_FILTER_SCALE3X:
        scale3x(m, &d);
        break;
    case XD_FILTER_SCALE4X: {
        img mid = img_new(m->w * 2, m->h * 2);
        scale2x(m, &mid);
        scale2x(&mid, &d);
        img_free(&mid);
        break;
    }
    case XD_FILTER_2XSAI:
        sai2x(m, &d);
        break;
    case XD_FILTER_SUPER2XSAI:
        super2xsai(m, &d);
        break;
    case XD_FILTER_SUPEREAGLE:
        supereagle(m, &d);
        break;
    case XD_FILTER_NEAREST:
    case XD_FILTER_INTEGER:
    case XD_FILTER_SMOOTH:
    case XD_FILTER_COUNT:
        XD_ASSERT(false); /* not pixel-art filters */
        break;
    }
    img_free(m);
    *m = d;
}

/* s scaled to dw x dh, which is about s's size times `scale`. Returns
 * {nullptr} if a size would be beyond cairo's limits. */
static img scale_img(const img *s, int dw, int dh, double scale, xd_image_filter filter) {
    if (dw > MAX_SIDE || dh > MAX_SIDE)
        return (img){};
    img d = img_new(dw, dh);
    if (dw == s->w && dh == s->h) {
        memcpy(d.px, s->px, (size_t)dw * (size_t)dh * sizeof(uint32_t));
        return d;
    }
    if (filter == XD_FILTER_NEAREST) {
        scale_nearest(s, &d);
        return d;
    }
    if (filter == XD_FILTER_SMOOTH || scale < 1) {
        scale_smooth(s, &d);
        return d;
    }

    /* Enlarge exactly to at least the target size, then smooth down. */
    img big;
    if (filter == XD_FILTER_INTEGER) {
        int k = 1;
        while (s->w * k < dw || s->h * k < dh)
            k++;
        if ((int64_t)s->w * k > MAX_SIDE || (int64_t)s->h * k > MAX_SIDE) {
            img_free(&d);
            return (img){};
        }
        big = img_new(s->w * k, s->h * k);
        scale_blocks(s, &big, k);
    } else {
        int f = filter_step(filter);
        big = img_new(s->w, s->h);
        memcpy(big.px, s->px, (size_t)s->w * (size_t)s->h * sizeof(uint32_t));
        while (big.w < dw || big.h < dh) {
            if ((int64_t)big.w * f > MAX_SIDE || (int64_t)big.h * f > MAX_SIDE) {
                img_free(&big);
                img_free(&d);
                return (img){};
            }
            pixel_art_step(&big, filter);
        }
    }
    if (big.w == dw && big.h == dh) {
        img_free(&d);
        return big;
    }
    scale_smooth(&big, &d);
    img_free(&big);
    return d;
}

/* A length times the scale, rounded, at least 1; -1 if out of range. */
static int scaled_len(int len, double scale) {
    double v = round(len * scale);
    if (v > MAX_SIDE)
        return -1;
    return v < 1 ? 1 : (int)v;
}

static bool valid(double scale, xd_image_filter filter) {
    return isfinite(scale) && scale > 0 && (unsigned)filter < XD_FILTER_COUNT;
}

cairo_surface_t *xd_image_scale(cairo_surface_t *src, double scale, xd_image_filter filter) {
    if (!valid(scale, filter))
        return nullptr;
    img s = img_from_surface(src);
    if (!s.px)
        return nullptr;
    int dw = scaled_len(s.w, scale), dh = scaled_len(s.h, scale);
    cairo_surface_t *out = nullptr;
    if (dw > 0 && dh > 0) {
        img d = scale_img(&s, dw, dh, scale, filter);
        if (d.px)
            out = img_to_surface(&d);
        img_free(&d);
    }
    img_free(&s);
    return out;
}

cairo_surface_t *xd_image_scale_frames(cairo_surface_t *src, int count, bool vertical,
                                       double scale, xd_image_filter filter) {
    if (!valid(scale, filter) || count < 1)
        return nullptr;
    img s = img_from_surface(src);
    if (!s.px)
        return nullptr;
    int along = vertical ? s.h : s.w;
    if (along % count != 0) {
        img_free(&s);
        return nullptr;
    }
    int fw = vertical ? s.w : s.w / count;
    int fh = vertical ? s.h / count : s.h;
    int dw = scaled_len(fw, scale), dh = scaled_len(fh, scale);
    int ow = vertical ? dw : dw * count, oh = vertical ? dh * count : dh;
    if (dw <= 0 || dh <= 0 || (int64_t)(vertical ? dh : dw) * count > MAX_SIDE) {
        img_free(&s);
        return nullptr;
    }

    img out = img_new(ow, oh);
    img frame = img_new(fw, fh);
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        int fx = vertical ? 0 : i * fw, fy = vertical ? i * fh : 0;
        for (int y = 0; y < fh; y++)
            memcpy(frame.px + (size_t)y * (size_t)fw,
                   s.px + (size_t)(fy + y) * (size_t)s.w + (size_t)fx,
                   (size_t)fw * sizeof(uint32_t));
        img d = scale_img(&frame, dw, dh, scale, filter);
        if (!d.px) {
            ok = false;
            break;
        }
        int ox = vertical ? 0 : i * dw, oy = vertical ? i * dh : 0;
        for (int y = 0; y < dh; y++)
            memcpy(out.px + (size_t)(oy + y) * (size_t)ow + (size_t)ox,
                   d.px + (size_t)y * (size_t)dw, (size_t)dw * sizeof(uint32_t));
        img_free(&d);
    }
    cairo_surface_t *res = ok ? img_to_surface(&out) : nullptr;
    img_free(&frame);
    img_free(&out);
    img_free(&s);
    return res;
}
