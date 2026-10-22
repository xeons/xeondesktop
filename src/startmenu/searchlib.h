/* SPDX-License-Identifier: MIT
 * xd-search's matching, apart from its window (unit-tested).
 *
 * "Search for files or folders named" matches part of the name, ignoring
 * case, as Windows 2000 did; with * or ? in it, the whole name against
 * that wildcard pattern instead. "Containing text" finds the text in a
 * file's bytes, ignoring the case of ASCII letters.
 */
#ifndef XD_STARTMENU_SEARCHLIB_H
#define XD_STARTMENU_SEARCHLIB_H

#include <glib.h>
#include <stddef.h>

typedef struct xd_search_name {
    char *part;        /* casefolded; nullptr: anything matches */
    GPatternSpec *glob; /* casefolded wildcard pattern, or nullptr */
} xd_search_name;

void xd_search_name_init(xd_search_name *search_name, const char *text);
void xd_search_name_clear(xd_search_name *search_name);
bool xd_search_name_matches(const xd_search_name *search_name, const char *name);

/* Whether `data` contains `needle` (both bytes; ASCII letters match either
 * case). An empty needle is in everything. */
bool xd_search_text_in(const char *data, size_t len, const char *needle);

#endif
