/* SPDX-License-Identifier: MIT
 * Scheduled Tasks' model (xd-schedtasks): a task is a systemd user timer
 * and the service it starts. This part has no D-Bus or GTK: XP's
 * schedules and their OnCalendar= expressions, the text the folder shows,
 * and the unit files written for a task.
 */
#ifndef XD_SCHEDTASKS_TASK_H
#define XD_SCHEDTASKS_TASK_H

#include <glib.h>

/* XP's "Perform this task" choices. CUSTOM is a timer we didn't write
 * (or one edited by hand): shown as it is, never rewritten. */
typedef enum {
    XD_SCHED_DAILY,
    XD_SCHED_WEEKLY,
    XD_SCHED_MONTHLY,
    XD_SCHED_ONCE,
    XD_SCHED_STARTUP, /* when the user's service manager starts */
    XD_SCHED_LOGON,   /* when the graphical session starts */
    XD_SCHED_CUSTOM,
    XD_SCHED_COUNT
} xd_sched_kind;

static constexpr unsigned XD_SCHED_ALL_DAYS = 0x7f;    /* bit 0 Monday ... bit 6 Sunday */
static constexpr unsigned XD_SCHED_ALL_MONTHS = 0xfff; /* bit 0 January ... bit 11 December */

typedef struct xd_schedule {
    xd_sched_kind kind;
    int hour, minute;      /* the start time (not STARTUP, LOGON) */
    unsigned weekdays;     /* WEEKLY */
    int day;               /* MONTHLY: 1..31, or 0 for the last day */
    unsigned months;       /* MONTHLY */
    int year, month, mday; /* ONCE */
} xd_schedule;

/* Whether the fields make sense for the kind (some day picked, a real
 * date, ...). CUSTOM is never valid: it can't be written. */
bool xd_schedule_valid(const xd_schedule *schedule);

/* The OnCalendar= value, in systemd's normalised form (what
 * `systemd-analyze calendar` prints and the timer's TimersCalendar
 * property gives back): "*-*-* 09:00:00", "Mon,Wed..Fri *-*-* 09:00:00",
 * "*-01,04-15 09:00:00", "*-*~01 09:00:00", "2026-10-20 09:00:00".
 * nullptr for STARTUP, LOGON and invalid schedules. Free with g_free. */
char *xd_schedule_calendar(const xd_schedule *schedule);

/* The reverse, for the forms above; false for anything else (the caller
 * shows those as CUSTOM). */
bool xd_schedule_from_calendar(const char *spec, xd_schedule *schedule);

/* XP's Schedule column: "At 9:00 AM every day", "Run at user logon"...
 * Free with g_free. */
char *xd_schedule_describe(const xd_schedule *schedule);

/* "9:00 AM", as XP's English Schedule column writes times. */
char *xd_schedule_time_text(int hour, int minute);

/* What's needed to write a task's units. */
typedef struct xd_task {
    const char *name;          /* the task's name: the units' Description= */
    const char *const *argv;   /* the program and its arguments */
    const char *workdir;       /* Start in; nullptr or "" for the home folder */
    xd_schedule sched;
    guint64 max_run_sec;       /* stop it after this long; 0 never */
} xd_task;

/* XP's rule for names (they were file names there): not empty, and none
 * of \ / : * ? " < > | or control characters. */
bool xd_task_name_valid(const char *name);

/* An absolute folder without control characters or surrounding blanks. */
bool xd_task_workdir_valid(const char *dir);

/* The units' base name for a task name: "xd-task-" and the name's letters
 * and digits ("Back up" -> "xd-task-Back-up"); "xd-task-task" when it has
 * none. The caller adds "-2", "-3"... when taken. Free with g_free. */
char *xd_task_unit_base(const char *name);

/* argv as an ExecStart= line: each word in double quotes with systemd's
 * escapes, so blanks, quotes, backslashes, specifiers (%) and variables
 * ($) all reach the program unchanged. nullptr for an empty argv. */
char *xd_task_exec_line(const char *const *argv);

/* argv as a command line to show and edit: words quoted only when the
 * shell would need it. g_shell_parse_argv reads it back. */
char *xd_task_command_line(const char *const *argv);

/* A desktop entry's Exec= as argv: the shell-style words, without field
 * codes (%f, %U, ...; "%%" is a percent sign). nullptr when it can't be
 * parsed or is empty. Free with g_strfreev. */
char **xd_task_argv_from_exec(const char *exec);

/* The .service and .timer files' text; nullptr when the task can't be
 * written (bad name, no program, invalid schedule). Free with g_free. */
char *xd_task_service_text(const xd_task *task);
char *xd_task_timer_text(const xd_task *task);

#endif
