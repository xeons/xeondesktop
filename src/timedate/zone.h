/* SPDX-License-Identifier: MIT
 * Date and Time Properties' Time Zone tab, the parts that don't need GTK.
 */
#ifndef XD_TIMEDATE_ZONE_H
#define XD_TIMEDATE_ZONE_H

#include <glib.h>

/* The zone's standard time, as an offset from UTC in `year`: the smaller
 * of its offsets in January and July, as daylight saving only puts clocks
 * forward, in either hemisphere. The map's band is drawn there, not at
 * the summer time's offset, which would put it beside the zone's city. */
GTimeSpan xd_zone_standard_offset(GTimeZone *tz, int year);

#endif
