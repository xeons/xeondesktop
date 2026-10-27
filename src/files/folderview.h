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

/* The Details view's columns, which also sort the view. Arrange Icons by
 * lists the first XD_SORT_ARRANGE_COUNT; the others are shown from the
 * header's right-click menu, as in XP. */
typedef enum {
    XD_SORT_NAME,
    XD_SORT_SIZE,
    XD_SORT_TYPE,
    XD_SORT_MODIFIED,
    XD_SORT_CREATED,
    XD_SORT_ACCESSED,
    XD_SORT_ATTRIBUTES,
    XD_SORT_OWNER,
    XD_SORT_COUNT,
} XdSortColumn;

enum { XD_SORT_ARRANGE_COUNT = XD_SORT_CREATED };

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
int xd_folder_view_get_thumbnail_size(XdFolderView *view);
void xd_folder_view_set_sort(XdFolderView *view, XdSortColumn column, gboolean descending);
XdSortColumn xd_folder_view_get_sort(XdFolderView *view);
gboolean xd_folder_view_get_sort_descending(XdFolderView *view);
void xd_folder_view_set_show_hidden(XdFolderView *view, gboolean show);
/* Arrange Icons by > Show in Groups (the icon views; My Computer always). */
void xd_folder_view_set_groups(XdFolderView *view, gboolean groups);
gboolean xd_folder_view_get_groups(XdFolderView *view);
/* The Details columns (shown, order, widths) as columns.h writes them;
 * nullptr: the defaults. get returns nullptr while they're the defaults
 * (free with g_free). */
void xd_folder_view_set_columns(XdFolderView *view, const char *spec);
char *xd_folder_view_get_columns(XdFolderView *view);
/* Arrange Icons by > Auto Arrange and Align to Grid (the icon views). With
 * Auto Arrange off, icons stay where they're put: dragged within the
 * folder, they move (onto the grid's cells with Align to Grid), and new
 * ones take the first free cell. Show in Groups always arranges. */
void xd_folder_view_set_auto_arrange(XdFolderView *view, gboolean on);
gboolean xd_folder_view_get_auto_arrange(XdFolderView *view);
void xd_folder_view_set_align_to_grid(XdFolderView *view, gboolean on);
gboolean xd_folder_view_get_align_to_grid(XdFolderView *view);
/* The icons' places with Auto Arrange off, by file name (`xy`: x, y
 * pairs in content pixels), for the view mode they were put in; set them
 * before the folder's location. get returns how many (only items still
 * in the folder); free `names` with g_strfreev and `xy` with g_free. */
void xd_folder_view_set_positions(XdFolderView *view, XdViewMode mode, char **names,
                                  const int *xy, gsize count);
gsize xd_folder_view_get_positions(XdFolderView *view, XdViewMode *mode, char ***names, int **xy);
/* Folder Options: names without their known type's extension, and
 * single-click to open an item (pointing underlines it). */
void xd_folder_view_set_hide_extensions(XdFolderView *view, gboolean hide);
void xd_folder_view_set_single_click(XdFolderView *view, gboolean single);

/* The Open and Save As dialogs' "Files of type": only the items the
 * filter keeps are shown (`name` is the file's own name, `content_type`
 * may be nullptr). nullptr shows everything. Call refilter when what the
 * filter keeps changes. */
typedef gboolean (*XdFolderViewFilter)(const char *name, const char *content_type,
                                       gboolean is_directory, gpointer data);
void xd_folder_view_set_filter(XdFolderView *view, XdFolderViewFilter filter, gpointer data);
void xd_folder_view_refilter(XdFolderView *view);
/* Whether more than one item can be selected (the default); with FALSE,
 * Ctrl, Shift, Ctrl+A and dragging select one item only, as an Open
 * dialog's list does. */
void xd_folder_view_set_multiple(XdFolderView *view, gboolean multiple);
/* Shows `files` (GFile*), wherever they are, instead of a folder's
 * contents, under the location `location` (XP's My Recent Documents in
 * the Open dialogs). Not watched for changes. */
void xd_folder_view_set_files(XdFolderView *view, GFile *location, GPtrArray *files);

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
 *   "item-middle-click" (GFile *file, gboolean is_directory) - the middle
 *       button pressed on an item (the selection stays as it was)
 */

G_END_DECLS

#endif
