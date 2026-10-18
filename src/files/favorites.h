/* SPDX-License-Identifier: MIT
 * The Favorites menu's list: GTK's bookmarks file
 * ($XDG_CONFIG_HOME/gtk-3.0/bookmarks), so the folders added here show in
 * every GTK file chooser's sidebar and the chooser's bookmarks show here.
 *
 * Each bookmark is a line "URI[ label]". Every other line (comments, blank
 * lines, anything not understood) is kept as it was when the file is
 * written back. Each change re-reads the file first, so changes made by
 * other programs meanwhile aren't lost.
 */
#ifndef XD_FILES_FAVORITES_H
#define XD_FILES_FAVORITES_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct XdFavorite {
    char *uri;
    char *label; /* nullptr: none given (the folder's own name is shown) */
} XdFavorite;

/* The bookmarks file in the user's config folder. Free with g_free. */
char *xd_favorites_path(void);

/* The bookmarks in `path`, in order (XdFavorite*). A missing file has
 * none. Free with g_ptr_array_unref. */
GPtrArray *xd_favorites_load(const char *path);

/* What the menu shows: the label, else the folder's name. Free with
 * g_free. */
char *xd_favorite_name(const XdFavorite *f);

/* Adds `uri` at the end; one already there just gets the new label.
 * An empty or nullptr `label` writes none. */
gboolean xd_favorites_add(const char *path, const char *uri, const char *label, GError **err);
/* Gives the bookmark for `uri` a new label (empty or nullptr: none). */
gboolean xd_favorites_rename(const char *path, const char *uri, const char *label, GError **err);
gboolean xd_favorites_remove(const char *path, const char *uri, GError **err);
/* Puts the bookmarks in the order of `uris` (nullptr-terminated); those
 * not named keep their order after them. Other lines stay where they are. */
gboolean xd_favorites_reorder(const char *path, const char *const *uris, GError **err);

G_END_DECLS

#endif
