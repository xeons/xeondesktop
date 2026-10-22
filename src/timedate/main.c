/* SPDX-License-Identifier: MIT
 * xd-timedate: Windows XP's Date and Time Properties, through systemd's
 * timedated (org.freedesktop.timedate1). Changes go through polkit, so
 * xd-polkit asks for a password when needed.
 *
 *   Date & Time    the month, year and day; an analog clock and the time
 *   Time Zone      the zone timedated knows; whether the hardware clock
 *                  keeps local time (timedated's LocalRTC)
 *   Internet Time  network time synchronization: on or off through
 *                  timedated; systemd-timesyncd's server and Update Now
 *                  through timesyncd (org.freedesktop.timesync1) and
 *                  systemd, when timesyncd is the time service
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <json-glib/json-glib.h>

#include "../core/common.h"
#include "../settings/settings.h"
#include "../gtkapp/gtkapp.h"
#include "ntp.h"

#define TIMEDATE "org.freedesktop.timedate1"
#define TIMEDATE_PATH "/org/freedesktop/timedate1"
#define TIMESYNC "org.freedesktop.timesync1"
#define TIMESYNC_PATH "/org/freedesktop/timesync1"
#define TIMESYNC_MANAGER "org.freedesktop.timesync1.Manager"
#define TIMESYNC_UNIT "systemd-timesyncd.service"

/* How long Update Now waits for the server's answer. */
static constexpr guint SYNC_SECONDS = 15;

typedef struct props {
    GtkWidget *window, *apply;
    GDBusProxy *td;
    GDBusProxy *ts;      /* timesyncd, whether it runs or not */
    GDBusProxy *systemd; /* for restarting timesyncd, made when needed */
    bool ts_activatable; /* the bus starts timesyncd when asked: it's installed */
    /* Date & Time */
    GtkWidget *month, *year, *calendar, *clock, *hour, *minute, *second, *zone_label;
    bool time_edited, date_edited;
    bool updating; /* we're setting the widgets ourselves */
    bool busy;      /* Apply is waiting for the service */
    /* Time Zone */
    GtkWidget *zones, *map, *local_rtc;
    char *zone; /* as timedated has it */
    /* Internet Time */
    GtkWidget *ntp, *server, *update, *ntp_status, *ntp_note;
    bool server_edited;
    gint64 sync_since; /* Update Now waits for an answer received after this (0: it doesn't) */
    char *sync_server;
    guint sync_timeout;
    char *sync_failed; /* why the last Update Now failed, shown until the next */
} props;

static void changed(props *properties) { gtk_widget_set_sensitive(properties->apply, TRUE); }

static GVariant *td_prop(props *properties, const char *name) {
    return properties->td != nullptr ? g_dbus_proxy_get_cached_property(properties->td, name)
                                     : nullptr;
}

static bool td_bool(props *properties, const char *name) {
    GVariant *variant = td_prop(properties, name);
    bool value = variant != nullptr && g_variant_get_boolean(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

/* ---- Date & Time ------------------------------------------------------------- */

static int spin(GtkWidget *widget) {
    return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}

static void set_spin(GtkWidget *widget, int value) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value);
}

static void show_time(props *properties, GDateTime *now) {
    properties->updating = true;
    set_spin(properties->hour, g_date_time_get_hour(now));
    set_spin(properties->minute, g_date_time_get_minute(now));
    set_spin(properties->second, g_date_time_get_second(now));
    properties->updating = false;
    gtk_widget_queue_draw(properties->clock);
}

static void show_date(props *properties, GDateTime *now) {
    properties->updating = true;
    int y = g_date_time_get_year(now), month = g_date_time_get_month(now);
    gtk_combo_box_set_active(GTK_COMBO_BOX(properties->month), month - 1);
    set_spin(properties->year, y);
    gtk_calendar_select_month(GTK_CALENDAR(properties->calendar), (guint)(month - 1), (guint)y);
    gtk_calendar_select_day(GTK_CALENDAR(properties->calendar),
                            (guint)g_date_time_get_day_of_month(now));
    properties->updating = false;
}

static void show_zone_label(props *properties) {
    GDateTime *now = g_date_time_new_now_local();
    char *text = g_strdup_printf("Current time zone: %s (%s)",
                                 properties->zone != nullptr ? properties->zone : "?",
                                 g_date_time_get_timezone_abbreviation(now));
    gtk_label_set_text(GTK_LABEL(properties->zone_label), text);
    g_free(text);
    g_date_time_unref(now);
}

/* The clock runs until the time is edited, as in Windows. */
static gboolean tick(gpointer data) {
    props *properties = data;
    GDateTime *now = g_date_time_new_now_local();
    if (!properties->time_edited)
        show_time(properties, now);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

static void on_time_changed([[maybe_unused]] GtkSpinButton *spin, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    properties->time_edited = true;
    gtk_widget_queue_draw(properties->clock);
    changed(properties);
}

static void on_month_year([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    int month = gtk_combo_box_get_active(GTK_COMBO_BOX(properties->month));
    int y = spin(properties->year);
    guint day = 0;
    gtk_calendar_get_date(GTK_CALENDAR(properties->calendar), nullptr, nullptr, &day);
    properties->updating = true;
    gtk_calendar_select_month(GTK_CALENDAR(properties->calendar), (guint)XD_MAX(month, 0),
                              (guint)y);
    /* Keep the day, short of a shorter month's end. */
    guint last = (guint)g_date_get_days_in_month((GDateMonth)(month + 1), (GDateYear)y);
    gtk_calendar_select_day(GTK_CALENDAR(properties->calendar), day < last ? day : last);
    properties->updating = false;
    properties->date_edited = true;
    changed(properties);
}

static void on_day([[maybe_unused]] GtkCalendar *calendar, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    properties->date_edited = true;
    changed(properties);
}

/* The analog clock: a face with hour marks and three hands. */
static gboolean draw_clock(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    props *properties = data;
    double size =
        XD_MIN(gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
    double cx = gtk_widget_get_allocated_width(widget) / 2.0;
    double cy = gtk_widget_get_allocated_height(widget) / 2.0;
    double radius = size / 2 - 4;
    cairo_arc(cairo, cx, cy, radius, 0, 2 * XD_PI);
    cairo_pattern_t *face =
        cairo_pattern_create_radial(cx - radius / 3, cy - radius / 3, radius / 6, cx, cy, radius);
    cairo_pattern_add_color_stop_rgb(face, 0, 1, 1, 1);
    cairo_pattern_add_color_stop_rgb(face, 1, 0.82, 0.86, 0.92);
    cairo_set_source(cairo, face);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(face);
    cairo_set_source_rgb(cairo, 0.25, 0.32, 0.45);
    cairo_set_line_width(cairo, 2);
    cairo_stroke(cairo);
    for (int i = 0; i < 60; i++) {
        double mark_angle = i * XD_PI / 30;
        bool hour = i % 5 == 0;
        double r0 = radius - (hour ? 9 : 4);
        cairo_move_to(cairo, cx + r0 * sin(mark_angle), cy - r0 * cos(mark_angle));
        cairo_line_to(cairo, cx + (radius - 2) * sin(mark_angle),
                      cy - (radius - 2) * cos(mark_angle));
        cairo_set_line_width(cairo, hour ? 2.5 : 1);
        cairo_set_source_rgb(cairo, hour ? 0.1 : 0.4, hour ? 0.2 : 0.45, hour ? 0.5 : 0.55);
        cairo_stroke(cairo);
    }
    int shown_hour = spin(properties->hour), shown_minute = spin(properties->minute),
        shown_second = spin(properties->second);
    struct {
        double angle, length, width, red;
    } hands[] = {
        {(shown_hour % 12 + shown_minute / 60.0) * XD_PI / 6, radius * 0.5, 4, 0.1},
        {(shown_minute + shown_second / 60.0) * XD_PI / 30, radius * 0.75, 3, 0.1},
        {shown_second * XD_PI / 30, radius * 0.85, 1, 0.75}, /* the red second hand */
    };
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    for (size_t i = 0; i < XD_ARRAY_LEN(hands); i++) {
        cairo_move_to(cairo, cx, cy);
        cairo_line_to(cairo, cx + hands[i].length * sin(hands[i].angle),
                      cy - hands[i].length * cos(hands[i].angle));
        cairo_set_line_width(cairo, hands[i].width);
        bool second = i == 2;
        cairo_set_source_rgb(cairo, hands[i].red, second ? 0.1 : 0.15, second ? 0.1 : 0.3);
        cairo_stroke(cairo);
    }
    cairo_arc(cairo, cx, cy, 3, 0, 2 * XD_PI);
    cairo_fill(cairo);
    return FALSE;
}

static gboolean two_digits(GtkSpinButton *spin, [[maybe_unused]] gpointer data) {
    char text[8];
    g_snprintf(text, sizeof(text), "%02d", gtk_spin_button_get_value_as_int(spin));
    gtk_entry_set_text(GTK_ENTRY(spin), text);
    return TRUE;
}

static GtkWidget *time_spin(props *properties, int max) {
    GtkWidget *widget = gtk_spin_button_new_with_range(0, max, 1);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(widget), TRUE);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widget), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(widget), 2);
    g_signal_connect(widget, "value-changed", G_CALLBACK(on_time_changed), properties);
    /* Two digits, as a clock shows them. */
    g_signal_connect(widget, "output", G_CALLBACK(two_digits), nullptr);
    return widget;
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(widget), child);
    return widget;
}

static GtkWidget *date_time_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);

    GtkWidget *date = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *my = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    properties->month = gtk_combo_box_text_new();
    for (int month = 1; month <= 12; month++) {
        GDateTime *date_time = g_date_time_new_local(2000, month, 1, 0, 0, 0);
        char *name = g_date_time_format(date_time, "%B");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(properties->month), name);
        g_free(name);
        g_date_time_unref(date_time);
    }
    properties->year = gtk_spin_button_new_with_range(1970, 2199, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(properties->year), TRUE);
    gtk_box_pack_start(GTK_BOX(my), properties->month, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(my), properties->year, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(date), my, FALSE, FALSE, 0);
    properties->calendar = gtk_calendar_new();
    gtk_calendar_set_display_options(GTK_CALENDAR(properties->calendar),
                                     GTK_CALENDAR_SHOW_DAY_NAMES);
    gtk_box_pack_start(GTK_BOX(date), properties->calendar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), frame("Date", date), TRUE, TRUE, 0);

    GtkWidget *time = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    properties->clock = gtk_drawing_area_new();
    gtk_widget_set_size_request(properties->clock, 130, 130);
    g_signal_connect(properties->clock, "draw", G_CALLBACK(draw_clock), properties);
    gtk_box_pack_start(GTK_BOX(time), properties->clock, TRUE, TRUE, 0);
    GtkWidget *hms = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_halign(hms, GTK_ALIGN_CENTER);
    properties->hour = time_spin(properties, 23);
    properties->minute = time_spin(properties, 59);
    properties->second = time_spin(properties, 59);
    gtk_box_pack_start(GTK_BOX(hms), properties->hour, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hms), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hms), properties->minute, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hms), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hms), properties->second, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time), hms, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), frame("Time", time), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, TRUE, TRUE, 0);

    properties->zone_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(properties->zone_label), 0.5f);
    gtk_box_pack_start(GTK_BOX(page), properties->zone_label, FALSE, FALSE, 0);

    GDateTime *now = g_date_time_new_now_local();
    show_date(properties, now);
    show_time(properties, now);
    g_date_time_unref(now);
    g_signal_connect(properties->month, "changed", G_CALLBACK(on_month_year), properties);
    g_signal_connect(properties->year, "value-changed", G_CALLBACK(on_month_year), properties);
    g_signal_connect(properties->calendar, "day-selected", G_CALLBACK(on_day), properties);
    return page;
}

/* ---- Time Zone ------------------------------------------------------------- */

/* XP's map, from Natural Earth's land polygons (public domain), in an
 * equirectangular projection without Antarctica; the time zone's band
 * (by its current UTC offset) is lit, its city marked. */
static constexpr double MAP_NORTH = 84.0;
static constexpr double MAP_SOUTH = -58.0;

static JsonNode *land; /* the GeoJSON, loaded once */

static void load_land(void) {
    xd_strbuf dir = {0};
    if (xd_data_dir(&dir)) {
        char *path = g_build_filename(xd_strbuf_cstr(&dir), "maps", "ne_110m_land.geojson", nullptr);
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_file(parser, path, nullptr))
            land = json_node_copy(json_parser_get_root(parser));
        g_object_unref(parser);
        g_free(path);
    }
    xd_strbuf_free(&dir);
}

static void map_point(double lon, double lat, double width, double height, double *map_x,
                      double *map_y) {
    *map_x = (lon + 180.0) / 360.0 * width;
    *map_y = (MAP_NORTH - lat) / (MAP_NORTH - MAP_SOUTH) * height;
}

/* One ring of [lon, lat] pairs as a closed path. */
static void ring_path(cairo_t *cairo, JsonArray *ring, double width, double height) {
    guint count = json_array_get_length(ring);
    for (guint i = 0; i < count; i++) {
        JsonArray *pt = json_array_get_array_element(ring, i);
        if (pt == nullptr || json_array_get_length(pt) < 2)
            continue;
        double x, y;
        map_point(json_array_get_double_element(pt, 0), json_array_get_double_element(pt, 1), width,
                  height, &x, &y);
        if (i == 0)
            cairo_move_to(cairo, x, y);
        else
            cairo_line_to(cairo, x, y);
    }
    cairo_close_path(cairo);
}

static void polygon_path(cairo_t *cairo, JsonArray *rings, double width, double height) {
    for (guint ring_index = 0; ring_index < json_array_get_length(rings); ring_index++) {
        JsonArray *ring = json_array_get_array_element(rings, ring_index);
        if (ring != nullptr)
            ring_path(cairo, ring, width, height);
    }
}

static void land_path(cairo_t *cairo, double width, double height) {
    JsonObject *root = land != nullptr && JSON_NODE_HOLDS_OBJECT(land) ? json_node_get_object(land)
                                                                       : nullptr;
    JsonArray *features = root != nullptr && json_object_has_member(root, "features")
                              ? json_object_get_array_member(root, "features")
                              : nullptr;
    for (guint i = 0; features != nullptr && i < json_array_get_length(features); i++) {
        JsonObject *land_feature = json_array_get_object_element(features, i);
        JsonObject *geometry =
            land_feature != nullptr && json_object_has_member(land_feature, "geometry")
                ? json_object_get_object_member(land_feature, "geometry")
                : nullptr;
        if (geometry == nullptr || !json_object_has_member(geometry, "coordinates"))
            continue;
        const char *type = json_object_get_string_member_with_default(geometry, "type", "");
        JsonArray *coords = json_object_get_array_member(geometry, "coordinates");
        if (strcmp(type, "Polygon") == 0) {
            polygon_path(cairo, coords, width, height);
        } else if (strcmp(type, "MultiPolygon") == 0) {
            for (guint k = 0; k < json_array_get_length(coords); k++)
                polygon_path(cairo, json_array_get_array_element(coords, k), width, height);
        }
    }
}

/* ISO 6709 as zone1970.tab writes it: +DDMM-DDDMM or +DDMMSS-DDDMMSS. */
static bool parse_coord(const char *text, size_t deg_digits, double *out, const char **end) {
    if (*text != '+' && *text != '-')
        return false;
    int sign = *text == '-' ? -1 : 1;
    text++;
    size_t count = 0;
    while (g_ascii_isdigit(text[count]))
        count++;
    if (count != deg_digits + 2 && count != deg_digits + 4)
        return false;
    char buf[8] = {0};
    memcpy(buf, text, deg_digits);
    double degrees = g_ascii_strtod(buf, nullptr);
    memset(buf, 0, sizeof(buf));
    memcpy(buf, text + deg_digits, 2);
    degrees += g_ascii_strtod(buf, nullptr) / 60.0;
    if (count == deg_digits + 4) {
        memset(buf, 0, sizeof(buf));
        memcpy(buf, text + deg_digits + 2, 2);
        degrees += g_ascii_strtod(buf, nullptr) / 3600.0;
    }
    *out = sign * degrees;
    *end = text + count;
    return true;
}

/* The zone's city from zone1970.tab. */
static bool zone_city(const char *zone, double *lat, double *lon) {
    char *text = nullptr;
    if (zone == nullptr || !g_file_get_contents("/usr/share/zoneinfo/zone1970.tab", &text,
                                                nullptr, nullptr))
        return false;
    bool found = false;
    char **lines = g_strsplit(text, "\n", -1);
    for (char **line = lines; *line != nullptr && !found; line++) {
        char **fields = g_strsplit(*line, "\t", 4);
        const char *end = nullptr;
        if ((*line)[0] != '#' && g_strv_length(fields) >= 3 && strcmp(fields[2], zone) == 0 &&
            parse_coord(fields[1], 2, lat, &end) && parse_coord(end, 3, lon, &end))
            found = true;
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(text);
    return found;
}

static gboolean draw_map(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    props *properties = data;
    double mw = gtk_widget_get_allocated_width(widget),
           mh = gtk_widget_get_allocated_height(widget);
    cairo_rectangle(cairo, 0, 0, mw, mh);
    cairo_set_source_rgb(cairo, 0.27, 0.45, 0.72); /* ocean */
    cairo_fill(cairo);
    const char *zone = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->zones));
    /* The band of the zone's current UTC offset (15 degrees an hour). */
    if (zone != nullptr) {
        GTimeZone *tz = g_time_zone_new_identifier(zone);
        if (tz != nullptr) {
            GDateTime *now = g_date_time_new_now(tz);
            GTimeSpan offset = g_date_time_get_utc_offset(now);
            double hours = (double)offset / (double)G_TIME_SPAN_HOUR;
            double x0, x1, y;
            map_point(hours * 15 - 7.5, 0, mw, mh, &x0, &y);
            map_point(hours * 15 + 7.5, 0, mw, mh, &x1, &y);
            cairo_rectangle(cairo, x0, 0, x1 - x0, mh);
            cairo_set_source_rgba(cairo, 1, 1, 0.7, 0.35);
            cairo_fill(cairo);
            g_date_time_unref(now);
            g_time_zone_unref(tz);
        }
    }
    land_path(cairo, mw, mh);
    cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_set_source_rgb(cairo, 0.35, 0.62, 0.30); /* land */
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0.20, 0.42, 0.18);
    cairo_set_line_width(cairo, 0.6);
    cairo_stroke(cairo);
    double lat, lon;
    if (zone_city(zone, &lat, &lon)) {
        double x, y;
        map_point(lon, lat, mw, mh, &x, &y);
        cairo_arc(cairo, x, y, 3.5, 0, 2 * XD_PI);
        cairo_set_source_rgb(cairo, 0.85, 0.1, 0.1);
        cairo_fill_preserve(cairo);
        cairo_set_source_rgb(cairo, 1, 1, 1);
        cairo_set_line_width(cairo, 1);
        cairo_stroke(cairo);
    }
    cairo_rectangle(cairo, 0.5, 0.5, mw - 1, mh - 1);
    cairo_set_source_rgb(cairo, 0.3, 0.3, 0.3);
    cairo_stroke(cairo);
    return FALSE;
}

/* The zones from the tz database's zone1970.tab (what timedated offers
 * too), when timedated can't list them. */
static char **tab_zones(void) {
    char *text = nullptr;
    if (!g_file_get_contents("/usr/share/zoneinfo/zone1970.tab", &text, nullptr, nullptr))
        return nullptr;
    GPtrArray *zones = g_ptr_array_new();
    char **lines = g_strsplit(text, "\n", -1);
    for (char **line = lines; *line != nullptr; line++) {
        if ((*line)[0] == '#' || (*line)[0] == '\0')
            continue;
        char **fields = g_strsplit(*line, "\t", 4);
        if (g_strv_length(fields) >= 3)
            g_ptr_array_add(zones, g_strdup(fields[2]));
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(text);
    g_ptr_array_add(zones, g_strdup("UTC"));
    g_ptr_array_add(zones, nullptr);
    return (char **)g_ptr_array_free(zones, FALSE);
}

static int compare_str(gconstpointer left, gconstpointer right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static void fill_zones(props *properties, char **list) {
    guint count = list != nullptr ? g_strv_length(list) : 0;
    if (count > 0)
        qsort(list, count, sizeof(char *), compare_str);
    int active = -1;
    for (guint i = 0; i < count; i++) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->zones), list[i], list[i]);
        if (properties->zone != nullptr && strcmp(list[i], properties->zone) == 0)
            active = (int)i;
    }
    if (active < 0 && properties->zone != nullptr) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->zones), properties->zone,
                                  properties->zone);
        active = (int)count;
    }
    properties->updating = true;
    gtk_combo_box_set_active(GTK_COMBO_BOX(properties->zones), active);
    properties->updating = false;
}

static void got_zones(GObject *src, GAsyncResult *res, gpointer data) {
    props *properties = data;
    GVariant *variant = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, nullptr);
    char **list = nullptr;
    if (variant != nullptr) {
        g_variant_get(variant, "(^as)", &list);
        g_variant_unref(variant);
    } else {
        list = tab_zones();
    }
    fill_zones(properties, list);
    g_strfreev(list);
}

static void on_zone([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    gtk_widget_queue_draw(properties->map);
    if (!properties->updating)
        changed(properties);
}

static void on_local_rtc([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    if (!properties->updating)
        changed(properties);
}

static void show_local_rtc(props *properties) {
    properties->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->local_rtc),
                                 td_bool(properties, "LocalRTC"));
    properties->updating = false;
    gtk_widget_set_sensitive(properties->local_rtc, properties->td != nullptr);
}

static GtkWidget *zone_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    properties->zones = gtk_combo_box_text_new();
    g_signal_connect(properties->zones, "changed", G_CALLBACK(on_zone), properties);
    gtk_box_pack_start(GTK_BOX(page), properties->zones, FALSE, FALSE, 0);
    load_land();
    properties->map = gtk_drawing_area_new();
    /* 360 by 142 degrees of longitude and latitude, unstretched. */
    gtk_widget_set_size_request(properties->map, 400, 158);
    gtk_widget_set_halign(properties->map, GTK_ALIGN_CENTER);
    g_signal_connect(properties->map, "draw", G_CALLBACK(draw_map), properties);
    gtk_box_pack_start(GTK_BOX(page), properties->map, FALSE, FALSE, 0);
    GtkWidget *note = gtk_label_new("Daylight saving time changes follow the time zone's rules.");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_box_pack_start(GTK_BOX(page), note, FALSE, FALSE, 0);
    /* Where XP has "Automatically adjust clock for daylight saving
     * changes": Windows keeps the hardware clock in local time, Linux in
     * UTC, so a computer that runs both shows the wrong time in one of
     * them unless Linux keeps local time too. */
    properties->local_rtc = gtk_check_button_new_with_mnemonic(
        "The _hardware clock keeps local time (for computers that also run Windows)");
    g_signal_connect(properties->local_rtc, "toggled", G_CALLBACK(on_local_rtc), properties);
    gtk_box_pack_start(GTK_BOX(page), properties->local_rtc, FALSE, FALSE, 0);
    return page;
}

/* ---- calling the services -------------------------------------------------------- */

static void error_box(props *properties, const char *what, GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error->message);
    xd_message_box(GTK_WINDOW(properties->window), "Date and Time", XD_MB_ERROR, XD_MB_OK, FALSE,
                   text);
    g_free(text);
}

/* A call waits for its service (and its password prompt, which has no
 * time limit) in a main loop of its own, so the window keeps drawing and
 * answering the compositor instead of turning "Not Responding". It takes
 * no input meanwhile. */
typedef struct pending_call {
    GMainLoop *loop;
    GVariant *result;
    GError *error;
} pending_call;

static void call_done(GObject *src, GAsyncResult *res, gpointer data) {
    pending_call *call = data;
    call->result = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &call->error);
    g_main_loop_quit(call->loop);
}

/* timesyncd and systemd ask polkit for a password only when the call
 * allows it; timedated takes that as an argument. */
static bool call_full(props *properties, GDBusProxy *proxy, const char *method, GVariant *args,
                      GError **error) {
    pending_call call = {.loop = g_main_loop_new(nullptr, FALSE)};
    properties->busy = true;
    gtk_widget_set_sensitive(properties->window, FALSE);
    g_dbus_proxy_call(proxy, method, args, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                      G_MAXINT, nullptr, call_done, &call);
    g_main_loop_run(call.loop);
    g_main_loop_unref(call.loop);
    gtk_widget_set_sensitive(properties->window, TRUE);
    properties->busy = false;
    if (call.result == nullptr) {
        g_propagate_error(error, call.error);
        return false;
    }
    g_variant_unref(call.result);
    return true;
}

static bool call(props *properties, GDBusProxy *proxy, const char *method, GVariant *args,
                 const char *what) {
    GError *error = nullptr;
    if (call_full(properties, proxy, method, args, &error))
        return true;
    error_box(properties, what, error);
    g_error_free(error);
    return false;
}

/* ---- Internet Time ---------------------------------------------------------- */

static bool ts_running(props *properties) {
    if (properties->ts == nullptr)
        return false;
    char *owner = g_dbus_proxy_get_name_owner(properties->ts);
    bool running = owner != nullptr;
    g_free(owner);
    return running;
}

static GVariant *ts_prop(props *properties, const char *name) {
    return ts_running(properties) ? g_dbus_proxy_get_cached_property(properties->ts, name)
                                  : nullptr;
}

static char **ts_strv(props *properties, const char *name) {
    GVariant *variant = ts_prop(properties, name);
    char **list = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING_ARRAY)
                      ? g_variant_dup_strv(variant, nullptr)
                      : nullptr;
    if (variant != nullptr)
        g_variant_unref(variant);
    return list;
}

static char *ts_string(props *properties, const char *name) {
    GVariant *variant = ts_prop(properties, name);
    char *value = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING)
                      ? g_variant_dup_string(variant, nullptr)
                      : nullptr;
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

static guint64 ts_u64(props *properties, const char *name) {
    GVariant *variant = ts_prop(properties, name);
    guint64 value = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT64)
                        ? g_variant_get_uint64(variant)
                        : 0;
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

/* timesyncd takes a server while it runs since systemd 255, which added
 * RuntimeNTPServers with SetRuntimeNTPServers. */
static bool ts_can_set_server(props *properties) {
    GVariant *variant = ts_prop(properties, "RuntimeNTPServers");
    if (variant == nullptr)
        return false;
    g_variant_unref(variant);
    return true;
}

static gint64 ts_received(props *properties) {
    GVariant *variant = ts_prop(properties, "NTPMessage");
    gint64 when = xd_ntp_message_received(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return when;
}

/* The servers timesyncd knows, and in *current the one it uses (or would
 * try first). */
static char **ts_servers(props *properties, char **current) {
    char **runtime = ts_strv(properties, "RuntimeNTPServers"),
         **link = ts_strv(properties, "LinkNTPServers"),
         **system = ts_strv(properties, "SystemNTPServers"),
         **fallback = ts_strv(properties, "FallbackNTPServers");
    char **list = xd_ntp_server_list((const char *const *)runtime, (const char *const *)link,
                                     (const char *const *)system, (const char *const *)fallback);
    g_strfreev(runtime);
    g_strfreev(link);
    g_strfreev(system);
    g_strfreev(fallback);
    *current = ts_string(properties, "ServerName");
    if ((*current == nullptr || (*current)[0] == '\0') && list[0] != nullptr) {
        g_free(*current);
        *current = g_strdup(list[0]);
    }
    return list;
}

static GtkWidget *server_entry(props *properties) {
    return gtk_bin_get_child(GTK_BIN(properties->server));
}

/* The server as typed, trimmed. */
static char *server_text(props *properties) {
    return g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(server_entry(properties)))));
}

static void fill_servers(props *properties) {
    char *current = nullptr;
    char **list = ts_running(properties) ? ts_servers(properties, &current) : nullptr;
    bool was_updating = properties->updating;
    properties->updating = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(properties->server));
    for (size_t i = 0; list != nullptr && list[i] != nullptr; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(properties->server), list[i]);
    gtk_entry_set_text(GTK_ENTRY(server_entry(properties)), current != nullptr ? current : "");
    properties->updating = was_updating;
    g_strfreev(list);
    g_free(current);
}

/* What the tab says under the server: Update Now's progress or failure,
 * else when the clock was last synchronized and will be next, as XP. */
static char *ntp_status_text(props *properties) {
    if (properties->sync_since != 0)
        return g_strdup_printf("Please wait while the time is synchronized with %s...",
                               properties->sync_server);
    if (properties->sync_failed != nullptr)
        return g_strdup(properties->sync_failed);
    if (!td_bool(properties, "CanNTP"))
        return g_strdup("Network time synchronization isn't available.");
    if (!td_bool(properties, "NTP"))
        return g_strdup("");
    gint64 received = ts_received(properties);
    if (received <= 0)
        return g_strdup(td_bool(properties, "NTPSynchronized")
                            ? "The clock is synchronized."
                            : "The clock isn't synchronized yet.");
    char *server = nullptr;
    g_strfreev(ts_servers(properties, &server));
    GDateTime *last = g_date_time_new_from_unix_local_usec(received);
    char *text = xd_ntp_synced_text(server != nullptr ? server : "the time server", last);
    /* timesyncd asks again after its poll interval. */
    guint64 poll = ts_u64(properties, "PollIntervalUSec");
    if (poll > 0 && poll < (guint64)(G_MAXINT64 - received)) {
        GDateTime *next = g_date_time_new_from_unix_local_usec(received + (gint64)poll);
        char *next_text = xd_ntp_next_text(next);
        char *both = g_strdup_printf("%s\n%s", text, next_text);
        g_free(next_text);
        g_free(text);
        text = both;
        g_date_time_unref(next);
    }
    g_date_time_unref(last);
    g_free(server);
    return text;
}

/* Why the server can't be chosen or Update Now pressed, if so; else how
 * long a server chosen here lasts. */
static const char *ntp_note_text(props *properties, bool on) {
    if (!td_bool(properties, "CanNTP"))
        return "";
    if (!ts_running(properties) && !properties->ts_activatable)
        return "This computer keeps its time with a time service other than "
               "systemd-timesyncd: choose its servers in that service's settings.";
    if (!ts_running(properties))
        return on ? "Click Apply to turn on synchronization before choosing a server." : "";
    if (!ts_can_set_server(properties))
        return "This version of systemd-timesyncd can't change its server while it runs: "
               "choose servers in timesyncd.conf.";
    return "A server you choose here is used until you restart the computer.";
}

static void show_internet(props *properties) {
    bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->ntp));
    bool can = td_bool(properties, "CanNTP");
    bool waiting = properties->sync_since != 0;
    if (!properties->server_edited && !waiting)
        fill_servers(properties);
    gtk_widget_set_sensitive(properties->server,
                             on && can && ts_can_set_server(properties) && !waiting);
    gtk_widget_set_sensitive(properties->update,
                             on && can && (ts_running(properties) || properties->ts_activatable) &&
                                 !waiting);
    char *status = ntp_status_text(properties);
    gtk_label_set_text(GTK_LABEL(properties->ntp_status), status);
    g_free(status);
    gtk_label_set_text(GTK_LABEL(properties->ntp_note), ntp_note_text(properties, on));
}

static void show_ntp(props *properties) {
    properties->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->ntp), td_bool(properties, "NTP"));
    properties->updating = false;
    gtk_widget_set_sensitive(properties->ntp, td_bool(properties, "CanNTP"));
    show_internet(properties);
}

static void end_sync(props *properties, const char *failure) {
    if (properties->sync_timeout != 0)
        g_source_remove(properties->sync_timeout);
    properties->sync_timeout = 0;
    properties->sync_since = 0;
    g_free(properties->sync_failed);
    properties->sync_failed =
        failure != nullptr ? xd_ntp_failed_text(properties->sync_server, failure) : nullptr;
    show_internet(properties);
}

/* Update Now succeeded once timesyncd has an answer received since. */
static void check_sync(props *properties) {
    if (properties->sync_since != 0 && ts_received(properties) >= properties->sync_since)
        end_sync(properties, nullptr);
}

static gboolean sync_timed_out(gpointer data) {
    props *properties = data;
    properties->sync_timeout = 0;
    /* Windows' own words for it. */
    end_sync(properties, "This operation returned because the timeout period expired.");
    return G_SOURCE_REMOVE;
}

/* SetRuntimeNTPServers' argument: the server, or none (back to the
 * servers timesyncd is set up with) when the box is empty. */
static GVariant *servers_arg(const char *server) {
    const char *list[] = {server, nullptr};
    return g_variant_new("(^as)", server[0] != '\0' ? list : list + 1);
}

/* timesyncd forgets the servers it was given when it restarts (it keeps
 * them nowhere), so Update Now hands them over again instead, which makes
 * it connect afresh too. Otherwise it's restarted, which systemd lets an
 * administrator do (polkit), and it synchronizes as it starts. */
static bool start_sync(props *properties, const char *server, GError **error) {
    if (!td_bool(properties, "NTP"))
        return call_full(properties, properties->td, "SetNTP", g_variant_new("(bb)", TRUE, TRUE),
                         error);
    if (properties->server_edited && ts_can_set_server(properties)) {
        if (!call_full(properties, properties->ts, "SetRuntimeNTPServers", servers_arg(server),
                       error))
            return false;
        properties->server_edited = false;
        return true;
    }
    char **runtime = ts_strv(properties, "RuntimeNTPServers");
    if (runtime != nullptr && runtime[0] != nullptr) {
        bool ok = call_full(properties, properties->ts, "SetRuntimeNTPServers",
                            g_variant_new("(^as)", runtime), error);
        g_strfreev(runtime);
        return ok;
    }
    g_strfreev(runtime);
    if (properties->systemd == nullptr) {
        properties->systemd = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM,
            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS |
                G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
            nullptr, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", nullptr, error);
        if (properties->systemd == nullptr)
            return false;
    }
    return call_full(properties, properties->systemd, "RestartUnit",
                     g_variant_new("(ss)", TIMESYNC_UNIT, "replace"), error);
}

static void on_update([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    if (properties->busy || properties->sync_since != 0)
        return;
    char *server = server_text(properties);
    g_free(properties->sync_server);
    properties->sync_server = g_strdup(server[0] != '\0' ? server : "the time server");
    gint64 since = g_get_real_time();
    GError *error = nullptr;
    if (start_sync(properties, server, &error)) {
        g_clear_pointer(&properties->sync_failed, g_free);
        properties->sync_since = since;
        properties->sync_timeout = g_timeout_add_seconds(SYNC_SECONDS, sync_timed_out, properties);
        show_internet(properties);
        check_sync(properties);
    } else {
        end_sync(properties, error->message);
        g_error_free(error);
    }
    g_free(server);
}

static void on_ntp([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    show_internet(properties);
    if (!properties->updating)
        changed(properties);
}

static void on_server([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    properties->server_edited = true;
    changed(properties);
}

static void ts_changed(props *properties) {
    check_sync(properties);
    if (properties->sync_since == 0)
        show_internet(properties);
}

static void ts_props_changed([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *variant,
                             [[maybe_unused]] const char *const *inv, gpointer data) {
    ts_changed(data);
}

/* timesyncd restarted, started or stopped. */
static void ts_owner_changed([[maybe_unused]] GObject *object, [[maybe_unused]] GParamSpec *ps,
                             gpointer data) {
    ts_changed(data);
}

static GtkWidget *internet_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    properties->ntp = gtk_check_button_new_with_mnemonic(
        "Automatically _synchronize with an Internet time server");
    g_signal_connect(properties->ntp, "toggled", G_CALLBACK(on_ntp), properties);
    gtk_box_pack_start(GTK_BOX(page), properties->ntp, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new_with_mnemonic("Se_rver:");
    properties->server = gtk_combo_box_text_new_with_entry();
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->server);
    gtk_entry_set_width_chars(GTK_ENTRY(server_entry(properties)), 24);
    g_signal_connect(properties->server, "changed", G_CALLBACK(on_server), properties);
    properties->update = gtk_button_new_with_mnemonic("_Update Now");
    g_signal_connect(properties->update, "clicked", G_CALLBACK(on_update), properties);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), properties->server, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), properties->update, FALSE, FALSE, 0);
    gtk_widget_set_margin_start(row, 20);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    properties->ntp_status = gtk_label_new("");
    properties->ntp_note = gtk_label_new("");
    GtkWidget *online = gtk_label_new(
        "Synchronization can occur only when your computer is connected to the Internet.");
    GtkWidget *labels[] = {properties->ntp_status, properties->ntp_note, online};
    for (size_t i = 0; i < XD_ARRAY_LEN(labels); i++) {
        gtk_label_set_xalign(GTK_LABEL(labels[i]), 0);
        gtk_label_set_line_wrap(GTK_LABEL(labels[i]), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(labels[i]), 60);
    }
    gtk_box_pack_start(GTK_BOX(page), properties->ntp_status, FALSE, FALSE, 0);
    /* At the foot of the tab, as XP's. */
    gtk_box_pack_end(GTK_BOX(page), online, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(page), properties->ntp_note, FALSE, FALSE, 0);
    return page;
}

/* ---- applying -------------------------------------------------------------------- */

/* Nor may the window be destroyed under a call. */
static gboolean on_delete([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] GdkEvent *gdk_event,
                          gpointer data) {
    return ((props *)data)->busy;
}

static bool apply(props *properties) {
    if (properties->td == nullptr)
        return false;
    bool ok = true;
    const char *zone = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->zones));
    if (zone != nullptr && (properties->zone == nullptr || strcmp(zone, properties->zone) != 0)) {
        ok = call(properties, properties->td, "SetTimezone", g_variant_new("(sb)", zone, TRUE),
                  "The time zone could not be changed.");
        if (ok) {
            g_free(properties->zone);
            properties->zone = g_strdup(zone);
            /* The process keeps its old idea of local time otherwise. */
            g_setenv("TZ", zone, TRUE);
            tzset();
        }
    }
    bool local_rtc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->local_rtc));
    /* The system clock is right: the hardware clock is set from it in its
     * new reckoning (fix_system false). */
    if (ok && local_rtc != td_bool(properties, "LocalRTC"))
        ok = call(properties, properties->td, "SetLocalRTC",
                  g_variant_new("(bbb)", local_rtc, FALSE, TRUE),
                  "The hardware clock could not be changed.");
    bool ntp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->ntp));
    bool set_time = properties->time_edited || properties->date_edited;
    /* Setting the time by hand turns network time off, as timedated
     * won't take one while it's on. */
    if (set_time && ntp)
        ntp = false;
    if (ok && ntp != td_bool(properties, "NTP"))
        ok = call(properties, properties->td, "SetNTP", g_variant_new("(bb)", ntp, TRUE),
                  "Network time synchronization could not be changed.");
    if (ok && ntp && properties->server_edited && ts_can_set_server(properties)) {
        char *server = server_text(properties);
        ok = call(properties, properties->ts, "SetRuntimeNTPServers", servers_arg(server),
                  "The time server could not be changed.");
        g_free(server);
        if (ok)
            properties->server_edited = false;
    }
    if (ok && set_time) {
        guint y = 0, month = 0, day = 0;
        gtk_calendar_get_date(GTK_CALENDAR(properties->calendar), &y, &month, &day);
        GDateTime *now = g_date_time_new_now_local();
        GDateTime *when = g_date_time_new_local(
            (int)y, (int)month + 1, (int)day,
            properties->time_edited ? spin(properties->hour) : g_date_time_get_hour(now),
            properties->time_edited ? spin(properties->minute) : g_date_time_get_minute(now),
            properties->time_edited ? spin(properties->second) : g_date_time_get_second(now));
        g_date_time_unref(now);
        if (when != nullptr) {
            gint64 usec = g_date_time_to_unix(when) * G_USEC_PER_SEC;
            ok = call(properties, properties->td, "SetTime",
                      g_variant_new("(xbb)", usec, FALSE, TRUE),
                      "The date and time could not be changed.");
            g_date_time_unref(when);
        }
        if (ok)
            properties->time_edited = properties->date_edited = false;
    }
    if (ok) {
        gtk_widget_set_sensitive(properties->apply, FALSE);
        show_local_rtc(properties);
    }
    show_zone_label(properties);
    show_ntp(properties);
    return ok;
}

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    props *properties = data;
    if (properties->busy)
        return; /* the window's close button, while Apply waits */
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply) &&
        !apply(properties))
        return;
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

static void td_changed([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *variant,
                       [[maybe_unused]] const char *const *inv, gpointer data) {
    props *properties = data;
    show_ntp(properties);
}

/* Whether the bus starts `name` when asked, i.e. it's installed:
 * timesyncd runs only while synchronization is on. */
static bool activatable(GDBusConnection *bus, const char *name) {
    GVariant *variant = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "ListActivatableNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1,
        nullptr, nullptr);
    if (variant == nullptr)
        return false;
    char **names = nullptr;
    g_variant_get(variant, "(^as)", &names);
    bool found = g_strv_contains((const char *const *)names, name);
    g_strfreev(names);
    g_variant_unref(variant);
    return found;
}

int main(int argc, char **argv) {
    int page = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
    }
    xd_gtk_app_preinit("org.xeondesktop.DateTime");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props properties;
    GError *error = nullptr;
    properties.td =
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, TIMEDATE,
                                      TIMEDATE_PATH, TIMEDATE, nullptr, &error);
    if (properties.td == nullptr) {
        g_printerr("xd-timedate: %s\n", error->message);
        g_clear_error(&error);
    }
    /* Never started for us: it runs only while synchronization is on. */
    properties.ts = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, nullptr, TIMESYNC, TIMESYNC_PATH,
        TIMESYNC_MANAGER, nullptr, &error);
    if (properties.ts == nullptr) {
        g_printerr("xd-timedate: %s\n", error->message);
        g_clear_error(&error);
    } else {
        properties.ts_activatable =
            activatable(g_dbus_proxy_get_connection(properties.ts), TIMESYNC);
    }
    GVariant *variant = td_prop(&properties, "Timezone");
    properties.zone = variant != nullptr ? g_variant_dup_string(variant, nullptr) : nullptr;
    if (variant != nullptr)
        g_variant_unref(variant);

    properties.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties.window), "Date and Time Properties");
    gtk_window_set_icon_name(GTK_WINDOW(properties.window), "preferences-system-time");
    gtk_window_set_resizable(GTK_WINDOW(properties.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "Cancel", GTK_RESPONSE_CANCEL);
    properties.apply =
        gtk_dialog_add_button(GTK_DIALOG(properties.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(properties.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(properties.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), date_time_page(&properties),
                             gtk_label_new_with_mnemonic("Date & _Time"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), zone_page(&properties),
                             gtk_label_new_with_mnemonic("Time _Zone"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), internet_page(&properties),
                             gtk_label_new_with_mnemonic("_Internet Time"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties.window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties.window, GTK_NOTEBOOK(tabs));
    show_zone_label(&properties);
    show_local_rtc(&properties);
    show_ntp(&properties);
    if (properties.td != nullptr) {
        g_signal_connect(properties.td, "g-properties-changed", G_CALLBACK(td_changed),
                         &properties);
        g_dbus_proxy_call(properties.td, "ListTimezones", nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                          nullptr, got_zones, &properties);
    } else {
        char **list = tab_zones();
        fill_zones(&properties, list);
        g_strfreev(list);
    }
    if (properties.ts != nullptr) {
        g_signal_connect(properties.ts, "g-properties-changed", G_CALLBACK(ts_props_changed),
                         &properties);
        g_signal_connect(properties.ts, "notify::g-name-owner", G_CALLBACK(ts_owner_changed),
                         &properties);
    }
    g_timeout_add(1000, tick, &properties);
    g_signal_connect(properties.window, "response", G_CALLBACK(on_response), &properties);
    g_signal_connect(properties.window, "delete-event", G_CALLBACK(on_delete), &properties);
    g_signal_connect(properties.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(properties.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), page);
    gtk_main();
    return EXIT_SUCCESS;
}
