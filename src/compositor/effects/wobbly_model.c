/* SPDX-License-Identifier: MIT */
#include "wobbly_model.h"

#include <math.h>

#include "../../core/common.h"

/* Compiz's: a step of 15 ms, masses of 15, springs pulling with half the
 * constant from each end. */
static constexpr double STEP_SECONDS = 0.015;
static constexpr double MASS = 15.0;
/* Let go, how strongly each mass is drawn to its place on the window,
 * against the springs between masses: enough to settle in about half a
 * second at the default constants, after a wobble or two. */
static constexpr double ANCHOR = 0.3;
/* Ever more steps after a stall would only freeze the compositor longer. */
static constexpr double MAX_BACKLOG_SECONDS = 0.2;
/* At rest: slower than this (units a step) and this close to its place. */
static constexpr double REST_SPEED = 0.02;
static constexpr double REST_DISTANCE = 0.3;

static void wobbly_get_rest_point(const xd_wobbly_model *model, int index, double *out_x,
                                  double *out_y) {
    int column = index % XD_WOBBLY_SIDE, row = index / XD_WOBBLY_SIDE;
    *out_x = model->rest_x + model->rest_width * column / (XD_WOBBLY_SIDE - 1);
    *out_y = model->rest_y + model->rest_height * row / (XD_WOBBLY_SIDE - 1);
}

void xd_wobbly_init(xd_wobbly_model *model, double left, double top, double width, double height,
                    double spring, double friction) {
    *model = (xd_wobbly_model){
        .spring = spring,
        .friction = friction,
        .grabbed = -1,
        .seed = 0x9e3779b9u,
    };
    xd_wobbly_set_rest(model, left, top, width, height);
    for (int i = 0; i < XD_WOBBLY_POINTS; i++)
        wobbly_get_rest_point(model, i, &model->points[i].x, &model->points[i].y);
}

void xd_wobbly_set_rest(xd_wobbly_model *model, double left, double top, double width,
                        double height) {
    model->rest_x = left;
    model->rest_y = top;
    model->rest_width = width;
    model->rest_height = height;
}

void xd_wobbly_grab(xd_wobbly_model *model, double pointer_x, double pointer_y) {
    int nearest = 0;
    double best = HUGE_VAL;
    for (int i = 0; i < XD_WOBBLY_POINTS; i++) {
        double rest_x = 0, rest_y = 0;
        wobbly_get_rest_point(model, i, &rest_x, &rest_y);
        double distance = (rest_x - pointer_x) * (rest_x - pointer_x) +
                          (rest_y - pointer_y) * (rest_y - pointer_y);
        if (distance < best) {
            best = distance;
            nearest = i;
        }
    }
    model->grabbed = nearest;
}

void xd_wobbly_release(xd_wobbly_model *model) {
    model->grabbed = -1;
}

/* -1..1, from a small LCG: shivers needn't be random, only uneven. */
static double wobbly_get_random(xd_wobbly_model *model) {
    model->seed = model->seed * 1664525u + 1013904223u;
    return (double)(model->seed >> 8) / (double)(1u << 23) - 1.0;
}

void xd_wobbly_shiver(xd_wobbly_model *model, double strength) {
    for (int i = 0; i < XD_WOBBLY_POINTS; i++) {
        model->points[i].vx += wobbly_get_random(model) * strength;
        model->points[i].vy += wobbly_get_random(model) * strength;
    }
}

void xd_wobbly_displace(xd_wobbly_model *model, double from_x, double from_y, double from_width,
                        double from_height, double share) {
    xd_wobbly_model from = *model;
    xd_wobbly_set_rest(&from, from_x, from_y, from_width, from_height);
    for (int i = 0; i < XD_WOBBLY_POINTS; i++) {
        double now_x = 0, now_y = 0, then_x = 0, then_y = 0;
        wobbly_get_rest_point(model, i, &now_x, &now_y);
        wobbly_get_rest_point(&from, i, &then_x, &then_y);
        model->points[i].x += (then_x - now_x) * share;
        model->points[i].y += (then_y - now_y) * share;
    }
}

static void wobbly_step_once(xd_wobbly_model *model) {
    double force_x[XD_WOBBLY_POINTS] = {0}, force_y[XD_WOBBLY_POINTS] = {0};
    double rest_x[XD_WOBBLY_POINTS], rest_y[XD_WOBBLY_POINTS];
    for (int i = 0; i < XD_WOBBLY_POINTS; i++)
        wobbly_get_rest_point(model, i, &rest_x[i], &rest_y[i]);
    /* Each spring wants its two masses as far apart as on the window. */
    for (int i = 0; i < XD_WOBBLY_POINTS; i++) {
        int neighbours[2] = {i % XD_WOBBLY_SIDE + 1 < XD_WOBBLY_SIDE ? i + 1 : -1,
                             i + XD_WOBBLY_SIDE < XD_WOBBLY_POINTS ? i + XD_WOBBLY_SIDE : -1};
        for (size_t k = 0; k < 2; k++) {
            int j = neighbours[k];
            if (j < 0)
                continue;
            double dx = (model->points[j].x - model->points[i].x) - (rest_x[j] - rest_x[i]);
            double dy = (model->points[j].y - model->points[i].y) - (rest_y[j] - rest_y[i]);
            force_x[i] += 0.5 * model->spring * dx;
            force_y[i] += 0.5 * model->spring * dy;
            force_x[j] -= 0.5 * model->spring * dx;
            force_y[j] -= 0.5 * model->spring * dy;
        }
    }
    for (int i = 0; i < XD_WOBBLY_POINTS; i++) {
        xd_wobbly_point *point = &model->points[i];
        if (i == model->grabbed) {
            point->x = rest_x[i];
            point->y = rest_y[i];
            point->vx = point->vy = 0;
            continue;
        }
        if (model->grabbed < 0) {
            force_x[i] += 0.5 * model->spring * ANCHOR * (rest_x[i] - point->x);
            force_y[i] += 0.5 * model->spring * ANCHOR * (rest_y[i] - point->y);
        }
        force_x[i] -= model->friction * point->vx;
        force_y[i] -= model->friction * point->vy;
        point->vx += force_x[i] / MASS;
        point->vy += force_y[i] / MASS;
        point->x += point->vx;
        point->y += point->vy;
    }
}

static bool wobbly_is_at_rest(const xd_wobbly_model *model) {
    if (model->grabbed >= 0)
        return false;
    for (int i = 0; i < XD_WOBBLY_POINTS; i++) {
        const xd_wobbly_point *point = &model->points[i];
        double rest_x = 0, rest_y = 0;
        wobbly_get_rest_point(model, i, &rest_x, &rest_y);
        if (fabs(point->vx) > REST_SPEED || fabs(point->vy) > REST_SPEED ||
            fabs(point->x - rest_x) > REST_DISTANCE || fabs(point->y - rest_y) > REST_DISTANCE)
            return false;
    }
    return true;
}

bool xd_wobbly_step(xd_wobbly_model *model, double seconds) {
    if (!(seconds > 0))
        seconds = 0;
    model->leftover = XD_MIN(model->leftover + seconds, MAX_BACKLOG_SECONDS);
    while (model->leftover >= STEP_SECONDS) {
        wobbly_step_once(model);
        model->leftover -= STEP_SECONDS;
    }
    /* A model gone wild (it can't at the table's ranges) settles at once
     * rather than drawing nonsense. */
    for (int i = 0; i < XD_WOBBLY_POINTS; i++) {
        if (!isfinite(model->points[i].x) || !isfinite(model->points[i].y) ||
            !isfinite(model->points[i].vx) || !isfinite(model->points[i].vy)) {
            model->grabbed = -1;
            for (int j = 0; j < XD_WOBBLY_POINTS; j++) {
                wobbly_get_rest_point(model, j, &model->points[j].x, &model->points[j].y);
                model->points[j].vx = model->points[j].vy = 0;
            }
            return false;
        }
    }
    if (!wobbly_is_at_rest(model))
        return true;
    for (int i = 0; i < XD_WOBBLY_POINTS; i++) {
        wobbly_get_rest_point(model, i, &model->points[i].x, &model->points[i].y);
        model->points[i].vx = model->points[i].vy = 0;
    }
    return false;
}

static void wobbly_get_bernstein(double along, double out[XD_WOBBLY_SIDE]) {
    double rest = 1.0 - along;
    out[0] = rest * rest * rest;
    out[1] = 3.0 * along * rest * rest;
    out[2] = 3.0 * along * along * rest;
    out[3] = along * along * along;
}

void xd_wobbly_evaluate(const xd_wobbly_model *model, double across, double down,
                        double *out_x, double *out_y) {
    double weights_x[XD_WOBBLY_SIDE], weights_y[XD_WOBBLY_SIDE];
    wobbly_get_bernstein(XD_CLAMP(across, 0.0, 1.0), weights_x);
    wobbly_get_bernstein(XD_CLAMP(down, 0.0, 1.0), weights_y);
    double sum_x = 0, sum_y = 0;
    for (int row = 0; row < XD_WOBBLY_SIDE; row++) {
        for (int column = 0; column < XD_WOBBLY_SIDE; column++) {
            double weight = weights_y[row] * weights_x[column];
            sum_x += weight * model->points[row * XD_WOBBLY_SIDE + column].x;
            sum_y += weight * model->points[row * XD_WOBBLY_SIDE + column].y;
        }
    }
    *out_x = sum_x;
    *out_y = sum_y;
}
