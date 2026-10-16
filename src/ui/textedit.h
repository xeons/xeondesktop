/* SPDX-License-Identifier: MIT
 * Single-line text editing model (the logic behind an edit control).
 *
 * Positions are byte offsets that always sit on UTF-8 character
 * boundaries. Inserted text is validated: invalid UTF-8 and control
 * characters are dropped, so the buffer is always valid UTF-8.
 */
#ifndef XD_UI_TEXTEDIT_H
#define XD_UI_TEXTEDIT_H

#include "../core/str.h"

typedef struct xd_textedit {
    xd_strbuf text;
    size_t cursor;
    size_t anchor;   /* other end of the selection; == cursor when none */
    size_t max_len;  /* bytes; 0 = unlimited */
} xd_textedit;

typedef enum xd_text_motion : uint8_t {
    XD_MOTION_LEFT,
    XD_MOTION_RIGHT,
    XD_MOTION_WORD_LEFT,
    XD_MOTION_WORD_RIGHT,
    XD_MOTION_HOME,
    XD_MOTION_END,
} xd_text_motion;

void xd_textedit_free(xd_textedit *e);
/* Replaces the text (validated) and selects all of it, as Windows does
 * when a dialog fills in an edit box. */
void xd_textedit_set(xd_textedit *e, xd_str text);
xd_str xd_textedit_view(const xd_textedit *e);

bool xd_textedit_has_selection(const xd_textedit *e);
void xd_textedit_selection(const xd_textedit *e, size_t *start, size_t *end);
xd_str xd_textedit_selected(const xd_textedit *e);
void xd_textedit_select_all(xd_textedit *e);

/* Inserts at the cursor, replacing any selection. */
void xd_textedit_insert(xd_textedit *e, xd_str utf8);
void xd_textedit_backspace(xd_textedit *e, bool word);
void xd_textedit_delete(xd_textedit *e, bool word);
void xd_textedit_delete_selection(xd_textedit *e);
void xd_textedit_move(xd_textedit *e, xd_text_motion motion, bool extend);
/* Puts the cursor at `pos` (snapped to a character boundary). */
void xd_textedit_set_cursor(xd_textedit *e, size_t pos, bool extend);

/* Length of the valid UTF-8 sequence at s, or 0 if invalid. Exposed for
 * tests. */
size_t xd_utf8_sequence_len(const unsigned char *s, size_t n);

#endif
