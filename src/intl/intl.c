/* SPDX-License-Identifier: MIT */
#include <langinfo.h>
#include <locale.h>
#include <monetary.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>

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
    for (const char *p = name; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && strchr("_.@-", *p) == nullptr)
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
        for (char **l = lines; *l != nullptr; l++) {
            char **f = g_strsplit_set(g_strstrip(*l), " \t", 2);
            if (f[0] != nullptr && f[1] != nullptr && f[0][0] != '#' &&
                strcmp(g_strstrip(f[1]), "UTF-8") == 0) {
                locale_t loc = open_locale(f[0]);
                if (loc != (locale_t)0) {
                    /* glibc falls back to a less specific installed
                     * locale; only take ones that really are UTF-8. */
                    if (strcmp(nl_langinfo_l(CODESET, loc), "UTF-8") == 0)
                        g_ptr_array_add(out, g_strdup(f[0]));
                    freelocale(loc);
                }
            }
            g_strfreev(f);
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
    for (const char *p = d_fmt != nullptr ? strchr(d_fmt, '%') : nullptr; p != nullptr;
         p = strchr(p + 1, '%')) {
        const char *c = p + 1;
        while (*c == '-' || *c == '_' || *c == '0' || *c == '^' || *c == '#' || *c == 'E' ||
               *c == 'O')
            c++;
        switch (*c) {
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
            p = c; /* a literal percent sign */
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
    size_t n = strftime_l(buf, sizeof(buf), fmt, tm, loc);
#pragma GCC diagnostic pop
    return g_strdup(n > 0 ? buf : "");
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

void xd_intl_samples_free(xd_intl_samples *s) {
    g_free(s->number);
    g_free(s->currency);
    g_free(s->time);
    g_free(s->short_date);
    g_free(s->long_date);
    *s = (xd_intl_samples){0};
}

/* ---- the formats file ------------------------------------------------------------- */

char *xd_intl_read_formats(const char *path) {
    char *text = nullptr;
    if (!g_file_get_contents(path, &text, nullptr, nullptr))
        return nullptr;
    char *found = nullptr;
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l != nullptr && found == nullptr; l++) {
        char *line = g_strstrip(*l);
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
    GFile *file = g_file_new_for_path(path);
    bool ok;
    if (locale == nullptr) {
        GError *e = nullptr;
        ok = g_file_delete(file, nullptr, &e) || g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
        if (!ok)
            g_propagate_error(error, e);
        else
            g_clear_error(&e);
        g_object_unref(file);
        return ok;
    }
    if (!xd_intl_valid_name(locale)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not a locale: %s", locale);
        g_object_unref(file);
        return false;
    }
    GString *s = g_string_new("# Regional and Language Options: Standards and formats\n");
    for (const char *const *v = xd_intl_format_vars; *v != nullptr; v++)
        g_string_append_printf(s, "%s=%s\n", *v, locale);
    char *dir = g_path_get_dirname(path);
    ok = g_mkdir_with_parents(dir, 0755) == 0;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create %s", dir);
    else
        ok = g_file_replace_contents(file, s->str, s->len, nullptr, FALSE,
                                     G_FILE_CREATE_REPLACE_DESTINATION, nullptr, nullptr, error);
    g_free(dir);
    g_string_free(s, TRUE);
    g_object_unref(file);
    return ok;
}

/* ---- locale1 --------------------------------------------------------------------- */

const char *xd_intl_lang(const char *const *locale) {
    for (const char *const *v = locale; v != nullptr && *v != nullptr; v++) {
        if (g_str_has_prefix(*v, "LANG="))
            return *v + 5;
    }
    return nullptr;
}

char **xd_intl_replace_lang(const char *const *locale, const char *lang) {
    GPtrArray *out = g_ptr_array_new();
    g_ptr_array_add(out, g_strdup_printf("LANG=%s", lang));
    for (const char *const *v = locale; v != nullptr && *v != nullptr; v++) {
        if (!g_str_has_prefix(*v, "LANG="))
            g_ptr_array_add(out, g_strdup(*v));
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
