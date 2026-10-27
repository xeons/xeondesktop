/* SPDX-License-Identifier: MIT
 * xdg-desktop-portal-xeondesktop: XeonDesktop's xdg-desktop-portal backend.
 * It serves org.freedesktop.impl.portal.FileChooser, so that programs
 * asking the portal for a file (Flatpaks, GTK 4, Firefox, Chromium, GTK 3's
 * GtkFileChooserNative) get our Open and Save As dialogs (chooser.c).
 *
 * The interfaces are xdg-desktop-portal's own introspection files, built
 * in as a resource. Each call exports an org.freedesktop.impl.portal.Request
 * at its handle, whose Close() closes that call's dialog.
 *
 *   xdg-desktop-portal-xeondesktop [--replace]
 */
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "../gtkapp/gtkapp.h"
#include "chooser.h"

#define PORTAL_BUS_NAME "org.freedesktop.impl.portal.desktop.xeondesktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define FILE_CHOOSER_INTERFACE "org.freedesktop.impl.portal.FileChooser"
#define REQUEST_INTERFACE "org.freedesktop.impl.portal.Request"
#define RESOURCE_PATH "/org/xeondesktop/portal/"

/* The resource compiled from xdg-desktop-portal's interface files. */
GResource *xd_portal_get_resource(void);

typedef struct portal_call {
    GDBusMethodInvocation *invocation;
    GDBusConnection *connection;
    unsigned request_id; /* the Request object's registration */
    xd_chooser *chooser;
} portal_call;

static GDBusInterfaceInfo *request_info;

static GDBusInterfaceInfo *load_interface(const char *file, const char *name) {
    char *path = g_strconcat(RESOURCE_PATH, file, nullptr);
    GBytes *bytes = g_resource_lookup_data(xd_portal_get_resource(), path,
                                           G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    g_free(path);
    if (bytes == nullptr)
        return nullptr;
    gsize size = 0;
    const char *xml = g_bytes_get_data(bytes, &size);
    GError *error = nullptr;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, &error);
    g_bytes_unref(bytes);
    if (node == nullptr) {
        fprintf(stderr, "xdg-desktop-portal-xeondesktop: %s\n", error->message);
        g_error_free(error);
        return nullptr;
    }
    GDBusInterfaceInfo *info = g_dbus_node_info_lookup_interface(node, name);
    if (info != nullptr)
        g_dbus_interface_info_ref(info);
    g_dbus_node_info_unref(node);
    return info;
}

static void finish_call(unsigned response, GVariant *results, gpointer data) {
    portal_call *call = data;
    if (call->request_id != 0)
        g_dbus_connection_unregister_object(call->connection, call->request_id);
    if (results == nullptr)
        results = g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0);
    g_dbus_method_invocation_return_value(call->invocation,
                                          g_variant_new("(u@a{sv})", response, results));
    g_object_unref(call->connection);
    g_free(call);
}

static void handle_request_method(
    [[maybe_unused]] GDBusConnection *connection, [[maybe_unused]] const char *sender,
    [[maybe_unused]] const char *object_path, [[maybe_unused]] const char *interface_name,
    const char *method_name, [[maybe_unused]] GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer data) {
    portal_call *call = data;
    if (strcmp(method_name, "Close") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD, "No method %s",
                                              method_name);
        return;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    if (call->chooser != nullptr) {
        xd_chooser *chooser = call->chooser;
        call->chooser = nullptr;
        xd_chooser_close(chooser); /* finish_call frees the call */
    }
}

static const GDBusInterfaceVTable request_vtable = {.method_call = handle_request_method};

static void handle_file_chooser_method(GDBusConnection *connection,
                                       [[maybe_unused]] const char *sender,
                                       [[maybe_unused]] const char *object_path,
                                       [[maybe_unused]] const char *interface_name,
                                       const char *method_name, GVariant *parameters,
                                       GDBusMethodInvocation *invocation,
                                       [[maybe_unused]] gpointer data) {
    xd_chooser_request request = {0};
    if (strcmp(method_name, "OpenFile") == 0) {
        request.mode = XD_CHOOSER_OPEN;
    } else if (strcmp(method_name, "SaveFile") == 0) {
        request.mode = XD_CHOOSER_SAVE;
    } else if (strcmp(method_name, "SaveFiles") == 0) {
        request.mode = XD_CHOOSER_SAVE_FILES;
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD, "No method %s",
                                              method_name);
        return;
    }
    const char *handle = nullptr, *app_id = nullptr;
    g_variant_get(parameters, "(&o&s&s&s@a{sv})", &handle, &app_id, &request.parent_window,
                  &request.title, &request.options);

    portal_call *call = g_new0(portal_call, 1);
    call->invocation = invocation;
    call->connection = g_object_ref(connection);
    GError *error = nullptr;
    if (request_info != nullptr)
        call->request_id = g_dbus_connection_register_object(
            connection, handle, request_info, &request_vtable, call, nullptr, &error);
    if (call->request_id == 0 && error != nullptr) {
        fprintf(stderr, "xdg-desktop-portal-xeondesktop: cannot export %s: %s\n", handle,
                error->message);
        g_error_free(error);
    }
    call->chooser = xd_chooser_show(&request, finish_call, call);
    g_variant_unref(request.options);
}

static const GDBusInterfaceVTable file_chooser_vtable = {.method_call =
                                                             handle_file_chooser_method};

static GDBusInterfaceInfo *file_chooser_info;

static void on_bus_acquired(GDBusConnection *connection, [[maybe_unused]] const char *name,
                            [[maybe_unused]] gpointer data) {
    GError *error = nullptr;
    if (g_dbus_connection_register_object(connection, PORTAL_OBJECT_PATH, file_chooser_info,
                                          &file_chooser_vtable, nullptr, nullptr, &error) == 0) {
        fprintf(stderr, "xdg-desktop-portal-xeondesktop: cannot export %s: %s\n",
                PORTAL_OBJECT_PATH, error->message);
        g_error_free(error);
        gtk_main_quit();
    }
}

static void on_name_lost([[maybe_unused]] GDBusConnection *connection, const char *name,
                         [[maybe_unused]] gpointer data) {
    fprintf(stderr, "xdg-desktop-portal-xeondesktop: lost the name %s\n", name);
    gtk_main_quit();
}

int main(int argc, char **argv) {
    bool replace = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replace") == 0 || strcmp(argv[i], "-r") == 0) {
            replace = true;
        } else {
            fprintf(stderr, "usage: xdg-desktop-portal-xeondesktop [--replace]\n");
            return 2;
        }
    }
    /* Our dialogs are portal UI: never ask a portal for anything ourselves. */
    g_setenv("GTK_USE_PORTAL", "0", TRUE);
    xd_gtk_app_preinit("xdg-desktop-portal-xeondesktop");
    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "xdg-desktop-portal-xeondesktop: cannot open the display\n");
        return 1;
    }
    xd_gtk_app_follow_theme();

    file_chooser_info =
        load_interface("org.freedesktop.impl.portal.FileChooser.xml", FILE_CHOOSER_INTERFACE);
    request_info = load_interface("org.freedesktop.impl.portal.Request.xml", REQUEST_INTERFACE);
    if (file_chooser_info == nullptr || request_info == nullptr) {
        fprintf(stderr, "xdg-desktop-portal-xeondesktop: the portal interfaces are missing\n");
        return 1;
    }
    GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
    if (replace)
        flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;
    unsigned owner = g_bus_own_name(G_BUS_TYPE_SESSION, PORTAL_BUS_NAME, flags, on_bus_acquired,
                                    nullptr, on_name_lost, nullptr, nullptr);
    gtk_main();
    g_bus_unown_name(owner);
    g_dbus_interface_info_unref(file_chooser_info);
    g_dbus_interface_info_unref(request_info);
    return 0;
}
