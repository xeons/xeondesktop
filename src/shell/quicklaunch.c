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
#include <gio/gio.h>
#include <stdio.h>
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
static void link_first(const char *dir, const char *prefix, const char *const *ids, size_t count) {
    static const char *const app_dirs[] = {"/usr/local/share/applications",
                                           "/usr/share/applications"};
    xd_strbuf src = {0};
    xd_strbuf dst = {0};
    for (size_t i = 0; i < count; i++) {
        for (size_t dir_index = 0; dir_index < XD_ARRAY_LEN(app_dirs); dir_index++) {
            xd_strbuf_clear(&src);
            xd_strbuf_appendf(&src, "%s/%s", app_dirs[dir_index], ids[i]);
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
    FILE *file = fopen(xd_strbuf_cstr(&path), "w");
    if (file != nullptr) {
        fputs("[Desktop Entry]\nType=Application\nName=Show Desktop\nIcon=user-desktop\n"
              "Exec=true\nX-XeonDesktop-Action=show-desktop\n",
              file);
        fclose(file);
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

static int compare_names(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static void release_items(xd_quicklaunch *ql) {
    for (size_t i = 0; i < ql->count; i++)
        xd_app_release(&ql->items[i]);
}

void xd_quicklaunch_reload(xd_shell *shell) {
    xd_quicklaunch *ql = &shell->quicklaunch;
    release_items(ql);
    xd_free(ql->items);
    xd_free(ql->files);
    xd_arena_destroy(&ql->arena);
    ql->items = nullptr;
    ql->files = nullptr;
    ql->count = 0;
    xd_arena_init(&ql->arena, 4096);

    xd_strbuf dir = {0};
    quicklaunch_dir(&dir);
    DIR *directory = opendir(xd_strbuf_cstr(&dir));
    if (directory == nullptr) {
        xd_strbuf_free(&dir);
        return;
    }
    char **names = nullptr;
    size_t count = 0;
    size_t cap = 0;
    struct dirent *de;
    while ((de = readdir(directory)) != nullptr) {
        xd_str name = xd_str_from_cstr(de->d_name);
        if (name.len < 9 || name.ptr[0] == '.' ||
            !xd_str_eq(xd_str_sub(name, name.len - 8, 8), XD_STR(".desktop")))
            continue;
        if (count == cap) {
            cap = cap == 0 ? 16 : cap * 2;
            names = xd_realloc_array(names, cap, sizeof(*names));
        }
        names[count++] = xd_str_dup_cstr(name);
    }
    closedir(directory);
    if (count > 1)
        qsort(names, count, sizeof(*names), compare_names);

    ql->items = xd_alloc_array(XD_MAX(count, 1), sizeof(*ql->items));
    ql->files = xd_alloc_array(XD_MAX(count, 1), sizeof(*ql->files));
    xd_strbuf path = {0};
    for (size_t i = 0; i < count; i++) {
        xd_strbuf_clear(&path);
        xd_strbuf_appendf(&path, "%s/%s", xd_strbuf_cstr(&dir), names[i]);
        xd_app app;
        if (xd_app_from_file(xd_strbuf_cstr(&path), &ql->arena, &app)) {
            ql->files[ql->count] = xd_str_arena_cstr(&ql->arena, xd_str_from_cstr(names[i]));
            ql->items[ql->count++] = app;
        }
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
    xd_free(ql->files);
    xd_arena_destroy(&ql->arena);
    *ql = (xd_quicklaunch){.watch_fd = -1};
}

void xd_quicklaunch_folder(xd_strbuf *out) { quicklaunch_dir(out); }

ssize_t xd_quicklaunch_find(const xd_shell *shell, const char *file) {
    const xd_quicklaunch *ql = &shell->quicklaunch;
    for (size_t i = 0; i < ql->count; i++) {
        if (strcmp(ql->files[i], file) == 0)
            return (ssize_t)i;
    }
    return -1;
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

/* ---- reordering and adding ------------------------------------------------------- */

/* A file name without its order prefix: "20-firefox.desktop" ->
 * "firefox.desktop". */
static const char *stem(const char *file) {
    const char *rest = file;
    while (*rest >= '0' && *rest <= '9')
        rest++;
    return rest != file && *rest == '-' ? rest + 1 : file;
}

/* Renames the folder's shortcuts to "10-", "20-", ... in `order` (file
 * names; nullptr entries skipped). Two passes, so no name is clobbered. */
static void renumber(const char *dir, const char *const *order, size_t count) {
    char **temps = xd_alloc_array(XD_MAX(count, 1), sizeof(*temps));
    for (size_t i = 0; i < count; i++) {
        temps[i] = nullptr;
        if (order[i] == nullptr)
            continue;
        char *from = g_build_filename(dir, order[i], nullptr);
        char *tmp = g_strdup_printf("%s/.xd-reorder-%zu", dir, i);
        if (rename(from, tmp) == 0)
            temps[i] = tmp;
        else
            g_free(tmp);
        g_free(from);
    }
    size_t k = 0;
    for (size_t i = 0; i < count; i++) {
        if (temps[i] == nullptr)
            continue;
        char *to = g_strdup_printf("%s/%02zu-%s", dir, ++k * 10, stem(order[i]));
        if (rename(temps[i], to) != 0)
            fprintf(stderr, "xd-shell: quick launch: cannot rename to %s\n", to);
        g_free(to);
        g_free(temps[i]);
    }
    xd_free(temps);
}

void xd_quicklaunch_move(xd_shell *shell, size_t from, size_t to) {
    xd_quicklaunch *ql = &shell->quicklaunch;
    if (from >= ql->count || to > ql->count || to == from || to == from + 1)
        return;
    const char **order = xd_alloc_array(ql->count, sizeof(*order));
    size_t count = 0;
    for (size_t i = 0; i <= ql->count; i++) {
        if (i == to)
            order[count++] = ql->files[from];
        if (i < ql->count && i != from)
            order[count++] = ql->files[i];
    }
    xd_strbuf dir = {0};
    quicklaunch_dir(&dir);
    renumber(xd_strbuf_cstr(&dir), order, count);
    xd_strbuf_free(&dir);
    xd_free(order);
}

/* A launcher for something that isn't an application: opens it with its
 * default program, with its type's icon. */
static bool write_launcher(const char *path, const char *target) {
    GFile *file = g_file_new_for_path(target);
    GFileInfo *info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_ICON,
        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    char *name = g_path_get_basename(target);
    char *icon = nullptr;
    if (info != nullptr) {
        g_free(name);
        name = g_strdup(g_file_info_get_display_name(info));
        GIcon *gi = g_file_info_get_icon(info);
        if (G_IS_THEMED_ICON(gi)) {
            const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(gi));
            if (names != nullptr && names[0] != nullptr)
                icon = g_strdup(names[0]);
        }
        g_object_unref(info);
    }
    g_object_unref(file);
    GKeyFile *kf = g_key_file_new();
    char *quoted = g_shell_quote(target);
    char *exec = g_strdup_printf("xdg-open %s", quoted);
    g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE,
                          G_KEY_FILE_DESKTOP_TYPE_APPLICATION);
    g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, name);
    g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, exec);
    g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON,
                          icon != nullptr ? icon : "text-x-generic");
    bool ok = g_key_file_save_to_file(kf, path, nullptr);
    g_key_file_free(kf);
    g_free(exec);
    g_free(quoted);
    g_free(icon);
    g_free(name);
    return ok;
}

bool xd_quicklaunch_add(xd_shell *shell, size_t at, const char *target) {
    xd_quicklaunch *ql = &shell->quicklaunch;
    xd_strbuf dir = {0};
    quicklaunch_dir(&dir);
    if (!mkdirs(&dir)) {
        xd_strbuf_free(&dir);
        return false;
    }
    /* Applications are linked; anything else gets a launcher. */
    char *base = g_path_get_basename(target);
    bool desktop = g_str_has_suffix(base, ".desktop");
    char *file = desktop ? g_strdup_printf("99-%s", base)
                         : g_strdup_printf("99-%s.desktop", base);
    char *path = g_build_filename(xd_strbuf_cstr(&dir), file, nullptr);
    bool ok = desktop ? symlink(target, path) == 0 : write_launcher(path, target);
    if (ok) {
        /* Put it where it was dropped. */
        at = XD_MIN(at, ql->count);
        const char **order = xd_alloc_array(ql->count + 1, sizeof(*order));
        size_t count = 0;
        for (size_t i = 0; i <= ql->count; i++) {
            if (i == at)
                order[count++] = file;
            if (i < ql->count)
                order[count++] = ql->files[i];
        }
        renumber(xd_strbuf_cstr(&dir), order, count);
        xd_free(order);
    } else {
        fprintf(stderr, "xd-shell: quick launch: cannot add %s\n", target);
    }
    g_free(path);
    g_free(file);
    g_free(base);
    xd_strbuf_free(&dir);
    return ok;
}
