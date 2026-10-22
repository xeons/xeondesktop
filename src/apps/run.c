/* SPDX-License-Identifier: MIT
 * xd-run: the Run dialog (Start > Run...).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gdesktopappinfo.h>

#include "../core/alloc.h"
#include "../ui/ui.h"
#include "common.h"
#include "runcmd.h"

enum { XD_RUN_HISTORY_MAX = 26 }; /* Windows keeps a-z */

typedef struct run_dialog {
    xd_ui *ui;
    xd_window *win;
    xd_widget *combo;
    xd_widget *ok;
    xd_widget *browse;
    /* Browse...: xd-choose-file's answer, read from its stdout. */
    int chooser_fd;
    xd_ui_source *chooser_src;
    xd_strbuf chooser_out;
    char **history;
    size_t history_n;
} run_dialog;

static void message_ok(xd_widget *widget, [[maybe_unused]] void *user) {
    xd_window_close(xd_widget_window(widget));
}

/* Windows' "cannot find" message box. */
static void show_not_found(xd_ui *ui, const char *name) {
    xd_window *box = xd_window_create(ui, name, "xd-run", 380, 104);
    xd_strbuf msg = {0};
    xd_strbuf_appendf(&msg,
                      "XeonDesktop cannot find '%s'. Make sure you typed the name correctly, "
                      "and then try again.",
                      name);
    xd_image(box, (xd_rect){12, 14, 32, 32}, "dialog-error");
    xd_label(box, (xd_rect){56, 14, 312, 40}, xd_strbuf_cstr(&msg), XD_LABEL_WRAP);
    xd_widget *ok = xd_button(box, (xd_rect){152, 68, 75, 23}, "OK", message_ok, nullptr);
    xd_window_set_default(box, ok);
    xd_window_set_cancel(box, ok);
    xd_window_show(box);
    xd_strbuf_free(&msg);
}

static void remember(run_dialog *state, const char *cmd) {
    /* Most recent first, no duplicates, at most 26. */
    char **lines = xd_alloc_array(XD_RUN_HISTORY_MAX, sizeof(char *));
    size_t count = 0;
    lines[count++] = xd_str_dup_cstr(xd_str_from_cstr(cmd));
    for (size_t i = 0; i < state->history_n && count < XD_RUN_HISTORY_MAX; i++) {
        if (strcmp(state->history[i], cmd) != 0)
            lines[count++] = xd_str_dup_cstr(xd_str_from_cstr(state->history[i]));
    }
    if (!xd_state_write_lines("run-history", lines, count))
        fprintf(stderr, "xd-run: cannot save history\n");
    xd_free_lines(lines, count);
}

static bool launch(const xd_run_target *run_target, xd_strbuf *err) {
    char *target = xd_str_dup_cstr(xd_strbuf_view(&run_target->target));
    bool ok = false;
    switch (run_target->kind) {
    case XD_RUN_OPEN: {
        char prog[] = "xdg-open";
        char *argv[] = {prog, target, nullptr};
        ok = xd_launch_argv(argv, err);
        break;
    }
    case XD_RUN_EXEC: {
        char *argv[] = {target, nullptr};
        ok = xd_launch_argv(argv, err);
        break;
    }
    case XD_RUN_SHELL: {
        char sh[] = "/bin/sh";
        char flag[] = "-c";
        char *argv[] = {sh, flag, target, nullptr};
        ok = xd_launch_argv(argv, err);
        break;
    }
    case XD_RUN_EMPTY:
    case XD_RUN_NOT_FOUND:
        break;
    }
    xd_free(target);
    return ok;
}

/* In the child: a session of its own, as xd_launch_argv's programs. */
static void detach([[maybe_unused]] gpointer data) { setsid(); }

static bool launch_app(const char *id) {
    GDesktopAppInfo *info = g_desktop_app_info_new(id);
    if (info == nullptr)
        return false;
    GError *error = nullptr;
    bool ok = g_desktop_app_info_launch_uris_as_manager(info, nullptr, nullptr,
                                                        G_SPAWN_SEARCH_PATH, detach, nullptr,
                                                        nullptr, nullptr, &error);
    if (!ok) {
        fprintf(stderr, "xd-run: cannot start %s: %s\n", id, error->message);
        g_error_free(error);
    }
    g_object_unref(info);
    return ok;
}

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) {
    run_dialog *state = user;
    const char *text = xd_widget_text(state->combo);
    xd_run_target run_target = xd_run_classify(xd_str_from_cstr(text), nullptr, getenv("HOME"));
    if (run_target.kind == XD_RUN_EMPTY) {
        xd_run_target_free(&run_target);
        return;
    }
    if (run_target.kind == XD_RUN_NOT_FOUND) {
        /* A Windows name for one of ours ("msconfig"), as App Paths has. */
        char *id = xd_run_app_path(xd_str_from_cstr(text));
        if (id != nullptr && launch_app(id)) {
            char *cmd = xd_str_dup_cstr(xd_str_trim(xd_str_from_cstr(text)));
            remember(state, cmd);
            xd_free(cmd);
            xd_window_close(state->win);
        } else {
            show_not_found(state->ui, xd_strbuf_cstr(&run_target.target));
        }
        g_free(id);
        xd_run_target_free(&run_target);
        return;
    }
    xd_strbuf err = {0};
    if (launch(&run_target, &err)) {
        char *cmd = xd_str_dup_cstr(xd_str_trim(xd_str_from_cstr(text)));
        remember(state, cmd);
        xd_free(cmd);
        xd_window_close(state->win);
    } else {
        show_not_found(state->ui, xd_strbuf_cstr(&run_target.target));
    }
    xd_strbuf_free(&err);
    xd_run_target_free(&run_target);
}

static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) {
    run_dialog *state = user;
    xd_window_close(state->win);
}

static void on_change(xd_widget *combo, void *user);

/* The chosen file goes in the Open box, quoted if it has spaces, as
 * Windows does. */
static void chooser_output(void *user) {
    run_dialog *state = user;
    char buf[1024];
    ssize_t bytes_read = read(state->chooser_fd, buf, sizeof(buf));
    if (bytes_read > 0) {
        xd_strbuf_append(&state->chooser_out, (xd_str){buf, (size_t)bytes_read});
        return;
    }
    if (bytes_read < 0 && errno == EINTR)
        return;
    xd_ui_remove_source(state->chooser_src);
    state->chooser_src = nullptr;
    close(state->chooser_fd);
    state->chooser_fd = -1;
    xd_str path = xd_str_trim(xd_strbuf_view(&state->chooser_out));
    if (path.len > 0) {
        char *chosen_path = xd_str_dup_cstr(path);
        xd_strbuf text = {0};
        if (strchr(chosen_path, ' ') != nullptr)
            xd_strbuf_appendf(&text, "\"%s\"", chosen_path);
        else
            xd_strbuf_append(&text, path);
        xd_widget_set_text(state->combo, xd_strbuf_cstr(&text));
        xd_strbuf_free(&text);
        xd_free(chosen_path);
        on_change(state->combo, state);
    }
    xd_strbuf_clear(&state->chooser_out);
    xd_widget_set_enabled(state->browse, true);
}

static void on_browse([[maybe_unused]] xd_widget *widget, void *user) {
    run_dialog *state = user;
    if (state->chooser_src != nullptr)
        return;
    const char *home = getenv("HOME");
    const char *args[] = {"--programs", "--title", "Browse", "--folder",
                          home != nullptr ? home : "/", nullptr};
    int fd = -1;
    if (xd_spawn_own("xd-choose-file", args, &fd) == 0)
        return;
    state->chooser_fd = fd;
    state->chooser_src = xd_ui_add_fd(state->ui, fd, chooser_output, state);
    xd_widget_set_enabled(state->browse, false);
}

/* OK is only available when there's something to run, as in Windows. */
static void on_change(xd_widget *combo, void *user) {
    run_dialog *state = user;
    xd_str text = xd_str_trim(xd_str_from_cstr(xd_widget_text(combo)));
    xd_widget_set_enabled(state->ok, text.len > 0);
}

int main(int argc, char **argv) {
    run_dialog state = {0};
    state.ui = xd_ui_connect(argc, argv);
    if (state.ui == nullptr)
        return EXIT_FAILURE;
    xd_state_read_lines("run-history", &state.history, &state.history_n, XD_RUN_HISTORY_MAX);

    state.win = xd_window_create(state.ui, "Run", "xd-run", 347, 159);
    /* A "cannot find" box (and Browse...'s chooser) go with it. */
    xd_window_set_main(state.win);
    xd_image(state.win, (xd_rect){12, 18, 32, 32}, "system-run");
    xd_label(state.win, (xd_rect){56, 18, 279, 40},
             "Type the name of a program, folder, document, or Internet address, and "
             "XeonDesktop will open it for you.",
             XD_LABEL_WRAP);
    xd_label(state.win, (xd_rect){12, 74, 40, 16}, "Open:", 0);
    state.combo = xd_combo(state.win, (xd_rect){56, 71, 279, 21}, true, on_change, &state);
    for (size_t i = 0; i < state.history_n; i++)
        xd_combo_add(state.combo, state.history[i]);

    state.ok = xd_button(state.win, (xd_rect){98, 124, 75, 23}, "OK", on_ok, &state);
    xd_widget *cancel =
        xd_button(state.win, (xd_rect){179, 124, 75, 23}, "Cancel", on_cancel, &state);
    state.chooser_fd = -1;
    state.browse =
        xd_button(state.win, (xd_rect){260, 124, 75, 23}, "Browse...", on_browse, &state);

    /* Windows pre-fills the last command, selected so typing replaces it. */
    if (state.history_n > 0)
        xd_combo_select(state.combo, 0);
    on_change(state.combo, &state);
    xd_window_set_default(state.win, state.ok);
    xd_window_set_cancel(state.win, cancel);
    xd_window_set_focus(state.win, state.combo);
    xd_window_show(state.win);

    xd_ui_run(state.ui);
    xd_ui_destroy(state.ui);
    xd_free_lines(state.history, state.history_n);
    xd_strbuf_free(&state.chooser_out);
    return EXIT_SUCCESS;
}
