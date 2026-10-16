/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "common.h"

bool xd_launch_argv(char *const argv[], xd_strbuf *err) {
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0)
        return false;
    sigset_t none;
    sigset_t all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                        POSIX_SPAWN_SETSID);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], nullptr, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0 && err != nullptr)
        xd_strbuf_appendf(err, "%s", strerror(rc));
    return rc == 0;
}

static void mkdirs(xd_strbuf *path) {
    /* mkdir -p on each prefix ending in '/'. */
    for (size_t i = 1; i < path->len; i++) {
        if (path->data[i] != '/')
            continue;
        path->data[i] = '\0';
        if (mkdir(path->data, 0700) < 0 && errno != EEXIST) {
            path->data[i] = '/';
            return;
        }
        path->data[i] = '/';
    }
}

void xd_state_path(const char *name, xd_strbuf *out) {
    xd_strbuf_clear(out);
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (state != nullptr && state[0] == '/')
        xd_strbuf_appendf(out, "%s/xeondesktop/", state);
    else
        xd_strbuf_appendf(out, "%s/.local/state/xeondesktop/", home != nullptr ? home : "/tmp");
    mkdirs(out);
    xd_strbuf_append(out, xd_str_from_cstr(name));
}

void xd_state_read_lines(const char *name, char ***lines, size_t *count, size_t max) {
    *lines = nullptr;
    *count = 0;
    xd_strbuf path = {0};
    xd_strbuf text = {0};
    xd_state_path(name, &path);
    if (xd_read_file(xd_strbuf_cstr(&path), 1u << 20, &text)) {
        *lines = xd_alloc_array(XD_MAX(max, 1), sizeof(char *));
        xd_str rest = xd_strbuf_view(&text);
        while (rest.len > 0 && *count < max) {
            size_t nl = rest.len;
            bool more = xd_str_find_char(rest, '\n', &nl);
            xd_str line = xd_str_trim(xd_str_sub(rest, 0, nl));
            if (line.len > 0)
                (*lines)[(*count)++] = xd_str_dup_cstr(line);
            rest = more ? xd_str_sub(rest, nl + 1, rest.len) : XD_STR("");
        }
    }
    xd_strbuf_free(&text);
    xd_strbuf_free(&path);
}

bool xd_state_write_lines(const char *name, char *const *lines, size_t count) {
    xd_strbuf path = {0};
    xd_strbuf tmp = {0};
    xd_state_path(name, &path);
    xd_strbuf_appendf(&tmp, "%s.tmp", xd_strbuf_cstr(&path));
    FILE *f = fopen(xd_strbuf_cstr(&tmp), "w");
    bool ok = f != nullptr;
    for (size_t i = 0; ok && i < count; i++)
        ok = fprintf(f, "%s\n", lines[i]) >= 0;
    if (f != nullptr)
        ok = fclose(f) == 0 && ok;
    if (ok)
        ok = rename(xd_strbuf_cstr(&tmp), xd_strbuf_cstr(&path)) == 0;
    else
        unlink(xd_strbuf_cstr(&tmp));
    xd_strbuf_free(&tmp);
    xd_strbuf_free(&path);
    return ok;
}

void xd_free_lines(char **lines, size_t count) {
    for (size_t i = 0; i < count; i++)
        xd_free(lines[i]);
    xd_free(lines);
}
