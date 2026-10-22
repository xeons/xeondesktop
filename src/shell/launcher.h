/* SPDX-License-Identifier: MIT
 * Programs' taskbar progress, counts and urgency, from Unity's
 * LauncherEntry API: the Linux stand-in for Windows 7's ITaskbarList3
 * (SetProgressValue, SetOverlayIcon, FlashWindow). Programs broadcast
 *
 *   com.canonical.Unity.LauncherEntry.Update(s app_uri, a{sv} properties)
 *
 * on the session bus, app_uri being application://NAME.desktop, with any
 * of progress (d), progress-visible (b), count (x), count-visible (b)
 * and urgent (b); each Update changes only the properties it names.
 * Firefox, Thunderbird, Chromium, libunity and KDE's programs send it.
 *
 * A program's state goes when its bus name does (each sender is
 * watched), or when the caller says its last window closed.
 */
#ifndef XD_SHELL_LAUNCHER_H
#define XD_SHELL_LAUNCHER_H

#include <glib.h>
#include <stdint.h>

typedef struct xd_launcher_state {
    double progress; /* 0..1 */
    bool progress_visible;
    int64_t count;
    bool count_visible;
    bool urgent;
} xd_launcher_state;

/* The desktop file id an app_uri names ("application://firefox.desktop":
 * "firefox.desktop"; ".desktop" is added where a sender left it off), or
 * nullptr if it isn't an application:// URI. g_free. */
char *xd_launcher_desktop_id(const char *app_uri);
/* Applies an Update's properties (a{sv}) to `st`; true if it changed.
 * Properties of the wrong type are ignored. */
bool xd_launcher_apply(xd_launcher_state *st, GVariant *props);
/* Whether the taskbar's program key (a desktop id, or "app:APP_ID" for a
 * window without a desktop file) is the program desktop id `id` names. */
bool xd_launcher_key_is(const char *id, const char *key);
/* The badge's text for a count ("99+" above 99), into buf. */
void xd_launcher_count_text(int64_t count, char *buf, size_t size);

typedef struct xd_launcher xd_launcher;

/* Runs when a program's state changes or goes (`now` all clear then):
 * its desktop id, and its state before and after. */
typedef void (*xd_launcher_changed_fn)(void *data, const char *id, const xd_launcher_state *was,
                                       const xd_launcher_state *now);

/* Starts listening on the session bus (nothing happens without one). */
xd_launcher *xd_launcher_new(xd_launcher_changed_fn changed, void *data);
void xd_launcher_free(xd_launcher *launcher);
/* The state of the program with taskbar key `key`; nullptr if it has none. */
const xd_launcher_state *xd_launcher_lookup(const xd_launcher *launcher, const char *key);
/* `open` holds the taskbar keys of the programs with windows now:
 * programs that had a window and have none any more are forgotten. */
void xd_launcher_sweep(xd_launcher *launcher, GHashTable *open);

#endif
