/* SPDX-License-Identifier: MIT
 * xd-cleanmgr-helper: cleans Disk Cleanup's system files as root, run by
 * pkexec (polkit's org.xeondesktop.cleanmgr.clean, an administrator's
 * password). It takes nothing from the caller but the categories' names:
 *
 *   xd-cleanmgr-helper clean packages|logs|crashes...
 *
 * Run as anyone else (the tests), XD_CLEANMGR_ROOT and XD_CLEANMGR_TOOLS
 * stand in for the system's paths and tools; as root they're ignored
 * (pkexec gives it a clean environment anyway).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "syscleanup.h"

int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "clean") != 0) {
        fprintf(stderr, "usage: xd-cleanmgr-helper clean packages|logs|crashes...\n");
        return 2;
    }
    XdCleanupKind kinds[XD_CLEANUP_KIND_COUNT];
    int count = 0;
    for (int i = 2; i < argc; i++) {
        XdCleanupKind kind;
        if (!xd_sysclean_kind_from_name(argv[i], &kind) || count >= XD_CLEANUP_KIND_COUNT) {
            fprintf(stderr, "xd-cleanmgr-helper: no such category: %s\n", argv[i]);
            return 2;
        }
        kinds[count++] = kind;
    }
    xd_sysclean_env env = {.root = "", .tools = nullptr, .now = g_get_real_time()};
    if (getuid() == 0) {
        umask(022);
        setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    } else {
        const char *root = getenv("XD_CLEANMGR_ROOT");
        env.root = root != nullptr ? root : "";
        env.tools = getenv("XD_CLEANMGR_TOOLS");
    }
    int status = 0;
    for (int i = 0; i < count; i++) {
        GError *err = nullptr;
        if (!xd_sysclean_clean(kinds[i], &env, &err)) {
            fprintf(stderr, "xd-cleanmgr-helper: cannot clean %s: %s\n",
                    xd_sysclean_name(kinds[i]), err != nullptr ? err->message : "failed");
            g_clear_error(&err);
            status = 1;
        }
    }
    return status;
}
