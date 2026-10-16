/* SPDX-License-Identifier: MIT
 * Quick Launch: the small icons next to the Start button.
 *
 * Like Windows' Quick Launch folder, the toolbar shows the shortcuts in a
 * directory: ~/.local/share/xeondesktop/quicklaunch/. Entries are .desktop
 * files or symlinks to installed ones, ordered by file name (prefix them
 * with numbers to reorder). The directory is watched, so edits show up
 * immediately.
 */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "shell.h"

static void quicklaunch_dir(xd_strbuf *out) {
    xd_strbuf_clear(out);
    const char *data = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (data != nullptr && data[0] == '/')
        xd_strbuf_appendf(out, "%s/xeondesktop/quicklaunch", data);
    else
        xd_strbuf_appendf(out, "%s/.local/share/xeondesktop/quicklaunch",
                          home != nullptr ? home : "/tmp");
}

static bool mkdirs(xd_strbuf *path) {
    for (size_t i = 1; i <= path->len; i++) {
        if (i < path->len && path->data[i] != '/')
            continue;
        char saved = path->data[i];
        path->data[i] = '\0';
        bool ok = mkdir(path->data, 0700) == 0 || errno == EEXIST;
        path->data[i] = saved;
        if (!ok)
            return false;
    }
    return true;
}

/* Links `prefix-id` in the folder to the first installed entry in ids. */
static void link_first(const char *dir, const char *prefix, const char *const *ids, size_t n) {
    static const char *const app_dirs[] = {"/usr/local/share/applications",
                                           "/usr/share/applications"};
    xd_strbuf src = {0};
    xd_strbuf dst = {0};
    for (size_t i = 0; i < n; i++) {
        for (size_t d = 0; d < XD_ARRAY_LEN(app_dirs); d++) {
            xd_strbuf_clear(&src);
            xd_strbuf_appendf(&src, "%s/%s", app_dirs[d], ids[i]);
            if (access(xd_strbuf_cstr(&src), R_OK) != 0)
                continue;
            xd_strbuf_clear(&dst);
            xd_strbuf_appendf(&dst, "%s/%s-%s", dir, prefix, ids[i]);
            if (symlink(xd_strbuf_cstr(&src), xd_strbuf_cstr(&dst)) != 0)
                fprintf(stderr, "xd-shell: quick launch: cannot link %s\n", ids[i]);
            xd_strbuf_free(&src);
            xd_strbuf_free(&dst);
            return;
        }
    }
    xd_strbuf_free(&src);
    xd_strbuf_free(&dst);
}

/* First run: Show Desktop, a web browser and a terminal, as Windows ships
 * Show Desktop and Internet Explorer. Only when the folder doesn't exist,
 * so a deliberately emptied folder stays empty. */
static void create_defaults(xd_strbuf *dir) {
    if (!mkdirs(dir))
        return;
    xd_strbuf path = {0};
    xd_strbuf_appendf(&path, "%s/10-show-desktop.desktop", xd_strbuf_cstr(dir));
    FILE *f = fopen(xd_strbuf_cstr(&path), "w");
    if (f != nullptr) {
        fputs("[Desktop Entry]\nType=Application\nName=Show Desktop\nIcon=user-desktop\n"
              "Exec=true\nX-XeonDesktop-Action=show-desktop\n",
              f);
        fclose(f);
    }
    xd_strbuf_free(&path);
    static const char *const browsers[] = {"firefox.desktop", "org.mozilla.firefox.desktop",
                                           "chromium.desktop", "google-chrome.desktop",
                                           "brave-browser.desktop", "falkon.desktop"};
    static const char *const terminals[] = {"Alacritty.desktop", "foot.desktop",
                                            "org.gnome.Console.desktop", "org.kde.konsole.desktop",
                                            "qterminal.desktop", "xterm.desktop"};
    link_first(xd_strbuf_cstr(dir), "20", browsers, XD_ARRAY_LEN(browsers));
    link_first(xd_strbuf_cstr(dir), "30", terminals, XD_ARRAY_LEN(terminals));
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void release_items(xd_quicklaunch *ql) {
    for (size_t i = 0; i < ql->count; i++)
        xd_app_release(&ql->items[i]);
}

void xd_quicklaunch_reload(xd_shell *shell) {
    xd_quicklaunch *ql = &shell->quicklaunch;
    release_items(ql);
    xd_free(ql->items);
    xd_arena_destroy(&ql->arena);
    ql->items = nullptr;
    ql->count = 0;
    xd_arena_init(&ql->arena, 4096);

    xd_strbuf dir = {0};
    quicklaunch_dir(&dir);
    DIR *d = opendir(xd_strbuf_cstr(&dir));
    if (d == nullptr) {
        xd_strbuf_free(&dir);
        return;
    }
    char **names = nullptr;
    size_t n = 0;
    size_t cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        xd_str name = xd_str_from_cstr(de->d_name);
        if (name.len < 9 || name.ptr[0] == '.' ||
            !xd_str_eq(xd_str_sub(name, name.len - 8, 8), XD_STR(".desktop")))
            continue;
        if (n == cap) {
            cap = cap == 0 ? 16 : cap * 2;
            names = xd_realloc_array(names, cap, sizeof(*names));
        }
        names[n++] = xd_str_dup_cstr(name);
    }
    closedir(d);
    if (n > 1)
        qsort(names, n, sizeof(*names), compare_names);

    ql->items = xd_alloc_array(XD_MAX(n, 1), sizeof(*ql->items));
    xd_strbuf path = {0};
    for (size_t i = 0; i < n; i++) {
        xd_strbuf_clear(&path);
        xd_strbuf_appendf(&path, "%s/%s", xd_strbuf_cstr(&dir), names[i]);
        xd_app app;
        if (xd_app_from_file(xd_strbuf_cstr(&path), &ql->arena, &app))
            ql->items[ql->count++] = app;
        xd_free(names[i]);
    }
    xd_free(names);
    xd_strbuf_free(&path);
    xd_strbuf_free(&dir);
}

void xd_quicklaunch_init(xd_shell *shell) {
    xd_quicklaunch *ql = &shell->quicklaunch;
    xd_arena_init(&ql->arena, 4096);
    xd_strbuf dir = {0};
    quicklaunch_dir(&dir);
    struct stat st;
    if (stat(xd_strbuf_cstr(&dir), &st) != 0)
        create_defaults(&dir);

    ql->watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ql->watch_fd >= 0 &&
        inotify_add_watch(ql->watch_fd, xd_strbuf_cstr(&dir),
                          IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                              IN_CLOSE_WRITE) < 0) {
        close(ql->watch_fd);
        ql->watch_fd = -1;
    }
    xd_strbuf_free(&dir);
    xd_quicklaunch_reload(shell);
}

bool xd_quicklaunch_watch_changed(xd_shell *shell) {
    alignas(struct inotify_event) char buf[4096];
    bool changed = false;
    while (read(shell->quicklaunch.watch_fd, buf, sizeof(buf)) > 0)
        changed = true;
    return changed;
}

void xd_quicklaunch_finish(xd_shell *shell) {
    xd_quicklaunch *ql = &shell->quicklaunch;
    if (ql->watch_fd >= 0)
        close(ql->watch_fd);
    release_items(ql);
    xd_free(ql->items);
    xd_arena_destroy(&ql->arena);
    *ql = (xd_quicklaunch){.watch_fd = -1};
}

void xd_quicklaunch_activate(xd_shell *shell, size_t index) {
    xd_quicklaunch *ql = &shell->quicklaunch;
    if (index >= ql->count)
        return;
    const xd_app *app = &ql->items[index];
    if (app->action != nullptr) {
        if (strcmp(app->action, "show-desktop") == 0)
            xd_show_desktop_toggle(shell);
        return;
    }
    xd_app_launch(app);
}
