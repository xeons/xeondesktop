/* SPDX-License-Identifier: MIT
 * Length-carrying string slices and a growable string buffer.
 *
 * xd_str never owns memory and is not necessarily NUL-terminated. Convert
 * with xd_str_dup_cstr / xd_strbuf when a C string is required.
 */
#ifndef XD_CORE_STR_H
#define XD_CORE_STR_H

#include <stdarg.h>

#include "alloc.h"

typedef struct xd_str {
    const char *ptr;
    size_t len;
} xd_str;

#define XD_STR(lit) ((xd_str){.ptr = "" lit, .len = sizeof(lit) - 1})

xd_str xd_str_from_cstr(const char *s);
bool xd_str_eq(xd_str a, xd_str b);
bool xd_str_eq_nocase(xd_str a, xd_str b);
bool xd_str_starts_with(xd_str s, xd_str prefix);
xd_str xd_str_trim(xd_str s);
/* Returns s[start, start+len) clamped to s. */
xd_str xd_str_sub(xd_str s, size_t start, size_t len);
/* Finds c; returns true and sets *index on success. */
bool xd_str_find_char(xd_str s, char c, size_t *index);

/* Parses a base-10 integer in [min, max]. Rejects trailing garbage. */
[[nodiscard]] bool xd_str_parse_i64(xd_str s, int64_t min, int64_t max, int64_t *out);
[[nodiscard]] bool xd_str_parse_double(xd_str s, double *out);

[[nodiscard]] char *xd_str_dup_cstr(xd_str s);
[[nodiscard]] char *xd_str_arena_cstr(xd_arena *arena, xd_str s);

typedef struct xd_strbuf {
    char *data; /* always NUL-terminated once anything is appended */
    size_t len;
    size_t cap;
} xd_strbuf;

void xd_strbuf_append(xd_strbuf *sb, xd_str s);
void xd_strbuf_append_char(xd_strbuf *sb, char c);
XD_PRINTF(2, 3) void xd_strbuf_appendf(xd_strbuf *sb, const char *fmt, ...);
XD_PRINTF(2, 0) void xd_strbuf_vappendf(xd_strbuf *sb, const char *fmt, va_list ap);
xd_str xd_strbuf_view(const xd_strbuf *sb);
const char *xd_strbuf_cstr(xd_strbuf *sb);
void xd_strbuf_clear(xd_strbuf *sb);
void xd_strbuf_free(xd_strbuf *sb);

/* Reads a whole file. Returns false (and leaves *out empty) on error. */
[[nodiscard]] bool xd_read_file(const char *path, size_t max_size, xd_strbuf *out);

#endif
