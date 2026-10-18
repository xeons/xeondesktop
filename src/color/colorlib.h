/* SPDX-License-Identifier: MIT
 * Color Management's pieces that need no window: naming colord's devices
 * and profiles as Windows 7's Color Management did, the ids colord's
 * other clients use for monitors, and importing ICC files into the user's
 * profile store.
 */
#ifndef XD_COLOR_COLORLIB_H
#define XD_COLOR_COLORLIB_H

#include <gio/gio.h>

/* colord's device id for a monitor, as gnome-settings-daemon makes it
 * ("xrandr-Vendor-Model-Serial", fewer parts when the EDID lacks them,
 * else "xrandr-CONNECTOR"), so that profiles assigned there carry over.
 * Free with g_free. */
char *xd_color_display_id(const char *vendor, const char *model, const char *serial,
                          const char *connector);

/* "Display", "Printer", "Scanner", "Camera", "Webcam" for colord's Kind;
 * "Device" for anything else. */
const char *xd_color_kind_label(const char *kind);

/* The Device list's text: "Display: Dell Inc. U2415", or the id when the
 * device names no vendor or model. Free with g_free. */
char *xd_color_device_label(const char *kind, const char *vendor, const char *model,
                            const char *id);

/* Whether `data` (len bytes) starts like an ICC profile: a header whose
 * size field covers it and the 'acsp' signature at byte 36. */
bool xd_color_looks_like_icc(const guint8 *data, size_t len);

/* colord's id for a profile: "icc-" and the MD5 of its contents, as
 * colord's own clients make it. Free with g_free. */
char *xd_color_profile_id(const guint8 *data, size_t len);

/* Copies an ICC file into the user's store (`store`, normally
 * $XDG_DATA_HOME/icc, made if missing) under its own name, or "name-2.icc"
 * and so on if a different file has that name; a file with the same
 * contents already there is used as it is. Returns the stored path (free
 * with g_free) and, with id, the profile id; nullptr and *error if the
 * file can't be read, isn't an ICC profile, or can't be written. */
char *xd_color_import(const char *path, const char *store, char **id, GError **error);

#endif
