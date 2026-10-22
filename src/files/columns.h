/* SPDX-License-Identifier: MIT
 * The Details view's columns as a folder remembers them: which are shown,
 * in what order and how wide, written "id:width,id:width,..." (width 0:
 * the column's default). Shared by the folder view and the unit tests.
 */
#ifndef XD_FILES_COLUMNS_H
#define XD_FILES_COLUMNS_H

#include <glib.h>
#include <stdlib.h>

/* Reads `spec` into `order` (column ids, shown left to right; `count` of
 * them) and `width` (per id, 0 for the default; ids not listed keep 0).
 * Ids are below `ncols`; Name (id 0) is always shown, first if the text
 * left it out. FALSE for anything malformed, leaving the outputs alone. */
static inline gboolean xd_columns_parse(const char *spec, int ncols, int *order, int *count,
                                        int *width) {
    if (spec == nullptr || *spec == '\0' || ncols <= 0 || ncols > 64)
        return FALSE;
    int got[64];
    int widths[64] = {0};
    gboolean seen[64] = {FALSE};
    int parsed_count = 0;
    char **parts = g_strsplit(spec, ",", -1);
    gboolean ok = TRUE;
    for (char **part = parts; ok && *part != nullptr; part++) {
        char *end = nullptr;
        long id = strtol(*part, &end, 10);
        if (end == *part || *end != ':' || id < 0 || id >= ncols || seen[id]) {
            ok = FALSE;
            break;
        }
        char *wend = nullptr;
        long px = strtol(end + 1, &wend, 10);
        if (wend == end + 1 || *wend != '\0' || px < 0 || px > 10000) {
            ok = FALSE;
            break;
        }
        seen[id] = TRUE;
        widths[id] = (int)px;
        got[parsed_count++] = (int)id;
    }
    g_strfreev(parts);
    if (!ok || parsed_count == 0)
        return FALSE;
    int k = 0;
    if (!seen[0])
        order[k++] = 0;
    for (int i = 0; i < parsed_count; i++)
        order[k++] = got[i];
    *count = k;
    for (int i = 0; i < ncols; i++)
        width[i] = widths[i];
    return TRUE;
}

/* The text for `order` and `width`, as xd_columns_parse reads it. Free
 * with g_free. */
static inline char *xd_columns_format(const int *order, int count, const int *width) {
    GString *string = g_string_new(nullptr);
    for (int i = 0; i < count; i++)
        g_string_append_printf(string, "%s%d:%d", i > 0 ? "," : "", order[i], width[order[i]]);
    return g_string_free(string, FALSE);
}

#endif
