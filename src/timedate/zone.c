/* SPDX-License-Identifier: MIT */
#include "zone.h"

GTimeSpan xd_zone_standard_offset(GTimeZone *tz, int year) {
    GDateTime *january = g_date_time_new(tz, year, 1, 1, 12, 0, 0);
    GDateTime *july = g_date_time_new(tz, year, 7, 1, 12, 0, 0);
    GTimeSpan offset =
        MIN(g_date_time_get_utc_offset(january), g_date_time_get_utc_offset(july));
    g_date_time_unref(january);
    g_date_time_unref(july);
    return offset;
}
