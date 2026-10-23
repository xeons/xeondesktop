/* SPDX-License-Identifier: MIT
 * Power Options' UPS, watched as Windows' UPS service did: while the power
 * is out, a notification after the first delay and again at each
 * interval, and one when it's restored; at the critical alarm (the
 * minutes on battery, or the UPS's own low battery: UPower's WarningLevel,
 * NUT's LB) the program, then Shut Down or Hibernate through logind, as
 * the battery alarms do (sleep.c, simulated in test sessions).
 *
 * UPower tells of its UPS's changes; NUT's upsd is asked every few
 * seconds, as upsmon does, on a worker thread (upssource.h).
 *
 * Only while the user is logged on: NUT's upsmon, running as root, is
 * what protects the computer at the login screen, and turns the UPS off.
 */
#include <errno.h>
#include <signal.h>

#include "../power/upssource.h"
#include "shell.h"

/* upsmon's POLLFREQ. */
static constexpr guint NUT_POLL_SECONDS = 5;
/* How long the critical alarm's program may run before the computer
 * shuts down or hibernates anyway. */
static constexpr int PROGRAM_WAIT_SECONDS = 30;

typedef struct ups_service {
    xd_shell *shell;
    bool is_started;
    char device[sizeof(((xd_ups_settings *)nullptr)->device)]; /* the one watched */
    GCancellable *cancellable;
    GDBusProxy *upower_device;
    guint poll_timer, wake_timer;
    bool is_reading;
    xd_ups_reading reading; /* the last */
    xd_ups_watch watch;
    /* The critical alarm's program, waited for. */
    pid_t program;
    int program_seconds;
    guint program_timer;
} ups_service;

static ups_service service;

static int64_t get_now(void) { return g_get_monotonic_time() / G_USEC_PER_SEC; }

static void remove_timer(guint *timer) {
    if (*timer != 0)
        g_source_remove(*timer);
    *timer = 0;
}

/* An XP-style message box (xd-message), as the battery alarms show. */
static void show_message(const char *title, const char *text, bool is_warning) {
    char *program = xd_shell_own_program("xd-message");
    char *quoted_program = g_shell_quote(program);
    char *quoted_title = g_shell_quote(title);
    char *quoted_text = g_shell_quote(text);
    char *command = g_strdup_printf("%s --quiet %s %s %s", quoted_program,
                                    is_warning ? "--warning" : "--info", quoted_title,
                                    quoted_text);
    xd_shell_launch(service.shell, command);
    g_free(command);
    g_free(quoted_text);
    g_free(quoted_title);
    g_free(quoted_program);
    g_free(program);
}

/* ---- the critical alarm ---------------------------------------------------------- */

static void do_critical_action(void) {
    const xd_ups_settings *settings = &service.shell->settings.ups;
    fprintf(stderr, "xd-shell: UPS critical alarm: %s\n", xd_ups_action_name(settings->action));
    if (settings->action == XD_UPS_HIBERNATE)
        xd_sleep_now(service.shell, true);
    else
        xd_power_off_now(service.shell);
}

/* The program still running? (The shell's children are reaped as they
 * end: SIGCHLD is ignored.) */
static bool is_program_running(pid_t pid) { return kill(pid, 0) == 0 || errno == EPERM; }

static gboolean on_program_timer([[maybe_unused]] gpointer data) {
    service.program_seconds++;
    if (is_program_running(service.program) && service.program_seconds < PROGRAM_WAIT_SECONDS)
        return G_SOURCE_CONTINUE;
    service.program_timer = 0;
    service.program = 0;
    do_critical_action();
    return G_SOURCE_REMOVE;
}

static void start_critical_alarm(void) {
    const xd_ups_settings *settings = &service.shell->settings.ups;
    if (settings->notify) {
        char *text = g_strdup_printf(
            "The UPS battery is almost exhausted. The computer will %s now.",
            settings->action == XD_UPS_HIBERNATE ? "hibernate" : "shut down");
        show_message("UPS Critical Alarm", text, true);
        g_free(text);
    }
    pid_t pid = settings->run_program && settings->program[0] != '\0'
                    ? xd_shell_launch(service.shell, settings->program)
                    : 0;
    if (pid <= 0) {
        do_critical_action();
        return;
    }
    fprintf(stderr, "xd-shell: UPS critical alarm: running %s\n", settings->program);
    service.program = pid;
    service.program_seconds = 0;
    if (service.program_timer == 0)
        service.program_timer = g_timeout_add_seconds(1, on_program_timer, nullptr);
}

/* ---- the UPS service's decisions ------------------------------------------------- */

static void decide(void);

static gboolean on_wake([[maybe_unused]] gpointer data) {
    service.wake_timer = 0;
    decide();
    return G_SOURCE_REMOVE;
}

/* What the last reading means now. */
static void decide(void) {
    const xd_ups_settings *settings = &service.shell->settings.ups;
    int64_t now = get_now();
    int64_t wake_at = -1;
    unsigned events = xd_ups_watch_step(settings, &service.watch, &service.reading, now, &wake_at);
    remove_timer(&service.wake_timer);
    if (wake_at >= 0)
        service.wake_timer = g_timeout_add_seconds((guint)XD_MAX(wake_at - now, 0), on_wake,
                                                   nullptr);
    const char *computer = g_get_host_name();
    if (events & XD_UPS_EVENT_POWER_FAILED) {
        fprintf(stderr, "xd-shell: UPS: power failure\n");
        char *text = g_strdup_printf("A power failure has occurred at %s. Please save your work "
                                     "and end all activity on this computer.",
                                     computer);
        show_message("Power Failure", text, true);
        g_free(text);
    }
    if (events & XD_UPS_EVENT_POWER_RESTORED) {
        fprintf(stderr, "xd-shell: UPS: power restored\n");
        char *text = g_strdup_printf("Power has been restored at %s. Normal operations have "
                                     "resumed.",
                                     computer);
        show_message("Power Restored", text, false);
        g_free(text);
    }
    if (events & XD_UPS_EVENT_CRITICAL)
        start_critical_alarm();
}

/* ---- readings -------------------------------------------------------------------- */

static void on_upower_changed([[maybe_unused]] GDBusProxy *proxy,
                              [[maybe_unused]] GVariant *changed_properties,
                              [[maybe_unused]] const char *const *invalidated,
                              [[maybe_unused]] gpointer data) {
    xd_ups_read_upower(service.upower_device, &service.reading);
    decide();
}

static void on_upower_device([[maybe_unused]] GObject *source, GAsyncResult *result,
                             [[maybe_unused]] gpointer data) {
    GError *error = nullptr;
    GDBusProxy *device = g_dbus_proxy_new_for_bus_finish(result, &error);
    if (device == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fprintf(stderr, "xd-shell: UPS: %s\n", error->message);
        g_error_free(error);
        return;
    }
    service.upower_device = device;
    g_signal_connect(device, "g-properties-changed", G_CALLBACK(on_upower_changed), &service);
    on_upower_changed(device, nullptr, nullptr, nullptr);
}

static void on_nut_read([[maybe_unused]] GObject *source, GAsyncResult *result,
                        [[maybe_unused]] gpointer data) {
    xd_ups_reading reading;
    if (!xd_ups_read_nut_finish(result, &reading))
        return; /* cancelled */
    service.is_reading = false;
    service.reading = reading;
    decide();
}

static gboolean on_poll([[maybe_unused]] gpointer data) {
    const char *address = nullptr;
    if (!service.is_reading &&
        xd_ups_get_device_kind(service.device, &address) == XD_UPS_DEVICE_NUT) {
        service.is_reading = true;
        xd_ups_read_nut_async(address, service.cancellable, on_nut_read, nullptr);
    }
    return G_SOURCE_CONTINUE;
}

static void stop_watching(void) {
    if (service.cancellable != nullptr) {
        g_cancellable_cancel(service.cancellable);
        g_clear_object(&service.cancellable);
    }
    if (service.upower_device != nullptr) {
        g_signal_handlers_disconnect_by_data(service.upower_device, &service);
        g_clear_object(&service.upower_device);
    }
    remove_timer(&service.poll_timer);
    remove_timer(&service.wake_timer);
    service.is_reading = false;
    xd_ups_reading_init(&service.reading);
    service.watch = (xd_ups_watch){0};
}

static void start_watching(void) {
    stop_watching();
    g_strlcpy(service.device, service.shell->settings.ups.device, sizeof(service.device));
    const char *address = nullptr;
    xd_ups_device_kind kind = xd_ups_get_device_kind(service.device, &address);
    if (kind == XD_UPS_DEVICE_NONE)
        return;
    service.cancellable = g_cancellable_new();
    if (kind == XD_UPS_DEVICE_UPOWER) {
        g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                 XD_UPOWER_NAME, address, XD_UPOWER_DEVICE_INTERFACE,
                                 service.cancellable, on_upower_device, nullptr);
    } else {
        on_poll(nullptr);
        service.poll_timer = g_timeout_add_seconds(NUT_POLL_SECONDS, on_poll, nullptr);
    }
}

void xd_ups_service_init(xd_shell *shell) {
    service.shell = shell;
    service.is_started = true;
    start_watching();
}

void xd_ups_service_update(xd_shell *shell) {
    if (!service.is_started || service.shell != shell)
        return;
    if (strcmp(service.device, shell->settings.ups.device) != 0)
        start_watching();
    else
        decide(); /* new delays or minutes */
}

void xd_ups_service_finish(xd_shell *shell) {
    if (!service.is_started || service.shell != shell)
        return;
    stop_watching();
    remove_timer(&service.program_timer);
    service = (ups_service){0};
}
