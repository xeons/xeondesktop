/* SPDX-License-Identifier: MIT
 * Task Manager's notification area icon: a StatusNotifierItem on the
 * session bus, registered with org.kde.StatusNotifierWatcher (the shell)
 * whenever it's there, and its menu as a com.canonical.dbusmenu object.
 */
#include "trayicon.h"

#include <cairo.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define ITEM_PATH "/StatusNotifierItem"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define MENU_PATH "/MenuBar"
#define MENU_IFACE "com.canonical.dbusmenu"

enum { MENU_RESTORE = 1, MENU_ON_TOP, MENU_SEPARATOR, MENU_CLOSE };

struct xd_trayicon {
    const xd_trayicon_ops *ops;
    gpointer data;
    GDBusConnection *bus;
    char *name; /* our well-known name, as registered */
    guint own_id, watch_id, item_reg, menu_reg;
    gboolean shown, on_top;
    int level;   /* LED rows lit */
    int percent; /* for the tooltip */
    guint revision;
    GCancellable *cancel;
};

/* ---- the meter --------------------------------------------------------------------- */

static constexpr int LED_ROWS = 7;

/* Windows' icon: a black square with green LED rows lit from the bottom,
 * the unlit ones dim. */
static GVariant *meter_pixmap(int size, int level) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cairo = cairo_create(surface);
    double unit = size / 16.0;
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_rectangle(cairo, 1 * unit, 0, 14 * unit, 16 * unit);
    cairo_fill(cairo);
    for (int row = 0; row < LED_ROWS; row++) {
        bool lit = row < level;
        if (lit)
            cairo_set_source_rgb(cairo, 0, 1, 0);
        else
            cairo_set_source_rgb(cairo, 0, 0.5, 0);
        double y = (14 - 2 * row) * unit;
        cairo_rectangle(cairo, 3 * unit, y, 4 * unit, unit);
        cairo_rectangle(cairo, 9 * unit, y, 4 * unit, unit);
        cairo_fill(cairo);
    }
    cairo_destroy(cairo);
    cairo_surface_flush(surface);

    /* SNI wants ARGB32 in network byte order, not premultiplied (opaque
     * here, so only the order matters). */
    const unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    gsize len = (gsize)size * (gsize)size * 4;
    guint8 *out = g_malloc(len);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            guint32 pixel;
            memcpy(&pixel, data + (size_t)y * (size_t)stride + (size_t)x * 4, sizeof(pixel));
            guint8 *out_pixel = out + ((size_t)y * (size_t)size + (size_t)x) * 4;
            out_pixel[0] = (guint8)(pixel >> 24);
            out_pixel[1] = (guint8)(pixel >> 16);
            out_pixel[2] = (guint8)(pixel >> 8);
            out_pixel[3] = (guint8)pixel;
        }
    }
    cairo_surface_destroy(surface);
    return g_variant_new("(ii@ay)", size, size,
                         g_variant_new_from_data(G_VARIANT_TYPE("ay"), out, len, TRUE, g_free,
                                                 out));
}

static GVariant *icon_pixmaps(const xd_trayicon *ti) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    g_variant_builder_add_value(&builder, meter_pixmap(16, ti->level));
    g_variant_builder_add_value(&builder, meter_pixmap(32, ti->level));
    return g_variant_builder_end(&builder);
}

static GVariant *tooltip(const xd_trayicon *ti) {
    char text[32];
    g_snprintf(text, sizeof(text), "CPU Usage: %d%%", ti->percent);
    return g_variant_new("(s@a(iiay)ss)", "",
                         g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0), text, "");
}

/* ---- org.kde.StatusNotifierItem ----------------------------------------------------- */

static const char item_xml[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "<signal name='NewIcon'/><signal name='NewToolTip'/>"
    "</interface></node>";

static void item_call([[maybe_unused]] GDBusConnection *connection,
                      [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                      [[maybe_unused]] const char *iface, const char *method,
                      [[maybe_unused]] GVariant *params, GDBusMethodInvocation *inv,
                      gpointer data) {
    xd_trayicon *ti = data;
    g_dbus_method_invocation_return_value(inv, nullptr);
    /* A click restores the window, as a double-click does in Windows; the
     * right button gets the menu from the host. */
    if (strcmp(method, "Activate") == 0 || strcmp(method, "SecondaryActivate") == 0)
        ti->ops->restore(ti->data);
}

static GVariant *item_get([[maybe_unused]] GDBusConnection *connection,
                          [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                          [[maybe_unused]] const char *iface, const char *prop,
                          [[maybe_unused]] GError **error, gpointer data) {
    xd_trayicon *ti = data;
    if (strcmp(prop, "Category") == 0)
        return g_variant_new_string("SystemServices");
    if (strcmp(prop, "Id") == 0)
        return g_variant_new_string("xd-taskmgr");
    if (strcmp(prop, "Title") == 0)
        return g_variant_new_string("Task Manager");
    if (strcmp(prop, "Status") == 0)
        return g_variant_new_string("Active");
    if (strcmp(prop, "IconName") == 0)
        return g_variant_new_string("");
    if (strcmp(prop, "IconPixmap") == 0)
        return icon_pixmaps(ti);
    if (strcmp(prop, "ToolTip") == 0)
        return tooltip(ti);
    if (strcmp(prop, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);
    if (strcmp(prop, "Menu") == 0)
        return g_variant_new_object_path(MENU_PATH);
    return nullptr;
}

/* ---- com.canonical.dbusmenu --------------------------------------------------------- */

static const char menu_xml[] =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<property name='Version' type='u' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconThemePath' type='as' access='read'/>"
    "<method name='GetLayout'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='as' direction='in'/><arg type='u' direction='out'/>"
    "<arg type='(ia{sv}av)' direction='out'/></method>"
    "<method name='GetGroupProperties'><arg type='ai' direction='in'/>"
    "<arg type='as' direction='in'/><arg type='a(ia{sv})' direction='out'/></method>"
    "<method name='GetProperty'><arg type='i' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='v' direction='out'/></method>"
    "<method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='EventGroup'><arg type='a(isvu)' direction='in'/>"
    "<arg type='ai' direction='out'/></method>"
    "<method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "<method name='AboutToShowGroup'><arg type='ai' direction='in'/>"
    "<arg type='ai' direction='out'/><arg type='ai' direction='out'/></method>"
    "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
    "</interface></node>";

static GVariant *menu_props(const xd_trayicon *ti, int id) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    switch (id) {
    case 0:
        g_variant_builder_add(&builder, "{sv}", "children-display",
                              g_variant_new_string("submenu"));
        break;
    case MENU_RESTORE:
        g_variant_builder_add(&builder, "{sv}", "label", g_variant_new_string("_Restore"));
        break;
    case MENU_ON_TOP:
        g_variant_builder_add(&builder, "{sv}", "label", g_variant_new_string("_Always On Top"));
        g_variant_builder_add(&builder, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
        g_variant_builder_add(&builder, "{sv}", "toggle-state",
                              g_variant_new_int32(ti->on_top ? 1 : 0));
        break;
    case MENU_SEPARATOR:
        g_variant_builder_add(&builder, "{sv}", "type", g_variant_new_string("separator"));
        break;
    case MENU_CLOSE:
        g_variant_builder_add(&builder, "{sv}", "label",
                              g_variant_new_string("_Close Task Manager"));
        break;
    default:
        break;
    }
    return g_variant_builder_end(&builder);
}

static GVariant *menu_node(const xd_trayicon *ti, int id) {
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    if (id == 0) {
        for (int i = MENU_RESTORE; i <= MENU_CLOSE; i++)
            g_variant_builder_add(&kids, "v", menu_node(ti, i));
    }
    return g_variant_new("(i@a{sv}av)", id, menu_props(ti, id), &kids);
}

/* False once Close has run: that frees `ti`. */
static bool menu_event(xd_trayicon *ti, int id, const char *event) {
    if (strcmp(event, "clicked") != 0)
        return true;
    if (id == MENU_RESTORE) {
        ti->ops->restore(ti->data);
    } else if (id == MENU_ON_TOP) {
        ti->ops->set_on_top(!ti->on_top, ti->data);
    } else if (id == MENU_CLOSE) {
        ti->ops->close(ti->data);
        return false;
    }
    return true;
}

static void menu_call([[maybe_unused]] GDBusConnection *connection,
                      [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                      [[maybe_unused]] const char *iface, const char *method, GVariant *params,
                      GDBusMethodInvocation *inv, gpointer data) {
    xd_trayicon *ti = data;
    if (strcmp(method, "GetLayout") == 0) {
        gint32 parent = 0;
        g_variant_get(params, "(ii@as)", &parent, nullptr, nullptr);
        if (parent < 0 || parent > MENU_CLOSE)
            parent = 0;
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(u@(ia{sv}av))", ti->revision, menu_node(ti, parent)));
    } else if (strcmp(method, "GetGroupProperties") == 0) {
        GVariantIter *ids = nullptr;
        g_variant_get(params, "(ai@as)", &ids, nullptr);
        GVariantBuilder group_builder;
        g_variant_builder_init(&group_builder, G_VARIANT_TYPE("a(ia{sv})"));
        gint32 id;
        while (g_variant_iter_next(ids, "i", &id)) {
            if (id >= 0 && id <= MENU_CLOSE)
                g_variant_builder_add(&group_builder, "(i@a{sv})", id, menu_props(ti, id));
        }
        g_variant_iter_free(ids);
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&group_builder)));
    } else if (strcmp(method, "GetProperty") == 0) {
        gint32 id = 0;
        const char *name = nullptr;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariant *props = menu_props(ti, id);
        GVariant *variant = g_variant_lookup_value(props, name, nullptr);
        g_variant_unref(g_variant_ref_sink(props));
        if (variant == nullptr)
            g_dbus_method_invocation_return_dbus_error(inv, "com.canonical.dbusmenu.Error",
                                                       "no such property");
        else
            g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", variant));
        if (variant != nullptr)
            g_variant_unref(variant);
    } else if (strcmp(method, "Event") == 0) {
        gint32 id = 0;
        const char *event = nullptr;
        g_variant_get(params, "(i&s@vu)", &id, &event, nullptr, nullptr);
        g_dbus_method_invocation_return_value(inv, nullptr);
        menu_event(ti, id, event);
    } else if (strcmp(method, "EventGroup") == 0) {
        GVariantIter *events = nullptr;
        g_variant_get(params, "(a(isvu))", &events);
        gint32 id;
        const char *event;
        GVariantBuilder bad;
        g_variant_builder_init(&bad, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai)",
                                                                 g_variant_builder_end(&bad)));
        while (g_variant_iter_next(events, "(i&svu)", &id, &event, nullptr, nullptr)) {
            if (!menu_event(ti, id, event))
                break;
        }
        g_variant_iter_free(events);
    } else if (strcmp(method, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else if (strcmp(method, "AboutToShowGroup") == 0) {
        GVariantBuilder updates_needed, id_errors;
        g_variant_builder_init(&updates_needed, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&id_errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(@ai@ai)", g_variant_builder_end(&updates_needed),
                               g_variant_builder_end(&id_errors)));
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod",
                                                   method);
    }
}

static GVariant *menu_get([[maybe_unused]] GDBusConnection *connection,
                          [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                          [[maybe_unused]] const char *iface, const char *prop,
                          [[maybe_unused]] GError **error, [[maybe_unused]] gpointer data) {
    if (strcmp(prop, "Version") == 0)
        return g_variant_new_uint32(3);
    if (strcmp(prop, "TextDirection") == 0)
        return g_variant_new_string("ltr");
    if (strcmp(prop, "Status") == 0)
        return g_variant_new_string("normal");
    if (strcmp(prop, "IconThemePath") == 0)
        return g_variant_new_strv(nullptr, 0);
    return nullptr;
}

/* ---- registration ------------------------------------------------------------------- */

static void set_shown(xd_trayicon *ti, gboolean shown) {
    if (ti->shown == shown)
        return;
    ti->shown = shown;
    if (ti->ops->shown != nullptr)
        ti->ops->shown(shown, ti->data);
}

static void registered(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (reply == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_debug("xd-taskmgr: tray: %s", error->message);
        g_error_free(error);
        return;
    }
    g_variant_unref(reply);
    set_shown(data, TRUE);
}

static void watcher_appeared(GDBusConnection *bus, [[maybe_unused]] const char *name,
                             [[maybe_unused]] const char *owner, gpointer data) {
    xd_trayicon *ti = data;
    g_dbus_connection_call(bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
                           "RegisterStatusNotifierItem", g_variant_new("(s)", ti->name), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, 5000, ti->cancel, registered, ti);
}

static void watcher_vanished([[maybe_unused]] GDBusConnection *bus,
                             [[maybe_unused]] const char *name, gpointer data) {
    set_shown(data, FALSE);
}

static void name_acquired(GDBusConnection *bus, [[maybe_unused]] const char *name,
                          gpointer data) {
    xd_trayicon *ti = data;
    if (ti->watch_id == 0)
        ti->watch_id = g_bus_watch_name_on_connection(bus, WATCHER_NAME,
                                                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                      watcher_appeared, watcher_vanished, ti,
                                                      nullptr);
}

xd_trayicon *xd_trayicon_new(const xd_trayicon_ops *ops, gpointer data) {
    GError *error = nullptr;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (bus == nullptr) {
        g_debug("xd-taskmgr: no session bus: %s", error->message);
        g_error_free(error);
        return nullptr;
    }
    xd_trayicon *ti = g_new0(xd_trayicon, 1);
    ti->ops = ops;
    ti->data = data;
    ti->bus = bus;
    ti->cancel = g_cancellable_new();
    ti->name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int)getpid());

    static const GDBusInterfaceVTable item_vtable = {item_call, item_get, nullptr, {nullptr}};
    static const GDBusInterfaceVTable menu_vtable = {menu_call, menu_get, nullptr, {nullptr}};
    GDBusNodeInfo *item = g_dbus_node_info_new_for_xml(item_xml, nullptr);
    GDBusNodeInfo *menu = g_dbus_node_info_new_for_xml(menu_xml, nullptr);
    ti->item_reg = g_dbus_connection_register_object(bus, ITEM_PATH, item->interfaces[0],
                                                     &item_vtable, ti, nullptr, nullptr);
    ti->menu_reg = g_dbus_connection_register_object(bus, MENU_PATH, menu->interfaces[0],
                                                     &menu_vtable, ti, nullptr, nullptr);
    g_dbus_node_info_unref(item);
    g_dbus_node_info_unref(menu);
    ti->own_id = g_bus_own_name_on_connection(bus, ti->name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                              name_acquired, nullptr, ti, nullptr);
    return ti;
}

void xd_trayicon_free(xd_trayicon *ti) {
    if (ti == nullptr)
        return;
    g_cancellable_cancel(ti->cancel);
    g_object_unref(ti->cancel);
    if (ti->watch_id != 0)
        g_bus_unwatch_name(ti->watch_id);
    g_bus_unown_name(ti->own_id);
    g_dbus_connection_unregister_object(ti->bus, ti->item_reg);
    g_dbus_connection_unregister_object(ti->bus, ti->menu_reg);
    g_object_unref(ti->bus);
    g_free(ti->name);
    g_free(ti);
}

gboolean xd_trayicon_shown(const xd_trayicon *ti) { return ti != nullptr && ti->shown; }

static void emit(xd_trayicon *ti, const char *path, const char *iface, const char *signal,
                 GVariant *params) {
    g_dbus_connection_emit_signal(ti->bus, nullptr, path, iface, signal, params, nullptr);
}

void xd_trayicon_set_cpu(xd_trayicon *ti, double cpu) {
    if (ti == nullptr)
        return;
    cpu = CLAMP(cpu, 0.0, 1.0);
    /* The first row lights with any use at all, as in Windows. */
    int level = cpu > 0.0 ? CLAMP((int)(cpu * LED_ROWS + 0.5), 1, LED_ROWS) : 0;
    int percent = (int)(cpu * 100 + 0.5);
    if (level != ti->level) {
        ti->level = level;
        emit(ti, ITEM_PATH, ITEM_IFACE, "NewIcon", nullptr);
    }
    if (percent != ti->percent) {
        ti->percent = percent;
        emit(ti, ITEM_PATH, ITEM_IFACE, "NewToolTip", nullptr);
    }
}

void xd_trayicon_set_on_top(xd_trayicon *ti, gboolean on_top) {
    if (ti == nullptr || ti->on_top == on_top)
        return;
    ti->on_top = on_top;
    ti->revision++;
    emit(ti, MENU_PATH, MENU_IFACE, "LayoutUpdated", g_variant_new("(ui)", ti->revision, 0));
}
