/* SPDX-License-Identifier: MIT
 * The pairing agent (org.bluez.Agent1): BlueZ calls it when pairing
 * needs the user, and it answers through XP-style dialogs:
 *
 *   RequestPinCode, RequestPasskey   "Type the passkey for the device"
 *                                    (the wizard's own passkey, if it chose one)
 *   DisplayPinCode, DisplayPasskey   "Type this passkey on the device"
 *   RequestConfirmation              "Does the device show this number?"
 *   RequestAuthorization,            "Allow the device to pair / to use
 *   AuthorizeService                  this service?"
 *
 * Questions are answered asynchronously, so the D-Bus call stays pending
 * while its dialog is up; BlueZ's Cancel closes it. Only BlueZ's own
 * connection may call us.
 */
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "bt.h"

#define AGENT_PATH "/org/xeondesktop/bluetooth/agent"
#define ERROR_REJECTED "org.bluez.Error.Rejected"
#define ERROR_CANCELED "org.bluez.Error.Canceled"

static const char agent_xml[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestPinCode'>"
    "      <arg type='o' direction='in'/><arg type='s' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPinCode'>"
    "      <arg type='o' direction='in'/><arg type='s' direction='in'/>"
    "    </method>"
    "    <method name='RequestPasskey'>"
    "      <arg type='o' direction='in'/><arg type='u' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPasskey'>"
    "      <arg type='o' direction='in'/><arg type='u' direction='in'/>"
    "      <arg type='q' direction='in'/>"
    "    </method>"
    "    <method name='RequestConfirmation'>"
    "      <arg type='o' direction='in'/><arg type='u' direction='in'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg type='o' direction='in'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg type='o' direction='in'/><arg type='s' direction='in'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

typedef enum request_kind : uint8_t {
    REQ_PIN,     /* answer: a PIN of up to 16 characters */
    REQ_PASSKEY, /* answer: a number up to 999999 */
    REQ_CONFIRM, /* answer: yes or no */
} request_kind;

struct bt_request {
    bt_app *app;
    GDBusMethodInvocation *inv;
    GtkWidget *dialog, *entry, *ok;
    request_kind kind;
};

/* Dialogs go over the wizard, else the property sheet if it's up. */
static GtkWindow *parent_window(bt_app *a) {
    GtkWindow *w = bt_wizard_window(a);
    if (w == nullptr && a->window != nullptr && gtk_widget_get_visible(a->window))
        w = GTK_WINDOW(a->window);
    return w;
}

static void on_button(GtkButton *b, gpointer dialog) {
    gtk_dialog_response(GTK_DIALOG(dialog),
                        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "response")));
}

/* A message box that doesn't block: the Bluetooth icon, `markup` and
 * `extra` (may be nullptr) over a centred row of buttons whose responses
 * are their indexes. The first button is the default. */
static GtkWidget *agent_dialog(bt_app *a, const char *markup, GtkWidget *extra,
                               const char *const *labels, int n, GtkWidget **first) {
    GtkWidget *dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), BT_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "bluetooth");
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWindow *parent = parent_window(a);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
    /* It comes unasked (a device wants to pair): keep it in sight. */
    gtk_window_set_keep_above(GTK_WINDOW(dlg), TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *msg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *image = gtk_image_new_from_icon_name("bluetooth", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
    gtk_widget_set_valign(image, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(msg), image, FALSE, FALSE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(col), label, FALSE, FALSE, 0);
    if (extra != nullptr)
        gtk_box_pack_start(GTK_BOX(col), extra, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(msg), col, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), msg, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    for (int i = 0; i < n; i++) {
        GtkWidget *b = gtk_button_new_with_mnemonic(labels[i]);
        gtk_widget_set_size_request(b, 75, -1);
        gtk_widget_set_can_default(b, TRUE);
        g_object_set_data(G_OBJECT(b), "response", GINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(on_button), dlg);
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
        if (i == 0 && first != nullptr)
            *first = b;
    }
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    return dlg;
}

static char *device_name(bt_app *a, const char *path) {
    GDBusInterface *dev = bt_interface(a, path, IFACE_DEVICE);
    char *name = dev != nullptr ? bt_device_name(dev) : g_strdup("A Bluetooth device");
    if (dev != nullptr)
        g_object_unref(dev);
    return name;
}

/* ---- questions --------------------------------------------------------------- */

/* Ends the pending question: answers BlueZ with `error` (or the dialog's
 * answer when nullptr) and closes the dialog. */
static void finish(bt_request *r, const char *error) {
    bt_app *a = r->app;
    if (a->request == r)
        a->request = nullptr;
    if (error != nullptr) {
        g_dbus_method_invocation_return_dbus_error(r->inv, error, "Not accepted");
    } else if (r->kind == REQ_PIN) {
        g_dbus_method_invocation_return_value(
            r->inv, g_variant_new("(s)", gtk_entry_get_text(GTK_ENTRY(r->entry))));
    } else if (r->kind == REQ_PASSKEY) {
        guint64 n = 0;
        g_ascii_string_to_unsigned(gtk_entry_get_text(GTK_ENTRY(r->entry)), 10, 0, 999999, &n,
                                   nullptr);
        g_dbus_method_invocation_return_value(r->inv, g_variant_new("(u)", (uint32_t)n));
    } else {
        g_dbus_method_invocation_return_value(r->inv, nullptr);
    }
    g_signal_handlers_disconnect_by_data(r->dialog, r);
    gtk_widget_destroy(r->dialog);
    g_free(r);
}

static void on_answer([[maybe_unused]] GtkDialog *d, int response, gpointer data) {
    /* Button 0 is OK or Yes; anything else (Escape, closing) says no. */
    finish(data, response == 0 ? nullptr : ERROR_REJECTED);
}

/* OK only for a valid answer: a PIN of 1 to 16 characters, a passkey of
 * up to six digits. */
static void on_entry_changed(GtkEditable *e, gpointer data) {
    bt_request *r = data;
    const char *text = gtk_entry_get_text(GTK_ENTRY(e));
    bool ok = text[0] != '\0';
    if (r->kind == REQ_PASSKEY)
        ok = ok && g_ascii_string_to_unsigned(text, 10, 0, 999999, nullptr, nullptr);
    gtk_widget_set_sensitive(r->ok, ok);
}

static void on_entry_activate([[maybe_unused]] GtkEntry *e, gpointer data) {
    bt_request *r = data;
    if (gtk_widget_get_sensitive(r->ok))
        finish(r, nullptr);
}

/* Asks; BlueZ's call stays pending until the user answers. */
static void ask(bt_app *a, GDBusMethodInvocation *inv, request_kind kind, const char *markup) {
    if (a->request != nullptr)
        finish(a->request, ERROR_CANCELED); /* BlueZ asks one thing at a time */
    bt_request *r = g_new0(bt_request, 1);
    r->app = a;
    r->inv = inv;
    r->kind = kind;
    static const char *const ok_cancel[] = {"OK", "Cancel"};
    static const char *const yes_no[] = {"_Yes", "_No"};
    if (kind == REQ_CONFIRM) {
        r->dialog = agent_dialog(a, markup, nullptr, yes_no, 2, &r->ok);
    } else {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new_with_mnemonic("_Passkey:");
        r->entry = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(r->entry), kind == REQ_PASSKEY ? 6 : 16);
        gtk_entry_set_input_purpose(GTK_ENTRY(r->entry), kind == REQ_PASSKEY
                                                             ? GTK_INPUT_PURPOSE_DIGITS
                                                             : GTK_INPUT_PURPOSE_FREE_FORM);
        gtk_label_set_mnemonic_widget(GTK_LABEL(label), r->entry);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), r->entry, TRUE, TRUE, 0);
        r->dialog = agent_dialog(a, markup, row, ok_cancel, 2, &r->ok);
        gtk_widget_set_sensitive(r->ok, FALSE);
        g_signal_connect(r->entry, "changed", G_CALLBACK(on_entry_changed), r);
        g_signal_connect(r->entry, "activate", G_CALLBACK(on_entry_activate), r);
        gtk_widget_grab_focus(r->entry);
    }
    if (kind == REQ_CONFIRM) {
        gtk_widget_grab_default(r->ok);
        gtk_widget_grab_focus(r->ok);
    }
    g_signal_connect(r->dialog, "response", G_CALLBACK(on_answer), r);
    a->request = r;
}

/* ---- shown passkeys ----------------------------------------------------------- */

static void close_display(bt_app *a) {
    if (a->display != nullptr)
        gtk_widget_destroy(a->display);
}

void bt_agent_pairing_done(bt_app *a) { close_display(a); }

static void cancel_done(GObject *src, GAsyncResult *res, [[maybe_unused]] gpointer data) {
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    if (r != nullptr)
        g_variant_unref(r);
}

static void on_display_response(GtkDialog *d, [[maybe_unused]] int response, gpointer data) {
    bt_app *a = data;
    /* Cancel: stop the pairing that's waiting for the passkey. */
    const char *path = g_object_get_data(G_OBJECT(d), "device");
    if (path != nullptr && a->bus != nullptr)
        g_dbus_connection_call(a->bus, BLUEZ, path, IFACE_DEVICE, "CancelPairing", nullptr, nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, cancel_done, nullptr);
    gtk_widget_destroy(GTK_WIDGET(d));
}

/* `code`: what to type on the device; `typed`: how many digits it says
 * have been typed (DisplayPasskey's "entered"), or -1. */
static void display(bt_app *a, const char *path, const char *code, int typed) {
    close_display(a);
    char *name = device_name(a, path);
    char *progress = typed > 0 ? g_strdup_printf("\n\n%d of %zu digits typed.", typed, strlen(code))
                               : g_strdup("");
    char *markup = g_markup_printf_escaped(
        "To pair with <b>%s</b>, type this passkey on the device, then press Enter on it "
        "if it has an Enter key:\n\n<span size='xx-large' weight='bold'>%s</span>%s",
        name, code, progress);
    static const char *const cancel[] = {"Cancel"};
    a->display = agent_dialog(a, markup, nullptr, cancel, 1, nullptr);
    g_object_set_data_full(G_OBJECT(a->display), "device", g_strdup(path), g_free);
    g_object_add_weak_pointer(G_OBJECT(a->display), (gpointer *)&a->display);
    g_signal_connect(a->display, "response", G_CALLBACK(on_display_response), a);
    g_free(markup);
    g_free(progress);
    g_free(name);
}

/* ---- the D-Bus object -------------------------------------------------------------- */

/* The wizard's passkey, when it's pairing `path`. */
static const char *wizard_pin(bt_app *a, const char *path) {
    return a->pin != nullptr && g_strcmp0(path, a->pairing_path) == 0 ? a->pin : nullptr;
}

static void on_call([[maybe_unused]] GDBusConnection *c, const char *sender,
                    [[maybe_unused]] const char *object, [[maybe_unused]] const char *iface,
                    const char *method, GVariant *params, GDBusMethodInvocation *inv,
                    gpointer data) {
    bt_app *a = data;
    /* Only bluetoothd may ask us for passkeys. */
    char *owner =
        g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(a->objects));
    bool from_bluez = owner != nullptr && g_strcmp0(sender, owner) == 0;
    g_free(owner);
    if (!from_bluez) {
        g_dbus_method_invocation_return_dbus_error(inv, ERROR_REJECTED, "Not BlueZ");
        return;
    }
    const char *path = nullptr;
    if (g_variant_n_children(params) > 0)
        g_variant_get_child(params, 0, "&o", &path);
    char *name = path != nullptr ? device_name(a, path) : nullptr;
    char *markup = nullptr;

    if (strcmp(method, "Release") == 0) {
        a->agent_registered = false;
        g_dbus_method_invocation_return_value(inv, nullptr);
    } else if (strcmp(method, "Cancel") == 0) {
        if (a->request != nullptr)
            finish(a->request, ERROR_CANCELED);
        close_display(a);
        g_dbus_method_invocation_return_value(inv, nullptr);
    } else if (strcmp(method, "RequestPinCode") == 0) {
        const char *pin = wizard_pin(a, path);
        if (pin != nullptr) {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", pin));
        } else {
            markup = g_markup_printf_escaped(
                "<b>%s</b> needs a passkey.\n\nType the passkey for the device. You can find "
                "it in the device's documentation, or choose your own and type it on the "
                "device too.",
                name);
            ask(a, inv, REQ_PIN, markup);
        }
    } else if (strcmp(method, "RequestPasskey") == 0) {
        const char *pin = wizard_pin(a, path);
        guint64 n = 0;
        if (pin != nullptr && g_ascii_string_to_unsigned(pin, 10, 0, 999999, &n, nullptr)) {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", (uint32_t)n));
        } else {
            markup = g_markup_printf_escaped(
                "<b>%s</b> needs a passkey.\n\nType the number the device shows.", name);
            ask(a, inv, REQ_PASSKEY, markup);
        }
    } else if (strcmp(method, "DisplayPinCode") == 0) {
        const char *pin = nullptr;
        g_variant_get(params, "(&o&s)", nullptr, &pin);
        display(a, path, pin, -1);
        g_dbus_method_invocation_return_value(inv, nullptr);
    } else if (strcmp(method, "DisplayPasskey") == 0) {
        uint32_t passkey = 0;
        uint16_t entered = 0;
        g_variant_get(params, "(&ouq)", nullptr, &passkey, &entered);
        char code[8];
        g_snprintf(code, sizeof(code), "%06u", passkey);
        display(a, path, code, entered);
        g_dbus_method_invocation_return_value(inv, nullptr);
    } else if (strcmp(method, "RequestConfirmation") == 0) {
        uint32_t passkey = 0;
        g_variant_get(params, "(&ou)", nullptr, &passkey);
        markup = g_markup_printf_escaped(
            "Does <b>%s</b> show this passkey?\n\n<span size='xx-large' "
            "weight='bold'>%06u</span>\n\nClick Yes only if the numbers match.",
            name, passkey);
        ask(a, inv, REQ_CONFIRM, markup);
    } else if (strcmp(method, "RequestAuthorization") == 0) {
        markup = g_markup_printf_escaped(
            "<b>%s</b> wants to pair with this computer.\n\nDo you want to allow this?", name);
        ask(a, inv, REQ_CONFIRM, markup);
    } else if (strcmp(method, "AuthorizeService") == 0) {
        const char *uuid = nullptr;
        g_variant_get(params, "(&o&s)", nullptr, &uuid);
        const char *service = xd_bt_uuid_name(uuid);
        markup = g_markup_printf_escaped(
            "<b>%s</b> wants to use this computer's <b>%s</b> service.\n\nDo you want to "
            "allow this?",
            name, service != nullptr ? service : uuid);
        ask(a, inv, REQ_CONFIRM, markup);
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod",
                                                   method);
    }
    g_free(markup);
    g_free(name);
}

static const GDBusInterfaceVTable vtable = {.method_call = on_call};

static void register_done(GObject *src, GAsyncResult *res, gpointer data) {
    bt_app *a = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        g_printerr("xd-bluetooth: can't be the pairing agent: %s\n", error->message);
        g_error_free(error);
        return;
    }
    g_variant_unref(r);
    a->agent_registered = true;
    /* Ours while Bluetooth Devices is open, as the device being added
     * may ask the default agent. */
    g_dbus_connection_call(a->bus, BLUEZ, "/org/bluez", "org.bluez.AgentManager1",
                           "RequestDefaultAgent", g_variant_new("(o)", AGENT_PATH), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, cancel_done, nullptr);
}

static void register_agent(bt_app *a) {
    char *owner =
        g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(a->objects));
    if (owner != nullptr)
        g_dbus_connection_call(a->bus, BLUEZ, "/org/bluez", "org.bluez.AgentManager1",
                               "RegisterAgent",
                               g_variant_new("(os)", AGENT_PATH, "KeyboardDisplay"), nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, register_done, a);
    else
        a->agent_registered = false;
    g_free(owner);
}

/* bluetoothd (re)started: it forgot us. */
static void on_owner([[maybe_unused]] GObject *m, [[maybe_unused]] GParamSpec *ps, gpointer data) {
    register_agent(data);
}

void bt_agent_start(bt_app *a) {
    GError *error = nullptr;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(agent_xml, &error);
    if (info == nullptr) {
        g_printerr("xd-bluetooth: %s\n", error->message);
        g_error_free(error);
        return;
    }
    a->agent_id = g_dbus_connection_register_object(a->bus, AGENT_PATH, info->interfaces[0],
                                                    &vtable, a, nullptr, &error);
    g_dbus_node_info_unref(info);
    if (a->agent_id == 0) {
        g_printerr("xd-bluetooth: %s\n", error->message);
        g_error_free(error);
        return;
    }
    g_signal_connect(a->objects, "notify::name-owner", G_CALLBACK(on_owner), a);
    register_agent(a);
}

void bt_agent_stop(bt_app *a) {
    if (a->request != nullptr)
        finish(a->request, ERROR_CANCELED);
    close_display(a);
    if (a->agent_registered) {
        GVariant *r =
            g_dbus_connection_call_sync(a->bus, BLUEZ, "/org/bluez", "org.bluez.AgentManager1",
                                        "UnregisterAgent", g_variant_new("(o)", AGENT_PATH),
                                        nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
        if (r != nullptr)
            g_variant_unref(r);
        a->agent_registered = false;
    }
    if (a->agent_id != 0)
        g_dbus_connection_unregister_object(a->bus, a->agent_id);
    a->agent_id = 0;
}
