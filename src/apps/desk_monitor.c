/* SPDX-License-Identifier: MIT
 * Display Properties > Settings > Advanced. XP's Advanced opened the
 * properties of the monitor and its graphics adapter (General, Adapter,
 * Monitor, Troubleshoot and Color Management); only its Monitor tab has
 * something here: the screen refresh rate, as XP had it there, and the
 * variable refresh rate, which Windows 10 and 11 put in Graphics settings.
 * It edits a copy of the monitor's settings; OK and Apply hand them back,
 * and Display Properties applies them (with its "keep these settings?").
 */
#include <stdio.h>

#include "../core/alloc.h"
#include "desk_monitor.h"

static constexpr int W = 360;
static constexpr int H = 296;

struct monitor_dialog {
    xd_window *win;
    monitor_done done;
    void *user;
    monitor_props applied; /* as last handed back */
    xd_widget *rate, *vrr, *apply;
};

static monitor_props current(const monitor_dialog *m) {
    return (monitor_props){
        .rate = xd_widget_selected(m->rate),
        .vrr = xd_checkbox_checked(m->vrr),
    };
}

static void update_apply(monitor_dialog *m) {
    monitor_props p = current(m);
    xd_widget_set_enabled(m->apply, p.rate != m->applied.rate || p.vrr != m->applied.vrr);
}

static void on_change([[maybe_unused]] xd_widget *w, void *user) { update_apply(user); }

static void finish(monitor_dialog *m, monitor_result r) {
    monitor_props p = current(m);
    monitor_done done = m->done;
    void *user = m->user;
    xd_window_close(m->win);
    xd_free(m);
    done(r, r == MONITOR_OK ? &p : nullptr, user);
}

void monitor_dialog_cancel(monitor_dialog *m) { finish(m, MONITOR_CANCEL); }

static void on_ok([[maybe_unused]] xd_widget *w, void *user) { finish(user, MONITOR_OK); }

static void on_cancel([[maybe_unused]] xd_widget *w, void *user) {
    finish(user, MONITOR_CANCEL);
}

static void on_apply([[maybe_unused]] xd_widget *w, void *user) {
    monitor_dialog *m = user;
    m->applied = current(m);
    update_apply(m);
    m->done(MONITOR_APPLY, &m->applied, m->user);
}

monitor_dialog *monitor_dialog_open(xd_ui *ui, const char *monitor, const char *const *rates,
                                    size_t rate_count, monitor_props props, bool vrr_capable,
                                    monitor_done done, void *user) {
    monitor_dialog *m = XD_NEW(monitor_dialog);
    m->done = done;
    m->user = user;
    m->applied = props;
    char title[192];
    snprintf(title, sizeof(title), "%s Properties", monitor);
    xd_window *win = m->win = xd_window_create(ui, title, "xd-desk", W, H);
    static const char *const tabs[] = {"Monitor"};
    xd_widget *tab = xd_tabs(win, (xd_rect){7, 7, W - 14, H - 47}, tabs, 1, nullptr, nullptr);

    xd_groupbox(win, (xd_rect){19, 38, W - 38, 62}, "Monitor type");
    xd_image(win, (xd_rect){31, 56, 32, 32}, "video-display");
    xd_label(win, (xd_rect){75, 64, W - 106, 16}, monitor, 0);

    xd_groupbox(win, (xd_rect){19, 110, W - 38, 132}, "Monitor settings");
    xd_label(win, (xd_rect){31, 130, 200, 16}, "Screen refresh rate:", 0);
    m->rate = xd_combo(win, (xd_rect){31, 148, 150, 21}, false, on_change, m);
    for (size_t i = 0; i < rate_count; i++)
        xd_widget_add_item(m->rate, rates[i]);
    xd_widget_select(m->rate, props.rate);
    xd_widget_set_enabled(m->rate, rate_count > 1);

    /* Off by default, as in Windows: some panels flicker with it. */
    m->vrr = xd_checkbox(win, (xd_rect){31, 180, W - 62, 20}, "Variable refresh rate",
                         props.vrr && vrr_capable, on_change, m);
    xd_widget_set_enabled(m->vrr, vrr_capable);
    xd_label(win, (xd_rect){49, 202, W - 80, 56},
             vrr_capable ? "Lets full-screen games change the monitor's refresh rate to match "
                           "their frame rate, for smooth motion without tearing. Some monitors "
                           "flicker with it."
                         : "This monitor or its graphics adapter doesn't support a variable "
                           "refresh rate.",
             XD_LABEL_WRAP | XD_LABEL_DIM);

    xd_widget *ok = xd_button(win, (xd_rect){W - 249, H - 33, 75, 23}, "OK", on_ok, m);
    xd_widget *cancel =
        xd_button(win, (xd_rect){W - 168, H - 33, 75, 23}, "Cancel", on_cancel, m);
    m->apply = xd_button(win, (xd_rect){W - 87, H - 33, 75, 23}, "Apply", on_apply, m);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, tab);
    update_apply(m);
    xd_window_show(win);
    return m;
}
