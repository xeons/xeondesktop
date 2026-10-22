/* SPDX-License-Identifier: MIT
 * Scanners and Cameras (xd-scanners): the parts without devices or GTK,
 * so the unit tests can check them. Uses SANE's headers only.
 */
#ifndef XD_SCANNERS_SCANLIB_H
#define XD_SCANNERS_SCANLIB_H

#include <glib.h>
#include <sane/sane.h>

/* XP's picture types in the Scanner and Camera Wizard. */
typedef enum xd_picture_type : uint8_t {
    XD_PICTURE_COLOR,
    XD_PICTURE_GRAY,
    XD_PICTURE_BW, /* black and white picture or text */
} xd_picture_type;

/* The scanner's mode (its SANE "mode" option) for `type`, from the
 * `count` modes it offers; nullptr when none fits. For black and white on a
 * scanner without a lineart mode, the gray mode, and *one_bit is set:
 * scan at depth 1. */
const char *xd_scan_mode_for(xd_picture_type type, const char *const *modes, size_t count,
                             bool *one_bit);

/* The resolutions to offer for a scanner whose "resolution" option is a
 * range: the usual ones (75 to 1200 dpi) inside it, or its ends when
 * none is. Appended to `out` (an array of int). */
void xd_scan_resolutions_in_range(int min, int max, GArray *out);

/* The resolution for a quick preview: about 50 dpi, inside the range. */
int xd_scan_preview_dpi(int min, int max);

/* The first free "<dir>/<name> 001.<ext>" (002, ... as XP numbered
 * pictures). Free with g_free; nullptr after 9999. */
char *xd_scan_next_file(const char *dir, const char *name, const char *ext);

/* Whether `name` can be a picture's name: not empty, no '/', not "." or
 * "..". */
bool xd_scan_name_valid(const char *name);

/* One line of SANE data (`frame`, `depth` 1, 8 or 16 bits, `pixels`
 * wide) into 8-bit RGB. A RED, GREEN or BLUE frame (three-pass
 * scanners) fills its channel only. False for formats it doesn't know. */
bool xd_scan_convert_line(SANE_Frame frame, int depth, int pixels, const SANE_Byte *src,
                          guint8 *rgb);

#endif
