/* SPDX-License-Identifier: MIT
 * File name helpers shared by the folder view and the unit tests.
 */
#ifndef XD_FILES_NAMES_H
#define XD_FILES_NAMES_H

#include <stddef.h>
#include <string.h>

/* The length of `name` without its extension (the last ".xxx"), as
 * Windows hides it: "photo.jpg" is "photo", "archive.tar.gz" is
 * "archive.tar". Names without one, dot files (".bashrc") and names
 * ending in a dot keep their full length. */
static inline size_t xd_name_base_len(const char *name) {
    const char *dot = strrchr(name, '.');
    size_t len = strlen(name);
    if (dot == nullptr || dot == name || dot[1] == '\0')
        return len;
    return (size_t)(dot - name);
}

#endif
