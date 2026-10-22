/* SPDX-License-Identifier: MIT
 * A monitor profile's calibration: the video card gamma table (the ICC
 * "vcgt" tag that calibration tools such as DisplayCAL write), read with
 * Little CMS.
 */
#ifndef XD_COMPOSITOR_VCGT_H
#define XD_COMPOSITOR_VCGT_H

#include <stddef.h>

/* Samples the ICC profile's vcgt curves at n points each into out (3 x n
 * values 0..1: red, green, blue). False if the file can't be read, has no
 * vcgt, or this build has no Little CMS. */
bool xd_vcgt_load(const char *path, size_t count, float *out);

#endif
