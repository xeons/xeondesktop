/* SPDX-License-Identifier: MIT
 * The notification area's icons: a StatusNotifierItem host (the protocol
 * KDE, Qt, Electron, libappindicator and friends use for tray icons), over
 * GDBus.
 *
 * We own org.kde.StatusNotifierWatcher when nobody else does, so programs
 * register their items with us; if another desktop's watcher is running,
 * we register as a host with it instead. Each item's properties are read
 * when it appears and again when it signals a change. Its menu
 * (com.canonical.dbusmenu) is shown with the shell's own menus.
 */
#include <gio/gio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "shell.h"

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define MENU_IFACE "com.canonical.dbusmenu"

struct xd_tray_item {
    xd_tray *tray;
    /* Our own applets (volume, network, ...) instead of a D-Bus item. */
    const xd_sysicon_ops *ops;
    void *data;
    char *key;     /* as registered: "busname/path" */
    char *bus;     /* unique or well-known name */
    char *path;    /* object path */
    char *title, *status, *icon_name, *attention_icon_name, *theme_path, *tooltip, *menu;
    bool item_is_menu;
    cairo_surface_t *pixmap;           /* IconPixmap, the size nearest 32px */
    cairo_surface_t *attention_pixmap; /* AttentionIconPixmap */
    cairo_surface_t *scaled;           /* pixmap at scaled_px */
    int scaled_px;
    guint name_watch;
    guint signals;
    GCancellable *cancel;
    int64_t active_ms; /* last registered, changed or clicked (monotonic) */
};

/* Quiet this long, a program's icon folds behind the chevron;
 * XD_TRAY_INACTIVE_SECONDS shortens it for testing. */
static int64_t inactive_ms(void) {
    const char *test = getenv("XD_TRAY_INACTIVE_SECONDS");
    gint64 s = test != nullptr ? g_ascii_strtoll(test, nullptr, 10) : 0;
    return s > 0 ? s * 1000 : 3 * 60 * 1000;
}

static int64_t now_ms(void) { return g_get_monotonic_time() / 1000; }

struct xd_tray {
    xd_shell *shell;
    GDBusConnection *bus;
    GCancellable *cancel;
    guint own_watcher, own_host;
    guint watcher_object;
    bool is_watcher;
    guint ext_registered, ext_unregistered, ext_watch;
    GDBusNodeInfo *watcher_info;
    GPtrArray *items; /* xd_tray_item *, in registration order */
    char *host_name;
    /* The item whose menu is open (or about to), and where to open it. */
    char *menu_key;
    xd_taskbar *menu_bar;
    xd_rect menu_anchor;
    uint32_t menu_serial;
};

static void changed(xd_tray *tray) { xd_taskbars_mark_dirty(tray->shell); }

/* ---- items -------------------------------------------------------------------- */

void xd_tray_add_sysicon(xd_shell *shell, const char *name, const xd_sysicon_ops *ops,
                         void *data) {
    xd_tray *tray = shell->tray;
    if (tray == nullptr)
        return;
    xd_tray_item *it = g_new0(xd_tray_item, 1);
    it->tray = tray;
    it->key = g_strconcat("sysicon:", name, nullptr);
    it->ops = ops;
    it->data = data;
    g_ptr_array_add(tray->items, it);
    changed(tray);
}

void xd_tray_changed(xd_shell *shell) {
    if (shell->tray != nullptr)
        changed(shell->tray);
}

static void item_free(xd_tray_item *it) {
    if (it->cancel != nullptr) {
        g_cancellable_cancel(it->cancel);
        g_object_unref(it->cancel);
    }
    if (it->name_watch != 0)
        g_bus_unwatch_name(it->name_watch);
    if (it->ops != nullptr && it->ops->destroy != nullptr)
        it->ops->destroy(it->data);
    if (it->signals != 0)
        g_dbus_connection_signal_unsubscribe(it->tray->bus, it->signals);
    g_free(it->key);
    g_free(it->bus);
    g_free(it->path);
    g_free(it->title);
    g_free(it->status);
    g_free(it->icon_name);
    g_free(it->attention_icon_name);
    g_free(it->theme_path);
    g_free(it->tooltip);
    g_free(it->menu);
    if (it->pixmap != nullptr)
        cairo_surface_destroy(it->pixmap);
    if (it->attention_pixmap != nullptr)
        cairo_surface_destroy(it->attention_pixmap);
    if (it->scaled != nullptr)
        cairo_surface_destroy(it->scaled);
    g_free(it);
}

static xd_tray_item *find_item(xd_tray *tray, const char *key) {
    for (guint i = 0; i < tray->items->len; i++) {
        xd_tray_item *it = g_ptr_array_index(tray->items, i);
        if (strcmp(it->key, key) == 0)
            return it;
    }
    return nullptr;
}

/* IconPixmap: ARGB32 images in network byte order, several sizes. Keeps the
 * one nearest 32px (crisp at 16px and on HiDPI). */
static cairo_surface_t *pixmap_from_variant(GVariant *v) {
    if (v == nullptr || !g_variant_is_of_type(v, G_VARIANT_TYPE("a(iiay)")))
        return nullptr;
    GVariantIter iter;
    g_variant_iter_init(&iter, v);
    gint32 w, h;
    GVariant *bytes;
    gint32 best_w = 0, best_h = 0;
    GVariant *best = nullptr;
    while (g_variant_iter_next(&iter, "(ii@ay)", &w, &h, &bytes)) {
        gsize len = g_variant_get_size(bytes);
        bool ok = w > 0 && h > 0 && w <= 512 && h <= 512 && len == (gsize)w * (gsize)h * 4;
        if (ok && (best == nullptr || abs(w - 32) < abs(best_w - 32))) {
            if (best != nullptr)
                g_variant_unref(best);
            best = bytes;
            best_w = w;
            best_h = h;
        } else {
            g_variant_unref(bytes);
        }
    }
    if (best == nullptr)
        return nullptr;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_w, best_h);
    const guint8 *src = g_variant_get_data(best);
    cairo_surface_flush(s);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < best_h; y++) {
        uint32_t *row = (uint32_t *)(void *)(dst + (size_t)y * (size_t)stride);
        for (int x = 0; x < best_w; x++) {
            const guint8 *p = src + ((size_t)y * (size_t)best_w + (size_t)x) * 4;
            uint32_t a = p[0], r = p[1] * a / 255, g = p[2] * a / 255, b = p[3] * a / 255;
            row[x] = a << 24 | r << 16 | g << 8 | b; /* premultiplied, native order */
        }
    }
    cairo_surface_mark_dirty(s);
    g_variant_unref(best);
    return s;
}

static char *dup_string(GVariant *props, const char *key) {
    const char *s = nullptr;
    if (g_variant_lookup(props, key, "&s", &s) && s[0] != '\0')
        return g_strdup(s);
    return nullptr;
}

static void set_string(char **field, char *value) {
    g_free(*field);
    *field = value;
}

static void got_properties(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (reply == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_debug("tray: GetAll failed: %s", error->message);
        g_error_free(error);
        return;
    }
    xd_tray_item *it = data;
    it->active_ms = now_ms();
    GVariant *props = nullptr;
    g_variant_get(reply, "(@a{sv})", &props);
    set_string(&it->title, dup_string(props, "Title"));
    set_string(&it->status, dup_string(props, "Status"));
    set_string(&it->icon_name, dup_string(props, "IconName"));
    set_string(&it->attention_icon_name, dup_string(props, "AttentionIconName"));
    set_string(&it->theme_path, dup_string(props, "IconThemePath"));
    const char *menu = nullptr;
    set_string(&it->menu, g_variant_lookup(props, "Menu", "&o", &menu) && strcmp(menu, "/") != 0
                              ? g_strdup(menu)
                              : nullptr);
    gboolean is_menu = FALSE;
    it->item_is_menu = g_variant_lookup(props, "ItemIsMenu", "b", &is_menu) && is_menu;
    /* ToolTip: (icon name, icon pixmaps, title, description). */
    GVariant *tip = g_variant_lookup_value(props, "ToolTip", G_VARIANT_TYPE("(sa(iiay)ss)"));
    char *tooltip = nullptr;
    if (tip != nullptr) {
        const char *t = nullptr, *d = nullptr;
        g_variant_get(tip, "(&s@a(iiay)&s&s)", nullptr, nullptr, &t, &d);
        if (t != nullptr && t[0] != '\0')
            tooltip = g_strdup(t);
        else if (d != nullptr && d[0] != '\0')
            tooltip = g_strdup(d);
        g_variant_unref(tip);
    }
    set_string(&it->tooltip, tooltip);
    GVariant *pix = g_variant_lookup_value(props, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
    if (it->pixmap != nullptr)
        cairo_surface_destroy(it->pixmap);
    it->pixmap = pixmap_from_variant(pix);
    if (pix != nullptr)
        g_variant_unref(pix);
    pix = g_variant_lookup_value(props, "AttentionIconPixmap", G_VARIANT_TYPE("a(iiay)"));
    if (it->attention_pixmap != nullptr)
        cairo_surface_destroy(it->attention_pixmap);
    it->attention_pixmap = pixmap_from_variant(pix);
    if (pix != nullptr)
        g_variant_unref(pix);
    if (it->scaled != nullptr) {
        cairo_surface_destroy(it->scaled);
        it->scaled = nullptr;
    }
    g_variant_unref(props);
    g_variant_unref(reply);
    changed(it->tray);
}

static void refresh(xd_tray_item *it) {
    g_dbus_connection_call(it->tray->bus, it->bus, it->path, "org.freedesktop.DBus.Properties",
                           "GetAll", g_variant_new("(s)", ITEM_IFACE),
                           G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000, it->cancel,
                           got_properties, it);
}

static void item_signal([[maybe_unused]] GDBusConnection *c, [[maybe_unused]] const char *sender,
                        [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                        const char *signal, [[maybe_unused]] GVariant *params, gpointer data) {
    /* NewIcon, NewTitle, NewStatus, NewToolTip, NewAttentionIcon, ...:
     * read everything again (it's small). */
    if (g_str_has_prefix(signal, "New"))
        refresh(data);
}

static void remove_item(xd_tray *tray, const char *key);

static void item_vanished([[maybe_unused]] GDBusConnection *c, [[maybe_unused]] const char *name,
                          gpointer data) {
    xd_tray_item *it = data;
    remove_item(it->tray, it->key);
}

static void emit_watcher(xd_tray *tray, const char *signal, const char *arg) {
    if (tray->is_watcher)
        g_dbus_connection_emit_signal(tray->bus, nullptr, WATCHER_PATH, WATCHER_NAME, signal,
                                      g_variant_new("(s)", arg), nullptr);
}

/* "busname/path", "busname" (path /StatusNotifierItem) or "/path" (from
 * `sender`, as libappindicator registers). */
static void add_item(xd_tray *tray, const char *service, const char *sender) {
    char *bus, *path;
    const char *slash = strchr(service, '/');
    if (service[0] == '/') {
        if (sender == nullptr)
            return;
        bus = g_strdup(sender);
        path = g_strdup(service);
    } else if (slash != nullptr) {
        bus = g_strndup(service, (gsize)(slash - service));
        path = g_strdup(slash);
    } else {
        bus = g_strdup(service);
        path = g_strdup("/StatusNotifierItem");
    }
    char *key = g_strconcat(bus, path, nullptr);
    if (!g_dbus_is_name(bus) || !g_variant_is_object_path(path) || find_item(tray, key) != nullptr) {
        g_free(key);
        g_free(bus);
        g_free(path);
        return;
    }
    xd_tray_item *it = g_new0(xd_tray_item, 1);
    it->tray = tray;
    it->key = key;
    it->bus = bus;
    it->path = path;
    it->cancel = g_cancellable_new();
    it->active_ms = now_ms();
    g_ptr_array_add(tray->items, it);
    it->signals = g_dbus_connection_signal_subscribe(tray->bus, bus, ITEM_IFACE, nullptr, path,
                                                     nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                     item_signal, it, nullptr);
    it->name_watch = g_bus_watch_name_on_connection(tray->bus, bus, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                    nullptr, item_vanished, it, nullptr);
    refresh(it);
    emit_watcher(tray, "StatusNotifierItemRegistered", key);
}

static void remove_item(xd_tray *tray, const char *key) {
    for (guint i = 0; i < tray->items->len; i++) {
        xd_tray_item *it = g_ptr_array_index(tray->items, i);
        if (strcmp(it->key, key) != 0)
            continue;
        emit_watcher(tray, "StatusNotifierItemUnregistered", it->key);
        g_ptr_array_remove_index(tray->items, i);
        item_free(it);
        changed(tray);
        return;
    }
}

/* ---- being the watcher ------------------------------------------------------- */

static const char watcher_xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    "<method name='RegisterStatusNotifierItem'><arg name='service' type='s' direction='in'/></method>"
    "<method name='RegisterStatusNotifierHost'><arg name='service' type='s' direction='in'/></method>"
    "<property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "<property name='ProtocolVersion' type='i' access='read'/>"
    "<signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "<signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "<signal name='StatusNotifierHostRegistered'/>"
    "</interface></node>";

static void watcher_call([[maybe_unused]] GDBusConnection *c, const char *sender,
                         [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                         const char *method, GVariant *params, GDBusMethodInvocation *inv,
                         gpointer data) {
    xd_tray *tray = data;
    const char *service = nullptr;
    g_variant_get(params, "(&s)", &service);
    if (strcmp(method, "RegisterStatusNotifierItem") == 0)
        add_item(tray, service, sender);
    /* Hosts: we're the only one that matters here. */
    g_dbus_method_invocation_return_value(inv, nullptr);
}

static GVariant *watcher_get([[maybe_unused]] GDBusConnection *c,
                             [[maybe_unused]] const char *sender,
                             [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                             const char *prop, [[maybe_unused]] GError **error, gpointer data) {
    xd_tray *tray = data;
    if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (guint i = 0; i < tray->items->len; i++) {
            const xd_tray_item *it = g_ptr_array_index(tray->items, i);
            if (it->ops == nullptr)
                g_variant_builder_add(&b, "s", it->key);
        }
        return g_variant_builder_end(&b);
    }
    if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0)
        return g_variant_new_boolean(TRUE);
    if (strcmp(prop, "ProtocolVersion") == 0)
        return g_variant_new_int32(0);
    return nullptr;
}

static const GDBusInterfaceVTable watcher_vtable = {
    .method_call = watcher_call,
    .get_property = watcher_get,
};

/* ---- using another desktop's watcher -------------------------------------- */

static void ext_signal([[maybe_unused]] GDBusConnection *c, [[maybe_unused]] const char *sender,
                       [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                       const char *signal, GVariant *params, gpointer data) {
    xd_tray *tray = data;
    const char *service = nullptr;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(s)")))
        return;
    g_variant_get(params, "(&s)", &service);
    if (strcmp(signal, "StatusNotifierItemRegistered") == 0) {
        add_item(tray, service, nullptr);
    } else {
        /* Unregistered names may lack the path; match either way. */
        xd_tray_item *it = find_item(tray, service);
        if (it == nullptr) {
            char *key = g_strconcat(service, "/StatusNotifierItem", nullptr);
            it = find_item(tray, key);
            g_free(key);
        }
        if (it != nullptr)
            remove_item(tray, it->key);
    }
}

static void got_ext_items(GObject *src, GAsyncResult *res, gpointer data) {
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    if (reply == nullptr)
        return;
    xd_tray *tray = data;
    GVariant *v = nullptr;
    g_variant_get(reply, "(v)", &v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE("as"))) {
        GVariantIter iter;
        const char *s;
        g_variant_iter_init(&iter, v);
        while (g_variant_iter_next(&iter, "&s", &s))
            add_item(tray, s, nullptr);
    }
    g_variant_unref(v);
    g_variant_unref(reply);
}

static void use_external_watcher(xd_tray *tray) {
    if (tray->ext_registered != 0)
        return;
    tray->ext_registered = g_dbus_connection_signal_subscribe(
        tray->bus, WATCHER_NAME, WATCHER_NAME, "StatusNotifierItemRegistered", WATCHER_PATH,
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE, ext_signal, tray, nullptr);
    tray->ext_unregistered = g_dbus_connection_signal_subscribe(
        tray->bus, WATCHER_NAME, WATCHER_NAME, "StatusNotifierItemUnregistered", WATCHER_PATH,
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE, ext_signal, tray, nullptr);
    g_dbus_connection_call(tray->bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
                           "RegisterStatusNotifierHost", g_variant_new("(s)", tray->host_name),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, tray->cancel, nullptr, nullptr);
    g_dbus_connection_call(tray->bus, WATCHER_NAME, WATCHER_PATH,
                           "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", WATCHER_NAME, "RegisteredStatusNotifierItems"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, tray->cancel,
                           got_ext_items, tray);
}

static void watcher_acquired([[maybe_unused]] GDBusConnection *c, [[maybe_unused]] const char *n,
                             gpointer data) {
    xd_tray *tray = data;
    tray->is_watcher = true;
    g_dbus_connection_emit_signal(tray->bus, nullptr, WATCHER_PATH, WATCHER_NAME,
                                  "StatusNotifierHostRegistered", nullptr, nullptr);
}

static void watcher_lost([[maybe_unused]] GDBusConnection *c, [[maybe_unused]] const char *n,
                         gpointer data) {
    xd_tray *tray = data;
    if (tray->bus == nullptr)
        return;
    /* Another desktop's watcher (KDE, waybar, ...) is running: be a host. */
    tray->is_watcher = false;
    use_external_watcher(tray);
}

/* ---- setup ---------------------------------------------------------------------- */

static void got_bus([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GDBusConnection *bus = g_bus_get_finish(res, nullptr);
    if (bus == nullptr)
        return; /* no session bus: no tray icons */
    xd_tray *tray = data;
    tray->bus = bus;
    tray->watcher_info = g_dbus_node_info_new_for_xml(watcher_xml, nullptr);
    tray->watcher_object = g_dbus_connection_register_object(
        bus, WATCHER_PATH, tray->watcher_info->interfaces[0], &watcher_vtable, tray, nullptr,
        nullptr);
    tray->host_name = g_strdup_printf("org.kde.StatusNotifierHost-%d", (int)getpid());
    tray->own_host = g_bus_own_name_on_connection(bus, tray->host_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                                  nullptr, nullptr, nullptr, nullptr);
    tray->own_watcher = g_bus_own_name_on_connection(bus, WATCHER_NAME,
                                                     G_BUS_NAME_OWNER_FLAGS_NONE,
                                                     watcher_acquired, watcher_lost, tray, nullptr);
}

void xd_tray_init(xd_shell *shell) {
    xd_tray *tray = g_new0(xd_tray, 1);
    tray->shell = shell;
    tray->items = g_ptr_array_new();
    tray->cancel = g_cancellable_new();
    shell->tray = tray;
    g_bus_get(G_BUS_TYPE_SESSION, tray->cancel, got_bus, tray);
}

void xd_tray_finish(xd_shell *shell) {
    xd_tray *tray = shell->tray;
    if (tray == nullptr)
        return;
    shell->tray = nullptr;
    g_cancellable_cancel(tray->cancel);
    g_object_unref(tray->cancel);
    for (guint i = 0; i < tray->items->len; i++)
        item_free(g_ptr_array_index(tray->items, i));
    g_ptr_array_free(tray->items, TRUE);
    if (tray->bus != nullptr) {
        if (tray->ext_registered != 0)
            g_dbus_connection_signal_unsubscribe(tray->bus, tray->ext_registered);
        if (tray->ext_unregistered != 0)
            g_dbus_connection_signal_unsubscribe(tray->bus, tray->ext_unregistered);
        if (tray->own_watcher != 0)
            g_bus_unown_name(tray->own_watcher);
        if (tray->own_host != 0)
            g_bus_unown_name(tray->own_host);
        if (tray->watcher_object != 0)
            g_dbus_connection_unregister_object(tray->bus, tray->watcher_object);
        g_object_unref(tray->bus);
    }
    if (tray->watcher_info != nullptr)
        g_dbus_node_info_unref(tray->watcher_info);
    g_free(tray->host_name);
    g_free(tray->menu_key);
    g_free(tray);
}

/* ---- what the taskbar shows ------------------------------------------------- */

static bool visible(const xd_tray_item *it) {
    if (it->ops != nullptr)
        return it->ops->icon(it->data) != nullptr;
    /* Passive items are hidden, as XP hides inactive icons. Nothing to
     * draw yet (properties pending) counts as hidden too. */
    if (it->status != nullptr && strcmp(it->status, "Passive") == 0)
        return false;
    return it->icon_name != nullptr || it->pixmap != nullptr;
}

size_t xd_tray_count(const xd_shell *shell) {
    if (shell->tray == nullptr)
        return 0;
    size_t n = 0;
    for (guint i = 0; i < shell->tray->items->len; i++)
        n += visible(g_ptr_array_index(shell->tray->items, i));
    return n;
}

/* Programs' icons first, then ours next to the clock, as in Windows. */
xd_tray_item *xd_tray_get(const xd_shell *shell, size_t index) {
    if (shell->tray == nullptr)
        return nullptr;
    for (int pass = 0; pass < 2; pass++) {
        for (guint i = 0; i < shell->tray->items->len; i++) {
            xd_tray_item *it = g_ptr_array_index(shell->tray->items, i);
            if ((it->ops != nullptr) == (pass == 1) && visible(it) && index-- == 0)
                return it;
        }
    }
    return nullptr;
}

bool xd_tray_item_inactive(const xd_tray_item *it) {
    /* Our own icons and ones asking for attention always show. */
    if (it->ops != nullptr || (it->status != nullptr && strcmp(it->status, "NeedsAttention") == 0))
        return false;
    return now_ms() - it->active_ms > inactive_ms();
}

char *xd_tray_item_tooltip(const xd_tray_item *it) {
    if (it->ops != nullptr)
        return it->ops->tooltip != nullptr ? it->ops->tooltip(it->data) : nullptr;
    return g_strdup(it->tooltip != nullptr ? it->tooltip : it->title);
}

bool xd_tray_item_draw(xd_tray_item *it, const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (it->ops == nullptr || it->ops->draw == nullptr)
        return false;
    it->ops->draw(it->data, t, cr, r);
    return true;
}

void xd_tray_item_scroll(xd_shell *shell, xd_tray_item *it, double delta) {
    (void)shell;
    if (it->ops != nullptr && it->ops->scroll != nullptr)
        it->ops->scroll(it->data, delta);
}

static cairo_surface_t *named_icon(xd_shell *shell, const xd_tray_item *it, const char *name,
                                   int px) {
    if (name == nullptr)
        return nullptr;
    cairo_surface_t *s = xd_icons_get(shell->icons, name, px);
    if (s != nullptr || it->theme_path == nullptr)
        return s;
    /* IconThemePath: the program's own icons. */
    static const char *const exts[] = {".png", ".svg"};
    for (size_t i = 0; i < XD_ARRAY_LEN(exts) && s == nullptr; i++) {
        char *path = g_strconcat(it->theme_path, "/", name, exts[i], nullptr);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            s = xd_icons_get(shell->icons, path, px);
        g_free(path);
    }
    return s;
}

cairo_surface_t *xd_tray_item_icon(xd_shell *shell, xd_tray_item *it, int px) {
    if (it->ops != nullptr) {
        const char *name = it->ops->icon(it->data);
        cairo_surface_t *s = name != nullptr ? xd_icons_get(shell->icons, name, px) : nullptr;
        /* Themes often have only the -symbolic variant. */
        if (s == nullptr && name != nullptr) {
            char *sym = g_strconcat(name, "-symbolic", nullptr);
            s = xd_icons_get(shell->icons, sym, px);
            g_free(sym);
        }
        return s;
    }
    bool attention = it->status != nullptr && strcmp(it->status, "NeedsAttention") == 0;
    cairo_surface_t *s = attention ? named_icon(shell, it, it->attention_icon_name, px) : nullptr;
    if (s == nullptr)
        s = named_icon(shell, it, it->icon_name, px);
    if (s != nullptr)
        return s;
    cairo_surface_t *pix = attention && it->attention_pixmap != nullptr ? it->attention_pixmap
                                                                         : it->pixmap;
    if (pix == nullptr)
        return nullptr;
    if (it->scaled == nullptr || it->scaled_px != px) {
        if (it->scaled != nullptr)
            cairo_surface_destroy(it->scaled);
        it->scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        cairo_t *cr = cairo_create(it->scaled);
        int w = cairo_image_surface_get_width(pix), h = cairo_image_surface_get_height(pix);
        cairo_scale(cr, px / (double)w, px / (double)h);
        cairo_set_source_surface(cr, pix, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_destroy(cr);
        it->scaled_px = px;
    }
    return it->scaled;
}

/* ---- clicks and menus ------------------------------------------------------- */

/* A dbusmenu label: '_' marks the access key ("__" is a real one). */
static char *menu_label(const char *label) {
    GString *s = g_string_new(nullptr);
    for (const char *p = label; *p != '\0'; p++) {
        if (*p == '_' && p[1] == '_')
            g_string_append_c(s, *p++);
        else if (*p != '_')
            g_string_append_c(s, *p);
    }
    return g_string_free(s, FALSE);
}

static void menu_event(xd_shell *shell, void *data) {
    xd_tray *tray = shell->tray;
    xd_tray_item *it = tray != nullptr && tray->menu_key != nullptr
                           ? find_item(tray, tray->menu_key)
                           : nullptr;
    if (it == nullptr || it->menu == nullptr)
        return;
    gint32 id = (gint32)(intptr_t)data;
    g_dbus_connection_call(tray->bus, it->bus, it->menu, MENU_IFACE, "Event",
                           g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0),
                                         (guint32)0),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr, nullptr);
}

static xd_menu_model *build_menu(xd_menu_tree *tree, GVariant *node, int depth) {
    xd_menu_model *m = xd_menu_model_new(tree);
    GVariant *children = g_variant_get_child_value(node, 2);
    GVariantIter iter;
    g_variant_iter_init(&iter, children);
    GVariant *boxed;
    bool last_separator = true; /* no separator first */
    while (g_variant_iter_next(&iter, "v", &boxed)) {
        gint32 id = 0;
        GVariant *props = nullptr;
        g_variant_get(boxed, "(i@a{sv}@av)", &id, &props, nullptr);
        gboolean visible_prop = TRUE, enabled = TRUE;
        g_variant_lookup(props, "visible", "b", &visible_prop);
        g_variant_lookup(props, "enabled", "b", &enabled);
        const char *type = nullptr, *label = nullptr, *display = nullptr, *icon = nullptr,
                   *toggle = nullptr;
        g_variant_lookup(props, "type", "&s", &type);
        g_variant_lookup(props, "label", "&s", &label);
        g_variant_lookup(props, "children-display", "&s", &display);
        g_variant_lookup(props, "icon-name", "&s", &icon);
        g_variant_lookup(props, "toggle-type", "&s", &toggle);
        gint32 state = 0;
        g_variant_lookup(props, "toggle-state", "i", &state);
        GVariant *kids = g_variant_get_child_value(boxed, 2);
        bool has_kids = g_variant_n_children(kids) > 0;
        g_variant_unref(kids);
        if (visible_prop) {
            if (type != nullptr && strcmp(type, "separator") == 0) {
                if (!last_separator)
                    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
                last_separator = true;
            } else {
                char *text = menu_label(label != nullptr ? label : "");
                bool sub = depth < 8 && (has_kids || (display != nullptr &&
                                                      strcmp(display, "submenu") == 0));
                xd_menu_item *item = xd_menu_add(tree, m, sub ? XD_MENU_SUBMENU : XD_MENU_ACTION,
                                                 text, icon != nullptr && icon[0] != '\0' ? icon
                                                                                          : nullptr);
                g_free(text);
                if (sub) {
                    item->submenu = build_menu(tree, boxed, depth + 1);
                } else {
                    item->action = menu_event;
                    item->action_data = (void *)(intptr_t)id;
                }
                item->disabled = !enabled;
                item->checked = toggle != nullptr && toggle[0] != '\0' && state == 1;
                last_separator = false;
            }
        }
        g_variant_unref(props);
        g_variant_unref(boxed);
    }
    g_variant_unref(children);
    return m;
}

/* Opens a built menu (in shell->context_tree) above a tray icon. */
static void open_model(xd_shell *shell, xd_taskbar *bar, xd_rect anchor, uint32_t serial,
                       xd_menu_model *model) {
    if (model->count == 0)
        return;
    xd_volume_flyout_dismiss(shell);
    xd_taskbar_take_keyboard(bar);
    shell->context_menu = true;
    xd_menu_open_root_bar(shell, model, bar, anchor, serial);
    shell->start_bar = bar;
    bar->dirty = true;
}

static bool bar_alive(xd_shell *shell, const xd_taskbar *bar) {
    XD_LIST_FOR_EACH(b, &shell->taskbars, xd_taskbar, link) {
        if (b == bar)
            return b->configured;
    }
    return false;
}

static void got_layout(GObject *src, GAsyncResult *res, gpointer data) {
    xd_tray *tray = data;
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (reply == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_debug("tray: GetLayout failed: %s", error->message);
        g_error_free(error);
        return;
    }
    xd_shell *shell = tray->shell;
    xd_taskbar *bar = tray->menu_bar;
    if (bar_alive(shell, bar) && g_variant_is_of_type(reply, G_VARIANT_TYPE("(u(ia{sv}av))"))) {
        GVariant *root = g_variant_get_child_value(reply, 1);
        xd_menu_close_all(shell);
        xd_menu_tree_free(&shell->context_tree);
        xd_menu_tree_init(&shell->context_tree);
        xd_menu_model *model = build_menu(&shell->context_tree, root, 0);
        g_variant_unref(root);
        open_model(shell, bar, tray->menu_anchor, tray->menu_serial, model);
    }
    g_variant_unref(reply);
}

static void show_menu(xd_tray *tray, xd_tray_item *it, xd_taskbar *bar, xd_rect anchor,
                      uint32_t serial) {
    g_free(tray->menu_key);
    tray->menu_key = g_strdup(it->key);
    tray->menu_bar = bar;
    tray->menu_anchor = anchor;
    tray->menu_serial = serial;
    /* Some programs fill their menus in only when asked. */
    g_dbus_connection_call(tray->bus, it->bus, it->menu, MENU_IFACE, "AboutToShow",
                           g_variant_new("(i)", 0), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000,
                           it->cancel, nullptr, nullptr);
    static const char *const props[] = {"type",        "label",        "enabled",
                                        "visible",     "toggle-type",  "toggle-state",
                                        "children-display", "icon-name", nullptr};
    g_dbus_connection_call(tray->bus, it->bus, it->menu, MENU_IFACE, "GetLayout",
                           g_variant_new("(ii^as)", 0, -1, props),
                           G_VARIANT_TYPE("(u(ia{sv}av))"), G_DBUS_CALL_FLAGS_NONE, 5000,
                           it->cancel, got_layout, tray);
}

typedef struct activate_ctx {
    xd_tray *tray;
    char *key;
    xd_taskbar *bar;
    xd_rect anchor;
    uint32_t serial;
} activate_ctx;

static void activated(GObject *src, GAsyncResult *res, gpointer data) {
    activate_ctx *a = data;
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (reply != nullptr) {
        g_variant_unref(reply);
    } else {
        /* No Activate (many libappindicator items): their menu instead. */
        xd_tray_item *it = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                               ? nullptr
                               : find_item(a->tray, a->key);
        if (it != nullptr && it->menu != nullptr)
            show_menu(a->tray, it, a->bar, a->anchor, a->serial);
        g_error_free(error);
    }
    g_free(a->key);
    g_free(a);
}

void xd_tray_item_click(xd_shell *shell, xd_tray_item *it, uint32_t button, xd_taskbar *bar,
                        xd_rect anchor, uint32_t serial) {
    xd_tray *tray = shell->tray;
    if (tray == nullptr)
        return;
    it->active_ms = now_ms();
    if (it->ops != nullptr) {
        /* Our applets: left click does the applet's thing (the volume
         * slider), else everything opens its menu. */
        if (button == XD_TRAY_BUTTON_LEFT && it->ops->activate != nullptr) {
            it->ops->activate(it->data, bar, anchor, serial);
        } else if (it->ops->menu != nullptr) {
            xd_menu_close_all(shell);
            xd_menu_tree_free(&shell->context_tree);
            xd_menu_tree_init(&shell->context_tree);
            xd_menu_model *m = xd_menu_model_new(&shell->context_tree);
            it->ops->menu(it->data, &shell->context_tree, m);
            open_model(shell, bar, anchor, serial, m);
        }
        return;
    }
    if (tray->bus == nullptr)
        return;
    bool right = button == XD_TRAY_BUTTON_RIGHT;
    if ((right || it->item_is_menu) && it->menu != nullptr) {
        show_menu(tray, it, bar, anchor, serial);
        return;
    }
    /* Screen coordinates aren't known to a Wayland client; programs use
     * them only to place their own windows. */
    const char *method = right                               ? "ContextMenu"
                         : button == XD_TRAY_BUTTON_MIDDLE ? "SecondaryActivate"
                                                            : "Activate";
    activate_ctx *a = g_new0(activate_ctx, 1);
    a->tray = tray;
    a->key = g_strdup(it->key);
    a->bar = bar;
    a->anchor = anchor;
    a->serial = serial;
    g_dbus_connection_call(tray->bus, it->bus, it->path, ITEM_IFACE, method,
                           g_variant_new("(ii)", 0, 0), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000,
                           it->cancel, activated, a);
}
