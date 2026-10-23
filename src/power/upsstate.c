/* SPDX-License-Identifier: MIT */
#include "upsstate.h"

#include <glib.h>
#include <math.h>
#include <string.h>

xd_ups_device_kind xd_ups_get_device_kind(const char *device, const char **address) {
    static const struct {
        const char *prefix;
        xd_ups_device_kind kind;
    } kinds[] = {{"upower:", XD_UPS_DEVICE_UPOWER}, {"nut:", XD_UPS_DEVICE_NUT}};
    for (size_t i = 0; device != nullptr && i < XD_ARRAY_LEN(kinds); i++) {
        size_t prefix_len = strlen(kinds[i].prefix);
        if (strncmp(device, kinds[i].prefix, prefix_len) == 0 && device[prefix_len] != '\0') {
            if (address != nullptr)
                *address = device + prefix_len;
            return kinds[i].kind;
        }
    }
    if (address != nullptr)
        *address = "";
    return XD_UPS_DEVICE_NONE;
}

void xd_ups_reading_init(xd_ups_reading *reading) {
    *reading = (xd_ups_reading){.charge = -1, .runtime = -1};
}

void xd_ups_parse_nut_status(const char *status, xd_ups_reading *reading) {
    reading->is_on_battery = reading->is_low_battery = reading->is_replace_battery = false;
    reading->has_condition = true;
    char **flags = g_strsplit_set(status != nullptr ? status : "", " \t", -1);
    for (char **flag = flags; *flag != nullptr; flag++) {
        if (strcmp(*flag, "OB") == 0)
            reading->is_on_battery = true;
        else if (strcmp(*flag, "LB") == 0)
            reading->is_low_battery = true;
        else if (strcmp(*flag, "RB") == 0)
            reading->is_replace_battery = true;
    }
    g_strfreev(flags);
}

void xd_ups_format_power_source(const xd_ups_reading *reading, char *out, size_t cap) {
    g_strlcpy(out,
              !reading->is_reachable    ? "Unknown"
              : reading->is_on_battery ? "Battery"
                                       : "AC Power",
              cap);
}

void xd_ups_format_runtime(const xd_ups_reading *reading, char *out, size_t cap) {
    if (!reading->is_reachable || reading->runtime < 0) {
        g_strlcpy(out, "Unknown", cap);
        return;
    }
    int minutes = reading->runtime / 60;
    if (minutes == 1)
        g_strlcpy(out, "1 minute", cap);
    else
        g_snprintf(out, cap, "%d minutes", minutes);
}

void xd_ups_format_capacity(const xd_ups_reading *reading, char *out, size_t cap) {
    if (!reading->is_reachable || reading->charge < 0)
        g_strlcpy(out, "Unknown", cap);
    else
        g_snprintf(out, cap, "%d%%", (int)lround(XD_MIN(reading->charge, 100.0)));
}

void xd_ups_format_condition(const xd_ups_reading *reading, char *out, size_t cap) {
    g_strlcpy(out,
              !reading->is_reachable || !reading->has_condition ? "Unknown"
              : reading->is_replace_battery                    ? "Replace"
                                                               : "Good",
              cap);
}

/* The earlier of two times, either -1 for none. */
static int64_t get_earlier(int64_t first, int64_t second) {
    if (first < 0)
        return second;
    if (second < 0)
        return first;
    return XD_MIN(first, second);
}

unsigned xd_ups_watch_step(const xd_ups_settings *settings, xd_ups_watch *watch,
                           const xd_ups_reading *reading, int64_t now, int64_t *wake_at) {
    unsigned events = 0;
    *wake_at = -1;
    if (reading->is_reachable && reading->is_on_battery != watch->is_on_battery) {
        if (reading->is_on_battery) {
            *watch = (xd_ups_watch){.is_on_battery = true,
                                    .failed_at = now,
                                    .next_notice_at = now + settings->notify_delay};
        } else {
            /* A failure too short to have been told needs no "restored". */
            if (watch->has_notified && settings->notify)
                events |= XD_UPS_EVENT_POWER_RESTORED;
            *watch = (xd_ups_watch){0};
        }
    }
    if (!watch->is_on_battery)
        return events;

    if (settings->notify) {
        if (now >= watch->next_notice_at) {
            events |= XD_UPS_EVENT_POWER_FAILED;
            watch->has_notified = true;
            watch->next_notice_at = now + XD_MAX(settings->notify_interval, 1);
        }
        *wake_at = watch->next_notice_at;
    }
    if (!watch->is_critical_done) {
        int64_t critical_at =
            settings->critical_after ? watch->failed_at + (int64_t)settings->critical_minutes * 60
                                     : -1;
        bool is_low = reading->is_reachable && reading->is_low_battery;
        if (is_low || (critical_at >= 0 && now >= critical_at)) {
            events |= XD_UPS_EVENT_CRITICAL;
            watch->is_critical_done = true;
        } else {
            *wake_at = get_earlier(*wake_at, critical_at);
        }
    }
    return events;
}
