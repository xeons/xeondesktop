/* SPDX-License-Identifier: MIT */
#include <cups/cups.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cupsops.h"

#ifndef XD_CUPS_DATADIR
#define XD_CUPS_DATADIR "/usr/share/cups"
#endif

G_DEFINE_QUARK(xd-cups-error-quark, xd_cups_error)

/* Listing runs in worker threads, where nobody can answer a password
 * box: libcups would otherwise ask on the terminal. Its callback is per
 * thread, so each thread sets this one before its first request. */
static const char *no_password([[maybe_unused]] const char *prompt,
                               [[maybe_unused]] http_t *http,
                               [[maybe_unused]] const char *method,
                               [[maybe_unused]] const char *resource,
                               [[maybe_unused]] void *data) {
    return nullptr;
}

static void thread_init(void) {
    static thread_local bool done = false;
    /* The main thread (whose id is the process's) has the real box. */
    if (done || gettid() == getpid())
        return;
    done = true;
    cupsSetPasswordCB2(no_password, nullptr);
}

/* Fails with libcups's last error, if there is one. */
static bool check(GError **error, const char *what) {
    ipp_status_t st = cupsLastError();
    if (st <= IPP_STATUS_OK_EVENTS_COMPLETE)
        return true;
    int code = (int)st;
    const char *why = cupsLastErrorString();
    if (st == IPP_STATUS_ERROR_CUPS_AUTHENTICATION_CANCELED)
        g_set_error_literal(error, XD_CUPS_ERROR, XD_CUPS_ERROR_CANCELED, why);
    else if (st == IPP_STATUS_ERROR_NOT_AUTHORIZED || st == IPP_STATUS_ERROR_FORBIDDEN ||
             st == IPP_STATUS_ERROR_NOT_AUTHENTICATED)
        g_set_error(error, XD_CUPS_ERROR, code,
                    "%s: access denied. Only a printer administrator can do this (%s).", what,
                    why);
    else if (st == IPP_STATUS_ERROR_SERVICE_UNAVAILABLE || st == IPP_STATUS_ERROR_INTERNAL)
        g_set_error(error, XD_CUPS_ERROR, code,
                    "%s: the print spooler (CUPS) isn't running or can't be reached (%s).", what,
                    why);
    else
        g_set_error(error, XD_CUPS_ERROR, code, "%s: %s", what, why);
    return false;
}

static char *dup_option(const char *name, cups_dest_t *d) {
    const char *v = cupsGetOption(name, d->num_options, d->options);
    return g_strdup(v != nullptr ? v : "");
}

static int int_option(const char *name, cups_dest_t *d, int fallback) {
    const char *v = cupsGetOption(name, d->num_options, d->options);
    if (v == nullptr)
        return fallback;
    long n = strtol(v, nullptr, 10);
    return (int)n;
}

static bool bool_option(const char *name, cups_dest_t *d, bool fallback) {
    const char *v = cupsGetOption(name, d->num_options, d->options);
    if (v == nullptr)
        return fallback;
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0;
}

static int count_jobs(const char *name) {
    cups_job_t *jobs = nullptr;
    int n = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, name, 0, CUPS_WHICHJOBS_ACTIVE);
    cupsFreeJobs(n, jobs);
    return n > 0 ? n : 0;
}

static xd_printer *from_dest(cups_dest_t *d) {
    xd_printer *p = g_new0(xd_printer, 1);
    p->name = g_strdup(d->name);
    p->info = dup_option("printer-info", d);
    p->location = dup_option("printer-location", d);
    p->model = dup_option("printer-make-and-model", d);
    p->device_uri = dup_option("device-uri", d);
    p->reasons = dup_option("printer-state-reasons", d);
    p->message = dup_option("printer-state-message", d);
    p->state = int_option("printer-state", d, IPP_PSTATE_IDLE);
    p->accepting = bool_option("printer-is-accepting-jobs", d, true);
    p->shared = bool_option("printer-is-shared", d, false);
    unsigned type = (unsigned)int_option("printer-type", d, 0);
    p->fax = (type & CUPS_PRINTER_FAX) != 0;
    p->is_default = d->is_default != 0;
    p->jobs = count_jobs(d->name);
    return p;
}

void xd_printer_free(xd_printer *p) {
    if (p == nullptr)
        return;
    g_free(p->name);
    g_free(p->info);
    g_free(p->location);
    g_free(p->model);
    g_free(p->device_uri);
    g_free(p->reasons);
    g_free(p->message);
    g_free(p);
}

static int by_name(gconstpointer a, gconstpointer b) {
    const xd_printer *x = *(xd_printer *const *)a;
    const xd_printer *y = *(xd_printer *const *)b;
    return g_utf8_collate(x->name, y->name);
}

GPtrArray *xd_printers_list(GError **error) {
    thread_init();
    cups_dest_t *dests = nullptr;
    int n = cupsGetDests2(CUPS_HTTP_DEFAULT, &dests);
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)xd_printer_free);
    /* No printers is no error; a scheduler that can't be reached is. */
    if (n == 0 && cupsLastError() > IPP_STATUS_OK_EVENTS_COMPLETE &&
        cupsLastError() != IPP_STATUS_ERROR_NOT_FOUND) {
        check(error, "The printers could not be listed");
        g_ptr_array_unref(list);
        return nullptr;
    }
    for (int i = 0; i < n; i++) {
        /* Instances (lpoptions' NAME/INSTANCE) are the same printer. */
        if (dests[i].instance == nullptr)
            g_ptr_array_add(list, from_dest(&dests[i]));
    }
    cupsFreeDests(n, dests);
    g_ptr_array_sort(list, by_name);
    return list;
}

xd_printer *xd_printer_get(const char *name, GError **error) {
    thread_init();
    cups_dest_t *dests = nullptr;
    int n = cupsGetDests2(CUPS_HTTP_DEFAULT, &dests);
    cups_dest_t *d = cupsGetDest(name, nullptr, n, dests);
    xd_printer *p = d != nullptr ? from_dest(d) : nullptr;
    cupsFreeDests(n, dests);
    if (p == nullptr)
        g_set_error(error, XD_CUPS_ERROR, IPP_STATUS_ERROR_NOT_FOUND,
                    "The printer %s no longer exists.", name);
    return p;
}

/* ipp://localhost/printers/NAME: how the scheduler names its queues. */
static void printer_uri(const char *name, char *uri, size_t size) {
    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, (int)size, "ipp", nullptr, "localhost", ippPort(),
                     "/printers/%s", name);
}

static ipp_t *printer_request(ipp_op_t op, const char *name) {
    ipp_t *req = ippNewRequest(op);
    char uri[HTTP_MAX_URI];
    printer_uri(name, uri, sizeof(uri));
    ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", nullptr, uri);
    ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", nullptr,
                 cupsUser());
    return req;
}

/* Sends `req` (and frees it); fails with what went wrong. */
static bool send_request(ipp_t *req, const char *resource, GError **error, const char *what) {
    ipp_t *resp = cupsDoRequest(CUPS_HTTP_DEFAULT, req, resource);
    ippDelete(resp);
    return check(error, what);
}

static void free_job(xd_print_job *j) {
    g_free(j->name);
    g_free(j->owner);
    g_free(j);
}

GPtrArray *xd_print_jobs_list(const char *printer, GError **error) {
    thread_init();
    static const char *const wanted[] = {
        "job-id",           "job-name",         "job-originating-user-name",
        "job-state",        "job-k-octets",     "job-impressions",
        "job-impressions-completed", "job-media-sheets", "job-media-sheets-completed",
        "time-at-creation",
    };
    ipp_t *req = printer_request(IPP_OP_GET_JOBS, printer);
    ippAddStrings(req, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes",
                  (int)G_N_ELEMENTS(wanted), nullptr, wanted);
    ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs", nullptr, "not-completed");
    ipp_t *resp = cupsDoRequest(CUPS_HTTP_DEFAULT, req, "/");
    if (resp == nullptr || !check(error, "The documents could not be listed")) {
        ippDelete(resp);
        return nullptr;
    }
    GPtrArray *jobs = g_ptr_array_new_with_free_func((GDestroyNotify)free_job);
    ipp_attribute_t *a = ippFirstAttribute(resp);
    while (a != nullptr) {
        while (a != nullptr && ippGetGroupTag(a) != IPP_TAG_JOB)
            a = ippNextAttribute(resp);
        if (a == nullptr)
            break;
        xd_print_job *j = g_new0(xd_print_job, 1);
        int sheets = 0, sheets_done = 0;
        for (; a != nullptr && ippGetGroupTag(a) == IPP_TAG_JOB; a = ippNextAttribute(resp)) {
            const char *n = ippGetName(a);
            if (n == nullptr)
                continue;
            if (strcmp(n, "job-id") == 0)
                j->id = ippGetInteger(a, 0);
            else if (strcmp(n, "job-name") == 0)
                j->name = g_strdup(ippGetString(a, 0, nullptr));
            else if (strcmp(n, "job-originating-user-name") == 0)
                j->owner = g_strdup(ippGetString(a, 0, nullptr));
            else if (strcmp(n, "job-state") == 0)
                j->state = ippGetInteger(a, 0);
            else if (strcmp(n, "job-k-octets") == 0)
                j->size = (guint64)ippGetInteger(a, 0) * 1024;
            else if (strcmp(n, "job-impressions") == 0)
                j->pages = ippGetInteger(a, 0);
            else if (strcmp(n, "job-impressions-completed") == 0)
                j->pages_done = ippGetInteger(a, 0);
            else if (strcmp(n, "job-media-sheets") == 0)
                sheets = ippGetInteger(a, 0);
            else if (strcmp(n, "job-media-sheets-completed") == 0)
                sheets_done = ippGetInteger(a, 0);
            else if (strcmp(n, "time-at-creation") == 0)
                j->created = ippGetInteger(a, 0);
        }
        if (j->pages <= 0) {
            j->pages = sheets;
            j->pages_done = sheets_done;
        }
        if (j->name == nullptr)
            j->name = g_strdup("");
        if (j->owner == nullptr)
            j->owner = g_strdup("");
        g_ptr_array_add(jobs, j);
    }
    ippDelete(resp);
    return jobs;
}

bool xd_printer_set_default(const char *name, GError **error) {
    cups_dest_t *dests = nullptr;
    int n = cupsGetDests2(CUPS_HTTP_DEFAULT, &dests);
    bool found = false;
    for (int i = 0; i < n; i++) {
        bool it = strcmp(dests[i].name, name) == 0 && dests[i].instance == nullptr;
        dests[i].is_default = it;
        found = found || it;
    }
    bool ok = found && cupsSetDests2(CUPS_HTTP_DEFAULT, n, dests) == 0;
    cupsFreeDests(n, dests);
    if (!found)
        g_set_error(error, XD_CUPS_ERROR, IPP_STATUS_ERROR_NOT_FOUND,
                    "The printer %s no longer exists.", name);
    else if (!ok)
        g_set_error(error, XD_CUPS_ERROR, IPP_STATUS_ERROR_INTERNAL,
                    "The default printer could not be saved in ~/.cups/lpoptions.");
    return ok;
}

bool xd_printer_pause(const char *name, bool paused, GError **error) {
    ipp_t *req = printer_request(paused ? IPP_OP_PAUSE_PRINTER : IPP_OP_RESUME_PRINTER, name);
    return send_request(req, "/admin/", error,
                        paused ? "The printer could not be paused"
                              : "The printer could not be resumed");
}

bool xd_printer_cancel_all(const char *name, GError **error) {
    cupsCancelJob2(CUPS_HTTP_DEFAULT, name, CUPS_JOBID_ALL, 0);
    return check(error, "The documents could not be cancelled");
}

bool xd_printer_delete(const char *name, GError **error) {
    ipp_t *req = printer_request(IPP_OP_CUPS_DELETE_PRINTER, name);
    return send_request(req, "/admin/", error, "The printer could not be deleted");
}

static void add_text(ipp_t *req, const char *attr, const char *value) {
    if (value != nullptr)
        ippAddString(req, IPP_TAG_PRINTER, IPP_TAG_TEXT, attr, nullptr, value);
}

bool xd_printer_add(const char *name, const char *device_uri, const char *info,
                    const char *location, GError **error) {
    ipp_t *req = printer_request(IPP_OP_CUPS_ADD_MODIFY_PRINTER, name);
    /* "everywhere": the scheduler makes the driver from what the printer
     * says of itself (IPP Everywhere, AirPrint, Mopria). */
    ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_NAME, "ppd-name", nullptr, "everywhere");
    ippAddString(req, IPP_TAG_PRINTER, IPP_TAG_URI, "device-uri", nullptr, device_uri);
    add_text(req, "printer-info", info);
    add_text(req, "printer-location", location);
    ippAddInteger(req, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", IPP_PSTATE_IDLE);
    ippAddBoolean(req, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
    return send_request(req, "/admin/", error, "The printer could not be added");
}

bool xd_printer_modify(const char *name, const char *info, const char *location,
                       GError **error) {
    ipp_t *req = printer_request(IPP_OP_CUPS_ADD_MODIFY_PRINTER, name);
    add_text(req, "printer-info", info);
    add_text(req, "printer-location", location);
    return send_request(req, "/admin/", error, "The printer's properties could not be saved");
}

bool xd_printer_print_test_page(const char *name, int *job_id, GError **error) {
    /* CUPS's own test page, as its web interface prints it: a banner file
     * the scheduler turns into the page. */
    const char *datadir = g_getenv("CUPS_DATADIR");
    char *file = g_build_filename(datadir != nullptr ? datadir : XD_CUPS_DATADIR, "data",
                                  "testprint", nullptr);
    if (!g_file_test(file, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, XD_CUPS_ERROR, IPP_STATUS_ERROR_NOT_FOUND,
                    "The test page could not be printed: %s is missing.", file);
        g_free(file);
        return false;
    }
    int id = cupsPrintFile2(CUPS_HTTP_DEFAULT, name, file, "Test Page", 0, nullptr);
    g_free(file);
    if (job_id != nullptr)
        *job_id = id;
    if (id > 0)
        return true;
    if (check(error, "The test page could not be printed"))
        g_set_error_literal(error, XD_CUPS_ERROR, IPP_STATUS_ERROR_INTERNAL,
                            "The test page could not be printed.");
    return false;
}

bool xd_print_job_do(const char *printer, int job, xd_job_op op, GError **error) {
    if (op == XD_JOB_CANCEL) {
        cupsCancelJob2(CUPS_HTTP_DEFAULT, printer, job, 0);
        return check(error, "The document could not be cancelled");
    }
    ipp_op_t ops[] = {
        [XD_JOB_PAUSE] = IPP_OP_HOLD_JOB,
        [XD_JOB_RESUME] = IPP_OP_RELEASE_JOB,
        [XD_JOB_RESTART] = IPP_OP_RESTART_JOB,
    };
    ipp_t *req = ippNewRequest(ops[op]);
    char uri[HTTP_MAX_URI];
    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, (int)sizeof(uri), "ipp", nullptr, "localhost",
                     ippPort(), "/jobs/%d", job);
    ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", nullptr, uri);
    ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", nullptr,
                 cupsUser());
    return send_request(req, "/jobs/", error, "The document could not be changed");
}

char *xd_printer_user_option(const char *name, const char *option) {
    cups_dest_t *dests = nullptr;
    int n = cupsGetDests2(CUPS_HTTP_DEFAULT, &dests);
    cups_dest_t *d = cupsGetDest(name, nullptr, n, dests);
    const char *v = d != nullptr ? cupsGetOption(option, d->num_options, d->options) : nullptr;
    char *value = g_strdup(v);
    cupsFreeDests(n, dests);
    return value;
}

bool xd_printer_set_user_options(const char *name, const char *const *names,
                                 const char *const *values, size_t n, GError **error) {
    cups_dest_t *dests = nullptr;
    int count = cupsGetDests2(CUPS_HTTP_DEFAULT, &dests);
    cups_dest_t *d = cupsGetDest(name, nullptr, count, dests);
    if (d == nullptr) {
        cupsFreeDests(count, dests);
        g_set_error(error, XD_CUPS_ERROR, IPP_STATUS_ERROR_NOT_FOUND,
                    "The printer %s no longer exists.", name);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (values[i] != nullptr)
            d->num_options = cupsAddOption(names[i], values[i], d->num_options, &d->options);
        else
            d->num_options = cupsRemoveOption(names[i], d->num_options, &d->options);
    }
    bool ok = cupsSetDests2(CUPS_HTTP_DEFAULT, count, dests) == 0;
    cupsFreeDests(count, dests);
    if (!ok)
        g_set_error(error, XD_CUPS_ERROR, IPP_STATUS_ERROR_INTERNAL,
                    "The printing preferences could not be saved in ~/.cups/lpoptions.");
    return ok;
}
