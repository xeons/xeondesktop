/* SPDX-License-Identifier: MIT */
#include <errno.h>

#include <glib/gstdio.h>

#include "../core/common.h"
#include "units.h"

#define SYSTEMD "org.freedesktop.systemd1"
#define SYSTEMD_PATH "/org/freedesktop/systemd1"
#define MANAGER SYSTEMD ".Manager"
#define UNIT_IFACE SYSTEMD ".Unit"
#define TIMER_IFACE SYSTEMD ".Timer"
#define SERVICE_IFACE SYSTEMD ".Service"

static constexpr char PREFIX[] = "xd-task-";
static constexpr int TIMEOUT_MS = 15000;

void xd_task_info_free(xd_task_info *info) {
    if (info == nullptr)
        return;
    g_free(info->base);
    g_free(info->timer);
    g_free(info->service);
    g_free(info->name);
    g_strfreev(info->argv);
    g_free(info->workdir);
    g_free(info->calendar);
    g_free(info->result);
    g_free(info);
}

char *xd_tasks_unit_dir(void) {
    return g_build_filename(g_get_user_config_dir(), "systemd", "user", nullptr);
}

/* ---- the manager --------------------------------------------------------------------- */

static GVariant *manager_call(GDBusConnection *bus, const char *method, GVariant *args,
                              const char *reply, GError **error) {
    if (bus == nullptr) {
        if (args != nullptr)
            g_variant_unref(g_variant_ref_sink(args));
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                            "The session bus isn't available.");
        return nullptr;
    }
    return g_dbus_connection_call_sync(bus, SYSTEMD, SYSTEMD_PATH, MANAGER, method, args,
                                       reply != nullptr ? G_VARIANT_TYPE(reply) : nullptr,
                                       G_DBUS_CALL_FLAGS_NONE, TIMEOUT_MS, nullptr, error);
}

static bool manager_do(GDBusConnection *bus, const char *method, GVariant *args,
                       GError **error) {
    GVariant *r = manager_call(bus, method, args, nullptr, error);
    if (r != nullptr)
        g_variant_unref(r);
    return r != nullptr;
}

/* For steps that may fail harmlessly (stopping what isn't loaded). */
static void manager_try(GDBusConnection *bus, const char *method, GVariant *args) {
    manager_do(bus, method, args, nullptr);
}

static GVariant *unit_files(const char *unit) {
    const char *files[] = {unit, nullptr};
    return g_variant_new_strv(files, -1);
}

/* A unit's properties on one interface (a{sv}); nullptr when unknown. */
static GVariant *unit_props(GDBusConnection *bus, const char *unit, const char *iface) {
    GVariant *r = manager_call(bus, "LoadUnit", g_variant_new("(s)", unit), "(o)", nullptr);
    if (r == nullptr)
        return nullptr;
    const char *path = nullptr;
    g_variant_get(r, "(&o)", &path);
    GVariant *all = g_dbus_connection_call_sync(
        bus, SYSTEMD, path, "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", iface), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
        TIMEOUT_MS, nullptr, nullptr);
    g_variant_unref(r);
    if (all == nullptr)
        return nullptr;
    GVariant *dict = g_variant_get_child_value(all, 0);
    g_variant_unref(all);
    return dict;
}

static char *prop_str(GVariant *dict, const char *name) {
    const char *s = nullptr;
    return dict != nullptr && g_variant_lookup(dict, name, "&s", &s) ? g_strdup(s) : nullptr;
}

static guint64 prop_u64(GVariant *dict, const char *name) {
    guint64 v = 0;
    if (dict == nullptr || !g_variant_lookup(dict, name, "t", &v))
        return 0;
    return v;
}

/* ---- listing -------------------------------------------------------------------------- */

/* The schedule from the timer's TimersCalendar and TimersMonotonic. */
static void read_schedule(xd_task_info *info, GVariant *timer) {
    /* What the editor starts from when the kind is changed. */
    static constexpr xd_schedule defaults = {.kind = XD_SCHED_CUSTOM, .hour = 9, .weekdays = 1,
                                             .day = 1, .months = XD_SCHED_ALL_MONTHS};
    info->sched = defaults;
    GVariant *cal = timer != nullptr ? g_variant_lookup_value(timer, "TimersCalendar",
                                                              G_VARIANT_TYPE("a(sst)"))
                                     : nullptr;
    GVariant *mono = timer != nullptr ? g_variant_lookup_value(timer, "TimersMonotonic",
                                                               G_VARIANT_TYPE("a(stt)"))
                                      : nullptr;
    gsize ncal = cal != nullptr ? g_variant_n_children(cal) : 0;
    gsize nmono = mono != nullptr ? g_variant_n_children(mono) : 0;
    if (ncal > 0) {
        const char *spec = nullptr;
        g_variant_get_child(cal, 0, "(&s&st)", nullptr, &spec, nullptr);
        info->calendar = g_strdup(spec);
    }
    if (ncal == 1 && nmono == 0) {
        xd_schedule_from_calendar(info->calendar, &info->sched);
    } else if (ncal == 0 && nmono == 1) {
        const char *base = nullptr;
        guint64 value = 0;
        g_variant_get_child(mono, 0, "(&stt)", &base, &value, nullptr);
        if (value == 0 && strcmp(base, "OnStartupUSec") == 0)
            info->sched.kind = XD_SCHED_STARTUP;
        else if (value == 0 && strcmp(base, "OnActiveUSec") == 0)
            info->sched.kind = XD_SCHED_LOGON;
    }
    if (info->sched.kind == XD_SCHED_CUSTOM)
        info->sched = defaults;
    if (cal != nullptr)
        g_variant_unref(cal);
    if (mono != nullptr)
        g_variant_unref(mono);
}

static void read_service(GDBusConnection *bus, xd_task_info *info) {
    GVariant *unit = unit_props(bus, info->service, UNIT_IFACE);
    char *state = prop_str(unit, "ActiveState");
    info->running = state != nullptr && (strcmp(state, "active") == 0 ||
                                         strcmp(state, "activating") == 0 ||
                                         strcmp(state, "deactivating") == 0 ||
                                         strcmp(state, "reloading") == 0);
    g_free(state);
    if (unit != nullptr)
        g_variant_unref(unit);

    GVariant *svc = unit_props(bus, info->service, SERVICE_IFACE);
    if (svc == nullptr)
        return;
    GVariant *exec = g_variant_lookup_value(svc, "ExecStart", G_VARIANT_TYPE("a(sasbttttuii)"));
    if (exec != nullptr && g_variant_n_children(exec) > 0) {
        GVariant *first = g_variant_get_child_value(exec, 0);
        GVariant *argv = g_variant_get_child_value(first, 1);
        info->argv = g_variant_dup_strv(argv, nullptr);
        g_variant_unref(argv);
        g_variant_unref(first);
    }
    if (exec != nullptr)
        g_variant_unref(exec);
    char *dir = prop_str(svc, "WorkingDirectory");
    /* "!" marks a folder that may be missing (WorkingDirectory=-...). */
    if (dir != nullptr && dir[0] == '!')
        memmove(dir, dir + 1, strlen(dir));
    info->workdir = dir;
    guint64 max = prop_u64(svc, "RuntimeMaxUSec");
    info->max_run_sec = max == G_MAXUINT64 ? 0 : max / G_USEC_PER_SEC;
    info->has_result = prop_u64(svc, "ExecMainExitTimestamp") != 0;
    /* Run by hand counts as a run too, not only the timer elapsing. */
    info->last_usec = XD_MAX(info->last_usec, prop_u64(svc, "ExecMainStartTimestamp"));
    gint32 status = 0, code = 0;
    g_variant_lookup(svc, "ExecMainStatus", "i", &status);
    g_variant_lookup(svc, "ExecMainCode", "i", &code);
    info->exit_status = status;
    /* siginfo's CLD_KILLED or CLD_DUMPED: the status is a signal. */
    info->killed = code == 2 || code == 3;
    info->result = prop_str(svc, "Result");
    /* 203 is systemd's "couldn't execute the program". */
    info->failed_start = info->result != nullptr &&
                         (strcmp(info->result, "resources") == 0 ||
                          (strcmp(info->result, "exit-code") == 0 && status == 203));
    g_variant_unref(svc);
}

static void read_info(GDBusConnection *bus, xd_task_info *info) {
    GVariant *unit = unit_props(bus, info->timer, UNIT_IFACE);
    info->name = prop_str(unit, "Description");
    if (info->name == nullptr || info->name[0] == '\0') {
        g_free(info->name);
        info->name = g_strdup(info->base);
    }
    char *file_state = prop_str(unit, "UnitFileState");
    /* Others' timers may be "static" (started by something else) or
     * "generated": still in use. */
    info->enabled = file_state != nullptr && file_state[0] != '\0' &&
                    strcmp(file_state, "disabled") != 0 && !g_str_has_prefix(file_state, "masked") &&
                    strcmp(file_state, "bad") != 0;
    g_free(file_state);
    if (unit != nullptr)
        g_variant_unref(unit);

    GVariant *timer = unit_props(bus, info->timer, TIMER_IFACE);
    read_schedule(info, timer);
    info->next_usec = prop_u64(timer, "NextElapseUSecRealtime");
    info->last_usec = prop_u64(timer, "LastTriggerUSec");
    info->service = prop_str(timer, "Unit");
    if (info->service == nullptr || info->service[0] == '\0') {
        g_free(info->service);
        info->service = g_strconcat(info->base, ".service", nullptr);
    }
    if (timer != nullptr)
        g_variant_unref(timer);
    read_service(bus, info);
}

static xd_task_info *info_new(const char *base, bool ours) {
    xd_task_info *info = g_new0(xd_task_info, 1);
    info->base = g_strdup(base);
    info->timer = g_strconcat(base, ".timer", nullptr);
    info->ours = ours;
    return info;
}

static int compare_info(gconstpointer a, gconstpointer b) {
    const xd_task_info *x = *(xd_task_info *const *)a, *y = *(xd_task_info *const *)b;
    int c = g_utf8_collate(x->name, y->name);
    return c != 0 ? c : strcmp(x->base, y->base);
}

GPtrArray *xd_tasks_list(GDBusConnection *bus, bool others) {
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)xd_task_info_free);
    char *dir = xd_tasks_unit_dir();
    GDir *d = g_dir_open(dir, 0, nullptr);
    for (const char *f; d != nullptr && (f = g_dir_read_name(d)) != nullptr;) {
        if (!g_str_has_prefix(f, PREFIX) || !g_str_has_suffix(f, ".timer"))
            continue;
        char *base = g_strndup(f, strlen(f) - strlen(".timer"));
        g_ptr_array_add(list, info_new(base, true));
        g_free(base);
    }
    if (d != nullptr)
        g_dir_close(d);
    g_free(dir);

    GVariant *units = others ? manager_call(bus, "ListUnitsByPatterns",
                                            g_variant_new("(^as^as)", (const char *[]){nullptr},
                                                          (const char *[]){"*.timer", nullptr}),
                                            "(a(ssssssouso))", nullptr)
                             : nullptr;
    if (units != nullptr) {
        GVariant *array = g_variant_get_child_value(units, 0);
        GVariantIter it;
        g_variant_iter_init(&it, array);
        const char *id = nullptr;
        while (g_variant_iter_next(&it, "(&ssssssouso)", &id, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            if (g_str_has_prefix(id, PREFIX) || !g_str_has_suffix(id, ".timer"))
                continue;
            char *base = g_strndup(id, strlen(id) - strlen(".timer"));
            g_ptr_array_add(list, info_new(base, false));
            g_free(base);
        }
        g_variant_unref(array);
        g_variant_unref(units);
    }
    for (guint i = 0; i < list->len; i++)
        read_info(bus, g_ptr_array_index(list, i));
    g_ptr_array_sort(list, compare_info);
    return list;
}

/* ---- changing ---------------------------------------------------------------------------- */

static bool unit_file_exists(const char *dir, const char *base, const char *suffix) {
    char *file = g_strconcat(base, suffix, nullptr);
    char *path = g_build_filename(dir, file, nullptr);
    bool exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    g_free(file);
    return exists;
}

char *xd_tasks_new_base(const char *name) {
    char *dir = xd_tasks_unit_dir();
    char *stem = xd_task_unit_base(name);
    char *base = g_strdup(stem);
    for (int n = 2; unit_file_exists(dir, base, ".timer") ||
                    unit_file_exists(dir, base, ".service");
         n++) {
        g_free(base);
        base = g_strdup_printf("%s-%d", stem, n);
    }
    g_free(stem);
    g_free(dir);
    return base;
}

static bool write_unit(const char *dir, const char *base, const char *suffix, const char *text,
                       GError **error) {
    char *file = g_strconcat(base, suffix, nullptr);
    char *path = g_build_filename(dir, file, nullptr);
    bool ok = g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0644,
                                       error);
    g_free(path);
    g_free(file);
    return ok;
}

static bool on_calendar(xd_sched_kind kind) {
    return kind != XD_SCHED_STARTUP && kind != XD_SCHED_LOGON;
}

bool xd_tasks_save(GDBusConnection *bus, const char *base, const xd_task *t, bool enabled,
                   GError **error) {
    char *service = xd_task_service_text(t);
    char *timer = xd_task_timer_text(t);
    if (service == nullptr || timer == nullptr || !g_str_has_prefix(base, PREFIX)) {
        g_free(service);
        g_free(timer);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The task's settings are not valid.");
        return false;
    }
    char *timer_unit = g_strconcat(base, ".timer", nullptr);
    /* The old timer goes first: a startup or logon timer started now would
     * run the task at once, and its [Install] target may have changed. */
    if (bus != nullptr) {
        if (!enabled || !on_calendar(t->sched.kind))
            manager_try(bus, "StopUnit", g_variant_new("(ss)", timer_unit, "replace"));
        manager_try(bus, "DisableUnitFiles", g_variant_new("(@asb)", unit_files(timer_unit),
                                                           FALSE));
    }
    char *dir = xd_tasks_unit_dir();
    bool ok = g_mkdir_with_parents(dir, 0755) == 0;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The folder %s could not be created: %s",
                    dir, g_strerror(errno));
    ok = ok && write_unit(dir, base, ".service", service, error) &&
         write_unit(dir, base, ".timer", timer, error);
    ok = ok && manager_do(bus, "Reload", nullptr, error);
    if (ok && enabled) {
        GVariant *r = manager_call(bus, "EnableUnitFiles",
                                   g_variant_new("(@asbb)", unit_files(timer_unit), FALSE, FALSE),
                                   "(ba(sss))", error);
        ok = r != nullptr;
        if (r != nullptr)
            g_variant_unref(r);
        if (ok && on_calendar(t->sched.kind))
            ok = manager_do(bus, "RestartUnit", g_variant_new("(ss)", timer_unit, "replace"),
                            error);
    }
    g_free(dir);
    g_free(timer_unit);
    g_free(service);
    g_free(timer);
    return ok;
}

bool xd_tasks_run(GDBusConnection *bus, const xd_task_info *info, GError **error) {
    return manager_do(bus, "StartUnit", g_variant_new("(ss)", info->service, "replace"), error);
}

bool xd_tasks_end(GDBusConnection *bus, const xd_task_info *info, GError **error) {
    return manager_do(bus, "StopUnit", g_variant_new("(ss)", info->service, "replace"), error);
}

/* To the Recycle Bin, or deleted when there's no bin there. */
static bool remove_file(const char *dir, const char *unit, GError **error) {
    char *path = g_build_filename(dir, unit, nullptr);
    GFile *file = g_file_new_for_path(path);
    bool ok = g_file_trash(file, nullptr, nullptr) || g_file_delete(file, nullptr, error) ||
              !g_file_query_exists(file, nullptr);
    if (ok)
        g_clear_error(error);
    g_object_unref(file);
    g_free(path);
    return ok;
}

bool xd_tasks_delete(GDBusConnection *bus, const xd_task_info *info, GError **error) {
    if (!info->ours) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Only tasks made with Scheduled Tasks can be deleted here.");
        return false;
    }
    if (bus != nullptr) {
        manager_try(bus, "StopUnit", g_variant_new("(ss)", info->timer, "replace"));
        manager_try(bus, "DisableUnitFiles", g_variant_new("(@asb)", unit_files(info->timer),
                                                           FALSE));
    }
    char *dir = xd_tasks_unit_dir();
    char *service = g_strconcat(info->base, ".service", nullptr);
    bool ok = remove_file(dir, info->timer, error) && remove_file(dir, service, error);
    g_free(service);
    g_free(dir);
    return ok && manager_do(bus, "Reload", nullptr, error);
}
