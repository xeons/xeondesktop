/* SPDX-License-Identifier: MIT
 * What programs were allowed or refused through xdg-desktop-portal: its
 * permission store (org.freedesktop.impl.portal.PermissionStore, on the
 * session bus), which the portals fill as the user answers their
 * questions. GIO only, so it can be tested without a display.
 *
 * Each permission is a table and an id in the store; under it, each
 * program (its app id, "" for programs outside a sandbox) has a list of
 * strings whose first says yes or no.
 */
#ifndef XD_PRIVACY_PERMISSIONS_H
#define XD_PRIVACY_PERMISSIONS_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct XdPermKind {
    const char *table, *id; /* where it is in the store */
    const char *label;      /* "Camera" */
    const char *allow;      /* the first value for yes ("yes"; location: "EXACT") */
    const char *deny;       /* and for no ("no"; location: "NONE") */
} XdPermKind;

/* The permissions shown, in order; the first is location's. */
extern const XdPermKind xd_perm_kinds[];
extern const size_t xd_perm_kind_count;

/* One program's answer for one permission. */
typedef struct XdPermEntry {
    const XdPermKind *kind;
    char *app;     /* app id; "" for programs outside a sandbox */
    char **values; /* as stored */
} XdPermEntry;

/* Whether `values` allow: anything but the kind's "no" (or nothing). */
gboolean xd_perm_allowed(const XdPermKind *kind, const char *const *values);

/* `values` with the first changed to the kind's yes or no, the rest
 * (location's last-used time) kept. Free with g_strfreev. */
char **xd_perm_values_set(const XdPermKind *kind, const char *const *values, gboolean allow);

/* When location was last used, from its values (Unix seconds), or -1. */
gint64 xd_perm_last_used(const XdPermEntry *e);

/* Every program's answers in the store on `bus`, sorted by app id, then
 * in xd_perm_kinds' order. Tables the store doesn't have yet are empty,
 * not errors. nullptr, with `error` set, if the store can't be reached.
 * Free with g_ptr_array_unref. */
GPtrArray *xd_perm_list(GDBusConnection *bus, GCancellable *cancel, GError **error);

/* Changes an answer to yes or no. */
gboolean xd_perm_set(GDBusConnection *bus, const XdPermEntry *e, gboolean allow,
                     GCancellable *cancel, GError **error);

/* Forgets an answer: the program asks again next time. */
gboolean xd_perm_remove(GDBusConnection *bus, const XdPermEntry *e, GCancellable *cancel,
                        GError **error);

G_END_DECLS

#endif
