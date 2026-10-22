/* SPDX-License-Identifier: MIT */
#include "textedit.h"

size_t xd_utf8_sequence_len(const unsigned char *sequence, size_t length) {
    if (length == 0)
        return 0;
    unsigned char lead = sequence[0];
    if (lead < 0x80)
        return 1;
    size_t len;
    uint32_t min;
    uint32_t cp;
    if ((lead & 0xE0) == 0xC0) {
        len = 2;
        min = 0x80;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        min = 0x800;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        min = 0x10000;
        cp = lead & 0x07;
    } else {
        return 0;
    }
    if (length < len)
        return 0;
    for (size_t i = 1; i < len; i++) {
        if ((sequence[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (sequence[i] & 0x3F);
    }
    /* Reject overlong forms, UTF-16 surrogates and out-of-range values. */
    if (cp < min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return 0;
    return len;
}

static bool is_continuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

static const unsigned char *bytes(const xd_textedit *edit) {
    return (const unsigned char *)xd_textedit_view(edit).ptr;
}

static size_t prev_boundary(const xd_textedit *edit, size_t pos) {
    const unsigned char *text = bytes(edit);
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && is_continuation(text[pos]))
        pos--;
    return pos;
}

static size_t next_boundary(const xd_textedit *edit, size_t pos) {
    const unsigned char *text = bytes(edit);
    size_t len = edit->text.len;
    if (pos >= len)
        return len;
    pos++;
    while (pos < len && is_continuation(text[pos]))
        pos++;
    return pos;
}

static bool is_word_byte(unsigned char byte) {
    return byte >= 0x80 || (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '_';
}

/* Ctrl+Left: skip spaces/punctuation, then the word before them. */
static size_t word_left(const xd_textedit *edit, size_t pos) {
    const unsigned char *text = bytes(edit);
    while (pos > 0 && !is_word_byte(text[pos - 1]))
        pos--;
    while (pos > 0 && is_word_byte(text[pos - 1]))
        pos--;
    return pos;
}

/* Ctrl+Right: to the start of the next word, as Windows edit boxes do. */
static size_t word_right(const xd_textedit *edit, size_t pos) {
    const unsigned char *text = bytes(edit);
    size_t len = edit->text.len;
    while (pos < len && is_word_byte(text[pos]))
        pos++;
    while (pos < len && !is_word_byte(text[pos]))
        pos++;
    return pos;
}

void xd_textedit_free(xd_textedit *edit) {
    xd_strbuf_free(&edit->text);
    edit->cursor = 0;
    edit->anchor = 0;
}

xd_str xd_textedit_view(const xd_textedit *edit) { return xd_strbuf_view(&edit->text); }

bool xd_textedit_has_selection(const xd_textedit *edit) { return edit->cursor != edit->anchor; }

void xd_textedit_selection(const xd_textedit *edit, size_t *start, size_t *end) {
    *start = XD_MIN(edit->cursor, edit->anchor);
    *end = XD_MAX(edit->cursor, edit->anchor);
}

xd_str xd_textedit_selected(const xd_textedit *edit) {
    size_t start;
    size_t end;
    xd_textedit_selection(edit, &start, &end);
    return xd_str_sub(xd_textedit_view(edit), start, end - start);
}

void xd_textedit_select_all(xd_textedit *edit) {
    edit->anchor = 0;
    edit->cursor = edit->text.len;
}

/* Removes bytes [start, end) from the buffer. */
static void erase(xd_textedit *edit, size_t start, size_t end) {
    XD_ASSERT(start <= end && end <= edit->text.len);
    if (start == end)
        return;
    memmove(edit->text.data + start, edit->text.data + end, edit->text.len - end);
    edit->text.len -= end - start;
    edit->text.data[edit->text.len] = '\0';
    edit->cursor = start;
    edit->anchor = start;
}

void xd_textedit_delete_selection(xd_textedit *edit) {
    size_t start;
    size_t end;
    xd_textedit_selection(edit, &start, &end);
    erase(edit, start, end);
}

void xd_textedit_insert(xd_textedit *edit, xd_str utf8) {
    xd_textedit_delete_selection(edit);

    /* Validate into a scratch buffer first. */
    xd_strbuf clean = {0};
    const unsigned char *in = (const unsigned char *)utf8.ptr;
    for (size_t i = 0; i < utf8.len;) {
        size_t sequence_len = xd_utf8_sequence_len(in + i, utf8.len - i);
        if (sequence_len == 0) {
            i++; /* drop the invalid byte */
            continue;
        }
        bool control = sequence_len == 1 && (in[i] < 0x20 || in[i] == 0x7F);
        if (!control)
            xd_strbuf_append(&clean, xd_str_sub(utf8, i, sequence_len));
        i += sequence_len;
    }

    size_t add = clean.len;
    if (edit->max_len > 0 && edit->text.len + add > edit->max_len) {
        /* Truncate at a character boundary. */
        add = edit->max_len > edit->text.len ? edit->max_len - edit->text.len : 0;
        const unsigned char *clean_bytes = (const unsigned char *)xd_strbuf_view(&clean).ptr;
        while (add > 0 && add < clean.len && is_continuation(clean_bytes[add]))
            add--;
    }
    if (add > 0) {
        /* Make room, then splice in at the cursor. */
        xd_strbuf_append(&edit->text, xd_str_sub(xd_strbuf_view(&clean), 0, add));
        size_t tail = edit->text.len - add - edit->cursor;
        memmove(edit->text.data + edit->cursor + add, edit->text.data + edit->cursor, tail);
        memcpy(edit->text.data + edit->cursor, clean.data, add);
        edit->cursor += add;
        edit->anchor = edit->cursor;
    }
    xd_strbuf_free(&clean);
}

void xd_textedit_backspace(xd_textedit *edit, bool word) {
    if (xd_textedit_has_selection(edit)) {
        xd_textedit_delete_selection(edit);
        return;
    }
    size_t start = word ? word_left(edit, edit->cursor) : prev_boundary(edit, edit->cursor);
    erase(edit, start, edit->cursor);
}

void xd_textedit_delete(xd_textedit *edit, bool word) {
    if (xd_textedit_has_selection(edit)) {
        xd_textedit_delete_selection(edit);
        return;
    }
    size_t end = word ? word_right(edit, edit->cursor) : next_boundary(edit, edit->cursor);
    erase(edit, edit->cursor, end);
}

void xd_textedit_move(xd_textedit *edit, xd_text_motion motion, bool extend) {
    size_t pos = edit->cursor;
    /* Left/Right without Shift collapse a selection to its edge. */
    if (!extend && xd_textedit_has_selection(edit) &&
        (motion == XD_MOTION_LEFT || motion == XD_MOTION_RIGHT)) {
        size_t start;
        size_t end;
        xd_textedit_selection(edit, &start, &end);
        pos = motion == XD_MOTION_LEFT ? start : end;
    } else {
        switch (motion) {
        case XD_MOTION_LEFT:
            pos = prev_boundary(edit, pos);
            break;
        case XD_MOTION_RIGHT:
            pos = next_boundary(edit, pos);
            break;
        case XD_MOTION_WORD_LEFT:
            pos = word_left(edit, pos);
            break;
        case XD_MOTION_WORD_RIGHT:
            pos = word_right(edit, pos);
            break;
        case XD_MOTION_HOME:
            pos = 0;
            break;
        case XD_MOTION_END:
            pos = edit->text.len;
            break;
        }
    }
    edit->cursor = pos;
    if (!extend)
        edit->anchor = pos;
}

void xd_textedit_set_cursor(xd_textedit *edit, size_t pos, bool extend) {
    if (pos > edit->text.len)
        pos = edit->text.len;
    const unsigned char *text = bytes(edit);
    while (pos > 0 && pos < edit->text.len && is_continuation(text[pos]))
        pos--;
    edit->cursor = pos;
    if (!extend)
        edit->anchor = pos;
}

void xd_textedit_set(xd_textedit *edit, xd_str text) {
    xd_strbuf_clear(&edit->text);
    edit->cursor = 0;
    edit->anchor = 0;
    xd_textedit_insert(edit, text);
    xd_textedit_select_all(edit);
}
