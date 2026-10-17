/* SPDX-License-Identifier: MIT
 * xd-shutdown: the Shut Down dialog, and with --logoff the Log Off
 * confirmation.
 *
 * Power actions go through systemctl (logind decides via polkit whether
 * the user may). In a nested development session they are only printed,
 * so a test click can't power off the host.
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../ui/ui.h"
#include "common.h"

typedef enum action : uint8_t {
    ACT_LOGOFF,
    ACT_SHUTDOWN,
    ACT_RESTART,
    ACT_STANDBY,
    ACT_HIBERNATE,
} action;

static const struct {
    const char *key; /* saved as the last choice */
    const char *label;
    const char *description;
    const char *systemctl;
} actions[] = {
    [ACT_LOGOFF] = {"logoff", "Log off %s",
                    "Ends your session. The computer keeps running so that someone else can "
                    "log on.",
                    nullptr},
    [ACT_SHUTDOWN] = {"shutdown", "Shut down",
                      "Ends your session and shuts down the computer so that you can safely "
                      "turn off the power.",
                      "poweroff"},
    [ACT_RESTART] = {"restart", "Restart",
                     "Ends your session, shuts down the computer, and starts it again.",
                     "reboot"},
    [ACT_STANDBY] = {"standby", "Stand by",
                     "Keeps your session in memory and puts the computer in a low-power state. "
                     "Move the mouse or press a key to wake it.",
                     "suspend"},
    [ACT_HIBERNATE] = {"hibernate", "Hibernate",
                       "Saves your session to disk and turns off the computer. Your session is "
                       "restored the next time you start it.",
                       "hibernate"},
};

typedef struct dialog {
    xd_ui *ui;
    xd_window *win;
    xd_widget *combo;
    xd_widget *description;
    action items[5]; /* combo index -> action */
    size_t count;
    bool simulate;
} dialog;

/* Is the session leader really our compositor? Guards against a stale or
 * forged XEONDESKTOP_PID making Log Off kill something else. */
static pid_t compositor_pid(void) {
    const char *env = getenv("XEONDESKTOP_PID");
    int64_t pid;
    if (env == nullptr || !xd_str_parse_i64(xd_str_from_cstr(env), 2, INT32_MAX, &pid))
        return 0;
    xd_strbuf path = {0};
    xd_strbuf comm = {0};
    xd_strbuf_appendf(&path, "/proc/%lld/comm", (long long)pid);
    bool ok = false;
    if (xd_read_file(xd_strbuf_cstr(&path), 256, &comm)) {
        /* xd-compositor, or xd-compositor-fx (the GPU build it hands over
         * to), which the kernel cuts to 15 characters: "xd-compositor-f". */
        xd_str name = xd_str_trim(xd_strbuf_view(&comm));
        ok = xd_str_eq(name, XD_STR("xd-compositor")) || xd_str_eq(name, XD_STR("xd-compositor-f"));
    }
    xd_strbuf_free(&comm);
    xd_strbuf_free(&path);
    return ok ? (pid_t)pid : 0;
}

static bool perform(action a, bool simulate) {
    if (a == ACT_LOGOFF) {
        pid_t pid = compositor_pid();
        if (pid == 0) {
            fprintf(stderr, "xd-shutdown: not running inside an XeonDesktop session\n");
            return false;
        }
        return kill(pid, SIGTERM) == 0;
    }
    if (simulate) {
        fprintf(stderr, "xd-shutdown: nested session, not running: systemctl %s\n",
                actions[a].systemctl);
        return true;
    }
    char prog[] = "systemctl";
    char *verb = xd_str_dup_cstr(xd_str_from_cstr(actions[a].systemctl));
    char *argv[] = {prog, verb, nullptr};
    xd_strbuf err = {0};
    bool ok = xd_launch_argv(argv, &err);
    if (!ok)
        fprintf(stderr, "xd-shutdown: systemctl %s: %s\n", verb, xd_strbuf_cstr(&err));
    xd_strbuf_free(&err);
    xd_free(verb);
    return ok;
}

/* Which sleep states the kernel offers ("freeze mem disk"). */
static void sleep_states(bool *standby, bool *hibernate) {
    xd_strbuf s = {0};
    *standby = false;
    *hibernate = false;
    if (xd_read_file("/sys/power/state", 4096, &s)) {
        xd_str rest = xd_str_trim(xd_strbuf_view(&s));
        while (rest.len > 0) {
            size_t sp = rest.len;
            bool more = xd_str_find_char(rest, ' ', &sp);
            xd_str word = xd_str_sub(rest, 0, sp);
            *standby = *standby || xd_str_eq(word, XD_STR("mem"));
            *hibernate = *hibernate || xd_str_eq(word, XD_STR("disk"));
            rest = more ? xd_str_sub(rest, sp + 1, rest.len) : XD_STR("");
        }
    }
    xd_strbuf_free(&s);
}

static void on_change(xd_widget *combo, void *user) {
    dialog *d = user;
    int i = xd_combo_selected(combo);
    if (i >= 0 && (size_t)i < d->count)
        xd_widget_set_text(d->description, actions[d->items[i]].description);
}

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    dialog *d = user;
    int i = xd_combo_selected(d->combo);
    if (i < 0 || (size_t)i >= d->count)
        return;
    action a = d->items[i];
    char *key = xd_str_dup_cstr(xd_str_from_cstr(actions[a].key));
    if (!xd_state_write_lines("shutdown-choice", &key, 1))
        fprintf(stderr, "xd-shutdown: cannot save the last choice\n");
    xd_free(key);
    xd_window_close(d->win);
    perform(a, d->simulate);
}

static void on_cancel([[maybe_unused]] xd_widget *w, void *user) {
    dialog *d = user;
    xd_window_close(d->win);
}

static void build_shutdown(dialog *d) {
    d->win = xd_window_create(d->ui, "Shut Down XeonDesktop", "xd-shutdown", 412, 208);
    xd_banner(d->win, (xd_rect){0, 0, 412, 72}, "XeonDesktop", "Professional");
    xd_image(d->win, (xd_rect){12, 86, 32, 32}, "system-shutdown");
    xd_label(d->win, (xd_rect){56, 86, 344, 16}, "What do you want the computer to do?", 0);
    d->combo = xd_combo(d->win, (xd_rect){56, 106, 220, 21}, false, on_change, d);
    d->description = xd_label(d->win, (xd_rect){56, 134, 344, 32}, "", XD_LABEL_WRAP);

    bool standby;
    bool hibernate;
    sleep_states(&standby, &hibernate);
    const char *user = getenv("USER");
    for (size_t a = 0; a < XD_ARRAY_LEN(actions); a++) {
        if ((a == ACT_STANDBY && !standby) || (a == ACT_HIBERNATE && !hibernate))
            continue;
        xd_strbuf label = {0};
        if (a == ACT_LOGOFF)
            xd_strbuf_appendf(&label, "Log off %s", user != nullptr ? user : "");
        else
            xd_strbuf_append(&label, xd_str_from_cstr(actions[a].label));
        xd_combo_add(d->combo, xd_strbuf_cstr(&label));
        xd_strbuf_free(&label);
        d->items[d->count++] = (action)a;
    }

    /* Start on the last choice, like Windows; Shut down otherwise. */
    char **last = nullptr;
    size_t n = 0;
    xd_state_read_lines("shutdown-choice", &last, &n, 1);
    int pick = 0;
    for (size_t i = 0; i < d->count; i++) {
        if (n > 0 ? strcmp(actions[d->items[i]].key, last[0]) == 0 : d->items[i] == ACT_SHUTDOWN)
            pick = (int)i;
    }
    xd_free_lines(last, n);
    xd_combo_select(d->combo, pick);
    on_change(d->combo, d);

    if (d->simulate)
        xd_label(d->win, (xd_rect){12, 177, 140, 16}, "Test session: simulated",
                 XD_LABEL_DIM);
    xd_widget *ok = xd_button(d->win, (xd_rect){163, 173, 75, 23}, "OK", on_ok, d);
    xd_widget *cancel = xd_button(d->win, (xd_rect){244, 173, 75, 23}, "Cancel", on_cancel, d);
    xd_widget *help = xd_button(d->win, (xd_rect){325, 173, 75, 23}, "Help", nullptr, nullptr);
    xd_widget_set_enabled(help, false);
    xd_window_set_default(d->win, ok);
    xd_window_set_cancel(d->win, cancel);
    xd_window_set_focus(d->win, d->combo);
}

static void on_logoff_yes([[maybe_unused]] xd_widget *w, void *user) {
    dialog *d = user;
    xd_window_close(d->win);
    perform(ACT_LOGOFF, d->simulate);
}

/* Windows 2000's "Are you sure you want to log off?" box. */
static void build_logoff(dialog *d) {
    d->win = xd_window_create(d->ui, "Log Off XeonDesktop", "xd-shutdown", 300, 100);
    xd_image(d->win, (xd_rect){12, 14, 32, 32}, "system-log-out");
    xd_label(d->win, (xd_rect){56, 22, 232, 16}, "Are you sure you want to log off?", 0);
    xd_widget *yes = xd_button(d->win, (xd_rect){72, 62, 75, 23}, "Yes", on_logoff_yes, d);
    xd_widget *no = xd_button(d->win, (xd_rect){153, 62, 75, 23}, "No", on_cancel, d);
    xd_window_set_default(d->win, yes);
    xd_window_set_cancel(d->win, no);
    xd_window_set_focus(d->win, yes);
}

int main(int argc, char **argv) {
    bool logoff = false;
    bool dry_run = false;
    const char *direct = nullptr; /* --action: act without a dialog */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--logoff") == 0)
            logoff = true;
        else if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = true;
        else if (strcmp(argv[i], "--action") == 0 && i + 1 < argc)
            direct = argv[++i];
    }

    dialog d = {0};
    const char *nested = getenv("XEONDESKTOP_NESTED");
    d.simulate = dry_run || nested == nullptr || strcmp(nested, "0") != 0;

    /* Windows 7's Shut Down button acts at once; the same safety rules
     * (nested sessions only simulate) apply. */
    if (direct != nullptr) {
        for (size_t a = 0; a < XD_ARRAY_LEN(actions); a++) {
            if (strcmp(actions[a].key, direct) == 0)
                return perform((action)a, d.simulate) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        fprintf(stderr, "xd-shutdown: unknown action %s\n", direct);
        return EXIT_FAILURE;
    }
    d.ui = xd_ui_connect(argc, argv);
    if (d.ui == nullptr)
        return EXIT_FAILURE;
    if (logoff)
        build_logoff(&d);
    else
        build_shutdown(&d);
    xd_window_show(d.win);
    xd_ui_run(d.ui);
    xd_ui_destroy(d.ui);
    return EXIT_SUCCESS;
}
