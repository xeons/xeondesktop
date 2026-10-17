/* SPDX-License-Identifier: MIT
 * xd-run: the Run dialog (Start > Run...).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void message_ok(xd_widget *w, [[maybe_unused]] void *user) {
    xd_window_close(xd_widget_window(w));
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

static void remember(run_dialog *d, const char *cmd) {
    /* Most recent first, no duplicates, at most 26. */
    char **lines = xd_alloc_array(XD_RUN_HISTORY_MAX, sizeof(char *));
    size_t n = 0;
    lines[n++] = xd_str_dup_cstr(xd_str_from_cstr(cmd));
    for (size_t i = 0; i < d->history_n && n < XD_RUN_HISTORY_MAX; i++) {
        if (strcmp(d->history[i], cmd) != 0)
            lines[n++] = xd_str_dup_cstr(xd_str_from_cstr(d->history[i]));
    }
    if (!xd_state_write_lines("run-history", lines, n))
        fprintf(stderr, "xd-run: cannot save history\n");
    xd_free_lines(lines, n);
}

static bool launch(const xd_run_target *t, xd_strbuf *err) {
    char *target = xd_str_dup_cstr(xd_strbuf_view(&t->target));
    bool ok = false;
    switch (t->kind) {
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

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    run_dialog *d = user;
    const char *text = xd_widget_text(d->combo);
    xd_run_target t = xd_run_classify(xd_str_from_cstr(text), nullptr, getenv("HOME"));
    if (t.kind == XD_RUN_EMPTY) {
        xd_run_target_free(&t);
        return;
    }
    if (t.kind == XD_RUN_NOT_FOUND) {
        show_not_found(d->ui, xd_strbuf_cstr(&t.target));
        xd_run_target_free(&t);
        return;
    }
    xd_strbuf err = {0};
    if (launch(&t, &err)) {
        char *cmd = xd_str_dup_cstr(xd_str_trim(xd_str_from_cstr(text)));
        remember(d, cmd);
        xd_free(cmd);
        xd_window_close(d->win);
    } else {
        show_not_found(d->ui, xd_strbuf_cstr(&t.target));
    }
    xd_strbuf_free(&err);
    xd_run_target_free(&t);
}

static void on_cancel([[maybe_unused]] xd_widget *w, void *user) {
    run_dialog *d = user;
    xd_window_close(d->win);
}

static void on_change(xd_widget *combo, void *user);

/* The chosen file goes in the Open box, quoted if it has spaces, as
 * Windows does. */
static void chooser_output(void *user) {
    run_dialog *d = user;
    char buf[1024];
    ssize_t n = read(d->chooser_fd, buf, sizeof(buf));
    if (n > 0) {
        xd_strbuf_append(&d->chooser_out, (xd_str){buf, (size_t)n});
        return;
    }
    if (n < 0 && errno == EINTR)
        return;
    xd_ui_remove_source(d->chooser_src);
    d->chooser_src = nullptr;
    close(d->chooser_fd);
    d->chooser_fd = -1;
    xd_str path = xd_str_trim(xd_strbuf_view(&d->chooser_out));
    if (path.len > 0) {
        char *p = xd_str_dup_cstr(path);
        xd_strbuf text = {0};
        if (strchr(p, ' ') != nullptr)
            xd_strbuf_appendf(&text, "\"%s\"", p);
        else
            xd_strbuf_append(&text, path);
        xd_widget_set_text(d->combo, xd_strbuf_cstr(&text));
        xd_strbuf_free(&text);
        xd_free(p);
        on_change(d->combo, d);
    }
    xd_strbuf_clear(&d->chooser_out);
    xd_widget_set_enabled(d->browse, true);
}

static void on_browse([[maybe_unused]] xd_widget *w, void *user) {
    run_dialog *d = user;
    if (d->chooser_src != nullptr)
        return;
    const char *home = getenv("HOME");
    const char *args[] = {"--programs", "--title", "Browse", "--folder",
                          home != nullptr ? home : "/", nullptr};
    int fd = -1;
    if (xd_spawn_own("xd-choose-file", args, &fd) == 0)
        return;
    d->chooser_fd = fd;
    d->chooser_src = xd_ui_add_fd(d->ui, fd, chooser_output, d);
    xd_widget_set_enabled(d->browse, false);
}

/* OK is only available when there's something to run, as in Windows. */
static void on_change(xd_widget *combo, void *user) {
    run_dialog *d = user;
    xd_str text = xd_str_trim(xd_str_from_cstr(xd_widget_text(combo)));
    xd_widget_set_enabled(d->ok, text.len > 0);
}

int main(int argc, char **argv) {
    run_dialog d = {0};
    d.ui = xd_ui_connect(argc, argv);
    if (d.ui == nullptr)
        return EXIT_FAILURE;
    xd_state_read_lines("run-history", &d.history, &d.history_n, XD_RUN_HISTORY_MAX);

    d.win = xd_window_create(d.ui, "Run", "xd-run", 347, 159);
    xd_image(d.win, (xd_rect){12, 18, 32, 32}, "system-run");
    xd_label(d.win, (xd_rect){56, 18, 279, 40},
             "Type the name of a program, folder, document, or Internet address, and "
             "XeonDesktop will open it for you.",
             XD_LABEL_WRAP);
    xd_label(d.win, (xd_rect){12, 74, 40, 16}, "Open:", 0);
    d.combo = xd_combo(d.win, (xd_rect){56, 71, 279, 21}, true, on_change, &d);
    for (size_t i = 0; i < d.history_n; i++)
        xd_combo_add(d.combo, d.history[i]);

    d.ok = xd_button(d.win, (xd_rect){98, 124, 75, 23}, "OK", on_ok, &d);
    xd_widget *cancel = xd_button(d.win, (xd_rect){179, 124, 75, 23}, "Cancel", on_cancel, &d);
    d.chooser_fd = -1;
    d.browse = xd_button(d.win, (xd_rect){260, 124, 75, 23}, "Browse...", on_browse, &d);

    /* Windows pre-fills the last command, selected so typing replaces it. */
    if (d.history_n > 0)
        xd_combo_select(d.combo, 0);
    on_change(d.combo, &d);
    xd_window_set_default(d.win, d.ok);
    xd_window_set_cancel(d.win, cancel);
    xd_window_set_focus(d.win, d.combo);
    xd_window_show(d.win);

    xd_ui_run(d.ui);
    xd_ui_destroy(d.ui);
    xd_free_lines(d.history, d.history_n);
    xd_strbuf_free(&d.chooser_out);
    return EXIT_SUCCESS;
}
