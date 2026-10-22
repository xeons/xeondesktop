/* SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#include "str.h"

xd_str xd_str_from_cstr(const char *text) {
    if (text == nullptr)
        return (xd_str){.ptr = "", .len = 0};
    return (xd_str){.ptr = text, .len = strlen(text)};
}

bool xd_str_eq(xd_str left, xd_str right) {
    return left.len == right.len && (left.len == 0 || memcmp(left.ptr, right.ptr, left.len) == 0);
}

static char ascii_lower(char character) {
    return (character >= 'A' && character <= 'Z') ? (char)(character - 'A' + 'a') : character;
}

bool xd_str_eq_nocase(xd_str left, xd_str right) {
    if (left.len != right.len)
        return false;
    for (size_t i = 0; i < left.len; i++) {
        if (ascii_lower(left.ptr[i]) != ascii_lower(right.ptr[i]))
            return false;
    }
    return true;
}

bool xd_str_starts_with(xd_str text, xd_str prefix) {
    return text.len >= prefix.len && xd_str_eq(xd_str_sub(text, 0, prefix.len), prefix);
}

static bool is_space(char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
           character == '\v' || character == '\f';
}

xd_str xd_str_trim(xd_str text) {
    size_t start = 0;
    size_t end = text.len;
    while (start < end && is_space(text.ptr[start]))
        start++;
    while (end > start && is_space(text.ptr[end - 1]))
        end--;
    return xd_str_sub(text, start, end - start);
}

xd_str xd_str_sub(xd_str text, size_t start, size_t len) {
    if (start > text.len)
        start = text.len;
    if (len > text.len - start)
        len = text.len - start;
    return (xd_str){.ptr = text.ptr + start, .len = len};
}

bool xd_str_find_char(xd_str text, char character, size_t *index) {
    for (size_t i = 0; i < text.len; i++) {
        if (text.ptr[i] == character) {
            *index = i;
            return true;
        }
    }
    return false;
}

bool xd_str_parse_i64(xd_str text, int64_t min, int64_t max, int64_t *out) {
    text = xd_str_trim(text);
    if (text.len == 0)
        return false;

    size_t i = 0;
    bool negative = false;
    if (text.ptr[0] == '-' || text.ptr[0] == '+') {
        negative = text.ptr[0] == '-';
        i++;
    }
    if (i == text.len)
        return false;

    int64_t value = 0;
    for (; i < text.len; i++) {
        char character = text.ptr[i];
        if (character < '0' || character > '9')
            return false;
        int64_t digit = character - '0';
        if (ckd_mul(&value, value, 10) ||
            (negative ? ckd_sub(&value, value, digit) : ckd_add(&value, value, digit)))
            return false;
    }
    if (value < min || value > max)
        return false;
    *out = value;
    return true;
}

bool xd_str_parse_double(xd_str text, double *out) {
    text = xd_str_trim(text);
    char buf[64];
    if (text.len == 0 || text.len >= sizeof(buf))
        return false;
    memcpy(buf, text.ptr, text.len);
    buf[text.len] = '\0';

    char *end = nullptr;
    errno = 0;
    double value = strtod(buf, &end);
    if (errno != 0 || end != buf + text.len)
        return false;
    *out = value;
    return true;
}

char *xd_str_dup_cstr(xd_str text) {
    size_t total;
    if (ckd_add(&total, text.len, 1))
        XD_PANIC("string too long");
    char *copy = xd_alloc(total);
    if (text.len > 0)
        memcpy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    return copy;
}

char *xd_str_arena_cstr(xd_arena *arena, xd_str text) {
    size_t total;
    if (ckd_add(&total, text.len, 1))
        XD_PANIC("string too long");
    char *copy = xd_arena_alloc(arena, total, 1);
    if (text.len > 0)
        memcpy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    return copy;
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

void xd_strbuf_append(xd_strbuf *sb, xd_str text) {
    strbuf_reserve(sb, text.len);
    if (text.len > 0)
        memcpy(sb->data + sb->len, text.ptr, text.len);
    sb->len += text.len;
    sb->data[sb->len] = '\0';
}

void xd_strbuf_append_char(xd_strbuf *sb, char character) {
    xd_strbuf_append(sb, (xd_str){.ptr = &character, .len = 1});
}

void xd_strbuf_vappendf(xd_strbuf *sb, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int length = vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    if (length < 0)
        XD_PANIC("invalid format string");

    strbuf_reserve(sb, (size_t)length);
    int written = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    XD_ASSERT(written == length);
    sb->len += (size_t)length;
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
    FILE *file = fopen(path, "rb");
    if (file == nullptr)
        return false;

    char chunk[4096];
    bool ok = true;
    for (;;) {
        size_t bytes_read = fread(chunk, 1, sizeof(chunk), file);
        if (bytes_read > 0) {
            if (out->len + bytes_read > max_size) {
                ok = false;
                break;
            }
            xd_strbuf_append(out, (xd_str){.ptr = chunk, .len = bytes_read});
        }
        if (bytes_read < sizeof(chunk)) {
            ok = ferror(file) == 0;
            break;
        }
    }
    fclose(file);
    if (!ok)
        xd_strbuf_clear(out);
    return ok;
}
