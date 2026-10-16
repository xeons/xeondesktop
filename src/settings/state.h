/* SPDX-License-Identifier: MIT
 * Small per-user state files: Run history, last Shut Down choice, Start
 * menu launch counts. They live in $XDG_STATE_HOME/xeondesktop/.
 */
#ifndef XD_SETTINGS_STATE_H
#define XD_SETTINGS_STATE_H

#include "../core/str.h"

/* $XDG_STATE_HOME/xeondesktop/<name> (the directory is created on demand). */
void xd_state_path(const char *name, xd_strbuf *out);

/* Reads a state file into at most `max` non-empty lines. */
void xd_state_read_lines(const char *name, char ***lines, size_t *count, size_t max);
/* Atomically replaces a state file with lines. */
bool xd_state_write_lines(const char *name, char *const *lines, size_t count);
void xd_free_lines(char **lines, size_t count);

#endif
