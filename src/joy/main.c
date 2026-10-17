/* SPDX-License-Identifier: MIT
 * xd-joy: Windows XP's Game Controllers. The list is udev's joysticks and
 * gamepads (ID_INPUT_JOYSTICK), kept current as they come and go; each
 * one's Properties has XP's Test tab, live from its evdev device (opened
 * read-only and never grabbed, so games keep it), and a Settings tab.
 * Calibration would need write access (EVIOCSABS), so it's only shown.
 */
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "joyinfo.h"

#define TITLE "Game Controllers"

enum { COL_NAME, COL_STATUS, COL_NODE, COL_COUNT };

typedef struct app {
    GtkWidget *window, *view, *properties;
    GtkListStore *store;
    struct udev *udev;
    struct udev_monitor *monitor;
} app;

/* ---- the Test tab ----------------------------------------------------------------- */

typedef struct axis_bar {
    unsigned code;
    GtkWidget *bar;
} axis_bar;

typedef struct joystick {
    GtkWidget *window, *xy, *buttons, *hat, *status;
    int fd;
    struct libevdev *dev;
    guint watch;
    bool has_xy;
    axis_bar bars[ABS_CNT];
    size_t n_bars;
    unsigned button_codes[KEY_CNT];
    size_t n_buttons;
} joystick;

static int value(joystick *j, unsigned code) {
    return libevdev_get_event_value(j->dev, EV_ABS, code);
}

static double centered(joystick *j, unsigned code) {
    return xd_joy_axis_centered(value(j, code), libevdev_get_abs_minimum(j->dev, code),
                                libevdev_get_abs_maximum(j->dev, code));
}

/* The X/Y box: XP's crosshair with the stick's position as a cross. */
static gboolean draw_xy(GtkWidget *w, cairo_t *cr, gpointer data) {
    joystick *j = data;
    double size = XD_MIN(gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    double x0 = (gtk_widget_get_allocated_width(w) - size) / 2;
    cairo_rectangle(cr, x0 + 0.5, 0.5, size - 1, size - 1);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    /* Centre lines. */
    cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
    cairo_move_to(cr, x0 + size / 2, 1);
    cairo_line_to(cr, x0 + size / 2, size - 1);
    cairo_move_to(cr, x0 + 1, size / 2);
    cairo_line_to(cr, x0 + size - 1, size / 2);
    cairo_stroke(cr);
    if (j->dev == nullptr || !j->has_xy)
        return FALSE;
    double r = size / 2 - 6;
    double px = x0 + size / 2 + centered(j, ABS_X) * r;
    double py = size / 2 + centered(j, ABS_Y) * r;
    cairo_set_source_rgb(cr, 0.85, 0, 0);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, px - 5, py);
    cairo_line_to(cr, px + 5, py);
    cairo_move_to(cr, px, py - 5);
    cairo_line_to(cr, px, py + 5);
    cairo_stroke(cr);
    return FALSE;
}

/* Buttons as numbered lights, eight to a row; lit (red) when pressed. */
static constexpr int LIGHT = 22;
static constexpr int LIGHT_GAP = 6;
static constexpr int LIGHTS_PER_ROW = 8;

static gboolean draw_buttons(GtkWidget *w, cairo_t *cr, gpointer data) {
    joystick *j = data;
    PangoLayout *layout = gtk_widget_create_pango_layout(w, "");
    for (size_t i = 0; i < j->n_buttons; i++) {
        int col = (int)(i % LIGHTS_PER_ROW), row = (int)(i / LIGHTS_PER_ROW);
        double cx = col * (LIGHT + LIGHT_GAP) + LIGHT / 2.0 + 1;
        double cy = row * (LIGHT + LIGHT_GAP) + LIGHT / 2.0 + 1;
        bool down =
            j->dev != nullptr && libevdev_get_event_value(j->dev, EV_KEY, j->button_codes[i]) != 0;
        cairo_arc(cr, cx, cy, LIGHT / 2.0, 0, 2 * XD_PI);
        if (down)
            cairo_set_source_rgb(cr, 0.9, 0.1, 0.1);
        else
            cairo_set_source_rgb(cr, 0.55, 0.1, 0.1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.3, 0.05, 0.05);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        char num[8];
        g_snprintf(num, sizeof(num), "%zu", i + 1);
        pango_layout_set_text(layout, num, -1);
        int tw, th;
        pango_layout_get_pixel_size(layout, &tw, &th);
        cairo_move_to(cr, cx - tw / 2.0, cy - th / 2.0);
        cairo_set_source_rgb(cr, 1, 1, 1);
        pango_cairo_show_layout(cr, layout);
    }
    g_object_unref(layout);
    return FALSE;
}

/* The point-of-view hat: a circle with an arrow where it's pushed. */
static gboolean draw_hat(GtkWidget *w, cairo_t *cr, gpointer data) {
    joystick *j = data;
    double size = XD_MIN(gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    double cx = gtk_widget_get_allocated_width(w) / 2.0,
           cy = gtk_widget_get_allocated_height(w) / 2.0;
    double r = size / 2 - 2;
    cairo_arc(cr, cx, cy, r, 0, 2 * XD_PI);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    int dir =
        j->dev != nullptr ? xd_joy_hat_direction(value(j, ABS_HAT0X), value(j, ABS_HAT0Y)) : -1;
    if (dir < 0)
        return FALSE;
    double a = dir * XD_PI / 4; /* 0 is up, clockwise */
    double tip = r - 3;
    cairo_set_source_rgb(cr, 0.85, 0, 0);
    cairo_set_line_width(cr, 3);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + tip * sin(a), cy - tip * cos(a));
    cairo_stroke(cr);
    cairo_move_to(cr, cx + tip * sin(a), cy - tip * cos(a));
    cairo_line_to(cr, cx + (tip - 8) * sin(a - 0.4), cy - (tip - 8) * cos(a - 0.4));
    cairo_line_to(cr, cx + (tip - 8) * sin(a + 0.4), cy - (tip - 8) * cos(a + 0.4));
    cairo_close_path(cr);
    cairo_fill(cr);
    return FALSE;
}

static void show_values(joystick *j) {
    /* Only while the Test tab is on screen; mapping it again catches up. */
    if (j->dev == nullptr || !gtk_widget_get_mapped(j->xy))
        return;
    for (size_t i = 0; i < j->n_bars; i++) {
        unsigned c = j->bars[i].code;
        gtk_level_bar_set_value(GTK_LEVEL_BAR(j->bars[i].bar),
                                xd_joy_axis_fraction(value(j, c),
                                                     libevdev_get_abs_minimum(j->dev, c),
                                                     libevdev_get_abs_maximum(j->dev, c)));
    }
    gtk_widget_queue_draw(j->xy);
    gtk_widget_queue_draw(j->buttons);
    if (j->hat != nullptr)
        gtk_widget_queue_draw(j->hat);
}

static void close_device(joystick *j) {
    if (j->watch != 0)
        g_source_remove(j->watch);
    j->watch = 0;
    if (j->dev != nullptr)
        libevdev_free(j->dev);
    j->dev = nullptr;
    if (j->fd >= 0)
        close(j->fd);
    j->fd = -1;
}

/* Events: libevdev keeps each axis's and button's state; we redraw from
 * it once the queue is empty. */
static gboolean on_input([[maybe_unused]] int fd, GIOCondition cond, gpointer data) {
    joystick *j = data;
    struct input_event ev;
    int rc;
    unsigned flag = LIBEVDEV_READ_FLAG_NORMAL;
    do {
        rc = libevdev_next_event(j->dev, flag, &ev);
        /* Events were dropped: catch up with the device's state. */
        if (rc == LIBEVDEV_READ_STATUS_SYNC)
            flag = LIBEVDEV_READ_FLAG_SYNC;
        else if (rc == -EAGAIN && flag == LIBEVDEV_READ_FLAG_SYNC) {
            flag = LIBEVDEV_READ_FLAG_NORMAL; /* caught up; back to the queue */
            rc = LIBEVDEV_READ_STATUS_SUCCESS;
        }
    } while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC);
    if (rc != -EAGAIN || (cond & (G_IO_HUP | G_IO_ERR))) {
        gtk_label_set_text(GTK_LABEL(j->status), "The game controller was disconnected.");
        gtk_widget_show(j->status); /* hidden while it reads */
        j->watch = 0;
        close_device(j);
        return G_SOURCE_REMOVE;
    }
    show_values(j);
    return G_SOURCE_CONTINUE;
}

static void on_test_mapped([[maybe_unused]] GtkWidget *w, gpointer data) { show_values(data); }

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *wrap_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
}

static GtkWidget *test_page(joystick *j) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       wrap_label("Test the game controller. Move the stick and press the "
                                  "buttons; if the controller is not functioning properly, it "
                                  "may need to be calibrated."),
                       FALSE, FALSE, 0);
    j->status = wrap_label("");
    gtk_box_pack_start(GTK_BOX(page), j->status, FALSE, FALSE, 0);

    GtkWidget *axes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *xy_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    j->xy = gtk_drawing_area_new();
    gtk_widget_set_size_request(j->xy, 110, 110);
    g_signal_connect(j->xy, "draw", G_CALLBACK(draw_xy), j);
    gtk_box_pack_start(GTK_BOX(xy_box), j->xy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(xy_box), gtk_label_new("X Axis / Y Axis"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(axes), xy_box, FALSE, FALSE, 0);
    /* The other axes, as vertical bars. */
    for (size_t i = 0; i < j->n_bars; i++) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);
        gtk_orientable_set_orientation(GTK_ORIENTABLE(bar), GTK_ORIENTATION_VERTICAL);
        gtk_level_bar_set_inverted(GTK_LEVEL_BAR(bar), TRUE); /* fills from the bottom */
        /* No low/high colours: a position, not a level. */
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_LOW);
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_HIGH);
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_FULL);
        gtk_widget_set_size_request(bar, 16, 110);
        gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
        j->bars[i].bar = bar;
        gtk_box_pack_start(GTK_BOX(col), bar, FALSE, FALSE, 0);
        GtkWidget *label = gtk_label_new(xd_joy_axis_name(j->bars[i].code));
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
        gtk_box_pack_start(GTK_BOX(col), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(axes), col, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(page), frame("Axes", axes), FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    j->buttons = gtk_drawing_area_new();
    size_t rows = XD_MAX((j->n_buttons + LIGHTS_PER_ROW - 1) / LIGHTS_PER_ROW, (size_t)1);
    gtk_widget_set_size_request(j->buttons, LIGHTS_PER_ROW * (LIGHT + LIGHT_GAP),
                                (int)rows * (LIGHT + LIGHT_GAP));
    g_signal_connect(j->buttons, "draw", G_CALLBACK(draw_buttons), j);
    gtk_box_pack_start(GTK_BOX(row), frame("Buttons", j->buttons), TRUE, TRUE, 0);
    if (j->dev != nullptr && libevdev_has_event_code(j->dev, EV_ABS, ABS_HAT0X)) {
        j->hat = gtk_drawing_area_new();
        gtk_widget_set_size_request(j->hat, 64, 64);
        g_signal_connect(j->hat, "draw", G_CALLBACK(draw_hat), j);
        gtk_box_pack_start(GTK_BOX(row), frame("Point of View Hat", j->hat), FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    g_signal_connect(j->xy, "map", G_CALLBACK(on_test_mapped), j);
    return page;
}

/* ---- the Settings tab ----------------------------------------------------------- */

enum { AX_NAME, AX_RANGE, AX_FLAT, AX_COUNT };

static GtkWidget *settings_page(joystick *j) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *cal = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(cal),
                       wrap_label("If your game controller is not functioning properly, it may "
                                  "need to be calibrated. The controller reports its own "
                                  "ranges; changing them needs write access to the device."),
                       FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *calibrate = gtk_button_new_with_mnemonic("_Calibrate...");
    GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset to default");
    gtk_widget_set_sensitive(calibrate, FALSE);
    gtk_widget_set_sensitive(reset, FALSE);
    gtk_box_pack_end(GTK_BOX(buttons), calibrate, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cal), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Calibration", cal), FALSE, FALSE, 0);

    /* Each axis's range and dead zone, as the device reports them. */
    GtkListStore *store = gtk_list_store_new(AX_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    for (unsigned c = 0; j->dev != nullptr && c < ABS_CNT; c++) {
        if (!libevdev_has_event_code(j->dev, EV_ABS, c))
            continue;
        const char *name = xd_joy_axis_is_hat(c) ? "Point of View Hat" : xd_joy_axis_name(c);
        if (name == nullptr)
            continue;
        char range[48], flat[16];
        g_snprintf(range, sizeof(range), "%d to %d", libevdev_get_abs_minimum(j->dev, c),
                   libevdev_get_abs_maximum(j->dev, c));
        g_snprintf(flat, sizeof(flat), "%d", libevdev_get_abs_flat(j->dev, c));
        gtk_list_store_insert_with_values(store, nullptr, -1, AX_NAME, name, AX_RANGE, range,
                                          AX_FLAT, flat, -1);
    }
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    const char *titles[] = {"Axis", "Range", "Dead zone"};
    for (int i = 0; i < AX_COUNT; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            titles[i], gtk_cell_renderer_text_new(), "text", i, nullptr);
        gtk_tree_view_column_set_expand(col, i == AX_NAME);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 120);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    return page;
}

/* ---- Properties ------------------------------------------------------------------- */

/* What the Test tab shows: X/Y in the box, other axes as bars (hats get
 * their own picture), buttons in joydev's order. */
static void find_controls(joystick *j) {
    j->has_xy = libevdev_has_event_code(j->dev, EV_ABS, ABS_X) &&
                libevdev_has_event_code(j->dev, EV_ABS, ABS_Y);
    for (unsigned c = 0; c < ABS_CNT; c++) {
        if (!libevdev_has_event_code(j->dev, EV_ABS, c) || xd_joy_axis_is_hat(c) ||
            xd_joy_axis_name(c) == nullptr || (j->has_xy && (c == ABS_X || c == ABS_Y)))
            continue;
        j->bars[j->n_bars++].code = c;
    }
    unsigned have[KEY_CNT];
    size_t n = 0;
    for (unsigned c = BTN_MISC; c < KEY_CNT; c++)
        if (libevdev_has_event_code(j->dev, EV_KEY, c))
            have[n++] = c;
    j->n_buttons = xd_joy_button_order(have, n, j->button_codes);
}

static void on_props_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    joystick *j = data;
    close_device(j);
    g_free(j);
}

static void on_response(GtkDialog *d, [[maybe_unused]] int response,
                        [[maybe_unused]] gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void open_properties(app *a, const char *name, const char *node) {
    joystick *j = g_new0(joystick, 1);
    const char *problem = nullptr;
    /* Read-only and never grabbed: games and the desktop keep the device. */
    j->fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int err = errno;
    if (j->fd < 0) {
        problem = g_strerror(err);
    } else {
        int rc = libevdev_new_from_fd(j->fd, &j->dev);
        if (rc < 0) {
            problem = g_strerror(-rc);
            j->dev = nullptr;
            close(j->fd);
            j->fd = -1;
        }
    }
    if (j->dev != nullptr) {
        find_controls(j);
        j->watch = g_unix_fd_add(j->fd, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), on_input, j);
    }

    char *title = g_strdup_printf("%s properties", name);
    j->window = gtk_dialog_new_with_buttons(title, GTK_WINDOW(a->window), GTK_DIALOG_MODAL, "OK",
                                            GTK_RESPONSE_OK, nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(j->window), FALSE);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), test_page(j),
                             gtk_label_new_with_mnemonic("_Test"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), settings_page(j),
                             gtk_label_new_with_mnemonic("_Settings"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(j->window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(j->window, GTK_NOTEBOOK(tabs));
    if (problem != nullptr) {
        char *text = g_strdup_printf("The game controller can't be read (%s): %s.", node, problem);
        gtk_label_set_text(GTK_LABEL(j->status), text);
        g_free(text);
    }
    g_signal_connect(j->window, "response", G_CALLBACK(on_response), j);
    g_signal_connect(j->window, "destroy", G_CALLBACK(on_props_destroy), j);
    gtk_widget_show_all(j->window);
    if (problem == nullptr)
        gtk_widget_hide(j->status);
}

/* ---- the list --------------------------------------------------------------------- */

static void fill(app *a) {
    gtk_list_store_clear(a->store);
    struct udev_enumerate *e = udev_enumerate_new(a->udev);
    if (e == nullptr)
        return;
    udev_enumerate_add_match_subsystem(e, "input");
    udev_enumerate_add_match_property(e, "ID_INPUT_JOYSTICK", "1");
    udev_enumerate_add_match_sysname(e, "event*"); /* the nodes evdev reads */
    udev_enumerate_scan_devices(e);
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
        struct udev_device *d =
            udev_device_new_from_syspath(a->udev, udev_list_entry_get_name(entry));
        if (d == nullptr)
            continue;
        struct udev_device *parent =
            udev_device_get_parent_with_subsystem_devtype(d, "input", nullptr);
        const char *name =
            parent != nullptr ? udev_device_get_sysattr_value(parent, "name") : nullptr;
        const char *node = udev_device_get_devnode(d);
        if (node != nullptr)
            gtk_list_store_insert_with_values(a->store, nullptr, -1, COL_NAME,
                                              name != nullptr ? name : "Game controller",
                                              COL_STATUS, "OK", COL_NODE, node, -1);
        udev_device_unref(d);
    }
    udev_enumerate_unref(e);
}

static void update_buttons(app *a) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->view));
    gtk_widget_set_sensitive(a->properties, gtk_tree_selection_count_selected_rows(sel) > 0);
}

static void on_selected([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    update_buttons(data);
}

static void on_properties([[maybe_unused]] GtkButton *b, gpointer data) {
    app *a = data;
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->view)),
                                         &model, &it))
        return;
    char *name = nullptr, *node = nullptr;
    gtk_tree_model_get(model, &it, COL_NAME, &name, COL_NODE, &node, -1);
    open_properties(a, name, node);
    g_free(name);
    g_free(node);
}

static void on_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                         [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    on_properties(nullptr, data);
}

/* A controller came or went. */
static gboolean on_uevent([[maybe_unused]] int fd, [[maybe_unused]] GIOCondition cond,
                          gpointer data) {
    app *a = data;
    struct udev_device *d = udev_monitor_receive_device(a->monitor);
    if (d == nullptr)
        return G_SOURCE_CONTINUE;
    const char *sysname = udev_device_get_sysname(d);
    if (sysname != nullptr && g_str_has_prefix(sysname, "event"))
        fill(a);
    udev_device_unref(d);
    update_buttons(a);
    return G_SOURCE_CONTINUE;
}

static void watch_devices(app *a) {
    a->monitor = udev_monitor_new_from_netlink(a->udev, "udev");
    if (a->monitor == nullptr)
        return;
    udev_monitor_filter_add_match_subsystem_devtype(a->monitor, "input", nullptr);
    if (udev_monitor_enable_receiving(a->monitor) < 0) {
        udev_monitor_unref(a->monitor);
        a->monitor = nullptr;
        return;
    }
    g_unix_fd_add(udev_monitor_get_fd(a->monitor), G_IO_IN, on_uevent, a);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.GameControllers");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static app a;
    a.udev = udev_new();

    a.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(a.window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(a.window), "input-gaming");
    gtk_window_set_default_size(GTK_WINDOW(a.window), 420, 360);
    gtk_dialog_add_button(GTK_DIALOG(a.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(a.window), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *intro = wrap_label("These settings help you configure the game controllers "
                                  "installed on your computer.");
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    a.store = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    a.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a.store));
    g_object_unref(a.store);
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Controller");
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "icon-name", "input-gaming", nullptr);
    gtk_tree_view_column_pack_start(c, icon, FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_add_attribute(c, r, "text", COL_NAME);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a.view), c);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a.view), gtk_tree_view_column_new_with_attributes(
                                                           "Status", gtk_cell_renderer_text_new(),
                                                           "text", COL_STATUS, nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), a.view);
    gtk_box_pack_start(GTK_BOX(inner), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    /* Linux finds controllers itself; there's nothing to add or remove. */
    GtkWidget *add = gtk_button_new_with_mnemonic("A_dd...");
    GtkWidget *remove = gtk_button_new_with_mnemonic("_Remove");
    gtk_widget_set_sensitive(add, FALSE);
    gtk_widget_set_sensitive(remove, FALSE);
    a.properties = gtk_button_new_with_mnemonic("_Properties");
    gtk_box_pack_start(GTK_BOX(row), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), a.properties, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), frame("Installed game controllers", inner), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(a.window))), box, TRUE, TRUE,
                       0);

    if (a.udev != nullptr) {
        fill(&a);
        watch_devices(&a);
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a.view));
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(a.store), &first))
        gtk_tree_selection_select_iter(sel, &first);
    update_buttons(&a);
    g_signal_connect(sel, "changed", G_CALLBACK(on_selected), &a);
    g_signal_connect(a.properties, "clicked", G_CALLBACK(on_properties), &a);
    g_signal_connect(a.view, "row-activated", G_CALLBACK(on_activated), &a);
    g_signal_connect(a.window, "response", G_CALLBACK(on_response), nullptr);
    g_signal_connect(a.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(a.window);
    gtk_main();
    if (a.monitor != nullptr)
        udev_monitor_unref(a.monitor);
    if (a.udev != nullptr)
        udev_unref(a.udev);
    return EXIT_SUCCESS;
}
