/* SPDX-License-Identifier: MIT
 * The clock flyout's month calendar (Windows' MonthCal): the 6 weeks of
 * days a month shows, from the locale's first day of the week, and moving
 * a day by days and months.
 */
#ifndef XD_SHELL_CALENDAR_H
#define XD_SHELL_CALENDAR_H

#include <stdbool.h>

enum {
    XD_CALENDAR_WEEKS = 6,
    XD_CALENDAR_CELLS = XD_CALENDAR_WEEKS * 7,
};

typedef struct xd_day {
    int year;
    int month; /* 1-12 */
    int day; /* 1-31 */
} xd_day;

/* The locale's (LC_TIME) first day of the week: 0 Sunday ... 6 Saturday. */
int xd_calendar_first_weekday(void);
/* The day's weekday: 0 Sunday ... 6 Saturday. */
int xd_calendar_weekday(xd_day day);
/* The cells of `year`-`month`'s page, week by week from `first_weekday`:
 * the end of the month before (always at least a day of it, as Windows'
 * calendar shows), the month, then the start of the next. */
void xd_calendar_page(int year, int month, int first_weekday, xd_day out[XD_CALENDAR_CELLS]);
/* `day` moved by `days`, or by `months` (the day clamped to the month's
 * last, as Windows' calendar keeps it). */
xd_day xd_calendar_add_days(xd_day day, int days);
xd_day xd_calendar_add_months(xd_day day, int months);
bool xd_calendar_same(xd_day left, xd_day right);

#endif
