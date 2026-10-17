/* SPDX-License-Identifier: MIT
 * xd-schedtasks' windows: the folder (main.c), the Scheduled Task Wizard
 * (wizard.c), a task's Properties (props.c) and the schedule editor both
 * of those use (editor.c).
 */
#ifndef XD_SCHEDTASKS_SCHEDTASKS_H
#define XD_SCHEDTASKS_SCHEDTASKS_H

#include <gtk/gtk.h>

#include "units.h"

typedef struct sched_app sched_app;
struct sched_app {
    GtkWidget *window;
    GDBusConnection *bus; /* the session bus; nullptr when there's none */
    void (*refresh)(sched_app *app);
    /* Opens a task's Properties by its unit base (after the wizard). */
    void (*open_props)(sched_app *app, const char *base);
};

static constexpr char SCHED_TITLE[] = "Scheduled Tasks";

/* An error box: what failed, then why. */
void sched_error(GtkWindow *parent, const char *what, const GError *error);

/* A command line as argv for a task: shell-style words, the program
 * found in PATH when it isn't a path (systemd only searches a few
 * folders). false with a message box on `parent` when it can't be read. */
char **sched_parse_command(GtkWindow *parent, const char *text);

/* "Start in" as written to the unit: "~/x" made absolute; false with a
 * message box when it isn't a usable folder. *out is nullptr for none. */
bool sched_parse_workdir(GtkWindow *parent, const char *text, char **out);

/* ---- the schedule editor ------------------------------------------------------- */

typedef struct sched_editor {
    GtkWidget *box;
    GtkWidget *kind; /* "Schedule Task:" (Properties only) */
    GtkWidget *hour, *minute, *time_row;
    GtkWidget *stack;
    GtkWidget *days[7];
    GtkWidget *day_radio, *day, *last_radio, *months[12];
    GtkWidget *calendar;
    GtkWidget *custom;
    xd_sched_kind kind_shown;
    bool has_custom; /* the kind combo has a "Custom" entry */
    void (*changed)(void *data);
    void *data;
} sched_editor;

/* With `with_kind`, a "Schedule Task:" choice heads it, as XP's Schedule
 * tab; the wizard picks the kind on a page of its own instead. */
GtkWidget *sched_editor_new(sched_editor *e, bool with_kind, void (*changed)(void *),
                            void *data);
/* `custom` is the timer's own expression, shown for CUSTOM. */
void sched_editor_set(sched_editor *e, const xd_schedule *s, const char *custom);
void sched_editor_set_kind(sched_editor *e, xd_sched_kind kind);
void sched_editor_get(const sched_editor *e, xd_schedule *s);

/* ---- the windows ------------------------------------------------------------------ */

/* The Scheduled Task Wizard (not modal: it stays open on its own). */
void sched_wizard_open(sched_app *app);

/* A task's Properties; true when it was changed. Other programs' timers
 * are shown read-only. */
bool sched_props_run(sched_app *app, const xd_task_info *info);

#endif
