/* SPDX-License-Identifier: MIT */
#include <sane/saneopts.h>
#include <string.h>

#include "scanlib.h"

/* Backends name their modes as they like; these are the words they use,
 * best first (SANE's standard value, then the usual others). */
static const char *const color_words[] = {SANE_VALUE_SCAN_MODE_COLOR, "rgb", "color", "colour",
                                          nullptr};
static const char *const gray_words[] = {SANE_VALUE_SCAN_MODE_GRAY, "grayscale", "gray", "grey",
                                         nullptr};
static const char *const bw_words[] = {SANE_VALUE_SCAN_MODE_LINEART, "binary", "black",
                                       "halftone", "lineart", nullptr};

static const char *find_mode(const char *const *words, const char *const *modes, size_t n) {
    /* The exact word first, then a mode containing it ("24bit Color"). */
    for (const char *const *w = words; *w != nullptr; w++) {
        for (size_t i = 0; i < n; i++) {
            if (g_ascii_strcasecmp(modes[i], *w) == 0)
                return modes[i];
        }
    }
    for (const char *const *w = words; *w != nullptr; w++) {
        for (size_t i = 0; i < n; i++) {
            char *lower = g_ascii_strdown(modes[i], -1);
            char *word = g_ascii_strdown(*w, -1);
            bool hit = strstr(lower, word) != nullptr;
            g_free(word);
            g_free(lower);
            if (hit)
                return modes[i];
        }
    }
    return nullptr;
}

const char *xd_scan_mode_for(xd_picture_type type, const char *const *modes, size_t n,
                             bool *one_bit) {
    *one_bit = false;
    switch (type) {
    case XD_PICTURE_COLOR:
        return find_mode(color_words, modes, n);
    case XD_PICTURE_GRAY:
        return find_mode(gray_words, modes, n);
    case XD_PICTURE_BW: {
        const char *bw = find_mode(bw_words, modes, n);
        if (bw != nullptr)
            return bw;
        const char *gray = find_mode(gray_words, modes, n);
        *one_bit = gray != nullptr;
        return gray;
    }
    default:
        return nullptr;
    }
}

void xd_scan_resolutions_in_range(int min, int max, GArray *out) {
    static const int usual[] = {75, 100, 150, 200, 300, 600, 1200};
    guint before = out->len;
    for (size_t i = 0; i < G_N_ELEMENTS(usual); i++) {
        if (usual[i] >= min && usual[i] <= max)
            g_array_append_val(out, usual[i]);
    }
    if (out->len == before) {
        g_array_append_val(out, min);
        if (max != min)
            g_array_append_val(out, max);
    }
}

int xd_scan_preview_dpi(int min, int max) { return CLAMP(50, min, MAX(min, max)); }

char *xd_scan_next_file(const char *dir, const char *name, const char *ext) {
    for (int i = 1; i <= 9999; i++) {
        char *base = g_strdup_printf("%s %03d.%s", name, i, ext);
        char *path = g_build_filename(dir, base, nullptr);
        g_free(base);
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            return path;
        g_free(path);
    }
    return nullptr;
}

bool xd_scan_name_valid(const char *name) {
    return name != nullptr && name[0] != '\0' && strchr(name, '/') == nullptr &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

/* Sample `i` of a line, scaled to 8 bits. 16-bit samples are in the
 * host's byte order (SANE says so). */
static guint8 sample(const SANE_Byte *src, int depth, int i) {
    if (depth == 8)
        return src[i];
    guint16 v;
    memcpy(&v, src + (size_t)i * 2, sizeof(v));
    return (guint8)(v >> 8);
}

static bool bit(const SANE_Byte *src, int i) {
    return (src[i / 8] & (0x80 >> (i % 8))) != 0;
}

bool xd_scan_convert_line(SANE_Frame frame, int depth, int pixels, const SANE_Byte *src,
                          guint8 *rgb) {
    if (depth != 1 && depth != 8 && depth != 16)
        return false;
    for (int x = 0; x < pixels; x++) {
        guint8 *px = rgb + (size_t)x * 3;
        switch (frame) {
        case SANE_FRAME_GRAY: {
            /* At depth 1 a set bit is black. */
            guint8 v = depth == 1 ? (bit(src, x) ? 0 : 255) : sample(src, depth, x);
            px[0] = px[1] = px[2] = v;
            break;
        }
        case SANE_FRAME_RGB:
            for (int c = 0; c < 3; c++)
                px[c] = depth == 1 ? (bit(src, x * 3 + c) ? 255 : 0)
                                   : sample(src, depth, x * 3 + c);
            break;
        case SANE_FRAME_RED:
        case SANE_FRAME_GREEN:
        case SANE_FRAME_BLUE: {
            int c = frame == SANE_FRAME_RED ? 0 : frame == SANE_FRAME_GREEN ? 1 : 2;
            px[c] = depth == 1 ? (bit(src, x) ? 255 : 0) : sample(src, depth, x);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}
