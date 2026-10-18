/* SPDX-License-Identifier: MIT
 * Sharing folders on the network, as XP's Sharing tab: Samba's user
 * shares, made and removed with `net usershare` (run with an argument
 * vector, never through a shell). They need an administrator to have
 * turned them on (smb.conf's "usershare path", "usershare max shares" and
 * membership of the group owning that folder); xd_usershare_probe says
 * which part is missing so the tab can say what to ask for.
 *
 * XD_SMB_CONF, when set, is passed to `net` and `testparm` as their
 * smb.conf (-s): the tests use a scratch one, never the system's.
 */
#ifndef XD_FILES_USERSHARE_H
#define XD_FILES_USERSHARE_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
    XD_USERSHARE_OK,
    XD_USERSHARE_NO_SAMBA,      /* no `net` program */
    XD_USERSHARE_DISABLED,      /* no usershare path, or max shares 0 */
    XD_USERSHARE_NO_PERMISSION, /* the usershare folder isn't writable by us */
    XD_USERSHARE_FAILED,        /* `net` failed otherwise */
} XdUsershareStatus;

typedef struct XdUsershare {
    char *name;
    char *path;
    char *comment;
    gboolean writable; /* Everyone may change files (the ACL's F) */
    gboolean guest_ok;
} XdUsershare;

/* The user's shares (XdUsershare*), and whether sharing is possible
 * (`detail`: the usershare folder for NO_PERMISSION, `net`'s message for
 * FAILED; free with g_free). Runs `net usershare info` and, to check the
 * folder, `testparm`. Free with g_ptr_array_unref. */
GPtrArray *xd_usershare_probe(XdUsershareStatus *status, char **detail);
/* What `net usershare info` printed (an INI file of the shares), parsed. */
GPtrArray *xd_usershare_parse_info(const char *text);
/* The status a failed `net usershare` run means, from its messages. */
XdUsershareStatus xd_usershare_status_of(const char *messages);
/* Whether we can make shares in `dir` (the usershare path); if not, the
 * group whose members can (free with g_free; nullptr if unknown). */
gboolean xd_usershare_dir_writable(const char *dir, char **group);

/* The share of `path` among `shares`, or nullptr. */
const XdUsershare *xd_usershare_find(GPtrArray *shares, const char *path);

/* Shares `path` as `name` (replacing a share of that name), read-only
 * or writable by everyone, as XP's "Allow network users to change my
 * files". */
gboolean xd_usershare_add(const char *name, const char *path, const char *comment,
                          gboolean writable, GError **err);
gboolean xd_usershare_delete(const char *name, GError **err);

/* XP's checks on a share name: nullptr if it's fine, else the message. */
const char *xd_usershare_name_problem(const char *name);
/* The name XP suggests: the folder's name, cut to 80 characters. Free
 * with g_free. */
char *xd_usershare_default_name(const char *path);
/* What an administrator must do, for the Sharing tab. Free with g_free. */
char *xd_usershare_help_text(XdUsershareStatus status, const char *detail);

/* Paths shared, for the folder view's hand overlay: listed in the
 * background at most every few seconds; `changed` runs when a listing
 * differs from the last. nullptr until the first listing. */
GHashTable *xd_usershare_shared_paths(void);
void xd_usershare_watch(void (*changed)(gpointer data), gpointer data);
void xd_usershare_unwatch(gpointer data);
/* Lists again now (after a change from the Sharing tab). */
void xd_usershare_refresh(void);

G_END_DECLS

#endif
