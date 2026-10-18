/* SPDX-License-Identifier: MIT
 * Night Light. Windows 10 had it in Settings (Display > Night light
 * settings): "Turn on now", a Strength slider, and "Schedule night light"
 * with Sunset to sunrise (from Location services) or Set hours. Here it's
 * a dialog in the style of Display Properties' others, editing a copy of
 * [NightLight]; OK hands it back, and the main dialog's Apply saves it.
 *
 * "Find my location automatically" asks GeoClue (org.freedesktop.GeoClue2
 * on the system bus) once, while the dialog is open, and keeps the place
 * in the settings, so the compositor never needs location services. The
 * replies come through GLib's main context, which libxdui doesn't run, so
 * a timer turns it while a lookup is under way.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <gio/gio.h>

#include "../core/alloc.h"
#include "../settings/nightlight.h"
#include "desk_nightlight.h"

static constexpr int PUMP_MS = 100;
static constexpr int LOOKUP_TIMEOUT_S = 30;
/* GeoClue's GCLUE_ACCURACY_LEVEL_CITY: enough for the sun, and kinder to
 * privacy than the street. */
static constexpr uint32_t ACCURACY_CITY = 4;
/* The time lists: every half hour, plus the setting's own if it's off
 * the half hour (set in desktop.ini by hand). */
enum { CLOCK_ITEMS_MAX = 49 };

static const char GEOCLUE[] = "org.freedesktop.GeoClue2";

typedef enum lookup_state : uint8_t {
    LOOKUP_IDLE,
    LOOKUP_FINDING,
    LOOKUP_FOUND,
    LOOKUP_FAILED,
} lookup_state;

struct nightlight_dialog {
    xd_ui *ui;
    xd_window *win;
    xd_nightlight_settings edit;
    nightlight_done done;
    void *user;

    xd_widget *enable, *strength, *strength_value;
    xd_widget *always, *sunset, *manual;
    xd_widget *on_label, *on_combo, *off_label, *off_combo;
    xd_widget *auto_location, *lat_label, *lat, *lon_label, *lon, *status;
    int on_minutes[CLOCK_ITEMS_MAX], off_minutes[CLOCK_ITEMS_MAX];

    /* GeoClue */
    lookup_state lookup;
    GCancellable *cancel;
    GDBusConnection *bus;
    char *client; /* our GeoClue client's object path */
    guint signal_id;
    bool started;
    int pending; /* calls in flight */
    xd_ui_source *pump;
    time_t lookup_start;
};

/* ---- shared pieces ------------------------------------------------------------ */

/* "9:30 PM", as Windows' en-US time format. */
static void format_clock(int minute, char *buf, size_t cap) {
    int h = minute / 60 % 24, m = minute % 60;
    snprintf(buf, cap, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
}

static void clock_combo_fill(xd_widget *combo, int *minutes, int current) {
    int n = 0, sel = 0;
    for (int m = 0; m < 24 * 60; m += 30) {
        if (current > m - 30 && current < m && current % 30 != 0) {
            sel = n;
            minutes[n++] = current;
        }
        if (m == current)
            sel = n;
        minutes[n++] = m;
    }
    if (current > 23 * 60 + 30) {
        sel = n;
        minutes[n++] = current;
    }
    for (int i = 0; i < n; i++) {
        char buf[16];
        format_clock(minutes[i], buf, sizeof(buf));
        xd_widget_add_item(combo, buf);
    }
    xd_widget_select(combo, sel);
}

static int clock_combo_value(const xd_widget *combo, const int *minutes) {
    int i = xd_widget_selected(combo);
    return i >= 0 && i < CLOCK_ITEMS_MAX ? minutes[i] : 0;
}

/* The slider runs from weaker (XD_NIGHTLIGHT_K_MAX) to stronger
 * (XD_NIGHTLIGHT_K_MIN), in 100 K steps. */
static int strength_from_kelvin(int k) {
    return (XD_NIGHTLIGHT_K_MAX - k + 50) / 100;
}

static int kelvin_from_strength(int v) { return XD_NIGHTLIGHT_K_MAX - v * 100; }

static void show_strength(nightlight_dialog *n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d K", kelvin_from_strength(xd_slider_value(n->strength)));
    xd_widget_set_text(n->strength_value, buf);
}

/* Reads a degrees edit; "" leaves *out alone and says so. */
typedef enum degrees_read : uint8_t { DEGREES_OK, DEGREES_EMPTY, DEGREES_BAD } degrees_read;
static degrees_read read_degrees(xd_widget *edit, double limit, double *out) {
    char *s = g_strstrip(g_strdup(xd_widget_text(edit)));
    degrees_read r = DEGREES_EMPTY;
    if (s[0] != '\0') {
        char *end = nullptr;
        double d = g_ascii_strtod(s, &end);
        r = end != s && *end == '\0' && fabs(d) <= limit ? DEGREES_OK : DEGREES_BAD;
        if (r == DEGREES_OK)
            *out = d;
    }
    g_free(s);
    return r;
}

static void set_degrees(xd_widget *edit, double v) {
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    xd_widget_set_text(edit, g_ascii_formatd(buf, sizeof(buf), "%.4f", v));
}

/* The status line under the location: what's happening with GeoClue, or
 * tonight's times. */
static void update_status(nightlight_dialog *n) {
    if (!xd_checkbox_checked(n->sunset)) {
        xd_widget_set_text(n->status, "");
        return;
    }
    bool automatic = xd_checkbox_checked(n->auto_location);
    if (automatic && n->lookup == LOOKUP_FINDING) {
        xd_widget_set_text(n->status, "Finding your location...");
        return;
    }
    xd_nightlight_settings s = n->edit;
    s.schedule = XD_NIGHTLIGHT_SUNSET;
    s.has_location = read_degrees(n->lat, 90, &s.latitude) == DEGREES_OK &&
                     read_degrees(n->lon, 180, &s.longitude) == DEGREES_OK;
    const char *prefix = automatic && n->lookup == LOOKUP_FAILED
                             ? "Your location couldn't be found. "
                             : "";
    char buf[192];
    if (!s.has_location) {
        snprintf(buf, sizeof(buf), "%sUntil there is a location, night light keeps to %s.",
                 prefix, "the set hours");
        xd_widget_set_text(n->status, buf);
        return;
    }
    double on, off;
    switch (xd_nightlight_times(&s, time(nullptr), &on, &off)) {
    case XD_SUN_UP_ALL_DAY:
        snprintf(buf, sizeof(buf), "%sThe sun doesn't set there today.", prefix);
        break;
    case XD_SUN_DOWN_ALL_DAY:
        snprintf(buf, sizeof(buf), "%sThe sun doesn't rise there today.", prefix);
        break;
    case XD_SUN_RISES_AND_SETS:
    default: {
        char set[16], rise[16];
        format_clock((int)lround(on) % 1440, set, sizeof(set));
        format_clock((int)lround(off) % 1440, rise, sizeof(rise));
        snprintf(buf, sizeof(buf), "%sToday: sunset %s, sunrise %s.", prefix, set, rise);
        break;
    }
    }
    xd_widget_set_text(n->status, buf);
}

/* Greys what the choices above make meaningless. */
static void update_enabled(nightlight_dialog *n) {
    bool manual = xd_checkbox_checked(n->manual);
    bool sunset = xd_checkbox_checked(n->sunset);
    bool automatic = xd_checkbox_checked(n->auto_location);
    xd_widget_set_enabled(n->on_label, manual);
    xd_widget_set_enabled(n->on_combo, manual);
    xd_widget_set_enabled(n->off_label, manual);
    xd_widget_set_enabled(n->off_combo, manual);
    xd_widget_set_enabled(n->auto_location, sunset);
    /* Typing is for when GeoClue isn't asked, or couldn't answer. */
    bool typing = sunset && (!automatic || n->lookup == LOOKUP_FAILED);
    xd_widget_set_enabled(n->lat_label, typing);
    xd_widget_set_enabled(n->lat, typing);
    xd_widget_set_enabled(n->lon_label, typing);
    xd_widget_set_enabled(n->lon, typing);
    update_status(n);
}

/* ---- GeoClue ------------------------------------------------------------------ */

static void lookup_failed(nightlight_dialog *n, const char *why) {
    if (n->lookup != LOOKUP_FINDING)
        return;
    g_message("GeoClue: %s", why);
    n->lookup = LOOKUP_FAILED;
    update_enabled(n);
}

/* Turns GLib's main context while replies are due. */
static void on_pump(void *user) {
    nightlight_dialog *n = user;
    for (int i = 0; i < 16 && g_main_context_iteration(nullptr, FALSE); i++) {
    }
    if (n->lookup == LOOKUP_FINDING && time(nullptr) - n->lookup_start > LOOKUP_TIMEOUT_S)
        lookup_failed(n, "no location in time");
    if (n->lookup != LOOKUP_FINDING && n->pending == 0 && n->pump != nullptr) {
        xd_ui_remove_source(n->pump);
        n->pump = nullptr;
    }
}

/* A reply arrived: false if the dialog is closing (cancelled) or the call
 * failed, which then ends the lookup. */
static bool reply_ok(nightlight_dialog *n, GObject *source, GAsyncResult *res, GVariant **out,
                     const char *what) {
    n->pending--;
    GError *err = nullptr;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (v == nullptr) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            char *why = g_strdup_printf("%s: %s", what, err->message);
            lookup_failed(n, why);
            g_free(why);
        }
        g_error_free(err);
        return false;
    }
    if (out != nullptr)
        *out = v;
    else
        g_variant_unref(v);
    return n->lookup == LOOKUP_FINDING;
}

static void call(nightlight_dialog *n, const char *path, const char *iface, const char *method,
                 GVariant *args, const GVariantType *reply, GAsyncReadyCallback cb) {
    n->pending++;
    g_dbus_connection_call(n->bus, GEOCLUE, path, iface, method, args, reply,
                           G_DBUS_CALL_FLAGS_NONE, 10000, n->cancel, cb, n);
}

static void on_location(GObject *source, GAsyncResult *res, gpointer user) {
    nightlight_dialog *n = user;
    GVariant *v = nullptr;
    if (!reply_ok(n, source, res, &v, "Location"))
        return;
    GVariant *props = g_variant_get_child_value(v, 0);
    double lat = 999, lon = 999; /* out of range until read */
    if (!g_variant_lookup(props, "Latitude", "d", &lat) ||
        !g_variant_lookup(props, "Longitude", "d", &lon) || !(fabs(lat) <= 90) ||
        !(fabs(lon) <= 180)) {
        lookup_failed(n, "a location without coordinates");
    } else {
        set_degrees(n->lat, lat);
        set_degrees(n->lon, lon);
        n->lookup = LOOKUP_FOUND;
        update_enabled(n);
    }
    g_variant_unref(props);
    g_variant_unref(v);
    /* One answer is all night light needs. */
    if (n->started) {
        g_dbus_connection_call(n->bus, GEOCLUE, n->client, "org.freedesktop.GeoClue2.Client",
                               "Stop", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                               nullptr, nullptr);
        n->started = false;
    }
}

static void on_location_updated([[maybe_unused]] GDBusConnection *bus,
                                [[maybe_unused]] const char *sender,
                                [[maybe_unused]] const char *path,
                                [[maybe_unused]] const char *iface,
                                [[maybe_unused]] const char *signal, GVariant *params,
                                gpointer user) {
    nightlight_dialog *n = user;
    if (n->lookup != LOOKUP_FINDING || !g_variant_is_of_type(params, G_VARIANT_TYPE("(oo)")))
        return;
    const char *location = nullptr;
    g_variant_get(params, "(&o&o)", nullptr, &location);
    call(n, location, "org.freedesktop.DBus.Properties", "GetAll",
         g_variant_new("(s)", "org.freedesktop.GeoClue2.Location"), G_VARIANT_TYPE("(a{sv})"),
         on_location);
}

static void on_started(GObject *source, GAsyncResult *res, gpointer user) {
    nightlight_dialog *n = user;
    if (reply_ok(n, source, res, nullptr, "Start"))
        n->started = true;
}

static void on_property_set(GObject *source, GAsyncResult *res, gpointer user) {
    reply_ok(user, source, res, nullptr, "setting up the client");
}

static void on_client(GObject *source, GAsyncResult *res, gpointer user) {
    nightlight_dialog *n = user;
    GVariant *v = nullptr;
    if (!reply_ok(n, source, res, &v, "GetClient"))
        return;
    g_variant_get(v, "(o)", &n->client);
    g_variant_unref(v);
    n->signal_id = g_dbus_connection_signal_subscribe(
        n->bus, GEOCLUE, "org.freedesktop.GeoClue2.Client", "LocationUpdated", n->client,
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_location_updated, n, nullptr);
    /* GeoClue asks who's asking (its agent, or geoclue.conf's list of
     * programs allowed without one). Calls on one connection arrive in
     * order. */
    const char *iface = "org.freedesktop.GeoClue2.Client";
    call(n, n->client, "org.freedesktop.DBus.Properties", "Set",
         g_variant_new("(ssv)", iface, "DesktopId", g_variant_new_string("xd-desk")), nullptr,
         on_property_set);
    call(n, n->client, "org.freedesktop.DBus.Properties", "Set",
         g_variant_new("(ssv)", iface, "RequestedAccuracyLevel",
                       g_variant_new_uint32(ACCURACY_CITY)),
         nullptr, on_property_set);
    call(n, n->client, iface, "Start", nullptr, nullptr, on_started);
}

static void on_bus(GObject *source, GAsyncResult *res, gpointer user) {
    (void)source;
    nightlight_dialog *n = user;
    n->pending--;
    GError *err = nullptr;
    n->bus = g_bus_get_finish(res, &err);
    if (n->bus == nullptr) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            lookup_failed(n, err->message);
        g_error_free(err);
        return;
    }
    if (n->lookup != LOOKUP_FINDING)
        return;
    call(n, "/org/freedesktop/GeoClue2/Manager", "org.freedesktop.GeoClue2.Manager", "GetClient",
         nullptr, G_VARIANT_TYPE("(o)"), on_client);
}

static void lookup_start(nightlight_dialog *n) {
    if (n->lookup == LOOKUP_FINDING || n->lookup == LOOKUP_FOUND)
        return;
    n->lookup = LOOKUP_FINDING;
    n->lookup_start = time(nullptr);
    if (n->cancel == nullptr)
        n->cancel = g_cancellable_new();
    if (n->bus == nullptr) {
        n->pending++;
        g_bus_get(G_BUS_TYPE_SYSTEM, n->cancel, on_bus, n);
    } else if (n->client == nullptr) {
        call(n, "/org/freedesktop/GeoClue2/Manager", "org.freedesktop.GeoClue2.Manager",
             "GetClient", nullptr, G_VARIANT_TYPE("(o)"), on_client);
    } else {
        call(n, n->client, "org.freedesktop.GeoClue2.Client", "Start", nullptr, nullptr,
             on_started);
    }
    if (n->pump == nullptr)
        n->pump = xd_ui_add_timer(n->ui, PUMP_MS, true, on_pump, n);
}

/* Stops the lookup and waits out the calls in flight, which the
 * cancellation answers at once. */
static void lookup_end(nightlight_dialog *n) {
    n->lookup = LOOKUP_IDLE;
    if (n->cancel != nullptr)
        g_cancellable_cancel(n->cancel);
    for (int i = 0; n->pending > 0 && i < 1000; i++)
        g_main_context_iteration(nullptr, TRUE);
    if (n->signal_id != 0)
        g_dbus_connection_signal_unsubscribe(n->bus, n->signal_id);
    if (n->started)
        g_dbus_connection_call(n->bus, GEOCLUE, n->client, "org.freedesktop.GeoClue2.Client",
                               "Stop", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                               nullptr, nullptr);
    if (n->bus != nullptr)
        g_dbus_connection_flush_sync(n->bus, nullptr, nullptr);
    if (n->pump != nullptr)
        xd_ui_remove_source(n->pump);
    g_clear_object(&n->cancel);
    g_clear_object(&n->bus);
    g_free(n->client);
}

/* ---- the dialog --------------------------------------------------------------- */

static void finish(nightlight_dialog *n, bool ok) {
    lookup_end(n);
    xd_window_close(n->win);
    nightlight_done done = n->done;
    void *user = n->user;
    xd_nightlight_settings edit = n->edit;
    xd_free(n);
    done(ok ? &edit : nullptr, user);
}

void nightlight_dialog_cancel(nightlight_dialog *n) { finish(n, false); }

static void on_cancel([[maybe_unused]] xd_widget *w, void *user) { finish(user, false); }

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    nightlight_dialog *n = user;
    xd_nightlight_settings *s = &n->edit;
    double lat = s->latitude, lon = s->longitude;
    degrees_read rl = read_degrees(n->lat, 90, &lat);
    degrees_read ro = read_degrees(n->lon, 180, &lon);
    if (rl == DEGREES_BAD || ro == DEGREES_BAD || (rl == DEGREES_EMPTY) != (ro == DEGREES_EMPTY)) {
        xd_widget_set_text(n->status, "Type a latitude from -90 to 90 and a longitude from "
                                      "-180 to 180, in degrees (north and east are positive).");
        xd_window_set_focus(n->win, rl != DEGREES_OK ? n->lat : n->lon);
        return;
    }
    s->enabled = xd_checkbox_checked(n->enable);
    s->temperature = kelvin_from_strength(xd_slider_value(n->strength));
    s->schedule = xd_checkbox_checked(n->always)   ? XD_NIGHTLIGHT_ALWAYS
                  : xd_checkbox_checked(n->sunset) ? XD_NIGHTLIGHT_SUNSET
                                                   : XD_NIGHTLIGHT_MANUAL;
    s->on_minute = clock_combo_value(n->on_combo, n->on_minutes);
    s->off_minute = clock_combo_value(n->off_combo, n->off_minutes);
    s->auto_location = xd_checkbox_checked(n->auto_location);
    s->has_location = rl == DEGREES_OK;
    if (s->has_location) {
        s->latitude = lat;
        s->longitude = lon;
    }
    finish(n, true);
}

static void on_strength(xd_widget *w, void *user) {
    (void)w;
    show_strength(user);
}

static void on_schedule(xd_widget *w, void *user) {
    nightlight_dialog *n = user;
    if (w == n->sunset && xd_checkbox_checked(n->auto_location))
        lookup_start(n);
    update_enabled(n);
}

static void on_auto_location(xd_widget *w, void *user) {
    nightlight_dialog *n = user;
    if (xd_checkbox_checked(w))
        lookup_start(n);
    update_enabled(n);
}

static void on_degrees_typed(xd_widget *w, void *user) {
    (void)w;
    update_status(user);
}

nightlight_dialog *nightlight_dialog_open(xd_ui *ui, const xd_nightlight_settings *current,
                                          nightlight_done done, void *user) {
    nightlight_dialog *n = XD_NEW(nightlight_dialog);
    n->ui = ui;
    n->edit = *current;
    n->done = done;
    n->user = user;
    const int W = 320, H = 424;
    xd_window *win = n->win = xd_window_create(ui, "Night Light", "xd-desk", W, H);

    n->enable = xd_checkbox(win, (xd_rect){12, 10, 296, 20}, "Turn on night light",
                            current->enabled, nullptr, nullptr);
    xd_label(win, (xd_rect){12, 34, 296, 28},
             "Night light shows warmer colors on the screen at night, which are easier on "
             "the eyes.",
             XD_LABEL_WRAP);

    xd_groupbox(win, (xd_rect){12, 70, 296, 74}, "Strength");
    n->strength = xd_slider(win, (xd_rect){22, 88, 276, 26}, 0,
                            strength_from_kelvin(XD_NIGHTLIGHT_K_MIN),
                            strength_from_kelvin(current->temperature), on_strength, n);
    xd_label(win, (xd_rect){26, 118, 60, 14}, "Weaker", 0);
    n->strength_value = xd_label(win, (xd_rect){126, 118, 70, 14}, "", 0);
    xd_label(win, (xd_rect){246, 118, 54, 14}, "Stronger", 0);
    show_strength(n);

    xd_groupbox(win, (xd_rect){12, 152, 296, 112}, "Schedule");
    n->always = xd_radio(win, (xd_rect){24, 170, 270, 20}, "All the time", true, on_schedule, n);
    n->sunset = xd_radio(win, (xd_rect){24, 192, 270, 20}, "Sunset to sunrise", false,
                         on_schedule, n);
    n->manual = xd_radio(win, (xd_rect){24, 214, 270, 20}, "Set hours:", false, on_schedule, n);
    xd_radio_set(current->schedule == XD_NIGHTLIGHT_ALWAYS   ? n->always
                 : current->schedule == XD_NIGHTLIGHT_SUNSET ? n->sunset
                                                             : n->manual);
    n->on_label = xd_label(win, (xd_rect){42, 240, 52, 16}, "Turn on:", 0);
    n->on_combo = xd_combo(win, (xd_rect){94, 237, 76, 21}, false, nullptr, nullptr);
    clock_combo_fill(n->on_combo, n->on_minutes, current->on_minute);
    n->off_label = xd_label(win, (xd_rect){178, 240, 52, 16}, "Turn off:", 0);
    n->off_combo = xd_combo(win, (xd_rect){228, 237, 76, 21}, false, nullptr, nullptr);
    clock_combo_fill(n->off_combo, n->off_minutes, current->off_minute);

    xd_groupbox(win, (xd_rect){12, 272, 296, 108}, "Location (for sunset and sunrise)");
    n->auto_location = xd_checkbox(win, (xd_rect){24, 290, 272, 20},
                                   "Find my location automatically", current->auto_location,
                                   on_auto_location, n);
    n->lat_label = xd_label(win, (xd_rect){24, 319, 54, 16}, "Latitude:", 0);
    n->lat = xd_edit(win, (xd_rect){80, 316, 70, 21}, "");
    n->lon_label = xd_label(win, (xd_rect){162, 319, 62, 16}, "Longitude:", 0);
    n->lon = xd_edit(win, (xd_rect){226, 316, 70, 21}, "");
    xd_widget_set_tooltip(n->lat, "Degrees north; south is negative");
    xd_widget_set_tooltip(n->lon, "Degrees east; west is negative");
    if (current->has_location) {
        set_degrees(n->lat, current->latitude);
        set_degrees(n->lon, current->longitude);
    }
    xd_edit_set_on_change(n->lat, on_degrees_typed, n);
    xd_edit_set_on_change(n->lon, on_degrees_typed, n);
    n->status = xd_label(win, (xd_rect){24, 344, 276, 30}, "", XD_LABEL_WRAP);

    xd_widget *ok = xd_button(win, (xd_rect){W - 168, H - 33, 75, 23}, "OK", on_ok, n);
    xd_widget *cancel = xd_button(win, (xd_rect){W - 87, H - 33, 75, 23}, "Cancel", on_cancel, n);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, n->enable);

    /* A fresh look at where we are, as Windows' location services would. */
    if (current->schedule == XD_NIGHTLIGHT_SUNSET && current->auto_location)
        lookup_start(n);
    update_enabled(n);
    xd_window_show(win);
    return n;
}
