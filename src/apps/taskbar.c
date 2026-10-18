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
    xd_settings saved, pending;
    xd_widget *apply;
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
static void on_buttons(xd_widget *w, void *user) {
    props *p = user;
    int i = xd_widget_selected(w);
    if (i >= 0 && i < XD_BUTTONS_COUNT)
        p->pending.buttons = (xd_taskbar_buttons)i;
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
        /* Only the XP menu's options are here so far. */
        if (p->start_customize != nullptr)
            xd_widget_set_enabled(p->start_customize, i == XD_START_XP);
    }
    changed(p);
}

static bool apply(props *p) {
    /* Only what changed here: the taskbar's own menu, or another dialog,
     * may have saved since. */
    xd_settings now;
    xd_settings_load(&now);
    xd_settings_merge(&p->saved, &p->pending, &now);
    if (!xd_settings_save(&now)) {
        fprintf(stderr, "xd-taskbar: cannot write settings\n");
        return false;
    }
    p->saved = p->pending = now;
    changed(p);
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
    size_t n = tab != nullptr ? (size_t)(tab - line) : strlen(line);
    n = XD_MIN(n, size - 1);
    memcpy(buf, line, n);
    buf[n] = '\0';
    return buf;
}

static const char *line_title(const char *line) {
    const char *tab = strchr(line, '\t');
    return tab != nullptr && tab[1] != '\0' ? tab + 1 : line;
}

/* "Title    Behavior", as XP's two columns. */
static void fill_list(customize *c) {
    int sel = xd_widget_selected(c->list);
    xd_widget_clear_items(c->list);
    for (size_t i = 0; i < c->count; i++) {
        char *row = xd_str_dup_cstr(xd_str_from_cstr(line_title(c->lines[i])));
        xd_strbuf sb = {0};
        xd_strbuf_appendf(&sb, "%s  —  %s", row, rule_names[c->rules[i]]);
        xd_widget_add_item(c->list, xd_strbuf_cstr(&sb));
        xd_strbuf_free(&sb);
        xd_free(row);
    }
    if (sel >= 0)
        xd_widget_select(c->list, sel);
}

static void on_cust_select(xd_widget *w, void *user) {
    customize *c = ((props *)user)->cust;
    int i = xd_widget_selected(w);
    xd_widget_set_enabled(c->behavior, i >= 0);
    if (i >= 0 && (size_t)i < c->count)
        xd_combo_select(c->behavior, (int)c->rules[i]);
}

static void on_cust_behavior(xd_widget *w, void *user) {
    customize *c = ((props *)user)->cust;
    int i = xd_widget_selected(c->list);
    int r = xd_combo_selected(w);
    if (i < 0 || (size_t)i >= c->count || r < 0 || r > XD_TRAY_ALWAYS_SHOW ||
        c->rules[i] == (xd_tray_rule)r)
        return;
    c->rules[i] = (xd_tray_rule)r;
    fill_list(c);
}

static void close_customize(props *p) {
    customize *c = p->cust;
    if (c == nullptr)
        return;
    p->cust = nullptr;
    xd_window_close(c->win);
    xd_free_lines(c->lines, c->count);
    xd_free(c->rules);
    xd_free(c);
}

static void on_cust_ok([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    customize *c = p->cust;
    for (size_t i = 0; i < c->count; i++) {
        char id[64];
        xd_settings_set_tray_rule(&p->pending, line_id(c->lines[i], id, sizeof(id)), c->rules[i]);
    }
    changed(p);
    close_customize(p);
}

static void on_cust_cancel([[maybe_unused]] xd_widget *w, void *user) { close_customize(user); }

static void on_cust_defaults([[maybe_unused]] xd_widget *w, void *user) {
    customize *c = ((props *)user)->cust;
    for (size_t i = 0; i < c->count; i++)
        c->rules[i] = XD_TRAY_AUTO;
    fill_list(c);
    on_cust_select(c->list, user);
}

static void on_customize([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    if (p->cust != nullptr)
        return;
    customize *c = XD_NEW(customize);
    p->cust = c;
    xd_state_read_lines("tray-icons", &c->lines, &c->count, XD_TRAY_RULES_MAX * 2);
    c->rules = xd_alloc_array(XD_MAX(c->count, 1), sizeof(*c->rules));
    for (size_t i = 0; i < c->count; i++) {
        char id[64];
        c->rules[i] = xd_settings_tray_rule(&p->pending, line_id(c->lines[i], id, sizeof(id)));
    }
    c->win = xd_window_create(p->ui, "Customize Notifications", "xd-taskbar", 380, 316);
    xd_label(c->win, (xd_rect){12, 12, 356, 44},
             "XeonDesktop shows icons for active and urgent notifications, and hides "
             "inactive ones. You can change this for items in the list below.",
             XD_LABEL_WRAP);
    xd_label(c->win, (xd_rect){12, 62, 356, 16},
             "Select an item, then choose its notification behavior:", 0);
    c->list = xd_listbox(c->win, (xd_rect){12, 82, 356, 150}, on_cust_select, p);
    xd_label(c->win, (xd_rect){12, 245, 70, 16}, "Behavior:", 0);
    c->behavior = xd_combo(c->win, (xd_rect){84, 242, 180, 21}, false, on_cust_behavior, p);
    for (size_t r = 0; r < XD_ARRAY_LEN(rule_names); r++)
        xd_combo_add(c->behavior, rule_names[r]);
    xd_widget_set_enabled(c->behavior, false);
    fill_list(c);
    xd_button(c->win, (xd_rect){12, 280, 110, 23}, "Restore Defaults", on_cust_defaults, p);
    xd_widget *ok = xd_button(c->win, (xd_rect){212, 280, 75, 23}, "OK", on_cust_ok, p);
    xd_widget *cancel = xd_button(c->win, (xd_rect){293, 280, 75, 23}, "Cancel", on_cust_cancel, p);
    xd_window_set_default(c->win, ok);
    xd_window_set_cancel(c->win, cancel);
    xd_window_set_focus(c->win, c->list);
    xd_window_show(c->win);
}

/* ---- Customize Start Menu (XP) ----------------------------------------------- */

static void close_start_custom(props *p) {
    start_custom *c = p->start_cust;
    if (c == nullptr)
        return;
    p->start_cust = nullptr;
    xd_window_close(c->win);
    xd_free(c);
}

static void on_start_clear([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    if (p->start_cust != nullptr) {
        p->start_cust->clear = true;
        xd_widget_set_enabled(w, false);
    }
}

static void on_start_custom_ok([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    start_custom *c = p->start_cust;
    p->pending.start_large_icons = xd_checkbox_checked(c->large);
    int n = xd_widget_selected(c->programs);
    if (n >= 0 && n <= XD_START_PROGRAMS_MAX)
        p->pending.start_programs = n;
    p->pending.start_highlight_new = xd_checkbox_checked(c->highlight);
    /* Clear List: the most-used programs are forgotten (not the programs). */
    if (c->clear && !xd_state_write_lines("start-frequent", nullptr, 0))
        fprintf(stderr, "xd-taskbar: cannot clear the Start menu's list\n");
    changed(p);
    close_start_custom(p);
}

static void on_start_custom_cancel([[maybe_unused]] xd_widget *w, void *user) {
    close_start_custom(user);
}

static void on_start_customize([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    if (p->start_cust != nullptr)
        return;
    start_custom *c = XD_NEW(start_custom);
    p->start_cust = c;
    c->win = xd_window_create(p->ui, "Customize Start Menu", "xd-taskbar", 380, 356);
    static const char *const tabs[] = {"General", "Advanced"};
    xd_widget *tab = xd_tabs(c->win, (xd_rect){7, 7, 366, 310}, tabs, 2, nullptr, p);

    /* General */
    xd_widget_set_page(xd_groupbox(c->win, (xd_rect){20, 40, 340, 84},
                                   "Select an icon size for programs"),
                       0);
    xd_widget_set_page(xd_image(c->win, (xd_rect){40, 62, 32, 32}, "application-x-executable"),
                       0);
    c->large = xd_radio(c->win, (xd_rect){80, 68, 100, 20}, "Large icons", true, nullptr, p);
    xd_widget_set_page(c->large, 0);
    xd_widget_set_page(xd_image(c->win, (xd_rect){200, 70, 16, 16}, "application-x-executable"),
                       0);
    xd_widget *small = xd_radio(c->win, (xd_rect){224, 68, 100, 20}, "Small icons", false,
                                nullptr, p);
    xd_widget_set_page(small, 0);
    xd_radio_set(p->pending.start_large_icons ? c->large : small);

    xd_widget_set_page(xd_groupbox(c->win, (xd_rect){20, 134, 340, 132}, "Programs"), 0);
    xd_widget_set_page(xd_label(c->win, (xd_rect){32, 154, 316, 44},
                                "The Start menu contains shortcuts to the programs you use "
                                "most often. Clearing the list of shortcuts does not delete "
                                "the programs.",
                                XD_LABEL_WRAP),
                       0);
    xd_widget_set_page(xd_label(c->win, (xd_rect){32, 207, 200, 16},
                                "Number of programs on Start menu:", 0),
                       0);
    c->programs = xd_combo(c->win, (xd_rect){236, 204, 60, 21}, false, nullptr, p);
    xd_widget_set_page(c->programs, 0);
    for (int i = 0; i <= XD_START_PROGRAMS_MAX; i++) {
        char num[8];
        snprintf(num, sizeof(num), "%d", i);
        xd_widget_add_item(c->programs, num);
    }
    xd_widget_select(c->programs, p->pending.start_programs);
    xd_widget_set_page(xd_button(c->win, (xd_rect){264, 234, 84, 23}, "Clear List",
                                 on_start_clear, p),
                       0);

    /* Advanced */
    xd_widget_set_page(xd_groupbox(c->win, (xd_rect){20, 40, 340, 56}, "Start menu settings"),
                       1);
    c->highlight = xd_checkbox(c->win, (xd_rect){32, 62, 316, 20},
                               "Highlight newly installed programs",
                               p->pending.start_highlight_new, nullptr, p);
    xd_widget_set_page(c->highlight, 1);

    xd_widget *ok = xd_button(c->win, (xd_rect){217, 325, 75, 23}, "OK", on_start_custom_ok, p);
    xd_widget *cancel = xd_button(c->win, (xd_rect){298, 325, 75, 23}, "Cancel",
                                  on_start_custom_cancel, p);
    xd_window_set_default(c->win, ok);
    xd_window_set_cancel(c->win, cancel);
    xd_window_set_focus(c->win, tab);
    xd_window_show(c->win);
}

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    close_customize(p);
    close_start_custom(p);
    if (xd_settings_equal(&p->saved, &p->pending) || apply(p))
        xd_window_close(p->win);
}
static void on_cancel([[maybe_unused]] xd_widget *w, void *user) {
    props *p = user;
    close_customize(p);
    close_start_custom(p);
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
    /* Aero: Windows 7's "Taskbar buttons" choice where XP's grouping box was. */
    bool aero = xd_ui_style(p.ui) == XD_STYLE_AERO;
    for (size_t i = 0; i < XD_ARRAY_LEN(boxes); i++) {
        int y = 60 + 24 * (int)i;
        if (aero && boxes[i].cb == on_group) {
            xd_widget_set_page(xd_label(p.win, (xd_rect){32, y + 2, 90, 16}, "Taskbar buttons:", 0),
                               0);
            xd_widget *combo = xd_combo(p.win, (xd_rect){124, y - 1, 228, 21}, false, on_buttons,
                                        &p);
            xd_widget_set_page(combo, 0);
            xd_widget_add_item(combo, "Always combine, hide labels");
            xd_widget_add_item(combo, "Combine when taskbar is full");
            xd_widget_add_item(combo, "Never combine");
            xd_widget_select(combo, (int)p.pending.buttons);
            continue;
        }
        const bool *value = (const bool *)((const char *)&p.pending + boxes[i].offset);
        xd_widget *box = xd_checkbox(p.win, (xd_rect){32, y, 320, 20}, boxes[i].label, *value,
                                     boxes[i].cb, &p);
        xd_widget_set_page(box, 0);
    }
    xd_widget_set_page(xd_groupbox(p.win, (xd_rect){20, 218, 340, 70}, "Notification area"), 0);
    xd_widget_set_page(xd_checkbox(p.win, (xd_rect){32, 238, 300, 20}, "Show the clock",
                                   p.pending.show_clock, on_clock, &p),
                       0);
    xd_widget_set_page(xd_checkbox(p.win, (xd_rect){32, 262, 220, 20}, "Hide inactive icons",
                                   p.pending.hide_inactive, on_hide_inactive, &p),
                       0);
    xd_widget_set_page(xd_button(p.win, (xd_rect){268, 260, 84, 23}, "Customize...",
                                 on_customize, &p),
                       0);

    /* Start Menu */
    xd_widget_set_page(xd_label(p.win, (xd_rect){20, 44, 340, 16}, "Start menu style:", 0), 1);
    xd_widget *style = xd_combo(p.win, (xd_rect){20, 62, 220, 21}, false, on_style, &p);
    xd_widget_set_page(style, 1);
    p.start_customize = xd_button(p.win, (xd_rect){276, 61, 84, 23}, "Customize...",
                                  on_start_customize, &p);
    xd_widget_set_page(p.start_customize, 1);
    xd_widget_add_item(style, "Classic (Windows 2000)");
    xd_widget_add_item(style, "Windows XP");
    xd_widget_add_item(style, "Windows 7");
    p.description = xd_label(p.win, (xd_rect){20, 96, 340, 64},
                             descriptions[p.pending.start_style], XD_LABEL_WRAP);
    xd_widget_set_page(p.description, 1);
    xd_widget_select(style, (int)p.pending.start_style);
    xd_widget_set_enabled(p.start_customize, p.pending.start_style == XD_START_XP);
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
