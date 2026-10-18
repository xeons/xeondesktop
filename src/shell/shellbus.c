/* SPDX-License-Identifier: MIT
 * org.xeondesktop.Shell on the session bus: how another compositor opens
 * the Start menu from a key binding (`xd-shell --toggle-start-menu`, or a
 * D-Bus call). Our own compositor sends the Windows key over the arranger
 * protocol instead, with the key's serial.
 *
 * A call from a key binding comes with no input serial of ours, and the
 * menu's popup grab wants one: the taskbar takes the keyboard first and
 * the menu opens on the keyboard focus that brings (keyboard_enter calls
 * xd_shellbus_keyboard_entered). A compositor that never gives it gets
 * the menu without a grab after a moment: Esc and the menu's own items
 * still close it.
 */
#include <gio/gio.h>
#include <stdio.h>

#include "shell.h"

#define SHELL_NAME "org.xeondesktop.Shell"
#define SHELL_PATH "/org/xeondesktop/Shell"

/* How long the taskbar waits for the keyboard before opening anyway. */
static constexpr guint FOCUS_WAIT_MS = 250;

static const char shell_xml[] = "<node>"
                                "  <interface name='" SHELL_NAME "'>"
                                "    <method name='ToggleStartMenu'/>"
                                "  </interface>"
                                "</node>";

struct xd_shellbus {
    xd_shell *shell;
    guint own_id;
    GDBusConnection *bus; /* once the name's connection is up */
    guint object_id;
    GDBusNodeInfo *info;
    xd_taskbar *pending; /* took the keyboard to open the Start menu */
    guint pending_timer;
};

static void cancel_pending(xd_shellbus *sb) {
    sb->pending = nullptr;
    if (sb->pending_timer != 0)
        g_source_remove(sb->pending_timer);
    sb->pending_timer = 0;
}

static void open_on(xd_shellbus *sb, xd_taskbar *bar, uint32_t serial) {
    xd_shell *shell = sb->shell;
    cancel_pending(sb);
    xd_taskbar_toggle_start(bar, serial);
    /* Nothing opened (no menu yet): give the keyboard back. */
    if (shell->start_menu == nullptr && shell->start7 == nullptr && bar->layer != nullptr) {
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(bar->surface);
    }
}

static gboolean focus_timeout(gpointer data) {
    xd_shellbus *sb = data;
    sb->pending_timer = 0;
    xd_taskbar *bar = sb->pending;
    if (bar != nullptr)
        open_on(sb, bar, 0);
    return G_SOURCE_REMOVE;
}

void xd_shell_toggle_start_menu(xd_shell *shell) {
    xd_shellbus *sb = shell->shellbus;
    if (sb == nullptr || sb->pending != nullptr)
        return;
    /* Open: close it, as a second press of the Windows key does. */
    if ((shell->start_menu != nullptr && !shell->context_menu) || shell->start7 != nullptr) {
        xd_menu_close_all(shell);
        return;
    }
    /* The first monitor's taskbar: there's no telling which monitor the
     * key was pressed on. */
    xd_taskbar *bar = nullptr;
    XD_LIST_FOR_EACH(b, &shell->taskbars, xd_taskbar, link) {
        if (b->surface != nullptr && b->configured) {
            bar = b;
            break;
        }
    }
    if (bar == nullptr)
        return;
    if (shell->kb_surface == bar->surface) {
        open_on(sb, bar, shell->kb_serial);
        return;
    }
    sb->pending = bar;
    xd_taskbar_take_keyboard(bar);
    sb->pending_timer = g_timeout_add(FOCUS_WAIT_MS, focus_timeout, sb);
}

void xd_shellbus_keyboard_entered(xd_shell *shell, struct wl_surface *surface, uint32_t serial) {
    xd_shellbus *sb = shell->shellbus;
    if (sb != nullptr && sb->pending != nullptr && sb->pending->surface == surface)
        open_on(sb, sb->pending, serial);
}

void xd_shellbus_taskbar_gone(xd_shell *shell, xd_taskbar *bar) {
    xd_shellbus *sb = shell->shellbus;
    if (sb != nullptr && sb->pending == bar)
        cancel_pending(sb);
}

static void method_call([[maybe_unused]] GDBusConnection *c, [[maybe_unused]] const char *sender,
                        [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                        const char *method, [[maybe_unused]] GVariant *params,
                        GDBusMethodInvocation *inv, gpointer data) {
    xd_shellbus *sb = data;
    if (g_strcmp0(method, "ToggleStartMenu") == 0) {
        xd_shell_toggle_start_menu(sb->shell);
        g_dbus_method_invocation_return_value(inv, nullptr);
        return;
    }
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "No such method: %s", method);
}

static const GDBusInterfaceVTable shell_vtable = {.method_call = method_call};

static void bus_acquired(GDBusConnection *bus, [[maybe_unused]] const char *name, gpointer data) {
    xd_shellbus *sb = data;
    sb->bus = g_object_ref(bus);
    sb->object_id = g_dbus_connection_register_object(bus, SHELL_PATH, sb->info->interfaces[0],
                                                      &shell_vtable, sb, nullptr, nullptr);
}

static void name_lost(GDBusConnection *bus, [[maybe_unused]] const char *name,
                      [[maybe_unused]] gpointer data) {
    /* No bus, or another shell has the name: that one gets the key. */
    if (bus != nullptr)
        fprintf(stderr, "xd-shell: %s is taken; another shell is running\n", SHELL_NAME);
}

void xd_shellbus_init(xd_shell *shell) {
    xd_shellbus *sb = g_new0(xd_shellbus, 1);
    sb->shell = shell;
    sb->info = g_dbus_node_info_new_for_xml(shell_xml, nullptr);
    shell->shellbus = sb;
    sb->own_id = g_bus_own_name(G_BUS_TYPE_SESSION, SHELL_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                bus_acquired, nullptr, name_lost, sb, nullptr);
}

void xd_shellbus_finish(xd_shell *shell) {
    xd_shellbus *sb = shell->shellbus;
    if (sb == nullptr)
        return;
    shell->shellbus = nullptr;
    cancel_pending(sb);
    g_bus_unown_name(sb->own_id);
    if (sb->bus != nullptr) {
        g_dbus_connection_unregister_object(sb->bus, sb->object_id);
        g_object_unref(sb->bus);
    }
    g_dbus_node_info_unref(sb->info);
    g_free(sb);
}

int xd_shellbus_call_toggle(void) {
    GError *error = nullptr;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    GVariant *ret = nullptr;
    if (bus != nullptr) {
        ret = g_dbus_connection_call_sync(bus, SHELL_NAME, SHELL_PATH, SHELL_NAME,
                                          "ToggleStartMenu", nullptr, nullptr,
                                          G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, nullptr, &error);
        g_object_unref(bus);
    }
    if (ret == nullptr) {
        if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
            g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER))
            fprintf(stderr, "xd-shell: no shell is running\n");
        else
            fprintf(stderr, "xd-shell: %s\n", error != nullptr ? error->message : "no session bus");
        g_clear_error(&error);
        return EXIT_FAILURE;
    }
    g_variant_unref(ret);
    return EXIT_SUCCESS;
}
