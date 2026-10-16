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
