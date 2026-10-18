/* SPDX-License-Identifier: MIT
 * Where an input method's candidate window goes (ime.c), apart from
 * wlroots so the unit tests can check it.
 */
#ifndef XD_COMPOSITOR_IMEPLACE_H
#define XD_COMPOSITOR_IMEPLACE_H

typedef struct xd_ime_rect {
    int x, y, width, height;
} xd_ime_rect;

/* The popup (w x h) for a text cursor at `cursor` (layout coordinates),
 * on the monitor `screen`: just below the cursor and starting at it, as
 * Windows' IME candidate lists do; above the cursor if it doesn't fit
 * below, and moved left (never past the monitor's left edge) to fit. */
static inline xd_ime_rect xd_ime_popup_place(xd_ime_rect cursor, int w, int h,
                                             xd_ime_rect screen) {
    xd_ime_rect r = {cursor.x, cursor.y + cursor.height, w, h};
    int right = screen.x + screen.width, bottom = screen.y + screen.height;
    if (r.y + h > bottom && cursor.y - h >= screen.y)
        r.y = cursor.y - h;
    if (r.x + w > right)
        r.x = right - w;
    if (r.x < screen.x)
        r.x = screen.x;
    return r;
}

#endif
