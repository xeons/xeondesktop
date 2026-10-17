/* SPDX-License-Identifier: MIT
 * XP's Customize Regional Options dialog (Numbers, Currency, Time, Date)
 * for Regional and Language Options' Customize... button.
 */
#ifndef XD_INTL_CUSTOMIZE_H
#define XD_INTL_CUSTOMIZE_H

#include <gtk/gtk.h>

#include "intl.h"

/* Shows the dialog for `locale` with the changes in `custom` and keeps
 * what OK or Apply takes there (only what differs from the locale's
 * own). `applied` runs after each, to show the result. */
void xd_intl_customize(GtkWindow *parent, const char *locale, xd_intl_custom *custom,
                       void (*applied)(void *data), void *data);

#endif
