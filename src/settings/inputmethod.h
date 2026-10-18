/* SPDX-License-Identifier: MIT
 * Input method frameworks ([Keyboard] InputMethod): Fcitx 5 and IBus,
 * which type languages that need more keys than the keyboard has
 * (Chinese, Japanese, Korean, ...), as XP's Text Services did.
 *
 * The compositor starts the chosen one with the session and gives it the
 * Wayland input method protocols (compositor/ime.c); Wayland programs
 * reach it through text-input-v3, X11 programs through XIM
 * (XMODIFIERS). Keyboard Properties picks it.
 */
#ifndef XD_SETTINGS_INPUTMETHOD_H
#define XD_SETTINGS_INPUTMETHOD_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

typedef enum xd_im_framework : uint8_t {
    XD_IM_NONE,
    XD_IM_FCITX5,
    XD_IM_IBUS,
    XD_IM_COUNT,
} xd_im_framework;

/* InputMethod's value: "fcitx5", "ibus"; anything else is none. */
xd_im_framework xd_im_from_name(const char *name);
const char *xd_im_name(xd_im_framework fw); /* "" for none */
/* What a list shows: "Fcitx 5", "IBus", "(None)". */
const char *xd_im_display_name(xd_im_framework fw);
/* Whether its program is on PATH (none: always). */
bool xd_im_installed(xd_im_framework fw);
/* The command that starts it, staying in the foreground where it can:
 * fcitx5 without -d; IBus 1.5.32's "ibus start --type wayland", which
 * starts ibus-daemon with its Wayland input method (input-method-v2).
 * nullptr for none. */
const char *const *xd_im_argv(xd_im_framework fw);
/* Its own settings program (the engines, its keys): fcitx5-configtool,
 * ibus-setup; nullptr for none. */
const char *xd_im_config_tool(xd_im_framework fw);
/* Whether `basename` is the file name of a program of an input method
 * framework that speaks the Wayland input method protocol itself. */
bool xd_im_program_is_framework(const char *basename);

/* The environment variables programs need to find the framework:
 * X11 programs (Xwayland) use XIM, named by XMODIFIERS; Qt 5 and Qt 6
 * before 6.8.2 need the framework's own Qt module (QT_IM_MODULE), as
 * they can't use text-input-v3; Qt 6.8.2 and later try text-input-v3
 * first (QT_IM_MODULES). GTK_IM_MODULE stays unset: GTK 3 and 4 on
 * Wayland use text-input-v3, which the compositor relays to the
 * framework, and the framework's GTK module would bypass the compositor
 * (Fcitx 5's Wayland guidance). */
enum { XD_IM_ENV_VARS = 3 };
extern const char *const xd_im_env_vars[XD_IM_ENV_VARS];
/* The value of xd_im_env_vars[i] for `fw`: "" for none. */
const char *xd_im_env_value(xd_im_framework fw, size_t i);
/* Sets the variables for programs started from now on, leaving alone
 * those the user set (the XEONDESKTOP_<var> marks, as for Qt's). With
 * none, ours become empty (the D-Bus and systemd environments can only
 * be given values) and unset ones stay unset. Returns whether any
 * changed. */
bool xd_im_env_apply(xd_im_framework fw);

/* Frameworks' own autostart entries (Fcitx 5 installs one in
 * /etc/xdg/autostart) would start one the user didn't choose, or a
 * second copy beside the compositor's. For each entry in `system_dirs`
 * (autostart directories) whose program is a framework's, a copy in
 * `user_dir` with XeonDesktop added to NotShowIn, so other desktops
 * still start it. A file of the user's own there is left alone; ours
 * are marked X-XeonDesktop-InputMethod. Returns false (and `error`) if
 * one couldn't be written. */
bool xd_im_autostart_override(const char *const *system_dirs, const char *user_dir,
                              GError **error);
/* The same for the session: $XDG_CONFIG_DIRS' autostart directories into
 * $XDG_CONFIG_HOME/autostart. */
bool xd_im_autostart_override_session(GError **error);

/* How to type emoji and other characters with it, for Keyboard
 * Properties; nullptr for none. */
const char *xd_im_emoji_hint(xd_im_framework fw);

#endif
