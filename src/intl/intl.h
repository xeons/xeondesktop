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
void xd_intl_samples_free(xd_intl_samples *intl_samples);

/* A long date strftime format ("%A, %B %-d, %Y") in the order of the
 * locale's short date format `d_fmt` (D_FMT): glibc has no long date. */
const char *xd_intl_long_date_format(const char *d_fmt);

/* Whether `name` looks like a locale name (letters, digits, "_.@-"). */
bool xd_intl_valid_name(const char *name);

/* The locale the formats file at `path` sets (its LC_TIME), or nullptr. */
char *xd_intl_read_formats(const char *path);
/* Writes the formats file for `locale`; nullptr removes it. */
bool xd_intl_write_formats(const char *path, const char *locale, GError **error);

/* ---- Customize -------------------------------------------------------------------
 *
 * glibc's formats come whole, as compiled locales, so customized formats
 * are a locale of the user's own: the chosen one with the changes,
 * written as a localedef source and compiled into
 * ~/.local/share/xeondesktop/locale, which LOCPATH in the formats file
 * points at. As LOCPATH hides the system's compiled locales, the
 * session's language (LANG and the like) is compiled there too. The
 * changes themselves are kept in ~/.config/xeondesktop/formats.ini.
 */

/* What Customize changes; nullptr / -1 / 0 = the locale's own. */
typedef struct xd_intl_custom {
    /* Numbers */
    char *decimal, *thousands;
    char *grouping;  /* localedef's: "3;3" (thousands), "3;2" (India), "-1" (none) */
    int measurement; /* 1 metric, 2 U.S.; 0 = the locale's */
    /* Currency */
    char *currency, *mon_decimal, *mon_thousands, *mon_grouping;
    int frac_digits;     /* -1 = the locale's */
    int positive_format; /* XP's: 0 "¤1.1", 1 "1.1¤", 2 "¤ 1.1", 3 "1.1 ¤"; -1 = the locale's */
    int negative_format; /* XP's 0 "(¤1.1)" ... 15 "(1.1 ¤)"; -1 = the locale's */
    /* Time: XP's picture format ("h:mm:ss tt") */
    char *time_format, *am, *pm;
    /* Date: the short date's picture format ("M/d/yyyy") */
    char *date_format;
} xd_intl_custom;

enum { XD_INTL_POSITIVE_FORMATS = 4, XD_INTL_NEGATIVE_FORMATS = 16 };
/* How currency format `i` looks with `symbol`: "$1.1", "(1.1 €)". */
char *xd_intl_currency_format_look(bool negative, int i, const char *symbol);

void xd_intl_custom_init(xd_intl_custom *custom);
void xd_intl_custom_clear(xd_intl_custom *custom);
bool xd_intl_custom_empty(const xd_intl_custom *custom);
/* `locale`'s own values in every field (the pictures converted from its
 * strftime formats); false if it isn't installed. */
bool xd_intl_custom_from_locale(const char *locale, xd_intl_custom *out);
/* `locale`'s values with `custom`'s changes over them. */
bool xd_intl_custom_effective(const char *locale, const xd_intl_custom *custom,
                              xd_intl_custom *out);

/* formats.ini: the locale the changes were made to, and the changes.
 * Loading a missing file gives none. Saving none (or a nullptr locale)
 * removes the file. */
void xd_intl_custom_load(const char *path, char **locale, xd_intl_custom *custom);
bool xd_intl_custom_save(const char *path, const char *locale, const xd_intl_custom *custom,
                         GError **error);

/* XP's time and date pictures (h hh H HH m mm s ss t tt; d dd ddd dddd M
 * MM MMM MMMM y yy yyyy; 'quoted' text) to strftime and back. A strftime
 * conversion with no picture equivalent is dropped. `ampm` is what %r
 * stands for (T_FMT_AMPM), nullptr for "%I:%M:%S %p". */
char *xd_intl_picture_to_strftime(const char *picture);
char *xd_intl_strftime_to_picture(const char *fmt, const char *ampm);
/* The separator a picture uses: the first text between two fields
 * (":" in "h:mm:ss"), or "". */
char *xd_intl_picture_separator(const char *picture);
/* `picture` with every unquoted `from` replaced by `to`. */
char *xd_intl_picture_replace_separator(const char *picture, const char *from, const char *to);

/* "123456789" grouped by localedef's `grouping` ("3;2") with `sep`. */
char *xd_intl_group_digits(const char *digits, const char *grouping, const char *sep);
/* glibc's GROUPING bytes as localedef's text ("\3\3" -> "3;3"). */
char *xd_intl_grouping_text(const char *bytes);
/* The samples Customize shows: 123456789.00 (negative: -123456789.00)
 * as numbers and as money, in the effective settings `custom`. */
char *xd_intl_number_sample(const xd_intl_custom *custom, bool negative);
char *xd_intl_currency_sample(const xd_intl_custom *custom, bool negative);
/* `when` in `custom`'s time format or short date, with `locale`'s names of
 * days and months. */
char *xd_intl_time_sample(const char *locale, const xd_intl_custom *custom, time_t when);
char *xd_intl_date_sample(const char *locale, const xd_intl_custom *custom, time_t when,
                          bool long_date);

/* The customized locale's name, "de_DE.UTF-8@xeondesktop", and back. */
char *xd_intl_custom_name(const char *locale);
/* The locale a name in the formats file stands for (the suffix off). */
char *xd_intl_custom_base(const char *name);
/* The localedef source ("de_DE", "sr_RS@latin") and charmap ("UTF-8") a
 * locale name is compiled from. */
void xd_intl_source_of(const char *locale, char **source, char **charmap);
/* The customized locale as a localedef source file; nullptr if `locale`
 * isn't installed. */
char *xd_intl_custom_source(const char *locale, const xd_intl_custom *custom);
/* Writes the formats file setting every format variable to `locale` and,
 * with `locpath`, LOCPATH; nullptr locale removes the file. */
bool xd_intl_write_formats_full(const char *path, const char *locale, const char *locpath,
                                GError **error);

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
