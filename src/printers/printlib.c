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
    size_t count = strlen(reason);
    const char *cursor = reasons;
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ' ')
            cursor++;
        const char *end = strchr(cursor, ',');
        size_t len = end != nullptr ? (size_t)(end - cursor) : strlen(cursor);
        if (len >= count && strncmp(cursor, reason, count) == 0) {
            const char *rest = cursor + count;
            size_t left = len - count;
            if (left == 0 || (left == 6 && strncmp(rest, "-error", 6) == 0) ||
                (left == 8 && strncmp(rest, "-warning", 8) == 0) ||
                (left == 7 && strncmp(rest, "-report", 7) == 0))
                return true;
        }
        cursor += len;
    }
    return false;
}

/* Whether any reason ends in -error (which XP called an error). */
static bool has_error(const char *reasons) {
    if (reasons == nullptr)
        return false;
    for (const char *match = strstr(reasons, "-error"); match != nullptr;
         match = strstr(match + 1, "-error")) {
        if (match[6] == '\0' || match[6] == ',')
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
    size_t unit = 0;
    while (unit + 1 < G_N_ELEMENTS(units) && bytes / div >= 1000) {
        div *= 1024;
        unit++;
    }
    guint64 hundredths = bytes / div * 100 + bytes % div * 100 / div;
    if (hundredths < 1000)
        return g_strdup_printf("%" G_GUINT64_FORMAT ".%02" G_GUINT64_FORMAT " %s", hundredths / 100,
                               hundredths % 100, units[unit]);
    if (hundredths < 10000)
        return g_strdup_printf("%" G_GUINT64_FORMAT ".%" G_GUINT64_FORMAT " %s", hundredths / 100,
                               hundredths / 10 % 10, units[unit]);
    return g_strdup_printf("%" G_GUINT64_FORMAT " %s", hundredths / 100, units[unit]);
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

static bool split_uri(const char *uri, uri_parts *components) {
    if (uri == nullptr)
        return false;
    http_uri_status_t http_uri_status = httpSeparateURI(
        HTTP_URI_CODING_ALL, uri, components->scheme, sizeof(components->scheme), components->user,
        sizeof(components->user), components->host, sizeof(components->host), &components->port,
        components->resource, sizeof(components->resource));
    return http_uri_status >= HTTP_URI_STATUS_OK;
}

static const char *const network_schemes[] = {"ipp", "ipps", "http", "https", "socket",
                                              "lpd", "dnssd", "smb", "beh"};

bool xd_printer_uri_is_network(const char *device_uri) {
    uri_parts components;
    if (!split_uri(device_uri, &components))
        return false;
    for (size_t i = 0; i < G_N_ELEMENTS(network_schemes); i++) {
        if (strcmp(components.scheme, network_schemes[i]) == 0)
            return true;
    }
    return false;
}

bool xd_printer_device_uri_complete(const char *device_uri) {
    uri_parts components;
    if (device_uri == nullptr || strchr(device_uri, ':') == nullptr ||
        !split_uri(device_uri, &components) || components.scheme[0] == '\0')
        return false;
    for (size_t i = 0; i < G_N_ELEMENTS(network_schemes); i++) {
        if (strcmp(components.scheme, network_schemes[i]) == 0)
            return components.host[0] != '\0';
    }
    return components.host[0] != '\0' ||
           (components.resource[0] != '\0' && strcmp(components.resource, "/") != 0);
}

char *xd_printer_device_uri_start(const char *device_uri) {
    if (device_uri == nullptr)
        return g_strdup("");
    if (strchr(device_uri, ':') != nullptr)
        return g_strdup(device_uri);
    return g_strdup_printf("%s://", device_uri);
}

/* "/dev/lp0" to 0, "/dev/ttyS1" to 1: the number at the end of a device. */
static int device_number(const char *path) {
    size_t count = strlen(path);
    while (count > 0 && g_ascii_isdigit(path[count - 1]))
        count--;
    return path[count] != '\0' ? atoi(path + count) : 0;
}

char *xd_printer_port_text(const char *device_uri) {
    if (device_uri == nullptr || device_uri[0] == '\0')
        return g_strdup("");
    uri_parts components;
    if (!split_uri(device_uri, &components))
        return g_strdup(device_uri);
    if (strcmp(components.scheme, "dnssd") == 0) {
        /* "Office Laser._ipp._tcp.local": the service's name. */
        char *dot = strstr(components.host, "._");
        if (dot != nullptr)
            *dot = '\0';
        return g_strdup(components.host);
    }
    if (strcmp(components.scheme, "smb") == 0) {
        /* Windows' own way of writing a shared printer. */
        const char *share =
            components.resource[0] == '/' ? components.resource + 1 : components.resource;
        return g_strdup_printf("\\\\%s\\%s", components.host, share);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(network_schemes); i++) {
        if (strcmp(components.scheme, network_schemes[i]) == 0)
            return g_strdup_printf("IP_%s", components.host);
    }
    if (strcmp(components.scheme, "usb") == 0 || strcmp(components.scheme, "hp") == 0)
        return g_strdup("USB");
    if (strcmp(components.scheme, "parallel") == 0)
        return g_strdup_printf("LPT%d:", device_number(components.resource) + 1);
    if (strcmp(components.scheme, "serial") == 0) {
        char *query = strchr(components.resource, '?');
        if (query != nullptr)
            *query = '\0';
        return g_strdup_printf("COM%d:", device_number(components.resource) + 1);
    }
    if (strcmp(components.scheme, "file") == 0)
        return g_strdup(components.resource);
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
    uri_parts components;
    if (!split_uri(device_uri, &components))
        return "Local Port";
    for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++) {
        if (strcmp(components.scheme, kinds[i][0]) == 0)
            return kinds[i][1];
    }
    return "Local Port";
}

static bool name_char_ok(char character) {
    unsigned char byte = (unsigned char)character;
    return byte > ' ' && byte != 0x7f && strchr("/\\#?'\"", character) == nullptr;
}

bool xd_printer_name_valid(const char *name) {
    size_t count = name != nullptr ? strlen(name) : 0;
    if (count == 0 || count > 127 || !g_utf8_validate(name, -1, nullptr))
        return false;
    for (size_t i = 0; i < count; i++) {
        if (!name_char_ok(name[i]))
            return false;
    }
    return true;
}

char *xd_printer_name_suggest(const char *text) {
    GString *string = g_string_new(nullptr);
    char *valid = g_utf8_make_valid(text != nullptr ? text : "", -1);
    for (const char *cursor = valid; *cursor != '\0'; cursor++) {
        if (name_char_ok(*cursor))
            g_string_append_c(string, *cursor);
        else if (string->len > 0 && string->str[string->len - 1] != '_')
            g_string_append_c(string, '_');
    }
    g_free(valid);
    while (string->len > 0 && string->str[string->len - 1] == '_')
        g_string_truncate(string, string->len - 1);
    if (string->len > 127) {
        /* Not in the middle of a character. */
        const char *end = g_utf8_find_prev_char(string->str, string->str + 128);
        g_string_truncate(string, end != nullptr ? (gsize)(end - string->str) : 127);
    }
    if (string->len == 0)
        g_string_assign(string, "Printer");
    return g_string_free(string, FALSE);
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
    char *label = g_strdup(keyword != nullptr ? keyword : "");
    for (char *cursor = label; *cursor != '\0'; cursor++) {
        if (*cursor == '-' || *cursor == '_')
            *cursor = ' ';
    }
    if (label[0] != '\0')
        label[0] = g_ascii_toupper(label[0]);
    return label;
}
