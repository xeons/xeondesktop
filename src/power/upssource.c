/* SPDX-License-Identifier: MIT */
#include "upssource.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <upsclient.h>

/* UPower's Device.Type, State and WarningLevel. */
enum { UPOWER_TYPE_UPS = 3 };
enum { UPOWER_DISCHARGING = 2, UPOWER_EMPTY = 3 };
enum { UPOWER_WARNING_LOW = 3 };

/* NUT's upsd port, and how long to wait for it to answer a connection. */
static constexpr uint16_t NUT_PORT = 3493;
static constexpr int NUT_CONNECT_SECONDS = 3;

void xd_ups_found_free(xd_ups_found *found) {
    g_free(found->device);
    g_free(found->description);
    g_free(found);
}

static void add_found(GPtrArray *found, char *device, char *description) {
    xd_ups_found *entry = g_new0(xd_ups_found, 1);
    entry->device = device;
    entry->description = description;
    g_ptr_array_add(found, entry);
}

/* ---- UPower ---------------------------------------------------------------------- */

static GVariant *get_property(GDBusProxy *device, const char *name, const GVariantType *type) {
    GVariant *value = g_dbus_proxy_get_cached_property(device, name);
    if (value != nullptr && !g_variant_is_of_type(value, type)) {
        g_variant_unref(value);
        value = nullptr;
    }
    return value;
}

static guint32 get_uint(GDBusProxy *device, const char *name) {
    GVariant *value = get_property(device, name, G_VARIANT_TYPE_UINT32);
    guint32 number = value != nullptr ? g_variant_get_uint32(value) : 0;
    if (value != nullptr)
        g_variant_unref(value);
    return number;
}

static void copy_string(GDBusProxy *device, const char *name, char *out, size_t cap) {
    GVariant *value = get_property(device, name, G_VARIANT_TYPE_STRING);
    g_strlcpy(out, value != nullptr ? g_variant_get_string(value, nullptr) : "", cap);
    if (value != nullptr)
        g_variant_unref(value);
}

void xd_ups_read_upower(GDBusProxy *device, xd_ups_reading *reading) {
    xd_ups_reading_init(reading);
    GVariant *present = get_property(device, "IsPresent", G_VARIANT_TYPE_BOOLEAN);
    reading->is_reachable = present != nullptr && g_variant_get_boolean(present);
    if (present != nullptr)
        g_variant_unref(present);
    if (!reading->is_reachable)
        return;
    guint32 state = get_uint(device, "State");
    reading->is_on_battery = state == UPOWER_DISCHARGING || state == UPOWER_EMPTY;
    /* UPower's warning for a UPS: Discharging on battery, then Low,
     * Critical and Action as it runs down, as NUT's LB. */
    reading->is_low_battery = get_uint(device, "WarningLevel") >= UPOWER_WARNING_LOW;
    GVariant *value = get_property(device, "Percentage", G_VARIANT_TYPE_DOUBLE);
    if (value != nullptr) {
        reading->charge = g_variant_get_double(value);
        g_variant_unref(value);
    }
    /* 0 while it charges or can't tell. */
    if ((value = get_property(device, "TimeToEmpty", G_VARIANT_TYPE_INT64)) != nullptr) {
        gint64 seconds = g_variant_get_int64(value);
        reading->runtime = seconds > 0 ? (int)XD_MIN(seconds, (gint64)INT32_MAX) : -1;
        g_variant_unref(value);
    }
    copy_string(device, "Vendor", reading->manufacturer, sizeof(reading->manufacturer));
    copy_string(device, "Model", reading->model, sizeof(reading->model));
}

void xd_ups_list_upower(GPtrArray *found) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (bus == nullptr)
        return;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, XD_UPOWER_NAME, XD_UPOWER_PATH, XD_UPOWER_NAME, "EnumerateDevices", nullptr,
        G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (reply == nullptr) {
        g_object_unref(bus);
        return;
    }
    GVariantIter *paths = nullptr;
    const char *path = nullptr;
    g_variant_get(reply, "(ao)", &paths);
    while (g_variant_iter_next(paths, "&o", &path)) {
        GDBusProxy *device = g_dbus_proxy_new_sync(
            bus, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, nullptr, XD_UPOWER_NAME, path,
            XD_UPOWER_DEVICE_INTERFACE, nullptr, nullptr);
        if (device == nullptr)
            continue;
        if (get_uint(device, "Type") == UPOWER_TYPE_UPS) {
            xd_ups_reading reading;
            xd_ups_read_upower(device, &reading);
            char *name = g_strstrip(g_strdup_printf("%s %s", reading.manufacturer, reading.model));
            add_found(found, g_strconcat("upower:", path, nullptr),
                      g_strdup_printf("%s (USB, through UPower)",
                                      name[0] != '\0' ? name : "UPS"));
            g_free(name);
        }
        g_object_unref(device);
    }
    g_variant_iter_free(paths);
    g_variant_unref(reply);
    g_object_unref(bus);
}

/* ---- NUT ------------------------------------------------------------------------- */

/* libupsclient's one-time setup (no SSL: upsd on a LAN, as upsmon and
 * upsc use it by default). */
static void init_upsclient(void) {
    static gsize done;
    if (g_once_init_enter(&done)) {
        upscli_init(0, nullptr, nullptr, nullptr);
        g_once_init_leave(&done, 1);
    }
}

/* Connects to "host[:port]"; false with `error` set. */
static bool connect_nut(UPSCONN_t *connection, const char *host, uint16_t port, char **error) {
    init_upsclient();
    struct timeval timeout = {.tv_sec = NUT_CONNECT_SECONDS};
    if (upscli_tryconnect(connection, host, port, 0, &timeout) < 0) {
        *error = g_strdup_printf("%s: %s", host, upscli_strerror(connection));
        upscli_disconnect(connection);
        return false;
    }
    return true;
}

bool xd_ups_list_nut(const char *host, GPtrArray *found, char **error) {
    *error = nullptr;
    /* "server:3494": upscli_splitname's way, with a UPS name of none. */
    char *spec = g_strconcat("ups@", host, nullptr);
    char *ups_name = nullptr;
    char *host_name = nullptr;
    uint16_t port = NUT_PORT;
    if (upscli_splitname(spec, &ups_name, &host_name, &port) != 0) {
        *error = g_strdup_printf("%s: not a host name", host);
        g_free(spec);
        return false;
    }
    g_free(spec);
    UPSCONN_t connection = {0};
    bool ok = connect_nut(&connection, host_name, port, error);
    if (ok) {
        const char *query[] = {"UPS"};
        size_t answer_count = 0;
        char **answer = nullptr;
        if (upscli_list_start(&connection, 1, query) < 0) {
            *error = g_strdup_printf("%s: %s", host_name, upscli_strerror(&connection));
            ok = false;
        }
        /* UPS <name> "<description>" */
        while (ok && upscli_list_next(&connection, 1, query, &answer_count, &answer) == 1) {
            if (answer_count < 3)
                continue;
            /* "myups (Back-UPS 700, NUT)", "myups on server (NUT)" */
            GString *description = g_string_new(answer[1]);
            if (strcmp(host, "localhost") != 0)
                g_string_append_printf(description, " on %s", host);
            if (answer[2][0] != '\0')
                g_string_append_printf(description, " (%s, NUT)", answer[2]);
            else
                g_string_append(description, " (NUT)");
            add_found(found, g_strdup_printf("nut:%s@%s", answer[1], host),
                      g_string_free(description, FALSE));
        }
        upscli_disconnect(&connection);
    }
    free(ups_name);
    free(host_name);
    return ok;
}

/* One variable's value into `out`; false if the UPS doesn't have it. */
static bool get_nut_variable(UPSCONN_t *connection, const char *ups_name, const char *variable,
                             char *out, size_t cap) {
    const char *query[] = {"VAR", ups_name, variable};
    size_t answer_count = 0;
    char **answer = nullptr;
    /* VAR <ups> <variable> "<value>" */
    if (upscli_get(connection, XD_ARRAY_LEN(query), query, &answer_count, &answer) < 0 ||
        answer_count < 4)
        return false;
    g_strlcpy(out, answer[3], cap);
    return true;
}

void xd_ups_read_nut(const char *address, xd_ups_reading *reading) {
    xd_ups_reading_init(reading);
    char *ups_name = nullptr;
    char *host_name = nullptr;
    uint16_t port = NUT_PORT;
    if (upscli_splitname(address, &ups_name, &host_name, &port) != 0)
        return;
    UPSCONN_t connection = {0};
    char *error = nullptr;
    if (connect_nut(&connection, host_name, port, &error)) {
        char value[128];
        if (get_nut_variable(&connection, ups_name, "ups.status", value, sizeof(value))) {
            reading->is_reachable = true;
            xd_ups_parse_nut_status(value, reading);
            if (get_nut_variable(&connection, ups_name, "battery.charge", value, sizeof(value)))
                reading->charge = g_ascii_strtod(value, nullptr);
            if (get_nut_variable(&connection, ups_name, "battery.runtime", value, sizeof(value)))
                reading->runtime = (int)g_ascii_strtoll(value, nullptr, 10);
            get_nut_variable(&connection, ups_name, "ups.mfr", reading->manufacturer,
                             sizeof(reading->manufacturer));
            get_nut_variable(&connection, ups_name, "ups.model", reading->model,
                             sizeof(reading->model));
        }
        upscli_disconnect(&connection);
    }
    g_free(error);
    free(ups_name);
    free(host_name);
}

static void read_nut_in_thread(GTask *task, [[maybe_unused]] gpointer source, gpointer address,
                               [[maybe_unused]] GCancellable *cancellable) {
    xd_ups_reading *reading = g_new(xd_ups_reading, 1);
    xd_ups_read_nut(address, reading);
    g_task_return_pointer(task, reading, g_free);
}

void xd_ups_read_nut_async(const char *address, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer data) {
    GTask *task = g_task_new(nullptr, cancellable, callback, data);
    g_task_set_task_data(task, g_strdup(address), g_free);
    g_task_run_in_thread(task, read_nut_in_thread);
    g_object_unref(task);
}

bool xd_ups_read_nut_finish(GAsyncResult *result, xd_ups_reading *reading) {
    xd_ups_reading *got = g_task_propagate_pointer(G_TASK(result), nullptr);
    if (got == nullptr)
        return false;
    *reading = *got;
    g_free(got);
    return true;
}
