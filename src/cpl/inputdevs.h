/* SPDX-License-Identifier: MIT
 * The Hardware tab of Mouse and Keyboard Properties: the input devices
 * udev knows of one kind, with their maker and where they're connected.
 */
#ifndef XD_CPL_INPUTDEVS_H
#define XD_CPL_INPUTDEVS_H

#include <gtk/gtk.h>

typedef enum xd_input_kind : uint8_t {
    XD_INPUT_POINTERS,  /* mice, touchpads, pointing sticks */
    XD_INPUT_KEYBOARDS,
} xd_input_kind;

/* A "Devices:" list and the selected one's properties. */
GtkWidget *xd_input_devices_page(xd_input_kind kind);
/* Whether udev knows a touchpad (Mouse Properties' Touchpad tab). */
bool xd_input_has_touchpad(void);

#endif
