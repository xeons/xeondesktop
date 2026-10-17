/* SPDX-License-Identifier: MIT
 * Helpers shared by the small session programs (xd-run, xd-shutdown).
 */
#ifndef XD_APPS_COMMON_H
#define XD_APPS_COMMON_H

#include <sys/types.h>

#include "../core/str.h"
#include "../settings/state.h"

/* Starts argv[0] (searched in PATH) detached from us: own session, default
 * signal handling. Returns false and fills *err (if given) on failure. */
[[nodiscard]] bool xd_launch_argv(char *const argv[], xd_strbuf *err);

/* Runs one of our programs (next to this one, else from PATH) with `args`
 * (nullptr-terminated); with out_fd, its stdout comes back through a
 * pipe. Returns the pid, or 0. */
pid_t xd_spawn_own(const char *program, const char *const *args, int *out_fd);

#endif
