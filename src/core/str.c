/* SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#include "str.h"

xd_str xd_str_from_cstr(const char *s) {
    if (s == nullptr)
        return (xd_str){.ptr = "", .len = 0};
    return (xd_str){.ptr = s, .len = strlen(s)};
}

bool xd_str_eq(xd_str a, xd_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool xd_str_eq_nocase(xd_str a, xd_str b) {
    if (a.len != b.len)
        return false;
    for (size_t i = 0; i < a.len; i++) {
        if (ascii_lower(a.ptr[i]) != ascii_lower(b.ptr[i]))
            return false;
    }
    return true;
}

bool xd_str_starts_with(xd_str s, xd_str prefix) {
    return s.len >= prefix.len && xd_str_eq(xd_str_sub(s, 0, prefix.len), prefix);
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

xd_str xd_str_trim(xd_str s) {
    size_t start = 0;
    size_t end = s.len;
    while (start < end && is_space(s.ptr[start]))
        start++;
    while (end > start && is_space(s.ptr[end - 1]))
        end--;
    return xd_str_sub(s, start, end - start);
}

xd_str xd_str_sub(xd_str s, size_t start, size_t len) {
    if (start > s.len)
        start = s.len;
    if (len > s.len - start)
        len = s.len - start;
    return (xd_str){.ptr = s.ptr + start, .len = len};
}

bool xd_str_find_char(xd_str s, char c, size_t *index) {
    for (size_t i = 0; i < s.len; i++) {
        if (s.ptr[i] == c) {
            *index = i;
            return true;
        }
    }
    return false;
}

bool xd_str_parse_i64(xd_str s, int64_t min, int64_t max, int64_t *out) {
    s = xd_str_trim(s);
    if (s.len == 0)
        return false;

    size_t i = 0;
    bool negative = false;
    if (s.ptr[0] == '-' || s.ptr[0] == '+') {
        negative = s.ptr[0] == '-';
        i++;
    }
    if (i == s.len)
        return false;

    int64_t value = 0;
    for (; i < s.len; i++) {
        char c = s.ptr[i];
        if (c < '0' || c > '9')
            return false;
        int64_t digit = c - '0';
        if (ckd_mul(&value, value, 10) ||
            (negative ? ckd_sub(&value, value, digit) : ckd_add(&value, value, digit)))
            return false;
    }
    if (value < min || value > max)
        return false;
    *out = value;
    return true;
}

bool xd_str_parse_double(xd_str s, double *out) {
    s = xd_str_trim(s);
    char buf[64];
    if (s.len == 0 || s.len >= sizeof(buf))
        return false;
    memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';

    char *end = nullptr;
    errno = 0;
    double v = strtod(buf, &end);
    if (errno != 0 || end != buf + s.len)
        return false;
    *out = v;
    return true;
}

char *xd_str_dup_cstr(xd_str s) {
    size_t total;
    if (ckd_add(&total, s.len, 1))
        XD_PANIC("string too long");
    char *p = xd_alloc(total);
    if (s.len > 0)
        memcpy(p, s.ptr, s.len);
    p[s.len] = '\0';
    return p;
}

char *xd_str_arena_cstr(xd_arena *arena, xd_str s) {
    size_t total;
    if (ckd_add(&total, s.len, 1))
        XD_PANIC("string too long");
    char *p = xd_arena_alloc(arena, total, 1);
    if (s.len > 0)
        memcpy(p, s.ptr, s.len);
    p[s.len] = '\0';
    return p;
}

static void strbuf_reserve(xd_strbuf *sb, size_t extra) {
    size_t need;
    if (ckd_add(&need, sb->len, extra) || ckd_add(&need, need, 1))
        XD_PANIC("string buffer overflow");
    if (need <= sb->cap)
        return;
    size_t cap = sb->cap == 0 ? 64 : sb->cap;
    while (cap < need) {
        if (ckd_mul(&cap, cap, 2))
            XD_PANIC("string buffer overflow");
    }
    sb->data = xd_realloc_array(sb->data, cap, 1);
    sb->cap = cap;
}

void xd_strbuf_append(xd_strbuf *sb, xd_str s) {
    strbuf_reserve(sb, s.len);
    if (s.len > 0)
        memcpy(sb->data + sb->len, s.ptr, s.len);
    sb->len += s.len;
    sb->data[sb->len] = '\0';
}

void xd_strbuf_append_char(xd_strbuf *sb, char c) {
    xd_strbuf_append(sb, (xd_str){.ptr = &c, .len = 1});
}

void xd_strbuf_vappendf(xd_strbuf *sb, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        XD_PANIC("invalid format string");

    strbuf_reserve(sb, (size_t)n);
    int written = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    XD_ASSERT(written == n);
    sb->len += (size_t)n;
}

void xd_strbuf_appendf(xd_strbuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xd_strbuf_vappendf(sb, fmt, ap);
    va_end(ap);
}

xd_str xd_strbuf_view(const xd_strbuf *sb) {
    return (xd_str){.ptr = sb->data != nullptr ? sb->data : "", .len = sb->len};
}

const char *xd_strbuf_cstr(xd_strbuf *sb) {
    strbuf_reserve(sb, 0);
    sb->data[sb->len] = '\0';
    return sb->data;
}

void xd_strbuf_clear(xd_strbuf *sb) {
    sb->len = 0;
    if (sb->data != nullptr)
        sb->data[0] = '\0';
}

void xd_strbuf_free(xd_strbuf *sb) {
    XD_FREE(&sb->data);
    sb->len = 0;
    sb->cap = 0;
}

bool xd_read_file(const char *path, size_t max_size, xd_strbuf *out) {
    xd_strbuf_clear(out);
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
        return false;

    char chunk[4096];
    bool ok = true;
    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n > 0) {
            if (out->len + n > max_size) {
                ok = false;
                break;
            }
            xd_strbuf_append(out, (xd_str){.ptr = chunk, .len = n});
        }
        if (n < sizeof(chunk)) {
            ok = ferror(f) == 0;
            break;
        }
    }
    fclose(f);
    if (!ok)
        xd_strbuf_clear(out);
    return ok;
}
