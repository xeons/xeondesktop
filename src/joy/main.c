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

static int value(joystick *controller, unsigned code) {
    return libevdev_get_event_value(controller->dev, EV_ABS, code);
}

static double centered(joystick *controller, unsigned code) {
    return xd_joy_axis_centered(value(controller, code),
                                libevdev_get_abs_minimum(controller->dev, code),
                                libevdev_get_abs_maximum(controller->dev, code));
}

/* The X/Y box: XP's crosshair with the stick's position as a cross. */
static gboolean draw_xy(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    joystick *controller = data;
    double size =
        XD_MIN(gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
    double x0 = (gtk_widget_get_allocated_width(widget) - size) / 2;
    cairo_rectangle(cairo, x0 + 0.5, 0.5, size - 1, size - 1);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0.5, 0.5, 0.5);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
    /* Centre lines. */
    cairo_set_source_rgb(cairo, 0.75, 0.75, 0.75);
    cairo_move_to(cairo, x0 + size / 2, 1);
    cairo_line_to(cairo, x0 + size / 2, size - 1);
    cairo_move_to(cairo, x0 + 1, size / 2);
    cairo_line_to(cairo, x0 + size - 1, size / 2);
    cairo_stroke(cairo);
    if (controller->dev == nullptr || !controller->has_xy)
        return FALSE;
    double radius = size / 2 - 6;
    double px = x0 + size / 2 + centered(controller, ABS_X) * radius;
    double py = size / 2 + centered(controller, ABS_Y) * radius;
    cairo_set_source_rgb(cairo, 0.85, 0, 0);
    cairo_set_line_width(cairo, 2);
    cairo_move_to(cairo, px - 5, py);
    cairo_line_to(cairo, px + 5, py);
    cairo_move_to(cairo, px, py - 5);
    cairo_line_to(cairo, px, py + 5);
    cairo_stroke(cairo);
    return FALSE;
}

/* Buttons as numbered lights, eight to a row; lit (red) when pressed. */
static constexpr int LIGHT = 22;
static constexpr int LIGHT_GAP = 6;
static constexpr int LIGHTS_PER_ROW = 8;

static gboolean draw_buttons(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    joystick *controller = data;
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, "");
    for (size_t i = 0; i < controller->n_buttons; i++) {
        int col = (int)(i % LIGHTS_PER_ROW), row = (int)(i / LIGHTS_PER_ROW);
        double cx = col * (LIGHT + LIGHT_GAP) + LIGHT / 2.0 + 1;
        double cy = row * (LIGHT + LIGHT_GAP) + LIGHT / 2.0 + 1;
        bool down =
            controller->dev != nullptr &&
            libevdev_get_event_value(controller->dev, EV_KEY, controller->button_codes[i]) != 0;
        cairo_arc(cairo, cx, cy, LIGHT / 2.0, 0, 2 * XD_PI);
        if (down)
            cairo_set_source_rgb(cairo, 0.9, 0.1, 0.1);
        else
            cairo_set_source_rgb(cairo, 0.55, 0.1, 0.1);
        cairo_fill_preserve(cairo);
        cairo_set_source_rgb(cairo, 0.3, 0.05, 0.05);
        cairo_set_line_width(cairo, 1);
        cairo_stroke(cairo);
        char num[8];
        g_snprintf(num, sizeof(num), "%zu", i + 1);
        pango_layout_set_text(layout, num, -1);
        int tw, th;
        pango_layout_get_pixel_size(layout, &tw, &th);
        cairo_move_to(cairo, cx - tw / 2.0, cy - th / 2.0);
        cairo_set_source_rgb(cairo, 1, 1, 1);
        pango_cairo_show_layout(cairo, layout);
    }
    g_object_unref(layout);
    return FALSE;
}

/* The point-of-view hat: a circle with an arrow where it's pushed. */
static gboolean draw_hat(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    joystick *controller = data;
    double size =
        XD_MIN(gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
    double cx = gtk_widget_get_allocated_width(widget) / 2.0,
           cy = gtk_widget_get_allocated_height(widget) / 2.0;
    double radius = size / 2 - 2;
    cairo_arc(cairo, cx, cy, radius, 0, 2 * XD_PI);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0.5, 0.5, 0.5);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
    int dir = controller->dev != nullptr
                  ? xd_joy_hat_direction(value(controller, ABS_HAT0X), value(controller, ABS_HAT0Y))
                  : -1;
    if (dir < 0)
        return FALSE;
    double angle = dir * XD_PI / 4; /* 0 is up, clockwise */
    double tip = radius - 3;
    cairo_set_source_rgb(cairo, 0.85, 0, 0);
    cairo_set_line_width(cairo, 3);
    cairo_move_to(cairo, cx, cy);
    cairo_line_to(cairo, cx + tip * sin(angle), cy - tip * cos(angle));
    cairo_stroke(cairo);
    cairo_move_to(cairo, cx + tip * sin(angle), cy - tip * cos(angle));
    cairo_line_to(cairo, cx + (tip - 8) * sin(angle - 0.4), cy - (tip - 8) * cos(angle - 0.4));
    cairo_line_to(cairo, cx + (tip - 8) * sin(angle + 0.4), cy - (tip - 8) * cos(angle + 0.4));
    cairo_close_path(cairo);
    cairo_fill(cairo);
    return FALSE;
}

static void show_values(joystick *controller) {
    /* Only while the Test tab is on screen; mapping it again catches up. */
    if (controller->dev == nullptr || !gtk_widget_get_mapped(controller->xy))
        return;
    for (size_t i = 0; i < controller->n_bars; i++) {
        unsigned code = controller->bars[i].code;
        gtk_level_bar_set_value(
            GTK_LEVEL_BAR(controller->bars[i].bar),
            xd_joy_axis_fraction(value(controller, code),
                                 libevdev_get_abs_minimum(controller->dev, code),
                                 libevdev_get_abs_maximum(controller->dev, code)));
    }
    gtk_widget_queue_draw(controller->xy);
    gtk_widget_queue_draw(controller->buttons);
    if (controller->hat != nullptr)
        gtk_widget_queue_draw(controller->hat);
}

static void close_device(joystick *controller) {
    if (controller->watch != 0)
        g_source_remove(controller->watch);
    controller->watch = 0;
    if (controller->dev != nullptr)
        libevdev_free(controller->dev);
    controller->dev = nullptr;
    if (controller->fd >= 0)
        close(controller->fd);
    controller->fd = -1;
}

/* Events: libevdev keeps each axis's and button's state; we redraw from
 * it once the queue is empty. */
static gboolean on_input([[maybe_unused]] int fd, GIOCondition cond, gpointer data) {
    joystick *controller = data;
    struct input_event ev;
    int rc;
    unsigned flag = LIBEVDEV_READ_FLAG_NORMAL;
    do {
        rc = libevdev_next_event(controller->dev, flag, &ev);
        /* Events were dropped: catch up with the device's state. */
        if (rc == LIBEVDEV_READ_STATUS_SYNC)
            flag = LIBEVDEV_READ_FLAG_SYNC;
        else if (rc == -EAGAIN && flag == LIBEVDEV_READ_FLAG_SYNC) {
            flag = LIBEVDEV_READ_FLAG_NORMAL; /* caught up; back to the queue */
            rc = LIBEVDEV_READ_STATUS_SUCCESS;
        }
    } while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC);
    if (rc != -EAGAIN || (cond & (G_IO_HUP | G_IO_ERR))) {
        gtk_label_set_text(GTK_LABEL(controller->status), "The game controller was disconnected.");
        gtk_widget_show(controller->status); /* hidden while it reads */
        controller->watch = 0;
        close_device(controller);
        return G_SOURCE_REMOVE;
    }
    show_values(controller);
    return G_SOURCE_CONTINUE;
}

static void on_test_mapped([[maybe_unused]] GtkWidget *widget, gpointer data) { show_values(data); }

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(widget), child);
    return widget;
}

static GtkWidget *wrap_label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 56);
    return widget;
}

static GtkWidget *test_page(joystick *controller) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       wrap_label("Test the game controller. Move the stick and press the "
                                  "buttons; if the controller is not functioning properly, it "
                                  "may need to be calibrated."),
                       FALSE, FALSE, 0);
    controller->status = wrap_label("");
    gtk_box_pack_start(GTK_BOX(page), controller->status, FALSE, FALSE, 0);

    GtkWidget *axes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *xy_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    controller->xy = gtk_drawing_area_new();
    gtk_widget_set_size_request(controller->xy, 110, 110);
    g_signal_connect(controller->xy, "draw", G_CALLBACK(draw_xy), controller);
    gtk_box_pack_start(GTK_BOX(xy_box), controller->xy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(xy_box), gtk_label_new("X Axis / Y Axis"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(axes), xy_box, FALSE, FALSE, 0);
    /* The other axes, as vertical bars. */
    for (size_t i = 0; i < controller->n_bars; i++) {
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
        controller->bars[i].bar = bar;
        gtk_box_pack_start(GTK_BOX(col), bar, FALSE, FALSE, 0);
        GtkWidget *label = gtk_label_new(xd_joy_axis_name(controller->bars[i].code));
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
        gtk_box_pack_start(GTK_BOX(col), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(axes), col, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(page), frame("Axes", axes), FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    controller->buttons = gtk_drawing_area_new();
    size_t rows = XD_MAX((controller->n_buttons + LIGHTS_PER_ROW - 1) / LIGHTS_PER_ROW, (size_t)1);
    gtk_widget_set_size_request(controller->buttons, LIGHTS_PER_ROW * (LIGHT + LIGHT_GAP),
                                (int)rows * (LIGHT + LIGHT_GAP));
    g_signal_connect(controller->buttons, "draw", G_CALLBACK(draw_buttons), controller);
    gtk_box_pack_start(GTK_BOX(row), frame("Buttons", controller->buttons), TRUE, TRUE, 0);
    if (controller->dev != nullptr && libevdev_has_event_code(controller->dev, EV_ABS, ABS_HAT0X)) {
        controller->hat = gtk_drawing_area_new();
        gtk_widget_set_size_request(controller->hat, 64, 64);
        g_signal_connect(controller->hat, "draw", G_CALLBACK(draw_hat), controller);
        gtk_box_pack_start(GTK_BOX(row), frame("Point of View Hat", controller->hat), FALSE, FALSE,
                           0);
    }
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    g_signal_connect(controller->xy, "map", G_CALLBACK(on_test_mapped), controller);
    return page;
}

/* ---- the Settings tab ----------------------------------------------------------- */

enum { AX_NAME, AX_RANGE, AX_FLAT, AX_COUNT };

static GtkWidget *settings_page(joystick *controller) {
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
    for (unsigned code = 0; controller->dev != nullptr && code < ABS_CNT; code++) {
        if (!libevdev_has_event_code(controller->dev, EV_ABS, code))
            continue;
        const char *name = xd_joy_axis_is_hat(code) ? "Point of View Hat" : xd_joy_axis_name(code);
        if (name == nullptr)
            continue;
        char range[48], flat[16];
        g_snprintf(range, sizeof(range), "%d to %d",
                   libevdev_get_abs_minimum(controller->dev, code),
                   libevdev_get_abs_maximum(controller->dev, code));
        g_snprintf(flat, sizeof(flat), "%d", libevdev_get_abs_flat(controller->dev, code));
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
static void find_controls(joystick *controller) {
    controller->has_xy = libevdev_has_event_code(controller->dev, EV_ABS, ABS_X) &&
                         libevdev_has_event_code(controller->dev, EV_ABS, ABS_Y);
    for (unsigned code = 0; code < ABS_CNT; code++) {
        if (!libevdev_has_event_code(controller->dev, EV_ABS, code) || xd_joy_axis_is_hat(code) ||
            xd_joy_axis_name(code) == nullptr ||
            (controller->has_xy && (code == ABS_X || code == ABS_Y)))
            continue;
        controller->bars[controller->n_bars++].code = code;
    }
    unsigned have[KEY_CNT];
    size_t count = 0;
    for (unsigned code = BTN_MISC; code < KEY_CNT; code++)
        if (libevdev_has_event_code(controller->dev, EV_KEY, code))
            have[count++] = code;
    controller->n_buttons = xd_joy_button_order(have, count, controller->button_codes);
}

static void on_props_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    joystick *controller = data;
    close_device(controller);
    g_free(controller);
}

static void on_response(GtkDialog *dialog, [[maybe_unused]] int response,
                        [[maybe_unused]] gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void open_properties(app *application, const char *name, const char *node) {
    joystick *controller = g_new0(joystick, 1);
    const char *problem = nullptr;
    /* Read-only and never grabbed: games and the desktop keep the device. */
    controller->fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int err = errno;
    if (controller->fd < 0) {
        problem = g_strerror(err);
    } else {
        int rc = libevdev_new_from_fd(controller->fd, &controller->dev);
        if (rc < 0) {
            problem = g_strerror(-rc);
            controller->dev = nullptr;
            close(controller->fd);
            controller->fd = -1;
        }
    }
    if (controller->dev != nullptr) {
        find_controls(controller);
        controller->watch = g_unix_fd_add(
            controller->fd, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), on_input, controller);
    }

    char *title = g_strdup_printf("%s properties", name);
    controller->window = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(application->window), GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK, nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(controller->window), FALSE);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), test_page(controller),
                             gtk_label_new_with_mnemonic("_Test"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), settings_page(controller),
                             gtk_label_new_with_mnemonic("_Settings"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(controller->window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(controller->window, GTK_NOTEBOOK(tabs));
    if (problem != nullptr) {
        char *text = g_strdup_printf("The game controller can't be read (%s): %s.", node, problem);
        gtk_label_set_text(GTK_LABEL(controller->status), text);
        g_free(text);
    }
    g_signal_connect(controller->window, "response", G_CALLBACK(on_response), controller);
    g_signal_connect(controller->window, "destroy", G_CALLBACK(on_props_destroy), controller);
    gtk_widget_show_all(controller->window);
    if (problem == nullptr)
        gtk_widget_hide(controller->status);
}

/* ---- the list --------------------------------------------------------------------- */

static void fill(app *application) {
    gtk_list_store_clear(application->store);
    struct udev_enumerate *enumerator = udev_enumerate_new(application->udev);
    if (enumerator == nullptr)
        return;
    udev_enumerate_add_match_subsystem(enumerator, "input");
    udev_enumerate_add_match_property(enumerator, "ID_INPUT_JOYSTICK", "1");
    udev_enumerate_add_match_sysname(enumerator, "event*"); /* the nodes evdev reads */
    udev_enumerate_scan_devices(enumerator);
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerator)) {
        struct udev_device *device =
            udev_device_new_from_syspath(application->udev, udev_list_entry_get_name(entry));
        if (device == nullptr)
            continue;
        struct udev_device *parent =
            udev_device_get_parent_with_subsystem_devtype(device, "input", nullptr);
        const char *name =
            parent != nullptr ? udev_device_get_sysattr_value(parent, "name") : nullptr;
        const char *node = udev_device_get_devnode(device);
        if (node != nullptr)
            gtk_list_store_insert_with_values(application->store, nullptr, -1, COL_NAME,
                                              name != nullptr ? name : "Game controller",
                                              COL_STATUS, "OK", COL_NODE, node, -1);
        udev_device_unref(device);
    }
    udev_enumerate_unref(enumerator);
}

static void update_buttons(app *application) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(application->view));
    gtk_widget_set_sensitive(application->properties,
                             gtk_tree_selection_count_selected_rows(sel) > 0);
}

static void on_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    update_buttons(data);
}

static void on_properties([[maybe_unused]] GtkButton *button, gpointer data) {
    app *application = data;
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(application->view)), &model, &it))
        return;
    char *name = nullptr, *node = nullptr;
    gtk_tree_model_get(model, &it, COL_NAME, &name, COL_NODE, &node, -1);
    open_properties(application, name, node);
    g_free(name);
    g_free(node);
}

static void on_activated([[maybe_unused]] GtkTreeView *tree_view,
                         [[maybe_unused]] GtkTreePath *path,
                         [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    on_properties(nullptr, data);
}

/* A controller came or went. */
static gboolean on_uevent([[maybe_unused]] int fd, [[maybe_unused]] GIOCondition cond,
                          gpointer data) {
    app *application = data;
    struct udev_device *device = udev_monitor_receive_device(application->monitor);
    if (device == nullptr)
        return G_SOURCE_CONTINUE;
    const char *sysname = udev_device_get_sysname(device);
    if (sysname != nullptr && g_str_has_prefix(sysname, "event"))
        fill(application);
    udev_device_unref(device);
    update_buttons(application);
    return G_SOURCE_CONTINUE;
}

static void watch_devices(app *application) {
    application->monitor = udev_monitor_new_from_netlink(application->udev, "udev");
    if (application->monitor == nullptr)
        return;
    udev_monitor_filter_add_match_subsystem_devtype(application->monitor, "input", nullptr);
    if (udev_monitor_enable_receiving(application->monitor) < 0) {
        udev_monitor_unref(application->monitor);
        application->monitor = nullptr;
        return;
    }
    g_unix_fd_add(udev_monitor_get_fd(application->monitor), G_IO_IN, on_uevent, application);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.GameControllers");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static app application;
    application.udev = udev_new();

    application.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(application.window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(application.window), "input-gaming");
    gtk_window_set_default_size(GTK_WINDOW(application.window), 420, 360);
    gtk_dialog_add_button(GTK_DIALOG(application.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(application.window), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *intro = wrap_label("These settings help you configure the game controllers "
                                  "installed on your computer.");
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    application.store = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    application.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(application.store));
    g_object_unref(application.store);
    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(column, "Controller");
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "icon-name", "input-gaming", nullptr);
    gtk_tree_view_column_pack_start(column, icon, FALSE);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(column, renderer, TRUE);
    gtk_tree_view_column_add_attribute(column, renderer, "text", COL_NAME);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(application.view), column);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(application.view),
        gtk_tree_view_column_new_with_attributes("Status", gtk_cell_renderer_text_new(), "text",
                                                 COL_STATUS, nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), application.view);
    gtk_box_pack_start(GTK_BOX(inner), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    /* Linux finds controllers itself; there's nothing to add or remove. */
    GtkWidget *add = gtk_button_new_with_mnemonic("A_dd...");
    GtkWidget *remove = gtk_button_new_with_mnemonic("_Remove");
    gtk_widget_set_sensitive(add, FALSE);
    gtk_widget_set_sensitive(remove, FALSE);
    application.properties = gtk_button_new_with_mnemonic("_Properties");
    gtk_box_pack_start(GTK_BOX(row), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), application.properties, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), frame("Installed game controllers", inner), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(application.window))), box,
                       TRUE, TRUE, 0);

    if (application.udev != nullptr) {
        fill(&application);
        watch_devices(&application);
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(application.view));
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(application.store), &first))
        gtk_tree_selection_select_iter(sel, &first);
    update_buttons(&application);
    g_signal_connect(sel, "changed", G_CALLBACK(on_selected), &application);
    g_signal_connect(application.properties, "clicked", G_CALLBACK(on_properties), &application);
    g_signal_connect(application.view, "row-activated", G_CALLBACK(on_activated), &application);
    g_signal_connect(application.window, "response", G_CALLBACK(on_response), nullptr);
    g_signal_connect(application.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(application.window);
    gtk_main();
    if (application.monitor != nullptr)
        udev_monitor_unref(application.monitor);
    if (application.udev != nullptr)
        udev_unref(application.udev);
    return EXIT_SUCCESS;
}
