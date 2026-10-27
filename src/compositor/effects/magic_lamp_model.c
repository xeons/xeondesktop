/* SPDX-License-Identifier: MIT
 * Magic Lamp's shape (magic_lamp_model.h).
 */
#include "magic_lamp_model.h"

#include <math.h>

/* The share of the effect spent bending before the window slides. */
static constexpr double BEND_SHARE = 0.4;
static constexpr double LAMP_PI = 3.14159265358979323846;

static double lamp_clamp(double value, double low, double high) {
    return value < low ? low : value > high ? high : value;
}

static double lamp_smooth(double value) {
    value = lamp_clamp(value, 0, 1);
    return value * value * (3 - 2 * value);
}

static double lamp_lerp(double from, double to, double share) {
    return from + (to - from) * share;
}

/* Turned so that `direction` is down (b grows toward the button). */
static void lamp_to_canonical(xd_lamp_direction direction, double x, double y, double *across,
                              double *along) {
    switch (direction) {
    case XD_LAMP_DOWN:
        *across = x;
        *along = y;
        break;
    case XD_LAMP_UP:
        *across = x;
        *along = -y;
        break;
    case XD_LAMP_RIGHT:
        *across = y;
        *along = x;
        break;
    case XD_LAMP_LEFT:
        *across = y;
        *along = -x;
        break;
    }
}

static void lamp_from_canonical(xd_lamp_direction direction, double across, double along,
                                double *x, double *y) {
    switch (direction) {
    case XD_LAMP_DOWN:
        *x = across;
        *y = along;
        break;
    case XD_LAMP_UP:
        *x = across;
        *y = -along;
        break;
    case XD_LAMP_RIGHT:
        *x = along;
        *y = across;
        break;
    case XD_LAMP_LEFT:
        *x = -along;
        *y = across;
        break;
    }
}

static xd_lamp_rect lamp_rect_to_canonical(xd_lamp_direction direction, xd_lamp_rect rect) {
    double first_across = 0, first_along = 0, second_across = 0, second_along = 0;
    lamp_to_canonical(direction, rect.x, rect.y, &first_across, &first_along);
    lamp_to_canonical(direction, rect.x + rect.width, rect.y + rect.height, &second_across,
                      &second_along);
    return (xd_lamp_rect){fmin(first_across, second_across), fmin(first_along, second_along),
                          fabs(second_across - first_across), fabs(second_along - first_along)};
}

xd_lamp_direction xd_lamp_get_direction(xd_lamp_rect window, xd_lamp_rect icon) {
    double icon_x = icon.x + icon.width / 2;
    double icon_y = icon.y + icon.height / 2;
    /* How far past each edge the button's middle is. */
    double below = icon_y - (window.y + window.height);
    double above = window.y - icon_y;
    double right = icon_x - (window.x + window.width);
    double left = window.x - icon_x;
    double window_x = window.x + window.width / 2;
    double window_y = window.y + window.height / 2;
    if (below <= 0 && above <= 0 && right <= 0 && left <= 0) {
        /* Over the window: the way from its middle. */
        double dx = icon_x - window_x;
        double dy = icon_y - window_y;
        if (fabs(dy) * window.width >= fabs(dx) * window.height)
            return dy >= 0 ? XD_LAMP_DOWN : XD_LAMP_UP;
        return dx >= 0 ? XD_LAMP_RIGHT : XD_LAMP_LEFT;
    }
    double best = below;
    xd_lamp_direction direction = XD_LAMP_DOWN;
    if (above > best) {
        best = above;
        direction = XD_LAMP_UP;
    }
    if (right > best) {
        best = right;
        direction = XD_LAMP_RIGHT;
    }
    if (left > best)
        direction = XD_LAMP_LEFT;
    return direction;
}

void xd_lamp_init(xd_lamp *lamp, xd_lamp_rect window, xd_lamp_rect icon) {
    lamp->direction = xd_lamp_get_direction(window, icon);
    lamp->window = lamp_rect_to_canonical(lamp->direction, window);
    lamp->icon = lamp_rect_to_canonical(lamp->direction, icon);
}

void xd_lamp_get_point(const xd_lamp *lamp, double across, double down, double progress,
                       double *x, double *y) {
    /* The point's place on the turned window: `side` across it, `depth`
     * from its far edge (0) to the edge nearer the button (1). */
    double side = across;
    double depth = down;
    switch (lamp->direction) {
    case XD_LAMP_DOWN:
        break;
    case XD_LAMP_UP:
        depth = 1 - down;
        break;
    case XD_LAMP_RIGHT:
        side = down;
        depth = across;
        break;
    case XD_LAMP_LEFT:
        side = down;
        depth = 1 - across;
        break;
    }
    const xd_lamp_rect *window = &lamp->window;
    const xd_lamp_rect *icon = &lamp->icon;
    progress = lamp_clamp(progress, 0, 1);
    double bend = lamp_smooth(progress / BEND_SHARE);
    double slide = lamp_smooth((progress - BEND_SHARE) / (1 - BEND_SHARE));

    /* Sliding: the far edge goes to the button's near edge. */
    double travel = icon->y - window->y;
    double along = window->y + depth * window->height + slide * travel;
    double bottom = window->y + window->height + slide * travel;
    /* What's past the button's near edge is squeezed into the button
     * (allowing for a window over it already, which isn't squeezed at
     * the start). */
    double overlap = fmax(window->y + window->height - icon->y, 0);
    double inside = bottom - icon->y;
    double room = icon->height + (1 - slide) * overlap;
    if (along > icon->y && inside > room && inside > 0)
        along = icon->y + (along - icon->y) * room / inside;

    /* Bending: across, the window narrows toward the button the nearer
     * the button it is, along a half cosine. */
    double reach = fmax(travel, 1);
    double nearness = lamp_clamp((along - window->y) / reach, 0, 1);
    double width_share = 1 - bend * (1 - cos(LAMP_PI * nearness)) / 2;
    double left = lamp_lerp(icon->x, window->x, width_share);
    double width = lamp_lerp(icon->width, window->width, width_share);
    lamp_from_canonical(lamp->direction, left + side * width, along, x, y);
}
