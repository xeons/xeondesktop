/* SPDX-License-Identifier: MIT
 * xd-message [--quiet] [--error|--warning|--info] TITLE TEXT: a Windows
 * message box with an OK button, for programs without dialogs of their own
 * (the shell: "Error Renaming File or Folder", the battery alarms).
 * --quiet: no sound (the battery alarms have their own, if any).
 */
#include <stdlib.h>
#include <string.h>

#include "../settings/soundplay.h"
#include "../ui/ui.h"

static constexpr int TEXT_W = 290;
static constexpr int LINE_H = 14;
static constexpr double CHAR_W = 5.2; /* an 8pt sans-serif's average, for wrapping */

/* Lines the text wraps to in TEXT_W (word by word, as the label does);
 * libxdui can't measure, and these messages are short. */
static int wrapped_lines(const char *text) {
    int lines = 1;
    double x = 0;
    const char *p = text;
    while (*p != '\0') {
        if (*p == '\n') {
            lines++;
            x = 0;
            p++;
            continue;
        }
        size_t word = strcspn(p, " \n");
        double w = (double)word * CHAR_W;
        if (x > 0 && x + w > TEXT_W) {
            lines++;
            x = 0;
        }
        x += w + CHAR_W;
        p += word;
        if (*p == ' ')
            p++;
    }
    return lines;
}

static void on_ok([[maybe_unused]] xd_widget *w, void *user) { xd_window_close(user); }

int main(int argc, char **argv) {
    const char *icon = "dialog-information";
    xd_sound_event sound = XD_SOUND_ASTERISK; /* each icon's, as in Windows */
    int first = 1;
    bool quiet = argc > first && strcmp(argv[first], "--quiet") == 0;
    if (quiet)
        first++;
    if (argc > first && strcmp(argv[first], "--error") == 0) {
        icon = "dialog-error";
        sound = XD_SOUND_CRITICAL_STOP;
        first++;
    } else if (argc > first && strcmp(argv[first], "--warning") == 0) {
        icon = "dialog-warning";
        sound = XD_SOUND_EXCLAMATION;
        first++;
    } else if (argc > first && strcmp(argv[first], "--info") == 0) {
        first++;
    }
    if (argc - first != 2) {
        fprintf(stderr, "usage: xd-message [--quiet] [--error|--warning|--info] TITLE TEXT\n");
        return EXIT_FAILURE;
    }
    const char *title = argv[first];
    const char *text = argv[first + 1];

    xd_ui *ui = xd_ui_connect(argc, argv);
    if (ui == nullptr)
        return EXIT_FAILURE;
    /* A little spare: the estimate may be short. */
    int text_h = wrapped_lines(text) * LINE_H + LINE_H / 2;
    int body_h = text_h > 32 ? text_h : 32;
    int h = 14 + body_h + 16 + 23 + 12;
    int w = 56 + TEXT_W + 14;
    xd_window *win = xd_window_create(ui, title, "xd-message", w, h);
    xd_image(win, (xd_rect){12, 14, 32, 32}, icon);
    xd_label(win, (xd_rect){56, body_h > 32 ? 14 : 22, TEXT_W, text_h}, text, XD_LABEL_WRAP);
    xd_widget *ok = xd_button(win, (xd_rect){(w - 75) / 2, h - 12 - 23, 75, 23}, "OK", on_ok, win);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, ok);
    xd_window_set_focus(win, ok);
    xd_window_show(win);
    if (!quiet)
        xd_sound_play_event(sound);
    xd_ui_run(ui);
    xd_ui_destroy(ui);
    return EXIT_SUCCESS;
}
