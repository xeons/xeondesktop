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

static constexpr int DIALOG_WIDTH = 360;
static constexpr int DIALOG_HEIGHT = 296;

struct monitor_dialog {
    xd_window *win;
    monitor_done done;
    void *user;
    monitor_props applied; /* as last handed back */
    xd_widget *rate, *vrr, *apply;
};

static monitor_props current(const monitor_dialog *refresh_dialog) {
    return (monitor_props){
        .rate = xd_widget_selected(refresh_dialog->rate),
        .vrr = xd_checkbox_checked(refresh_dialog->vrr),
    };
}

static void update_apply(monitor_dialog *refresh_dialog) {
    monitor_props shown = current(refresh_dialog);
    xd_widget_set_enabled(refresh_dialog->apply, shown.rate != refresh_dialog->applied.rate ||
                                                     shown.vrr != refresh_dialog->applied.vrr);
}

static void on_change([[maybe_unused]] xd_widget *widget, void *user) { update_apply(user); }

static void finish(monitor_dialog *refresh_dialog, monitor_result outcome) {
    monitor_props shown = current(refresh_dialog);
    monitor_done done = refresh_dialog->done;
    void *user = refresh_dialog->user;
    xd_window_close(refresh_dialog->win);
    xd_free(refresh_dialog);
    done(outcome, outcome == MONITOR_OK ? &shown : nullptr, user);
}

void monitor_dialog_cancel(monitor_dialog *refresh_dialog) {
    finish(refresh_dialog, MONITOR_CANCEL);
}

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) { finish(user, MONITOR_OK); }

static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) {
    finish(user, MONITOR_CANCEL);
}

static void on_apply([[maybe_unused]] xd_widget *widget, void *user) {
    monitor_dialog *refresh_dialog = user;
    refresh_dialog->applied = current(refresh_dialog);
    update_apply(refresh_dialog);
    refresh_dialog->done(MONITOR_APPLY, &refresh_dialog->applied, refresh_dialog->user);
}

monitor_dialog *monitor_dialog_open(xd_ui *ui, const char *monitor, const char *const *rates,
                                    size_t rate_count, monitor_props props, bool vrr_capable,
                                    monitor_done done, void *user) {
    monitor_dialog *refresh_dialog = XD_NEW(monitor_dialog);
    refresh_dialog->done = done;
    refresh_dialog->user = user;
    refresh_dialog->applied = props;
    char title[192];
    snprintf(title, sizeof(title), "%s Properties", monitor);
    xd_window *win = refresh_dialog->win =
        xd_window_create(ui, title, "xd-desk", DIALOG_WIDTH, DIALOG_HEIGHT);
    static const char *const tabs[] = {"Monitor"};
    xd_widget *tab = xd_tabs(win, (xd_rect){7, 7, DIALOG_WIDTH - 14, DIALOG_HEIGHT - 47}, tabs, 1,
                             nullptr, nullptr);

    xd_groupbox(win, (xd_rect){19, 38, DIALOG_WIDTH - 38, 62}, "Monitor type");
    xd_image(win, (xd_rect){31, 56, 32, 32}, "video-display");
    xd_label(win, (xd_rect){75, 64, DIALOG_WIDTH - 106, 16}, monitor, 0);

    xd_groupbox(win, (xd_rect){19, 110, DIALOG_WIDTH - 38, 132}, "Monitor settings");
    xd_label(win, (xd_rect){31, 130, 200, 16}, "Screen refresh rate:", 0);
    refresh_dialog->rate =
        xd_combo(win, (xd_rect){31, 148, 150, 21}, false, on_change, refresh_dialog);
    for (size_t i = 0; i < rate_count; i++)
        xd_widget_add_item(refresh_dialog->rate, rates[i]);
    xd_widget_select(refresh_dialog->rate, props.rate);
    xd_widget_set_enabled(refresh_dialog->rate, rate_count > 1);

    /* Off by default, as in Windows: some panels flicker with it. */
    refresh_dialog->vrr =
        xd_checkbox(win, (xd_rect){31, 180, DIALOG_WIDTH - 62, 20}, "Variable refresh rate",
                    props.vrr && vrr_capable, on_change, refresh_dialog);
    xd_widget_set_enabled(refresh_dialog->vrr, vrr_capable);
    xd_label(win, (xd_rect){49, 202, DIALOG_WIDTH - 80, 56},
             vrr_capable ? "Lets full-screen games change the monitor's refresh rate to match "
                           "their frame rate, for smooth motion without tearing. Some monitors "
                           "flicker with it."
                         : "This monitor or its graphics adapter doesn't support a variable "
                           "refresh rate.",
             XD_LABEL_WRAP | XD_LABEL_DIM);

    xd_widget *ok = xd_button(win, (xd_rect){DIALOG_WIDTH - 249, DIALOG_HEIGHT - 33, 75, 23}, "OK",
                              on_ok, refresh_dialog);
    xd_widget *cancel = xd_button(win, (xd_rect){DIALOG_WIDTH - 168, DIALOG_HEIGHT - 33, 75, 23},
                                  "Cancel", on_cancel, refresh_dialog);
    refresh_dialog->apply = xd_button(win, (xd_rect){DIALOG_WIDTH - 87, DIALOG_HEIGHT - 33, 75, 23},
                                      "Apply", on_apply, refresh_dialog);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, tab);
    update_apply(refresh_dialog);
    xd_window_show(win);
    return refresh_dialog;
}
