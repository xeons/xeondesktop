/* SPDX-License-Identifier: MIT
 * xd-scanners: Windows XP's Scanners and Cameras folder. Scanners come
 * from SANE, cameras from libgphoto2 (both look for devices themselves:
 * there is nothing to add by hand). A device's Properties test it; opening
 * it starts the Scanner and Camera Wizard.
 *
 *   xd-scanners                       the folder
 *   xd-scanners --scan DEVICE         the wizard for a SANE device
 *   xd-scanners --camera MODEL PORT   the wizard for a camera (as
 *                                     gphoto2 --camera MODEL --port PORT)
 *
 * SANE_CONFIG_DIR chooses SANE's configuration, as for every SANE program.
 */
#include <stdlib.h>

#include "../gtkapp/gtkapp.h"
#include "scanners.h"

static constexpr GdkModifierType NO_MODS = (GdkModifierType)0;

enum { D_ICON, D_NAME, D_DEVICE, D_COUNT };

typedef struct folder {
    GtkWidget *window, *view, *status, *open_item, *props_item;
    GtkListStore *store;
    GPtrArray *devices; /* xd_imaging_device*, as listed */
    GCancellable *cancel;
    bool loading;
} folder;

/* ---- windows --------------------------------------------------------------------------- */

static int open_windows;

static void on_held_destroy([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] gpointer data) {
    if (--open_windows == 0)
        gtk_main_quit();
}

void xd_scanners_hold(GtkWidget *window) {
    open_windows++;
    g_signal_connect(window, "destroy", G_CALLBACK(on_held_destroy), nullptr);
}

static const char *device_icon(const xd_imaging_device *imaging_device) {
    return imaging_device->camera ? "camera-photo" : "scanner";
}

/* ---- Properties ------------------------------------------------------------------------- */

typedef struct props {
    GtkWidget *window, *test, *status;
    xd_imaging_device *dev;
    GCancellable *cancel;
} props;

static void test_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *cancellable) {
    const xd_imaging_device *imaging_device = data;
    GError *error = nullptr;
    bool ok = imaging_device->camera
                  ? xd_camera_test(imaging_device->id, imaging_device->port, &error)
                  : xd_scanner_test(imaging_device->id, &error);
    if (ok)
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);
}

static void on_tested([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    bool ok = g_task_propagate_boolean(G_TASK(res), &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }
    props *properties = data;
    gtk_widget_set_sensitive(properties->test, TRUE);
    gtk_label_set_text(GTK_LABEL(properties->status), ok ? "Device ready" : "Not responding");
    char *msg = ok ? g_strdup("The device successfully completed the diagnostic test.")
                   : g_strdup_printf("The device did not complete the diagnostic test.\n\n%s",
                                     error->message);
    xd_message_box(GTK_WINDOW(properties->window), "Scanner or Camera Test Results",
                   ok ? XD_MB_INFO : XD_MB_ERROR, XD_MB_OK, FALSE, msg);
    g_free(msg);
    g_clear_error(&error);
}

static void cb_test([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    gtk_widget_set_sensitive(properties->test, FALSE);
    gtk_label_set_text(GTK_LABEL(properties->status), "Testing...");
    GTask *task = g_task_new(nullptr, properties->cancel, on_tested, properties);
    /* Its own copy: the thread may outlive the window. */
    g_task_set_task_data(task, xd_imaging_device_copy(properties->dev),
                         (GDestroyNotify)xd_imaging_device_free);
    g_task_run_in_thread(task, test_thread);
    g_object_unref(task);
}

static void props_free([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    g_cancellable_cancel(properties->cancel);
    g_object_unref(properties->cancel);
    xd_imaging_device_free(properties->dev);
    g_free(properties);
}

static void grid_row(GtkWidget *grid, int row, const char *title, GtkWidget *value) {
    GtkWidget *widget = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_xalign(GTK_LABEL(value), 0);
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
}

static void open_properties(GtkWindow *parent, const xd_imaging_device *imaging_device) {
    props *properties = g_new0(props, 1);
    properties->dev = xd_imaging_device_copy(imaging_device);
    properties->cancel = g_cancellable_new();
    properties->window = gtk_dialog_new();
    char *title = g_strdup_printf("%s Properties", imaging_device->model);
    gtk_window_set_title(GTK_WINDOW(properties->window), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(properties->window), device_icon(imaging_device));
    gtk_window_set_resizable(GTK_WINDOW(properties->window), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(properties->window), parent);
    gtk_dialog_add_button(GTK_DIALOG(properties->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties->window), "Cancel", GTK_RESPONSE_CANCEL);
    xd_scanners_hold(properties->window);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_pack_start(
        GTK_BOX(top),
        gtk_image_new_from_icon_name(device_icon(imaging_device), GTK_ICON_SIZE_DIALOG), FALSE,
        FALSE, 0);
    GtkWidget *name = gtk_label_new(imaging_device->model);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_box_pack_start(GTK_BOX(top), name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    grid_row(grid, 0, "Manufacturer:",
             gtk_label_new(imaging_device->vendor[0] != '\0' ? imaging_device->vendor : "Unknown"));
    grid_row(grid, 1, "Description:", gtk_label_new(imaging_device->type));
    grid_row(grid, 2, "Port:", gtk_label_new(imaging_device->port));
    properties->status = gtk_label_new("Device ready");
    grid_row(grid, 3, "Status:", properties->status);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    GtkWidget *diag = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *hint = gtk_label_new("To check that the device works, click Test.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_box_pack_start(GTK_BOX(diag), hint, FALSE, FALSE, 0);
    properties->test =
        gtk_button_new_with_mnemonic(imaging_device->camera ? "_Test Camera" : "_Test Scanner");
    gtk_widget_set_halign(properties->test, GTK_ALIGN_END);
    g_signal_connect(properties->test, "clicked", G_CALLBACK(cb_test), properties);
    gtk_box_pack_start(GTK_BOX(diag), properties->test, FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new("Scanner and Camera Status");
    gtk_container_set_border_width(GTK_CONTAINER(diag), 8);
    gtk_container_add(GTK_CONTAINER(frame), diag);
    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 0);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), page, gtk_label_new_with_mnemonic("_General"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties->window))), tabs,
                       TRUE, TRUE, 0);
    g_signal_connect(properties->window, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    g_signal_connect(properties->window, "destroy", G_CALLBACK(props_free), properties);
    gtk_widget_set_size_request(properties->window, 380, -1);
    gtk_widget_show_all(properties->window);
}

/* ---- the folder --------------------------------------------------------------------------- */

static xd_imaging_device *selected(folder *scanner_folder) {
    GList *sel = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(scanner_folder->view));
    xd_imaging_device *imaging_device = nullptr;
    GtkTreeIter iter;
    if (sel != nullptr &&
        gtk_tree_model_get_iter(GTK_TREE_MODEL(scanner_folder->store), &iter, sel->data))
        gtk_tree_model_get(GTK_TREE_MODEL(scanner_folder->store), &iter, D_DEVICE, &imaging_device,
                           -1);
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    return imaging_device;
}

static void update_status(folder *scanner_folder) {
    const xd_imaging_device *imaging_device = selected(scanner_folder);
    gtk_widget_set_sensitive(scanner_folder->open_item, imaging_device != nullptr);
    gtk_widget_set_sensitive(scanner_folder->props_item, imaging_device != nullptr);
    if (imaging_device != nullptr) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(scanner_folder->open_item),
                                imaging_device->camera ? "_Get Pictures" : "_Scan");
        gtk_label_set_text(GTK_LABEL(scanner_folder->status), imaging_device->type);
        return;
    }
    int count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(scanner_folder->store), nullptr);
    char *status = g_strdup_printf("%d object(s)", count);
    gtk_label_set_text(GTK_LABEL(scanner_folder->status),
                       scanner_folder->loading ? "Looking for devices..." : status);
    g_free(status);
}

static void on_listed([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GPtrArray *devices = g_task_propagate_pointer(G_TASK(res), nullptr);
    if (devices == nullptr)
        return; /* the window is gone */
    folder *scanner_folder = data;
    scanner_folder->loading = false;
    gtk_list_store_clear(scanner_folder->store);
    if (scanner_folder->devices != nullptr)
        g_ptr_array_unref(scanner_folder->devices);
    scanner_folder->devices = devices;
    for (guint i = 0; i < devices->len; i++) {
        xd_imaging_device *imaging_device = g_ptr_array_index(devices, i);
        gtk_list_store_insert_with_values(scanner_folder->store, nullptr, -1, D_ICON,
                                          device_icon(imaging_device), D_NAME,
                                          imaging_device->model, D_DEVICE, imaging_device, -1);
    }
    update_status(scanner_folder);
}

static void list_thread(GTask *task, [[maybe_unused]] gpointer src, [[maybe_unused]] gpointer data,
                        [[maybe_unused]] GCancellable *cancellable) {
    g_task_return_pointer(task, xd_imaging_devices_list(), (GDestroyNotify)g_ptr_array_unref);
}

static void reload(folder *scanner_folder) {
    if (scanner_folder->loading)
        return;
    scanner_folder->loading = true;
    update_status(scanner_folder);
    GTask *task = g_task_new(nullptr, scanner_folder->cancel, on_listed, scanner_folder);
    g_task_run_in_thread(task, list_thread);
    g_object_unref(task);
}

static void cb_open([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *scanner_folder = data;
    const xd_imaging_device *imaging_device = selected(scanner_folder);
    if (imaging_device != nullptr)
        xd_imaging_wizard_open(GTK_WINDOW(scanner_folder->window), imaging_device);
}

static void cb_props([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *scanner_folder = data;
    const xd_imaging_device *imaging_device = selected(scanner_folder);
    if (imaging_device != nullptr)
        open_properties(GTK_WINDOW(scanner_folder->window), imaging_device);
}

static void cb_refresh([[maybe_unused]] GtkWidget *widget, gpointer data) { reload(data); }

static void cb_close([[maybe_unused]] GtkWidget *widget, gpointer data) {
    gtk_widget_destroy(((folder *)data)->window);
}

static void on_activated([[maybe_unused]] GtkIconView *icon_view,
                         [[maybe_unused]] GtkTreePath *path, gpointer data) {
    cb_open(nullptr, data);
}

static void on_selection([[maybe_unused]] GtkIconView *icon_view, gpointer data) {
    update_status(data);
}

static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb,
                            folder *scanner_folder, GtkAccelGroup *accel, guint key,
                            GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, scanner_folder);
    if (accel != nullptr && key != 0)
        gtk_widget_add_accelerator(item, "activate", accel, key, mods, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static gboolean on_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    folder *scanner_folder = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path =
        gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget), (int)ev->x, (int)ev->y);
    gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
    GtkWidget *menu = gtk_menu_new();
    if (path != nullptr) {
        gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
        gtk_tree_path_free(path);
        const xd_imaging_device *imaging_device = selected(scanner_folder);
        GtkWidget *open =
            menu_item(menu, "", G_CALLBACK(cb_open), scanner_folder, nullptr, 0, NO_MODS);
        gtk_label_set_markup_with_mnemonic(GTK_LABEL(gtk_bin_get_child(GTK_BIN(open))),
                                           imaging_device != nullptr && imaging_device->camera
                                               ? "<b>_Get Pictures</b>"
                                               : "<b>_Scan</b>");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "P_roperties", G_CALLBACK(cb_props), scanner_folder, nullptr, 0, NO_MODS);
    } else {
        menu_item(menu, "R_efresh", G_CALLBACK(cb_refresh), scanner_folder, nullptr, 0, NO_MODS);
    }
    gtk_menu_attach_to_widget(GTK_MENU(menu), widget, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static GtkWidget *submenu(GtkWidget *bar, const char *label) {
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    return menu;
}

static void on_folder_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *scanner_folder = data;
    g_cancellable_cancel(scanner_folder->cancel);
    g_clear_object(&scanner_folder->cancel);
    g_clear_object(&scanner_folder->store);
    if (scanner_folder->devices != nullptr)
        g_ptr_array_unref(scanner_folder->devices);
    scanner_folder->devices = nullptr;
}

static void open_folder(void) {
    static folder scanner_folder;
    scanner_folder.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(scanner_folder.window), "Scanners and Cameras");
    gtk_window_set_icon_name(GTK_WINDOW(scanner_folder.window), "scanner");
    gtk_window_set_default_size(GTK_WINDOW(scanner_folder.window), 560, 380);
    xd_scanners_hold(scanner_folder.window);
    g_signal_connect(scanner_folder.window, "destroy", G_CALLBACK(on_folder_destroy),
                     &scanner_folder);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(scanner_folder.window), accel);
    g_object_unref(accel);
    scanner_folder.cancel = g_cancellable_new();
    scanner_folder.store =
        gtk_list_store_new(D_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(scanner_folder.window), box);
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    scanner_folder.open_item =
        menu_item(file, "_Scan", G_CALLBACK(cb_open), &scanner_folder, accel, 0, NO_MODS);
    scanner_folder.props_item = menu_item(file, "P_roperties", G_CALLBACK(cb_props),
                                          &scanner_folder, accel, GDK_KEY_Return, GDK_MOD1_MASK);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "_Close", G_CALLBACK(cb_close), &scanner_folder, accel, GDK_KEY_w,
              GDK_CONTROL_MASK);
    GtkWidget *view = submenu(bar, "_View");
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), &scanner_folder, accel, GDK_KEY_F5,
              NO_MODS);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    scanner_folder.view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(scanner_folder.store));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(scanner_folder.view), GTK_SELECTION_SINGLE);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(scanner_folder.view), 100);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "stock-size", GTK_ICON_SIZE_DIALOG, nullptr);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 94, "alignment",
                 PANGO_ALIGN_CENTER, "xalign", 0.5, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(scanner_folder.view), pix, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(scanner_folder.view), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(scanner_folder.view), pix, "icon-name", D_ICON);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(scanner_folder.view), text, "text", D_NAME);
    g_signal_connect(scanner_folder.view, "item-activated", G_CALLBACK(on_activated),
                     &scanner_folder);
    g_signal_connect(scanner_folder.view, "selection-changed", G_CALLBACK(on_selection),
                     &scanner_folder);
    g_signal_connect(scanner_folder.view, "button-press-event", G_CALLBACK(on_button),
                     &scanner_folder);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), scanner_folder.view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    scanner_folder.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(scanner_folder.status), 0);
    gtk_widget_set_margin_start(scanner_folder.status, 6);
    gtk_widget_set_margin_top(scanner_folder.status, 2);
    gtk_widget_set_margin_bottom(scanner_folder.status, 2);
    gtk_box_pack_start(GTK_BOX(box), scanner_folder.status, FALSE, FALSE, 0);
    gtk_widget_show_all(scanner_folder.window);
    reload(&scanner_folder);
}

int main(int argc, char **argv) {
    char *scan = nullptr;
    gboolean camera = FALSE;
    GOptionEntry entries[] = {
        {"scan", 0, 0, G_OPTION_ARG_STRING, &scan, "Scan with a SANE device", "DEVICE"},
        {"camera", 0, 0, G_OPTION_ARG_NONE, &camera,
         "Copy pictures from a camera (MODEL PORT follow)", nullptr},
        G_OPTION_ENTRY_NULL,
    };
    xd_gtk_app_preinit("org.xeondesktop.Scanners");
    GError *error = nullptr;
    if (!gtk_init_with_args(&argc, &argv, "- Scanners and Cameras", entries, nullptr, &error)) {
        g_printerr("xd-scanners: %s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }
    if (camera && argc != 3) {
        g_printerr("xd-scanners: --camera needs a MODEL and a PORT\n");
        return EXIT_FAILURE;
    }
    xd_gtk_app_follow_theme();
    if (scan != nullptr || camera) {
        /* The model and port name it; the rest is what XP's wizard shows. */
        char empty[] = "";
        char type_scanner[] = "scanner", type_camera[] = "Digital camera";
        xd_imaging_device imaging_device = {
            .camera = camera,
            .id = camera ? argv[1] : scan,
            .port = camera ? argv[2] : scan,
            .vendor = empty,
            .model = camera ? argv[1] : scan,
            .type = camera ? type_camera : type_scanner,
        };
        xd_imaging_wizard_open(nullptr, &imaging_device);
    } else {
        open_folder();
    }
    g_free(scan);
    gtk_main();
    return EXIT_SUCCESS;
}
