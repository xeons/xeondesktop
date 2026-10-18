/* SPDX-License-Identifier: MIT
 * What the Run dialog does with what you typed.
 */
#ifndef XD_APPS_RUNCMD_H
#define XD_APPS_RUNCMD_H

#include "../core/str.h"

typedef enum xd_run_kind : uint8_t {
    XD_RUN_EMPTY,
    XD_RUN_OPEN,      /* folder, document or URL: hand to xdg-open */
    XD_RUN_EXEC,      /* an executable file given by path */
    XD_RUN_SHELL,     /* a command line whose program is in PATH */
    XD_RUN_NOT_FOUND, /* `target` holds the name that wasn't found */
} xd_run_kind;

typedef struct xd_run_target {
    xd_run_kind kind;
    xd_strbuf target; /* path/URL/command line, with ~ expanded */
} xd_run_target;

/* Classifies `input`. `path_env` is the PATH to search (nullptr = $PATH),
 * `home` replaces a leading ~. */
xd_run_target xd_run_classify(xd_str input, const char *path_env, const char *home);
void xd_run_target_free(xd_run_target *t);

/* Windows' App Paths: a program known by its Windows name ("msconfig",
 * "msconfig.exe", any case), from the installed desktop entries'
 * X-XeonDesktop-AppPath lists. Its desktop file id (free with g_free), or
 * nullptr. Only for a name on its own, which nothing in PATH answers. */
char *xd_run_app_path(xd_str input);

#endif
