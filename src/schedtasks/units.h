/* SPDX-License-Identifier: MIT
 * Scheduled Tasks and the user's systemd (org.freedesktop.systemd1 on the
 * session bus, the user's own service manager): listing the timers,
 * writing a task's units into $XDG_CONFIG_HOME/systemd/user and loading,
 * enabling, starting, stopping and removing them. Everything about a
 * unit is read from the manager's properties, never from its files.
 */
#ifndef XD_SCHEDTASKS_UNITS_H
#define XD_SCHEDTASKS_UNITS_H

#include <gio/gio.h>

#include "task.h"

/* A timer as the folder shows it. */
typedef struct xd_task_info {
    char *base;    /* ours: "xd-task-Backup"; others: the timer's name without ".timer" */
    char *timer;   /* "xd-task-Backup.timer" */
    char *service; /* the unit it starts */
    bool ours;     /* written by us: can be edited and deleted */
    char *name;    /* its Description= */
    char **argv;   /* the service's ExecStart= (the first command) */
    char *workdir;
    guint64 max_run_sec;
    xd_schedule sched;
    char *calendar; /* the timer's OnCalendar= (CUSTOM shows it) */
    bool enabled;   /* UnitFileState */
    bool running;   /* the service is active */
    bool failed_start; /* it couldn't be started last time */
    guint64 next_usec, last_usec; /* realtime; 0 none */
    bool has_result;  /* the service has run */
    int exit_status;  /* ExecMainStatus: the exit status, or the signal when killed */
    bool killed;
    char *result;     /* "success", "exit-code", "signal", "timeout"... */
} xd_task_info;

void xd_task_info_free(xd_task_info *info);

/* Our tasks (the xd-task-*.timer files in the unit folder) and, when
 * `others`, the user's other timers. Free with g_ptr_array_unref. */
GPtrArray *xd_tasks_list(GDBusConnection *bus, bool others);

/* $XDG_CONFIG_HOME/systemd/user. Free with g_free. */
char *xd_tasks_unit_dir(void);

/* An unused unit base for a new task's name ("xd-task-Backup-2"...). */
char *xd_tasks_new_base(const char *name);

/* Writes `base`.service and `base`.timer, reloads the manager, enables
 * the timer (or disables it) and starts it when it runs on a calendar
 * (startup and logon timers start with the manager or session: started
 * now, they'd run at once). Files are written even when the manager
 * can't be reached; the error says so. */
bool xd_tasks_save(GDBusConnection *bus, const char *base, const xd_task *task, bool enabled,
                   GError **error);

/* Run (start the service now) and End Task (stop it). */
bool xd_tasks_run(GDBusConnection *bus, const xd_task_info *info, GError **error);
bool xd_tasks_end(GDBusConnection *bus, const xd_task_info *info, GError **error);

/* Stops and disables the timer, moves both files to the Recycle Bin (or
 * deletes them when there's none) and reloads the manager. */
bool xd_tasks_delete(GDBusConnection *bus, const xd_task_info *info, GError **error);

#endif
