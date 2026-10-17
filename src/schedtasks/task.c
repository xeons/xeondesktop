/* SPDX-License-Identifier: MIT */
#include "../core/common.h"
#include "task.h"

static const char *const day_names[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
static const char *const month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* ---- schedules ---------------------------------------------------------------- */

static bool time_valid(const xd_schedule *s) {
    return s->hour >= 0 && s->hour < 24 && s->minute >= 0 && s->minute < 60;
}

bool xd_schedule_valid(const xd_schedule *s) {
    switch (s->kind) {
    case XD_SCHED_DAILY:
        return time_valid(s);
    case XD_SCHED_WEEKLY:
        return time_valid(s) && (s->weekdays & XD_SCHED_ALL_DAYS) != 0;
    case XD_SCHED_MONTHLY:
        return time_valid(s) && s->day >= 0 && s->day <= 31 &&
               (s->months & XD_SCHED_ALL_MONTHS) != 0;
    case XD_SCHED_ONCE:
        /* systemd's calendar years end at 2199 as well. */
        return time_valid(s) && s->year >= 1970 && s->year <= 2199 && s->month >= 1 &&
               s->month <= 12 && s->mday >= 1 &&
               g_date_valid_dmy((GDateDay)s->mday, (GDateMonth)s->month, (GDateYear)s->year);
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
    for (int d = 0; d < 7;) {
        if ((days & (1u << d)) == 0) {
            d++;
            continue;
        }
        int end = d;
        while (end + 1 < 7 && (days & (1u << (end + 1))) != 0)
            end++;
        if (!first)
            g_string_append_c(out, ',');
        first = false;
        if (end - d >= 2) {
            g_string_append_printf(out, "%s..%s", day_names[d], day_names[end]);
        } else {
            g_string_append(out, day_names[d]);
            if (end > d)
                g_string_append_printf(out, ",%s", day_names[end]);
        }
        d = end + 1;
    }
}

/* systemd lists months one by one ("01,02,03"), "*" for all of them. */
static void append_months(GString *out, unsigned months) {
    if ((months & XD_SCHED_ALL_MONTHS) == XD_SCHED_ALL_MONTHS) {
        g_string_append_c(out, '*');
        return;
    }
    bool first = true;
    for (int m = 0; m < 12; m++) {
        if ((months & (1u << m)) == 0)
            continue;
        g_string_append_printf(out, "%s%02d", first ? "" : ",", m + 1);
        first = false;
    }
}

char *xd_schedule_calendar(const xd_schedule *s) {
    if (!xd_schedule_valid(s))
        return nullptr;
    GString *out = g_string_new(nullptr);
    switch (s->kind) {
    case XD_SCHED_DAILY:
        g_string_append(out, "*-*-*");
        break;
    case XD_SCHED_WEEKLY:
        if ((s->weekdays & XD_SCHED_ALL_DAYS) != XD_SCHED_ALL_DAYS) {
            append_weekdays(out, s->weekdays);
            g_string_append_c(out, ' ');
        }
        g_string_append(out, "*-*-*");
        break;
    case XD_SCHED_MONTHLY:
        g_string_append(out, "*-");
        append_months(out, s->months);
        if (s->day == 0)
            g_string_append(out, "~01"); /* the first day counting from the end */
        else
            g_string_append_printf(out, "-%02d", s->day);
        break;
    case XD_SCHED_ONCE:
        g_string_append_printf(out, "%04d-%02d-%02d", s->year, s->month, s->mday);
        break;
    case XD_SCHED_STARTUP:
    case XD_SCHED_LOGON:
    case XD_SCHED_CUSTOM:
    case XD_SCHED_COUNT:
        g_string_free(out, TRUE);
        return nullptr;
    }
    g_string_append_printf(out, " %02d:%02d:00", s->hour, s->minute);
    return g_string_free(out, FALSE);
}

/* A decimal number in [min, max], nothing else. */
static bool number(const char *text, unsigned min, unsigned max, unsigned *out) {
    guint64 v = 0;
    if (!g_ascii_string_to_unsigned(text, 10, min, max, &v, nullptr))
        return false;
    *out = (unsigned)v;
    return true;
}

static int day_index(const char *name) {
    for (int d = 0; d < 7; d++)
        if (strcmp(name, day_names[d]) == 0)
            return d;
    return -1;
}

/* "Mon,Wed..Fri,Sun" */
static bool parse_weekdays(const char *text, unsigned *days) {
    *days = 0;
    char **parts = g_strsplit(text, ",", -1);
    bool ok = parts[0] != nullptr;
    for (char **p = parts; ok && *p != nullptr; p++) {
        char **range = g_strsplit(*p, "..", 2);
        int a = day_index(range[0]);
        int b = range[1] != nullptr ? day_index(range[1]) : a;
        ok = a >= 0 && b >= a;
        for (int d = a; ok && d <= b; d++)
            *days |= 1u << d;
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
    for (char **p = parts; ok && *p != nullptr; p++) {
        unsigned m = 0;
        ok = strlen(*p) == 2 && number(*p, 1, 12, &m);
        if (ok)
            *months |= 1u << (m - 1);
    }
    g_strfreev(parts);
    return ok;
}

/* "09:30:00": seconds are always 0 in ours. */
static bool parse_time(const char *text, xd_schedule *s) {
    char **f = g_strsplit(text, ":", -1);
    unsigned h = 0, m = 0, sec = 1;
    bool ok = g_strv_length(f) == 3 && strlen(f[0]) == 2 && strlen(f[1]) == 2 &&
              strcmp(f[2], "00") == 0 && number(f[0], 0, 23, &h) && number(f[1], 0, 59, &m) &&
              number(f[2], 0, 0, &sec);
    g_strfreev(f);
    if (ok) {
        s->hour = (int)h;
        s->minute = (int)m;
    }
    return ok;
}

/* The date part: "*-*-*", "2026-10-20", "*-MONTHS-DD" or "*-MONTHS~01". */
static bool parse_date(const char *text, xd_schedule *s) {
    if (strcmp(text, "*-*-*") == 0) {
        s->kind = XD_SCHED_DAILY;
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
        bool ok = parse_months(months, &s->months) &&
                  (tilde != nullptr || (strlen(sep + 1) == 2 && number(sep + 1, 1, 31, &day)));
        g_free(months);
        s->kind = XD_SCHED_MONTHLY;
        s->day = (int)day;
        return ok;
    }
    char **f = g_strsplit(text, "-", -1);
    unsigned y = 0, m = 0, d = 0;
    bool ok = g_strv_length(f) == 3 && strlen(f[0]) == 4 && strlen(f[1]) == 2 &&
              strlen(f[2]) == 2 && number(f[0], 1970, 2199, &y) && number(f[1], 1, 12, &m) &&
              number(f[2], 1, 31, &d);
    g_strfreev(f);
    s->kind = XD_SCHED_ONCE;
    s->year = (int)y;
    s->month = (int)m;
    s->mday = (int)d;
    return ok;
}

bool xd_schedule_from_calendar(const char *spec, xd_schedule *s) {
    *s = (xd_schedule){.kind = XD_SCHED_CUSTOM, .weekdays = XD_SCHED_ALL_DAYS,
                       .day = 1, .months = XD_SCHED_ALL_MONTHS};
    xd_schedule r = *s;
    char **w = g_strsplit(spec != nullptr ? spec : "", " ", -1);
    guint n = g_strv_length(w);
    bool ok = false;
    if (n == 2) {
        ok = parse_date(w[0], &r) && parse_time(w[1], &r);
        /* All seven days are written as every day: it reads back as daily. */
    } else if (n == 3) {
        ok = parse_weekdays(w[0], &r.weekdays) && strcmp(w[1], "*-*-*") == 0 &&
             parse_time(w[2], &r);
        r.kind = XD_SCHED_WEEKLY;
    }
    g_strfreev(w);
    if (!ok || !xd_schedule_valid(&r))
        return false;
    /* Only the one way we'd write it: nothing systemd would print otherwise. */
    char *again = xd_schedule_calendar(&r);
    ok = again != nullptr && strcmp(again, spec) == 0;
    g_free(again);
    if (ok)
        *s = r;
    return ok;
}

char *xd_schedule_time_text(int hour, int minute) {
    int h12 = hour % 12 == 0 ? 12 : hour % 12;
    return g_strdup_printf("%d:%02d %s", h12, minute, hour < 12 ? "AM" : "PM");
}

static void append_list(GString *out, const char *const *names, int n, unsigned bits) {
    bool first = true;
    for (int i = 0; i < n; i++) {
        if ((bits & (1u << i)) == 0)
            continue;
        g_string_append_printf(out, "%s%s", first ? "" : ", ", names[i]);
        first = false;
    }
}

char *xd_schedule_describe(const xd_schedule *s) {
    if (s->kind == XD_SCHED_STARTUP)
        return g_strdup("Run at system startup");
    if (s->kind == XD_SCHED_LOGON)
        return g_strdup("Run at user logon");
    if (!xd_schedule_valid(s))
        return g_strdup("Custom schedule");
    char *time = xd_schedule_time_text(s->hour, s->minute);
    GString *out = g_string_new(nullptr);
    g_string_append_printf(out, "At %s ", time);
    g_free(time);
    switch (s->kind) {
    case XD_SCHED_DAILY:
        g_string_append(out, "every day");
        break;
    case XD_SCHED_WEEKLY:
        if ((s->weekdays & XD_SCHED_ALL_DAYS) == XD_SCHED_ALL_DAYS) {
            g_string_append(out, "every day");
        } else {
            g_string_append(out, "every ");
            append_list(out, day_names, 7, s->weekdays);
            g_string_append(out, " of every week");
        }
        break;
    case XD_SCHED_MONTHLY:
        if (s->day == 0)
            g_string_append(out, "on the last day of ");
        else
            g_string_append_printf(out, "on day %d of ", s->day);
        if ((s->months & XD_SCHED_ALL_MONTHS) == XD_SCHED_ALL_MONTHS)
            g_string_append(out, "every month");
        else
            append_list(out, month_names, 12, s->months);
        break;
    case XD_SCHED_ONCE:
        g_string_append_printf(out, "on %d/%d/%d", s->month, s->mday, s->year);
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

static bool has_control(const char *s) {
    for (const char *p = s; *p != '\0'; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
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
    size_t n = strlen(dir);
    /* systemd strips blanks around a value, and a final backslash would
     * join the next line to this one. */
    return !g_ascii_isspace(dir[n - 1]) && dir[n - 1] != '\\';
}

char *xd_task_unit_base(const char *name) {
    GString *out = g_string_new("xd-task-");
    size_t start = out->len;
    bool dash = false;
    for (const char *p = name != nullptr ? name : ""; *p != '\0' && out->len < start + 48; p++) {
        if (g_ascii_isalnum(*p) || *p == '_') {
            if (dash && out->len > start)
                g_string_append_c(out, '-');
            dash = false;
            g_string_append_c(out, *p);
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
    for (const char *const *a = argv; *a != nullptr; a++) {
        if (a != argv)
            g_string_append_c(out, ' ');
        /* Double quotes take C escapes; "%%" and "$$" are a literal % and $
         * (specifiers are expanded on the whole line, variables in each
         * word). Quoting the first word too keeps a leading "-", "@", "+"
         * or "!" from being read as one of ExecStart='s prefixes. */
        g_string_append_c(out, '"');
        for (const char *p = *a; *p != '\0'; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
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
                if (c < 0x20 || c == 0x7f)
                    g_string_append_printf(out, "\\x%02x", c);
                else
                    g_string_append_c(out, (char)c);
            }
        }
        g_string_append_c(out, '"');
    }
    return g_string_free(out, FALSE);
}

static bool needs_quoting(const char *word) {
    if (word[0] == '\0')
        return true;
    for (const char *p = word; *p != '\0'; p++)
        if (!g_ascii_isalnum(*p) && strchr("/._-+=:,@%", *p) == nullptr &&
            (unsigned char)*p < 0x80)
            return true;
    return false;
}

char *xd_task_command_line(const char *const *argv) {
    GString *out = g_string_new(nullptr);
    for (const char *const *a = argv; a != nullptr && *a != nullptr; a++) {
        if (a != argv)
            g_string_append_c(out, ' ');
        if (needs_quoting(*a)) {
            char *q = g_shell_quote(*a);
            g_string_append(out, q);
            g_free(q);
        } else {
            g_string_append(out, *a);
        }
    }
    return g_string_free(out, FALSE);
}

char **xd_task_argv_from_exec(const char *exec) {
    char **words = nullptr;
    if (exec == nullptr || !g_shell_parse_argv(exec, nullptr, &words, nullptr))
        return nullptr;
    GPtrArray *out = g_ptr_array_new();
    for (char **w = words; *w != nullptr; w++) {
        /* A word that is only a field code goes; codes inside a word are
         * dropped from it. */
        if ((*w)[0] == '%' && (*w)[1] != '\0' && (*w)[1] != '%' && (*w)[2] == '\0')
            continue;
        GString *arg = g_string_new(nullptr);
        for (const char *p = *w; *p != '\0'; p++) {
            if (*p != '%') {
                g_string_append_c(arg, *p);
            } else if (p[1] == '%') {
                g_string_append_c(arg, '%');
                p++;
            } else if (p[1] != '\0') {
                p++;
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
    for (const char *p = value; *p != '\0'; p++) {
        if (*p == '%')
            g_string_append_c(out, '%');
        g_string_append_c(out, *p);
    }
}

static bool task_writable(const xd_task *t) {
    return t != nullptr && xd_task_name_valid(t->name) && t->argv != nullptr &&
           t->argv[0] != nullptr && t->argv[0][0] != '\0' &&
           (t->workdir == nullptr || t->workdir[0] == '\0' || xd_task_workdir_valid(t->workdir)) &&
           xd_schedule_valid(&t->sched);
}

static void append_description(GString *out, const char *name) {
    char *trimmed = g_strstrip(g_strdup(name));
    g_string_append(out, "Description=");
    append_value(out, trimmed);
    g_string_append_c(out, '\n');
    g_free(trimmed);
}

static constexpr char HEADER[] = "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n";

char *xd_task_service_text(const xd_task *t) {
    if (!task_writable(t))
        return nullptr;
    char *exec = xd_task_exec_line(t->argv);
    GString *out = g_string_new(HEADER);
    g_string_append(out, "[Unit]\n");
    append_description(out, t->name);
    /* Type=exec: active while the program runs (RuntimeMaxSec= doesn't
     * apply to oneshot services), and failing to start is an error. */
    g_string_append_printf(out, "\n[Service]\nType=exec\nExecStart=%s\n", exec);
    if (t->workdir != nullptr && t->workdir[0] != '\0') {
        g_string_append(out, "WorkingDirectory=");
        append_value(out, t->workdir);
        g_string_append_c(out, '\n');
    }
    if (t->max_run_sec > 0)
        g_string_append_printf(out, "RuntimeMaxSec=%" G_GUINT64_FORMAT "\n", t->max_run_sec);
    g_free(exec);
    return g_string_free(out, FALSE);
}

char *xd_task_timer_text(const xd_task *t) {
    if (!task_writable(t))
        return nullptr;
    GString *out = g_string_new(HEADER);
    g_string_append(out, "[Unit]\n");
    append_description(out, t->name);
    const char *target = "timers.target";
    switch (t->sched.kind) {
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
        char *cal = xd_schedule_calendar(&t->sched);
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
