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

static void on_held_destroy([[maybe_unused]] GtkWidget *w, [[maybe_unused]] gpointer data) {
    if (--open_windows == 0)
        gtk_main_quit();
}

void xd_scanners_hold(GtkWidget *window) {
    open_windows++;
    g_signal_connect(window, "destroy", G_CALLBACK(on_held_destroy), nullptr);
}

static const char *device_icon(const xd_imaging_device *d) {
    return d->camera ? "camera-photo" : "scanner";
}

/* ---- Properties ------------------------------------------------------------------------- */

typedef struct props {
    GtkWidget *window, *test, *status;
    xd_imaging_device *dev;
    GCancellable *cancel;
} props;

static void test_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *c) {
    const xd_imaging_device *d = data;
    GError *error = nullptr;
    bool ok = d->camera ? xd_camera_test(d->id, d->port, &error) : xd_scanner_test(d->id, &error);
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
    props *p = data;
    gtk_widget_set_sensitive(p->test, TRUE);
    gtk_label_set_text(GTK_LABEL(p->status), ok ? "Device ready" : "Not responding");
    char *msg = ok ? g_strdup("The device successfully completed the diagnostic test.")
                   : g_strdup_printf("The device did not complete the diagnostic test.\n\n%s",
                                     error->message);
    xd_message_box(GTK_WINDOW(p->window), "Scanner or Camera Test Results",
                   ok ? XD_MB_INFO : XD_MB_ERROR, XD_MB_OK, FALSE, msg);
    g_free(msg);
    g_clear_error(&error);
}

static void cb_test([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    gtk_widget_set_sensitive(p->test, FALSE);
    gtk_label_set_text(GTK_LABEL(p->status), "Testing...");
    GTask *task = g_task_new(nullptr, p->cancel, on_tested, p);
    /* Its own copy: the thread may outlive the window. */
    g_task_set_task_data(task, xd_imaging_device_copy(p->dev),
                         (GDestroyNotify)xd_imaging_device_free);
    g_task_run_in_thread(task, test_thread);
    g_object_unref(task);
}

static void props_free([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    g_cancellable_cancel(p->cancel);
    g_object_unref(p->cancel);
    xd_imaging_device_free(p->dev);
    g_free(p);
}

static void grid_row(GtkWidget *grid, int row, const char *title, GtkWidget *value) {
    GtkWidget *l = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_xalign(GTK_LABEL(value), 0);
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
}

static void open_properties(GtkWindow *parent, const xd_imaging_device *d) {
    props *p = g_new0(props, 1);
    p->dev = xd_imaging_device_copy(d);
    p->cancel = g_cancellable_new();
    p->window = gtk_dialog_new();
    char *title = g_strdup_printf("%s Properties", d->model);
    gtk_window_set_title(GTK_WINDOW(p->window), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(p->window), device_icon(d));
    gtk_window_set_resizable(GTK_WINDOW(p->window), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(p->window), parent);
    gtk_dialog_add_button(GTK_DIALOG(p->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p->window), "Cancel", GTK_RESPONSE_CANCEL);
    xd_scanners_hold(p->window);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_pack_start(GTK_BOX(top), gtk_image_new_from_icon_name(device_icon(d),
                                                                  GTK_ICON_SIZE_DIALOG),
                       FALSE, FALSE, 0);
    GtkWidget *name = gtk_label_new(d->model);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_box_pack_start(GTK_BOX(top), name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    grid_row(grid, 0, "Manufacturer:", gtk_label_new(d->vendor[0] != '\0' ? d->vendor
                                                                          : "Unknown"));
    grid_row(grid, 1, "Description:", gtk_label_new(d->type));
    grid_row(grid, 2, "Port:", gtk_label_new(d->port));
    p->status = gtk_label_new("Device ready");
    grid_row(grid, 3, "Status:", p->status);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    GtkWidget *diag = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *hint = gtk_label_new("To check that the device works, click Test.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_box_pack_start(GTK_BOX(diag), hint, FALSE, FALSE, 0);
    p->test = gtk_button_new_with_mnemonic(d->camera ? "_Test Camera" : "_Test Scanner");
    gtk_widget_set_halign(p->test, GTK_ALIGN_END);
    g_signal_connect(p->test, "clicked", G_CALLBACK(cb_test), p);
    gtk_box_pack_start(GTK_BOX(diag), p->test, FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new("Scanner and Camera Status");
    gtk_container_set_border_width(GTK_CONTAINER(diag), 8);
    gtk_container_add(GTK_CONTAINER(frame), diag);
    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 0);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), page, gtk_label_new_with_mnemonic("_General"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p->window))), tabs, TRUE,
                       TRUE, 0);
    g_signal_connect(p->window, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    g_signal_connect(p->window, "destroy", G_CALLBACK(props_free), p);
    gtk_widget_set_size_request(p->window, 380, -1);
    gtk_widget_show_all(p->window);
}

/* ---- the folder --------------------------------------------------------------------------- */

static xd_imaging_device *selected(folder *f) {
    GList *sel = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(f->view));
    xd_imaging_device *d = nullptr;
    GtkTreeIter iter;
    if (sel != nullptr && gtk_tree_model_get_iter(GTK_TREE_MODEL(f->store), &iter, sel->data))
        gtk_tree_model_get(GTK_TREE_MODEL(f->store), &iter, D_DEVICE, &d, -1);
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    return d;
}

static void update_status(folder *f) {
    const xd_imaging_device *d = selected(f);
    gtk_widget_set_sensitive(f->open_item, d != nullptr);
    gtk_widget_set_sensitive(f->props_item, d != nullptr);
    if (d != nullptr) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(f->open_item),
                                d->camera ? "_Get Pictures" : "_Scan");
        gtk_label_set_text(GTK_LABEL(f->status), d->type);
        return;
    }
    int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(f->store), nullptr);
    char *s = g_strdup_printf("%d object(s)", n);
    gtk_label_set_text(GTK_LABEL(f->status), f->loading ? "Looking for devices..." : s);
    g_free(s);
}

static void on_listed([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GPtrArray *devices = g_task_propagate_pointer(G_TASK(res), nullptr);
    if (devices == nullptr)
        return; /* the window is gone */
    folder *f = data;
    f->loading = false;
    gtk_list_store_clear(f->store);
    if (f->devices != nullptr)
        g_ptr_array_unref(f->devices);
    f->devices = devices;
    for (guint i = 0; i < devices->len; i++) {
        xd_imaging_device *d = g_ptr_array_index(devices, i);
        gtk_list_store_insert_with_values(f->store, nullptr, -1, D_ICON, device_icon(d), D_NAME,
                                          d->model, D_DEVICE, d, -1);
    }
    update_status(f);
}

static void list_thread(GTask *task, [[maybe_unused]] gpointer src,
                        [[maybe_unused]] gpointer data, [[maybe_unused]] GCancellable *c) {
    g_task_return_pointer(task, xd_imaging_devices_list(), (GDestroyNotify)g_ptr_array_unref);
}

static void reload(folder *f) {
    if (f->loading)
        return;
    f->loading = true;
    update_status(f);
    GTask *task = g_task_new(nullptr, f->cancel, on_listed, f);
    g_task_run_in_thread(task, list_thread);
    g_object_unref(task);
}

static void cb_open([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    const xd_imaging_device *d = selected(f);
    if (d != nullptr)
        xd_imaging_wizard_open(GTK_WINDOW(f->window), d);
}

static void cb_props([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    const xd_imaging_device *d = selected(f);
    if (d != nullptr)
        open_properties(GTK_WINDOW(f->window), d);
}

static void cb_refresh([[maybe_unused]] GtkWidget *w, gpointer data) { reload(data); }

static void cb_close([[maybe_unused]] GtkWidget *w, gpointer data) {
    gtk_widget_destroy(((folder *)data)->window);
}

static void on_activated([[maybe_unused]] GtkIconView *v, [[maybe_unused]] GtkTreePath *p,
                         gpointer data) {
    cb_open(nullptr, data);
}

static void on_selection([[maybe_unused]] GtkIconView *v, gpointer data) { update_status(data); }

static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, folder *f,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, f);
    if (accel != nullptr && key != 0)
        gtk_widget_add_accelerator(item, "activate", accel, key, mods, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    folder *f = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w), (int)ev->x, (int)ev->y);
    gtk_icon_view_unselect_all(GTK_ICON_VIEW(w));
    GtkWidget *menu = gtk_menu_new();
    if (path != nullptr) {
        gtk_icon_view_select_path(GTK_ICON_VIEW(w), path);
        gtk_tree_path_free(path);
        const xd_imaging_device *d = selected(f);
        GtkWidget *open = menu_item(menu, "", G_CALLBACK(cb_open), f, nullptr, 0, NO_MODS);
        gtk_label_set_markup_with_mnemonic(GTK_LABEL(gtk_bin_get_child(GTK_BIN(open))),
                                           d != nullptr && d->camera ? "<b>_Get Pictures</b>"
                                                                     : "<b>_Scan</b>");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "P_roperties", G_CALLBACK(cb_props), f, nullptr, 0, NO_MODS);
    } else {
        menu_item(menu, "R_efresh", G_CALLBACK(cb_refresh), f, nullptr, 0, NO_MODS);
    }
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, nullptr);
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

static void on_folder_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    g_cancellable_cancel(f->cancel);
    g_clear_object(&f->cancel);
    g_clear_object(&f->store);
    if (f->devices != nullptr)
        g_ptr_array_unref(f->devices);
    f->devices = nullptr;
}

static void open_folder(void) {
    static folder f;
    f.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(f.window), "Scanners and Cameras");
    gtk_window_set_icon_name(GTK_WINDOW(f.window), "scanner");
    gtk_window_set_default_size(GTK_WINDOW(f.window), 560, 380);
    xd_scanners_hold(f.window);
    g_signal_connect(f.window, "destroy", G_CALLBACK(on_folder_destroy), &f);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(f.window), accel);
    g_object_unref(accel);
    f.cancel = g_cancellable_new();
    f.store = gtk_list_store_new(D_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(f.window), box);
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    f.open_item = menu_item(file, "_Scan", G_CALLBACK(cb_open), &f, accel, 0, NO_MODS);
    f.props_item = menu_item(file, "P_roperties", G_CALLBACK(cb_props), &f, accel,
                             GDK_KEY_Return, GDK_MOD1_MASK);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "_Close", G_CALLBACK(cb_close), &f, accel, GDK_KEY_w, GDK_CONTROL_MASK);
    GtkWidget *view = submenu(bar, "_View");
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), &f, accel, GDK_KEY_F5, NO_MODS);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    f.view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(f.store));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(f.view), GTK_SELECTION_SINGLE);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(f.view), 100);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "stock-size", GTK_ICON_SIZE_DIALOG, nullptr);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 94, "alignment",
                 PANGO_ALIGN_CENTER, "xalign", 0.5, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(f.view), pix, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(f.view), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(f.view), pix, "icon-name", D_ICON);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(f.view), text, "text", D_NAME);
    g_signal_connect(f.view, "item-activated", G_CALLBACK(on_activated), &f);
    g_signal_connect(f.view, "selection-changed", G_CALLBACK(on_selection), &f);
    g_signal_connect(f.view, "button-press-event", G_CALLBACK(on_button), &f);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), f.view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    f.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(f.status), 0);
    gtk_widget_set_margin_start(f.status, 6);
    gtk_widget_set_margin_top(f.status, 2);
    gtk_widget_set_margin_bottom(f.status, 2);
    gtk_box_pack_start(GTK_BOX(box), f.status, FALSE, FALSE, 0);
    gtk_widget_show_all(f.window);
    reload(&f);
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
        xd_imaging_device d = {
            .camera = camera,
            .id = camera ? argv[1] : scan,
            .port = camera ? argv[2] : scan,
            .vendor = empty,
            .model = camera ? argv[1] : scan,
            .type = camera ? type_camera : type_scanner,
        };
        xd_imaging_wizard_open(nullptr, &d);
    } else {
        open_folder();
    }
    g_free(scan);
    gtk_main();
    return EXIT_SUCCESS;
}
