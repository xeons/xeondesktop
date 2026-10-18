/* SPDX-License-Identifier: MIT */
#include "vcgt.h"

#ifdef XD_HAVE_LCMS2
#include <lcms2.h>

bool xd_vcgt_load(const char *path, size_t n, float *out) {
    if (n < 2)
        return false;
    cmsHPROFILE profile = cmsOpenProfileFromFile(path, "r");
    if (profile == nullptr)
        return false;
    /* Little CMS reads both of vcgt's encodings (a table or a formula)
     * into three tone curves. */
    cmsToneCurve **curves = cmsReadTag(profile, cmsSigVcgtTag);
    bool ok = curves != nullptr && curves[0] != nullptr && curves[1] != nullptr &&
              curves[2] != nullptr;
    for (size_t c = 0; ok && c < 3; c++) {
        for (size_t i = 0; i < n; i++)
            out[c * n + i] = cmsEvalToneCurveFloat(curves[c], (float)i / (float)(n - 1));
    }
    cmsCloseProfile(profile);
    return ok;
}
#else
bool xd_vcgt_load([[maybe_unused]] const char *path, [[maybe_unused]] size_t n,
                  [[maybe_unused]] float *out) {
    return false;
}
#endif
