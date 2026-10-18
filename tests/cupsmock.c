/* SPDX-License-Identifier: MIT
 * xd-cupsmock: a stand-in for cupsd, for the UI tests of Printers and
 * Faxes. It answers on a Unix socket of its own (CUPS_SERVER=SOCKET for
 * the program under test) with two printers (which can be paused), a few
 * devices for CUPS-Get-Devices and a cupsd.conf, and writes each change it
 * is asked for to LOG, one line per attribute:
 *
 *     xd-cupsmock SOCKET LOG [--auth]
 *
 * With --auth, requests to /admin (changes, cupsd.conf) want a password,
 * as cupsd's default policy does; any password will do.
 *
 * Requests are read and answered with libcups's own server side (the
 * calls ippeveprinter uses); nothing here touches a real scheduler or
 * printer. Without root, cupsd itself can't be started in a test.
 */
#include <cups/cups.h>
#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct mock_printer {
    char *name, *uri, *info, *location, *model;
    bool shared, paused;
} mock_printer;

static GMutex lock;
static GPtrArray *printers;
static bool sharing;
static char *conf;
static FILE *log_file;
static bool require_auth;

static const char INITIAL_CONF[] = "LogLevel warn\n"
                                   "Listen /run/cups/cups.sock\n"
                                   "Browsing Off\n"
                                   "BrowseLocalProtocols none\n"
                                   "DefaultAuthType Basic\n"
                                   "WebInterface Yes\n"
                                   "<Location />\n"
                                   "  Order allow,deny\n"
                                   "</Location>\n"
                                   "<Location /admin>\n"
                                   "  Order allow,deny\n"
                                   "</Location>\n";

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
}

static mock_printer *add_printer(const char *name, const char *uri, const char *info,
                                 const char *location, const char *model) {
    mock_printer *p = g_new0(mock_printer, 1);
    p->name = g_strdup(name);
    p->uri = g_strdup(uri);
    p->info = g_strdup(info);
    p->location = g_strdup(location);
    p->model = g_strdup(model);
    g_ptr_array_add(printers, p);
    return p;
}

static mock_printer *find_printer(ipp_t *req) {
    ipp_attribute_t *a = ippFindAttribute(req, "printer-uri", IPP_TAG_URI);
    const char *uri = a != nullptr ? ippGetString(a, 0, nullptr) : nullptr;
    const char *at = uri != nullptr ? strstr(uri, "/printers/") : nullptr;
    if (at == nullptr)
        return nullptr;
    at += strlen("/printers/");
    for (guint i = 0; i < printers->len; i++) {
        mock_printer *p = g_ptr_array_index(printers, i);
        if (strcmp(p->name, at) == 0)
            return p;
    }
    return nullptr;
}

static void describe(ipp_t *resp, const mock_printer *p) {
    char uri[HTTP_MAX_URI];
    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, (int)sizeof(uri), "ipp", nullptr, "localhost",
                     631, "/printers/%s", p->name);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", nullptr, p->name);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uri-supported", nullptr, uri);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_URI, "device-uri", nullptr, p->uri);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", nullptr, p->info);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-location", nullptr, p->location);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-make-and-model", nullptr,
                 p->model);
    ippAddInteger(resp, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state",
                  p->paused ? IPP_PSTATE_STOPPED : IPP_PSTATE_IDLE);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-state-reasons", nullptr,
                 p->paused ? "paused" : "none");
    ippAddInteger(resp, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-type", CUPS_PRINTER_LOCAL);
    ippAddBoolean(resp, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
    ippAddBoolean(resp, IPP_TAG_PRINTER, "printer-is-shared", (char)p->shared);
    ippAddBoolean(resp, IPP_TAG_PRINTER, "server-is-sharing-printers", (char)sharing);
    static const char *const media[] = {"iso_a4_210x297mm", "na_letter_8.5x11in"};
    ippAddStrings(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-supported", 2, nullptr, media);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-default", nullptr, media[0]);
    ippAddRange(resp, IPP_TAG_PRINTER, "copies-supported", 1, 99);
    ippAddResolution(resp, IPP_TAG_PRINTER, "printer-resolution-supported", IPP_RES_PER_INCH,
                     600, 600);
}

static void get_devices(ipp_t *resp) {
    static const char *const devices[][4] = {
        /* class, uri, info, make and model */
        {"direct", "usb://Example/Inkjet%20100?serial=42", "Example Inkjet 100 (USB)",
         "Example Inkjet 100"},
        {"network", "socket://192.168.1.30:9100", "Example Laser 300", "Example Laser 300"},
        {"network", "socket", "AppSocket/HP JetDirect", "Unknown"},
        {"network", "ipp", "Internet Printing Protocol (ipp)", "Unknown"},
        {"network", "lpd", "LPD/LPR Host or Printer", "Unknown"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(devices); i++) {
        if (i > 0)
            ippAddSeparator(resp);
        ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "device-class", nullptr,
                     devices[i][0]);
        ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "device-info", nullptr, devices[i][2]);
        ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "device-make-and-model", nullptr,
                     devices[i][3]);
        ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_URI, "device-uri", nullptr, devices[i][1]);
        ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "device-id", nullptr, "");
        ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "device-location", nullptr, "");
    }
}

static void add_modify(ipp_t *req, ipp_t *resp) {
    mock_printer *p = find_printer(req);
    if (p == nullptr) {
        ippSetStatusCode(resp, IPP_STATUS_ERROR_NOT_FOUND);
        return;
    }
    for (ipp_attribute_t *a = ippFirstAttribute(req); a != nullptr; a = ippNextAttribute(req)) {
        const char *n = ippGetName(a);
        if (n == nullptr || ippGetGroupTag(a) != IPP_TAG_PRINTER)
            continue;
        char value[1024];
        ippAttributeString(a, value, sizeof(value));
        log_line("add-modify %s %s=%s", p->name, n, value);
        char **field = strcmp(n, "device-uri") == 0         ? &p->uri
                       : strcmp(n, "printer-info") == 0     ? &p->info
                       : strcmp(n, "printer-location") == 0 ? &p->location
                                                            : nullptr;
        if (field != nullptr) {
            g_free(*field);
            *field = g_strdup(ippGetString(a, 0, nullptr));
        } else if (strcmp(n, "printer-is-shared") == 0) {
            p->shared = ippGetBoolean(a, 0) != 0;
        }
    }
}

static ipp_t *process(ipp_t *req) {
    ipp_t *resp = ippNewResponse(req);
    g_mutex_lock(&lock);
    ipp_op_t op = ippGetOperation(req);
    if (op == IPP_OP_CUPS_GET_PRINTERS) {
        for (guint i = 0; i < printers->len; i++) {
            if (i > 0)
                ippAddSeparator(resp);
            describe(resp, g_ptr_array_index(printers, i));
        }
    } else if (op == IPP_OP_GET_PRINTER_ATTRIBUTES) {
        const mock_printer *p = find_printer(req);
        if (p != nullptr)
            describe(resp, p);
        else
            ippSetStatusCode(resp, IPP_STATUS_ERROR_NOT_FOUND);
    } else if (op == IPP_OP_CUPS_GET_DEFAULT) {
        ippSetStatusCode(resp, IPP_STATUS_ERROR_NOT_FOUND);
    } else if (op == IPP_OP_GET_JOBS) {
        /* No documents. */
    } else if (op == IPP_OP_CUPS_GET_DEVICES) {
        log_line("get-devices");
        get_devices(resp);
    } else if (op == IPP_OP_CUPS_ADD_MODIFY_PRINTER) {
        add_modify(req, resp);
    } else if (op == IPP_OP_PAUSE_PRINTER || op == IPP_OP_RESUME_PRINTER) {
        mock_printer *p = find_printer(req);
        if (p != nullptr) {
            p->paused = op == IPP_OP_PAUSE_PRINTER;
            log_line("%s %s", p->paused ? "pause" : "resume", p->name);
        } else {
            ippSetStatusCode(resp, IPP_STATUS_ERROR_NOT_FOUND);
        }
    } else {
        ippSetStatusCode(resp, IPP_STATUS_ERROR_OPERATION_NOT_SUPPORTED);
    }
    g_mutex_unlock(&lock);
    return resp;
}

static bool respond(http_t *http, http_status_t code, const char *type, const char *body,
                    size_t length, ipp_t *ipp) {
    httpClearFields(http);
    if (type != nullptr)
        httpSetField(http, HTTP_FIELD_CONTENT_TYPE, type);
    httpSetLength(http, ipp != nullptr ? ippLength(ipp) : length);
    if (httpWriteResponse(http, code) < 0)
        return false;
    if (ipp != nullptr) {
        ippSetState(ipp, IPP_STATE_IDLE);
        return ippWrite(http, ipp) == IPP_STATE_DATA;
    }
    if (length > 0 && httpWrite2(http, body, length) < 0)
        return false;
    return httpFlushWrite(http) >= 0;
}

/* One request; false when the connection is done. */
static bool serve_one(http_t *http) {
    if (!httpWait(http, 60000))
        return false;
    char resource[1024];
    http_state_t state = httpReadRequest(http, resource, sizeof(resource));
    if (state == HTTP_STATE_WAITING)
        return true;
    if (state == HTTP_STATE_ERROR || state == HTTP_STATE_UNKNOWN_METHOD ||
        state == HTTP_STATE_UNKNOWN_VERSION)
        return false;
    http_status_t status;
    while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE)
        ;
    if (status != HTTP_STATUS_OK)
        return false;
    /* With --auth, /admin wants a password (any will do), as cupsd's
     * default policy wants a printer administrator's. */
    const char *auth = httpGetField(http, HTTP_FIELD_AUTHORIZATION);
    bool authorized = auth != nullptr && auth[0] != '\0';
    if (require_auth && g_str_has_prefix(resource, "/admin")) {
        if (!authorized) {
            log_line("password asked for %s", resource);
            httpClearFields(http);
            httpSetField(http, HTTP_FIELD_WWW_AUTHENTICATE, "Basic realm=\"CUPS\"");
            httpSetLength(http, 0);
            httpWriteResponse(http, HTTP_STATUS_UNAUTHORIZED);
            return false; /* a body that may follow isn't read: start afresh */
        }
        /* "Basic " and base64 of user:password: who, and whether a
         * password came. */
        gsize n = 0;
        guchar *raw = g_str_has_prefix(auth, "Basic ") ? g_base64_decode(auth + 6, &n) : nullptr;
        char *pair = g_strndup((const char *)raw, n);
        char *colon = strchr(pair, ':');
        if (colon != nullptr)
            *colon = '\0';
        log_line("authorized for %s as %s with%s a password", resource, pair,
                 colon != nullptr && colon[1] != '\0' ? "" : "out");
        g_free(pair);
        g_free(raw);
    }
    if ((state == HTTP_STATE_POST || state == HTTP_STATE_PUT) &&
        httpGetExpect(http) == HTTP_STATUS_CONTINUE && httpWriteResponse(http, HTTP_STATUS_CONTINUE))
        return false;
    if (state == HTTP_STATE_POST) {
        ipp_t *req = ippNew();
        ipp_state_t s;
        while ((s = ippRead(http, req)) != IPP_STATE_DATA) {
            if (s == IPP_STATE_ERROR) {
                ippDelete(req);
                return false;
            }
        }
        ipp_t *resp = process(req);
        bool ok = respond(http, HTTP_STATUS_OK, "application/ipp", nullptr, 0, resp);
        ippDelete(req);
        ippDelete(resp);
        return ok;
    }
    if (state == HTTP_STATE_GET && strcmp(resource, "/admin/conf/cupsd.conf") == 0) {
        g_mutex_lock(&lock);
        char *text = g_strdup(conf);
        g_mutex_unlock(&lock);
        bool ok = respond(http, HTTP_STATUS_OK, "text/plain", text, strlen(text), nullptr);
        g_free(text);
        return ok;
    }
    if (state == HTTP_STATE_PUT && strcmp(resource, "/admin/conf/cupsd.conf") == 0) {
        GString *body = g_string_new(nullptr);
        char buf[4096];
        ssize_t n;
        while ((n = httpRead2(http, buf, sizeof(buf))) > 0)
            g_string_append_len(body, buf, n);
        g_mutex_lock(&lock);
        g_free(conf);
        conf = g_string_free(body, FALSE);
        /* What cupsAdminSetServerSettings writes for _share_printers. */
        sharing = strstr(conf, "\nBrowsing On") != nullptr ||
                  strstr(conf, "\nBrowsing Yes") != nullptr;
        gchar **lines = g_strsplit(conf, "\n", -1);
        for (gchar **l = lines; *l != nullptr; l++) {
            if (g_str_has_prefix(*l, "Brows") || g_str_has_prefix(*l, "Port") ||
                g_str_has_prefix(*l, "Listen"))
                log_line("cupsd.conf %s", *l);
        }
        g_strfreev(lines);
        g_mutex_unlock(&lock);
        return respond(http, HTTP_STATUS_CREATED, nullptr, nullptr, 0, nullptr);
    }
    respond(http, HTTP_STATUS_NOT_FOUND, "text/plain", "", 0, nullptr);
    return false;
}

static gpointer serve(gpointer data) {
    http_t *http = data;
    while (serve_one(http))
        ;
    httpClose(http);
    return nullptr;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4 || (argc == 4 && strcmp(argv[3], "--auth") != 0)) {
        fprintf(stderr, "usage: xd-cupsmock SOCKET LOG [--auth]\n");
        return 2;
    }
    require_auth = argc == 4;
    log_file = fopen(argv[2], "a");
    if (log_file == nullptr) {
        perror(argv[2]);
        return 1;
    }
    printers = g_ptr_array_new();
    add_printer("Office_Laser", "socket://192.168.1.20:9100", "Office Laser", "Second floor",
                "Example Laser 200");
    add_printer("Home_Inkjet", "usb://Example/Inkjet%20100?serial=42", "", "",
                "Example Inkjet 100");
    conf = g_strdup(INITIAL_CONF);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (fd < 0 || strlen(argv[1]) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "xd-cupsmock: bad socket %s\n", argv[1]);
        return 1;
    }
    g_strlcpy(addr.sun_path, argv[1], sizeof(addr.sun_path));
    unlink(argv[1]);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        perror(argv[1]);
        return 1;
    }
    for (;;) {
        http_t *http = httpAcceptConnection(fd, 1);
        if (http != nullptr)
            g_thread_unref(g_thread_new("client", serve, http));
    }
}
