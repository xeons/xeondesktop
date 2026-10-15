/* SPDX-License-Identifier: MIT
 * INI parser used for themes, color schemes and settings.
 *
 * Syntax: [Section], Key=Value, comments start with ';' or '#'.
 * The parser never allocates; slices passed to the callback point into the
 * caller's buffer.
 */
#ifndef XD_CORE_INI_H
#define XD_CORE_INI_H

#include "str.h"

/* Return false to stop parsing. */
typedef bool (*xd_ini_handler)(void *user, xd_str section, xd_str key, xd_str value);

typedef struct xd_ini_error {
    size_t line; /* 1-based, 0 when there is no error */
    const char *message;
} xd_ini_error;

/* Returns true when the whole input parsed and the handler never stopped. */
[[nodiscard]] bool xd_ini_parse(xd_str text, xd_ini_handler handler, void *user,
                                xd_ini_error *err);

#endif
