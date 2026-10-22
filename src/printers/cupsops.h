/* SPDX-License-Identifier: MIT
 * Printers and Faxes' requests to CUPS, through libcups: the printers and
 * their jobs, and what changes them. Each call blocks until the scheduler
 * answers, so the windows make them in worker threads (libcups keeps a
 * connection per thread): listing ones ask nobody for a password, and
 * changes run between xd_cups_auth_worker_begin and _end, where cupsd
 * may ask for one through the box auth.c shows.
 */
#ifndef XD_PRINTERS_CUPSOPS_H
#define XD_PRINTERS_CUPSOPS_H

#include <cups/ipp.h>
#include <glib.h>

typedef struct xd_printer {
    char *name;
    char *info;       /* printer-info: XP's Comment */
    char *location;
    char *model;      /* printer-make-and-model */
    char *device_uri;
    char *reasons;    /* printer-state-reasons, comma-separated */
    char *message;    /* printer-state-message */
    int state;        /* IPP printer-state */
    int jobs;         /* documents waiting or printing */
    bool accepting;
    bool is_default;  /* the user's default (lpoptions), else the system's */
    bool fax;
    bool shared;
} xd_printer;

typedef struct xd_print_job {
    int id;
    char *name, *owner;
    int state; /* IPP job-state */
    int pages, pages_done;
    guint64 size; /* bytes */
    gint64 created; /* Unix time */
} xd_print_job;

typedef enum xd_job_op : uint8_t {
    XD_JOB_PAUSE,
    XD_JOB_RESUME,
    XD_JOB_RESTART,
    XD_JOB_CANCEL,
} xd_job_op;

/* A worker thread's libcups requests ask nobody for a password (libcups
 * would ask on the terminal). The calls here that read do it themselves;
 * a worker calling libcups directly does it first. */
void xd_cups_thread_init(void);

GQuark xd_cups_error_quark(void);
#define XD_CUPS_ERROR (xd_cups_error_quark())
/* Error codes are IPP status codes; this one means the password box was
 * cancelled, which needs no message. */
#define XD_CUPS_ERROR_CANCELED 0x1000

/* The printers, sorted by name. Free with g_ptr_array_unref. */
GPtrArray *xd_printers_list(GError **error);
/* One printer, or nullptr. Free with xd_printer_free. */
xd_printer *xd_printer_get(const char *name, GError **error);
void xd_printer_free(xd_printer *printer);
/* Whether a printer or class has this name (false if the scheduler can't
 * be reached). */
bool xd_printer_exists(const char *name);

/* The printer's documents not yet printed, oldest first. */
GPtrArray *xd_print_jobs_list(const char *printer, GError **error);

/* Makes `name` this user's default printer (~/.cups/lpoptions). */
bool xd_printer_set_default(const char *name, GError **error);
/* Pause-Printer / Resume-Printer. */
bool xd_printer_pause(const char *name, bool paused, GError **error);
bool xd_printer_cancel_all(const char *name, GError **error);
bool xd_printer_delete(const char *name, GError **error);
/* CUPS-Add-Modify-Printer: a new IPP Everywhere queue for `device_uri`
 * (the scheduler asks the printer what it can do), enabled and
 * accepting jobs. */
bool xd_printer_add(const char *name, const char *device_uri, const char *info,
                    const char *location, GError **error);
/* What Properties' Apply changes: nullptr strings and a negative
 * `shared` are left as they are. */
typedef struct xd_printer_changes {
    const char *info, *location;
    const char *device_uri; /* the Ports tab's port */
    int shared;             /* the Sharing tab: printer-is-shared */
} xd_printer_changes;
/* The CUPS-Add-Modify-Printer request for `changes` (the unit tests
 * read it). Free with ippDelete. */
ipp_t *xd_printer_modify_request(const char *name, const xd_printer_changes *changes);
bool xd_printer_modify(const char *name, const xd_printer_changes *changes, GError **error);

/* A port CUPS-Get-Devices found: a device CUPS's backends see (a USB
 * printer, one on the network), or a kind of port with no address yet
 * ("socket", "lpd": the uri is the bare scheme). */
typedef struct xd_print_device {
    char *uri, *info, *model;
    char *kind; /* device-class: "direct", "network", "file", "serial" */
} xd_print_device;
/* The ports, in the order CUPS gives them, after the backends have
 * looked for up to `timeout` seconds. Run it in a worker between
 * xd_cups_auth_worker_begin and _end: cupsd's default policy keeps the
 * list to printer administrators. Free with g_ptr_array_unref. */
GPtrArray *xd_print_devices_list(int timeout, GError **error);

/* Whether the scheduler shares its printers on the network (cupsd.conf's
 * Browsing and BrowseLocalProtocols, CUPS_SERVER_SHARE_PRINTERS): 1, 0,
 * or -1 if it can't be told without a printer administrator's password.
 * `printer` is one of its printers, for the attribute that says so. */
int xd_cups_server_sharing(const char *printer);
/* Turns the scheduler's printer sharing on or off (cupsd restarts). */
bool xd_cups_set_server_sharing(bool on, GError **error);
/* Prints CUPS's test page; `job_id` gets the job's number. */
bool xd_printer_print_test_page(const char *name, int *job_id, GError **error);

bool xd_print_job_do(const char *printer, int job, xd_job_op op, GError **error);

/* Per-user options (Printing Preferences), as lpoptions keeps them. The
 * value or nullptr; g_free it. */
char *xd_printer_user_option(const char *name, const char *option);
/* Sets per-user options, `value_count` names and values; a nullptr value removes
 * the option (the printer's default applies again). */
bool xd_printer_set_user_options(const char *name, const char *const *names,
                                 const char *const *values, size_t value_count, GError **error);

#endif
