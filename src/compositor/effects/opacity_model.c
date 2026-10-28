/* SPDX-License-Identifier: MIT
 * Window Opacity's values (opacity_model.h).
 */
#include "opacity_model.h"

#include "../../core/common.h"
#include "../../settings/effects.h"

/* Alt and the wheel go no lower, so a window can't vanish. */
static constexpr float LOWEST_OPACITY = 0.05f;

float xd_opacity_get_lowest(xd_opacity_property property) {
    return property == XD_OPACITY_PROPERTY_OPACITY ? LOWEST_OPACITY : 0.0f;
}

float xd_opacity_step_value(xd_opacity_property property, float value, float step, int steps) {
    float changed = value + step * (float)steps;
    return XD_CLAMP(changed, xd_opacity_get_lowest(property), 1.0f);
}

float xd_opacity_choose_rule(const xd_opacity_rule *rules, size_t count, const char *app_id,
                             const char *title, bool is_dialog) {
    for (size_t i = 0; i < count; i++) {
        if (rules[i].windows[0] != '\0' &&
            xd_effects_match_window(rules[i].windows, app_id, title, is_dialog))
            return rules[i].value;
    }
    return 1.0f;
}

bool xd_opacity_is_colored(float brightness, float saturation) {
    return brightness < 1.0f || saturation < 1.0f;
}

void xd_opacity_adjust_color(float rgb[3], float brightness, float saturation) {
    float grey = rgb[0] * XD_OPACITY_RED_WEIGHT + rgb[1] * XD_OPACITY_GREEN_WEIGHT +
                 rgb[2] * XD_OPACITY_BLUE_WEIGHT;
    for (size_t i = 0; i < 3; i++)
        rgb[i] = (grey + (rgb[i] - grey) * saturation) * brightness;
}
