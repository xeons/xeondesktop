/* SPDX-License-Identifier: MIT
 * xd-scanners' windows: the Scanners and Cameras folder and Properties
 * (main.c) and the Scanner and Camera Wizard (imgwizard.c).
 */
#ifndef XD_SCANNERS_SCANNERS_H
#define XD_SCANNERS_SCANNERS_H

#include <gtk/gtk.h>

#include "devices.h"

/* XP's Scanner and Camera Wizard: for a scanner, choose the picture type
 * and preview, then scan into the Pictures folder; for a camera, choose
 * pictures and copy them there. */
void xd_imaging_wizard_open(GtkWindow *parent, const xd_imaging_device *dev);

/* Each window counts itself in while open; the program ends with the
 * last one. */
void xd_scanners_hold(GtkWidget *window);

#endif
