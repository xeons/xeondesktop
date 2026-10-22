/* SPDX-License-Identifier: MIT
 * Network drives and network places, as Windows XP's Map Network Drive
 * and Add Network Place keep them; GIO only (no GTK), so the shell's
 * logon reconnect and the unit tests can use it.
 *
 * A mapped drive is a network folder (\\server\share, smb://server/share,
 * nfs://server/path, or any location GIO mounts) mounted through GIO
 * (gvfs). Linux has no drive letters, so a drive is known by its folder.
 * The list lives in $XDG_CONFIG_HOME/xeondesktop/network.ini (GKeyFile):
 *
 *   [Drive 1]                  [Place 1]
 *   URI=smb://server/share     URI=sftp://example.org/srv
 *   Reconnect=true             Name=Web files
 *   User=OFFICE\bob
 *
 * Drives without Reconnect last until the next logon, as in XP. Passwords
 * are never kept here: "Remember my password" asks gvfs to keep them in
 * the Secret Service (GMountOperation's password-save flag).
 */
#ifndef XD_FILES_NETDRIVES_H
#define XD_FILES_NETDRIVES_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct XdNetDrive {
    char *uri;
    char *user; /* Connect As's user name ("DOMAIN\user"), or nullptr */
    gboolean reconnect;
} XdNetDrive;

typedef struct XdNetPlace {
    char *uri;
    char *name;
} XdNetPlace;

/* A folder as typed: "\\server\share\dir" (smb://server/share/dir) or a
 * URI with a host ("nfs://server/path"). `need_share`: a share or path
 * must follow the server, as Map Network Drive wants; without it
 * "\\server" alone is the computer (smb://server/). The URI is
 * normalised (no trailing slash). Free with g_free; nullptr and `err`
 * (G_IO_ERROR_INVALID_ARGUMENT) when it isn't a network folder. */
char *xd_net_parse_folder(const char *text, gboolean need_share, GError **err);
/* Whether two network folders' URIs name the same place (letter case of
 * the scheme and host, trailing slashes and escaping aside). */
gboolean xd_net_same_uri(const char *left, const char *right);
/* How XP shows the folder: "\\server\share\dir" for SMB, the unescaped
 * URI for others. Free with g_free. */
char *xd_net_display_path(const char *uri);
/* XP's name for a mapped drive, "share on 'server'" (its last folder for
 * deeper paths). Free with g_free. */
char *xd_net_drive_name(const char *uri);
/* The server's name, or nullptr. Free with g_free. */
char *xd_net_host(const char *uri);

/* Whether GIO can open `scheme` here (gvfs has a backend for it). */
gboolean xd_net_scheme_supported(const char *scheme);
/* For a scheme GIO can't open: what to tell the user (which package
 * brings it). Free with g_free. */
char *xd_net_unsupported_text(const char *uri);

/* $XDG_CONFIG_HOME/xeondesktop/network.ini. Free with g_free. */
char *xd_net_config_path(void);
/* The mapped drives (XdNetDrive*) and network places (XdNetPlace*) in
 * `path`, in order; none when it's missing. Free with g_ptr_array_unref. */
GPtrArray *xd_net_drives_load(const char *path);
GPtrArray *xd_net_places_load(const char *path);
/* Adds a drive, replacing one for the same folder. */
gboolean xd_net_drive_add(const char *path, const char *uri, const char *user,
                          gboolean reconnect, GError **err);
/* Removes the drive or place for `uri` (TRUE also when there was none). */
gboolean xd_net_drive_remove(const char *path, const char *uri, GError **err);
gboolean xd_net_place_add(const char *path, const char *uri, const char *name, GError **err);
gboolean xd_net_place_remove(const char *path, const char *uri, GError **err);
/* At logon: forgets the drives mapped without Reconnect. */
gboolean xd_net_forget_session_drives(const char *path, GError **err);

/* ---- answering gvfs's questions ---- */

/* Answers ask-password with what the user typed in "Connect to server":
 * `user` may be "DOMAIN\user" (split when a domain is asked for); an
 * empty one connects anonymously where the server allows it; `remember`
 * asks gvfs to keep the password in the Secret Service. */
void xd_net_login_reply(GMountOperation *op, GAskPasswordFlags flags, const char *user,
                        const char *password, gboolean remember);
/* Connect As's user name and password, given before mounting: the first
 * ask-password is answered with them (xd_net_answer_preset), later ones
 * (they were wrong) ask the user. */
void xd_net_operation_preset(GMountOperation *op, const char *user, const char *password);
/* TRUE if the preset answered this question. */
gboolean xd_net_answer_preset(GMountOperation *op, GAskPasswordFlags flags);
/* A mount operation for mounting without asking anyone (logon): every
 * question is aborted, so a server wanting a password that isn't in the
 * Secret Service is skipped. */
GMountOperation *xd_net_quiet_operation_new(void);

/* Logon: forgets the drives that weren't to reconnect and mounts the
 * others, all at once, each given at most `timeout_s` seconds. Servers
 * that can't be reached or want a password are skipped quietly (My
 * Computer shows them disconnected). `done` gets how many failed. */
void xd_net_reconnect_all(const char *path, guint timeout_s,
                          void (*done)(guint connected, guint failed, gpointer data),
                          gpointer data);

G_END_DECLS

#endif
