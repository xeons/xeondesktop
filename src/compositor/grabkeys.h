/* SPDX-License-Identifier: MIT
 * Programs that take the keyboard and mouse (constraints.c): which keys
 * stay the desktop's while a program takes our shortcuts, and Ctrl+Alt on
 * its own giving the mouse and keyboard back. Apart from wlroots so the
 * unit tests can check them.
 */
#ifndef XD_COMPOSITOR_GRABKEYS_H
#define XD_COMPOSITOR_GRABKEYS_H

#include <stdint.h>
#include <xkbcommon/xkbcommon-keysyms.h>

typedef struct xd_grab_mods {
    bool ctrl, alt, shift, logo;
} xd_grab_mods;

/* Whether a key (pressed with `m` held) is still ours while the focused
 * program takes the shortcuts. What no Windows program can take either:
 * Ctrl+Alt+Delete (the secure attention sequence) and Win+L (Winlogon's;
 * a full-screen remote desktop gets neither); and what belongs to the
 * session rather than the desktop: switching virtual terminals
 * (Ctrl+Alt+F1..F12, which the keymap turns into XF86Switch_VT_n) and
 * Ctrl+Alt+Backspace. */
static inline bool xd_grab_key_kept(xd_grab_mods m, uint32_t sym) {
    if (m.ctrl && m.alt && (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete))
        return true;
    if (m.ctrl && m.alt && sym == XKB_KEY_BackSpace)
        return true;
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12)
        return true;
    return m.logo && !m.ctrl && !m.alt && (sym == XKB_KEY_l || sym == XKB_KEY_L);
}

/* Ctrl+Alt pressed together, with no other key, and let go: as VMware's
 * Ctrl+Alt, it releases a captured mouse (and keyboard). */
typedef struct xd_release_chord {
    bool ctrl, alt;
    bool spoiled; /* another key came, or it already fired */
} xd_release_chord;

static inline bool xd_chord_is_ctrl(uint32_t sym) {
    return sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R;
}

static inline bool xd_chord_is_alt(uint32_t sym) {
    return sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || sym == XKB_KEY_Meta_L ||
           sym == XKB_KEY_Meta_R;
}

/* Feeds a key event (its keysyms); true when the chord is complete: the
 * first of Ctrl and Alt is let go after both were held alone. */
static inline bool xd_release_chord_key(xd_release_chord *c, const uint32_t *syms, int nsyms,
                                        bool pressed) {
    bool ctrl = false, alt = false;
    for (int i = 0; i < nsyms; i++) {
        ctrl = ctrl || xd_chord_is_ctrl(syms[i]);
        alt = alt || xd_chord_is_alt(syms[i]);
    }
    if (pressed) {
        if (!ctrl && !alt) {
            c->spoiled = true;
            return false;
        }
        if (!c->ctrl && !c->alt)
            c->spoiled = false; /* a fresh start */
        if (ctrl)
            c->ctrl = true;
        else
            c->alt = true;
        return false;
    }
    if (!ctrl && !alt)
        return false;
    bool fire = c->ctrl && c->alt && !c->spoiled;
    if (fire)
        c->spoiled = true; /* not again when the other one is let go */
    if (ctrl)
        c->ctrl = false;
    else
        c->alt = false;
    return fire;
}

#endif
