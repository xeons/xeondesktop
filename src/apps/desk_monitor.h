/* SPDX-License-Identifier: MIT
 * Display Properties > Settings > Advanced: XP's properties of the monitor
 * and its graphics adapter, here its Monitor tab alone: the screen refresh
 * rate and the variable refresh rate.
 */
#ifndef XD_APPS_DESK_MONITOR_H
#define XD_APPS_DESK_MONITOR_H

#include "../ui/ui.h"

typedef struct monitor_dialog monitor_dialog;

/* What the dialog edits for one monitor. */
typedef struct monitor_props {
    int rate; /* index into the refresh rates it was given */
    bool vrr; /* Variable refresh rate */
} monitor_props;

typedef enum monitor_result : uint8_t {
    MONITOR_APPLY,  /* Apply: the dialog stays open */
    MONITOR_OK,     /* closed with OK */
    MONITOR_CANCEL, /* closed with Cancel (props is nullptr) */
} monitor_result;

typedef void (*monitor_done)(monitor_result r, const monitor_props *props, void *user);

/* `rates` are the refresh rate combo's items; vrr_capable says whether the
 * monitor can do variable refresh rates at all (else the box is grey). */
monitor_dialog *monitor_dialog_open(xd_ui *ui, const char *monitor, const char *const *rates,
                                    size_t rate_count, monitor_props current, bool vrr_capable,
                                    monitor_done done, void *user);
/* Closes it as Cancel does (done runs). */
void monitor_dialog_cancel(monitor_dialog *m);

#endif
