/* SPDX-License-Identifier: MIT
 * Disk Cleanup's system files, which only an administrator may delete:
 * the package manager's cache (pacman's, apt's or dnf's), old system logs
 * (the systemd journal's archived files over two weeks old) and crash
 * dumps (systemd-coredump's). Measured as the user, since all of it can
 * be read; cleaned by xd-cleanmgr-helper, which pkexec runs as root, with
 * the system's own tools where it has them (pacman -Sc, apt-get clean,
 * dnf clean packages, journalctl --vacuum-time), and crash dumps deleted
 * directly, never through a link.
 */
#ifndef XD_FILES_SYSCLEANUP_H
#define XD_FILES_SYSCLEANUP_H

#include <gio/gio.h>

#include "cleanup.h"

/* Where the system files are. Tests point both at scratch folders; the
 * helper running as root uses the real ones. */
typedef struct xd_sysclean_env {
    const char *root;  /* prefix for the system's paths: "" for / */
    const char *tools; /* the folder pacman, apt-get, dnf and journalctl are in; nullptr:
                          /usr/bin, /usr/sbin, /bin, /sbin */
    gint64 now;        /* µs (g_get_real_time): what's two weeks old */
} xd_sysclean_env;

/* The category of system files of the kind (XD_CLEANUP_PACKAGES, _LOGS
 * or _CRASHES), with its size and View Files' folder; nullptr when
 * there's nothing to free (or no package manager this knows). */
XdCleanupCategory *xd_sysclean_scan(XdCleanupKind kind, const xd_sysclean_env *env,
                                    GCancellable *cancel);

/* Cleans the kind's files, as root. FALSE with `err` set on failure. */
bool xd_sysclean_clean(XdCleanupKind kind, const xd_sysclean_env *env, GError **err);

/* "packages", "logs", "crashes": the kind's name on the helper's command
 * line, and back. */
const char *xd_sysclean_name(XdCleanupKind kind);
bool xd_sysclean_kind_from_name(const char *name, XdCleanupKind *out);

#endif
