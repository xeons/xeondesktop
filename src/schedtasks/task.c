/* SPDX-License-Identifier: MIT */
#include "../core/common.h"
#include "task.h"

static const char *const day_names[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
static const char *const month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* ---- schedules ---------------------------------------------------------------- */

static bool time_valid(const xd_schedule *schedule) {
    return schedule->hour >= 0 && schedule->hour < 24 && schedule->minute >= 0 &&
           schedule->minute < 60;
}

bool xd_schedule_valid(const xd_schedule *schedule) {
    switch (schedule->kind) {
    case XD_SCHED_DAILY:
        return time_valid(schedule);
    case XD_SCHED_WEEKLY:
        return time_valid(schedule) && (schedule->weekdays & XD_SCHED_ALL_DAYS) != 0;
    case XD_SCHED_MONTHLY:
        return time_valid(schedule) && schedule->day >= 0 && schedule->day <= 31 &&
               (schedule->months & XD_SCHED_ALL_MONTHS) != 0;
    case XD_SCHED_ONCE:
        /* systemd's calendar years end at 2199 as well. */
        return time_valid(schedule) && schedule->year >= 1970 && schedule->year <= 2199 &&
               schedule->month >= 1 && schedule->month <= 12 && schedule->mday >= 1 &&
               g_date_valid_dmy((GDateDay)schedule->mday, (GDateMonth)schedule->month,
                                (GDateYear)schedule->year);
    case XD_SCHED_STARTUP:
    case XD_SCHED_LOGON:
        return true;
    case XD_SCHED_CUSTOM:
    case XD_SCHED_COUNT:
        break;
    }
    return false;
}

/* systemd's weekday list: Monday first, runs of three or more as "A..B";
 * all seven is no weekday at all. */
static void append_weekdays(GString *out, unsigned days) {
    bool first = true;
    for (int run_start = 0; run_start < 7;) {
        if ((days & (1u << run_start)) == 0) {
            run_start++;
            continue;
        }
        int end = run_start;
        while (end + 1 < 7 && (days & (1u << (end + 1))) != 0)
            end++;
        if (!first)
            g_string_append_c(out, ',');
        first = false;
        if (end - run_start >= 2) {
            g_string_append_printf(out, "%s..%s", day_names[run_start], day_names[end]);
        } else {
            g_string_append(out, day_names[run_start]);
            if (end > run_start)
                g_string_append_printf(out, ",%s", day_names[end]);
        }
        run_start = end + 1;
    }
}

/* systemd lists months one by one ("01,02,03"), "*" for all of them. */
static void append_months(GString *out, unsigned months) {
    if ((months & XD_SCHED_ALL_MONTHS) == XD_SCHED_ALL_MONTHS) {
        g_string_append_c(out, '*');
        return;
    }
    bool first = true;
    for (int month_index = 0; month_index < 12; month_index++) {
        if ((months & (1u << month_index)) == 0)
            continue;
        g_string_append_printf(out, "%s%02d", first ? "" : ",", month_index + 1);
        first = false;
    }
}

char *xd_schedule_calendar(const xd_schedule *schedule) {
    if (!xd_schedule_valid(schedule))
        return nullptr;
    GString *out = g_string_new(nullptr);
    switch (schedule->kind) {
    case XD_SCHED_DAILY:
        g_string_append(out, "*-*-*");
        break;
    case XD_SCHED_WEEKLY:
        if ((schedule->weekdays & XD_SCHED_ALL_DAYS) != XD_SCHED_ALL_DAYS) {
            append_weekdays(out, schedule->weekdays);
            g_string_append_c(out, ' ');
        }
        g_string_append(out, "*-*-*");
        break;
    case XD_SCHED_MONTHLY:
        g_string_append(out, "*-");
        append_months(out, schedule->months);
        if (schedule->day == 0)
            g_string_append(out, "~01"); /* the first day counting from the end */
        else
            g_string_append_printf(out, "-%02d", schedule->day);
        break;
    case XD_SCHED_ONCE:
        g_string_append_printf(out, "%04d-%02d-%02d", schedule->year, schedule->month,
                               schedule->mday);
        break;
    case XD_SCHED_STARTUP:
    case XD_SCHED_LOGON:
    case XD_SCHED_CUSTOM:
    case XD_SCHED_COUNT:
        g_string_free(out, TRUE);
        return nullptr;
    }
    g_string_append_printf(out, " %02d:%02d:00", schedule->hour, schedule->minute);
    return g_string_free(out, FALSE);
}

/* A decimal number in [min, max], nothing else. */
static bool number(const char *text, unsigned min, unsigned max, unsigned *out) {
    guint64 value = 0;
    if (!g_ascii_string_to_unsigned(text, 10, min, max, &value, nullptr))
        return false;
    *out = (unsigned)value;
    return true;
}

static int day_index(const char *name) {
    for (int weekday = 0; weekday < 7; weekday++)
        if (strcmp(name, day_names[weekday]) == 0)
            return weekday;
    return -1;
}

/* "Mon,Wed..Fri,Sun" */
static bool parse_weekdays(const char *text, unsigned *days) {
    *days = 0;
    char **parts = g_strsplit(text, ",", -1);
    bool ok = parts[0] != nullptr;
    for (char **part = parts; ok && *part != nullptr; part++) {
        char **range = g_strsplit(*part, "..", 2);
        int first_day = day_index(range[0]);
        int last_day = range[1] != nullptr ? day_index(range[1]) : first_day;
        ok = first_day >= 0 && last_day >= first_day;
        for (int weekday = first_day; ok && weekday <= last_day; weekday++)
            *days |= 1u << weekday;
        g_strfreev(range);
    }
    g_strfreev(parts);
    return ok && *days != 0;
}

/* "*" or "01,04" */
static bool parse_months(const char *text, unsigned *months) {
    if (strcmp(text, "*") == 0) {
        *months = XD_SCHED_ALL_MONTHS;
        return true;
    }
    *months = 0;
    char **parts = g_strsplit(text, ",", -1);
    bool ok = parts[0] != nullptr;
    for (char **part = parts; ok && *part != nullptr; part++) {
        unsigned month = 0;
        ok = strlen(*part) == 2 && number(*part, 1, 12, &month);
        if (ok)
            *months |= 1u << (month - 1);
    }
    g_strfreev(parts);
    return ok;
}

/* "09:30:00": seconds are always 0 in ours. */
static bool parse_time(const char *text, xd_schedule *schedule) {
    char **fields = g_strsplit(text, ":", -1);
    unsigned hour = 0, minute = 0, sec = 1;
    bool ok = g_strv_length(fields) == 3 && strlen(fields[0]) == 2 && strlen(fields[1]) == 2 &&
              strcmp(fields[2], "00") == 0 && number(fields[0], 0, 23, &hour) &&
              number(fields[1], 0, 59, &minute) && number(fields[2], 0, 0, &sec);
    g_strfreev(fields);
    if (ok) {
        schedule->hour = (int)hour;
        schedule->minute = (int)minute;
    }
    return ok;
}

/* The date part: "*-*-*", "2026-10-20", "*-MONTHS-DD" or "*-MONTHS~01". */
static bool parse_date(const char *text, xd_schedule *schedule) {
    if (strcmp(text, "*-*-*") == 0) {
        schedule->kind = XD_SCHED_DAILY;
        return true;
    }
    if (g_str_has_prefix(text, "*-")) {
        const char *rest = text + 2;
        const char *tilde = strchr(rest, '~');
        const char *dash = strrchr(rest, '-');
        const char *sep = tilde != nullptr ? tilde : dash;
        if (sep == nullptr || (tilde != nullptr && strcmp(tilde, "~01") != 0))
            return false;
        char *months = g_strndup(rest, (size_t)(sep - rest));
        unsigned day = 0;
        bool ok = parse_months(months, &schedule->months) &&
                  (tilde != nullptr || (strlen(sep + 1) == 2 && number(sep + 1, 1, 31, &day)));
        g_free(months);
        schedule->kind = XD_SCHED_MONTHLY;
        schedule->day = (int)day;
        return ok;
    }
    char **fields = g_strsplit(text, "-", -1);
    unsigned y = 0, month = 0, day_of_month = 0;
    bool ok = g_strv_length(fields) == 3 && strlen(fields[0]) == 4 && strlen(fields[1]) == 2 &&
              strlen(fields[2]) == 2 && number(fields[0], 1970, 2199, &y) &&
              number(fields[1], 1, 12, &month) && number(fields[2], 1, 31, &day_of_month);
    g_strfreev(fields);
    schedule->kind = XD_SCHED_ONCE;
    schedule->year = (int)y;
    schedule->month = (int)month;
    schedule->mday = (int)day_of_month;
    return ok;
}

bool xd_schedule_from_calendar(const char *spec, xd_schedule *schedule) {
    *schedule = (xd_schedule){.kind = XD_SCHED_CUSTOM,
                              .weekdays = XD_SCHED_ALL_DAYS,
                              .day = 1,
                              .months = XD_SCHED_ALL_MONTHS};
    xd_schedule parsed = *schedule;
    char **words = g_strsplit(spec != nullptr ? spec : "", " ", -1);
    guint word_count = g_strv_length(words);
    bool ok = false;
    if (word_count == 2) {
        ok = parse_date(words[0], &parsed) && parse_time(words[1], &parsed);
        /* All seven days are written as every day: it reads back as daily. */
    } else if (word_count == 3) {
        ok = parse_weekdays(words[0], &parsed.weekdays) && strcmp(words[1], "*-*-*") == 0 &&
             parse_time(words[2], &parsed);
        parsed.kind = XD_SCHED_WEEKLY;
    }
    g_strfreev(words);
    if (!ok || !xd_schedule_valid(&parsed))
        return false;
    /* Only the one way we'd write it: nothing systemd would print otherwise. */
    char *again = xd_schedule_calendar(&parsed);
    ok = again != nullptr && strcmp(again, spec) == 0;
    g_free(again);
    if (ok)
        *schedule = parsed;
    return ok;
}

char *xd_schedule_time_text(int hour, int minute) {
    int h12 = hour % 12 == 0 ? 12 : hour % 12;
    return g_strdup_printf("%d:%02d %s", h12, minute, hour < 12 ? "AM" : "PM");
}

static void append_list(GString *out, const char *const *names, int count, unsigned bits) {
    bool first = true;
    for (int i = 0; i < count; i++) {
        if ((bits & (1u << i)) == 0)
            continue;
        g_string_append_printf(out, "%s%s", first ? "" : ", ", names[i]);
        first = false;
    }
}

char *xd_schedule_describe(const xd_schedule *schedule) {
    if (schedule->kind == XD_SCHED_STARTUP)
        return g_strdup("Run at system startup");
    if (schedule->kind == XD_SCHED_LOGON)
        return g_strdup("Run at user logon");
    if (!xd_schedule_valid(schedule))
        return g_strdup("Custom schedule");
    char *time = xd_schedule_time_text(schedule->hour, schedule->minute);
    GString *out = g_string_new(nullptr);
    g_string_append_printf(out, "At %s ", time);
    g_free(time);
    switch (schedule->kind) {
    case XD_SCHED_DAILY:
        g_string_append(out, "every day");
        break;
    case XD_SCHED_WEEKLY:
        if ((schedule->weekdays & XD_SCHED_ALL_DAYS) == XD_SCHED_ALL_DAYS) {
            g_string_append(out, "every day");
        } else {
            g_string_append(out, "every ");
            append_list(out, day_names, 7, schedule->weekdays);
            g_string_append(out, " of every week");
        }
        break;
    case XD_SCHED_MONTHLY:
        if (schedule->day == 0)
            g_string_append(out, "on the last day of ");
        else
            g_string_append_printf(out, "on day %d of ", schedule->day);
        if ((schedule->months & XD_SCHED_ALL_MONTHS) == XD_SCHED_ALL_MONTHS)
            g_string_append(out, "every month");
        else
            append_list(out, month_names, 12, schedule->months);
        break;
    case XD_SCHED_ONCE:
        g_string_append_printf(out, "on %d/%d/%d", schedule->month, schedule->mday, schedule->year);
        break;
    case XD_SCHED_STARTUP:
    case XD_SCHED_LOGON:
    case XD_SCHED_CUSTOM:
    case XD_SCHED_COUNT:
        break;
    }
    return g_string_free(out, FALSE);
}

/* ---- names ---------------------------------------------------------------------- */

static bool has_control(const char *text) {
    for (const char *cursor = text; *cursor != '\0'; cursor++)
        if ((unsigned char)*cursor < 0x20 || *cursor == 0x7f)
            return true;
    return false;
}

bool xd_task_name_valid(const char *name) {
    if (name == nullptr || !g_utf8_validate(name, -1, nullptr) || has_control(name))
        return false;
    char *trimmed = g_strstrip(g_strdup(name));
    bool ok = trimmed[0] != '\0' && strpbrk(trimmed, "\\/:*?\"<>|") == nullptr;
    g_free(trimmed);
    return ok;
}

bool xd_task_workdir_valid(const char *dir) {
    if (dir == nullptr || !g_path_is_absolute(dir) || !g_utf8_validate(dir, -1, nullptr) ||
        has_control(dir))
        return false;
    size_t length = strlen(dir);
    /* systemd strips blanks around a value, and a final backslash would
     * join the next line to this one. */
    return !g_ascii_isspace(dir[length - 1]) && dir[length - 1] != '\\';
}

char *xd_task_unit_base(const char *name) {
    GString *out = g_string_new("xd-task-");
    size_t start = out->len;
    bool dash = false;
    for (const char *cursor = name != nullptr ? name : ""; *cursor != '\0' && out->len < start + 48;
         cursor++) {
        if (g_ascii_isalnum(*cursor) || *cursor == '_') {
            if (dash && out->len > start)
                g_string_append_c(out, '-');
            dash = false;
            g_string_append_c(out, *cursor);
        } else {
            dash = true;
        }
    }
    if (out->len == start)
        g_string_append(out, "task");
    return g_string_free(out, FALSE);
}

/* ---- command lines ----------------------------------------------------------------- */

char *xd_task_exec_line(const char *const *argv) {
    if (argv == nullptr || argv[0] == nullptr || argv[0][0] == '\0')
        return nullptr;
    GString *out = g_string_new(nullptr);
    for (const char *const *arg = argv; *arg != nullptr; arg++) {
        if (arg != argv)
            g_string_append_c(out, ' ');
        /* Double quotes take C escapes; "%%" and "$$" are a literal % and $
         * (specifiers are expanded on the whole line, variables in each
         * word). Quoting the first word too keeps a leading "-", "@", "+"
         * or "!" from being read as one of ExecStart='s prefixes. */
        g_string_append_c(out, '"');
        for (const char *cursor = *arg; *cursor != '\0'; cursor++) {
            unsigned char character = (unsigned char)*cursor;
            switch (character) {
            case '\\':
                g_string_append(out, "\\\\");
                break;
            case '"':
                g_string_append(out, "\\\"");
                break;
            case '%':
                g_string_append(out, "%%");
                break;
            case '$':
                g_string_append(out, "$$");
                break;
            case '\n':
                g_string_append(out, "\\n");
                break;
            case '\t':
                g_string_append(out, "\\t");
                break;
            default:
                if (character < 0x20 || character == 0x7f)
                    g_string_append_printf(out, "\\x%02x", character);
                else
                    g_string_append_c(out, (char)character);
            }
        }
        g_string_append_c(out, '"');
    }
    return g_string_free(out, FALSE);
}

static bool needs_quoting(const char *word) {
    if (word[0] == '\0')
        return true;
    for (const char *cursor = word; *cursor != '\0'; cursor++)
        if (!g_ascii_isalnum(*cursor) && strchr("/._-+=:,@%", *cursor) == nullptr &&
            (unsigned char)*cursor < 0x80)
            return true;
    return false;
}

char *xd_task_command_line(const char *const *argv) {
    GString *out = g_string_new(nullptr);
    for (const char *const *arg = argv; arg != nullptr && *arg != nullptr; arg++) {
        if (arg != argv)
            g_string_append_c(out, ' ');
        if (needs_quoting(*arg)) {
            char *quoted = g_shell_quote(*arg);
            g_string_append(out, quoted);
            g_free(quoted);
        } else {
            g_string_append(out, *arg);
        }
    }
    return g_string_free(out, FALSE);
}

char **xd_task_argv_from_exec(const char *exec) {
    char **words = nullptr;
    if (exec == nullptr || !g_shell_parse_argv(exec, nullptr, &words, nullptr))
        return nullptr;
    GPtrArray *out = g_ptr_array_new();
    for (char **word = words; *word != nullptr; word++) {
        /* A word that is only a field code goes; codes inside a word are
         * dropped from it. */
        if ((*word)[0] == '%' && (*word)[1] != '\0' && (*word)[1] != '%' && (*word)[2] == '\0')
            continue;
        GString *arg = g_string_new(nullptr);
        for (const char *cursor = *word; *cursor != '\0'; cursor++) {
            if (*cursor != '%') {
                g_string_append_c(arg, *cursor);
            } else if (cursor[1] == '%') {
                g_string_append_c(arg, '%');
                cursor++;
            } else if (cursor[1] != '\0') {
                cursor++;
            }
        }
        g_ptr_array_add(out, g_string_free(arg, FALSE));
    }
    g_strfreev(words);
    g_ptr_array_add(out, nullptr);
    char **argv = (char **)g_ptr_array_free(out, FALSE);
    if (argv[0] == nullptr || argv[0][0] == '\0') {
        g_strfreev(argv);
        return nullptr;
    }
    return argv;
}

/* ---- unit files -------------------------------------------------------------------- */

/* A value where specifiers are expanded: % doubled. */
static void append_value(GString *out, const char *value) {
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor == '%')
            g_string_append_c(out, '%');
        g_string_append_c(out, *cursor);
    }
}

static bool task_writable(const xd_task *task) {
    return task != nullptr && xd_task_name_valid(task->name) && task->argv != nullptr &&
           task->argv[0] != nullptr && task->argv[0][0] != '\0' &&
           (task->workdir == nullptr || task->workdir[0] == '\0' ||
            xd_task_workdir_valid(task->workdir)) &&
           xd_schedule_valid(&task->sched);
}

static void append_description(GString *out, const char *name) {
    char *trimmed = g_strstrip(g_strdup(name));
    g_string_append(out, "Description=");
    append_value(out, trimmed);
    g_string_append_c(out, '\n');
    g_free(trimmed);
}

static constexpr char HEADER[] = "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n";

char *xd_task_service_text(const xd_task *task) {
    if (!task_writable(task))
        return nullptr;
    char *exec = xd_task_exec_line(task->argv);
    GString *out = g_string_new(HEADER);
    g_string_append(out, "[Unit]\n");
    append_description(out, task->name);
    /* Type=exec: active while the program runs (RuntimeMaxSec= doesn't
     * apply to oneshot services), and failing to start is an error. */
    g_string_append_printf(out, "\n[Service]\nType=exec\nExecStart=%s\n", exec);
    if (task->workdir != nullptr && task->workdir[0] != '\0') {
        g_string_append(out, "WorkingDirectory=");
        append_value(out, task->workdir);
        g_string_append_c(out, '\n');
    }
    if (task->max_run_sec > 0)
        g_string_append_printf(out, "RuntimeMaxSec=%" G_GUINT64_FORMAT "\n", task->max_run_sec);
    g_free(exec);
    return g_string_free(out, FALSE);
}

char *xd_task_timer_text(const xd_task *task) {
    if (!task_writable(task))
        return nullptr;
    GString *out = g_string_new(HEADER);
    g_string_append(out, "[Unit]\n");
    append_description(out, task->name);
    const char *target = "timers.target";
    switch (task->sched.kind) {
    case XD_SCHED_STARTUP:
        /* Already past when the timer starts (with the service manager),
         * so it elapses at once. */
        g_string_append(out, "\n[Timer]\nOnStartupSec=0\n");
        break;
    case XD_SCHED_LOGON:
        /* Started with each graphical session and stopped with it: it
         * elapses as soon as it starts. */
        g_string_append(out, "PartOf=graphical-session.target\n"
                              "After=graphical-session.target\n"
                              "\n[Timer]\nOnActiveSec=0\n");
        target = "graphical-session.target";
        break;
    case XD_SCHED_DAILY:
    case XD_SCHED_WEEKLY:
    case XD_SCHED_MONTHLY:
    case XD_SCHED_ONCE: {
        char *cal = xd_schedule_calendar(&task->sched);
        g_string_append_printf(out, "\n[Timer]\nOnCalendar=%s\n", cal);
        g_free(cal);
        break;
    }
    case XD_SCHED_CUSTOM:
    case XD_SCHED_COUNT:
        break;
    }
    g_string_append_printf(out, "\n[Install]\nWantedBy=%s\n", target);
    return g_string_free(out, FALSE);
}
