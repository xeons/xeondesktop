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

/* Cairo's own limit on an image surface's side; and ours on an image's
 * area (16 megapixels, 64 MB), so a theme package's image scaled up many
 * times can't take all the memory there is (and the compositor with it). */
constexpr int MAX_SIDE = 32767;
constexpr int64_t MAX_PIXELS = 16 * 1024 * 1024;

static bool too_big(int64_t width, int64_t height) {
    return width > MAX_SIDE || height > MAX_SIDE || width * height > MAX_PIXELS;
}

static const char *const filter_names[XD_FILTER_COUNT] = {
    [XD_FILTER_NEAREST] = "Nearest",       [XD_FILTER_INTEGER] = "Integer",
    [XD_FILTER_SMOOTH] = "Smooth",         [XD_FILTER_SCALE2X] = "Scale2x",
    [XD_FILTER_SCALE3X] = "Scale3x",       [XD_FILTER_SCALE4X] = "Scale4x",
    [XD_FILTER_2XSAI] = "2xSaI",           [XD_FILTER_SUPER2XSAI] = "Super2xSaI",
    [XD_FILTER_SUPEREAGLE] = "SuperEagle",
};

const char *xd_image_filter_name(xd_image_filter image_filter) {
    if ((unsigned)image_filter >= XD_FILTER_COUNT)
        return nullptr;
    return filter_names[image_filter];
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

static img img_new(int width, int height) {
    return (img){xd_alloc_array((size_t)width * (size_t)height, sizeof(uint32_t)), width, height};
}

static void img_free(img *bitmap) { XD_FREE(&bitmap->px); }

static inline uint32_t px_at(const img *bitmap, int x, int y) {
    x = x < 0 ? 0 : x >= bitmap->w ? bitmap->w - 1 : x;
    y = y < 0 ? 0 : y >= bitmap->h ? bitmap->h - 1 : y;
    return bitmap->px[(size_t)y * (size_t)bitmap->w + (size_t)x];
}

static inline void px_set(img *bitmap, int x, int y, uint32_t argb) {
    if (x >= 0 && x < bitmap->w && y >= 0 && y < bitmap->h)
        bitmap->px[(size_t)y * (size_t)bitmap->w + (size_t)x] = argb;
}

/* A packed copy of the surface's pixels, or {nullptr} if it isn't a usable
 * image surface. Anything other than ARGB32 is painted into ARGB32 first. */
static img img_from_surface(cairo_surface_t *surface) {
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
        return (img){};
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    if (width <= 0 || height <= 0)
        return (img){};
    cairo_surface_t *argb = surface;
    if (cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32) {
        argb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        cairo_t *cairo = cairo_create(argb);
        cairo_set_source_surface(cairo, surface, 0, 0);
        cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cairo);
        cairo_destroy(cairo);
    }
    cairo_surface_flush(argb);
    const unsigned char *data = cairo_image_surface_get_data(argb);
    int stride = cairo_image_surface_get_stride(argb);
    img bitmap = {};
    if (data) {
        bitmap = img_new(width, height);
        for (int y = 0; y < height; y++)
            memcpy(bitmap.px + (size_t)y * (size_t)width, data + (size_t)y * (size_t)stride,
                   (size_t)width * sizeof(uint32_t));
    }
    if (argb != surface)
        cairo_surface_destroy(argb);
    return bitmap;
}

static cairo_surface_t *img_to_surface(const img *bitmap) {
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bitmap->w, bitmap->h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < bitmap->h; y++)
        memcpy(data + (size_t)y * (size_t)stride, bitmap->px + (size_t)y * (size_t)bitmap->w,
               (size_t)bitmap->w * sizeof(uint32_t));
    cairo_surface_mark_dirty(surface);
    return surface;
}

/* ---- Blending --------------------------------------------------------- */

/* Weighted mean of four premultiplied pixels, per channel (alpha
 * included), the weights in eighths (they sum to 8), rounded half up.
 * Being a weighted mean, no channel can exceed the largest input's, and
 * colour can't exceed alpha, so translucent edges stay valid. */
static inline uint32_t mix8(uint32_t first, unsigned wp, uint32_t second, unsigned wq,
                            uint32_t third, unsigned wr, uint32_t fourth, unsigned ws) {
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        unsigned sum = ((first >> sh) & 0xff) * wp + ((second >> sh) & 0xff) * wq +
                       ((third >> sh) & 0xff) * wr + ((fourth >> sh) & 0xff) * ws + 4;
        out |= (uint32_t)(sum / 8) << sh;
    }
    return out;
}

/* 1/2 each. */
static inline uint32_t half(uint32_t first, uint32_t second) {
    return mix8(first, 4, second, 4, 0, 0, 0, 0);
}

/* 3/4 p, 1/4 q. */
static inline uint32_t three_quarters(uint32_t first, uint32_t second) {
    return mix8(first, 6, second, 2, 0, 0, 0, 0);
}

/* 1/4 each. */
static inline uint32_t quarter4(uint32_t first, uint32_t second, uint32_t third, uint32_t fourth) {
    return mix8(first, 2, second, 2, third, 2, fourth, 2);
}

/* ---- Nearest and whole-number blocks ---------------------------------- */

/* Each output pixel takes the source pixel under its centre. */
static void scale_nearest(const img *input, img *dest) {
    for (int y = 0; y < dest->h; y++) {
        int sy = (int)(((int64_t)y * 2 + 1) * input->h / ((int64_t)dest->h * 2));
        for (int x = 0; x < dest->w; x++) {
            int sx = (int)(((int64_t)x * 2 + 1) * input->w / ((int64_t)dest->w * 2));
            dest->px[(size_t)y * (size_t)dest->w + (size_t)x] = px_at(input, sx, sy);
        }
    }
}

/* d is s->w * n by s->h * n: every pixel an exact n x n block. */
static void scale_blocks(const img *input, img *dest, int factor) {
    for (int y = 0; y < dest->h; y++)
        for (int x = 0; x < dest->w; x++)
            dest->px[(size_t)y * (size_t)dest->w + (size_t)x] =
                px_at(input, x / factor, y / factor);
}

/* ---- Smooth ----------------------------------------------------------- */

/* One pass of an area average along one axis: `line_count` lines of `slen` source
 * samples become `dlen` samples each, every one the mean of the stretch of
 * source it covers (partly covered samples by the part covered). Samples
 * are 4 channels; `sstep`/`dstep` are the distances between samples along
 * the line and `sline`/`dline` between lines, in channels. */
static void area_pass(const double *src, double *dst, int line_count, int slen, int dlen,
                      size_t sstep, size_t sline, size_t dstep, size_t dline) {
    double ratio = (double)slen / dlen;
    for (int line = 0; line < line_count; line++) {
        const double *sl = src + (size_t)line * sline;
        double *dl = dst + (size_t)line * dline;
        for (int i = 0; i < dlen; i++) {
            double x0 = i * ratio, x1 = (i + 1) * ratio;
            double acc[4] = {};
            for (int k = (int)x0; k < slen && k < x1; k++) {
                double weight = fmin(k + 1, x1) - fmax(k, x0);
                for (int channel = 0; channel < 4; channel++)
                    acc[channel] += weight * sl[(size_t)k * sstep + (size_t)channel];
            }
            for (int channel = 0; channel < 4; channel++)
                dl[(size_t)i * dstep + (size_t)channel] = acc[channel] / ratio;
        }
    }
}

/* s shrunk to d's size (no larger either way) by averaging the area each
 * output pixel covers, in premultiplied ARGB: the right way to bring an
 * enlarged pixel-art image down to a size in between, every source pixel
 * weighing what it covers (cairo's GOOD filter samples bilinearly at mild
 * reductions, which drops and doubles rows). */
static void scale_area(const img *input, img *dest) {
    double *source_samples =
        xd_alloc_array((size_t)input->w * (size_t)input->h * 4, sizeof(double));
    double *row_samples = xd_alloc_array((size_t)dest->w * (size_t)input->h * 4, sizeof(double));
    double *dest_samples = xd_alloc_array((size_t)dest->w * (size_t)dest->h * 4, sizeof(double));
    size_t np = (size_t)input->w * (size_t)input->h;
    for (size_t pixel = 0; pixel < np; pixel++)
        for (int ch = 0; ch < 4; ch++)
            source_samples[pixel * 4 + (size_t)ch] =
                (double)((input->px[pixel] >> (8 * ch)) & 0xff);
    /* Rows first (s->h lines of s->w), then columns (d->w lines of s->h). */
    area_pass(source_samples, row_samples, input->h, input->w, dest->w, 4, (size_t)input->w * 4, 4,
              (size_t)dest->w * 4);
    area_pass(row_samples, dest_samples, dest->w, input->h, dest->h, (size_t)dest->w * 4, 4,
              (size_t)dest->w * 4, 4);
    size_t nd = (size_t)dest->w * (size_t)dest->h;
    for (size_t pixel = 0; pixel < nd; pixel++) {
        unsigned channels[4];
        for (int ch = 0; ch < 4; ch++) {
            double x = floor(dest_samples[pixel * 4 + (size_t)ch] + 0.5);
            channels[ch] = x < 0 ? 0 : x > 255 ? 255 : (unsigned)x;
        }
        /* Keep it valid premultiplied colour despite rounding. */
        for (int ch = 0; ch < 3; ch++)
            if (channels[ch] > channels[3])
                channels[ch] = channels[3];
        dest->px[pixel] =
            channels[0] | channels[1] << 8 | channels[2] << 16 | (uint32_t)channels[3] << 24;
    }
    xd_free(dest_samples);
    xd_free(row_samples);
    xd_free(source_samples);
}

/* s scaled to d's size: an area average going down, otherwise cairo's
 * bilinear filter. EXTEND_PAD repeats the edge pixels outwards, so the
 * edges don't fade into transparency. */
static void scale_smooth(const img *input, img *dest) {
    if (dest->w <= input->w && dest->h <= input->h) {
        scale_area(input, dest);
        return;
    }
    cairo_surface_t *ss = cairo_image_surface_create_for_data(
        (unsigned char *)input->px, CAIRO_FORMAT_ARGB32, input->w, input->h, input->w * 4);
    cairo_surface_t *ds = cairo_image_surface_create_for_data(
        (unsigned char *)dest->px, CAIRO_FORMAT_ARGB32, dest->w, dest->h, dest->w * 4);
    cairo_t *cairo = cairo_create(ds);
    cairo_scale(cairo, (double)dest->w / input->w, (double)dest->h / input->h);
    cairo_set_source_surface(cairo, ss, 0, 0);
    cairo_pattern_t *pat = cairo_get_source(cairo);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cairo);
    cairo_destroy(cairo);
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
static void scale2x(const img *input, img *dest) {
    for (int y = 0; y < input->h; y++) {
        for (int x = 0; x < input->w; x++) {
            uint32_t up = px_at(input, x, y - 1), dd = px_at(input, x - 1, y);
            uint32_t centre = px_at(input, x, y), right = px_at(input, x + 1, y);
            uint32_t down = px_at(input, x, y + 1);
            uint32_t e0 = centre, e1 = centre, e2 = centre, e3 = centre;
            if (up != down && dd != right) {
                if (dd == up)
                    e0 = dd;
                if (up == right)
                    e1 = right;
                if (dd == down)
                    e2 = dd;
                if (down == right)
                    e3 = right;
            }
            px_set(dest, 2 * x, 2 * y, e0);
            px_set(dest, 2 * x + 1, 2 * y, e1);
            px_set(dest, 2 * x, 2 * y + 1, e2);
            px_set(dest, 2 * x + 1, 2 * y + 1, e3);
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
static void scale3x(const img *input, img *dest) {
    for (int y = 0; y < input->h; y++) {
        for (int x = 0; x < input->w; x++) {
            uint32_t up_left = px_at(input, x - 1, y - 1), up = px_at(input, x, y - 1);
            uint32_t up_right = px_at(input, x + 1, y - 1), dd = px_at(input, x - 1, y);
            uint32_t centre = px_at(input, x, y), right = px_at(input, x + 1, y);
            uint32_t down_left = px_at(input, x - 1, y + 1), down = px_at(input, x, y + 1);
            uint32_t i = px_at(input, x + 1, y + 1);
            uint32_t block[9] = {centre, centre, centre, centre, centre,
                                 centre, centre, centre, centre};
            if (up != down && dd != right) {
                if (dd == up)
                    block[0] = dd;
                if ((dd == up && centre != up_right) || (up == right && centre != up_left))
                    block[1] = up;
                if (up == right)
                    block[2] = right;
                if ((dd == up && centre != down_left) || (dd == down && centre != up_left))
                    block[3] = dd;
                if ((up == right && centre != i) || (down == right && centre != up_right))
                    block[5] = right;
                if (dd == down)
                    block[6] = dd;
                if ((dd == down && centre != i) || (down == right && centre != down_left))
                    block[7] = down;
                if (down == right)
                    block[8] = right;
            }
            for (int k = 0; k < 9; k++)
                px_set(dest, 3 * x + k % 3, 3 * y + k / 3, block[k]);
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

static inline sai_nb sai_read(const img *input, int x, int y) {
    return (sai_nb){
        px_at(input, x - 1, y - 1), px_at(input, x, y - 1),     px_at(input, x + 1, y - 1),
        px_at(input, x + 2, y - 1), px_at(input, x - 1, y),     px_at(input, x, y),
        px_at(input, x + 1, y),     px_at(input, x + 2, y),     px_at(input, x - 1, y + 1),
        px_at(input, x, y + 1),     px_at(input, x + 1, y + 1), px_at(input, x + 2, y + 1),
        px_at(input, x - 1, y + 2), px_at(input, x, y + 2),     px_at(input, x + 1, y + 2),
        px_at(input, x + 2, y + 2),
    };
}

/* When both diagonals of the square are single colours (a == d, b == c),
 * they cross, and one must be drawn over the other. The two pixels outside
 * each corner of the square (g,e at a; f,k at b; h,n at c; l,o at d) vote:
 * a pair that is all of one diagonal's colour is background, which says
 * the other diagonal is the thin line to keep joined up.
 * > 0: keep a's diagonal; < 0: keep b's; 0: undecided. */
static int sai_vote(const sai_nb *neighbours) {
    const uint32_t pairs[4][2] = {{neighbours->g, neighbours->e},
                                  {neighbours->f, neighbours->k},
                                  {neighbours->h, neighbours->n},
                                  {neighbours->l, neighbours->o}};
    int votes = 0;
    for (int k = 0; k < 4; k++) {
        if (pairs[k][0] == neighbours->b && pairs[k][1] == neighbours->b)
            votes++;
        else if (pairs[k][0] == neighbours->a && pairs[k][1] == neighbours->a)
            votes--;
    }
    return votes;
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
static void sai2x(const img *input, img *dst) {
    for (int y = 0; y < input->h; y++) {
        for (int x = 0; x < input->w; x++) {
            sai_nb neighbours = sai_read(input, x, y);
            uint32_t right, below, centre;
            if (neighbours.a == neighbours.d && neighbours.b != neighbours.c) {
                bool snap_right = (neighbours.a == neighbours.e && neighbours.b == neighbours.l) ||
                                  (neighbours.a == neighbours.c && neighbours.a == neighbours.f &&
                                   neighbours.b != neighbours.e && neighbours.b == neighbours.j);
                bool snap_below = (neighbours.a == neighbours.g && neighbours.c == neighbours.o) ||
                                  (neighbours.a == neighbours.b && neighbours.a == neighbours.h &&
                                   neighbours.g != neighbours.c && neighbours.c == neighbours.m);
                right = snap_right ? neighbours.a : half(neighbours.a, neighbours.b);
                below = snap_below ? neighbours.a : half(neighbours.a, neighbours.c);
                centre = neighbours.a;
            } else if (neighbours.b == neighbours.c && neighbours.a != neighbours.d) {
                bool snap_right = (neighbours.b == neighbours.f && neighbours.a == neighbours.h) ||
                                  (neighbours.b == neighbours.e && neighbours.b == neighbours.d &&
                                   neighbours.a != neighbours.f && neighbours.a == neighbours.i);
                bool snap_below = (neighbours.c == neighbours.h && neighbours.a == neighbours.f) ||
                                  (neighbours.c == neighbours.g && neighbours.c == neighbours.d &&
                                   neighbours.a != neighbours.h && neighbours.a == neighbours.i);
                right = snap_right ? neighbours.b : half(neighbours.a, neighbours.b);
                below = snap_below ? neighbours.c : half(neighbours.a, neighbours.c);
                centre = neighbours.b;
            } else if (neighbours.a == neighbours.d && neighbours.b == neighbours.c) {
                if (neighbours.a == neighbours.b) {
                    right = below = centre = neighbours.a;
                } else {
                    right = half(neighbours.a, neighbours.b);
                    below = half(neighbours.a, neighbours.c);
                    int vote = sai_vote(&neighbours);
                    centre = vote > 0 ? neighbours.a
                             : vote < 0
                                 ? neighbours.b
                                 : quarter4(neighbours.a, neighbours.b, neighbours.c, neighbours.d);
                }
            } else {
                centre = quarter4(neighbours.a, neighbours.b, neighbours.c, neighbours.d);
                if (neighbours.a == neighbours.c && neighbours.a == neighbours.f &&
                    neighbours.b != neighbours.e && neighbours.b == neighbours.j)
                    right = neighbours.a;
                else if (neighbours.b == neighbours.e && neighbours.b == neighbours.d &&
                         neighbours.a != neighbours.f && neighbours.a == neighbours.i)
                    right = neighbours.b;
                else
                    right = half(neighbours.a, neighbours.b);
                if (neighbours.a == neighbours.b && neighbours.a == neighbours.h &&
                    neighbours.g != neighbours.c && neighbours.c == neighbours.m)
                    below = neighbours.a;
                else if (neighbours.c == neighbours.g && neighbours.c == neighbours.d &&
                         neighbours.a != neighbours.h && neighbours.a == neighbours.i)
                    below = neighbours.c;
                else
                    below = half(neighbours.a, neighbours.c);
            }
            px_set(dst, 2 * x, 2 * y, neighbours.a);
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
static void super2xsai(const img *input, img *dst) {
    for (int y = -1; y < input->h; y++) {
        for (int x = 0; x < input->w; x++) {
            sai_nb neighbours = sai_read(input, x, y);
            uint32_t tl = neighbours.a, tr, bl = neighbours.c, br;
            if (neighbours.b == neighbours.c && neighbours.a != neighbours.d) {
                tr = br = neighbours.b;
            } else if (neighbours.a == neighbours.d && neighbours.b != neighbours.c) {
                tr = br = neighbours.a;
            } else if (neighbours.a == neighbours.d && neighbours.b == neighbours.c) {
                int vote = sai_vote(&neighbours);
                tr = br = vote > 0   ? neighbours.a
                          : vote < 0 ? neighbours.b
                                     : half(neighbours.a, neighbours.b);
            } else {
                if (neighbours.d == neighbours.b && neighbours.d == neighbours.n &&
                    neighbours.c != neighbours.o && neighbours.d != neighbours.m)
                    br = three_quarters(neighbours.d, neighbours.c);
                else if (neighbours.a == neighbours.c && neighbours.c == neighbours.o &&
                         neighbours.n != neighbours.d && neighbours.c != neighbours.p)
                    br = three_quarters(neighbours.c, neighbours.d);
                else
                    br = half(neighbours.c, neighbours.d);
                if (neighbours.b == neighbours.d && neighbours.b == neighbours.e &&
                    neighbours.a != neighbours.f && neighbours.b != neighbours.i)
                    tr = three_quarters(neighbours.b, neighbours.a);
                else if (neighbours.a == neighbours.c && neighbours.a == neighbours.f &&
                         neighbours.e != neighbours.b && neighbours.a != neighbours.j)
                    tr = three_quarters(neighbours.a, neighbours.b);
                else
                    tr = half(neighbours.a, neighbours.b);
            }
            if ((neighbours.a == neighbours.d && neighbours.b != neighbours.c &&
                 neighbours.g == neighbours.a && neighbours.a != neighbours.o) ||
                (neighbours.a == neighbours.h && neighbours.b == neighbours.a &&
                 neighbours.g != neighbours.c && neighbours.a != neighbours.m))
                bl = half(neighbours.c, neighbours.a);
            if ((neighbours.c == neighbours.b && neighbours.a != neighbours.d &&
                 neighbours.h == neighbours.c && neighbours.c != neighbours.f) ||
                (neighbours.g == neighbours.c && neighbours.d == neighbours.c &&
                 neighbours.h != neighbours.a && neighbours.c != neighbours.i))
                tl = half(neighbours.c, neighbours.a);
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
static void supereagle(const img *input, img *dst) {
    for (int y = -1; y < input->h; y++) {
        for (int x = -1; x < input->w; x++) {
            sai_nb neighbours = sai_read(input, x, y);
            uint32_t qa, qb, qc, qd;
            if (neighbours.b == neighbours.c && neighbours.a != neighbours.d) {
                qb = qc = neighbours.b;
                qa = (neighbours.h == neighbours.c || neighbours.f == neighbours.b)
                         ? three_quarters(neighbours.b, neighbours.a)
                         : half(neighbours.a, neighbours.b);
                qd = (neighbours.k == neighbours.b || neighbours.n == neighbours.c)
                         ? three_quarters(neighbours.c, neighbours.d)
                         : half(neighbours.c, neighbours.d);
            } else if (neighbours.a == neighbours.d && neighbours.b != neighbours.c) {
                qa = qd = neighbours.a;
                qb = (neighbours.l == neighbours.d || neighbours.e == neighbours.a)
                         ? three_quarters(neighbours.a, neighbours.b)
                         : half(neighbours.b, neighbours.a);
                qc = (neighbours.g == neighbours.a || neighbours.o == neighbours.d)
                         ? three_quarters(neighbours.d, neighbours.c)
                         : half(neighbours.d, neighbours.c);
            } else if (neighbours.a == neighbours.d && neighbours.b == neighbours.c) {
                int vote = neighbours.a == neighbours.b ? 0 : sai_vote(&neighbours);
                if (vote < 0) {
                    qb = qc = neighbours.b;
                    qa = qd = half(neighbours.a, neighbours.b);
                } else if (vote > 0) {
                    qa = qd = neighbours.a;
                    qb = qc = half(neighbours.a, neighbours.b);
                } else {
                    qa = qd = neighbours.a;
                    qb = qc = neighbours.b;
                }
            } else {
                qa = mix8(neighbours.a, 6, neighbours.b, 1, neighbours.c, 1, 0, 0);
                qb = mix8(neighbours.b, 6, neighbours.a, 1, neighbours.d, 1, 0, 0);
                qc = mix8(neighbours.c, 6, neighbours.a, 1, neighbours.d, 1, 0, 0);
                qd = mix8(neighbours.d, 6, neighbours.b, 1, neighbours.c, 1, 0, 0);
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
static void pixel_art_step(img *bitmap, xd_image_filter filter) {
    int step_factor = filter_step(filter);
    img stepped = img_new(bitmap->w * step_factor, bitmap->h * step_factor);
    switch (filter) {
    case XD_FILTER_SCALE2X:
        scale2x(bitmap, &stepped);
        break;
    case XD_FILTER_SCALE3X:
        scale3x(bitmap, &stepped);
        break;
    case XD_FILTER_SCALE4X: {
        img mid = img_new(bitmap->w * 2, bitmap->h * 2);
        scale2x(bitmap, &mid);
        scale2x(&mid, &stepped);
        img_free(&mid);
        break;
    }
    case XD_FILTER_2XSAI:
        sai2x(bitmap, &stepped);
        break;
    case XD_FILTER_SUPER2XSAI:
        super2xsai(bitmap, &stepped);
        break;
    case XD_FILTER_SUPEREAGLE:
        supereagle(bitmap, &stepped);
        break;
    case XD_FILTER_NEAREST:
    case XD_FILTER_INTEGER:
    case XD_FILTER_SMOOTH:
    case XD_FILTER_COUNT:
        XD_ASSERT(false); /* not pixel-art filters */
        break;
    }
    img_free(bitmap);
    *bitmap = stepped;
}

/* s scaled to dw x dh, which is about s's size times `scale`. Returns
 * {nullptr} if a size would be beyond cairo's limits. */
static img scale_img(const img *input, int dw, int dh, double scale, xd_image_filter filter) {
    if (too_big(dw, dh))
        return (img){};
    img dest = img_new(dw, dh);
    if (dw == input->w && dh == input->h) {
        memcpy(dest.px, input->px, (size_t)dw * (size_t)dh * sizeof(uint32_t));
        return dest;
    }
    if (filter == XD_FILTER_NEAREST) {
        scale_nearest(input, &dest);
        return dest;
    }
    if (filter == XD_FILTER_SMOOTH || scale < 1) {
        scale_smooth(input, &dest);
        return dest;
    }

    /* Enlarge exactly to at least the target size, then smooth down. */
    img big;
    if (filter == XD_FILTER_INTEGER) {
        int k = 1;
        while (input->w * k < dw || input->h * k < dh)
            k++;
        if (too_big((int64_t)input->w * k, (int64_t)input->h * k)) {
            img_free(&dest);
            return (img){};
        }
        big = img_new(input->w * k, input->h * k);
        scale_blocks(input, &big, k);
    } else {
        int step_factor = filter_step(filter);
        big = img_new(input->w, input->h);
        memcpy(big.px, input->px, (size_t)input->w * (size_t)input->h * sizeof(uint32_t));
        while (big.w < dw || big.h < dh) {
            if (too_big((int64_t)big.w * step_factor, (int64_t)big.h * step_factor)) {
                img_free(&big);
                img_free(&dest);
                return (img){};
            }
            pixel_art_step(&big, filter);
        }
    }
    if (big.w == dw && big.h == dh) {
        img_free(&dest);
        return big;
    }
    scale_smooth(&big, &dest);
    img_free(&big);
    return dest;
}

/* A length times the scale, rounded, at least 1; -1 if out of range. */
static int scaled_len(int len, double scale) {
    double rounded = round(len * scale);
    if (rounded > MAX_SIDE)
        return -1;
    return rounded < 1 ? 1 : (int)rounded;
}

static bool valid(double scale, xd_image_filter filter) {
    return isfinite(scale) && scale > 0 && (unsigned)filter < XD_FILTER_COUNT;
}

cairo_surface_t *xd_image_scale(cairo_surface_t *src, double scale, xd_image_filter filter) {
    if (!valid(scale, filter))
        return nullptr;
    img input = img_from_surface(src);
    if (!input.px)
        return nullptr;
    int dw = scaled_len(input.w, scale), dh = scaled_len(input.h, scale);
    cairo_surface_t *out = nullptr;
    if (dw > 0 && dh > 0) {
        img dest = scale_img(&input, dw, dh, scale, filter);
        if (dest.px)
            out = img_to_surface(&dest);
        img_free(&dest);
    }
    img_free(&input);
    return out;
}

cairo_surface_t *xd_image_scale_frames(cairo_surface_t *src, int count, bool vertical,
                                       double scale, xd_image_filter filter) {
    if (!valid(scale, filter) || count < 1)
        return nullptr;
    img input = img_from_surface(src);
    if (!input.px)
        return nullptr;
    int along = vertical ? input.h : input.w;
    if (along % count != 0) {
        img_free(&input);
        return nullptr;
    }
    int fw = vertical ? input.w : input.w / count;
    int fh = vertical ? input.h / count : input.h;
    int dw = scaled_len(fw, scale), dh = scaled_len(fh, scale);
    int ow = vertical ? dw : dw * count, oh = vertical ? dh * count : dh;
    if (dw <= 0 || dh <= 0 || too_big(ow, oh) || (int64_t)(vertical ? dh : dw) * count > MAX_SIDE) {
        img_free(&input);
        return nullptr;
    }

    img out = img_new(ow, oh);
    img frame = img_new(fw, fh);
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        int fx = vertical ? 0 : i * fw, fy = vertical ? i * fh : 0;
        for (int y = 0; y < fh; y++)
            memcpy(frame.px + (size_t)y * (size_t)fw,
                   input.px + (size_t)(fy + y) * (size_t)input.w + (size_t)fx,
                   (size_t)fw * sizeof(uint32_t));
        img dest = scale_img(&frame, dw, dh, scale, filter);
        if (!dest.px) {
            ok = false;
            break;
        }
        int ox = vertical ? 0 : i * dw, oy = vertical ? i * dh : 0;
        for (int y = 0; y < dh; y++)
            memcpy(out.px + (size_t)(oy + y) * (size_t)ow + (size_t)ox,
                   dest.px + (size_t)y * (size_t)dw, (size_t)dw * sizeof(uint32_t));
        img_free(&dest);
    }
    cairo_surface_t *res = ok ? img_to_surface(&out) : nullptr;
    img_free(&frame);
    img_free(&out);
    img_free(&input);
    return res;
}
