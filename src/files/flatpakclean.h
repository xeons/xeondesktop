/* SPDX-License-Identifier: MIT
 * Disk Cleanup's Unused Flatpak runtimes: runtimes and extensions that no
 * installed program uses any more (what flatpak uninstall --unused
 * removes), in the user's installation and the system's, through
 * libflatpak. Removing the system's asks polkit through Flatpak's own
 * system helper; nothing of ours runs as root.
 */
#ifndef XD_FILES_FLATPAKCLEAN_H
#define XD_FILES_FLATPAKCLEAN_H

#include <gio/gio.h>

#include "cleanup.h"

/* The unused runtimes of the installations on the places' drive, with
 * their installed size; nullptr when there are none. */
XdCleanupCategory *xd_flatpak_unused_scan(const XdCleanupPlaces *cleanup_places,
                                          GCancellable *cancel);

/* Uninstalls what the category found. FALSE with `err` set to the first
 * failure, after trying every installation. */
bool xd_flatpak_unused_remove(const XdCleanupCategory *cleanup_category, GCancellable *cancel,
                              GError **err);

#endif
