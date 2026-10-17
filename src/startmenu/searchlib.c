/* SPDX-License-Identifier: MIT */
#include "searchlib.h"

#include <string.h>

void xd_search_name_init(xd_search_name *n, const char *text) {
    *n = (xd_search_name){0};
    char *trimmed = g_strstrip(g_strdup(text != nullptr ? text : ""));
    if (trimmed[0] != '\0') {
        char *folded = g_utf8_casefold(trimmed, -1);
        if (strpbrk(folded, "*?") != nullptr) {
            n->glob = g_pattern_spec_new(folded);
            g_free(folded);
        } else {
            n->part = folded;
        }
    }
    g_free(trimmed);
}

void xd_search_name_clear(xd_search_name *n) {
    g_free(n->part);
    if (n->glob != nullptr)
        g_pattern_spec_free(n->glob);
    *n = (xd_search_name){0};
}

bool xd_search_name_matches(const xd_search_name *n, const char *name) {
    if (n->part == nullptr && n->glob == nullptr)
        return true;
    /* File names needn't be UTF-8: compare what the user would see. */
    char *display = g_filename_display_name(name);
    char *folded = g_utf8_casefold(display, -1);
    bool match = n->glob != nullptr ? g_pattern_spec_match_string(n->glob, folded)
                                    : strstr(folded, n->part) != nullptr;
    g_free(folded);
    g_free(display);
    return match;
}

bool xd_search_text_in(const char *data, size_t len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0)
        return true;
    if (n > len)
        return false;
    for (size_t i = 0; i + n <= len; i++) {
        if (g_ascii_tolower(data[i]) == g_ascii_tolower(needle[0]) &&
            g_ascii_strncasecmp(data + i, needle, n) == 0)
            return true;
    }
    return false;
}
