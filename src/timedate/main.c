/* SPDX-License-Identifier: MIT
 * xd-timedate: Windows XP's Date and Time Properties, through systemd's
 * timedated (org.freedesktop.timedate1). Changes go through polkit, so
 * xd-polkit asks for a password when needed.
 *
 *   Date & Time    the month, year and day; an analog clock and the time
 *   Time Zone      the zone timedated knows
 *   Internet Time  network time synchronization (systemd-timesyncd)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <json-glib/json-glib.h>

#include "../core/common.h"
#include "../settings/settings.h"
#include "../gtkapp/gtkapp.h"

#define TIMEDATE "org.freedesktop.timedate1"
#define TIMEDATE_PATH "/org/freedesktop/timedate1"

typedef struct props {
    GtkWidget *window, *apply;
    GDBusProxy *td;
    /* Date & Time */
    GtkWidget *month, *year, *calendar, *clock, *hour, *minute, *second, *zone_label;
    bool time_edited, date_edited;
    bool updating; /* we're setting the widgets ourselves */
    /* Time Zone */
    GtkWidget *zones, *map;
    char *zone; /* as timedated has it */
    /* Internet Time */
    GtkWidget *ntp, *ntp_status;
} props;

static void changed(props *p) { gtk_widget_set_sensitive(p->apply, TRUE); }

static GVariant *td_prop(props *p, const char *name) {
    return p->td != nullptr ? g_dbus_proxy_get_cached_property(p->td, name) : nullptr;
}

static bool td_bool(props *p, const char *name) {
    GVariant *v = td_prop(p, name);
    bool b = v != nullptr && g_variant_get_boolean(v);
    if (v != nullptr)
        g_variant_unref(v);
    return b;
}

/* ---- Date & Time ------------------------------------------------------------- */

static int spin(GtkWidget *w) { return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)); }

static void set_spin(GtkWidget *w, int v) { gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), v); }

static void show_time(props *p, GDateTime *now) {
    p->updating = true;
    set_spin(p->hour, g_date_time_get_hour(now));
    set_spin(p->minute, g_date_time_get_minute(now));
    set_spin(p->second, g_date_time_get_second(now));
    p->updating = false;
    gtk_widget_queue_draw(p->clock);
}

static void show_date(props *p, GDateTime *now) {
    p->updating = true;
    int y = g_date_time_get_year(now), m = g_date_time_get_month(now);
    gtk_combo_box_set_active(GTK_COMBO_BOX(p->month), m - 1);
    set_spin(p->year, y);
    gtk_calendar_select_month(GTK_CALENDAR(p->calendar), (guint)(m - 1), (guint)y);
    gtk_calendar_select_day(GTK_CALENDAR(p->calendar), (guint)g_date_time_get_day_of_month(now));
    p->updating = false;
}

static void show_zone_label(props *p) {
    GDateTime *now = g_date_time_new_now_local();
    char *text = g_strdup_printf("Current time zone: %s (%s)", p->zone != nullptr ? p->zone : "?",
                                 g_date_time_get_timezone_abbreviation(now));
    gtk_label_set_text(GTK_LABEL(p->zone_label), text);
    g_free(text);
    g_date_time_unref(now);
}

/* The clock runs until the time is edited, as in Windows. */
static gboolean tick(gpointer data) {
    props *p = data;
    GDateTime *now = g_date_time_new_now_local();
    if (!p->time_edited)
        show_time(p, now);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

static void on_time_changed([[maybe_unused]] GtkSpinButton *s, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    p->time_edited = true;
    gtk_widget_queue_draw(p->clock);
    changed(p);
}

static void on_month_year([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    int m = gtk_combo_box_get_active(GTK_COMBO_BOX(p->month));
    int y = spin(p->year);
    guint day = 0;
    gtk_calendar_get_date(GTK_CALENDAR(p->calendar), nullptr, nullptr, &day);
    p->updating = true;
    gtk_calendar_select_month(GTK_CALENDAR(p->calendar), (guint)XD_MAX(m, 0), (guint)y);
    /* Keep the day, short of a shorter month's end. */
    guint last = (guint)g_date_get_days_in_month((GDateMonth)(m + 1), (GDateYear)y);
    gtk_calendar_select_day(GTK_CALENDAR(p->calendar), day < last ? day : last);
    p->updating = false;
    p->date_edited = true;
    changed(p);
}

static void on_day([[maybe_unused]] GtkCalendar *c, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    p->date_edited = true;
    changed(p);
}

/* The analog clock: a face with hour marks and three hands. */
static gboolean draw_clock(GtkWidget *w, cairo_t *cr, gpointer data) {
    props *p = data;
    double size = XD_MIN(gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    double cx = gtk_widget_get_allocated_width(w) / 2.0;
    double cy = gtk_widget_get_allocated_height(w) / 2.0;
    double r = size / 2 - 4;
    cairo_arc(cr, cx, cy, r, 0, 2 * XD_PI);
    cairo_pattern_t *face = cairo_pattern_create_radial(cx - r / 3, cy - r / 3, r / 6, cx, cy, r);
    cairo_pattern_add_color_stop_rgb(face, 0, 1, 1, 1);
    cairo_pattern_add_color_stop_rgb(face, 1, 0.82, 0.86, 0.92);
    cairo_set_source(cr, face);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(face);
    cairo_set_source_rgb(cr, 0.25, 0.32, 0.45);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    for (int i = 0; i < 60; i++) {
        double a = i * XD_PI / 30;
        bool hour = i % 5 == 0;
        double r0 = r - (hour ? 9 : 4);
        cairo_move_to(cr, cx + r0 * sin(a), cy - r0 * cos(a));
        cairo_line_to(cr, cx + (r - 2) * sin(a), cy - (r - 2) * cos(a));
        cairo_set_line_width(cr, hour ? 2.5 : 1);
        cairo_set_source_rgb(cr, hour ? 0.1 : 0.4, hour ? 0.2 : 0.45, hour ? 0.5 : 0.55);
        cairo_stroke(cr);
    }
    int h = spin(p->hour), m = spin(p->minute), s = spin(p->second);
    struct {
        double angle, length, width, red;
    } hands[] = {
        {(h % 12 + m / 60.0) * XD_PI / 6, r * 0.5, 4, 0.1},
        {(m + s / 60.0) * XD_PI / 30, r * 0.75, 3, 0.1},
        {s * XD_PI / 30, r * 0.85, 1, 0.75}, /* the red second hand */
    };
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (size_t i = 0; i < XD_ARRAY_LEN(hands); i++) {
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, cx + hands[i].length * sin(hands[i].angle),
                      cy - hands[i].length * cos(hands[i].angle));
        cairo_set_line_width(cr, hands[i].width);
        bool second = i == 2;
        cairo_set_source_rgb(cr, hands[i].red, second ? 0.1 : 0.15, second ? 0.1 : 0.3);
        cairo_stroke(cr);
    }
    cairo_arc(cr, cx, cy, 3, 0, 2 * XD_PI);
    cairo_fill(cr);
    return FALSE;
}

static gboolean two_digits(GtkSpinButton *b, [[maybe_unused]] gpointer data) {
    char text[8];
    g_snprintf(text, sizeof(text), "%02d", gtk_spin_button_get_value_as_int(b));
    gtk_entry_set_text(GTK_ENTRY(b), text);
    return TRUE;
}

static GtkWidget *time_spin(props *p, int max) {
    GtkWidget *s = gtk_spin_button_new_with_range(0, max, 1);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(s), TRUE);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(s), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(s), 2);
    g_signal_connect(s, "value-changed", G_CALLBACK(on_time_changed), p);
    /* Two digits, as a clock shows them. */
    g_signal_connect(s, "output", G_CALLBACK(two_digits), nullptr);
    return s;
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *date_time_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);

    GtkWidget *date = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *my = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->month = gtk_combo_box_text_new();
    for (int m = 1; m <= 12; m++) {
        GDateTime *d = g_date_time_new_local(2000, m, 1, 0, 0, 0);
        char *name = g_date_time_format(d, "%B");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->month), name);
        g_free(name);
        g_date_time_unref(d);
    }
    p->year = gtk_spin_button_new_with_range(1970, 2199, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(p->year), TRUE);
    gtk_box_pack_start(GTK_BOX(my), p->month, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(my), p->year, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(date), my, FALSE, FALSE, 0);
    p->calendar = gtk_calendar_new();
    gtk_calendar_set_display_options(GTK_CALENDAR(p->calendar), GTK_CALENDAR_SHOW_DAY_NAMES);
    gtk_box_pack_start(GTK_BOX(date), p->calendar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), frame("Date", date), TRUE, TRUE, 0);

    GtkWidget *time = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    p->clock = gtk_drawing_area_new();
    gtk_widget_set_size_request(p->clock, 130, 130);
    g_signal_connect(p->clock, "draw", G_CALLBACK(draw_clock), p);
    gtk_box_pack_start(GTK_BOX(time), p->clock, TRUE, TRUE, 0);
    GtkWidget *hms = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_halign(hms, GTK_ALIGN_CENTER);
    p->hour = time_spin(p, 23);
    p->minute = time_spin(p, 59);
    p->second = time_spin(p, 59);
    gtk_box_pack_start(GTK_BOX(hms), p->hour, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hms), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hms), p->minute, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hms), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hms), p->second, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time), hms, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), frame("Time", time), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, TRUE, TRUE, 0);

    p->zone_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->zone_label), 0.5f);
    gtk_box_pack_start(GTK_BOX(page), p->zone_label, FALSE, FALSE, 0);

    GDateTime *now = g_date_time_new_now_local();
    show_date(p, now);
    show_time(p, now);
    g_date_time_unref(now);
    g_signal_connect(p->month, "changed", G_CALLBACK(on_month_year), p);
    g_signal_connect(p->year, "value-changed", G_CALLBACK(on_month_year), p);
    g_signal_connect(p->calendar, "day-selected", G_CALLBACK(on_day), p);
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

static void map_point(double lon, double lat, double w, double h, double *x, double *y) {
    *x = (lon + 180.0) / 360.0 * w;
    *y = (MAP_NORTH - lat) / (MAP_NORTH - MAP_SOUTH) * h;
}

/* One ring of [lon, lat] pairs as a closed path. */
static void ring_path(cairo_t *cr, JsonArray *ring, double w, double h) {
    guint n = json_array_get_length(ring);
    for (guint i = 0; i < n; i++) {
        JsonArray *pt = json_array_get_array_element(ring, i);
        if (pt == nullptr || json_array_get_length(pt) < 2)
            continue;
        double x, y;
        map_point(json_array_get_double_element(pt, 0), json_array_get_double_element(pt, 1), w,
                  h, &x, &y);
        if (i == 0)
            cairo_move_to(cr, x, y);
        else
            cairo_line_to(cr, x, y);
    }
    cairo_close_path(cr);
}

static void polygon_path(cairo_t *cr, JsonArray *rings, double w, double h) {
    for (guint r = 0; r < json_array_get_length(rings); r++) {
        JsonArray *ring = json_array_get_array_element(rings, r);
        if (ring != nullptr)
            ring_path(cr, ring, w, h);
    }
}

static void land_path(cairo_t *cr, double w, double h) {
    JsonObject *root = land != nullptr && JSON_NODE_HOLDS_OBJECT(land) ? json_node_get_object(land)
                                                                       : nullptr;
    JsonArray *features = root != nullptr && json_object_has_member(root, "features")
                              ? json_object_get_array_member(root, "features")
                              : nullptr;
    for (guint i = 0; features != nullptr && i < json_array_get_length(features); i++) {
        JsonObject *f = json_array_get_object_element(features, i);
        JsonObject *g = f != nullptr && json_object_has_member(f, "geometry")
                            ? json_object_get_object_member(f, "geometry")
                            : nullptr;
        if (g == nullptr || !json_object_has_member(g, "coordinates"))
            continue;
        const char *type = json_object_get_string_member_with_default(g, "type", "");
        JsonArray *coords = json_object_get_array_member(g, "coordinates");
        if (strcmp(type, "Polygon") == 0) {
            polygon_path(cr, coords, w, h);
        } else if (strcmp(type, "MultiPolygon") == 0) {
            for (guint k = 0; k < json_array_get_length(coords); k++)
                polygon_path(cr, json_array_get_array_element(coords, k), w, h);
        }
    }
}

/* ISO 6709 as zone1970.tab writes it: +DDMM-DDDMM or +DDMMSS-DDDMMSS. */
static bool parse_coord(const char *s, size_t deg_digits, double *out, const char **end) {
    if (*s != '+' && *s != '-')
        return false;
    int sign = *s == '-' ? -1 : 1;
    s++;
    size_t n = 0;
    while (g_ascii_isdigit(s[n]))
        n++;
    if (n != deg_digits + 2 && n != deg_digits + 4)
        return false;
    char buf[8] = {0};
    memcpy(buf, s, deg_digits);
    double v = g_ascii_strtod(buf, nullptr);
    memset(buf, 0, sizeof(buf));
    memcpy(buf, s + deg_digits, 2);
    v += g_ascii_strtod(buf, nullptr) / 60.0;
    if (n == deg_digits + 4) {
        memset(buf, 0, sizeof(buf));
        memcpy(buf, s + deg_digits + 2, 2);
        v += g_ascii_strtod(buf, nullptr) / 3600.0;
    }
    *out = sign * v;
    *end = s + n;
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
    for (char **l = lines; *l != nullptr && !found; l++) {
        char **f = g_strsplit(*l, "\t", 4);
        const char *end = nullptr;
        if ((*l)[0] != '#' && g_strv_length(f) >= 3 && strcmp(f[2], zone) == 0 &&
            parse_coord(f[1], 2, lat, &end) && parse_coord(end, 3, lon, &end))
            found = true;
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(text);
    return found;
}

static gboolean draw_map(GtkWidget *w, cairo_t *cr, gpointer data) {
    props *p = data;
    double mw = gtk_widget_get_allocated_width(w), mh = gtk_widget_get_allocated_height(w);
    cairo_rectangle(cr, 0, 0, mw, mh);
    cairo_set_source_rgb(cr, 0.27, 0.45, 0.72); /* ocean */
    cairo_fill(cr);
    const char *zone = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->zones));
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
            cairo_rectangle(cr, x0, 0, x1 - x0, mh);
            cairo_set_source_rgba(cr, 1, 1, 0.7, 0.35);
            cairo_fill(cr);
            g_date_time_unref(now);
            g_time_zone_unref(tz);
        }
    }
    land_path(cr, mw, mh);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_set_source_rgb(cr, 0.35, 0.62, 0.30); /* land */
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.20, 0.42, 0.18);
    cairo_set_line_width(cr, 0.6);
    cairo_stroke(cr);
    double lat, lon;
    if (zone_city(zone, &lat, &lon)) {
        double x, y;
        map_point(lon, lat, mw, mh, &x, &y);
        cairo_arc(cr, x, y, 3.5, 0, 2 * XD_PI);
        cairo_set_source_rgb(cr, 0.85, 0.1, 0.1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    cairo_rectangle(cr, 0.5, 0.5, mw - 1, mh - 1);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_stroke(cr);
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
    for (char **l = lines; *l != nullptr; l++) {
        if ((*l)[0] == '#' || (*l)[0] == '\0')
            continue;
        char **fields = g_strsplit(*l, "\t", 4);
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

static int compare_str(gconstpointer a, gconstpointer b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void fill_zones(props *p, char **list) {
    guint n = list != nullptr ? g_strv_length(list) : 0;
    qsort(list, n, sizeof(char *), compare_str);
    int active = -1;
    for (guint i = 0; i < n; i++) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->zones), list[i], list[i]);
        if (p->zone != nullptr && strcmp(list[i], p->zone) == 0)
            active = (int)i;
    }
    if (active < 0 && p->zone != nullptr) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->zones), p->zone, p->zone);
        active = (int)n;
    }
    p->updating = true;
    gtk_combo_box_set_active(GTK_COMBO_BOX(p->zones), active);
    p->updating = false;
}

static void got_zones(GObject *src, GAsyncResult *res, gpointer data) {
    props *p = data;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, nullptr);
    char **list = nullptr;
    if (r != nullptr) {
        g_variant_get(r, "(^as)", &list);
        g_variant_unref(r);
    } else {
        list = tab_zones();
    }
    fill_zones(p, list);
    g_strfreev(list);
}

static void on_zone([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    gtk_widget_queue_draw(p->map);
    if (!p->updating)
        changed(p);
}

static GtkWidget *zone_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    p->zones = gtk_combo_box_text_new();
    g_signal_connect(p->zones, "changed", G_CALLBACK(on_zone), p);
    gtk_box_pack_start(GTK_BOX(page), p->zones, FALSE, FALSE, 0);
    load_land();
    p->map = gtk_drawing_area_new();
    /* 360 by 142 degrees of longitude and latitude, unstretched. */
    gtk_widget_set_size_request(p->map, 400, 158);
    gtk_widget_set_halign(p->map, GTK_ALIGN_CENTER);
    g_signal_connect(p->map, "draw", G_CALLBACK(draw_map), p);
    gtk_box_pack_start(GTK_BOX(page), p->map, FALSE, FALSE, 0);
    GtkWidget *note = gtk_label_new("Daylight saving time changes follow the time zone's rules.");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_box_pack_start(GTK_BOX(page), note, FALSE, FALSE, 0);
    return page;
}

/* ---- Internet Time ---------------------------------------------------------- */

static void show_ntp(props *p) {
    p->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->ntp), td_bool(p, "NTP"));
    p->updating = false;
    gtk_widget_set_sensitive(p->ntp, td_bool(p, "CanNTP"));
    gtk_label_set_text(GTK_LABEL(p->ntp_status),
                       !td_bool(p, "CanNTP")        ? "Network time synchronization isn't available."
                       : td_bool(p, "NTPSynchronized") ? "The clock is synchronized."
                                                       : "The clock isn't synchronized yet.");
}

static void on_ntp([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    props *p = data;
    if (!p->updating)
        changed(p);
}

static GtkWidget *internet_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    p->ntp = gtk_check_button_new_with_mnemonic(
        "Automatically _synchronize with an Internet time server");
    g_signal_connect(p->ntp, "toggled", G_CALLBACK(on_ntp), p);
    gtk_box_pack_start(GTK_BOX(page), p->ntp, FALSE, FALSE, 0);
    GtkWidget *server = gtk_label_new("Servers: those systemd-timesyncd is set up to use.");
    gtk_label_set_xalign(GTK_LABEL(server), 0);
    gtk_box_pack_start(GTK_BOX(page), server, FALSE, FALSE, 0);
    p->ntp_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->ntp_status), 0);
    gtk_box_pack_start(GTK_BOX(page), p->ntp_status, FALSE, FALSE, 0);
    return page;
}

/* ---- applying -------------------------------------------------------------------- */

static void error_box(props *p, const char *what, GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error->message);
    xd_message_box(GTK_WINDOW(p->window), "Date and Time", XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

/* Synchronous: Apply waits for timedated (and its password prompt). */
static bool call(props *p, const char *method, GVariant *args, const char *what) {
    GError *error = nullptr;
    GVariant *r = g_dbus_proxy_call_sync(p->td, method, args, G_DBUS_CALL_FLAGS_NONE, -1,
                                         nullptr, &error);
    if (r == nullptr) {
        error_box(p, what, error);
        g_error_free(error);
        return false;
    }
    g_variant_unref(r);
    return true;
}

static bool apply(props *p) {
    if (p->td == nullptr)
        return false;
    bool ok = true;
    const char *zone = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->zones));
    if (zone != nullptr && (p->zone == nullptr || strcmp(zone, p->zone) != 0)) {
        ok = call(p, "SetTimezone", g_variant_new("(sb)", zone, TRUE),
                  "The time zone could not be changed.");
        if (ok) {
            g_free(p->zone);
            p->zone = g_strdup(zone);
            /* The process keeps its old idea of local time otherwise. */
            g_setenv("TZ", zone, TRUE);
            tzset();
        }
    }
    bool ntp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->ntp));
    bool set_time = p->time_edited || p->date_edited;
    /* Setting the time by hand turns network time off, as timedated
     * won't take one while it's on. */
    if (set_time && ntp)
        ntp = false;
    if (ok && ntp != td_bool(p, "NTP"))
        ok = call(p, "SetNTP", g_variant_new("(bb)", ntp, TRUE),
                  "Network time synchronization could not be changed.");
    if (ok && set_time) {
        guint y = 0, m = 0, d = 0;
        gtk_calendar_get_date(GTK_CALENDAR(p->calendar), &y, &m, &d);
        GDateTime *now = g_date_time_new_now_local();
        GDateTime *when = g_date_time_new_local(
            (int)y, (int)m + 1, (int)d,
            p->time_edited ? spin(p->hour) : g_date_time_get_hour(now),
            p->time_edited ? spin(p->minute) : g_date_time_get_minute(now),
            p->time_edited ? spin(p->second) : g_date_time_get_second(now));
        g_date_time_unref(now);
        if (when != nullptr) {
            gint64 usec = g_date_time_to_unix(when) * G_USEC_PER_SEC;
            ok = call(p, "SetTime", g_variant_new("(xbb)", usec, FALSE, TRUE),
                      "The date and time could not be changed.");
            g_date_time_unref(when);
        }
        if (ok)
            p->time_edited = p->date_edited = false;
    }
    if (ok)
        gtk_widget_set_sensitive(p->apply, FALSE);
    show_zone_label(p);
    show_ntp(p);
    return ok;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !apply(p))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void td_changed([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *c,
                       [[maybe_unused]] const char *const *inv, gpointer data) {
    props *p = data;
    show_ntp(p);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.DateTime");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props p;
    GError *error = nullptr;
    p.td = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                         TIMEDATE, TIMEDATE_PATH, TIMEDATE, nullptr, &error);
    if (p.td == nullptr) {
        g_printerr("xd-timedate: %s\n", error->message);
        g_error_free(error);
    }
    GVariant *z = td_prop(&p, "Timezone");
    p.zone = z != nullptr ? g_variant_dup_string(z, nullptr) : nullptr;
    if (z != nullptr)
        g_variant_unref(z);

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), "Date and Time Properties");
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "preferences-system-time");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), date_time_page(&p),
                             gtk_label_new_with_mnemonic("Date & _Time"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), zone_page(&p),
                             gtk_label_new_with_mnemonic("Time _Zone"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), internet_page(&p),
                             gtk_label_new_with_mnemonic("_Internet Time"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));
    show_zone_label(&p);
    show_ntp(&p);
    if (p.td != nullptr) {
        g_signal_connect(p.td, "g-properties-changed", G_CALLBACK(td_changed), &p);
        g_dbus_proxy_call(p.td, "ListTimezones", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                          got_zones, &p);
    } else {
        char **list = tab_zones();
        fill_zones(&p, list);
        g_strfreev(list);
    }
    g_timeout_add(1000, tick, &p);
    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_main();
    return EXIT_SUCCESS;
}
