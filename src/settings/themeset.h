/* SPDX-License-Identifier: MIT
 * Themes, as Display Properties' Themes tab chooses them (XP's): a theme
 * package's style together with the wallpaper, sound scheme and cursors
 * it brings (docs/THEMES.md), each put where the settings keep it, and a
 * package saved from the current settings.
 *
 * A package's files are unpacked where the settings can point at them:
 * the wallpaper and sounds into DATA/xeondesktop/theme-files/NAME/ (at
 * their paths in the package), the cursors, an XCursor theme, into
 * DATA/icons/xdtheme-NAME/, where libXcursor finds it. DATA is
 * $XDG_DATA_HOME; the functions take it (nullptr: GLib's) for tests.
 */
#ifndef XD_SETTINGS_THEMESET_H
#define XD_SETTINGS_THEMESET_H

#include "sounds.h"
#include "themepkg.h"

/* Whether the package brings anything besides its style: the Themes tab
 * lists only those. */
bool xd_theme_set_has_extras(const xd_pkg_info *info);

/* The cursor theme package `name`'s cursors are installed as
 * ("xdtheme-NAME"); false if that doesn't fit [Mouse] CursorTheme. */
bool xd_theme_set_cursor_theme(const char *name, char *out, size_t cap);

/* What choosing `p` as a theme sets besides its style: its wallpaper and
 * how it's placed into `s`, its cursor theme into `s` (Mouse), and its
 * sound scheme into `sounds` (`*sounds_set` true; untouched and false if
 * it has none). What it doesn't bring is left as it is, as XP's themes
 * did. With `unpack`, the files these point at are put there first;
 * without, only the settings are worked out (to compare). False with
 * `error` set, and `s` and `sounds` untouched, if the package can't be
 * read or its files written. */
bool xd_theme_set_extras(const xd_installed_package *p, const char *data, bool unpack,
                         xd_settings *s, xd_sound_scheme *sounds, bool *sounds_set,
                         GError **error);

/* Whether `s` and `sounds` hold what choosing `p` sets (its package, and
 * xd_theme_set_extras'); XP shows "(Modified)" after the theme otherwise. */
bool xd_theme_set_matches(const xd_installed_package *p, const char *data, const xd_settings *s,
                          const xd_sound_scheme *sounds);

/* Saves the current look as a package: `dest`, a .xdtheme named `name`
 * (theme.xml's), with the style of the package at `package` (its files
 * and variants, the variant `variant` first; nullptr: a package that
 * leaves everything to `s`'s style), `s`'s wallpaper, the sound scheme
 * `sounds` (its sound files copied in) and `s`'s cursor theme (found in
 * `icon_dirs`, libXcursor's search path; copied in). False with `error`
 * set if it can't be written. */
bool xd_theme_set_save(const char *dest, const char *name, const xd_settings *s,
                       const xd_sound_scheme *sounds, const char *package, const char *variant,
                       const char *const *icon_dirs, GError **error);

#endif
