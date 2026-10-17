/* SPDX-License-Identifier: MIT
 * The parts of Regional and Language Options (xd-intl) that don't need
 * GTK: which locales are installed, their names, sample formats, and the
 * per-user formats file.
 *
 * The user's formats are LC_NUMERIC, LC_TIME, ... set in
 * ~/.config/environment.d/60-xeondesktop-locale.conf: systemd reads that
 * directory for the user's services, and xeondesktop-session reads it for
 * the session (Linux has no per-user locale.conf).
 */
#ifndef XD_INTL_INTL_H
#define XD_INTL_INTL_H

#include <glib.h>
#include <time.h>

/* The variables a "Standards and formats" choice sets. */
extern const char *const xd_intl_format_vars[];

/* The UTF-8 locales listed in `supported` (glibc's SUPPORTED: "name
 * charset" lines) that newlocale() can load, i.e. are installed, sorted
 * by name. nullptr-terminated; never nullptr. */
char **xd_intl_installed_locales(const char *supported);

/* "German (Germany)", from the locale's own LC_IDENTIFICATION; the name
 * itself if the locale has none or isn't installed. */
char *xd_intl_display_name(const char *locale);

typedef struct xd_intl_samples {
    char *number, *currency, *time, *short_date, *long_date;
} xd_intl_samples;

/* Formats 123456789.00 and `when` as `locale` would. False if the locale
 * isn't installed. */
bool xd_intl_samples_get(const char *locale, time_t when, xd_intl_samples *out);
void xd_intl_samples_free(xd_intl_samples *s);

/* A long date strftime format ("%A, %B %-d, %Y") in the order of the
 * locale's short date format `d_fmt` (D_FMT): glibc has no long date. */
const char *xd_intl_long_date_format(const char *d_fmt);

/* Whether `name` looks like a locale name (letters, digits, "_.@-"). */
bool xd_intl_valid_name(const char *name);

/* The locale the formats file at `path` sets (its LC_TIME), or nullptr. */
char *xd_intl_read_formats(const char *path);
/* Writes the formats file for `locale`; nullptr removes it. */
bool xd_intl_write_formats(const char *path, const char *locale, GError **error);

/* org.freedesktop.locale1's Locale ("LANG=...", "LC_TIME=...") with LANG
 * set to `lang` and the rest kept. */
char **xd_intl_replace_lang(const char *const *locale, const char *lang);
/* LANG from such a list, or nullptr. */
const char *xd_intl_lang(const char *const *locale);

/* locale1 keeps several X11 layouts comma-separated ("us,de"); the first
 * is the default. These read and replace the first one, keeping the rest
 * (nullptr counts as ""). */
char *xd_intl_first(const char *list);
char *xd_intl_replace_first(const char *list, const char *value);

#endif
