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
    int hour = minute / 60 % 24, minute_of_hour = minute % 60;
    snprintf(buf, cap, "%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, minute_of_hour,
             hour < 12 ? "AM" : "PM");
}

static void clock_combo_fill(xd_widget *combo, int *minutes, int current) {
    int count = 0, sel = 0;
    for (int slot = 0; slot < 24 * 60; slot += 30) {
        if (current > slot - 30 && current < slot && current % 30 != 0) {
            sel = count;
            minutes[count++] = current;
        }
        if (slot == current)
            sel = count;
        minutes[count++] = slot;
    }
    if (current > 23 * 60 + 30) {
        sel = count;
        minutes[count++] = current;
    }
    for (int i = 0; i < count; i++) {
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

static int kelvin_from_strength(int strength) { return XD_NIGHTLIGHT_K_MAX - strength * 100; }

static void show_strength(nightlight_dialog *night_light) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d K",
             kelvin_from_strength(xd_slider_value(night_light->strength)));
    xd_widget_set_text(night_light->strength_value, buf);
}

/* Reads a degrees edit; "" leaves *out alone and says so. */
typedef enum degrees_read : uint8_t { DEGREES_OK, DEGREES_EMPTY, DEGREES_BAD } degrees_read;
static degrees_read read_degrees(xd_widget *edit, double limit, double *out) {
    char *text = g_strstrip(g_strdup(xd_widget_text(edit)));
    degrees_read status = DEGREES_EMPTY;
    if (text[0] != '\0') {
        char *end = nullptr;
        double degrees = g_ascii_strtod(text, &end);
        status = end != text && *end == '\0' && fabs(degrees) <= limit ? DEGREES_OK : DEGREES_BAD;
        if (status == DEGREES_OK)
            *out = degrees;
    }
    g_free(text);
    return status;
}

static void set_degrees(xd_widget *edit, double degrees) {
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    xd_widget_set_text(edit, g_ascii_formatd(buf, sizeof(buf), "%.4f", degrees));
}

/* The status line under the location: what's happening with GeoClue, or
 * tonight's times. */
static void update_status(nightlight_dialog *night_light) {
    if (!xd_checkbox_checked(night_light->sunset)) {
        xd_widget_set_text(night_light->status, "");
        return;
    }
    bool automatic = xd_checkbox_checked(night_light->auto_location);
    if (automatic && night_light->lookup == LOOKUP_FINDING) {
        xd_widget_set_text(night_light->status, "Finding your location...");
        return;
    }
    xd_nightlight_settings nightlight_settings = night_light->edit;
    nightlight_settings.schedule = XD_NIGHTLIGHT_SUNSET;
    nightlight_settings.has_location =
        read_degrees(night_light->lat, 90, &nightlight_settings.latitude) == DEGREES_OK &&
        read_degrees(night_light->lon, 180, &nightlight_settings.longitude) == DEGREES_OK;
    const char *prefix = automatic && night_light->lookup == LOOKUP_FAILED
                             ? "Your location couldn't be found. "
                             : "";
    char buf[192];
    if (!nightlight_settings.has_location) {
        snprintf(buf, sizeof(buf), "%sUntil there is a location, night light keeps to %s.",
                 prefix, "the set hours");
        xd_widget_set_text(night_light->status, buf);
        return;
    }
    double on, off;
    switch (xd_nightlight_times(&nightlight_settings, time(nullptr), &on, &off)) {
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
    xd_widget_set_text(night_light->status, buf);
}

/* Greys what the choices above make meaningless. */
static void update_enabled(nightlight_dialog *night_light) {
    bool manual = xd_checkbox_checked(night_light->manual);
    bool sunset = xd_checkbox_checked(night_light->sunset);
    bool automatic = xd_checkbox_checked(night_light->auto_location);
    xd_widget_set_enabled(night_light->on_label, manual);
    xd_widget_set_enabled(night_light->on_combo, manual);
    xd_widget_set_enabled(night_light->off_label, manual);
    xd_widget_set_enabled(night_light->off_combo, manual);
    xd_widget_set_enabled(night_light->auto_location, sunset);
    /* Typing is for when GeoClue isn't asked, or couldn't answer. */
    bool typing = sunset && (!automatic || night_light->lookup == LOOKUP_FAILED);
    xd_widget_set_enabled(night_light->lat_label, typing);
    xd_widget_set_enabled(night_light->lat, typing);
    xd_widget_set_enabled(night_light->lon_label, typing);
    xd_widget_set_enabled(night_light->lon, typing);
    update_status(night_light);
}

/* ---- GeoClue ------------------------------------------------------------------ */

static void lookup_failed(nightlight_dialog *night_light, const char *why) {
    if (night_light->lookup != LOOKUP_FINDING)
        return;
    g_message("GeoClue: %s", why);
    night_light->lookup = LOOKUP_FAILED;
    update_enabled(night_light);
}

/* Turns GLib's main context while replies are due. */
static void on_pump(void *user) {
    nightlight_dialog *night_light = user;
    for (int i = 0; i < 16 && g_main_context_iteration(nullptr, FALSE); i++) {
    }
    if (night_light->lookup == LOOKUP_FINDING &&
        time(nullptr) - night_light->lookup_start > LOOKUP_TIMEOUT_S)
        lookup_failed(night_light, "no location in time");
    if (night_light->lookup != LOOKUP_FINDING && night_light->pending == 0 &&
        night_light->pump != nullptr) {
        xd_ui_remove_source(night_light->pump);
        night_light->pump = nullptr;
    }
}

/* A reply arrived: false if the dialog is closing (cancelled) or the call
 * failed, which then ends the lookup. */
static bool reply_ok(nightlight_dialog *night_light, GObject *source, GAsyncResult *res,
                     GVariant **out, const char *what) {
    night_light->pending--;
    GError *err = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (variant == nullptr) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            char *why = g_strdup_printf("%s: %s", what, err->message);
            lookup_failed(night_light, why);
            g_free(why);
        }
        g_error_free(err);
        return false;
    }
    if (out != nullptr)
        *out = variant;
    else
        g_variant_unref(variant);
    return night_light->lookup == LOOKUP_FINDING;
}

static void call(nightlight_dialog *night_light, const char *path, const char *iface,
                 const char *method, GVariant *args, const GVariantType *reply,
                 GAsyncReadyCallback cb) {
    night_light->pending++;
    g_dbus_connection_call(night_light->bus, GEOCLUE, path, iface, method, args, reply,
                           G_DBUS_CALL_FLAGS_NONE, 10000, night_light->cancel, cb, night_light);
}

static void on_location(GObject *source, GAsyncResult *res, gpointer user) {
    nightlight_dialog *night_light = user;
    GVariant *variant = nullptr;
    if (!reply_ok(night_light, source, res, &variant, "Location"))
        return;
    GVariant *props = g_variant_get_child_value(variant, 0);
    double lat = 999, lon = 999; /* out of range until read */
    if (!g_variant_lookup(props, "Latitude", "d", &lat) ||
        !g_variant_lookup(props, "Longitude", "d", &lon) || !(fabs(lat) <= 90) ||
        !(fabs(lon) <= 180)) {
        lookup_failed(night_light, "a location without coordinates");
    } else {
        set_degrees(night_light->lat, lat);
        set_degrees(night_light->lon, lon);
        night_light->lookup = LOOKUP_FOUND;
        update_enabled(night_light);
    }
    g_variant_unref(props);
    g_variant_unref(variant);
    /* One answer is all night light needs. */
    if (night_light->started) {
        g_dbus_connection_call(night_light->bus, GEOCLUE, night_light->client,
                               "org.freedesktop.GeoClue2.Client", "Stop", nullptr, nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
        night_light->started = false;
    }
}

static void on_location_updated([[maybe_unused]] GDBusConnection *bus,
                                [[maybe_unused]] const char *sender,
                                [[maybe_unused]] const char *path,
                                [[maybe_unused]] const char *iface,
                                [[maybe_unused]] const char *signal, GVariant *params,
                                gpointer user) {
    nightlight_dialog *night_light = user;
    if (night_light->lookup != LOOKUP_FINDING ||
        !g_variant_is_of_type(params, G_VARIANT_TYPE("(oo)")))
        return;
    const char *location = nullptr;
    g_variant_get(params, "(&o&o)", nullptr, &location);
    call(night_light, location, "org.freedesktop.DBus.Properties", "GetAll",
         g_variant_new("(s)", "org.freedesktop.GeoClue2.Location"), G_VARIANT_TYPE("(a{sv})"),
         on_location);
}

static void on_started(GObject *source, GAsyncResult *res, gpointer user) {
    nightlight_dialog *night_light = user;
    if (reply_ok(night_light, source, res, nullptr, "Start"))
        night_light->started = true;
}

static void on_property_set(GObject *source, GAsyncResult *res, gpointer user) {
    reply_ok(user, source, res, nullptr, "setting up the client");
}

static void on_client(GObject *source, GAsyncResult *res, gpointer user) {
    nightlight_dialog *night_light = user;
    GVariant *variant = nullptr;
    if (!reply_ok(night_light, source, res, &variant, "GetClient"))
        return;
    g_variant_get(variant, "(o)", &night_light->client);
    g_variant_unref(variant);
    night_light->signal_id = g_dbus_connection_signal_subscribe(
        night_light->bus, GEOCLUE, "org.freedesktop.GeoClue2.Client", "LocationUpdated",
        night_light->client, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_location_updated, night_light,
        nullptr);
    /* GeoClue asks who's asking (its agent, or geoclue.conf's list of
     * programs allowed without one). Calls on one connection arrive in
     * order. */
    const char *iface = "org.freedesktop.GeoClue2.Client";
    call(night_light, night_light->client, "org.freedesktop.DBus.Properties", "Set",
         g_variant_new("(ssv)", iface, "DesktopId", g_variant_new_string("xd-desk")), nullptr,
         on_property_set);
    call(night_light, night_light->client, "org.freedesktop.DBus.Properties", "Set",
         g_variant_new("(ssv)", iface, "RequestedAccuracyLevel",
                       g_variant_new_uint32(ACCURACY_CITY)),
         nullptr, on_property_set);
    call(night_light, night_light->client, iface, "Start", nullptr, nullptr, on_started);
}

static void on_bus(GObject *source, GAsyncResult *res, gpointer user) {
    (void)source;
    nightlight_dialog *night_light = user;
    night_light->pending--;
    GError *err = nullptr;
    night_light->bus = g_bus_get_finish(res, &err);
    if (night_light->bus == nullptr) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            lookup_failed(night_light, err->message);
        g_error_free(err);
        return;
    }
    if (night_light->lookup != LOOKUP_FINDING)
        return;
    call(night_light, "/org/freedesktop/GeoClue2/Manager", "org.freedesktop.GeoClue2.Manager",
         "GetClient", nullptr, G_VARIANT_TYPE("(o)"), on_client);
}

static void lookup_start(nightlight_dialog *night_light) {
    if (night_light->lookup == LOOKUP_FINDING || night_light->lookup == LOOKUP_FOUND)
        return;
    night_light->lookup = LOOKUP_FINDING;
    night_light->lookup_start = time(nullptr);
    if (night_light->cancel == nullptr)
        night_light->cancel = g_cancellable_new();
    if (night_light->bus == nullptr) {
        night_light->pending++;
        g_bus_get(G_BUS_TYPE_SYSTEM, night_light->cancel, on_bus, night_light);
    } else if (night_light->client == nullptr) {
        call(night_light, "/org/freedesktop/GeoClue2/Manager", "org.freedesktop.GeoClue2.Manager",
             "GetClient", nullptr, G_VARIANT_TYPE("(o)"), on_client);
    } else {
        call(night_light, night_light->client, "org.freedesktop.GeoClue2.Client", "Start", nullptr,
             nullptr, on_started);
    }
    if (night_light->pump == nullptr)
        night_light->pump = xd_ui_add_timer(night_light->ui, PUMP_MS, true, on_pump, night_light);
}

/* Stops the lookup and waits out the calls in flight, which the
 * cancellation answers at once. */
static void lookup_end(nightlight_dialog *night_light) {
    night_light->lookup = LOOKUP_IDLE;
    if (night_light->cancel != nullptr)
        g_cancellable_cancel(night_light->cancel);
    for (int i = 0; night_light->pending > 0 && i < 1000; i++)
        g_main_context_iteration(nullptr, TRUE);
    if (night_light->signal_id != 0)
        g_dbus_connection_signal_unsubscribe(night_light->bus, night_light->signal_id);
    if (night_light->started)
        g_dbus_connection_call(night_light->bus, GEOCLUE, night_light->client,
                               "org.freedesktop.GeoClue2.Client", "Stop", nullptr, nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    if (night_light->bus != nullptr)
        g_dbus_connection_flush_sync(night_light->bus, nullptr, nullptr);
    if (night_light->pump != nullptr)
        xd_ui_remove_source(night_light->pump);
    g_clear_object(&night_light->cancel);
    g_clear_object(&night_light->bus);
    g_free(night_light->client);
}

/* ---- the dialog --------------------------------------------------------------- */

static void finish(nightlight_dialog *night_light, bool ok) {
    lookup_end(night_light);
    xd_window_close(night_light->win);
    nightlight_done done = night_light->done;
    void *user = night_light->user;
    xd_nightlight_settings edit = night_light->edit;
    xd_free(night_light);
    done(ok ? &edit : nullptr, user);
}

void nightlight_dialog_cancel(nightlight_dialog *night_light) { finish(night_light, false); }

static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) { finish(user, false); }

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) {
    nightlight_dialog *night_light = user;
    xd_nightlight_settings *nightlight_settings = &night_light->edit;
    double lat = nightlight_settings->latitude, lon = nightlight_settings->longitude;
    degrees_read rl = read_degrees(night_light->lat, 90, &lat);
    degrees_read ro = read_degrees(night_light->lon, 180, &lon);
    if (rl == DEGREES_BAD || ro == DEGREES_BAD || (rl == DEGREES_EMPTY) != (ro == DEGREES_EMPTY)) {
        xd_widget_set_text(night_light->status,
                           "Type a latitude from -90 to 90 and a longitude from "
                           "-180 to 180, in degrees (north and east are positive).");
        xd_window_set_focus(night_light->win,
                            rl != DEGREES_OK ? night_light->lat : night_light->lon);
        return;
    }
    nightlight_settings->enabled = xd_checkbox_checked(night_light->enable);
    nightlight_settings->temperature = kelvin_from_strength(xd_slider_value(night_light->strength));
    nightlight_settings->schedule = xd_checkbox_checked(night_light->always) ? XD_NIGHTLIGHT_ALWAYS
                                    : xd_checkbox_checked(night_light->sunset)
                                        ? XD_NIGHTLIGHT_SUNSET
                                        : XD_NIGHTLIGHT_MANUAL;
    nightlight_settings->on_minute =
        clock_combo_value(night_light->on_combo, night_light->on_minutes);
    nightlight_settings->off_minute =
        clock_combo_value(night_light->off_combo, night_light->off_minutes);
    nightlight_settings->auto_location = xd_checkbox_checked(night_light->auto_location);
    nightlight_settings->has_location = rl == DEGREES_OK;
    if (nightlight_settings->has_location) {
        nightlight_settings->latitude = lat;
        nightlight_settings->longitude = lon;
    }
    finish(night_light, true);
}

static void on_strength(xd_widget *widget, void *user) {
    (void)widget;
    show_strength(user);
}

static void on_schedule(xd_widget *widget, void *user) {
    nightlight_dialog *night_light = user;
    if (widget == night_light->sunset && xd_checkbox_checked(night_light->auto_location))
        lookup_start(night_light);
    update_enabled(night_light);
}

static void on_auto_location(xd_widget *widget, void *user) {
    nightlight_dialog *night_light = user;
    if (xd_checkbox_checked(widget))
        lookup_start(night_light);
    update_enabled(night_light);
}

static void on_degrees_typed(xd_widget *widget, void *user) {
    (void)widget;
    update_status(user);
}

nightlight_dialog *nightlight_dialog_open(xd_ui *ui, const xd_nightlight_settings *current,
                                          nightlight_done done, void *user) {
    nightlight_dialog *night_light = XD_NEW(nightlight_dialog);
    night_light->ui = ui;
    night_light->edit = *current;
    night_light->done = done;
    night_light->user = user;
    const int width = 320, height = 424;
    xd_window *win = night_light->win =
        xd_window_create(ui, "Night Light", "xd-desk", width, height);

    night_light->enable = xd_checkbox(win, (xd_rect){12, 10, 296, 20}, "Turn on night light",
                                      current->enabled, nullptr, nullptr);
    xd_label(win, (xd_rect){12, 34, 296, 28},
             "Night light shows warmer colors on the screen at night, which are easier on "
             "the eyes.",
             XD_LABEL_WRAP);

    xd_groupbox(win, (xd_rect){12, 70, 296, 74}, "Strength");
    night_light->strength =
        xd_slider(win, (xd_rect){22, 88, 276, 26}, 0, strength_from_kelvin(XD_NIGHTLIGHT_K_MIN),
                  strength_from_kelvin(current->temperature), on_strength, night_light);
    xd_label(win, (xd_rect){26, 118, 60, 14}, "Weaker", 0);
    night_light->strength_value = xd_label(win, (xd_rect){126, 118, 70, 14}, "", 0);
    xd_label(win, (xd_rect){246, 118, 54, 14}, "Stronger", 0);
    show_strength(night_light);

    xd_groupbox(win, (xd_rect){12, 152, 296, 112}, "Schedule");
    night_light->always =
        xd_radio(win, (xd_rect){24, 170, 270, 20}, "All the time", true, on_schedule, night_light);
    night_light->sunset = xd_radio(win, (xd_rect){24, 192, 270, 20}, "Sunset to sunrise", false,
                                   on_schedule, night_light);
    night_light->manual =
        xd_radio(win, (xd_rect){24, 214, 270, 20}, "Set hours:", false, on_schedule, night_light);
    xd_radio_set(current->schedule == XD_NIGHTLIGHT_ALWAYS   ? night_light->always
                 : current->schedule == XD_NIGHTLIGHT_SUNSET ? night_light->sunset
                                                             : night_light->manual);
    night_light->on_label = xd_label(win, (xd_rect){42, 240, 52, 16}, "Turn on:", 0);
    night_light->on_combo = xd_combo(win, (xd_rect){94, 237, 76, 21}, false, nullptr, nullptr);
    clock_combo_fill(night_light->on_combo, night_light->on_minutes, current->on_minute);
    night_light->off_label = xd_label(win, (xd_rect){178, 240, 52, 16}, "Turn off:", 0);
    night_light->off_combo = xd_combo(win, (xd_rect){228, 237, 76, 21}, false, nullptr, nullptr);
    clock_combo_fill(night_light->off_combo, night_light->off_minutes, current->off_minute);

    xd_groupbox(win, (xd_rect){12, 272, 296, 108}, "Location (for sunset and sunrise)");
    night_light->auto_location =
        xd_checkbox(win, (xd_rect){24, 290, 272, 20}, "Find my location automatically",
                    current->auto_location, on_auto_location, night_light);
    night_light->lat_label = xd_label(win, (xd_rect){24, 319, 54, 16}, "Latitude:", 0);
    night_light->lat = xd_edit(win, (xd_rect){80, 316, 70, 21}, "");
    night_light->lon_label = xd_label(win, (xd_rect){162, 319, 62, 16}, "Longitude:", 0);
    night_light->lon = xd_edit(win, (xd_rect){226, 316, 70, 21}, "");
    xd_widget_set_tooltip(night_light->lat, "Degrees north; south is negative");
    xd_widget_set_tooltip(night_light->lon, "Degrees east; west is negative");
    if (current->has_location) {
        set_degrees(night_light->lat, current->latitude);
        set_degrees(night_light->lon, current->longitude);
    }
    xd_edit_set_on_change(night_light->lat, on_degrees_typed, night_light);
    xd_edit_set_on_change(night_light->lon, on_degrees_typed, night_light);
    night_light->status = xd_label(win, (xd_rect){24, 344, 276, 30}, "", XD_LABEL_WRAP);

    xd_widget *ok =
        xd_button(win, (xd_rect){width - 168, height - 33, 75, 23}, "OK", on_ok, night_light);
    xd_widget *cancel = xd_button(win, (xd_rect){width - 87, height - 33, 75, 23}, "Cancel",
                                  on_cancel, night_light);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, night_light->enable);

    /* A fresh look at where we are, as Windows' location services would. */
    if (current->schedule == XD_NIGHTLIGHT_SUNSET && current->auto_location)
        lookup_start(night_light);
    update_enabled(night_light);
    xd_window_show(win);
    return night_light;
}
