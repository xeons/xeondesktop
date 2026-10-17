/* SPDX-License-Identifier: MIT
 * XdFolderView: the folder contents view (libxdfolder).
 *
 * A GTK3 widget that draws Windows XP's five views itself (Thumbnails,
 * Tiles, Icons, List, Details) and only lays out and paints what's visible,
 * so large folders stay fast. Contents come from GIO: asynchronous
 * enumeration, a directory monitor for live updates, content types and
 * icons, and thumbnails already in the freedesktop thumbnail cache.
 * Put it in a GtkScrolledWindow; it implements GtkScrollable.
 */
#ifndef XD_FILES_FOLDERVIEW_H
#define XD_FILES_FOLDERVIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define XD_TYPE_FOLDER_VIEW (xd_folder_view_get_type())
G_DECLARE_FINAL_TYPE(XdFolderView, xd_folder_view, XD, FOLDER_VIEW, GtkDrawingArea)

typedef enum {
    XD_VIEW_THUMBNAILS,
    XD_VIEW_TILES,
    XD_VIEW_ICONS,
    XD_VIEW_LIST,
    XD_VIEW_DETAILS,
    XD_VIEW_COUNT,
} XdViewMode;

typedef enum {
    XD_SORT_NAME,
    XD_SORT_SIZE,
    XD_SORT_TYPE,
    XD_SORT_MODIFIED,
    XD_SORT_COUNT,
} XdSortColumn;

enum { XD_THUMB_MIN = 48, XD_THUMB_MAX = 256, XD_THUMB_DEFAULT = 96 };

GtkWidget *xd_folder_view_new(void);

/* Windows-style size: whole kilobytes rounded up, "1,234 KB". Free with
 * g_free. */
char *xd_format_size(guint64 bytes);

/* Shows the contents of `dir` (loaded asynchronously). */
void xd_folder_view_set_location(XdFolderView *view, GFile *dir);
GFile *xd_folder_view_get_location(XdFolderView *view);
void xd_folder_view_reload(XdFolderView *view);

void xd_folder_view_set_mode(XdFolderView *view, XdViewMode mode);
XdViewMode xd_folder_view_get_mode(XdFolderView *view);
/* Thumbnail cell size in px (Thumbnails view), XD_THUMB_MIN..MAX. */
void xd_folder_view_set_thumbnail_size(XdFolderView *view, int px);
void xd_folder_view_set_sort(XdFolderView *view, XdSortColumn column, gboolean descending);
XdSortColumn xd_folder_view_get_sort(XdFolderView *view);
gboolean xd_folder_view_get_sort_descending(XdFolderView *view);
void xd_folder_view_set_show_hidden(XdFolderView *view, gboolean show);
/* Arrange Icons by > Show in Groups (the icon views; My Computer always). */
void xd_folder_view_set_groups(XdFolderView *view, gboolean groups);
gboolean xd_folder_view_get_groups(XdFolderView *view);

void xd_folder_view_select_all(XdFolderView *view);
void xd_folder_view_invert_selection(XdFolderView *view);
guint xd_folder_view_get_item_count(XdFolderView *view);
guint xd_folder_view_get_selected_count(XdFolderView *view);
/* Total size of selected files, or of all files when nothing is selected. */
guint64 xd_folder_view_get_size(XdFolderView *view, gboolean selected_only);
/* The selected items (GFile*, in view order). Free with
 * g_list_free_full(list, g_object_unref). */
GList *xd_folder_view_get_selected_files(XdFolderView *view);

/* Selects these files (GFile*), now or as soon as they appear (after a
 * copy, say), until the user changes the selection. */
void xd_folder_view_select_files(XdFolderView *view, GList *files);
/* Where `file`'s name is drawn, in widget coordinates (scrolled into
 * view first), for renaming in place. FALSE when it isn't shown. */
gboolean xd_folder_view_get_name_area(XdFolderView *view, GFile *file, GdkRectangle *area);
/* Files cut to the clipboard, drawn faded (nullptr: none). */
void xd_folder_view_set_cut_files(XdFolderView *view, GList *files);

/* Signals:
 *   "item-activated" (GFile *file, gboolean is_directory)
 *   "selection-changed" ()
 *   "contents-changed" () - loading finished or the folder changed
 *   "load-error" (const char *message)
 *   "context-menu" (GdkEvent *event) - right-click (after updating the
 *       selection) or the Menu key / Shift+F10, when event is NULL
 *   "files-dropped" (GStrv uris, GFile *folder, GdkDragAction action) -
 *       files dropped into the folder (or a folder item), with the action
 *       Windows would take (copy, move or shortcut)
 */

G_END_DECLS

#endif
