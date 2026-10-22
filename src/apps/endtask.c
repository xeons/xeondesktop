/* SPDX-License-Identifier: MIT
 * xd-endtask PID TITLE: Windows' "End Program" box, which the compositor
 * shows when you close a window that isn't responding. End Now ends the
 * program (SIGKILL, as TerminateProcess does); Cancel leaves it be. The
 * box goes away by itself if the program ends meanwhile (it was only slow
 * to close).
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/pidfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../ui/ui.h"

typedef struct end_dialog {
    xd_window *win;
    pid_t pid;
    uid_t uid; /* the program's owner when we started: a reused PID won't match */
    xd_ui_source *exited; /* its pidfd turning readable */
} end_dialog;

/* Once only: the program can end in the same turn as a click. */
static void close_box(end_dialog *state) {
    if (state->exited != nullptr)
        xd_ui_remove_source(state->exited);
    state->exited = nullptr;
    if (state->win != nullptr)
        xd_window_close(state->win);
    state->win = nullptr;
}

static bool same_process(const end_dialog *state) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", (int)state->pid);
    struct stat st;
    return stat(path, &st) == 0 && st.st_uid == state->uid;
}

static void on_end_now([[maybe_unused]] xd_widget *widget, void *user) {
    end_dialog *state = user;
    if (same_process(state) && kill(state->pid, SIGKILL) != 0)
        fprintf(stderr, "xd-endtask: cannot end %d: %s\n", (int)state->pid, strerror(errno));
    close_box(state);
}

static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) { close_box(user); }

static void on_exited(void *user) { close_box(user); }

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: xd-endtask PID TITLE\n");
        return EXIT_FAILURE;
    }
    char *end = nullptr;
    long pid = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || pid <= 1) {
        fprintf(stderr, "xd-endtask: bad process id '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }
    end_dialog state = {.pid = (pid_t)pid, .uid = getuid()};
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld", pid);
    struct stat st;
    if (stat(path, &st) != 0)
        return EXIT_SUCCESS; /* it's gone already */
    state.uid = st.st_uid;

    xd_ui *ui = xd_ui_connect(argc, argv);
    if (ui == nullptr)
        return EXIT_FAILURE;
    xd_strbuf title = {0};
    xd_strbuf_appendf(&title, "End Program - %s", argv[2]);
    state.win = xd_window_create(ui, xd_strbuf_cstr(&title), "xd-endtask", 400, 176);
    xd_image(state.win, (xd_rect){12, 14, 32, 32}, "dialog-warning");
    xd_label(state.win, (xd_rect){56, 14, 332, 16}, "This program is not responding.", 0);
    xd_label(state.win, (xd_rect){56, 38, 332, 32},
             "To return to XeonDesktop and check the status of the program, click Cancel.",
             XD_LABEL_WRAP);
    xd_label(state.win, (xd_rect){56, 76, 332, 46},
             "If you choose to end the program immediately, you will lose any unsaved data. "
             "To end the program now, click End Now.",
             XD_LABEL_WRAP);
    xd_button(state.win, (xd_rect){232, 140, 75, 23}, "End Now", on_end_now, &state);
    xd_widget *cancel =
        xd_button(state.win, (xd_rect){313, 140, 75, 23}, "Cancel", on_cancel, &state);
    xd_window_set_default(state.win, cancel);
    xd_window_set_cancel(state.win, cancel);
    xd_window_set_focus(state.win, cancel);
    /* Followed by a pidfd, which a reused PID can't fool. */
    int pidfd = pidfd_open(state.pid, PIDFD_NONBLOCK);
    if (pidfd >= 0)
        state.exited = xd_ui_add_fd(ui, pidfd, on_exited, &state);
    xd_window_show(state.win);
    xd_ui_run(ui);
    if (state.exited != nullptr)
        xd_ui_remove_source(state.exited);
    if (pidfd >= 0)
        close(pidfd);
    xd_ui_destroy(ui);
    xd_strbuf_free(&title);
    return EXIT_SUCCESS;
}
