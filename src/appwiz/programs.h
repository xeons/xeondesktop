/* SPDX-License-Identifier: MIT
 * The parts of Add or Remove Programs (xd-appwiz) that don't need a
 * window or a package manager: how often a program is used, sizes and
 * dates as XP showed them, the sort orders, and package ids.
 */
#ifndef XD_APPWIZ_PROGRAMS_H
#define XD_APPWIZ_PROGRAMS_H

#include <glib.h>

/* "Used: rarely/occasionally/frequently", from the Start menu's launch
 * counts. UNKNOWN when there are no counts at all (the Start menu has never
 * started anything), so nothing is shown. */
typedef enum xd_appwiz_usage {
    XD_USAGE_UNKNOWN,
    XD_USAGE_RARELY,
    XD_USAGE_OCCASIONALLY,
    XD_USAGE_FREQUENTLY,
} xd_appwiz_usage;

xd_appwiz_usage xd_appwiz_usage_for(int64_t launches, bool have_counts);
/* "rarely", ...; nullptr for UNKNOWN. */
const char *xd_appwiz_usage_name(xd_appwiz_usage usage);

/* The Start menu's launch counts ("count<TAB>desktop id" lines, from
 * $XDG_STATE_HOME/xeondesktop/start-frequent, as xd-shell writes them):
 * desktop id -> count. Malformed lines are skipped. */
GHashTable *xd_appwiz_parse_launch_counts(char *const *lines, size_t count);
int64_t xd_appwiz_launches(GHashTable *counts, const char *desktop_id);

/* When the Start menu (or the taskbar) last started each program
 * ("seconds<TAB>desktop id" lines, from $XDG_STATE_HOME/xeondesktop/
 * start-last-used): desktop id -> seconds since the epoch. */
GHashTable *xd_appwiz_parse_launch_times(char *const *lines, size_t count);
/* 0 if never. */
int64_t xd_appwiz_launch_time(GHashTable *times, const char *desktop_id);

/* When a program last opened something, from the recent documents list
 * (recently-used.xbel), which records each program by name or command:
 * matches the desktop id without ".desktop" or the executable's base
 * name. Seconds since the epoch, or 0 if never. */
int64_t xd_appwiz_last_used(GBookmarkFile *recent, const char *desktop_id,
                            const char *executable);

/* A size as XP wrote it: "22.39MB", "1.20GB"; "0.01MB" at least. */
char *xd_appwiz_format_size(guint64 bytes);
/* The short local date ("10/3/2006" in the US), or "" for 0. */
char *xd_appwiz_format_date(int64_t when);

/* Where a program came from, as the selected row's "Source" says it. */
/* The distribution's own name for PackageKit's backend ("alpm" is
 * "pacman", "aptcc" "APT"); the backend's own name if it isn't known,
 * "a package manager" without one. */
const char *xd_appwiz_package_manager(const char *backend);
/* "pacman (extra)": the package manager and the repository in the
 * package id's data field; "pacman (local package)" for one installed
 * from a file; "pacman" when PackageKit doesn't say. g_free it. */
char *xd_appwiz_package_source(const char *backend, const char *package_id);
/* "Flatpak (flathub, system)": the remote and the installation ("user",
 * "system" for the default system one, else its id). g_free it. */
char *xd_appwiz_flatpak_source(const char *remote, const char *installation);
/* A desktop entry that no package owns: "AppImage" when it starts one,
 * else "Not from a package". */
const char *xd_appwiz_other_source(const char *executable);

/* What the list is sorted by: the Sort by box. */
typedef enum xd_appwiz_sort {
    XD_SORT_NAME,
    XD_SORT_SIZE,
    XD_SORT_FREQUENCY,
    XD_SORT_DATE,
    XD_SORT_SOURCE,
} xd_appwiz_sort;

typedef struct xd_appwiz_key {
    const char *name;
    guint64 size; /* 0: not known */
    int64_t launches;
    int64_t last_used; /* 0: never */
    const char *source; /* Source's text; nullptr: not known (yet) */
} xd_appwiz_key;

/* Name A-Z; the others largest, most used or most recent first, unknowns
 * last, ties by name. */
int xd_appwiz_compare(const xd_appwiz_key *left, const xd_appwiz_key *right, xd_appwiz_sort sort);

/* A PackageKit package id ("name;version;arch;data"), split with
 * PackageKit's own pk_package_id_split. `repo` is the software source:
 * "extra" for "installed:extra" or "extra", "" when PackageKit only says
 * "installed" or "local". Returns false for a malformed id. Out parameters
 * may be nullptr. */
bool xd_appwiz_split_package_id(const char *package_id, char **name, char **version,
                                char **repo);
/* Whether the id names an installed package ("installed" or
 * "installed:repo" in the data field). */
bool xd_appwiz_package_installed(const char *package_id);

#endif
