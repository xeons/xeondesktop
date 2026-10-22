/* SPDX-License-Identifier: MIT
 * How a frame is presented (present.c): variable refresh rate and
 * tearing, decided apart from wlroots so the unit tests can check it.
 */
#ifndef XD_COMPOSITOR_PRESENT_H
#define XD_COMPOSITOR_PRESENT_H

#include "../settings/monitors.h"

/* What a frame's choice depends on, for one output. */
typedef struct xd_present_state {
    xd_vrr_mode vrr;    /* the monitor's setting */
    bool vrr_on;        /* adaptive sync is on now */
    bool vrr_rejected;  /* the output refused it since the setting changed */
    bool locked;        /* the session is locked */
    bool fullscreen;    /* a full-screen window is in front on this output */
    bool scanout;       /* the frame is that window's own buffer, nothing over it */
    bool async_hint;    /* that window asked to tear (tearing-control's async) */
} xd_present_state;

typedef struct xd_present_choice {
    bool set_vrr; /* change adaptive sync in this commit... */
    bool vrr;     /* ...to this */
    bool tearing; /* flip at once instead of at the next vblank */
} xd_present_choice;

static inline xd_present_choice xd_present_choose(xd_present_state present_state) {
    bool game = present_state.fullscreen && !present_state.locked;
    bool want =
        present_state.vrr == XD_VRR_ALWAYS || (present_state.vrr == XD_VRR_FULLSCREEN && game);
    xd_present_choice present_choice = {0};
    if (want != present_state.vrr_on && !(want && present_state.vrr_rejected)) {
        present_choice.set_vrr = true;
        present_choice.vrr = want;
    }
    /* Tearing only when the window is all there is on the monitor: a
     * notification, menu or the Alt+Tab window over a game would tear
     * too. The kernel's asynchronous flip changes nothing but the
     * buffer, so not in the commit that turns adaptive sync on or off. */
    present_choice.tearing =
        game && present_state.scanout && present_state.async_hint && !present_choice.set_vrr;
    return present_choice;
}

#endif
