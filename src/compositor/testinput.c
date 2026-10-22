/* SPDX-License-Identifier: MIT
 * xd-test-input-v1 (testing only, with --allow-virtual-input): a
 * touchscreen, a drawing tablet with its pen and pad, and a touchpad's
 * gestures for the UI tests, which run headless where there are none.
 * Each is a real wlroots device, added as the backend adds libinput's, so
 * the tests go through the same code as real hardware; only the events
 * come from xd-inject.
 */
#include <linux/input-event-codes.h>
#include <time.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_tablet_pad.h>
#include <wlr/interfaces/wlr_tablet_tool.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"
#include "xd-test-input-v1-protocol.h"

typedef struct test_input {
    xd_server *server;
    bool has_touch, has_tablet, has_pointer;
    struct wlr_touch touch;
    struct wlr_tablet tablet;
    struct wlr_tablet_tool tool;
    struct wlr_tablet_pad pad;
    /* The pad's one group: four buttons, a ring and a strip. */
    struct wlr_tablet_pad_group group;
    unsigned int group_buttons[4], group_rings[1], group_strips[1];
    struct wlr_pointer pointer; /* the touchpad, for gestures */
    double tool_x, tool_y;      /* where the pen is, 0..1 of the tablet */
} test_input;

static const struct wlr_touch_impl touch_impl = {.name = "xd-test-touch"};
static const struct wlr_tablet_impl tablet_impl = {.name = "xd-test-tablet"};
static const struct wlr_tablet_pad_impl pad_impl = {.name = "xd-test-pad"};
static const struct wlr_pointer_impl pointer_impl = {.name = "xd-test-touchpad"};

static uint32_t now_msec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static test_input *from_resource(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

/* Layout coordinates as the device reports them: 0..1 of its area. */
static void normalize(test_input *ti, const struct wlr_input_device *device, wl_fixed_t x,
                      wl_fixed_t y, double *nx, double *ny) {
    struct wlr_box box = xd_input_device_area(ti->server, device);
    *nx = box.width > 0 ? (wl_fixed_to_double(x) - box.x) / box.width : 0;
    *ny = box.height > 0 ? (wl_fixed_to_double(y) - box.y) / box.height : 0;
}

/* ---- touch --------------------------------------------------------------------- */

static struct wlr_touch *touch(test_input *ti) {
    if (!ti->has_touch) {
        wlr_touch_init(&ti->touch, &touch_impl, "xd-test touchscreen");
        ti->has_touch = true;
        xd_input_add_device(ti->server, &ti->touch.base);
    }
    return &ti->touch;
}

static void touch_down(struct wl_client *client, struct wl_resource *res, int32_t id, wl_fixed_t x,
                       wl_fixed_t y) {
    (void)client;
    test_input *ti = from_resource(res);
    struct wlr_touch *test_touch = touch(ti);
    struct wlr_touch_down_event touch_down_event = {
        .touch = test_touch, .time_msec = now_msec(), .touch_id = id};
    normalize(ti, &test_touch->base, x, y, &touch_down_event.x, &touch_down_event.y);
    wl_signal_emit_mutable(&test_touch->events.down, &touch_down_event);
    wl_signal_emit_mutable(&test_touch->events.frame, nullptr);
}

static void touch_motion(struct wl_client *client, struct wl_resource *res, int32_t id,
                         wl_fixed_t x, wl_fixed_t y) {
    (void)client;
    test_input *ti = from_resource(res);
    struct wlr_touch *test_touch = touch(ti);
    struct wlr_touch_motion_event touch_motion_event = {
        .touch = test_touch, .time_msec = now_msec(), .touch_id = id};
    normalize(ti, &test_touch->base, x, y, &touch_motion_event.x, &touch_motion_event.y);
    wl_signal_emit_mutable(&test_touch->events.motion, &touch_motion_event);
    wl_signal_emit_mutable(&test_touch->events.frame, nullptr);
}

static void touch_up(struct wl_client *client, struct wl_resource *res, int32_t id) {
    (void)client;
    struct wlr_touch *test_touch = touch(from_resource(res));
    struct wlr_touch_up_event touch_up_event = {
        .touch = test_touch, .time_msec = now_msec(), .touch_id = id};
    wl_signal_emit_mutable(&test_touch->events.up, &touch_up_event);
    wl_signal_emit_mutable(&test_touch->events.frame, nullptr);
}

static void touch_cancel(struct wl_client *client, struct wl_resource *res, int32_t id) {
    (void)client;
    struct wlr_touch *test_touch = touch(from_resource(res));
    struct wlr_touch_cancel_event touch_cancel_event = {
        .touch = test_touch, .time_msec = now_msec(), .touch_id = id};
    wl_signal_emit_mutable(&test_touch->events.cancel, &touch_cancel_event);
    wl_signal_emit_mutable(&test_touch->events.frame, nullptr);
}

/* ---- the tablet, its pen and its pad ------------------------------------------------ */

static void tablet(test_input *ti) {
    if (ti->has_tablet)
        return;
    ti->has_tablet = true;
    wlr_tablet_init(&ti->tablet, &tablet_impl, "xd-test tablet");
    ti->tablet.width_mm = 216;
    ti->tablet.height_mm = 135;
    ti->tool = (struct wlr_tablet_tool){
        .type = WLR_TABLET_TOOL_TYPE_PEN,
        .hardware_serial = 1,
        .pressure = true,
        .tilt = true,
    };
    wl_signal_init(&ti->tool.events.destroy);
    xd_input_add_device(ti->server, &ti->tablet.base);

    wlr_tablet_pad_init(&ti->pad, &pad_impl, "xd-test tablet pad");
    ti->pad.button_count = XD_ARRAY_LEN(ti->group_buttons);
    ti->pad.ring_count = 1;
    ti->pad.strip_count = 1;
    for (unsigned int i = 0; i < XD_ARRAY_LEN(ti->group_buttons); i++)
        ti->group_buttons[i] = i;
    ti->group = (struct wlr_tablet_pad_group){
        .button_count = XD_ARRAY_LEN(ti->group_buttons),
        .buttons = ti->group_buttons,
        .ring_count = 1,
        .rings = ti->group_rings,
        .strip_count = 1,
        .strips = ti->group_strips,
        .mode_count = 1,
    };
    wl_list_insert(&ti->pad.groups, &ti->group.link);
    xd_input_add_device(ti->server, &ti->pad.base);
}

static void tool_proximity(struct wl_client *client, struct wl_resource *res, wl_fixed_t x,
                           wl_fixed_t y, uint32_t in) {
    (void)client;
    test_input *ti = from_resource(res);
    tablet(ti);
    normalize(ti, &ti->tablet.base, x, y, &ti->tool_x, &ti->tool_y);
    struct wlr_tablet_tool_proximity_event tablet_tool_proximity_event = {
        .tablet = &ti->tablet,
        .tool = &ti->tool,
        .time_msec = now_msec(),
        .x = ti->tool_x,
        .y = ti->tool_y,
        .state = in ? WLR_TABLET_TOOL_PROXIMITY_IN : WLR_TABLET_TOOL_PROXIMITY_OUT,
    };
    wl_signal_emit_mutable(&ti->tablet.events.proximity, &tablet_tool_proximity_event);
}

static void tool_motion(struct wl_client *client, struct wl_resource *res, wl_fixed_t x,
                        wl_fixed_t y, wl_fixed_t pressure, wl_fixed_t tilt_x, wl_fixed_t tilt_y) {
    (void)client;
    test_input *ti = from_resource(res);
    tablet(ti);
    double ox = ti->tool_x, oy = ti->tool_y;
    normalize(ti, &ti->tablet.base, x, y, &ti->tool_x, &ti->tool_y);
    struct wlr_tablet_tool_axis_event tablet_tool_axis_event = {
        .tablet = &ti->tablet,
        .tool = &ti->tool,
        .time_msec = now_msec(),
        .updated_axes = WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y |
                        WLR_TABLET_TOOL_AXIS_PRESSURE | WLR_TABLET_TOOL_AXIS_TILT_X |
                        WLR_TABLET_TOOL_AXIS_TILT_Y,
        .x = ti->tool_x,
        .y = ti->tool_y,
        .dx = (ti->tool_x - ox) * ti->tablet.width_mm,
        .dy = (ti->tool_y - oy) * ti->tablet.height_mm,
        .pressure = wl_fixed_to_double(pressure),
        .tilt_x = wl_fixed_to_double(tilt_x),
        .tilt_y = wl_fixed_to_double(tilt_y),
    };
    wl_signal_emit_mutable(&ti->tablet.events.axis, &tablet_tool_axis_event);
}

static void tool_tip(struct wl_client *client, struct wl_resource *res, uint32_t down) {
    (void)client;
    test_input *ti = from_resource(res);
    tablet(ti);
    struct wlr_tablet_tool_tip_event tablet_tool_tip_event = {
        .tablet = &ti->tablet,
        .tool = &ti->tool,
        .time_msec = now_msec(),
        .x = ti->tool_x,
        .y = ti->tool_y,
        .state = down ? WLR_TABLET_TOOL_TIP_DOWN : WLR_TABLET_TOOL_TIP_UP,
    };
    wl_signal_emit_mutable(&ti->tablet.events.tip, &tablet_tool_tip_event);
}

static void tool_button(struct wl_client *client, struct wl_resource *res, uint32_t button,
                        uint32_t pressed) {
    (void)client;
    test_input *ti = from_resource(res);
    tablet(ti);
    struct wlr_tablet_tool_button_event tablet_tool_button_event = {
        .tablet = &ti->tablet,
        .tool = &ti->tool,
        .time_msec = now_msec(),
        .button = button,
        .state = pressed ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED,
    };
    wl_signal_emit_mutable(&ti->tablet.events.button, &tablet_tool_button_event);
}

static void pad_button(struct wl_client *client, struct wl_resource *res, uint32_t button,
                       uint32_t pressed) {
    (void)client;
    test_input *ti = from_resource(res);
    tablet(ti);
    struct wlr_tablet_pad_button_event tablet_pad_button_event = {
        .time_msec = now_msec(),
        .button = button,
        .state = pressed ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED,
    };
    wl_signal_emit_mutable(&ti->pad.events.button, &tablet_pad_button_event);
}

static void pad_ring(struct wl_client *client, struct wl_resource *res, uint32_t ring,
                     wl_fixed_t position) {
    (void)client;
    test_input *ti = from_resource(res);
    tablet(ti);
    struct wlr_tablet_pad_ring_event tablet_pad_ring_event = {
        .time_msec = now_msec(),
        .source = WLR_TABLET_PAD_RING_SOURCE_FINGER,
        .ring = ring,
        .position = wl_fixed_to_double(position),
    };
    wl_signal_emit_mutable(&ti->pad.events.ring, &tablet_pad_ring_event);
}

static void pad_strip(struct wl_client *client, struct wl_resource *res, uint32_t strip,
                      wl_fixed_t position) {
    (void)client;
    test_input *ti = from_resource(res);
    tablet(ti);
    struct wlr_tablet_pad_strip_event tablet_pad_strip_event = {
        .time_msec = now_msec(),
        .source = WLR_TABLET_PAD_STRIP_SOURCE_FINGER,
        .strip = strip,
        .position = wl_fixed_to_double(position),
    };
    wl_signal_emit_mutable(&ti->pad.events.strip, &tablet_pad_strip_event);
}

/* ---- the touchpad's gestures ----------------------------------------------------------- */

static struct wlr_pointer *touchpad(test_input *ti) {
    if (!ti->has_pointer) {
        wlr_pointer_init(&ti->pointer, &pointer_impl, "xd-test touchpad");
        ti->has_pointer = true;
        xd_input_add_device(ti->server, &ti->pointer.base);
    }
    return &ti->pointer;
}

static void swipe_begin(struct wl_client *client, struct wl_resource *res, uint32_t fingers) {
    (void)client;
    struct wlr_pointer *pointer = touchpad(from_resource(res));
    struct wlr_pointer_swipe_begin_event pointer_swipe_begin_event = {
        .pointer = pointer, .time_msec = now_msec(), .fingers = fingers};
    wl_signal_emit_mutable(&pointer->events.swipe_begin, &pointer_swipe_begin_event);
}

static void swipe_update(struct wl_client *client, struct wl_resource *res, wl_fixed_t dx,
                         wl_fixed_t dy) {
    (void)client;
    struct wlr_pointer *pointer = touchpad(from_resource(res));
    struct wlr_pointer_swipe_update_event pointer_swipe_update_event = {
        .pointer = pointer,
        .time_msec = now_msec(),
        .dx = wl_fixed_to_double(dx),
        .dy = wl_fixed_to_double(dy)};
    wl_signal_emit_mutable(&pointer->events.swipe_update, &pointer_swipe_update_event);
}

static void swipe_end(struct wl_client *client, struct wl_resource *res, uint32_t cancelled) {
    (void)client;
    struct wlr_pointer *pointer = touchpad(from_resource(res));
    struct wlr_pointer_swipe_end_event pointer_swipe_end_event = {
        .pointer = pointer, .time_msec = now_msec(), .cancelled = cancelled != 0};
    wl_signal_emit_mutable(&pointer->events.swipe_end, &pointer_swipe_end_event);
}

static void pinch_begin(struct wl_client *client, struct wl_resource *res, uint32_t fingers) {
    (void)client;
    struct wlr_pointer *pointer = touchpad(from_resource(res));
    struct wlr_pointer_pinch_begin_event pointer_pinch_begin_event = {
        .pointer = pointer, .time_msec = now_msec(), .fingers = fingers};
    wl_signal_emit_mutable(&pointer->events.pinch_begin, &pointer_pinch_begin_event);
}

static void pinch_update(struct wl_client *client, struct wl_resource *res, wl_fixed_t dx,
                         wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation) {
    (void)client;
    struct wlr_pointer *pointer = touchpad(from_resource(res));
    struct wlr_pointer_pinch_update_event pointer_pinch_update_event = {
        .pointer = pointer,
        .time_msec = now_msec(),
        .dx = wl_fixed_to_double(dx),
        .dy = wl_fixed_to_double(dy),
        .scale = wl_fixed_to_double(scale),
        .rotation = wl_fixed_to_double(rotation),
    };
    wl_signal_emit_mutable(&pointer->events.pinch_update, &pointer_pinch_update_event);
}

static void pinch_end(struct wl_client *client, struct wl_resource *res, uint32_t cancelled) {
    (void)client;
    struct wlr_pointer *pointer = touchpad(from_resource(res));
    struct wlr_pointer_pinch_end_event pointer_pinch_end_event = {
        .pointer = pointer, .time_msec = now_msec(), .cancelled = cancelled != 0};
    wl_signal_emit_mutable(&pointer->events.pinch_end, &pointer_pinch_end_event);
}

static void hold_begin(struct wl_client *client, struct wl_resource *res, uint32_t fingers) {
    (void)client;
    struct wlr_pointer *pointer = touchpad(from_resource(res));
    struct wlr_pointer_hold_begin_event pointer_hold_begin_event = {
        .pointer = pointer, .time_msec = now_msec(), .fingers = fingers};
    wl_signal_emit_mutable(&pointer->events.hold_begin, &pointer_hold_begin_event);
}

static void hold_end(struct wl_client *client, struct wl_resource *res, uint32_t cancelled) {
    (void)client;
    struct wlr_pointer *pointer = touchpad(from_resource(res));
    struct wlr_pointer_hold_end_event pointer_hold_end_event = {
        .pointer = pointer, .time_msec = now_msec(), .cancelled = cancelled != 0};
    wl_signal_emit_mutable(&pointer->events.hold_end, &pointer_hold_end_event);
}

static void destroy_request([[maybe_unused]] struct wl_client *client, struct wl_resource *res) {
    wl_resource_destroy(res);
}

static const struct xd_test_input_v1_interface test_input_impl = {
    .destroy = destroy_request,
    .touch_down = touch_down,
    .touch_motion = touch_motion,
    .touch_up = touch_up,
    .touch_cancel = touch_cancel,
    .tool_proximity = tool_proximity,
    .tool_motion = tool_motion,
    .tool_tip = tool_tip,
    .tool_button = tool_button,
    .pad_button = pad_button,
    .pad_ring = pad_ring,
    .pad_strip = pad_strip,
    .swipe_begin = swipe_begin,
    .swipe_update = swipe_update,
    .swipe_end = swipe_end,
    .pinch_begin = pinch_begin,
    .pinch_update = pinch_update,
    .pinch_end = pinch_end,
    .hold_begin = hold_begin,
    .hold_end = hold_end,
};

/* The client is done: its devices are unplugged. */
static void handle_resource_destroy(struct wl_resource *res) {
    test_input *ti = from_resource(res);
    if (ti->has_touch)
        wlr_touch_finish(&ti->touch);
    if (ti->has_tablet) {
        wl_list_remove(&ti->group.link); /* ours, not the pad's to free */
        wlr_tablet_pad_finish(&ti->pad);
        wlr_tablet_finish(&ti->tablet);
        /* The pen goes with the tablet, as libinput's do. */
        wl_signal_emit_mutable(&ti->tool.events.destroy, &ti->tool);
    }
    if (ti->has_pointer)
        wlr_pointer_finish(&ti->pointer);
    xd_free(ti);
}

static void bind_test_input(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *res =
        wl_resource_create(client, &xd_test_input_v1_interface, (int)version, id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    test_input *ti = XD_NEW(test_input);
    ti->server = data;
    wl_resource_set_implementation(res, &test_input_impl, ti, handle_resource_destroy);
    /* Plugged in at once, so programs can take the seat's new touch
     * capability (and the tablet) before the first touch comes. */
    touch(ti);
    tablet(ti);
    touchpad(ti);
}

void xd_test_input_init(xd_server *server) {
    wl_global_create(server->display, &xd_test_input_v1_interface, 1, server, bind_test_input);
    wlr_log(WLR_INFO, "test touchscreen, tablet and touchpad available (testing only)");
}
