/* SPDX-License-Identifier: MIT */
#include "vcgt.h"

#ifdef XD_HAVE_LCMS2
#include <lcms2.h>

bool xd_vcgt_load(const char *path, size_t count, float *out) {
    if (count < 2)
        return false;
    cmsHPROFILE profile = cmsOpenProfileFromFile(path, "r");
    if (profile == nullptr)
        return false;
    /* Little CMS reads both of vcgt's encodings (a table or a formula)
     * into three tone curves. */
    cmsToneCurve **curves = cmsReadTag(profile, cmsSigVcgtTag);
    bool ok = curves != nullptr && curves[0] != nullptr && curves[1] != nullptr &&
              curves[2] != nullptr;
    for (size_t channel = 0; ok && channel < 3; channel++) {
        for (size_t i = 0; i < count; i++)
            out[channel * count + i] =
                cmsEvalToneCurveFloat(curves[channel], (float)i / (float)(count - 1));
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
