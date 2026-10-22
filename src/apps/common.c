/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
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

pid_t xd_spawn_own(const char *program, const char *const *args, int *out_fd) {
    char self[4096];
    xd_strbuf path = {0};
    xd_strbuf_append(&path, xd_str_from_cstr(program));
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
        self[length] = '\0';
        char *slash = strrchr(self, '/');
        if (slash != nullptr) {
            slash[1] = '\0';
            xd_strbuf local = {0};
            xd_strbuf_appendf(&local, "%s%s", self, program);
            if (access(xd_strbuf_cstr(&local), X_OK) == 0) {
                xd_strbuf_clear(&path);
                xd_strbuf_append(&path, xd_strbuf_view(&local));
            }
            xd_strbuf_free(&local);
        }
    }
    char *argv[16];
    size_t argc = 0;
    const char *exe = xd_strbuf_cstr(&path);
    argv[argc++] = (char *)(uintptr_t)exe;
    for (size_t i = 0; args != nullptr && args[i] != nullptr && argc + 1 < XD_ARRAY_LEN(argv); i++)
        argv[argc++] = (char *)(uintptr_t)args[i];
    argv[argc] = nullptr;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    int fds[2] = {-1, -1};
    if (out_fd != nullptr) {
        if (pipe2(fds, O_CLOEXEC) < 0) {
            posix_spawn_file_actions_destroy(&fa);
            xd_strbuf_free(&path);
            return 0;
        }
        posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    }
    pid_t pid = 0;
    int err = posix_spawnp(&pid, argv[0], &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (out_fd != nullptr) {
        close(fds[1]);
        if (err == 0)
            *out_fd = fds[0];
        else
            close(fds[0]);
    }
    if (err != 0) {
        fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(err));
        pid = 0;
    }
    xd_strbuf_free(&path);
    return pid;
}
