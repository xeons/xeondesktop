/* SPDX-License-Identifier: MIT
 * The Add Bluetooth Device Wizard, as in XP SP2:
 *
 *   Welcome    "My device is set up and ready to be found."
 *   Search     discovery (Adapter1.StartDiscovery); pick a device
 *   Passkey    choose one, use the documentation's, your own, or none
 *   Adding     Device1.Pair (the agent answers with the passkey), then
 *              Trusted and Connect
 *   Completing the outcome
 *
 * Bluetooth 2.1 and later devices pair without a typed passkey (Secure
 * Simple Pairing): the agent shows a number to compare or to type on the
 * device instead.
 */
#include <sys/random.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "bt.h"

enum { PAGE_WELCOME, PAGE_SEARCH, PAGE_PASSKEY, PAGE_ADDING, PAGE_FINISH };
enum { FOUND_ICON, FOUND_TEXT, FOUND_PATH, FOUND_COUNT };

#define WIZARD_TITLE "Add Bluetooth Device Wizard"

struct bt_wizard {
    bt_app *app;
    GtkWidget *window, *stack, *back, *next, *cancel;
    int page;
    /* Welcome */
    GtkWidget *ready;
    /* Search */
    GtkListStore *found;
    GtkWidget *found_view, *spinner, *search_status;
    char *adapter;    /* the adapter we search with */
    bool discovering; /* we started discovery */
    GHashTable *seen; /* devices that turned up while searching */
    /* Passkey */
    GtkWidget *pk_auto, *pk_doc, *pk_doc_entry, *pk_own, *pk_own_entry, *pk_none;
    /* Adding */
    GtkWidget *adding_text, *adding_pin, *adding_spinner;
    char *device;
    GCancellable *cancel_op;
    bool busy;
    /* Completing */
    GtkWidget *finish_text;
};

GtkWindow *bt_wizard_window(bt_app *a) {
    return a->wizard != nullptr ? GTK_WINDOW(a->wizard->window) : nullptr;
}

/* ---- look --------------------------------------------------------------------- */

/* The welcome and completing pages' watermark: XP's blue band. */
static gboolean draw_watermark(GtkWidget *w, cairo_t *cr, [[maybe_unused]] gpointer data) {
    double width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, height);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.35, 0.53, 0.89);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.05, 0.20, 0.55);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    GtkIconTheme *theme = gtk_icon_theme_get_for_screen(gtk_widget_get_screen(w));
    int scale = gtk_widget_get_scale_factor(w);
    cairo_surface_t *icon =
        gtk_icon_theme_load_surface(theme, "bluetooth", 64, scale, gtk_widget_get_window(w),
                                    GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
    if (icon != nullptr) {
        cairo_set_source_surface(cr, icon, (width - 64) / 2, 24);
        cairo_paint(cr);
        cairo_surface_destroy(icon);
    }
    return FALSE;
}

/* A white header band (the window colour) behind an interior page's title. */
static gboolean draw_header(GtkWidget *w, cairo_t *cr, [[maybe_unused]] gpointer data) {
    GdkRGBA base = {1, 1, 1, 1};
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    gtk_style_context_lookup_color(ctx, "theme_base_color", &base);
    gdk_cairo_set_source_rgba(cr, &base);
    cairo_paint(cr);
    return FALSE;
}

static GtkWidget *text(const char *markup) {
    GtkWidget *l = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 52);
    return l;
}

/* Welcome and Completing: watermark on the left, a big title. */
static GtkWidget *outer_page(const char *title, GtkWidget **body) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *mark = gtk_drawing_area_new();
    gtk_widget_set_size_request(mark, 164, -1);
    g_signal_connect(mark, "draw", G_CALLBACK(draw_watermark), nullptr);
    gtk_box_pack_start(GTK_BOX(page), mark, FALSE, FALSE, 0);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(right), 16);
    char *markup = g_markup_printf_escaped("<span size='large' weight='bold'>%s</span>", title);
    gtk_box_pack_start(GTK_BOX(right), text(markup), FALSE, FALSE, 0);
    g_free(markup);
    gtk_box_pack_start(GTK_BOX(page), right, TRUE, TRUE, 0);
    g_signal_connect(page, "draw", G_CALLBACK(draw_header), nullptr);
    *body = right;
    return page;
}

/* Interior pages: the header band, then the body. */
static GtkWidget *inner_page(const char *title, const char *subtitle, GtkWidget **body) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_signal_connect(header, "draw", G_CALLBACK(draw_header), nullptr);
    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(titles), 10);
    char *markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_box_pack_start(GTK_BOX(titles), text(markup), FALSE, FALSE, 0);
    g_free(markup);
    GtkWidget *sub = text(nullptr);
    gtk_label_set_text(GTK_LABEL(sub), subtitle);
    gtk_widget_set_margin_start(sub, 16);
    gtk_box_pack_start(GTK_BOX(titles), sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), titles, TRUE, TRUE, 0);
    GtkWidget *icon = gtk_image_new_from_icon_name("bluetooth", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    gtk_widget_set_margin_end(icon, 10);
    gtk_box_pack_end(GTK_BOX(header), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(*body), 14);
    gtk_widget_set_margin_start(*body, 30);
    gtk_box_pack_start(GTK_BOX(page), *body, TRUE, TRUE, 0);
    return page;
}

/* ---- searching ---------------------------------------------------------------------- */

static char *selected_device(bt_wizard *w) {
    GtkTreeModel *model;
    GtkTreeIter it;
    char *path = nullptr;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(w->found_view)),
                                        &model, &it))
        gtk_tree_model_get(model, &it, FOUND_PATH, &path, -1);
    return path;
}

static void update_buttons(bt_wizard *w);

/* Devices of our adapter that aren't paired and are in range (seen in
 * this search, or with a signal strength). */
static void fill_found(bt_wizard *w) {
    bt_app *a = w->app;
    char *keep = selected_device(w);
    gtk_list_store_clear(w->found);
    GList *objs = a->objects != nullptr ? g_dbus_object_manager_get_objects(a->objects) : nullptr;
    GtkTreeIter it;
    for (GList *o = objs; o != nullptr; o = o->next) {
        GDBusInterface *dev = g_dbus_object_get_interface(o->data, IFACE_DEVICE);
        if (dev == nullptr)
            continue;
        const char *path = g_dbus_object_get_object_path(o->data);
        char *adapter = bt_prop_s(dev, "Adapter");
        GVariant *rssi = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(dev), "RSSI");
        if (!bt_prop_b(dev, "Paired") && g_strcmp0(adapter, w->adapter) == 0 &&
            (rssi != nullptr || g_hash_table_contains(w->seen, path))) {
            char *name = bt_device_name(dev);
            xd_bt_type t = bt_device_type(dev);
            char *markup = g_markup_printf_escaped("%s\n<small>%s</small>", name, t.name);
            gtk_list_store_insert_with_values(w->found, &it, -1, FOUND_ICON, t.icon, FOUND_TEXT,
                                              markup, FOUND_PATH, path, -1);
            if (g_strcmp0(keep, path) == 0)
                gtk_tree_selection_select_iter(
                    gtk_tree_view_get_selection(GTK_TREE_VIEW(w->found_view)), &it);
            g_free(markup);
            g_free(name);
        }
        if (rssi != nullptr)
            g_variant_unref(rssi);
        g_free(adapter);
        g_object_unref(dev);
    }
    g_list_free_full(objs, g_object_unref);
    g_free(keep);

    GDBusInterface *ad = bt_interface(a, w->adapter, IFACE_ADAPTER);
    bool searching = ad != nullptr && bt_prop_b(ad, "Discovering");
    if (ad != nullptr)
        g_object_unref(ad);
    gtk_widget_set_visible(w->spinner, searching);
    if (searching)
        gtk_spinner_start(GTK_SPINNER(w->spinner));
    else
        gtk_spinner_stop(GTK_SPINNER(w->spinner));
    gtk_label_set_text(GTK_LABEL(w->search_status),
                       searching ? "Searching for Bluetooth devices..."
                                 : "If you don't see the device that you want to add, make sure "
                                   "that it is turned on and can be found, then click Search "
                                   "Again.");
    update_buttons(w);
}

static void stop_discovery(bt_wizard *w) {
    if (!w->discovering || w->adapter == nullptr)
        return;
    w->discovering = false;
    /* Synchronous: the wizard may be closing with the program. */
    GVariant *r = g_dbus_connection_call_sync(w->app->bus, BLUEZ, w->adapter, IFACE_ADAPTER,
                                              "StopDiscovery", nullptr, nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (r != nullptr)
        g_variant_unref(r);
}

static void discovery_started(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r != nullptr) {
        g_variant_unref(r);
        return;
    }
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        bt_wizard *w = data;
        w->discovering = false;
        bt_error(GTK_WINDOW(w->window), "Searching for Bluetooth devices failed.", error);
    }
    g_error_free(error);
}

static void start_discovery(bt_wizard *w) {
    GDBusInterface *ad = bt_adapter(w->app);
    g_free(w->adapter);
    w->adapter = ad != nullptr ? g_strdup(g_dbus_proxy_get_object_path(G_DBUS_PROXY(ad))) : nullptr;
    if (ad != nullptr)
        g_object_unref(ad);
    g_hash_table_remove_all(w->seen);
    if (w->adapter == nullptr) {
        gtk_label_set_text(GTK_LABEL(w->search_status), "No Bluetooth radio was found.");
        return;
    }
    if (!w->discovering) { /* BlueZ counts each client's discovery */
        w->discovering = true;
        g_dbus_connection_call(w->app->bus, BLUEZ, w->adapter, IFACE_ADAPTER, "StartDiscovery",
                               nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, w->cancel_op,
                               discovery_started, w);
    }
    fill_found(w);
}

static void on_search_again([[maybe_unused]] GtkButton *b, gpointer data) {
    bt_wizard *w = data;
    stop_discovery(w);
    start_discovery(w);
}

/* ---- adding ------------------------------------------------------------------------- */

static void show_page(bt_wizard *w, int page);

static void finish_with(bt_wizard *w, const char *message) {
    bt_app *a = w->app;
    g_clear_pointer(&a->pin, g_free);
    g_clear_pointer(&a->pairing_path, g_free);
    bt_agent_pairing_done(a);
    w->busy = false;
    gtk_label_set_markup(GTK_LABEL(w->finish_text), message);
    show_page(w, PAGE_FINISH);
}

static void connected(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return; /* the wizard is gone */
    }
    bt_wizard *w = data;
    if (r != nullptr) {
        g_variant_unref(r);
        finish_with(w, "The Bluetooth device was successfully connected to your computer. "
                       "Your computer and the device can communicate whenever they are near "
                       "each other.\n\nTo see the device, or change its settings, use "
                       "Bluetooth Devices in Control Panel.");
    } else {
        g_dbus_error_strip_remote_error(error);
        char *msg = g_markup_printf_escaped(
            "The device was added to your computer, but it could not be connected:\n\n%s\n\n"
            "It can connect later, whenever it's near your computer.",
            error->message);
        finish_with(w, msg);
        g_free(msg);
        g_error_free(error);
    }
}

static void paired(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }
    bt_wizard *w = data;
    bt_app *a = w->app;
    char *remote = r == nullptr ? g_dbus_error_get_remote_error(error) : nullptr;
    bool ok = r != nullptr || g_strcmp0(remote, "org.bluez.Error.AlreadyExists") == 0;
    g_free(remote);
    if (r != nullptr)
        g_variant_unref(r);
    if (!ok) {
        g_dbus_error_strip_remote_error(error);
        char *msg = g_markup_printf_escaped(
            "The device could not be added:\n\n%s\n\nMake sure that the device is turned on "
            "and can be found, and that you typed the right passkey, then run the wizard "
            "again.",
            error->message);
        finish_with(w, msg);
        g_free(msg);
        g_error_free(error);
        return;
    }
    g_clear_error(&error);
    /* Trusted: it can connect again without asking. */
    bt_set_prop(a, GTK_WINDOW(w->window), w->device, IFACE_DEVICE, "Trusted",
                g_variant_new_boolean(TRUE));
    gtk_label_set_text(GTK_LABEL(w->adding_text), "Connecting to the device...");
    gtk_widget_hide(w->adding_pin);
    g_dbus_connection_call(a->bus, BLUEZ, w->device, IFACE_DEVICE, "Connect", nullptr, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, 60000, w->cancel_op, connected, w);
}

/* Eight random digits, for "Choose a passkey for me". */
static char *random_pin(void) {
    uint8_t bytes[8] = {0};
    if (getrandom(bytes, sizeof(bytes), 0) != (ssize_t)sizeof(bytes))
        for (size_t i = 0; i < sizeof(bytes); i++)
            bytes[i] = (uint8_t)g_random_int_range(0, 256);
    char *pin = g_malloc(sizeof(bytes) + 1);
    for (size_t i = 0; i < sizeof(bytes); i++)
        pin[i] = (char)('0' + bytes[i] % 10);
    pin[sizeof(bytes)] = '\0';
    return pin;
}

static void start_adding(bt_wizard *w) {
    bt_app *a = w->app;
    g_free(w->device);
    w->device = selected_device(w);
    if (w->device == nullptr)
        return;
    g_clear_pointer(&a->pin, g_free);
    g_free(a->pairing_path);
    a->pairing_path = g_strdup(w->device);
    bool show_pin = false;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->pk_auto))) {
        a->pin = random_pin();
        show_pin = true;
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->pk_doc))) {
        a->pin = g_strdup(gtk_entry_get_text(GTK_ENTRY(w->pk_doc_entry)));
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->pk_own))) {
        a->pin = g_strdup(gtk_entry_get_text(GTK_ENTRY(w->pk_own_entry)));
        show_pin = true;
    }
    GDBusInterface *dev = bt_interface(a, w->device, IFACE_DEVICE);
    char *name = dev != nullptr ? bt_device_name(dev) : g_strdup("the device");
    if (dev != nullptr)
        g_object_unref(dev);
    char *msg = g_strdup_printf("Adding %s. Exchanging passkeys...", name);
    gtk_label_set_text(GTK_LABEL(w->adding_text), msg);
    g_free(msg);
    g_free(name);
    if (show_pin) {
        char *pin = g_markup_printf_escaped(
            "If the device asks for a passkey, type this one on it:\n\n<span size='xx-large' "
            "weight='bold'>%s</span>",
            a->pin);
        gtk_label_set_markup(GTK_LABEL(w->adding_pin), pin);
        g_free(pin);
        gtk_widget_show(w->adding_pin);
    } else {
        gtk_widget_hide(w->adding_pin);
    }
    w->busy = true;
    show_page(w, PAGE_ADDING);
    g_dbus_connection_call(a->bus, BLUEZ, w->device, IFACE_DEVICE, "Pair", nullptr, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, 120000, w->cancel_op, paired, w);
}

/* ---- pages and buttons ------------------------------------------------------------- */

static void update_buttons(bt_wizard *w) {
    bool next = true;
    switch (w->page) {
    case PAGE_WELCOME:
        next = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->ready));
        break;
    case PAGE_SEARCH: {
        char *path = selected_device(w);
        next = path != nullptr;
        g_free(path);
        break;
    }
    case PAGE_PASSKEY:
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->pk_doc)))
            next = gtk_entry_get_text_length(GTK_ENTRY(w->pk_doc_entry)) > 0;
        else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->pk_own)))
            next = gtk_entry_get_text_length(GTK_ENTRY(w->pk_own_entry)) > 0;
        break;
    case PAGE_ADDING:
        next = false;
        break;
    default:
        break;
    }
    gtk_widget_set_sensitive(w->next, next);
    gtk_widget_set_sensitive(w->back, w->page != PAGE_WELCOME && w->page < PAGE_ADDING);
    gtk_widget_set_sensitive(w->cancel, w->page != PAGE_FINISH);
    gtk_button_set_label(GTK_BUTTON(w->next), w->page == PAGE_FINISH ? "Finish" : "_Next >");
    gtk_widget_set_sensitive(w->pk_doc_entry,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->pk_doc)));
    gtk_widget_set_sensitive(w->pk_own_entry,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->pk_own)));
}

static void show_page(bt_wizard *w, int page) {
    if (w->page == PAGE_SEARCH && page != PAGE_SEARCH)
        stop_discovery(w); /* BlueZ pairs better without it */
    w->page = page;
    char name[8];
    g_snprintf(name, sizeof(name), "%d", page);
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), name);
    if (page == PAGE_SEARCH)
        start_discovery(w);
    if (page == PAGE_ADDING)
        gtk_spinner_start(GTK_SPINNER(w->adding_spinner));
    else
        gtk_spinner_stop(GTK_SPINNER(w->adding_spinner));
    update_buttons(w);
    gtk_widget_grab_focus(w->next);
}

static void on_next([[maybe_unused]] GtkButton *b, gpointer data) {
    bt_wizard *w = data;
    if (w->page == PAGE_FINISH)
        gtk_widget_destroy(w->window);
    else if (w->page == PAGE_PASSKEY)
        start_adding(w);
    else
        show_page(w, w->page + 1);
}

static void on_back([[maybe_unused]] GtkButton *b, gpointer data) {
    bt_wizard *w = data;
    if (w->page > PAGE_WELCOME && w->page < PAGE_ADDING)
        show_page(w, w->page - 1);
}

static void on_cancel([[maybe_unused]] GtkButton *b, gpointer data) {
    bt_wizard *w = data;
    gtk_widget_destroy(w->window);
}

static void on_changed([[maybe_unused]] GtkWidget *widget, gpointer data) { update_buttons(data); }

static void on_found_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                               [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    bt_wizard *w = data;
    if (gtk_widget_get_sensitive(w->next))
        on_next(nullptr, w);
}

/* ---- live updates ------------------------------------------------------------------- */

static void props_changed([[maybe_unused]] GDBusObjectManagerClient *m,
                          [[maybe_unused]] GDBusObjectProxy *o, [[maybe_unused]] GDBusProxy *i,
                          [[maybe_unused]] GVariant *c, [[maybe_unused]] const char *const *inv,
                          gpointer data) {
    bt_wizard *w = data;
    if (w->page == PAGE_SEARCH)
        fill_found(w);
}

static void added([[maybe_unused]] GDBusObjectManager *m, GDBusObject *o, gpointer data) {
    bt_wizard *w = data;
    if (w->page == PAGE_SEARCH) {
        g_hash_table_add(w->seen, g_strdup(g_dbus_object_get_object_path(o)));
        fill_found(w);
    }
}

static void removed([[maybe_unused]] GDBusObjectManager *m, [[maybe_unused]] GDBusObject *o,
                    gpointer data) {
    bt_wizard *w = data;
    if (w->page == PAGE_SEARCH)
        fill_found(w);
}

/* ---- building ---------------------------------------------------------------------- */

static GtkWidget *welcome_page(bt_wizard *w) {
    GtkWidget *body;
    GtkWidget *page = outer_page("Welcome to the Add Bluetooth Device Wizard", &body);
    gtk_box_pack_start(
        GTK_BOX(body),
        text("Before you proceed, refer to the \"Bluetooth\" section of the device "
             "documentation. Then set up your device so that your computer can find it:\n\n"
             "  •  Turn it on\n"
             "  •  Make it discoverable (visible)\n"
             "  •  Give it a name (optional)\n"
             "  •  Press the button on the bottom of the device (keyboards and mice only)"),
        FALSE, FALSE, 0);
    w->ready = gtk_check_button_new_with_mnemonic("_My device is set up and ready to be found.");
    g_signal_connect(w->ready, "toggled", G_CALLBACK(on_changed), w);
    gtk_box_pack_start(GTK_BOX(body), w->ready, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(body), text("Only add Bluetooth devices that you trust."), FALSE,
                     FALSE, 0);
    return page;
}

static GtkWidget *search_page(bt_wizard *w) {
    GtkWidget *body;
    GtkWidget *page =
        inner_page(WIZARD_TITLE, "Select the Bluetooth device that you want to add.", &body);
    w->found = gtk_list_store_new(FOUND_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    w->found_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(w->found));
    g_object_unref(w->found);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(w->found_view), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "stock-size", GTK_ICON_SIZE_DND, "xpad", 6, "ypad", 3, nullptr);
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", FOUND_ICON);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, r, TRUE);
    gtk_tree_view_column_add_attribute(col, r, "markup", FOUND_TEXT);
    gtk_tree_view_append_column(GTK_TREE_VIEW(w->found_view), col);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), w->found_view);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(w->found_view)), "changed",
                     G_CALLBACK(on_changed), w);
    g_signal_connect(w->found_view, "row-activated", G_CALLBACK(on_found_activated), w);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    w->spinner = gtk_spinner_new();
    gtk_widget_set_valign(w->spinner, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(w->spinner, TRUE); /* shown while searching */
    w->search_status = text("");
    GtkWidget *again = gtk_button_new_with_mnemonic("_Search Again");
    gtk_widget_set_valign(again, GTK_ALIGN_START);
    g_signal_connect(again, "clicked", G_CALLBACK(on_search_again), w);
    gtk_box_pack_start(GTK_BOX(row), w->spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), w->search_status, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row), again, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
    return page;
}

static GtkWidget *passkey_entry(bt_wizard *w, GtkWidget *grid, int row, GtkWidget *radio) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(e), 16); /* a PIN's bytes */
    gtk_entry_set_width_chars(GTK_ENTRY(e), 18);
    gtk_grid_attach(GTK_GRID(grid), radio, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), e, 1, row, 1, 1);
    g_signal_connect(e, "changed", G_CALLBACK(on_changed), w);
    return e;
}

static GtkWidget *passkey_page(bt_wizard *w) {
    GtkWidget *body;
    GtkWidget *page = inner_page(WIZARD_TITLE, "Do you need a passkey to add your device?", &body);
    gtk_box_pack_start(GTK_BOX(body),
                       text("To answer this question, refer to the \"Bluetooth\" section of "
                            "the documentation that came with your device. If the "
                            "documentation specifies a passkey, use that one."),
                       FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    w->pk_auto = gtk_radio_button_new_with_mnemonic(nullptr, "_Choose a passkey for me");
    gtk_grid_attach(GTK_GRID(grid), w->pk_auto, 0, 0, 2, 1);
    w->pk_doc = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(w->pk_auto), "_Use the passkey found in the documentation:");
    w->pk_doc_entry = passkey_entry(w, grid, 1, w->pk_doc);
    w->pk_own = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(w->pk_auto),
                                                               "_Let me choose my own passkey:");
    w->pk_own_entry = passkey_entry(w, grid, 2, w->pk_own);
    w->pk_none = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(w->pk_auto),
                                                                "_Don't use a passkey");
    gtk_grid_attach(GTK_GRID(grid), w->pk_none, 0, 3, 2, 1);
    GtkWidget *radios[] = {w->pk_auto, w->pk_doc, w->pk_own, w->pk_none};
    for (size_t i = 0; i < XD_ARRAY_LEN(radios); i++)
        g_signal_connect(radios[i], "toggled", G_CALLBACK(on_changed), w);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(body),
                     text("<small>Devices made for Bluetooth 2.1 and later choose for "
                          "themselves: they show a number for you to compare, or you type one "
                          "this computer shows.</small>"),
                     FALSE, FALSE, 0);
    return page;
}

static GtkWidget *adding_page(bt_wizard *w) {
    GtkWidget *body;
    GtkWidget *page = inner_page(WIZARD_TITLE, "Please wait while the device is added.", &body);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    w->adding_spinner = gtk_spinner_new();
    w->adding_text = text("");
    gtk_box_pack_start(GTK_BOX(row), w->adding_spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), w->adding_text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
    w->adding_pin = text("");
    gtk_widget_set_no_show_all(w->adding_pin, TRUE);
    gtk_box_pack_start(GTK_BOX(body), w->adding_pin, FALSE, FALSE, 12);
    return page;
}

static GtkWidget *finish_page(bt_wizard *w) {
    GtkWidget *body;
    GtkWidget *page = outer_page("Completing the Add Bluetooth Device Wizard", &body);
    w->finish_text = text("");
    gtk_label_set_max_width_chars(GTK_LABEL(w->finish_text), 46); /* beside the watermark */
    gtk_box_pack_start(GTK_BOX(body), w->finish_text, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(body), text("To close this wizard, click Finish."), FALSE, FALSE, 0);
    return page;
}

static void on_destroy([[maybe_unused]] GtkWidget *win, gpointer data) {
    bt_wizard *w = data;
    bt_app *a = w->app;
    if (w->busy && w->device != nullptr && a->bus != nullptr) {
        /* Closed while pairing: stop it. */
        GVariant *r = g_dbus_connection_call_sync(a->bus, BLUEZ, w->device, IFACE_DEVICE,
                                                  "CancelPairing", nullptr, nullptr,
                                                  G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
        if (r != nullptr)
            g_variant_unref(r);
    }
    g_cancellable_cancel(w->cancel_op);
    g_object_unref(w->cancel_op);
    stop_discovery(w);
    if (a->objects != nullptr)
        g_signal_handlers_disconnect_by_data(a->objects, w);
    g_clear_pointer(&a->pin, g_free);
    g_clear_pointer(&a->pairing_path, g_free);
    g_hash_table_unref(w->seen);
    g_free(w->adapter);
    g_free(w->device);
    g_free(w);
    a->wizard = nullptr;
    if ((a->quit_with_wizard || a->window == nullptr) && gtk_main_level() > 0)
        gtk_main_quit();
}

void bt_wizard_open(bt_app *a) {
    if (a->wizard != nullptr) {
        gtk_window_present(GTK_WINDOW(a->wizard->window));
        return;
    }
    bt_wizard *w = g_new0(bt_wizard, 1);
    w->app = a;
    a->wizard = w;
    w->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    w->cancel_op = g_cancellable_new();
    w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w->window), WIZARD_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(w->window), "bluetooth");
    gtk_window_set_resizable(GTK_WINDOW(w->window), FALSE);
    /* A dialog, as the property sheet is: GTK's Wayland backend makes a
     * parentless dialog (the sheet) the child of the next normal window
     * mapped, which would loop with the wizard's own parent. */
    gtk_window_set_type_hint(GTK_WINDOW(w->window), GDK_WINDOW_TYPE_HINT_DIALOG);
    if (a->window != nullptr && gtk_widget_get_visible(a->window))
        gtk_window_set_transient_for(GTK_WINDOW(w->window), GTK_WINDOW(a->window));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    w->stack = gtk_stack_new();
    gtk_widget_set_size_request(w->stack, 500, 320);
    GtkWidget *pages[] = {welcome_page(w), search_page(w), passkey_page(w), adding_page(w),
                          finish_page(w)};
    for (size_t i = 0; i < XD_ARRAY_LEN(pages); i++) {
        char name[8];
        g_snprintf(name, sizeof(name), "%zu", i);
        gtk_stack_add_named(GTK_STACK(w->stack), pages[i], name);
    }
    gtk_box_pack_start(GTK_BOX(box), w->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(buttons), 10);
    w->back = gtk_button_new_with_mnemonic("< _Back");
    w->next = gtk_button_new_with_mnemonic("_Next >");
    w->cancel = gtk_button_new_with_mnemonic("Cancel");
    GtkWidget *bs[] = {w->back, w->next, w->cancel};
    for (size_t i = 0; i < XD_ARRAY_LEN(bs); i++)
        gtk_widget_set_size_request(bs[i], 75, -1);
    gtk_widget_set_margin_start(w->cancel, 8);
    gtk_box_pack_end(GTK_BOX(buttons), w->cancel, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), w->next, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), w->back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(w->window), box);
    gtk_widget_set_can_default(w->next, TRUE);
    gtk_window_set_default(GTK_WINDOW(w->window), w->next);

    g_signal_connect(w->next, "clicked", G_CALLBACK(on_next), w);
    g_signal_connect(w->back, "clicked", G_CALLBACK(on_back), w);
    g_signal_connect(w->cancel, "clicked", G_CALLBACK(on_cancel), w);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_destroy), w);
    if (a->objects != nullptr) {
        g_signal_connect(a->objects, "interface-proxy-properties-changed",
                         G_CALLBACK(props_changed), w);
        g_signal_connect(a->objects, "object-added", G_CALLBACK(added), w);
        g_signal_connect(a->objects, "object-removed", G_CALLBACK(removed), w);
    }
    gtk_widget_show_all(w->window);
    show_page(w, PAGE_WELCOME);
}
