/* SPDX-License-Identifier: MIT
 * Printers and Faxes' requests to CUPS, through libcups: the printers and
 * their jobs, and what changes them. Each call blocks until the scheduler
 * answers; listing is safe in worker threads (libcups keeps a connection
 * per thread). Changes run on the main thread, where cupsd may ask for a
 * password through the box auth.c shows, or in a worker thread between
 * xd_cups_auth_worker_begin and _end (Add Printer).
 */
#ifndef XD_PRINTERS_CUPSOPS_H
#define XD_PRINTERS_CUPSOPS_H

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
void xd_printer_free(xd_printer *p);
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
/* Changes the Comment and Location. */
bool xd_printer_modify(const char *name, const char *info, const char *location,
                       GError **error);
/* Prints CUPS's test page; `job_id` gets the job's number. */
bool xd_printer_print_test_page(const char *name, int *job_id, GError **error);

bool xd_print_job_do(const char *printer, int job, xd_job_op op, GError **error);

/* Per-user options (Printing Preferences), as lpoptions keeps them. The
 * value or nullptr; g_free it. */
char *xd_printer_user_option(const char *name, const char *option);
/* Sets per-user options, `n` names and values; a nullptr value removes
 * the option (the printer's default applies again). */
bool xd_printer_set_user_options(const char *name, const char *const *names,
                                 const char *const *values, size_t n, GError **error);

#endif
