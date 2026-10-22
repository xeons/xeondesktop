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
    guint refill;     /* the list redone shortly (fill_found) */
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

GtkWindow *bt_wizard_window(bt_app *state) {
    return state->wizard != nullptr ? GTK_WINDOW(state->wizard->window) : nullptr;
}

/* ---- look --------------------------------------------------------------------- */

/* The welcome and completing pages' watermark: XP's blue band. */
static gboolean draw_watermark(GtkWidget *widget, cairo_t *cairo, [[maybe_unused]] gpointer data) {
    double width = gtk_widget_get_allocated_width(widget),
           height = gtk_widget_get_allocated_height(widget);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, 0, 0, height);
    cairo_pattern_add_color_stop_rgb(pattern, 0, 0.35, 0.53, 0.89);
    cairo_pattern_add_color_stop_rgb(pattern, 1, 0.05, 0.20, 0.55);
    cairo_rectangle(cairo, 0, 0, width, height);
    cairo_set_source(cairo, pattern);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
    GtkIconTheme *theme = gtk_icon_theme_get_for_screen(gtk_widget_get_screen(widget));
    int scale = gtk_widget_get_scale_factor(widget);
    cairo_surface_t *icon =
        gtk_icon_theme_load_surface(theme, "bluetooth", 64, scale, gtk_widget_get_window(widget),
                                    GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
    if (icon != nullptr) {
        cairo_set_source_surface(cairo, icon, (width - 64) / 2, 24);
        cairo_paint(cairo);
        cairo_surface_destroy(icon);
    }
    return FALSE;
}

/* A white header band (the window colour) behind an interior page's title. */
static gboolean draw_header(GtkWidget *widget, cairo_t *cairo, [[maybe_unused]] gpointer data) {
    GdkRGBA base = {1, 1, 1, 1};
    GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
    gtk_style_context_lookup_color(ctx, "theme_base_color", &base);
    gdk_cairo_set_source_rgba(cairo, &base);
    cairo_paint(cairo);
    return FALSE;
}

static GtkWidget *text(const char *markup) {
    GtkWidget *label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 52);
    return label;
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

static char *selected_device(bt_wizard *device_wizard) {
    GtkTreeModel *model;
    GtkTreeIter it;
    char *path = nullptr;
    if (gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(device_wizard->found_view)), &model, &it))
        gtk_tree_model_get(model, &it, FOUND_PATH, &path, -1);
    return path;
}

static void update_buttons(bt_wizard *device_wizard);

/* Devices of our adapter that aren't paired and are in range (seen in
 * this search, or with a signal strength). */
static void fill_found(bt_wizard *device_wizard) {
    bt_app *state = device_wizard->app;
    char *keep = selected_device(device_wizard);
    gtk_list_store_clear(device_wizard->found);
    GList *objs =
        state->objects != nullptr ? g_dbus_object_manager_get_objects(state->objects) : nullptr;
    GtkTreeIter it;
    for (GList *list = objs; list != nullptr; list = list->next) {
        GDBusInterface *dev = g_dbus_object_get_interface(list->data, IFACE_DEVICE);
        if (dev == nullptr)
            continue;
        const char *path = g_dbus_object_get_object_path(list->data);
        char *adapter = bt_prop_s(dev, "Adapter");
        GVariant *rssi = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(dev), "RSSI");
        if (!bt_prop_b(dev, "Paired") && g_strcmp0(adapter, device_wizard->adapter) == 0 &&
            (rssi != nullptr || g_hash_table_contains(device_wizard->seen, path))) {
            char *name = bt_device_name(dev);
            xd_bt_type bt_type = bt_device_type(dev);
            char *markup = g_markup_printf_escaped("%s\n<small>%s</small>", name, bt_type.name);
            gtk_list_store_insert_with_values(device_wizard->found, &it, -1, FOUND_ICON,
                                              bt_type.icon, FOUND_TEXT, markup, FOUND_PATH, path,
                                              -1);
            if (g_strcmp0(keep, path) == 0)
                gtk_tree_selection_select_iter(
                    gtk_tree_view_get_selection(GTK_TREE_VIEW(device_wizard->found_view)), &it);
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

    GDBusInterface *ad = bt_interface(state, device_wizard->adapter, IFACE_ADAPTER);
    bool searching = ad != nullptr && bt_prop_b(ad, "Discovering");
    if (ad != nullptr)
        g_object_unref(ad);
    gtk_widget_set_visible(device_wizard->spinner, searching);
    if (searching)
        gtk_spinner_start(GTK_SPINNER(device_wizard->spinner));
    else
        gtk_spinner_stop(GTK_SPINNER(device_wizard->spinner));
    gtk_label_set_text(GTK_LABEL(device_wizard->search_status),
                       searching ? "Searching for Bluetooth devices..."
                                 : "If you don't see the device that you want to add, make sure "
                                   "that it is turned on and can be found, then click Search "
                                   "Again.");
    update_buttons(device_wizard);
}

static void stop_discovery(bt_wizard *device_wizard) {
    if (!device_wizard->discovering || device_wizard->adapter == nullptr)
        return;
    device_wizard->discovering = false;
    /* Synchronous: the wizard may be closing with the program. */
    GVariant *variant = g_dbus_connection_call_sync(
        device_wizard->app->bus, BLUEZ, device_wizard->adapter, IFACE_ADAPTER, "StopDiscovery",
        nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (variant != nullptr)
        g_variant_unref(variant);
}

static void discovery_started(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant != nullptr) {
        g_variant_unref(variant);
        return;
    }
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        bt_wizard *device_wizard = data;
        device_wizard->discovering = false;
        bt_error(GTK_WINDOW(device_wizard->window), "Searching for Bluetooth devices failed.",
                 error);
    }
    g_error_free(error);
}

static void start_discovery(bt_wizard *device_wizard) {
    GDBusInterface *ad = bt_adapter(device_wizard->app);
    g_free(device_wizard->adapter);
    device_wizard->adapter =
        ad != nullptr ? g_strdup(g_dbus_proxy_get_object_path(G_DBUS_PROXY(ad))) : nullptr;
    if (ad != nullptr)
        g_object_unref(ad);
    g_hash_table_remove_all(device_wizard->seen);
    if (device_wizard->adapter == nullptr) {
        gtk_label_set_text(GTK_LABEL(device_wizard->search_status),
                           "No Bluetooth radio was found.");
        return;
    }
    if (!device_wizard->discovering) { /* BlueZ counts each client's discovery */
        device_wizard->discovering = true;
        g_dbus_connection_call(device_wizard->app->bus, BLUEZ, device_wizard->adapter,
                               IFACE_ADAPTER, "StartDiscovery", nullptr, nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, device_wizard->cancel_op,
                               discovery_started, device_wizard);
    }
    fill_found(device_wizard);
}

static void on_search_again([[maybe_unused]] GtkButton *button, gpointer data) {
    bt_wizard *device_wizard = data;
    stop_discovery(device_wizard);
    start_discovery(device_wizard);
}

/* ---- adding ------------------------------------------------------------------------- */

static void show_page(bt_wizard *device_wizard, int page);

static void finish_with(bt_wizard *device_wizard, const char *message) {
    bt_app *state = device_wizard->app;
    g_clear_pointer(&state->pin, g_free);
    g_clear_pointer(&state->pairing_path, g_free);
    bt_agent_pairing_done(state);
    device_wizard->busy = false;
    gtk_label_set_markup(GTK_LABEL(device_wizard->finish_text), message);
    show_page(device_wizard, PAGE_FINISH);
}

static void connected(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return; /* the wizard is gone */
    }
    bt_wizard *device_wizard = data;
    if (variant != nullptr) {
        g_variant_unref(variant);
        finish_with(device_wizard,
                    "The Bluetooth device was successfully connected to your computer. "
                    "Your computer and the device can communicate whenever they are near "
                    "each other.\n\nTo see the device, or change its settings, use "
                    "Bluetooth Devices in Control Panel.");
    } else {
        g_dbus_error_strip_remote_error(error);
        char *msg = g_markup_printf_escaped(
            "The device was added to your computer, but it could not be connected:\n\n%s\n\n"
            "It can connect later, whenever it's near your computer.",
            error->message);
        finish_with(device_wizard, msg);
        g_free(msg);
        g_error_free(error);
    }
}

static void paired(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }
    bt_wizard *device_wizard = data;
    bt_app *state = device_wizard->app;
    char *remote = variant == nullptr ? g_dbus_error_get_remote_error(error) : nullptr;
    bool ok = variant != nullptr || g_strcmp0(remote, "org.bluez.Error.AlreadyExists") == 0;
    g_free(remote);
    if (variant != nullptr)
        g_variant_unref(variant);
    if (!ok) {
        g_dbus_error_strip_remote_error(error);
        char *msg = g_markup_printf_escaped(
            "The device could not be added:\n\n%s\n\nMake sure that the device is turned on "
            "and can be found, and that you typed the right passkey, then run the wizard "
            "again.",
            error->message);
        finish_with(device_wizard, msg);
        g_free(msg);
        g_error_free(error);
        return;
    }
    g_clear_error(&error);
    /* Trusted: it can connect again without asking. */
    bt_set_prop(state, GTK_WINDOW(device_wizard->window), device_wizard->device, IFACE_DEVICE,
                "Trusted", g_variant_new_boolean(TRUE));
    gtk_label_set_text(GTK_LABEL(device_wizard->adding_text), "Connecting to the device...");
    gtk_widget_hide(device_wizard->adding_pin);
    g_dbus_connection_call(state->bus, BLUEZ, device_wizard->device, IFACE_DEVICE, "Connect",
                           nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 60000,
                           device_wizard->cancel_op, connected, device_wizard);
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

static void start_adding(bt_wizard *device_wizard) {
    bt_app *state = device_wizard->app;
    g_free(device_wizard->device);
    device_wizard->device = selected_device(device_wizard);
    if (device_wizard->device == nullptr)
        return;
    g_clear_pointer(&state->pin, g_free);
    g_free(state->pairing_path);
    state->pairing_path = g_strdup(device_wizard->device);
    bool show_pin = false;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(device_wizard->pk_auto))) {
        state->pin = random_pin();
        show_pin = true;
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(device_wizard->pk_doc))) {
        state->pin = g_strdup(gtk_entry_get_text(GTK_ENTRY(device_wizard->pk_doc_entry)));
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(device_wizard->pk_own))) {
        state->pin = g_strdup(gtk_entry_get_text(GTK_ENTRY(device_wizard->pk_own_entry)));
        show_pin = true;
    }
    GDBusInterface *dev = bt_interface(state, device_wizard->device, IFACE_DEVICE);
    char *name = dev != nullptr ? bt_device_name(dev) : g_strdup("the device");
    if (dev != nullptr)
        g_object_unref(dev);
    char *msg = g_strdup_printf("Adding %s. Exchanging passkeys...", name);
    gtk_label_set_text(GTK_LABEL(device_wizard->adding_text), msg);
    g_free(msg);
    g_free(name);
    if (show_pin) {
        char *pin = g_markup_printf_escaped(
            "If the device asks for a passkey, type this one on it:\n\n<span size='xx-large' "
            "weight='bold'>%s</span>",
            state->pin);
        gtk_label_set_markup(GTK_LABEL(device_wizard->adding_pin), pin);
        g_free(pin);
        gtk_widget_show(device_wizard->adding_pin);
    } else {
        gtk_widget_hide(device_wizard->adding_pin);
    }
    device_wizard->busy = true;
    show_page(device_wizard, PAGE_ADDING);
    g_dbus_connection_call(state->bus, BLUEZ, device_wizard->device, IFACE_DEVICE, "Pair", nullptr,
                           nullptr, G_DBUS_CALL_FLAGS_NONE, 120000, device_wizard->cancel_op,
                           paired, device_wizard);
}

/* ---- pages and buttons ------------------------------------------------------------- */

static void update_buttons(bt_wizard *device_wizard) {
    bool next = true;
    switch (device_wizard->page) {
    case PAGE_WELCOME:
        next = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(device_wizard->ready));
        break;
    case PAGE_SEARCH: {
        char *path = selected_device(device_wizard);
        next = path != nullptr;
        g_free(path);
        break;
    }
    case PAGE_PASSKEY:
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(device_wizard->pk_doc)))
            next = gtk_entry_get_text_length(GTK_ENTRY(device_wizard->pk_doc_entry)) > 0;
        else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(device_wizard->pk_own)))
            next = gtk_entry_get_text_length(GTK_ENTRY(device_wizard->pk_own_entry)) > 0;
        break;
    case PAGE_ADDING:
        next = false;
        break;
    default:
        break;
    }
    gtk_widget_set_sensitive(device_wizard->next, next);
    gtk_widget_set_sensitive(device_wizard->back, device_wizard->page != PAGE_WELCOME &&
                                                      device_wizard->page < PAGE_ADDING);
    gtk_widget_set_sensitive(device_wizard->cancel, device_wizard->page != PAGE_FINISH);
    gtk_button_set_label(GTK_BUTTON(device_wizard->next),
                         device_wizard->page == PAGE_FINISH ? "Finish" : "_Next >");
    gtk_widget_set_sensitive(
        device_wizard->pk_doc_entry,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(device_wizard->pk_doc)));
    gtk_widget_set_sensitive(
        device_wizard->pk_own_entry,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(device_wizard->pk_own)));
}

static void show_page(bt_wizard *device_wizard, int page) {
    if (device_wizard->page == PAGE_SEARCH && page != PAGE_SEARCH)
        stop_discovery(device_wizard); /* BlueZ pairs better without it */
    device_wizard->page = page;
    char name[8];
    g_snprintf(name, sizeof(name), "%d", page);
    gtk_stack_set_visible_child_name(GTK_STACK(device_wizard->stack), name);
    if (page == PAGE_SEARCH)
        start_discovery(device_wizard);
    if (page == PAGE_ADDING)
        gtk_spinner_start(GTK_SPINNER(device_wizard->adding_spinner));
    else
        gtk_spinner_stop(GTK_SPINNER(device_wizard->adding_spinner));
    update_buttons(device_wizard);
    gtk_widget_grab_focus(device_wizard->next);
}

static void on_next([[maybe_unused]] GtkButton *button, gpointer data) {
    bt_wizard *device_wizard = data;
    if (device_wizard->page == PAGE_FINISH)
        gtk_widget_destroy(device_wizard->window);
    else if (device_wizard->page == PAGE_PASSKEY)
        start_adding(device_wizard);
    else
        show_page(device_wizard, device_wizard->page + 1);
}

static void on_back([[maybe_unused]] GtkButton *button, gpointer data) {
    bt_wizard *device_wizard = data;
    if (device_wizard->page > PAGE_WELCOME && device_wizard->page < PAGE_ADDING)
        show_page(device_wizard, device_wizard->page - 1);
}

static void on_cancel([[maybe_unused]] GtkButton *button, gpointer data) {
    bt_wizard *device_wizard = data;
    gtk_widget_destroy(device_wizard->window);
}

static void on_changed([[maybe_unused]] GtkWidget *widget, gpointer data) { update_buttons(data); }

static void on_found_activated([[maybe_unused]] GtkTreeView *tree_view,
                               [[maybe_unused]] GtkTreePath *path,
                               [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    bt_wizard *device_wizard = data;
    if (gtk_widget_get_sensitive(device_wizard->next))
        on_next(nullptr, device_wizard);
}

/* ---- live updates ------------------------------------------------------------------- */

static gboolean refill_now(gpointer data) {
    bt_wizard *device_wizard = data;
    device_wizard->refill = 0;
    if (device_wizard->page == PAGE_SEARCH)
        fill_found(device_wizard);
    return G_SOURCE_REMOVE;
}

/* While searching, every device in range reports its signal strength
 * over and over: the list is redone at most a few times a second. */
static void refill_soon(bt_wizard *device_wizard) {
    if (device_wizard->page == PAGE_SEARCH && device_wizard->refill == 0)
        device_wizard->refill = g_timeout_add(250, refill_now, device_wizard);
}

static void props_changed([[maybe_unused]] GDBusObjectManagerClient *dbus_object_manager_client,
                          [[maybe_unused]] GDBusObjectProxy *object,
                          [[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *variant,
                          [[maybe_unused]] const char *const *inv, gpointer data) {
    refill_soon(data);
}

static void added([[maybe_unused]] GDBusObjectManager *dbus_object_manager, GDBusObject *object,
                  gpointer data) {
    bt_wizard *device_wizard = data;
    if (device_wizard->page == PAGE_SEARCH) {
        g_hash_table_add(device_wizard->seen, g_strdup(g_dbus_object_get_object_path(object)));
        refill_soon(device_wizard);
    }
}

static void removed([[maybe_unused]] GDBusObjectManager *dbus_object_manager,
                    [[maybe_unused]] GDBusObject *object, gpointer data) {
    refill_soon(data);
}

/* ---- building ---------------------------------------------------------------------- */

static GtkWidget *welcome_page(bt_wizard *device_wizard) {
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
    device_wizard->ready =
        gtk_check_button_new_with_mnemonic("_My device is set up and ready to be found.");
    g_signal_connect(device_wizard->ready, "toggled", G_CALLBACK(on_changed), device_wizard);
    gtk_box_pack_start(GTK_BOX(body), device_wizard->ready, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(body), text("Only add Bluetooth devices that you trust."), FALSE,
                     FALSE, 0);
    return page;
}

static GtkWidget *search_page(bt_wizard *device_wizard) {
    GtkWidget *body;
    GtkWidget *page =
        inner_page(WIZARD_TITLE, "Select the Bluetooth device that you want to add.", &body);
    device_wizard->found =
        gtk_list_store_new(FOUND_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    device_wizard->found_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(device_wizard->found));
    g_object_unref(device_wizard->found);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(device_wizard->found_view), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "stock-size", GTK_ICON_SIZE_DND, "xpad", 6, "ypad", 3, nullptr);
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", FOUND_ICON);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_add_attribute(col, renderer, "markup", FOUND_TEXT);
    gtk_tree_view_append_column(GTK_TREE_VIEW(device_wizard->found_view), col);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), device_wizard->found_view);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(device_wizard->found_view)),
                     "changed", G_CALLBACK(on_changed), device_wizard);
    g_signal_connect(device_wizard->found_view, "row-activated", G_CALLBACK(on_found_activated),
                     device_wizard);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    device_wizard->spinner = gtk_spinner_new();
    gtk_widget_set_valign(device_wizard->spinner, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(device_wizard->spinner, TRUE); /* shown while searching */
    device_wizard->search_status = text("");
    GtkWidget *again = gtk_button_new_with_mnemonic("_Search Again");
    gtk_widget_set_valign(again, GTK_ALIGN_START);
    g_signal_connect(again, "clicked", G_CALLBACK(on_search_again), device_wizard);
    gtk_box_pack_start(GTK_BOX(row), device_wizard->spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), device_wizard->search_status, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row), again, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
    return page;
}

static GtkWidget *passkey_entry(bt_wizard *device_wizard, GtkWidget *grid, int row,
                                GtkWidget *radio) {
    GtkWidget *entry_widget = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry_widget), 16); /* a PIN's bytes */
    gtk_entry_set_width_chars(GTK_ENTRY(entry_widget), 18);
    gtk_grid_attach(GTK_GRID(grid), radio, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_widget, 1, row, 1, 1);
    g_signal_connect(entry_widget, "changed", G_CALLBACK(on_changed), device_wizard);
    return entry_widget;
}

static GtkWidget *passkey_page(bt_wizard *device_wizard) {
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
    device_wizard->pk_auto =
        gtk_radio_button_new_with_mnemonic(nullptr, "_Choose a passkey for me");
    gtk_grid_attach(GTK_GRID(grid), device_wizard->pk_auto, 0, 0, 2, 1);
    device_wizard->pk_doc = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(device_wizard->pk_auto), "_Use the passkey found in the documentation:");
    device_wizard->pk_doc_entry = passkey_entry(device_wizard, grid, 1, device_wizard->pk_doc);
    device_wizard->pk_own = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(device_wizard->pk_auto), "_Let me choose my own passkey:");
    device_wizard->pk_own_entry = passkey_entry(device_wizard, grid, 2, device_wizard->pk_own);
    device_wizard->pk_none = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(device_wizard->pk_auto), "_Don't use a passkey");
    gtk_grid_attach(GTK_GRID(grid), device_wizard->pk_none, 0, 3, 2, 1);
    GtkWidget *radios[] = {device_wizard->pk_auto, device_wizard->pk_doc, device_wizard->pk_own,
                           device_wizard->pk_none};
    for (size_t i = 0; i < XD_ARRAY_LEN(radios); i++)
        g_signal_connect(radios[i], "toggled", G_CALLBACK(on_changed), device_wizard);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(body),
                     text("<small>Devices made for Bluetooth 2.1 and later choose for "
                          "themselves: they show a number for you to compare, or you type one "
                          "this computer shows.</small>"),
                     FALSE, FALSE, 0);
    return page;
}

static GtkWidget *adding_page(bt_wizard *device_wizard) {
    GtkWidget *body;
    GtkWidget *page = inner_page(WIZARD_TITLE, "Please wait while the device is added.", &body);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    device_wizard->adding_spinner = gtk_spinner_new();
    device_wizard->adding_text = text("");
    gtk_box_pack_start(GTK_BOX(row), device_wizard->adding_spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), device_wizard->adding_text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
    device_wizard->adding_pin = text("");
    gtk_widget_set_no_show_all(device_wizard->adding_pin, TRUE);
    gtk_box_pack_start(GTK_BOX(body), device_wizard->adding_pin, FALSE, FALSE, 12);
    return page;
}

static GtkWidget *finish_page(bt_wizard *device_wizard) {
    GtkWidget *body;
    GtkWidget *page = outer_page("Completing the Add Bluetooth Device Wizard", &body);
    device_wizard->finish_text = text("");
    gtk_label_set_max_width_chars(GTK_LABEL(device_wizard->finish_text),
                                  46); /* beside the watermark */
    gtk_box_pack_start(GTK_BOX(body), device_wizard->finish_text, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(body), text("To close this wizard, click Finish."), FALSE, FALSE, 0);
    return page;
}

static void on_destroy([[maybe_unused]] GtkWidget *win, gpointer data) {
    bt_wizard *device_wizard = data;
    bt_app *state = device_wizard->app;
    if (device_wizard->busy && device_wizard->device != nullptr && state->bus != nullptr) {
        /* Closed while pairing: stop it. */
        GVariant *variant = g_dbus_connection_call_sync(
            state->bus, BLUEZ, device_wizard->device, IFACE_DEVICE, "CancelPairing", nullptr,
            nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
        if (variant != nullptr)
            g_variant_unref(variant);
    }
    g_cancellable_cancel(device_wizard->cancel_op);
    g_object_unref(device_wizard->cancel_op);
    if (device_wizard->refill != 0)
        g_source_remove(device_wizard->refill);
    stop_discovery(device_wizard);
    if (state->objects != nullptr)
        g_signal_handlers_disconnect_by_data(state->objects, device_wizard);
    g_clear_pointer(&state->pin, g_free);
    g_clear_pointer(&state->pairing_path, g_free);
    g_hash_table_unref(device_wizard->seen);
    g_free(device_wizard->adapter);
    g_free(device_wizard->device);
    g_free(device_wizard);
    state->wizard = nullptr;
    if ((state->quit_with_wizard || state->window == nullptr) && gtk_main_level() > 0)
        gtk_main_quit();
}

void bt_wizard_open(bt_app *state) {
    if (state->wizard != nullptr) {
        gtk_window_present(GTK_WINDOW(state->wizard->window));
        return;
    }
    bt_wizard *device_wizard = g_new0(bt_wizard, 1);
    device_wizard->app = state;
    state->wizard = device_wizard;
    device_wizard->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    device_wizard->cancel_op = g_cancellable_new();
    device_wizard->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(device_wizard->window), WIZARD_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(device_wizard->window), "bluetooth");
    gtk_window_set_resizable(GTK_WINDOW(device_wizard->window), FALSE);
    /* A dialog, as the property sheet is: GTK's Wayland backend makes a
     * parentless dialog (the sheet) the child of the next normal window
     * mapped, which would loop with the wizard's own parent. */
    gtk_window_set_type_hint(GTK_WINDOW(device_wizard->window), GDK_WINDOW_TYPE_HINT_DIALOG);
    if (state->window != nullptr && gtk_widget_get_visible(state->window))
        gtk_window_set_transient_for(GTK_WINDOW(device_wizard->window), GTK_WINDOW(state->window));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    device_wizard->stack = gtk_stack_new();
    gtk_widget_set_size_request(device_wizard->stack, 500, 320);
    GtkWidget *pages[] = {welcome_page(device_wizard), search_page(device_wizard),
                          passkey_page(device_wizard), adding_page(device_wizard),
                          finish_page(device_wizard)};
    for (size_t i = 0; i < XD_ARRAY_LEN(pages); i++) {
        char name[8];
        g_snprintf(name, sizeof(name), "%zu", i);
        gtk_stack_add_named(GTK_STACK(device_wizard->stack), pages[i], name);
    }
    gtk_box_pack_start(GTK_BOX(box), device_wizard->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(buttons), 10);
    device_wizard->back = gtk_button_new_with_mnemonic("< _Back");
    device_wizard->next = gtk_button_new_with_mnemonic("_Next >");
    device_wizard->cancel = gtk_button_new_with_mnemonic("Cancel");
    GtkWidget *bs[] = {device_wizard->back, device_wizard->next, device_wizard->cancel};
    for (size_t i = 0; i < XD_ARRAY_LEN(bs); i++)
        gtk_widget_set_size_request(bs[i], 75, -1);
    gtk_widget_set_margin_start(device_wizard->cancel, 8);
    gtk_box_pack_end(GTK_BOX(buttons), device_wizard->cancel, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), device_wizard->next, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), device_wizard->back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(device_wizard->window), box);
    gtk_widget_set_can_default(device_wizard->next, TRUE);
    gtk_window_set_default(GTK_WINDOW(device_wizard->window), device_wizard->next);

    g_signal_connect(device_wizard->next, "clicked", G_CALLBACK(on_next), device_wizard);
    g_signal_connect(device_wizard->back, "clicked", G_CALLBACK(on_back), device_wizard);
    g_signal_connect(device_wizard->cancel, "clicked", G_CALLBACK(on_cancel), device_wizard);
    g_signal_connect(device_wizard->window, "destroy", G_CALLBACK(on_destroy), device_wizard);
    if (state->objects != nullptr) {
        g_signal_connect(state->objects, "interface-proxy-properties-changed",
                         G_CALLBACK(props_changed), device_wizard);
        g_signal_connect(state->objects, "object-added", G_CALLBACK(added), device_wizard);
        g_signal_connect(state->objects, "object-removed", G_CALLBACK(removed), device_wizard);
    }
    gtk_widget_show_all(device_wizard->window);
    show_page(device_wizard, PAGE_WELCOME);
}
