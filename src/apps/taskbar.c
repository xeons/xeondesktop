/* SPDX-License-Identifier: MIT
 * xd-taskbar: Taskbar and Start Menu Properties.
 */
#include <stddef.h>
#include <stdlib.h>

#include "../settings/settings.h"
#include "../ui/ui.h"

typedef struct props {
    xd_ui *ui;
    xd_window *win;
    xd_settings saved, pending;
    xd_widget *apply;
    xd_widget *description;
} props;

static const char *const descriptions[XD_START_STYLE_COUNT] = {
    [XD_START_CLASSIC] = "Cascading menus with the side banner, as in Windows 2000. "
                         "Programs opens a submenu for each folder.",
    [XD_START_XP] = "Two columns under your name and picture: pinned and most-used programs on "
                    "the left, your folders and Control Panel on the right, Log Off and Turn "
                    "Off Computer at the bottom. All Programs cascades.",
    [XD_START_WIN7] = "Two columns: your pinned and most-used programs with a search box on the "
                      "left, your folders, Control Panel and Shut Down on the right. All "
                      "Programs opens in place.",
};

static void changed(props *p) {
    xd_widget_set_enabled(p->apply, !xd_settings_equal(&p->saved, &p->pending));
}

static void on_quick_launch(xd_widget *w, void *user) {
    props *p = user;
    p->pending.quick_launch = xd_checkbox_checked(w);
    changed(p);
}

static void on_lock(xd_widget *w, void *user) {
    props *p = user;
    p->pending.locked = xd_checkbox_checked(w);
    changed(p);
}
static void on_auto_hide(xd_widget *w, void *user) {
    props *p = user;
    p->pending.auto_hide = xd_checkbox_checked(w);
    changed(p);
}
static void on_top(xd_widget *w, void *user) {
    props *p = user;
    p->pending.on_top = xd_checkbox_checked(w);
    changed(p);
}
static void on_group(xd_widget *w, void *user) {
    props *p = user;
    p->pending.group = xd_checkbox_checked(w);
    changed(p);
}
static void on_clock(xd_widget *w, void *user) {
    props *p = user;
    p->pending.show_clock = xd_checkbox_checked(w);
    changed(p);
}
static void on_hide_inactive(xd_widget *w, void *user) {
    props *p = user;
    p->pending.hide_inactive = xd_checkbox_checked(w);
    changed(p);
}
static void on_all_displays(xd_widget *w, void *user) {
    props *p = user;
    p->pending.all_displays = xd_checkbox_checked(w);
    changed(p);
}

static void on_style(xd_widget *w, void *user) {
    props *p = user;
    int i = xd_widget_selected(w);
    if (i >= 0 && i < XD_START_STYLE_COUNT) {
        p->pending.start_style = (xd_start_style)i;
        xd_widget_set_text(p->description, descriptions[i]);
    }
    changed(p);
}

static bool apply(props *p) {
    if (!xd_settings_save(&p->pending)) {
        fprintf(stderr, "xd-taskbar: cannot write settings\n");
        return false;
    }
    p->saved = p->pending;
    changed(p);
    return true;
}

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    if (xd_settings_equal(&p->saved, &p->pending) || apply(p))
        xd_window_close(p->win);
}
static void on_cancel([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    xd_window_close(p->win);
}
static void on_apply([[maybe_unused]] xd_widget *w, void *user) { apply(user); }
static void on_page([[maybe_unused]] xd_widget *w, [[maybe_unused]] void *user) {}

int main(int argc, char **argv) {
    static props p;
    p.ui = xd_ui_connect(argc, argv);
    if (p.ui == nullptr)
        return EXIT_FAILURE;
    xd_settings_load(&p.saved);
    p.pending = p.saved;
    int page = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && strcmp(argv[i + 1], "1") == 0)
            page = 1;
    }

    p.win = xd_window_create(p.ui, "Taskbar and Start Menu Properties", "xd-taskbar", 380, 340);
    static const char *const tabs[] = {"Taskbar", "Start Menu"};
    xd_widget *tab = xd_tabs(p.win, (xd_rect){7, 7, 366, 294}, tabs, 2, on_page, &p);

    /* Taskbar */
    /* XP's Taskbar tab, plus Windows 7's "all displays". */
    xd_widget_set_page(xd_groupbox(p.win, (xd_rect){20, 40, 340, 172}, "Taskbar appearance"), 0);
    static const struct {
        const char *label;
        size_t offset;
        xd_widget_cb cb;
    } boxes[] = {
        {"Lock the taskbar", offsetof(xd_settings, locked), on_lock},
        {"Auto-hide the taskbar", offsetof(xd_settings, auto_hide), on_auto_hide},
        {"Keep the taskbar on top of other windows", offsetof(xd_settings, on_top), on_top},
        {"Group similar taskbar buttons", offsetof(xd_settings, group), on_group},
        {"Show Quick Launch", offsetof(xd_settings, quick_launch), on_quick_launch},
        {"Show the taskbar on all displays", offsetof(xd_settings, all_displays),
         on_all_displays},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(boxes); i++) {
        const bool *value = (const bool *)((const char *)&p.pending + boxes[i].offset);
        xd_widget *box = xd_checkbox(p.win, (xd_rect){32, 60 + 24 * (int)i, 320, 20},
                                     boxes[i].label, *value, boxes[i].cb, &p);
        xd_widget_set_page(box, 0);
    }
    xd_widget_set_page(xd_groupbox(p.win, (xd_rect){20, 218, 340, 70}, "Notification area"), 0);
    xd_widget_set_page(xd_checkbox(p.win, (xd_rect){32, 238, 300, 20}, "Show the clock",
                                   p.pending.show_clock, on_clock, &p),
                       0);
    xd_widget_set_page(xd_checkbox(p.win, (xd_rect){32, 262, 300, 20}, "Hide inactive icons",
                                   p.pending.hide_inactive, on_hide_inactive, &p),
                       0);

    /* Start Menu */
    xd_widget_set_page(xd_label(p.win, (xd_rect){20, 44, 340, 16}, "Start menu style:", 0), 1);
    xd_widget *style = xd_combo(p.win, (xd_rect){20, 62, 220, 21}, false, on_style, &p);
    xd_widget_set_page(style, 1);
    xd_widget_add_item(style, "Classic (Windows 2000)");
    xd_widget_add_item(style, "Windows XP");
    xd_widget_add_item(style, "Windows 7");
    p.description = xd_label(p.win, (xd_rect){20, 96, 340, 64},
                             descriptions[p.pending.start_style], XD_LABEL_WRAP);
    xd_widget_set_page(p.description, 1);
    xd_widget_select(style, (int)p.pending.start_style);
    xd_widget_select(tab, page);

    xd_widget *ok = xd_button(p.win, (xd_rect){136, 309, 75, 23}, "OK", on_ok, &p);
    xd_widget *cancel = xd_button(p.win, (xd_rect){217, 309, 75, 23}, "Cancel", on_cancel, &p);
    p.apply = xd_button(p.win, (xd_rect){298, 309, 75, 23}, "Apply", on_apply, &p);
    xd_window_set_default(p.win, ok);
    xd_window_set_cancel(p.win, cancel);
    xd_window_set_focus(p.win, tab);
    changed(&p);
    xd_window_show(p.win);
    xd_ui_run(p.ui);
    xd_ui_destroy(p.ui);
    return EXIT_SUCCESS;
}
