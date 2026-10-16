/* SPDX-License-Identifier: MIT
 * Helpers shared by the small session programs (xd-run, xd-shutdown).
 */
#ifndef XD_APPS_COMMON_H
#define XD_APPS_COMMON_H

#include <sys/types.h>

#include "../core/str.h"

/* Starts argv[0] (searched in PATH) detached from us: own session, default
 * signal handling. Returns false and fills *err (if given) on failure. */
[[nodiscard]] bool xd_launch_argv(char *const argv[], xd_strbuf *err);

/* $XDG_STATE_HOME/xeondesktop/<name> (created on demand). */
void xd_state_path(const char *name, xd_strbuf *out);

/* Reads a state file into lines (newest first for histories). */
void xd_state_read_lines(const char *name, char ***lines, size_t *count, size_t max);
/* Atomically replaces a state file with lines. */
bool xd_state_write_lines(const char *name, char *const *lines, size_t count);
void xd_free_lines(char **lines, size_t count);

#endif
