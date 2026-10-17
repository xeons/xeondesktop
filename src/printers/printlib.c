/* SPDX-License-Identifier: MIT */
#include <cups/cups.h>
#include <stdlib.h>
#include <string.h>

#include "printlib.h"

/* IPP's printer-state and job-state values (RFC 8011). */
enum { P_IDLE = 3, P_PROCESSING = 4, P_STOPPED = 5 };
enum { J_PENDING = 3, J_HELD, J_PROCESSING, J_STOPPED, J_CANCELED, J_ABORTED, J_COMPLETED };

/* Whether the comma-separated `reasons` has `reason`, with or without
 * the -error, -warning or -report suffix IPP allows. */
static bool has_reason(const char *reasons, const char *reason) {
    if (reasons == nullptr)
        return false;
    size_t n = strlen(reason);
    const char *p = reasons;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ')
            p++;
        const char *end = strchr(p, ',');
        size_t len = end != nullptr ? (size_t)(end - p) : strlen(p);
        if (len >= n && strncmp(p, reason, n) == 0) {
            const char *rest = p + n;
            size_t left = len - n;
            if (left == 0 || (left == 6 && strncmp(rest, "-error", 6) == 0) ||
                (left == 8 && strncmp(rest, "-warning", 8) == 0) ||
                (left == 7 && strncmp(rest, "-report", 7) == 0))
                return true;
        }
        p += len;
    }
    return false;
}

/* Whether any reason ends in -error (which XP called an error). */
static bool has_error(const char *reasons) {
    if (reasons == nullptr)
        return false;
    for (const char *p = strstr(reasons, "-error"); p != nullptr; p = strstr(p + 1, "-error")) {
        if (p[6] == '\0' || p[6] == ',')
            return true;
    }
    return false;
}

bool xd_printer_is_paused(int state, const char *reasons) {
    return state == P_STOPPED && has_reason(reasons, "paused");
}

const char *xd_printer_status_text(int state, const char *reasons, bool accepting) {
    if (xd_printer_is_paused(state, reasons))
        return "Paused";
    /* The printer's own troubles, worst first. */
    static const char *const problems[][2] = {
        {"offline", "Offline"},
        {"media-jam", "Paper Jam"},
        {"media-empty", "Paper Out"},
        {"media-needed", "Paper Out"},
        {"door-open", "Door Open"},
        {"cover-open", "Door Open"},
        {"toner-empty", "Out of Toner/Ink"},
        {"marker-supply-empty", "Out of Toner/Ink"},
        {"output-area-full", "Output Bin Full"},
        {"connecting-to-device", "Initializing"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(problems); i++) {
        if (has_reason(reasons, problems[i][0]))
            return problems[i][1];
    }
    if (state == P_STOPPED || has_error(reasons))
        return "Error";
    if (state == P_PROCESSING)
        return "Printing";
    if (!accepting)
        return "Not Accepting Documents";
    if (has_reason(reasons, "toner-low") || has_reason(reasons, "marker-supply-low"))
        return "Toner/Ink Low";
    return "Ready";
}

const char *xd_print_job_status_text(int state) {
    switch (state) {
    case J_PENDING:
        return "";
    case J_HELD:
        return "Paused";
    case J_PROCESSING:
        return "Printing";
    case J_STOPPED:
    case J_ABORTED:
        return "Error";
    case J_CANCELED:
        return "Deleted";
    case J_COMPLETED:
        return "Printed";
    default:
        return "";
    }
}

char *xd_print_size_text(guint64 bytes) {
    if (bytes < 1000)
        return g_strdup_printf("%" G_GUINT64_FORMAT " bytes", bytes);
    static const char *const units[] = {"KB", "MB", "GB", "TB"};
    /* Hundredths of the unit, rounded down; the next unit from 1000 on,
     * so there are never more than three digits. */
    guint64 div = 1024;
    size_t u = 0;
    while (u + 1 < G_N_ELEMENTS(units) && bytes / div >= 1000) {
        div *= 1024;
        u++;
    }
    guint64 h = bytes / div * 100 + bytes % div * 100 / div;
    if (h < 1000)
        return g_strdup_printf("%" G_GUINT64_FORMAT ".%02" G_GUINT64_FORMAT " %s", h / 100,
                               h % 100, units[u]);
    if (h < 10000)
        return g_strdup_printf("%" G_GUINT64_FORMAT ".%" G_GUINT64_FORMAT " %s", h / 100,
                               h / 10 % 10, units[u]);
    return g_strdup_printf("%" G_GUINT64_FORMAT " %s", h / 100, units[u]);
}

char *xd_print_pages_text(int done, int total, bool printing) {
    if (total <= 0)
        return printing && done > 0 ? g_strdup_printf("%d", done) : g_strdup("N/A");
    if (printing && done > 0 && done < total)
        return g_strdup_printf("%d/%d", done, total);
    return g_strdup_printf("%d", total);
}

/* The URI's parts, decoded, through libcups. */
typedef struct uri_parts {
    char scheme[32], user[256], host[256], resource[1024];
    int port;
} uri_parts;

static bool split_uri(const char *uri, uri_parts *u) {
    if (uri == nullptr)
        return false;
    http_uri_status_t s = httpSeparateURI(HTTP_URI_CODING_ALL, uri, u->scheme, sizeof(u->scheme),
                                          u->user, sizeof(u->user), u->host, sizeof(u->host),
                                          &u->port, u->resource, sizeof(u->resource));
    return s >= HTTP_URI_STATUS_OK;
}

static const char *const network_schemes[] = {"ipp", "ipps", "http", "https", "socket",
                                              "lpd", "dnssd", "smb", "beh"};

bool xd_printer_uri_is_network(const char *device_uri) {
    uri_parts u;
    if (!split_uri(device_uri, &u))
        return false;
    for (size_t i = 0; i < G_N_ELEMENTS(network_schemes); i++) {
        if (strcmp(u.scheme, network_schemes[i]) == 0)
            return true;
    }
    return false;
}

/* "/dev/lp0" to 0, "/dev/ttyS1" to 1: the number at the end of a device. */
static int device_number(const char *path) {
    size_t n = strlen(path);
    while (n > 0 && g_ascii_isdigit(path[n - 1]))
        n--;
    return path[n] != '\0' ? atoi(path + n) : 0;
}

char *xd_printer_port_text(const char *device_uri) {
    if (device_uri == nullptr || device_uri[0] == '\0')
        return g_strdup("");
    uri_parts u;
    if (!split_uri(device_uri, &u))
        return g_strdup(device_uri);
    if (strcmp(u.scheme, "dnssd") == 0) {
        /* "Office Laser._ipp._tcp.local": the service's name. */
        char *dot = strstr(u.host, "._");
        if (dot != nullptr)
            *dot = '\0';
        return g_strdup(u.host);
    }
    if (strcmp(u.scheme, "smb") == 0) {
        /* Windows' own way of writing a shared printer. */
        const char *share = u.resource[0] == '/' ? u.resource + 1 : u.resource;
        return g_strdup_printf("\\\\%s\\%s", u.host, share);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(network_schemes); i++) {
        if (strcmp(u.scheme, network_schemes[i]) == 0)
            return g_strdup_printf("IP_%s", u.host);
    }
    if (strcmp(u.scheme, "usb") == 0 || strcmp(u.scheme, "hp") == 0)
        return g_strdup("USB");
    if (strcmp(u.scheme, "parallel") == 0)
        return g_strdup_printf("LPT%d:", device_number(u.resource) + 1);
    if (strcmp(u.scheme, "serial") == 0) {
        char *q = strchr(u.resource, '?');
        if (q != nullptr)
            *q = '\0';
        return g_strdup_printf("COM%d:", device_number(u.resource) + 1);
    }
    if (strcmp(u.scheme, "file") == 0)
        return g_strdup(u.resource);
    return g_strdup(device_uri);
}

const char *xd_printer_port_description(const char *device_uri) {
    static const char *const kinds[][2] = {
        {"socket", "Standard TCP/IP Port"},
        {"ipp", "Internet Printing (IPP)"},
        {"ipps", "Internet Printing (IPP over TLS)"},
        {"http", "Internet Printing (IPP)"},
        {"https", "Internet Printing (IPP over TLS)"},
        {"lpd", "LPR Port"},
        {"dnssd", "Network Printer (DNS-SD)"},
        {"smb", "Windows Printer Share"},
        {"usb", "USB Port"},
        {"hp", "USB Port (HPLIP)"},
        {"parallel", "Printer Port"},
        {"serial", "Serial Port"},
        {"file", "Print to File"},
    };
    uri_parts u;
    if (!split_uri(device_uri, &u))
        return "Local Port";
    for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++) {
        if (strcmp(u.scheme, kinds[i][0]) == 0)
            return kinds[i][1];
    }
    return "Local Port";
}

static bool name_char_ok(char c) {
    unsigned char b = (unsigned char)c;
    return b > ' ' && b != 0x7f && strchr("/\\#?'\"", c) == nullptr;
}

bool xd_printer_name_valid(const char *name) {
    size_t n = name != nullptr ? strlen(name) : 0;
    if (n == 0 || n > 127 || !g_utf8_validate(name, -1, nullptr))
        return false;
    for (size_t i = 0; i < n; i++) {
        if (!name_char_ok(name[i]))
            return false;
    }
    return true;
}

char *xd_printer_name_suggest(const char *text) {
    GString *s = g_string_new(nullptr);
    char *valid = g_utf8_make_valid(text != nullptr ? text : "", -1);
    for (const char *p = valid; *p != '\0'; p++) {
        if (name_char_ok(*p))
            g_string_append_c(s, *p);
        else if (s->len > 0 && s->str[s->len - 1] != '_')
            g_string_append_c(s, '_');
    }
    g_free(valid);
    while (s->len > 0 && s->str[s->len - 1] == '_')
        g_string_truncate(s, s->len - 1);
    if (s->len > 127) {
        /* Not in the middle of a character. */
        const char *end = g_utf8_find_prev_char(s->str, s->str + 128);
        g_string_truncate(s, end != nullptr ? (gsize)(end - s->str) : 127);
    }
    if (s->len == 0)
        g_string_assign(s, "Printer");
    return g_string_free(s, FALSE);
}

const char *xd_print_orientation_label(int orientation_requested) {
    switch (orientation_requested) {
    case 3:
        return "Portrait";
    case 4:
        return "Landscape";
    case 5:
        return "Rotated Landscape";
    case 6:
        return "Rotated Portrait";
    default:
        return nullptr;
    }
}

const char *xd_print_sides_label(const char *sides) {
    if (sides == nullptr)
        return nullptr;
    if (strcmp(sides, "one-sided") == 0)
        return "None";
    if (strcmp(sides, "two-sided-long-edge") == 0)
        return "Flip on Long Edge";
    if (strcmp(sides, "two-sided-short-edge") == 0)
        return "Flip on Short Edge";
    return nullptr;
}

const char *xd_print_color_label(const char *print_color_mode) {
    if (print_color_mode == nullptr)
        return nullptr;
    if (strcmp(print_color_mode, "color") == 0)
        return "Color";
    if (strcmp(print_color_mode, "monochrome") == 0)
        return "Black & White";
    if (strcmp(print_color_mode, "auto") == 0)
        return "Automatic";
    return nullptr;
}

const char *xd_print_quality_label(int print_quality) {
    switch (print_quality) {
    case 3:
        return "Draft";
    case 4:
        return "Normal";
    case 5:
        return "Best";
    default:
        return nullptr;
    }
}

char *xd_print_keyword_label(const char *keyword) {
    char *s = g_strdup(keyword != nullptr ? keyword : "");
    for (char *c = s; *c != '\0'; c++) {
        if (*c == '-' || *c == '_')
            *c = ' ';
    }
    if (s[0] != '\0')
        s[0] = g_ascii_toupper(s[0]);
    return s;
}
