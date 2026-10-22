/* SPDX-License-Identifier: MIT */
#include "calendar.h"

#include <langinfo.h>
#include <stdint.h>

#include <glib.h>

int xd_calendar_first_weekday(void) {
#ifdef __GLIBC__ /* _NL_TIME_FIRST_WEEKDAY is an enum constant, not a macro */
    /* glibc: FIRST_WEEKDAY counts from WEEK_1STDAY's weekday, a date (as
     * a number, YYYYMMDD) that is a Sunday (19971130) or a Monday
     * (19971201). */
    int first = nl_langinfo(_NL_TIME_FIRST_WEEKDAY)[0];
    const char *word = nl_langinfo(_NL_TIME_WEEK_1STDAY); /* the number itself */
    uintptr_t week_start = (uintptr_t)word;
    int base = week_start == 19971201 ? 1 : 0;
    if (first >= 1 && first <= 7)
        return (base + first - 1) % 7;
#endif
    return 0;
}

static GDate *to_gdate(xd_day day) {
    return g_date_new_dmy((GDateDay)day.day, (GDateMonth)day.month, (GDateYear)day.year);
}

static xd_day from_gdate(const GDate *date) {
    GDateYear year = g_date_get_year(date);
    GDateMonth month = g_date_get_month(date);
    GDateDay day = g_date_get_day(date);
    return (xd_day){(int)year, (int)month, (int)day};
}

int xd_calendar_weekday(xd_day day) {
    GDate *date = to_gdate(day);
    GDateWeekday weekday = g_date_get_weekday(date); /* Monday 1 ... Sunday 7 */
    g_date_free(date);
    return (int)weekday % 7;
}

void xd_calendar_page(int year, int month, int first_weekday, xd_day out[XD_CALENDAR_CELLS]) {
    xd_day first = {year, month, 1};
    int leading = (xd_calendar_weekday(first) - first_weekday + 7) % 7;
    if (leading == 0)
        leading = 7;
    GDate *date = to_gdate(first);
    g_date_subtract_days(date, (guint)leading);
    for (int cell = 0; cell < XD_CALENDAR_CELLS; cell++) {
        out[cell] = from_gdate(date);
        g_date_add_days(date, 1);
    }
    g_date_free(date);
}

xd_day xd_calendar_add_days(xd_day day, int days) {
    GDate *date = to_gdate(day);
    if (days >= 0)
        g_date_add_days(date, (guint)days);
    else
        g_date_subtract_days(date, (guint)-days);
    xd_day moved = from_gdate(date);
    g_date_free(date);
    return moved;
}

xd_day xd_calendar_add_months(xd_day day, int months) {
    /* GLib clamps the day to the new month's last. */
    GDate *date = to_gdate(day);
    if (months >= 0)
        g_date_add_months(date, (guint)months);
    else
        g_date_subtract_months(date, (guint)-months);
    xd_day moved = from_gdate(date);
    g_date_free(date);
    return moved;
}

bool xd_calendar_same(xd_day left, xd_day right) {
    return left.year == right.year && left.month == right.month && left.day == right.day;
}
