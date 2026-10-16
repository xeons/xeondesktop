/* SPDX-License-Identifier: MIT */
#include "textedit.h"

size_t xd_utf8_sequence_len(const unsigned char *s, size_t n) {
    if (n == 0)
        return 0;
    unsigned char c = s[0];
    if (c < 0x80)
        return 1;
    size_t len;
    uint32_t min;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        min = 0x80;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        min = 0x800;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        min = 0x10000;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (n < len)
        return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    /* Reject overlong forms, UTF-16 surrogates and out-of-range values. */
    if (cp < min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return 0;
    return len;
}

static bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

static const unsigned char *bytes(const xd_textedit *e) {
    return (const unsigned char *)xd_textedit_view(e).ptr;
}

static size_t prev_boundary(const xd_textedit *e, size_t pos) {
    const unsigned char *b = bytes(e);
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && is_continuation(b[pos]))
        pos--;
    return pos;
}

static size_t next_boundary(const xd_textedit *e, size_t pos) {
    const unsigned char *b = bytes(e);
    size_t len = e->text.len;
    if (pos >= len)
        return len;
    pos++;
    while (pos < len && is_continuation(b[pos]))
        pos++;
    return pos;
}

static bool is_word_byte(unsigned char c) {
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

/* Ctrl+Left: skip spaces/punctuation, then the word before them. */
static size_t word_left(const xd_textedit *e, size_t pos) {
    const unsigned char *b = bytes(e);
    while (pos > 0 && !is_word_byte(b[pos - 1]))
        pos--;
    while (pos > 0 && is_word_byte(b[pos - 1]))
        pos--;
    return pos;
}

/* Ctrl+Right: to the start of the next word, as Windows edit boxes do. */
static size_t word_right(const xd_textedit *e, size_t pos) {
    const unsigned char *b = bytes(e);
    size_t len = e->text.len;
    while (pos < len && is_word_byte(b[pos]))
        pos++;
    while (pos < len && !is_word_byte(b[pos]))
        pos++;
    return pos;
}

void xd_textedit_free(xd_textedit *e) {
    xd_strbuf_free(&e->text);
    e->cursor = 0;
    e->anchor = 0;
}

xd_str xd_textedit_view(const xd_textedit *e) { return xd_strbuf_view(&e->text); }

bool xd_textedit_has_selection(const xd_textedit *e) { return e->cursor != e->anchor; }

void xd_textedit_selection(const xd_textedit *e, size_t *start, size_t *end) {
    *start = XD_MIN(e->cursor, e->anchor);
    *end = XD_MAX(e->cursor, e->anchor);
}

xd_str xd_textedit_selected(const xd_textedit *e) {
    size_t s;
    size_t t;
    xd_textedit_selection(e, &s, &t);
    return xd_str_sub(xd_textedit_view(e), s, t - s);
}

void xd_textedit_select_all(xd_textedit *e) {
    e->anchor = 0;
    e->cursor = e->text.len;
}

/* Removes bytes [start, end) from the buffer. */
static void erase(xd_textedit *e, size_t start, size_t end) {
    XD_ASSERT(start <= end && end <= e->text.len);
    if (start == end)
        return;
    memmove(e->text.data + start, e->text.data + end, e->text.len - end);
    e->text.len -= end - start;
    e->text.data[e->text.len] = '\0';
    e->cursor = start;
    e->anchor = start;
}

void xd_textedit_delete_selection(xd_textedit *e) {
    size_t s;
    size_t t;
    xd_textedit_selection(e, &s, &t);
    erase(e, s, t);
}

void xd_textedit_insert(xd_textedit *e, xd_str utf8) {
    xd_textedit_delete_selection(e);

    /* Validate into a scratch buffer first. */
    xd_strbuf clean = {0};
    const unsigned char *in = (const unsigned char *)utf8.ptr;
    for (size_t i = 0; i < utf8.len;) {
        size_t n = xd_utf8_sequence_len(in + i, utf8.len - i);
        if (n == 0) {
            i++; /* drop the invalid byte */
            continue;
        }
        bool control = n == 1 && (in[i] < 0x20 || in[i] == 0x7F);
        if (!control)
            xd_strbuf_append(&clean, xd_str_sub(utf8, i, n));
        i += n;
    }

    size_t add = clean.len;
    if (e->max_len > 0 && e->text.len + add > e->max_len) {
        /* Truncate at a character boundary. */
        add = e->max_len > e->text.len ? e->max_len - e->text.len : 0;
        const unsigned char *c = (const unsigned char *)xd_strbuf_view(&clean).ptr;
        while (add > 0 && add < clean.len && is_continuation(c[add]))
            add--;
    }
    if (add > 0) {
        /* Make room, then splice in at the cursor. */
        xd_strbuf_append(&e->text, xd_str_sub(xd_strbuf_view(&clean), 0, add));
        size_t tail = e->text.len - add - e->cursor;
        memmove(e->text.data + e->cursor + add, e->text.data + e->cursor, tail);
        memcpy(e->text.data + e->cursor, clean.data, add);
        e->cursor += add;
        e->anchor = e->cursor;
    }
    xd_strbuf_free(&clean);
}

void xd_textedit_backspace(xd_textedit *e, bool word) {
    if (xd_textedit_has_selection(e)) {
        xd_textedit_delete_selection(e);
        return;
    }
    size_t start = word ? word_left(e, e->cursor) : prev_boundary(e, e->cursor);
    erase(e, start, e->cursor);
}

void xd_textedit_delete(xd_textedit *e, bool word) {
    if (xd_textedit_has_selection(e)) {
        xd_textedit_delete_selection(e);
        return;
    }
    size_t end = word ? word_right(e, e->cursor) : next_boundary(e, e->cursor);
    erase(e, e->cursor, end);
}

void xd_textedit_move(xd_textedit *e, xd_text_motion motion, bool extend) {
    size_t pos = e->cursor;
    /* Left/Right without Shift collapse a selection to its edge. */
    if (!extend && xd_textedit_has_selection(e) &&
        (motion == XD_MOTION_LEFT || motion == XD_MOTION_RIGHT)) {
        size_t s;
        size_t t;
        xd_textedit_selection(e, &s, &t);
        pos = motion == XD_MOTION_LEFT ? s : t;
    } else {
        switch (motion) {
        case XD_MOTION_LEFT:
            pos = prev_boundary(e, pos);
            break;
        case XD_MOTION_RIGHT:
            pos = next_boundary(e, pos);
            break;
        case XD_MOTION_WORD_LEFT:
            pos = word_left(e, pos);
            break;
        case XD_MOTION_WORD_RIGHT:
            pos = word_right(e, pos);
            break;
        case XD_MOTION_HOME:
            pos = 0;
            break;
        case XD_MOTION_END:
            pos = e->text.len;
            break;
        }
    }
    e->cursor = pos;
    if (!extend)
        e->anchor = pos;
}

void xd_textedit_set_cursor(xd_textedit *e, size_t pos, bool extend) {
    if (pos > e->text.len)
        pos = e->text.len;
    const unsigned char *b = bytes(e);
    while (pos > 0 && pos < e->text.len && is_continuation(b[pos]))
        pos--;
    e->cursor = pos;
    if (!extend)
        e->anchor = pos;
}

void xd_textedit_set(xd_textedit *e, xd_str text) {
    xd_strbuf_clear(&e->text);
    e->cursor = 0;
    e->anchor = 0;
    xd_textedit_insert(e, text);
    xd_textedit_select_all(e);
}
