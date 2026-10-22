/* SPDX-License-Identifier: MIT
 * The Shortcut tab's fields of a launcher (.desktop file), as Windows' .lnk
 * Shortcut tab has them: Target (Exec, or a link's URL), Start in (Path),
 * Comment and the icon. Read and written with GKeyFile, which keeps the
 * rest of the file (other keys, translations, comments) as it was.
 */
#ifndef XD_FILES_LAUNCHER_H
#define XD_FILES_LAUNCHER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct XdLauncher {
    char *name;     /* Name, in the current language (read only) */
    char *target;   /* Exec, or URL for Type=Link */
    char *start_in; /* Path */
    char *comment;  /* Comment, in the current language if it has one */
    char *icon;     /* Icon: a theme name or a file */
    gboolean is_link;
} XdLauncher;

/* FALSE (with `err`) if `path` isn't a launcher that has a target
 * (Type=Application or Type=Link). Free with xd_launcher_clear. */
gboolean xd_launcher_read(const char *path, XdLauncher *out, GError **err);
void xd_launcher_clear(XdLauncher *launcher);

/* Writes the fields into the launcher at `path`; empty ones remove their
 * key. A launcher that can't be written (a system one) is written as a
 * copy of the same name in `user_dir` (~/.local/share/applications),
 * which takes its place in menus; with `user_dir` nullptr, it isn't
 * written. Returns the file written, or nullptr (with `err`). Free with
 * g_free. */
char *xd_launcher_write(const char *path, const XdLauncher *launcher, const char *user_dir,
                        GError **err);
/* An installed program's launcher (in a system applications folder),
 * which the user's own copy may override, as menu editors do. Any other
 * that can't be written (on a read-only drive) mustn't be copied there:
 * that would install it, Exec and all. */
gboolean xd_launcher_is_installed(const char *path);

G_END_DECLS

#endif
