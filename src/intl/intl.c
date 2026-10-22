/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <monetary.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "intl.h"

/* What GNOME's Formats choice sets too: everything but the language of
 * messages, character classes and sorting. */
const char *const xd_intl_format_vars[] = {
    "LC_NUMERIC", "LC_TIME",    "LC_MONETARY", "LC_PAPER",
    "LC_MEASUREMENT", "LC_ADDRESS", "LC_TELEPHONE", "LC_NAME", nullptr,
};

bool xd_intl_valid_name(const char *name) {
    if (name == nullptr || name[0] == '\0' || strlen(name) > 128)
        return false;
    for (const char *cursor = name; *cursor != '\0'; cursor++) {
        if (!g_ascii_isalnum(*cursor) && strchr("_.@-", *cursor) == nullptr)
            return false;
    }
    return true;
}

static locale_t open_locale(const char *name) {
    if (!xd_intl_valid_name(name))
        return (locale_t)0;
    return newlocale(LC_ALL_MASK, name, (locale_t)0);
}

char **xd_intl_installed_locales(const char *supported) {
    GPtrArray *out = g_ptr_array_new();
    char *text = nullptr;
    if (g_file_get_contents(supported, &text, nullptr, nullptr)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **line = lines; *line != nullptr; line++) {
            char **fields = g_strsplit_set(g_strstrip(*line), " \t", 2);
            if (fields[0] != nullptr && fields[1] != nullptr && fields[0][0] != '#' &&
                strcmp(g_strstrip(fields[1]), "UTF-8") == 0) {
                locale_t loc = open_locale(fields[0]);
                if (loc != (locale_t)0) {
                    /* glibc falls back to a less specific installed
                     * locale; only take ones that really are UTF-8. */
                    if (strcmp(nl_langinfo_l(CODESET, loc), "UTF-8") == 0)
                        g_ptr_array_add(out, g_strdup(fields[0]));
                    freelocale(loc);
                }
            }
            g_strfreev(fields);
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_ptr_array_sort_values(out, (GCompareFunc)g_strcmp0);
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

char *xd_intl_display_name(const char *locale) {
    locale_t loc = open_locale(locale);
    if (loc == (locale_t)0)
        return g_strdup(locale != nullptr ? locale : "");
    const char *lang = nl_langinfo_l(_NL_IDENTIFICATION_LANGUAGE, loc);
    const char *terr = nl_langinfo_l(_NL_IDENTIFICATION_TERRITORY, loc);
    const char *at = strchr(locale, '@');
    char *name;
    if (lang == nullptr || lang[0] == '\0')
        name = g_strdup(locale);
    else if (terr != nullptr && terr[0] != '\0' && strcmp(terr, "ISO") != 0)
        name = at != nullptr ? g_strdup_printf("%s (%s, %s)", lang, terr, at + 1)
                             : g_strdup_printf("%s (%s)", lang, terr);
    else
        name = at != nullptr ? g_strdup_printf("%s (%s)", lang, at + 1) : g_strdup(lang);
    freelocale(loc);
    return name;
}

const char *xd_intl_long_date_format(const char *d_fmt) {
    /* The first of day, month and year in the short date decides. */
    for (const char *percent = d_fmt != nullptr ? strchr(d_fmt, '%') : nullptr; percent != nullptr;
         percent = strchr(percent + 1, '%')) {
        const char *conversion = percent + 1;
        while (*conversion == '-' || *conversion == '_' || *conversion == '0' ||
               *conversion == '^' || *conversion == '#' || *conversion == 'E' || *conversion == 'O')
            conversion++;
        switch (*conversion) {
        case 'd':
        case 'e':
            return "%A, %-d %B %Y";
        case 'm':
        case 'b':
        case 'B':
        case 'h':
        case 'D': /* %m/%d/%y */
            return "%A, %B %-d, %Y";
        case 'y':
        case 'Y':
        case 'C':
        case 'G':
        case 'F': /* %Y-%m-%d */
            return "%Y %B %-d, %A";
        case '%':
            percent = conversion; /* a literal percent sign */
            break;
        default:
            break;
        }
    }
    return "%A, %-d %B %Y";
}

static char *format_time(locale_t loc, const char *fmt, const struct tm *tm) {
    char buf[256];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    size_t length = strftime_l(buf, sizeof(buf), fmt, tm, loc);
#pragma GCC diagnostic pop
    return g_strdup(length > 0 ? buf : "");
}

bool xd_intl_samples_get(const char *locale, time_t when, xd_intl_samples *out) {
    *out = (xd_intl_samples){0};
    locale_t loc = open_locale(locale);
    if (loc == (locale_t)0)
        return false;
    char buf[128];
    /* printf's ' flag (POSIX, not ISO C) groups thousands by the current
     * LC_NUMERIC. */
    locale_t old = uselocale(loc);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    snprintf(buf, sizeof(buf), "%'.2f", 123456789.0);
#pragma GCC diagnostic pop
    uselocale(old);
    out->number = g_strdup(buf);
    out->currency = g_strdup(strfmon_l(buf, sizeof(buf), loc, "%n", 123456789.0) > 0 ? buf : "");
    struct tm tm;
    localtime_r(&when, &tm);
    out->time = format_time(loc, "%X", &tm);
    out->short_date = format_time(loc, "%x", &tm);
    out->long_date = format_time(loc, xd_intl_long_date_format(nl_langinfo_l(D_FMT, loc)), &tm);
    freelocale(loc);
    return true;
}

void xd_intl_samples_free(xd_intl_samples *intl_samples) {
    g_free(intl_samples->number);
    g_free(intl_samples->currency);
    g_free(intl_samples->time);
    g_free(intl_samples->short_date);
    g_free(intl_samples->long_date);
    *intl_samples = (xd_intl_samples){0};
}

/* ---- the formats file ------------------------------------------------------------- */

char *xd_intl_read_formats(const char *path) {
    char *text = nullptr;
    if (!g_file_get_contents(path, &text, nullptr, nullptr))
        return nullptr;
    char *found = nullptr;
    char **lines = g_strsplit(text, "\n", -1);
    for (char **current = lines; *current != nullptr && found == nullptr; current++) {
        char *line = g_strstrip(*current);
        if (!g_str_has_prefix(line, "LC_TIME="))
            continue;
        char *value = g_shell_unquote(line + strlen("LC_TIME="), nullptr);
        if (value != nullptr && xd_intl_valid_name(value))
            found = value;
        else
            g_free(value);
    }
    g_strfreev(lines);
    g_free(text);
    return found;
}

bool xd_intl_write_formats(const char *path, const char *locale, GError **error) {
    return xd_intl_write_formats_full(path, locale, nullptr, error);
}

bool xd_intl_write_formats_full(const char *path, const char *locale, const char *locpath,
                                GError **error) {
    GFile *file = g_file_new_for_path(path);
    bool ok;
    if (locale == nullptr) {
        GError *delete_error = nullptr;
        ok = g_file_delete(file, nullptr, &delete_error) ||
             g_error_matches(delete_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
        if (!ok)
            g_propagate_error(error, delete_error);
        else
            g_clear_error(&delete_error);
        g_object_unref(file);
        return ok;
    }
    if (!xd_intl_valid_name(locale)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not a locale: %s", locale);
        g_object_unref(file);
        return false;
    }
    GString *string = g_string_new("# Regional and Language Options: Standards and formats\n");
    /* Customized formats: the compiled locales there (intl.h). */
    if (locpath != nullptr)
        g_string_append_printf(string, "LOCPATH=%s\n", locpath);
    for (const char *const *variable = xd_intl_format_vars; *variable != nullptr; variable++)
        g_string_append_printf(string, "%s=%s\n", *variable, locale);
    char *dir = g_path_get_dirname(path);
    ok = g_mkdir_with_parents(dir, 0755) == 0;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create %s", dir);
    else
        ok = g_file_replace_contents(file, string->str, string->len, nullptr, FALSE,
                                     G_FILE_CREATE_REPLACE_DESTINATION, nullptr, nullptr, error);
    g_free(dir);
    g_string_free(string, TRUE);
    g_object_unref(file);
    return ok;
}

/* ---- Customize ------------------------------------------------------------------- */

void xd_intl_custom_init(xd_intl_custom *custom) {
    *custom = (xd_intl_custom){.frac_digits = -1, .positive_format = -1, .negative_format = -1};
}

void xd_intl_custom_clear(xd_intl_custom *custom) {
    char **strings[] = {&custom->decimal,      &custom->thousands,   &custom->grouping,
                        &custom->currency,     &custom->mon_decimal, &custom->mon_thousands,
                        &custom->mon_grouping, &custom->time_format, &custom->am,
                        &custom->pm,           &custom->date_format};
    for (size_t i = 0; i < G_N_ELEMENTS(strings); i++)
        g_clear_pointer(strings[i], g_free);
    xd_intl_custom_init(custom);
}

bool xd_intl_custom_empty(const xd_intl_custom *custom) {
    const char *strings[] = {custom->decimal,      custom->thousands,   custom->grouping,
                             custom->currency,     custom->mon_decimal, custom->mon_thousands,
                             custom->mon_grouping, custom->time_format, custom->am,
                             custom->pm,           custom->date_format};
    for (size_t i = 0; i < G_N_ELEMENTS(strings); i++)
        if (strings[i] != nullptr)
            return false;
    return custom->measurement == 0 && custom->frac_digits < 0 && custom->positive_format < 0 &&
           custom->negative_format < 0;
}

/* XP's currency formats as glibc's p/n_cs_precedes, p/n_sep_by_space
 * and n_sign_posn, with how they look ($ the symbol, 1.1 the amount). */
static const struct {
    int cs_precedes, sep_by_space, sign_posn;
    const char *look;
} negative_formats[XD_INTL_NEGATIVE_FORMATS] = {
    {1, 0, 0, "($1.1)"}, {1, 0, 1, "-$1.1"},  {1, 0, 4, "$-1.1"},  {1, 0, 2, "$1.1-"},
    {0, 0, 0, "(1.1$)"}, {0, 0, 1, "-1.1$"},  {0, 0, 3, "1.1-$"},  {0, 0, 2, "1.1$-"},
    {0, 1, 1, "-1.1 $"}, {1, 1, 1, "-$ 1.1"}, {0, 1, 2, "1.1 $-"}, {1, 1, 2, "$ 1.1-"},
    {1, 2, 4, "$ -1.1"}, {0, 2, 3, "1.1- $"}, {1, 1, 0, "($ 1.1)"}, {0, 1, 0, "(1.1 $)"},
};
static const struct {
    int cs_precedes, sep_by_space;
    const char *look;
} positive_formats[XD_INTL_POSITIVE_FORMATS] = {
    {1, 0, "$1.1"}, {0, 0, "1.1$"}, {1, 1, "$ 1.1"}, {0, 1, "1.1 $"}};

/* A look with the symbol and amount put in. */
static char *fill_look(const char *look, const char *symbol, const char *amount) {
    GString *string = g_string_new(nullptr);
    for (const char *cursor = look; *cursor != '\0'; cursor++) {
        if (*cursor == '$') {
            g_string_append(string, symbol != nullptr ? symbol : "");
        } else if (g_str_has_prefix(cursor, "1.1")) {
            g_string_append(string, amount);
            cursor += 2;
        } else {
            g_string_append_c(string, *cursor);
        }
    }
    return g_string_free(string, FALSE);
}

char *xd_intl_currency_format_look(bool negative, int i, const char *symbol) {
    const char *look = negative ? negative_formats[CLAMP(i, 0, XD_INTL_NEGATIVE_FORMATS - 1)].look
                                : positive_formats[CLAMP(i, 0, XD_INTL_POSITIVE_FORMATS - 1)].look;
    return fill_look(look, symbol, "1.1");
}

static int positive_format_of(int cs, int sep) {
    for (int i = 0; i < XD_INTL_POSITIVE_FORMATS; i++)
        if (positive_formats[i].cs_precedes == cs && positive_formats[i].sep_by_space == (sep != 0))
            return i;
    return 0;
}

static int negative_format_of(int cs, int sep, int posn) {
    for (int i = 0; i < XD_INTL_NEGATIVE_FORMATS; i++)
        if (negative_formats[i].cs_precedes == cs && negative_formats[i].sep_by_space == sep &&
            negative_formats[i].sign_posn == posn)
            return i;
    /* Close enough: the same order, any spacing. */
    for (int i = 0; i < XD_INTL_NEGATIVE_FORMATS; i++)
        if (negative_formats[i].cs_precedes == cs && negative_formats[i].sign_posn == posn)
            return i;
    return cs ? 1 : 5;
}

/* A one-byte langinfo value (CHAR_MAX: unspecified). */
static int byte_info(nl_item item, locale_t loc) {
    return (unsigned char)nl_langinfo_l(item, loc)[0];
}

char *xd_intl_grouping_text(const char *bytes) {
    GString *string = g_string_new(nullptr);
    for (const char *cursor = bytes; cursor != nullptr && *cursor != '\0'; cursor++) {
        int size = (unsigned char)*cursor;
        if (string->len > 0)
            g_string_append_c(string, ';');
        /* "No further grouping" is CHAR_MAX: 127 where char is signed
         * (x86), 255 where it isn't (ARM); locale data may carry either.
         * No group is that long. */
        if (size == 0 || size >= SCHAR_MAX) {
            g_string_append(string, "-1");
            break;
        }
        g_string_append_printf(string, "%d", size);
    }
    if (string->len == 0)
        g_string_append(string, "-1");
    return g_string_free(string, FALSE);
}

static bool grouping_valid(const char *grouping) {
    if (grouping == nullptr || grouping[0] == '\0' || strlen(grouping) > 32)
        return false;
    char **parts = g_strsplit(grouping, ";", -1);
    bool ok = true;
    for (char **part = parts; ok && *part != nullptr; part++) {
        gint64 size = 0;
        ok = g_ascii_string_to_signed(*part, 10, -1, 99, &size, nullptr);
    }
    g_strfreev(parts);
    return ok;
}

bool xd_intl_custom_from_locale(const char *locale, xd_intl_custom *out) {
    xd_intl_custom_init(out);
    locale_t loc = open_locale(locale);
    if (loc == (locale_t)0)
        return false;
    out->decimal = g_strdup(nl_langinfo_l(RADIXCHAR, loc));
    out->thousands = g_strdup(nl_langinfo_l(THOUSEP, loc));
    out->grouping = xd_intl_grouping_text(nl_langinfo_l(GROUPING, loc));
    int measurement = byte_info(_NL_MEASUREMENT_MEASUREMENT, loc);
    out->measurement = measurement == 2 ? 2 : 1;
    out->currency = g_strdup(nl_langinfo_l(CURRENCY_SYMBOL, loc));
    out->mon_decimal = g_strdup(nl_langinfo_l(MON_DECIMAL_POINT, loc));
    out->mon_thousands = g_strdup(nl_langinfo_l(MON_THOUSANDS_SEP, loc));
    out->mon_grouping = xd_intl_grouping_text(nl_langinfo_l(MON_GROUPING, loc));
    int frac = byte_info(FRAC_DIGITS, loc);
    out->frac_digits = frac == CHAR_MAX ? 2 : frac;
    int pcs = byte_info(P_CS_PRECEDES, loc), psep = byte_info(P_SEP_BY_SPACE, loc);
    int ncs = byte_info(N_CS_PRECEDES, loc), nsep = byte_info(N_SEP_BY_SPACE, loc);
    int nposn = byte_info(N_SIGN_POSN, loc);
    out->positive_format = positive_format_of(pcs == 1, psep == CHAR_MAX ? 0 : psep);
    out->negative_format = negative_format_of(ncs == 1, nsep == CHAR_MAX ? 0 : nsep,
                                              nposn == CHAR_MAX ? 1 : nposn);
    out->time_format = xd_intl_strftime_to_picture(nl_langinfo_l(T_FMT, loc),
                                                   nl_langinfo_l(T_FMT_AMPM, loc));
    out->am = g_strdup(nl_langinfo_l(AM_STR, loc));
    out->pm = g_strdup(nl_langinfo_l(PM_STR, loc));
    out->date_format = xd_intl_strftime_to_picture(nl_langinfo_l(D_FMT, loc), nullptr);
    freelocale(loc);
    return true;
}

static void over(char **dst, const char *value) {
    if (value != nullptr) {
        g_free(*dst);
        *dst = g_strdup(value);
    }
}

bool xd_intl_custom_effective(const char *locale, const xd_intl_custom *custom,
                              xd_intl_custom *out) {
    if (!xd_intl_custom_from_locale(locale, out))
        return false;
    over(&out->decimal, custom->decimal);
    over(&out->thousands, custom->thousands);
    over(&out->grouping, custom->grouping);
    over(&out->currency, custom->currency);
    over(&out->mon_decimal, custom->mon_decimal);
    over(&out->mon_thousands, custom->mon_thousands);
    over(&out->mon_grouping, custom->mon_grouping);
    over(&out->time_format, custom->time_format);
    over(&out->am, custom->am);
    over(&out->pm, custom->pm);
    over(&out->date_format, custom->date_format);
    if (custom->measurement != 0)
        out->measurement = custom->measurement;
    if (custom->frac_digits >= 0)
        out->frac_digits = custom->frac_digits;
    if (custom->positive_format >= 0)
        out->positive_format = custom->positive_format;
    if (custom->negative_format >= 0)
        out->negative_format = custom->negative_format;
    return true;
}

/* ---- formats.ini ---- */

#define CUSTOM_GROUP "Customize"

static const struct {
    const char *key;
    size_t offset;
} string_keys[] = {
    {"DecimalSymbol", offsetof(xd_intl_custom, decimal)},
    {"DigitGroupingSymbol", offsetof(xd_intl_custom, thousands)},
    {"DigitGrouping", offsetof(xd_intl_custom, grouping)},
    {"CurrencySymbol", offsetof(xd_intl_custom, currency)},
    {"CurrencyDecimalSymbol", offsetof(xd_intl_custom, mon_decimal)},
    {"CurrencyDigitGroupingSymbol", offsetof(xd_intl_custom, mon_thousands)},
    {"CurrencyDigitGrouping", offsetof(xd_intl_custom, mon_grouping)},
    {"TimeFormat", offsetof(xd_intl_custom, time_format)},
    {"AMSymbol", offsetof(xd_intl_custom, am)},
    {"PMSymbol", offsetof(xd_intl_custom, pm)},
    {"ShortDateFormat", offsetof(xd_intl_custom, date_format)},
};

static char **string_field(xd_intl_custom *custom, size_t i) {
    return (char **)(void *)((char *)custom + string_keys[i].offset);
}

static int int_key(GKeyFile *kf, const char *key, int lo, int hi, int none) {
    GError *error = nullptr;
    int value = g_key_file_get_integer(kf, CUSTOM_GROUP, key, &error);
    bool ok = error == nullptr && value >= lo && value <= hi;
    g_clear_error(&error);
    return ok ? value : none;
}

void xd_intl_custom_load(const char *path, char **locale, xd_intl_custom *custom) {
    xd_intl_custom_init(custom);
    *locale = nullptr;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr)) {
        char *locale_name = g_key_file_get_string(kf, CUSTOM_GROUP, "Locale", nullptr);
        if (xd_intl_valid_name(locale_name))
            *locale = locale_name;
        else
            g_free(locale_name);
        for (size_t i = 0; i < G_N_ELEMENTS(string_keys); i++) {
            char *value = g_key_file_get_string(kf, CUSTOM_GROUP, string_keys[i].key, nullptr);
            bool grouping = strstr(string_keys[i].key, "Grouping") != nullptr &&
                            strstr(string_keys[i].key, "Symbol") == nullptr;
            bool ok = value != nullptr && strlen(value) <= 80 &&
                      g_utf8_validate(value, -1, nullptr) && (!grouping || grouping_valid(value));
            if (ok)
                *string_field(custom, i) = value;
            else
                g_free(value);
        }
        custom->measurement = int_key(kf, "MeasurementSystem", 1, 2, 0);
        custom->frac_digits = int_key(kf, "CurrencyDigits", 0, 9, -1);
        custom->positive_format =
            int_key(kf, "PositiveCurrencyFormat", 0, XD_INTL_POSITIVE_FORMATS - 1, -1);
        custom->negative_format =
            int_key(kf, "NegativeCurrencyFormat", 0, XD_INTL_NEGATIVE_FORMATS - 1, -1);
    }
    g_key_file_free(kf);
}

bool xd_intl_custom_save(const char *path, const char *locale, const xd_intl_custom *custom,
                         GError **error) {
    if (locale == nullptr || xd_intl_custom_empty(custom)) {
        if (g_remove(path) == 0 || errno == ENOENT)
            return true;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot remove %s: %s", path,
                    g_strerror(errno));
        return false;
    }
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_comment(kf, nullptr, nullptr,
                           " Regional and Language Options: Customize. Applied as the locale "
                           "in ~/.local/share/xeondesktop/locale.",
                           nullptr);
    g_key_file_set_string(kf, CUSTOM_GROUP, "Locale", locale);
    for (size_t i = 0; i < G_N_ELEMENTS(string_keys); i++) {
        const char *value = *string_field((xd_intl_custom *)(uintptr_t)custom, i);
        if (value != nullptr)
            g_key_file_set_string(kf, CUSTOM_GROUP, string_keys[i].key, value);
    }
    if (custom->measurement != 0)
        g_key_file_set_integer(kf, CUSTOM_GROUP, "MeasurementSystem", custom->measurement);
    if (custom->frac_digits >= 0)
        g_key_file_set_integer(kf, CUSTOM_GROUP, "CurrencyDigits", custom->frac_digits);
    if (custom->positive_format >= 0)
        g_key_file_set_integer(kf, CUSTOM_GROUP, "PositiveCurrencyFormat", custom->positive_format);
    if (custom->negative_format >= 0)
        g_key_file_set_integer(kf, CUSTOM_GROUP, "NegativeCurrencyFormat", custom->negative_format);
    char *dir = g_path_get_dirname(path);
    bool ok = g_mkdir_with_parents(dir, 0755) == 0;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create %s", dir);
    else
        ok = g_key_file_save_to_file(kf, path, error);
    g_free(dir);
    g_key_file_free(kf);
    return ok;
}

/* ---- picture formats ---- */

/* The letters that make XP's time and date fields. */
static bool field_letter(char character) {
    return strchr("hHmstdMyg", character) != nullptr && character != '\0';
}

/* Walks a picture: calls `field` for each run of a field letter and
 * `text` for each piece of text (quotes resolved). */
typedef void (*field_fn)(char letter, int count, void *data);
typedef void (*text_fn)(const char *text, size_t len, bool quoted, void *data);

static void walk_picture(const char *picture, field_fn field, text_fn text, void *data) {
    const char *cursor = picture;
    while (cursor != nullptr && *cursor != '\0') {
        if (*cursor == '\'') {
            GString *string = g_string_new(nullptr);
            cursor++;
            while (*cursor != '\0') {
                if (*cursor == '\'' && cursor[1] == '\'') {
                    g_string_append_c(string, '\'');
                    cursor += 2;
                } else if (*cursor == '\'') {
                    cursor++;
                    break;
                } else {
                    g_string_append_c(string, *cursor++);
                }
            }
            text(string->str, string->len, true, data);
            g_string_free(string, TRUE);
        } else if (field_letter(*cursor)) {
            char letter = *cursor;
            int count = 0;
            while (*cursor == letter) {
                cursor++;
                count++;
            }
            field(letter, count, data);
        } else {
            const char *start = cursor;
            while (*cursor != '\0' && *cursor != '\'' && !field_letter(*cursor))
                cursor++;
            text(start, (size_t)(cursor - start), false, data);
        }
    }
}

static void to_strftime_field(char letter, int count, void *data) {
    GString *string = data;
    switch (letter) {
    case 'h':
        g_string_append(string, count == 1 ? "%-I" : "%I");
        break;
    case 'H':
        g_string_append(string, count == 1 ? "%-H" : "%H");
        break;
    case 'm':
        g_string_append(string, count == 1 ? "%-M" : "%M");
        break;
    case 's':
        g_string_append(string, count == 1 ? "%-S" : "%S");
        break;
    case 't':
        g_string_append(string, "%p");
        break;
    case 'd':
        g_string_append(string, count == 1 ? "%-d" : count == 2 ? "%d" : count == 3 ? "%a" : "%A");
        break;
    case 'M':
        g_string_append(string, count == 1 ? "%-m" : count == 2 ? "%m" : count == 3 ? "%b" : "%B");
        break;
    case 'y':
        g_string_append(string, count == 1 ? "%-y" : count == 2 ? "%y" : "%Y");
        break;
    default: /* g: the era, which glibc's formats don't show this way */
        break;
    }
}

static void to_strftime_text(const char *text, size_t len, [[maybe_unused]] bool quoted,
                             void *data) {
    GString *string = data;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '%')
            g_string_append_c(string, '%');
        g_string_append_c(string, text[i]);
    }
}

char *xd_intl_picture_to_strftime(const char *picture) {
    GString *string = g_string_new(nullptr);
    walk_picture(picture, to_strftime_field, to_strftime_text, string);
    return g_string_free(string, FALSE);
}

/* Text in a picture: letters (and quotes) quoted. */
static void picture_text(GString *string, const char *text, size_t len) {
    bool needs = false;
    for (size_t i = 0; i < len && !needs; i++)
        needs = g_ascii_isalpha(text[i]) || text[i] == '\'';
    if (!needs) {
        g_string_append_len(string, text, (gssize)len);
        return;
    }
    g_string_append_c(string, '\'');
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\'')
            g_string_append_c(string, '\'');
        g_string_append_c(string, text[i]);
    }
    g_string_append_c(string, '\'');
}

static void strftime_picture(GString *string, const char *fmt, const char *ampm, int depth) {
    const char *cursor = fmt;
    while (cursor != nullptr && *cursor != '\0') {
        if (*cursor != '%') {
            const char *start = cursor;
            while (*cursor != '\0' && *cursor != '%')
                cursor++;
            picture_text(string, start, (size_t)(cursor - start));
            continue;
        }
        cursor++;
        bool no_pad = false;
        while (*cursor == '-' || *cursor == '_' || *cursor == '0' || *cursor == '^' ||
               *cursor == '#') {
            no_pad = no_pad || *cursor == '-';
            cursor++;
        }
        while (g_ascii_isdigit(*cursor))
            cursor++;
        if (*cursor == 'E' || *cursor == 'O')
            cursor++;
        if (*cursor == '\0')
            break;
        const char *pic = nullptr;
        switch (*cursor) {
        case 'H': pic = no_pad ? "H" : "HH"; break;
        case 'k': pic = "H"; break;
        case 'I': pic = no_pad ? "h" : "hh"; break;
        case 'l': pic = "h"; break;
        case 'M': pic = no_pad ? "m" : "mm"; break;
        case 'S': pic = no_pad ? "s" : "ss"; break;
        case 'p':
        case 'P': pic = "tt"; break;
        case 'R': pic = "HH:mm"; break;
        case 'T':
        case 'X': pic = "HH:mm:ss"; break;
        case 'r':
            if (depth == 0 && ampm != nullptr && ampm[0] != '\0')
                strftime_picture(string, ampm, nullptr, depth + 1);
            else
                pic = "hh:mm:ss tt";
            break;
        case 'd': pic = no_pad ? "d" : "dd"; break;
        case 'e': pic = "d"; break;
        case 'm': pic = no_pad ? "M" : "MM"; break;
        case 'b':
        case 'h': pic = "MMM"; break;
        case 'B': pic = "MMMM"; break;
        case 'a': pic = "ddd"; break;
        case 'A': pic = "dddd"; break;
        case 'y':
        case 'g': pic = no_pad ? "y" : "yy"; break;
        case 'Y':
        case 'G': pic = "yyyy"; break;
        case 'D':
        case 'x': pic = "MM/dd/yy"; break;
        case 'F': pic = "yyyy-MM-dd"; break;
        case '%': pic = "%"; break;
        default: break; /* no picture field: dropped */
        }
        if (pic != nullptr)
            g_string_append(string, pic);
        cursor++;
    }
}

char *xd_intl_strftime_to_picture(const char *fmt, const char *ampm) {
    GString *string = g_string_new(nullptr);
    strftime_picture(string, fmt, ampm, 0);
    return g_string_free(string, FALSE);
}

typedef struct sep_finder {
    bool seen_field, done;
    GString *sep;
} sep_finder;

static void sep_field([[maybe_unused]] char letter, [[maybe_unused]] int count, void *data) {
    sep_finder *finder = data;
    if (finder->seen_field && finder->sep->len > 0)
        finder->done = true;
    finder->seen_field = true;
}

static void sep_text(const char *text, size_t len, [[maybe_unused]] bool quoted, void *data) {
    sep_finder *finder = data;
    if (finder->seen_field && !finder->done)
        g_string_append_len(finder->sep, text, (gssize)len);
}

char *xd_intl_picture_separator(const char *picture) {
    sep_finder finder = {.sep = g_string_new(nullptr)};
    walk_picture(picture, sep_field, sep_text, &finder);
    /* Text after the last field isn't a separator. */
    if (!finder.done)
        g_string_truncate(finder.sep, 0);
    return g_string_free(finder.sep, FALSE);
}

typedef struct sep_replacer {
    GString *out;
    const char *from, *to;
} sep_replacer;

static void replace_field(char letter, int count, void *data) {
    sep_replacer *replacer = data;
    for (int i = 0; i < count; i++)
        g_string_append_c(replacer->out, letter);
}

static void replace_text(const char *text, size_t len, bool quoted, void *data) {
    sep_replacer *replacer = data;
    char *piece = g_strndup(text, len);
    if (!quoted && replacer->from[0] != '\0') {
        char **parts = g_strsplit(piece, replacer->from, -1);
        g_free(piece);
        piece = g_strjoinv(replacer->to, parts);
        g_strfreev(parts);
    }
    picture_text(replacer->out, piece, strlen(piece));
    g_free(piece);
}

char *xd_intl_picture_replace_separator(const char *picture, const char *from, const char *to) {
    sep_replacer replacer = {g_string_new(nullptr), from != nullptr ? from : "",
                             to != nullptr ? to : ""};
    walk_picture(picture, replace_field, replace_text, &replacer);
    return g_string_free(replacer.out, FALSE);
}

/* ---- samples ---- */

char *xd_intl_group_digits(const char *digits, const char *grouping, const char *sep) {
    char **parts = g_strsplit(grouping != nullptr ? grouping : "-1", ";", -1);
    guint nparts = g_strv_length(parts);
    size_t len = strlen(digits);
    /* Group sizes from the right; the last one repeats. */
    GPtrArray *groups = g_ptr_array_new_with_free_func(g_free);
    size_t end = len;
    for (guint i = 0; end > 0; i = MIN(i + 1, nparts > 0 ? nparts - 1 : 0)) {
        gint64 size = -1;
        if (nparts > 0)
            g_ascii_string_to_signed(parts[i], 10, -1, 99, &size, nullptr);
        if (size <= 0 || (size_t)size >= end) {
            g_ptr_array_add(groups, g_strndup(digits, end));
            break;
        }
        g_ptr_array_add(groups, g_strndup(digits + end - (size_t)size, (gsize)size));
        end -= (size_t)size;
    }
    GString *string = g_string_new(nullptr);
    for (guint i = groups->len; i-- > 0;) {
        g_string_append(string, g_ptr_array_index(groups, i));
        if (i > 0)
            g_string_append(string, sep != nullptr ? sep : "");
    }
    g_ptr_array_unref(groups);
    g_strfreev(parts);
    return g_string_free(string, FALSE);
}

char *xd_intl_number_sample(const xd_intl_custom *custom, bool negative) {
    char *whole = xd_intl_group_digits("123456789", custom->grouping, custom->thousands);
    char *sample = g_strdup_printf("%s%s%s00", negative ? "-" : "", whole,
                                   custom->decimal != nullptr ? custom->decimal : ".");
    g_free(whole);
    return sample;
}

char *xd_intl_currency_sample(const xd_intl_custom *custom, bool negative) {
    char *whole = xd_intl_group_digits("123456789", custom->mon_grouping, custom->mon_thousands);
    GString *amount = g_string_new(whole);
    g_free(whole);
    if (custom->frac_digits > 0) {
        g_string_append(amount, custom->mon_decimal != nullptr ? custom->mon_decimal : ".");
        for (int i = 0; i < custom->frac_digits; i++)
            g_string_append_c(amount, '0');
    }
    const char *look =
        negative
            ? negative_formats[CLAMP(custom->negative_format, 0, XD_INTL_NEGATIVE_FORMATS - 1)].look
            : positive_formats[CLAMP(custom->positive_format, 0, XD_INTL_POSITIVE_FORMATS - 1)]
                  .look;
    char *sample = fill_look(look, custom->currency, amount->str);
    g_string_free(amount, TRUE);
    return sample;
}

char *xd_intl_time_sample(const char *locale, const xd_intl_custom *custom, time_t when) {
    locale_t loc = open_locale(locale);
    if (loc == (locale_t)0)
        return g_strdup("");
    struct tm tm;
    localtime_r(&when, &tm);
    char *fmt =
        xd_intl_picture_to_strftime(custom->time_format != nullptr ? custom->time_format : "");
    /* AM and PM as set here, not the locale's. */
    const char *mark = tm.tm_hour < 12 ? custom->am : custom->pm;
    char *escaped = mark != nullptr ? g_strdup(mark) : g_strdup("");
    char **bits = g_strsplit(escaped, "%", -1);
    g_free(escaped);
    escaped = g_strjoinv("%%", bits);
    g_strfreev(bits);
    bits = g_strsplit(fmt, "%p", -1);
    char *with = g_strjoinv(escaped, bits);
    g_strfreev(bits);
    char *out = format_time(loc, with, &tm);
    g_free(with);
    g_free(escaped);
    g_free(fmt);
    freelocale(loc);
    return out;
}

char *xd_intl_date_sample(const char *locale, const xd_intl_custom *custom, time_t when,
                          bool long_date) {
    locale_t loc = open_locale(locale);
    if (loc == (locale_t)0)
        return g_strdup("");
    struct tm tm;
    localtime_r(&when, &tm);
    char *fmt =
        xd_intl_picture_to_strftime(custom->date_format != nullptr ? custom->date_format : "");
    char *out = format_time(loc, long_date ? xd_intl_long_date_format(fmt) : fmt, &tm);
    g_free(fmt);
    freelocale(loc);
    return out;
}

/* ---- the customized locale ---- */

#define CUSTOM_SUFFIX "@xeondesktop"

char *xd_intl_custom_name(const char *locale) { return g_strconcat(locale, CUSTOM_SUFFIX, nullptr); }

char *xd_intl_custom_base(const char *name) {
    if (name != nullptr && g_str_has_suffix(name, CUSTOM_SUFFIX))
        return g_strndup(name, strlen(name) - strlen(CUSTOM_SUFFIX));
    return g_strdup(name);
}

void xd_intl_source_of(const char *locale, char **source, char **charmap) {
    const char *dot = strchr(locale, '.');
    const char *at = strchr(locale, '@');
    const char *end_code = at != nullptr ? at : locale + strlen(locale);
    if (dot != nullptr && dot > end_code)
        dot = nullptr;
    const char *end_name = dot != nullptr ? dot : end_code;
    /* "sr_RS.UTF-8@latin": the source is "sr_RS@latin". */
    char *name = g_strndup(locale, (gsize)(end_name - locale));
    *source = g_strconcat(name, at != nullptr ? at : "", nullptr);
    g_free(name);
    char *code = dot != nullptr ? g_strndup(dot + 1, (gsize)(end_code - dot - 1)) : nullptr;
    if (code == nullptr || g_ascii_strcasecmp(code, "utf8") == 0 ||
        g_ascii_strcasecmp(code, "utf-8") == 0) {
        g_free(code);
        code = g_strdup("UTF-8");
    }
    *charmap = code;
}

/* A localedef string: every character as <Uxxxx>, so nothing needs
 * escaping. */
static void put_string(GString *string, const char *text) {
    g_string_append_c(string, '"');
    for (const char *cursor = text; cursor != nullptr && *cursor != '\0';
         cursor = g_utf8_next_char(cursor)) {
        unsigned code_point = (unsigned)g_utf8_get_char(cursor);
        /* localedef: four hex digits, eight beyond the BMP */
        g_string_append_printf(string, code_point > 0xFFFF ? "<U%08X>" : "<U%04X>", code_point);
    }
    g_string_append_c(string, '"');
}

static void put_key(GString *string, const char *key, const char *text) {
    g_string_append_printf(string, "%s ", key);
    put_string(string, text);
    g_string_append_c(string, '\n');
}

static void put_list(GString *string, const char *key, locale_t loc, nl_item first, int count) {
    g_string_append_printf(string, "%s ", key);
    for (int i = 0; i < count; i++) {
        if (i > 0)
            g_string_append(string, ";");
        put_string(string, nl_langinfo_l((nl_item)(first + i), loc));
    }
    g_string_append_c(string, '\n');
}

/* A category as the source locale has it; its name is plain text (from
 * a name xd_intl_valid_name() accepted). */
static void put_copy(GString *string, const char *category, const char *source) {
    g_string_append_printf(string, "%s\ncopy \"%s\"\nEND %s\n\n", category, source, category);
}

/* A langinfo word (LC_TIME's week start, a date). */
static unsigned word_info(nl_item item, locale_t loc) {
    union {
        const char *string;
        unsigned word;
    } value = {.string = nl_langinfo_l(item, loc)};
    return value.word;
}

static void put_time(GString *string, locale_t loc, const xd_intl_custom *custom) {
    g_string_append(string, "LC_TIME\n");
    put_list(string, "abday", loc, ABDAY_1, 7);
    put_list(string, "day", loc, DAY_1, 7);
    put_list(string, "abmon", loc, ABMON_1, 12);
    put_list(string, "mon", loc, MON_1, 12);
    put_list(string, "alt_mon", loc, ALTMON_1, 12);
    put_list(string, "ab_alt_mon", loc, _NL_ABALTMON_1, 12);
    put_key(string, "d_t_fmt", nl_langinfo_l(D_T_FMT, loc));
    char *d_fmt = custom->date_format != nullptr ? xd_intl_picture_to_strftime(custom->date_format)
                                                 : g_strdup(nl_langinfo_l(D_FMT, loc));
    put_key(string, "d_fmt", d_fmt);
    g_free(d_fmt);
    char *t_fmt = custom->time_format != nullptr ? xd_intl_picture_to_strftime(custom->time_format)
                                                 : g_strdup(nl_langinfo_l(T_FMT, loc));
    put_key(string, "t_fmt", t_fmt);
    /* %r is the 12-hour time: the new format if it has AM and PM. */
    put_key(string, "t_fmt_ampm",
            strstr(t_fmt, "%p") != nullptr ? t_fmt : nl_langinfo_l(T_FMT_AMPM, loc));
    g_free(t_fmt);
    g_string_append(string, "am_pm ");
    put_string(string, custom->am != nullptr ? custom->am : nl_langinfo_l(AM_STR, loc));
    g_string_append_c(string, ';');
    put_string(string, custom->pm != nullptr ? custom->pm : nl_langinfo_l(PM_STR, loc));
    g_string_append_c(string, '\n');
    put_key(string, "date_fmt", nl_langinfo_l(_DATE_FMT, loc));
    int ndays = byte_info(_NL_TIME_WEEK_NDAYS, loc);
    unsigned first_day = word_info(_NL_TIME_WEEK_1STDAY, loc);
    int first_week = byte_info(_NL_TIME_WEEK_1STWEEK, loc);
    if (ndays > 0 && first_day > 0)
        g_string_append_printf(string, "week %d;%u;%d\n", ndays, first_day, first_week);
    int first_weekday = byte_info(_NL_TIME_FIRST_WEEKDAY, loc);
    int first_workday = byte_info(_NL_TIME_FIRST_WORKDAY, loc);
    int direction = byte_info(_NL_TIME_CAL_DIRECTION, loc);
    if (first_weekday > 0 && first_weekday <= 7)
        g_string_append_printf(string, "first_weekday %d\n", first_weekday);
    if (first_workday > 0 && first_workday <= 7)
        g_string_append_printf(string, "first_workday %d\n", first_workday);
    if (direction > 0 && direction <= 3)
        g_string_append_printf(string, "cal_direction %d\n", direction);
    g_string_append(string, "END LC_TIME\n\n");
}

static void put_byte(GString *string, const char *key, int value) {
    g_string_append_printf(string, "%s %d\n", key, value == CHAR_MAX ? -1 : value);
}

static void put_monetary(GString *string, locale_t loc, const xd_intl_custom *custom) {
    g_string_append(string, "LC_MONETARY\n");
    put_key(string, "int_curr_symbol", nl_langinfo_l(INT_CURR_SYMBOL, loc));
    put_key(string, "currency_symbol",
            custom->currency != nullptr ? custom->currency : nl_langinfo_l(CURRENCY_SYMBOL, loc));
    put_key(string, "mon_decimal_point",
            custom->mon_decimal != nullptr ? custom->mon_decimal
                                           : nl_langinfo_l(MON_DECIMAL_POINT, loc));
    put_key(string, "mon_thousands_sep",
            custom->mon_thousands != nullptr ? custom->mon_thousands
                                             : nl_langinfo_l(MON_THOUSANDS_SEP, loc));
    char *grouping = custom->mon_grouping != nullptr
                         ? g_strdup(custom->mon_grouping)
                         : xd_intl_grouping_text(nl_langinfo_l(MON_GROUPING, loc));
    g_string_append_printf(string, "mon_grouping %s\n", grouping);
    g_free(grouping);
    put_key(string, "positive_sign", nl_langinfo_l(POSITIVE_SIGN, loc));
    const char *negative = nl_langinfo_l(NEGATIVE_SIGN, loc);
    /* XP's negative formats show a minus sign. */
    put_key(string, "negative_sign",
            custom->negative_format >= 0 && negative[0] == '\0' ? "-" : negative);
    int frac = custom->frac_digits >= 0 ? custom->frac_digits : byte_info(FRAC_DIGITS, loc);
    put_byte(string, "int_frac_digits",
             custom->frac_digits >= 0 ? custom->frac_digits : byte_info(INT_FRAC_DIGITS, loc));
    put_byte(string, "frac_digits", frac);
    int pcs = byte_info(P_CS_PRECEDES, loc), psep = byte_info(P_SEP_BY_SPACE, loc);
    if (custom->positive_format >= 0) {
        pcs = positive_formats[custom->positive_format].cs_precedes;
        psep = positive_formats[custom->positive_format].sep_by_space;
    }
    int ncs = byte_info(N_CS_PRECEDES, loc), nsep = byte_info(N_SEP_BY_SPACE, loc);
    int nposn = byte_info(N_SIGN_POSN, loc);
    if (custom->negative_format >= 0) {
        ncs = negative_formats[custom->negative_format].cs_precedes;
        nsep = negative_formats[custom->negative_format].sep_by_space;
        nposn = negative_formats[custom->negative_format].sign_posn;
    }
    put_byte(string, "p_cs_precedes", pcs);
    put_byte(string, "p_sep_by_space", psep);
    put_byte(string, "n_cs_precedes", ncs);
    put_byte(string, "n_sep_by_space", nsep);
    put_byte(string, "p_sign_posn", byte_info(P_SIGN_POSN, loc));
    put_byte(string, "n_sign_posn", nposn);
    g_string_append(string, "END LC_MONETARY\n\n");
}

char *xd_intl_custom_source(const char *locale, const xd_intl_custom *custom) {
    locale_t loc = open_locale(locale);
    if (loc == (locale_t)0)
        return nullptr;
    char *source = nullptr, *charmap = nullptr;
    xd_intl_source_of(locale, &source, &charmap);
    GString *string = g_string_new(nullptr);
    g_string_append_printf(string,
                           "comment_char %%\nescape_char /\n"
                           "%% Regional and Language Options: %s, customized\n\n",
                           locale);
    put_copy(string, "LC_IDENTIFICATION", source);
    put_copy(string, "LC_CTYPE", source);
    put_copy(string, "LC_COLLATE", source);
    put_copy(string, "LC_MESSAGES", source);
    if (custom->decimal != nullptr || custom->thousands != nullptr || custom->grouping != nullptr) {
        g_string_append(string, "LC_NUMERIC\n");
        put_key(string, "decimal_point",
                custom->decimal != nullptr ? custom->decimal : nl_langinfo_l(RADIXCHAR, loc));
        put_key(string, "thousands_sep",
                custom->thousands != nullptr ? custom->thousands : nl_langinfo_l(THOUSEP, loc));
        char *grouping = custom->grouping != nullptr
                             ? g_strdup(custom->grouping)
                             : xd_intl_grouping_text(nl_langinfo_l(GROUPING, loc));
        g_string_append_printf(string, "grouping %s\nEND LC_NUMERIC\n\n", grouping);
        g_free(grouping);
    } else {
        put_copy(string, "LC_NUMERIC", source);
    }
    if (custom->currency != nullptr || custom->mon_decimal != nullptr ||
        custom->mon_thousands != nullptr || custom->mon_grouping != nullptr ||
        custom->frac_digits >= 0 || custom->positive_format >= 0 || custom->negative_format >= 0)
        put_monetary(string, loc, custom);
    else
        put_copy(string, "LC_MONETARY", source);
    if (custom->time_format != nullptr || custom->am != nullptr || custom->pm != nullptr ||
        custom->date_format != nullptr)
        put_time(string, loc, custom);
    else
        put_copy(string, "LC_TIME", source);
    put_copy(string, "LC_PAPER", source);
    put_copy(string, "LC_NAME", source);
    put_copy(string, "LC_ADDRESS", source);
    put_copy(string, "LC_TELEPHONE", source);
    if (custom->measurement != 0)
        g_string_append_printf(string, "LC_MEASUREMENT\nmeasurement %d\nEND LC_MEASUREMENT\n",
                               custom->measurement);
    else
        put_copy(string, "LC_MEASUREMENT", source);
    freelocale(loc);
    g_free(source);
    g_free(charmap);
    return g_string_free(string, FALSE);
}

/* ---- locale1 --------------------------------------------------------------------- */

const char *xd_intl_lang(const char *const *locale) {
    for (const char *const *variable = locale; variable != nullptr && *variable != nullptr;
         variable++) {
        if (g_str_has_prefix(*variable, "LANG="))
            return *variable + 5;
    }
    return nullptr;
}

char **xd_intl_replace_lang(const char *const *locale, const char *lang) {
    GPtrArray *out = g_ptr_array_new();
    g_ptr_array_add(out, g_strdup_printf("LANG=%s", lang));
    for (const char *const *variable = locale; variable != nullptr && *variable != nullptr;
         variable++) {
        if (!g_str_has_prefix(*variable, "LANG="))
            g_ptr_array_add(out, g_strdup(*variable));
    }
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

char *xd_intl_first(const char *list) {
    const char *comma = list != nullptr ? strchr(list, ',') : nullptr;
    return list == nullptr ? g_strdup("")
           : comma != nullptr ? g_strndup(list, (gsize)(comma - list))
                              : g_strdup(list);
}

char *xd_intl_replace_first(const char *list, const char *value) {
    const char *comma = list != nullptr ? strchr(list, ',') : nullptr;
    return g_strconcat(value != nullptr ? value : "", comma != nullptr ? comma : "", nullptr);
}
