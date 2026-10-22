/* SPDX-License-Identifier: MIT
 * xd-taskbar: Taskbar and Start Menu Properties.
 */
#include <stddef.h>
#include <stdlib.h>

#include <string.h>

#include "../core/alloc.h"
#include "../settings/settings.h"
#include "../settings/state.h"
#include "../ui/ui.h"

/* Customize Notifications: the icons the shell has seen and the rule
 * being edited for each (applied to `pending` on OK). */
typedef struct customize {
    xd_window *win;
    xd_widget *list, *behavior;
    char **lines; /* "id<TAB>title" */
    size_t count;
    xd_tray_rule *rules;
} customize;

/* XP's Customize Start Menu: what it changes goes to `pending` on OK. */
typedef struct start_custom {
    xd_window *win;
    xd_widget *large, *programs, *highlight;
    bool clear; /* Clear List was pressed */
} start_custom;

typedef struct props {
    xd_ui *ui;
    xd_window *win;
    xd_visual_style style;
    xd_settings saved, pending;
    xd_widget *apply;
    /* The unified taskbar has 7's "Taskbar buttons" where XP's grouping
     * box is; "Use small icons" follows it while left automatic. */
    xd_widget *group, *buttons_label, *buttons, *small_icons;
    xd_widget *description;
    xd_widget *start_customize;
    customize *cust;
    start_custom *start_cust;
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

static void changed(props *properties) {
    xd_widget_set_enabled(properties->apply,
                          !xd_settings_equal(&properties->saved, &properties->pending));
}

static void on_quick_launch(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.quick_launch = xd_checkbox_checked(widget);
    changed(properties);
}

static void on_lock(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.locked = xd_checkbox_checked(widget);
    changed(properties);
}
static void on_auto_hide(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.auto_hide = xd_checkbox_checked(widget);
    changed(properties);
}
static void on_top(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.on_top = xd_checkbox_checked(widget);
    changed(properties);
}
static void on_group(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.group = xd_checkbox_checked(widget);
    changed(properties);
}
static void show_buttons_choice(props *properties) {
    bool unified = xd_settings_taskbar_unified(&properties->pending, properties->style);
    xd_widget_set_visible(properties->group, !unified);
    xd_widget_set_visible(properties->buttons_label, unified);
    xd_widget_set_visible(properties->buttons, unified);
}
static void on_unified(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.layout =
        xd_checkbox_checked(widget) ? XD_TASKBAR_LAYOUT_UNIFIED : XD_TASKBAR_LAYOUT_TASKS;
    show_buttons_choice(properties);
    xd_checkbox_set(properties->small_icons,
                    !xd_settings_taskbar_large_icons(&properties->pending, properties->style));
    changed(properties);
}
static void on_small_icons(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.icons =
        xd_checkbox_checked(widget) ? XD_TASKBAR_ICONS_SMALL : XD_TASKBAR_ICONS_LARGE;
    changed(properties);
}
static void on_buttons(xd_widget *widget, void *user) {
    props *properties = user;
    int i = xd_widget_selected(widget);
    if (i >= 0 && i < XD_BUTTONS_COUNT)
        properties->pending.buttons = (xd_taskbar_buttons)i;
    changed(properties);
}
static void on_clock(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.show_clock = xd_checkbox_checked(widget);
    changed(properties);
}
static void on_hide_inactive(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.hide_inactive = xd_checkbox_checked(widget);
    changed(properties);
}
static void on_all_displays(xd_widget *widget, void *user) {
    props *properties = user;
    properties->pending.all_displays = xd_checkbox_checked(widget);
    changed(properties);
}

static void on_style(xd_widget *widget, void *user) {
    props *properties = user;
    int i = xd_widget_selected(widget);
    if (i >= 0 && i < XD_START_STYLE_COUNT) {
        properties->pending.start_style = (xd_start_style)i;
        xd_widget_set_text(properties->description, descriptions[i]);
        /* Only the XP menu's options are here so far. */
        if (properties->start_customize != nullptr)
            xd_widget_set_enabled(properties->start_customize, i == XD_START_XP);
    }
    changed(properties);
}

static bool apply(props *properties) {
    /* Only what changed here: the taskbar's own menu, or another dialog,
     * may have saved since. */
    xd_settings now;
    xd_settings_load(&now);
    xd_settings_merge(&properties->saved, &properties->pending, &now);
    if (!xd_settings_save(&now)) {
        fprintf(stderr, "xd-taskbar: cannot write settings\n");
        return false;
    }
    properties->saved = properties->pending = now;
    changed(properties);
    return true;
}

/* ---- Customize Notifications ----------------------------------------------- */

static const char *const rule_names[] = {
    [XD_TRAY_AUTO] = "Hide when inactive",
    [XD_TRAY_ALWAYS_HIDE] = "Always hide",
    [XD_TRAY_ALWAYS_SHOW] = "Always show",
};

static const char *line_id(const char *line, char *buf, size_t size) {
    const char *tab = strchr(line, '\t');
    size_t count = tab != nullptr ? (size_t)(tab - line) : strlen(line);
    count = XD_MIN(count, size - 1);
    memcpy(buf, line, count);
    buf[count] = '\0';
    return buf;
}

static const char *line_title(const char *line) {
    const char *tab = strchr(line, '\t');
    return tab != nullptr && tab[1] != '\0' ? tab + 1 : line;
}

/* "Title    Behavior", as XP's two columns. */
static void fill_list(customize *notifications) {
    int sel = xd_widget_selected(notifications->list);
    xd_widget_clear_items(notifications->list);
    for (size_t i = 0; i < notifications->count; i++) {
        char *row = xd_str_dup_cstr(xd_str_from_cstr(line_title(notifications->lines[i])));
        xd_strbuf sb = {0};
        xd_strbuf_appendf(&sb, "%s  —  %s", row, rule_names[notifications->rules[i]]);
        xd_widget_add_item(notifications->list, xd_strbuf_cstr(&sb));
        xd_strbuf_free(&sb);
        xd_free(row);
    }
    if (sel >= 0)
        xd_widget_select(notifications->list, sel);
}

static void on_cust_select(xd_widget *widget, void *user) {
    customize *notifications = ((props *)user)->cust;
    int i = xd_widget_selected(widget);
    xd_widget_set_enabled(notifications->behavior, i >= 0);
    if (i >= 0 && (size_t)i < notifications->count)
        xd_combo_select(notifications->behavior, (int)notifications->rules[i]);
}

static void on_cust_behavior(xd_widget *widget, void *user) {
    customize *notifications = ((props *)user)->cust;
    int i = xd_widget_selected(notifications->list);
    int rule = xd_combo_selected(widget);
    if (i < 0 || (size_t)i >= notifications->count || rule < 0 || rule > XD_TRAY_ALWAYS_SHOW ||
        notifications->rules[i] == (xd_tray_rule)rule)
        return;
    notifications->rules[i] = (xd_tray_rule)rule;
    fill_list(notifications);
}

static void close_customize(props *properties) {
    customize *notifications = properties->cust;
    if (notifications == nullptr)
        return;
    properties->cust = nullptr;
    xd_window_close(notifications->win);
    xd_free_lines(notifications->lines, notifications->count);
    xd_free(notifications->rules);
    xd_free(notifications);
}

static void on_cust_ok([[maybe_unused]] xd_widget *widget, void *user) {
    props *properties = user;
    customize *notifications = properties->cust;
    for (size_t i = 0; i < notifications->count; i++) {
        char id[64];
        xd_settings_set_tray_rule(&properties->pending,
                                  line_id(notifications->lines[i], id, sizeof(id)),
                                  notifications->rules[i]);
    }
    changed(properties);
    close_customize(properties);
}

static void on_cust_cancel([[maybe_unused]] xd_widget *widget, void *user) {
    close_customize(user);
}

static void on_cust_defaults([[maybe_unused]] xd_widget *widget, void *user) {
    customize *notifications = ((props *)user)->cust;
    for (size_t i = 0; i < notifications->count; i++)
        notifications->rules[i] = XD_TRAY_AUTO;
    fill_list(notifications);
    on_cust_select(notifications->list, user);
}

static void on_customize([[maybe_unused]] xd_widget *widget, void *user) {
    props *properties = user;
    if (properties->cust != nullptr)
        return;
    customize *notifications = XD_NEW(customize);
    properties->cust = notifications;
    xd_state_read_lines("tray-icons", &notifications->lines, &notifications->count,
                        XD_TRAY_RULES_MAX * 2);
    notifications->rules =
        xd_alloc_array(XD_MAX(notifications->count, 1), sizeof(*notifications->rules));
    for (size_t i = 0; i < notifications->count; i++) {
        char id[64];
        notifications->rules[i] = xd_settings_tray_rule(
            &properties->pending, line_id(notifications->lines[i], id, sizeof(id)));
    }
    notifications->win =
        xd_window_create(properties->ui, "Customize Notifications", "xd-taskbar", 380, 316);
    xd_label(notifications->win, (xd_rect){12, 12, 356, 44},
             "XeonDesktop shows icons for active and urgent notifications, and hides "
             "inactive ones. You can change this for items in the list below.",
             XD_LABEL_WRAP);
    xd_label(notifications->win, (xd_rect){12, 62, 356, 16},
             "Select an item, then choose its notification behavior:", 0);
    notifications->list =
        xd_listbox(notifications->win, (xd_rect){12, 82, 356, 150}, on_cust_select, properties);
    xd_label(notifications->win, (xd_rect){12, 245, 70, 16}, "Behavior:", 0);
    notifications->behavior = xd_combo(notifications->win, (xd_rect){84, 242, 180, 21}, false,
                                       on_cust_behavior, properties);
    for (size_t rule_index = 0; rule_index < XD_ARRAY_LEN(rule_names); rule_index++)
        xd_combo_add(notifications->behavior, rule_names[rule_index]);
    xd_widget_set_enabled(notifications->behavior, false);
    fill_list(notifications);
    xd_button(notifications->win, (xd_rect){12, 280, 110, 23}, "Restore Defaults", on_cust_defaults,
              properties);
    xd_widget *ok =
        xd_button(notifications->win, (xd_rect){212, 280, 75, 23}, "OK", on_cust_ok, properties);
    xd_widget *cancel = xd_button(notifications->win, (xd_rect){293, 280, 75, 23}, "Cancel",
                                  on_cust_cancel, properties);
    xd_window_set_default(notifications->win, ok);
    xd_window_set_cancel(notifications->win, cancel);
    xd_window_set_focus(notifications->win, notifications->list);
    xd_window_show(notifications->win);
}

/* ---- Customize Start Menu (XP) ----------------------------------------------- */

static void close_start_custom(props *properties) {
    start_custom *start_options = properties->start_cust;
    if (start_options == nullptr)
        return;
    properties->start_cust = nullptr;
    xd_window_close(start_options->win);
    xd_free(start_options);
}

static void on_start_clear([[maybe_unused]] xd_widget *widget, void *user) {
    props *properties = user;
    if (properties->start_cust != nullptr) {
        properties->start_cust->clear = true;
        xd_widget_set_enabled(widget, false);
    }
}

static void on_start_custom_ok([[maybe_unused]] xd_widget *widget, void *user) {
    props *properties = user;
    start_custom *start_options = properties->start_cust;
    properties->pending.start_large_icons = xd_checkbox_checked(start_options->large);
    int count = xd_widget_selected(start_options->programs);
    if (count >= 0 && count <= XD_START_PROGRAMS_MAX)
        properties->pending.start_programs = count;
    properties->pending.start_highlight_new = xd_checkbox_checked(start_options->highlight);
    /* Clear List: the most-used programs are forgotten (not the programs). */
    if (start_options->clear && !xd_state_write_lines("start-frequent", nullptr, 0))
        fprintf(stderr, "xd-taskbar: cannot clear the Start menu's list\n");
    changed(properties);
    close_start_custom(properties);
}

static void on_start_custom_cancel([[maybe_unused]] xd_widget *widget, void *user) {
    close_start_custom(user);
}

static void on_start_customize([[maybe_unused]] xd_widget *widget, void *user) {
    props *properties = user;
    if (properties->start_cust != nullptr)
        return;
    start_custom *start_options = XD_NEW(start_custom);
    properties->start_cust = start_options;
    start_options->win =
        xd_window_create(properties->ui, "Customize Start Menu", "xd-taskbar", 380, 356);
    static const char *const tabs[] = {"General", "Advanced"};
    xd_widget *tab =
        xd_tabs(start_options->win, (xd_rect){7, 7, 366, 310}, tabs, 2, nullptr, properties);

    /* General */
    xd_widget_set_page(xd_groupbox(start_options->win, (xd_rect){20, 40, 340, 84},
                                   "Select an icon size for programs"),
                       0);
    xd_widget_set_page(
        xd_image(start_options->win, (xd_rect){40, 62, 32, 32}, "application-x-executable"), 0);
    start_options->large = xd_radio(start_options->win, (xd_rect){80, 68, 100, 20}, "Large icons",
                                    true, nullptr, properties);
    xd_widget_set_page(start_options->large, 0);
    xd_widget_set_page(
        xd_image(start_options->win, (xd_rect){200, 70, 16, 16}, "application-x-executable"), 0);
    xd_widget *small = xd_radio(start_options->win, (xd_rect){224, 68, 100, 20}, "Small icons",
                                false, nullptr, properties);
    xd_widget_set_page(small, 0);
    xd_radio_set(properties->pending.start_large_icons ? start_options->large : small);

    xd_widget_set_page(xd_groupbox(start_options->win, (xd_rect){20, 134, 340, 132}, "Programs"),
                       0);
    xd_widget_set_page(xd_label(start_options->win, (xd_rect){32, 154, 316, 44},
                                "The Start menu contains shortcuts to the programs you use "
                                "most often. Clearing the list of shortcuts does not delete "
                                "the programs.",
                                XD_LABEL_WRAP),
                       0);
    xd_widget_set_page(xd_label(start_options->win, (xd_rect){32, 207, 200, 16},
                                "Number of programs on Start menu:", 0),
                       0);
    start_options->programs =
        xd_combo(start_options->win, (xd_rect){236, 204, 60, 21}, false, nullptr, properties);
    xd_widget_set_page(start_options->programs, 0);
    for (int i = 0; i <= XD_START_PROGRAMS_MAX; i++) {
        char num[8];
        snprintf(num, sizeof(num), "%d", i);
        xd_widget_add_item(start_options->programs, num);
    }
    xd_widget_select(start_options->programs, properties->pending.start_programs);
    xd_widget_set_page(xd_button(start_options->win, (xd_rect){264, 234, 84, 23}, "Clear List",
                                 on_start_clear, properties),
                       0);

    /* Advanced */
    xd_widget_set_page(
        xd_groupbox(start_options->win, (xd_rect){20, 40, 340, 56}, "Start menu settings"), 1);
    start_options->highlight = xd_checkbox(
        start_options->win, (xd_rect){32, 62, 316, 20}, "Highlight newly installed programs",
        properties->pending.start_highlight_new, nullptr, properties);
    xd_widget_set_page(start_options->highlight, 1);

    xd_widget *ok = xd_button(start_options->win, (xd_rect){217, 325, 75, 23}, "OK",
                              on_start_custom_ok, properties);
    xd_widget *cancel = xd_button(start_options->win, (xd_rect){298, 325, 75, 23}, "Cancel",
                                  on_start_custom_cancel, properties);
    xd_window_set_default(start_options->win, ok);
    xd_window_set_cancel(start_options->win, cancel);
    xd_window_set_focus(start_options->win, tab);
    xd_window_show(start_options->win);
}

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) {
    props *properties = user;
    close_customize(properties);
    close_start_custom(properties);
    if (xd_settings_equal(&properties->saved, &properties->pending) || apply(properties))
        xd_window_close(properties->win);
}
static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) {
    props *properties = user;
    close_customize(properties);
    close_start_custom(properties);
    xd_window_close(properties->win);
}
static void on_apply([[maybe_unused]] xd_widget *widget, void *user) { apply(user); }
static void on_page([[maybe_unused]] xd_widget *widget, [[maybe_unused]] void *user) {}

int main(int argc, char **argv) {
    static props properties;
    properties.ui = xd_ui_connect(argc, argv);
    if (properties.ui == nullptr)
        return EXIT_FAILURE;
    xd_settings_load(&properties.saved);
    properties.pending = properties.saved;
    int page = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && strcmp(argv[i + 1], "1") == 0)
            page = 1;
    }

    properties.style = xd_ui_style(properties.ui);
    properties.win = xd_window_create(properties.ui, "Taskbar and Start Menu Properties",
                                      "xd-taskbar", 380, 388);
    static const char *const tabs[] = {"Taskbar", "Start Menu"};
    xd_widget *tab =
        xd_tabs(properties.win, (xd_rect){7, 7, 366, 342}, tabs, 2, on_page, &properties);

    /* Taskbar */
    /* XP's Taskbar tab, with Windows 7's unified taskbar, its "Taskbar
     * buttons" and "Use small icons", and "all displays". */
    xd_widget_set_page(
        xd_groupbox(properties.win, (xd_rect){20, 40, 340, 220}, "Taskbar appearance"), 0);
    static const struct {
        const char *label;
        size_t offset;
        xd_widget_cb cb;
    } boxes[] = {
        {"Lock the taskbar", offsetof(xd_settings, locked), on_lock},
        {"Auto-hide the taskbar", offsetof(xd_settings, auto_hide), on_auto_hide},
        {"Keep the taskbar on top of other windows", offsetof(xd_settings, on_top), on_top},
        {"Use the unified taskbar", 0, on_unified},
        {"Group similar taskbar buttons", offsetof(xd_settings, group), on_group},
        {"Use small icons", 0, on_small_icons},
        {"Show Quick Launch", offsetof(xd_settings, quick_launch), on_quick_launch},
        {"Show the taskbar on all displays", offsetof(xd_settings, all_displays), on_all_displays},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(boxes); i++) {
        int y = 60 + 24 * (int)i;
        bool checked;
        if (boxes[i].cb == on_unified)
            checked = xd_settings_taskbar_unified(&properties.pending, properties.style);
        else if (boxes[i].cb == on_small_icons)
            checked = !xd_settings_taskbar_large_icons(&properties.pending, properties.style);
        else
            checked = *(const bool *)((const char *)&properties.pending + boxes[i].offset);
        xd_widget *box = xd_checkbox(properties.win, (xd_rect){32, y, 320, 20}, boxes[i].label,
                                     checked, boxes[i].cb, &properties);
        xd_widget_set_page(box, 0);
        if (boxes[i].cb == on_small_icons)
            properties.small_icons = box;
        if (boxes[i].cb != on_group)
            continue;
        /* Its place on the unified taskbar: 7's "Taskbar buttons". */
        properties.group = box;
        properties.buttons_label =
            xd_label(properties.win, (xd_rect){32, y + 2, 90, 16}, "Taskbar buttons:", 0);
        xd_widget_set_page(properties.buttons_label, 0);
        properties.buttons = xd_combo(properties.win, (xd_rect){124, y - 1, 228, 21}, false,
                                      on_buttons, &properties);
        xd_widget_set_page(properties.buttons, 0);
        xd_widget_add_item(properties.buttons, "Always combine, hide labels");
        xd_widget_add_item(properties.buttons, "Combine when taskbar is full");
        xd_widget_add_item(properties.buttons, "Never combine");
        xd_widget_select(properties.buttons, (int)properties.pending.buttons);
    }
    show_buttons_choice(&properties);
    xd_widget_set_page(
        xd_groupbox(properties.win, (xd_rect){20, 266, 340, 70}, "Notification area"), 0);
    xd_widget_set_page(xd_checkbox(properties.win, (xd_rect){32, 286, 300, 20}, "Show the clock",
                                   properties.pending.show_clock, on_clock, &properties),
                       0);
    xd_widget_set_page(xd_checkbox(properties.win, (xd_rect){32, 310, 220, 20},
                                   "Hide inactive icons", properties.pending.hide_inactive,
                                   on_hide_inactive, &properties),
                       0);
    xd_widget_set_page(xd_button(properties.win, (xd_rect){268, 308, 84, 23}, "Customize...",
                                 on_customize, &properties),
                       0);

    /* Start Menu */
    xd_widget_set_page(xd_label(properties.win, (xd_rect){20, 44, 340, 16}, "Start menu style:", 0),
                       1);
    xd_widget *style =
        xd_combo(properties.win, (xd_rect){20, 62, 220, 21}, false, on_style, &properties);
    xd_widget_set_page(style, 1);
    properties.start_customize = xd_button(properties.win, (xd_rect){276, 61, 84, 23},
                                           "Customize...", on_start_customize, &properties);
    xd_widget_set_page(properties.start_customize, 1);
    xd_widget_add_item(style, "Classic (Windows 2000)");
    xd_widget_add_item(style, "Windows XP");
    xd_widget_add_item(style, "Windows 7");
    properties.description = xd_label(properties.win, (xd_rect){20, 96, 340, 64},
                                      descriptions[properties.pending.start_style], XD_LABEL_WRAP);
    xd_widget_set_page(properties.description, 1);
    xd_widget_select(style, (int)properties.pending.start_style);
    xd_widget_set_enabled(properties.start_customize,
                          properties.pending.start_style == XD_START_XP);
    xd_widget_select(tab, page);

    xd_widget *ok =
        xd_button(properties.win, (xd_rect){136, 357, 75, 23}, "OK", on_ok, &properties);
    xd_widget *cancel =
        xd_button(properties.win, (xd_rect){217, 357, 75, 23}, "Cancel", on_cancel, &properties);
    properties.apply =
        xd_button(properties.win, (xd_rect){298, 357, 75, 23}, "Apply", on_apply, &properties);
    xd_window_set_default(properties.win, ok);
    xd_window_set_cancel(properties.win, cancel);
    xd_window_set_focus(properties.win, tab);
    changed(&properties);
    xd_window_show(properties.win);
    xd_ui_run(properties.ui);
    xd_ui_destroy(properties.ui);
    return EXIT_SUCCESS;
}
