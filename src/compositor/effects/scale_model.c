/* SPDX-License-Identifier: MIT
 * Scale's maths (scale_model.h): Compiz's scale plugin's slot layout and
 * its velocity model.
 */
#include "scale_model.h"

#include <math.h>

#include "../../core/alloc.h"

typedef struct scale_cell {
    xd_effect_rect rect;
    bool filled;
} scale_cell;

void xd_scale_layout(const xd_effect_rect *windows, size_t count, xd_effect_rect area,
                     double spacing, xd_scale_slot *slots) {
    if (count == 0)
        return;
    scale_cell *cells = XD_NEW_ARRAY(scale_cell, count);
    /* Compiz's layoutSlotsForArea: rows of equal height, each split into
     * equal widths, the last row holding what's left. */
    double root = sqrt((double)count + 1.0);
    size_t lines = (size_t)root;
    size_t per_line = (count + lines - 1) / lines;
    double height = (area.height - (double)(lines + 1) * spacing) / (double)lines;
    double y = area.y + spacing;
    size_t made = 0;
    for (size_t line = 0; line < lines && made < count; line++) {
        size_t in_line = XD_MIN(count - made, per_line);
        double width = (area.width - (double)(in_line + 1) * spacing) / (double)in_line;
        double x = area.x + spacing;
        for (size_t i = 0; i < in_line; i++) {
            cells[made++].rect = (xd_effect_rect){x, y, XD_MAX(width, 1.0), XD_MAX(height, 1.0)};
            x += width + spacing;
        }
        y += height + spacing;
    }
    /* fillInWindows: each window, top first, takes the free cell whose
     * centre is nearest its own. */
    for (size_t i = 0; i < count; i++) {
        const xd_effect_rect *window = &windows[i];
        double centre_x = window->x + window->width / 2, centre_y = window->y + window->height / 2;
        size_t best = 0;
        double best_distance = HUGE_VAL;
        for (size_t cell = 0; cell < count; cell++) {
            if (cells[cell].filled)
                continue;
            const xd_effect_rect *rect = &cells[cell].rect;
            double dx = rect->x + rect->width / 2 - centre_x;
            double dy = rect->y + rect->height / 2 - centre_y;
            double distance = dx * dx + dy * dy;
            if (distance < best_distance) {
                best_distance = distance;
                best = cell;
            }
        }
        cells[best].filled = true;
        const xd_effect_rect *rect = &cells[best].rect;
        double window_width = XD_MAX(window->width, 1.0);
        double window_height = XD_MAX(window->height, 1.0);
        double scale =
            XD_MIN(XD_MIN(rect->width / window_width, rect->height / window_height), 1.0);
        slots[i] = (xd_scale_slot){
            .x = rect->x + (rect->width - window_width * scale) / 2,
            .y = rect->y + (rect->height - window_height * scale) / 2,
            .scale = scale,
        };
    }
    xd_free(cells);
}

/* adjustScaleVelocity: one step's change of speed towards the target. */
static double scale_adjust_velocity(double distance, double velocity, double pull, double weight,
                                    double low, double high) {
    double amount = XD_CLAMP(fabs(distance) * weight, low, high);
    return (amount * velocity + distance * pull) / (amount + 1.0);
}

bool xd_scale_step_motion(xd_scale_motion *motion, xd_scale_slot target, double milliseconds,
                          double speed, double timestep) {
    /* preparePaintScreen: the time since the last frame in steps of
     * `timestep`. */
    double amount = milliseconds * 0.05 * speed;
    int steps = (int)(amount / (0.5 * XD_MAX(timestep, 0.1)));
    if (steps < 1)
        steps = 1;
    double chunk = amount / steps;
    for (int i = 0; i < steps; i++) {
        double dx = target.x - motion->x, dy = target.y - motion->y;
        double ds = target.scale - motion->scale;
        motion->velocity_x = scale_adjust_velocity(dx, motion->velocity_x, 0.15, 1.5, 0.5, 5.0);
        motion->velocity_y = scale_adjust_velocity(dy, motion->velocity_y, 0.15, 1.5, 0.5, 5.0);
        motion->velocity_scale =
            scale_adjust_velocity(ds, motion->velocity_scale, 0.1, 7.0, 0.01, 0.15);
        if (fabs(dx) < 0.1 && fabs(motion->velocity_x) < 0.2 && fabs(dy) < 0.1 &&
            fabs(motion->velocity_y) < 0.2 && fabs(ds) < 0.001 &&
            fabs(motion->velocity_scale) < 0.002) {
            *motion = (xd_scale_motion){.x = target.x, .y = target.y, .scale = target.scale};
            return false;
        }
        motion->x += motion->velocity_x * chunk;
        motion->y += motion->velocity_y * chunk;
        motion->scale += motion->velocity_scale * chunk;
    }
    return true;
}

int xd_scale_find_thumbnail_at(const xd_effect_rect *windows, const xd_scale_motion *motions,
                               size_t count, double x, double y) {
    for (size_t i = count; i-- > 0;) {
        const xd_scale_motion *motion = &motions[i];
        if (x >= motion->x && y >= motion->y &&
            x < motion->x + windows[i].width * motion->scale &&
            y < motion->y + windows[i].height * motion->scale)
            return (int)i;
    }
    return -1;
}

size_t xd_scale_find_neighbour(const xd_effect_rect *windows, const xd_scale_slot *slots,
                               size_t count, size_t from, int dx, int dy) {
    double from_x = slots[from].x + windows[from].width * slots[from].scale / 2;
    double from_y = slots[from].y + windows[from].height * slots[from].scale / 2;
    size_t best = from;
    double best_score = HUGE_VAL;
    for (size_t i = 0; i < count; i++) {
        double x = slots[i].x + windows[i].width * slots[i].scale / 2 - from_x;
        double y = slots[i].y + windows[i].height * slots[i].scale / 2 - from_y;
        double along = x * dx + y * dy;
        if (i == from || along <= 0.5)
            continue;
        /* Straight ahead counts most: across costs twice as much. */
        double across = fabs(x * dy) + fabs(y * dx);
        double score = along + 2 * across;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}
