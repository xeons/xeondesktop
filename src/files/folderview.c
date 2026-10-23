/* SPDX-License-Identifier: MIT */
#include <gio/gdesktopappinfo.h>
#include <math.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "../thumbnailer/queue.h"
#include "columns.h"
#include "folderview.h"
#include "names.h"
#include "netdrives.h"
#include "places.h"
#include "udisks.h"
#include "usershare.h"

#define ATTRIBUTES                                                             \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME  \
    "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE      \
    "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_ICON \
    "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP \
    "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK \
    "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_THUMBNAIL_PATH \
    "," G_FILE_ATTRIBUTE_THUMBNAIL_IS_VALID \
    "," G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_TIME_ACCESS     \
    "," G_FILE_ATTRIBUTE_OWNER_USER "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE

/* Layout in px (GTK logical pixels). */
static constexpr int PAD = 6;
static constexpr int ROW_H = 18;
static constexpr int HEADER_H = 22;
static constexpr int LIST_ICON = 16;
static constexpr int ICONS_W = 80;
static constexpr int ICONS_H = 78;
static constexpr int TILE_W = 250;
static constexpr int TILE_H = 64;
static constexpr int LIST_COL_MAX = 300;
static constexpr int BATCH = 256;
static constexpr int GROUP_H = 26;   /* a group's heading */
static constexpr int GROUP_GAP = 8;  /* between a group and the next heading */
static constexpr int COL_MIN = 10;   /* the narrowest a column is dragged */
static constexpr int DIVIDER_HIT = 4; /* px either side of a header divider */

typedef struct item {
    GFile *file;
    char *name;
    char *short_name; /* without a known type's extension; nullptr: no extension to hide */
    char *collate_key;
    char *type_desc;
    char *size_text;
    char *date_text;
    char *key; /* the file's own name: where it was put (Auto Arrange off) */
    guint64 size;
    gint64 mtime;
    /* The other Details columns; the dates are formatted when shown. */
    gint64 ctime, atime; /* 0: unknown */
    char *created_text, *accessed_text;
    char *attrs; /* XP's letters: R (read-only), H (hidden) */
    char *owner;
    gboolean is_dir;
    gboolean hidden;
    GIcon *icon;
    char *thumb_path;
    GdkPixbuf *icon_pb;
    int icon_px;
    GdkPixbuf *thumb_pb;
    int thumb_px;
    /* xd-thumbnailer can make its thumbnail; the cached one is of an older
     * version of the file; making one failed. */
    gboolean thumb_want, thumb_stale, thumb_failed;
    int name_w; /* List view: measured label width, 0 = not yet */
    gboolean selected;
    gboolean shortcut; /* a symbolic link or a launcher: XP's arrow */
    gboolean shared;   /* a folder shared on the network: XP's hand */
    int order;    /* My Computer: the drives' own order */
    char *extra;  /* Tiles' third line instead of the size (a drive's free space) */
    /* Show in Groups: the heading it's under, and the groups' order. */
    char *group;
    int group_rank;
} item;

/* A group's heading in the grouped icon views (content coordinates). */
typedef struct heading {
    int y;
    char *text;
} heading;

struct _XdFolderView {
    GtkDrawingArea parent;
    GtkAdjustment *hadj, *vadj;
    gulong hadj_id, vadj_id;
    gulong theme_id; /* the icon theme's "changed" */
    GtkScrollablePolicy hpolicy, vpolicy;

    GFile *dir;
    /* Special folders: My Computer lists drives; the Recycle Bin lists
     * the trash's files folder (`listed`) under their original names;
     * Control Panel lists categories or applets. */
    XdPlace place;
    GFile *listed;
    GPtrArray *more_listed; /* the Recycle Bin: other drives' trashes still to list */
    gboolean watching_drives;
    GAppInfoMonitor *apps_monitor; /* Control Panel: applets installed or removed */
    GCancellable *cancel;
    GFileMonitor *monitor;
    guint reload_source;
    gboolean reload_again; /* the folder changed while it was loading */
    GPtrArray *all;   /* item*, owned */
    /* Items being listed (owned), which replace `all` once they're all
     * in: a refresh keeps showing the old ones until then. */
    GPtrArray *loading;
    GPtrArray *shown; /* item*, borrowed: filtered and sorted */

    XdViewMode mode;
    int thumb;
    XdThumbQueue *thumbs; /* thumbnails being made, for the Thumbnails view */
    XdSortColumn sort;
    gboolean desc;
    gboolean show_hidden;
    gboolean groups;  /* Show in Groups */
    gboolean grouped; /* laid out in groups now (an icon view) */
    GArray *rects;    /* GdkRectangle per shown item, when grouped */
    GArray *headings; /* heading, when grouped */
    gboolean hide_extensions;
    /* Single-click to open: the item under the pointer is underlined,
     * and the one pressed opens on release. */
    gboolean single_click;
    int hover;
    int pressed;
    gboolean underline_label; /* while drawing the hovered item */
    int focus;
    int anchor;

    /* A press on an item that may become a drag (the selection's files,
     * as a URI list, to Quick Launch or another program). */
    gboolean drag_pending;
    double drag_x, drag_y;
    guint drag_button;

    /* Rubber-band selection (content coordinates), and the selection it
     * started from (Ctrl adds to it). */
    gboolean banding;
    double band_x0, band_y0, band_x1, band_y1;
    GArray *band_base; /* gboolean per shown item */
    gboolean band_toggle;

    /* Files to select once they're loaded (URIs), and the one with the
     * focus: the selection across a reload, or files a copy is about to
     * create (`keep_sticky`: until they appear). */
    GHashTable *keep;
    char *keep_focus;
    gboolean keep_sticky;
    /* Cut to the clipboard: drawn faded until pasted (URIs). */
    GHashTable *cut;

    /* A drag over us: its URIs (asked for on entering), the folder under
     * the pointer it would drop into (-1: this one), and whether the drop
     * is waiting for the data. */
    char **drag_uris;
    gboolean drag_requested;
    gboolean dropping;
    int drop_index;

    /* Layout (content coordinates). */
    int cell_w, cell_h, cols, rows;
    int content_w, content_h;
    /* Details: the columns shown, left to right (`ncols`), the widths
     * chosen (0: the default; Name's fills the window) and the widths
     * laid out (0 for those not shown). */
    int order[XD_SORT_COUNT];
    int ncols;
    int width[XD_SORT_COUNT];
    int col_w[XD_SORT_COUNT];

    /* The header: a divider being dragged (its column's width), or a
     * column pressed, maybe dragged elsewhere (-1: none). */
    int hdr_resize;
    int hdr_resize_w;
    int hdr_press;
    gboolean hdr_dragging;
    double hdr_press_x, hdr_x;
    gboolean hdr_cursor; /* the resize pointer is showing */

    /* Auto Arrange off: where each item is (file name -> packed point,
     * content coordinates), for the view mode they were put in, and
     * whether the view is laid out that way now (`rects` holds them). */
    gboolean auto_arrange;
    gboolean align_grid;
    GHashTable *positions;
    XdViewMode positions_mode;
    gboolean placed;
    gboolean positions_given; /* for the location set next */
    double drag_cx, drag_cy; /* the press that started a drag, in content */

    /* A drag over us, while it's here (its last motion): Ctrl, Shift and
     * Alt pressed or released without moving the pointer change the
     * action too. */
    GdkEvent *over_event;
    GdkKeymap *keymap;
    gulong keymap_id;
};

enum { PROP_0, PROP_HADJ, PROP_VADJ, PROP_HPOLICY, PROP_VPOLICY };
enum {
    SIG_ACTIVATED,
    SIG_SELECTION,
    SIG_CONTENTS,
    SIG_ERROR,
    SIG_CONTEXT,
    SIG_DROPPED,
    SIG_MIDDLE,
    SIG_COUNT
};
static guint signals[SIG_COUNT];

G_DEFINE_TYPE_WITH_CODE(XdFolderView, xd_folder_view, GTK_TYPE_DRAWING_AREA,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, nullptr))

/* ---- items --------------------------------------------------------------------- */

/* The name as shown (Folder Options can hide known extensions). */
static const char *item_label(const XdFolderView *folder_view, const item *it) {
    return folder_view->hide_extensions && it->short_name != nullptr ? it->short_name : it->name;
}

static void item_free(gpointer user_data) {
    item *it = user_data;
    g_clear_object(&it->file);
    g_clear_object(&it->icon);
    g_clear_object(&it->icon_pb);
    g_clear_object(&it->thumb_pb);
    g_free(it->name);
    g_free(it->short_name);
    g_free(it->collate_key);
    g_free(it->type_desc);
    g_free(it->size_text);
    g_free(it->date_text);
    g_free(it->key);
    g_free(it->created_text);
    g_free(it->accessed_text);
    g_free(it->attrs);
    g_free(it->owner);
    g_free(it->thumb_path);
    g_free(it->extra);
    g_free(it->group);
    xd_free(it);
}

char *xd_format_size(guint64 size) {
    guint64 kb = (size + 1023) / 1024;
    if (size > 0 && kb == 0)
        kb = 1;
    char *digits = g_strdup_printf("%" G_GUINT64_FORMAT, kb);
    /* Thousands separators. */
    GString *out = g_string_new(nullptr);
    size_t count = strlen(digits);
    for (size_t i = 0; i < count; i++) {
        g_string_append_c(out, digits[i]);
        if ((count - i - 1) % 3 == 0 && i + 1 < count)
            g_string_append_c(out, ',');
    }
    g_string_append(out, " KB");
    g_free(digits);
    return g_string_free(out, FALSE);
}

static item *item_new(GFile *dir, GFileInfo *info) {
    item *it = XD_NEW(item);
    it->file = g_file_get_child(dir, g_file_info_get_name(info));
    it->key = g_strdup(g_file_info_get_name(info));
    it->name = g_strdup(g_file_info_get_display_name(info));
    it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
    it->is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    it->hidden = g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info);
    const char *kind = g_file_info_get_content_type(info);
    bool launcher = kind != nullptr && g_content_type_is_a(kind, "application/x-desktop");
    it->shortcut = g_file_info_get_is_symlink(info) || launcher;
    it->size = (guint64)g_file_info_get_size(info);
    GDateTime *dt = g_file_info_get_modification_date_time(info);
    if (dt != nullptr) {
        it->mtime = g_date_time_to_unix(dt);
        GDateTime *local = g_date_time_to_local(dt);
        it->date_text = g_date_time_format(local, "%x %H:%M");
        g_date_time_unref(local);
        g_date_time_unref(dt);
    }
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_CREATED))
        it->ctime = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CREATED);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_ACCESS))
        it->atime = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_ACCESS);
    const char *owner = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER);
    it->owner = g_strdup(owner != nullptr ? owner : "");
    /* Read-only: a file we can't write (XP doesn't mark folders so). */
    gboolean ro = !it->is_dir && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) &&
                  !g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
    it->attrs = g_strconcat(ro ? "R" : "", it->hidden ? "H" : "", nullptr);
    const char *ctype = g_file_info_get_content_type(info);
    it->type_desc = it->is_dir ? g_strdup("File Folder")
                    : ctype != nullptr ? g_content_type_get_description(ctype)
                                       : g_strdup("File");
    it->size_text = it->is_dir ? g_strdup("") : xd_format_size(it->size);
    /* The extension is the known type's when the name alone says so. */
    size_t base = xd_name_base_len(it->name);
    if (!it->is_dir && !launcher && ctype != nullptr && it->name[base] != '\0') {
        char *guess = g_content_type_guess(it->name, nullptr, 0, nullptr);
        if (!g_content_type_is_unknown(guess) && g_content_type_is_a(ctype, guess))
            it->short_name = g_strndup(it->name, base);
        g_free(guess);
    }
    GIcon *icon = g_file_info_get_icon(info);
    it->icon = icon != nullptr ? g_object_ref(icon) : nullptr;
    /* A launcher shows its program's name and icon, as Windows shows a
     * shortcut's name without ".lnk". */
    char *path = launcher ? g_file_get_path(it->file) : nullptr;
    GDesktopAppInfo *app = path != nullptr ? g_desktop_app_info_new_from_filename(path) : nullptr;
    if (app != nullptr) {
        g_free(it->name);
        it->name = g_strdup(g_app_info_get_name(G_APP_INFO(app)));
        g_free(it->collate_key);
        it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
        g_free(it->type_desc);
        it->type_desc = g_strdup("Shortcut");
        GIcon *app_icon = g_app_info_get_icon(G_APP_INFO(app));
        if (app_icon != nullptr)
            g_set_object(&it->icon, app_icon);
        g_object_unref(app);
    }
    g_free(path);
    const char *thumb = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
    it->thumb_path = thumb != nullptr ? g_strdup(thumb) : nullptr;
    it->thumb_stale = thumb != nullptr &&
                      g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_THUMBNAIL_IS_VALID) &&
                      !g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_THUMBNAIL_IS_VALID);
    it->thumb_want = !it->is_dir && !launcher && g_file_is_native(it->file) &&
                     xd_thumb_supported(ctype);
    return it;
}

static GdkPixbuf *item_icon(XdFolderView *folder_view, item *it, int px) {
    if (it->icon_pb != nullptr && it->icon_px == px)
        return it->icon_pb;
    g_clear_object(&it->icon_pb);
    it->icon_px = px;
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    /* Between 32 and 64 (Tiles, Thumbnails), ask for 64 and draw that at
     * px: Breeze has its 16px icons at 3x but few 48px ones, and GTK3
     * takes a 16px monochrome outline as an exact match for 48. */
    int ask = px > 32 && px < 64 ? 64 : px;
    GtkIconInfo *info = it->icon != nullptr
                            ? gtk_icon_theme_lookup_by_gicon(theme, it->icon, ask,
                                                             GTK_ICON_LOOKUP_FORCE_SIZE)
                            : nullptr;
    if (info == nullptr)
        info = gtk_icon_theme_lookup_icon(theme, it->is_dir ? "folder" : "text-x-generic", ask,
                                          GTK_ICON_LOOKUP_FORCE_SIZE);
    if (info != nullptr) {
        const char *file = gtk_icon_info_get_filename(info);
        if (ask != px && file != nullptr && g_str_has_suffix(file, ".svg"))
            it->icon_pb = gdk_pixbuf_new_from_file_at_size(file, px, px, nullptr);
        if (it->icon_pb == nullptr) {
            GdkPixbuf *pb = gtk_icon_info_load_icon(info, nullptr);
            it->icon_pb = pb != nullptr && ask != px
                              ? gdk_pixbuf_scale_simple(pb, px, px, GDK_INTERP_BILINEAR)
                              : (pb != nullptr ? g_object_ref(pb) : nullptr);
            g_clear_object(&pb);
        }
        g_object_unref(info);
    }
    (void)folder_view;
    return it->icon_pb;
}

/* Another icon theme (Display Properties): every item's icon is drawn again
 * from it, as the rest of the window already is. */
static void icon_theme_changed([[maybe_unused]] GtkIconTheme *theme, gpointer data) {
    XdFolderView *folder_view = data;
    for (guint i = 0; i < folder_view->all->len; i++) {
        item *it = g_ptr_array_index(folder_view->all, i);
        g_clear_object(&it->icon_pb);
        it->icon_px = 0;
    }
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}

/* A thumbnail xd-thumbnailer made (nullptr `path`: it couldn't): the item
 * shows it, if it's still here. */
static void thumb_done(const char *uri, const char *path, gpointer data) {
    XdFolderView *folder_view = data;
    GFile *file = g_file_new_for_uri(uri);
    for (guint i = 0; i < folder_view->all->len; i++) {
        item *it = g_ptr_array_index(folder_view->all, i);
        if (!g_file_equal(it->file, file))
            continue;
        if (path != nullptr) {
            g_free(it->thumb_path);
            it->thumb_path = g_strdup(path);
            it->thumb_stale = FALSE;
            g_clear_object(&it->thumb_pb);
            it->thumb_px = 0;
        } else {
            it->thumb_failed = TRUE;
        }
        gtk_widget_queue_draw(GTK_WIDGET(folder_view));
        break;
    }
    g_object_unref(file);
}

/* Asks for a thumbnail the cache doesn't have (or has only smaller, or of
 * an older version of the file). Asked while drawing, so what's on screen
 * is asked for last, and made first. */
static void want_thumb(XdFolderView *folder_view, item *it, int px) {
    if (!it->thumb_want || it->thumb_failed)
        return;
    XdThumbSize size = xd_thumb_size_for(px);
    if (it->thumb_path != nullptr && !it->thumb_stale &&
        (size == XD_THUMB_NORMAL || strstr(it->thumb_path, "/normal/") == nullptr))
        return;
    if (folder_view->thumbs == nullptr)
        folder_view->thumbs = xd_thumb_queue_new(thumb_done, folder_view);
    char *uri = g_file_get_uri(it->file);
    xd_thumb_queue_request(folder_view->thumbs, uri, size);
    g_free(uri);
}

/* A cached thumbnail scaled to fit px x px, or nullptr (one is asked for:
 * the item is drawn again when it's made). */
static GdkPixbuf *item_thumb(XdFolderView *folder_view, item *it, int px) {
    want_thumb(folder_view, it, px);
    if (it->thumb_path == nullptr)
        return nullptr;
    if (it->thumb_pb != nullptr && it->thumb_px == px)
        return it->thumb_pb;
    g_clear_object(&it->thumb_pb);
    it->thumb_px = px;
    it->thumb_pb = gdk_pixbuf_new_from_file_at_scale(it->thumb_path, px, px, TRUE, nullptr);
    if (it->thumb_pb == nullptr)
        g_clear_pointer(&it->thumb_path, g_free); /* don't retry a broken one */
    return it->thumb_pb;
}

/* ---- sorting and filtering ------------------------------------------------------- */

/* ---- groups -------------------------------------------------------------------------- */

static gboolean is_icon_mode(XdViewMode view_mode) {
    return view_mode == XD_VIEW_THUMBNAILS || view_mode == XD_VIEW_ICONS ||
           view_mode == XD_VIEW_TILES;
}

/* My Computer always shows its groups, as XP does; other folders when
 * Show in Groups is on. The icon views and Details have them; XP's List
 * view never shows groups (its list control can't). */
static gboolean wants_groups(const XdFolderView *folder_view) {
    return (is_icon_mode(folder_view->mode) || folder_view->mode == XD_VIEW_DETAILS) &&
           (folder_view->place == XD_PLACE_COMPUTER || folder_view->groups);
}

/* XP's headings: My Computer by kind of drive; folders by the sort
 * column (initial letter, type, size class, or how long ago). */
static void set_group(const XdFolderView *folder_view, item *it, GDateTime *now) {
    g_free(it->group);
    it->group = nullptr;
    it->group_rank = 0;
    if (folder_view->place == XD_PLACE_COMPUTER) {
        static const struct {
            const char *kind, *group;
        } kinds[] = {
            {"File Folder", "Files Stored on This Computer"},
            {"Local Disk", "Hard Disk Drives"},
            {"Removable Disk", "Devices with Removable Storage"},
            {"CD Drive", "Devices with Removable Storage"},
            {"Network Drive", "Network Drives"},
            {XD_DISCONNECTED_DRIVE, "Network Drives"},
        };
        it->group_rank = (int)G_N_ELEMENTS(kinds);
        it->group = g_strdup("Other");
        for (size_t k = 0; k < G_N_ELEMENTS(kinds); k++) {
            if (g_strcmp0(it->type_desc, kinds[k].kind) == 0) {
                g_free(it->group);
                it->group = g_strdup(kinds[k].group);
                /* CDs with removables, disconnected drives with the
                 * connected ones. */
                it->group_rank = k >= 5 ? (int)k - 2 : k >= 3 ? (int)k - 1 : (int)k;
                break;
            }
        }
        return;
    }
    switch (folder_view->sort) {
    case XD_SORT_TYPE:
        it->group = g_strdup(it->type_desc != nullptr ? it->type_desc : "");
        return;
    case XD_SORT_ATTRIBUTES:
        it->group = g_strdup(it->attrs != nullptr ? it->attrs : "");
        return;
    case XD_SORT_OWNER:
        it->group = g_strdup(it->owner != nullptr ? it->owner : "");
        return;
    case XD_SORT_SIZE: {
        static const struct {
            guint64 below;
            const char *name;
        } sizes[] = {
            {10 * 1024, "Tiny"},
            {100 * 1024, "Small"},
            {1024 * 1024, "Medium"},
            {16 * 1024 * 1024, "Large"},
            {G_MAXUINT64, "Huge"},
        };
        if (it->is_dir) {
            it->group = g_strdup("Folders");
            return;
        }
        for (size_t k = 0; k < G_N_ELEMENTS(sizes); k++) {
            if (it->size < sizes[k].below) {
                it->group = g_strdup(sizes[k].name);
                it->group_rank = (int)k + 1;
                return;
            }
        }
        return;
    }
    case XD_SORT_MODIFIED:
    case XD_SORT_CREATED:
    case XD_SORT_ACCESSED: {
        /* Newest groups last, as the dates sort; descending flips both. */
        static const char *const ages[] = {"A Long Time Ago", "Earlier This Year", "Last Month",
                                           "Earlier This Month", "Last Week", "Earlier This Week",
                                           "Yesterday", "Today"};
        gint64 when = folder_view->sort == XD_SORT_CREATED    ? it->ctime
                      : folder_view->sort == XD_SORT_ACCESSED ? it->atime
                                                              : it->mtime;
        GDateTime *date_time = g_date_time_new_from_unix_local(when);
        int rank = 0;
        if (date_time != nullptr && now != nullptr) {
            int y = g_date_time_get_year(now), ty = g_date_time_get_year(date_time);
            int doy = g_date_time_get_day_of_year(now),
                tdoy = g_date_time_get_day_of_year(date_time);
            int days = (y - ty) * 365 + (doy - tdoy);
            int dow = g_date_time_get_day_of_week(now) - 1; /* Monday 0 */
            int month = g_date_time_get_month(now), tm = g_date_time_get_month(date_time);
            if (days <= 0)
                rank = 7;
            else if (days == 1)
                rank = 6;
            else if (days <= dow)
                rank = 5;
            else if (days <= dow + 7)
                rank = 4;
            else if (y == ty && month == tm)
                rank = 3;
            else if ((y == ty && month - tm == 1) || (y - ty == 1 && month == 1 && tm == 12))
                rank = 2;
            else if (y == ty)
                rank = 1;
        }
        if (date_time != nullptr)
            g_date_time_unref(date_time);
        it->group = g_strdup(ages[rank]);
        it->group_rank = rank;
        return;
    }
    case XD_SORT_NAME:
    case XD_SORT_COUNT:
        break;
    }
    /* By name: the initial letter. */
    gunichar initial = g_utf8_get_char_validated(it->name, -1);
    if (initial != (gunichar)-1 && initial != (gunichar)-2 && g_unichar_isalpha(initial)) {
        char buf[8] = {0};
        g_unichar_to_utf8(g_unichar_toupper(initial), buf);
        it->group = g_strdup(buf);
    } else {
        it->group = g_strdup("Other");
        it->group_rank = -1;
    }
}

/* Groups in their own order (sizes, ages) rather than by heading. */
static gboolean ranked_groups(XdSortColumn sort) {
    return sort == XD_SORT_SIZE || sort == XD_SORT_MODIFIED || sort == XD_SORT_CREATED ||
           sort == XD_SORT_ACCESSED;
}

static int compare_groups(const XdFolderView *folder_view, const item *left, const item *right) {
    int order = (left->group_rank > right->group_rank) - (left->group_rank < right->group_rank);
    if (order == 0 && folder_view->place != XD_PLACE_COMPUTER && !ranked_groups(folder_view->sort))
        order = g_utf8_collate(left->group != nullptr ? left->group : "",
                               right->group != nullptr ? right->group : "");
    return folder_view->place != XD_PLACE_COMPUTER && folder_view->desc ? -order : order;
}

static int compare_items(gconstpointer pa, gconstpointer pb, gpointer data) {
    const item *left = *(const item *const *)pa;
    const item *right = *(const item *const *)pb;
    const XdFolderView *folder_view = data;
    if (folder_view->grouped) {
        int group_order = compare_groups(folder_view, left, right);
        if (group_order != 0)
            return group_order;
    }
    /* My Computer's drives and Control Panel's categories keep their own
     * order. */
    if (folder_view->place == XD_PLACE_COMPUTER || folder_view->place == XD_PLACE_NETWORK ||
        (folder_view->place == XD_PLACE_CONTROL && left->is_dir && right->is_dir))
        return left->order - right->order;
    /* Folders first, always, as in Windows. */
    if (left->is_dir != right->is_dir)
        return left->is_dir ? -1 : 1;
    int order = 0;
    switch (folder_view->sort) {
    case XD_SORT_SIZE:
        order = (left->size > right->size) - (left->size < right->size);
        break;
    case XD_SORT_TYPE:
        order = g_utf8_collate(left->type_desc, right->type_desc);
        break;
    case XD_SORT_MODIFIED:
        order = (left->mtime > right->mtime) - (left->mtime < right->mtime);
        break;
    case XD_SORT_CREATED:
        order = (left->ctime > right->ctime) - (left->ctime < right->ctime);
        break;
    case XD_SORT_ACCESSED:
        order = (left->atime > right->atime) - (left->atime < right->atime);
        break;
    case XD_SORT_ATTRIBUTES:
        order = g_strcmp0(left->attrs, right->attrs);
        break;
    case XD_SORT_OWNER:
        order = g_utf8_collate(left->owner != nullptr ? left->owner : "",
                               right->owner != nullptr ? right->owner : "");
        break;
    case XD_SORT_NAME:
    case XD_SORT_COUNT:
        break;
    }
    if (order == 0)
        order = strcmp(left->collate_key, right->collate_key);
    return folder_view->desc ? -order : order;
}

static void layout_contents(XdFolderView *folder_view);

static void refilter(XdFolderView *folder_view) {
    g_ptr_array_set_size(folder_view->shown, 0);
    folder_view->grouped = wants_groups(folder_view);
    GDateTime *now = folder_view->grouped ? g_date_time_new_now_local() : nullptr;
    for (guint i = 0; i < folder_view->all->len; i++) {
        item *it = g_ptr_array_index(folder_view->all, i);
        if (folder_view->grouped)
            set_group(folder_view, it, now);
        if (folder_view->show_hidden || !it->hidden)
            g_ptr_array_add(folder_view->shown, it);
        else
            it->selected = FALSE;
    }
    if (now != nullptr)
        g_date_time_unref(now);
    g_ptr_array_sort_with_data(folder_view->shown, compare_items, folder_view);
    if (folder_view->focus >= (int)folder_view->shown->len)
        folder_view->focus = (int)folder_view->shown->len - 1;
    layout_contents(folder_view);
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}

/* ---- layout ------------------------------------------------------------------------ */

/* floor() to int, clamped to a sane coordinate range. */
static int ifloor(double value) {
    double floored = floor(value);
    if (!(floored > -1e7))
        return -10000000;
    if (!(floored < 1e7))
        return 10000000;
    return (int)floored;
}

static int widget_w(XdFolderView *folder_view) {
    return MAX(gtk_widget_get_allocated_width(GTK_WIDGET(folder_view)), 1);
}
static int widget_h(XdFolderView *folder_view) {
    return MAX(gtk_widget_get_allocated_height(GTK_WIDGET(folder_view)), 1);
}

static int text_width(XdFolderView *folder_view, const char *text) {
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(folder_view), text);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    g_object_unref(layout);
    return width;
}

static void configure(GtkAdjustment *adjustment, double page, double upper) {
    if (adjustment == nullptr)
        return;
    upper = MAX(upper, page);
    double value = CLAMP(gtk_adjustment_get_value(adjustment), 0.0, upper - page);
    gtk_adjustment_configure(adjustment, value, 0.0, upper, ROW_H * 2.0, page * 0.9, page);
}

static void clear_headings(XdFolderView *folder_view) {
    for (guint i = 0; i < folder_view->headings->len; i++)
        g_free(g_array_index(folder_view->headings, heading, i).text);
    g_array_set_size(folder_view->headings, 0);
}

/* The icon views in groups: each under its heading, starting a new row. */
static void layout_groups(XdFolderView *folder_view) {
    int count = (int)folder_view->shown->len;
    g_array_set_size(folder_view->rects, (guint)count);
    clear_headings(folder_view);
    int y = PAD, col = 0;
    const char *current = nullptr;
    for (int i = 0; i < count; i++) {
        item *it = g_ptr_array_index(folder_view->shown, (guint)i);
        const char *group_name = it->group != nullptr ? it->group : "";
        if (current == nullptr || strcmp(group_name, current) != 0) {
            if (current != nullptr)
                y += (col > 0 ? folder_view->cell_h : 0) + GROUP_GAP;
            heading hd = {y, g_strdup(group_name)};
            g_array_append_val(folder_view->headings, hd);
            y += GROUP_H;
            col = 0;
            current = group_name;
        }
        g_array_index(folder_view->rects, GdkRectangle, i) = (GdkRectangle){
            PAD + col * folder_view->cell_w, y, folder_view->cell_w, folder_view->cell_h};
        if (++col == folder_view->cols) {
            col = 0;
            y += folder_view->cell_h;
        }
    }
    folder_view->content_h = y + (col > 0 ? folder_view->cell_h : 0) + PAD;
}

/* Details in groups: each heading on a line of its own above its rows,
 * under the column header. */
static void layout_detail_groups(XdFolderView *folder_view) {
    int count = (int)folder_view->shown->len;
    g_array_set_size(folder_view->rects, (guint)count);
    clear_headings(folder_view);
    int y = HEADER_H;
    const char *current = nullptr;
    for (int i = 0; i < count; i++) {
        item *it = g_ptr_array_index(folder_view->shown, (guint)i);
        const char *group_name = it->group != nullptr ? it->group : "";
        if (current == nullptr || strcmp(group_name, current) != 0) {
            if (current != nullptr)
                y += GROUP_GAP;
            heading hd = {y, g_strdup(group_name)};
            g_array_append_val(folder_view->headings, hd);
            y += GROUP_H;
            current = group_name;
        }
        g_array_index(folder_view->rects, GdkRectangle, i) =
            (GdkRectangle){0, y, folder_view->cell_w, ROW_H};
        y += ROW_H;
    }
    folder_view->content_h = y + PAD;
}

/* ---- Details columns ---------------------------------------------------------------- */

static const char *const folder_titles[XD_SORT_COUNT] = {
    "Name", "Size", "Type", "Date Modified", "Date Created", "Date Accessed", "Attributes", "Owner"};

static const char *column_title(const XdFolderView *folder_view, int col) {
    static const char *const computer_titles[] = {"Name", "Total Size", "Type", "Free Space"};
    static const char *const trash_titles[] = {"Name", "Size", "Original Location",
                                               "Date Deleted"};
    static const char *const control_titles[] = {"Name", "", "Comments", ""};
    static const char *const network_titles[] = {"Name", "", "Comments", "Network Location"};
    if (col < 0 || col >= XD_SORT_COUNT)
        return "";
    if (col <= XD_SORT_MODIFIED) {
        switch (folder_view->place) {
        case XD_PLACE_COMPUTER:
            return computer_titles[col];
        case XD_PLACE_TRASH:
            return trash_titles[col];
        case XD_PLACE_CONTROL:
            return control_titles[col];
        case XD_PLACE_NETWORK:
            return network_titles[col];
        case XD_PLACE_FOLDER:
            break;
        }
    }
    return folder_titles[col];
}

/* The columns a place has: My Computer and the Recycle Bin their four,
 * Control Panel Name and Comments, folders all of them. */
static gboolean column_available(const XdFolderView *folder_view, int col) {
    switch (folder_view->place) {
    case XD_PLACE_FOLDER:
        return col >= 0 && col < XD_SORT_COUNT;
    case XD_PLACE_CONTROL:
        return col == XD_SORT_NAME || col == XD_SORT_TYPE;
    case XD_PLACE_NETWORK:
        return col == XD_SORT_NAME || col == XD_SORT_TYPE || col == XD_SORT_MODIFIED;
    case XD_PLACE_COMPUTER:
    case XD_PLACE_TRASH:
        break;
    }
    return col >= 0 && col <= XD_SORT_MODIFIED;
}

/* Windows' default widths; Name takes the spare room (0). */
static int default_width(const XdFolderView *folder_view, int col, int view_w) {
    switch ((XdSortColumn)col) {
    case XD_SORT_SIZE:
        return 80;
    case XD_SORT_TYPE:
        return folder_view->place == XD_PLACE_CONTROL ? MAX(260, view_w * 3 / 5) : 150;
    case XD_SORT_MODIFIED:
    case XD_SORT_CREATED:
    case XD_SORT_ACCESSED:
        return 130;
    case XD_SORT_ATTRIBUTES:
        return 70;
    case XD_SORT_OWNER:
        return 120;
    case XD_SORT_NAME:
    case XD_SORT_COUNT:
        break;
    }
    return 0;
}

static void default_columns(XdFolderView *folder_view) {
    folder_view->ncols = 0;
    for (int column = 0; column <= XD_SORT_MODIFIED; column++)
        folder_view->order[folder_view->ncols++] = column;
    memset(folder_view->width, 0, sizeof(folder_view->width));
}

/* Where column `col` starts (content coordinates). */
static int column_x(const XdFolderView *folder_view, int col) {
    int x = 0;
    for (int i = 0; i < folder_view->ncols && folder_view->order[i] != col; i++)
        x += folder_view->col_w[folder_view->order[i]];
    return x;
}

/* A date for a Details column, formatted the first time it's shown. */
static const char *date_cell(char **text, gint64 when) {
    if (*text == nullptr) {
        GDateTime *date_time = when != 0 ? g_date_time_new_from_unix_local(when) : nullptr;
        *text = date_time != nullptr ? g_date_time_format(date_time, "%x %H:%M") : g_strdup("");
        if (date_time != nullptr)
            g_date_time_unref(date_time);
    }
    return *text;
}

static const char *or_empty(const char *text) { return text != nullptr ? text : ""; }

static const char *cell_text(item *it, int col) {
    switch ((XdSortColumn)col) {
    case XD_SORT_NAME:
        return it->name;
    case XD_SORT_SIZE:
        return or_empty(it->size_text);
    case XD_SORT_TYPE:
        return or_empty(it->type_desc);
    case XD_SORT_MODIFIED:
        return or_empty(it->date_text);
    case XD_SORT_CREATED:
        return date_cell(&it->created_text, it->ctime);
    case XD_SORT_ACCESSED:
        return date_cell(&it->accessed_text, it->atime);
    case XD_SORT_ATTRIBUTES:
        return or_empty(it->attrs);
    case XD_SORT_OWNER:
        return or_empty(it->owner);
    case XD_SORT_COUNT:
        break;
    }
    return "";
}

/* ---- free placement (Auto Arrange off) --------------------------------------------- */

static gboolean uses_rects(const XdFolderView *folder_view) {
    return folder_view->grouped || folder_view->placed;
}

static constexpr int GRID_COLS_MAX = 4095;

static gpointer cell_key(int row, int col) {
    return GUINT_TO_POINTER(((guint)row << 12 | (guint)col) + 1u);
}

static int grid_cols(const XdFolderView *folder_view) {
    return CLAMP(folder_view->cols, 1, GRID_COLS_MAX);
}

/* Marks the grid cells `area` covers. */
static void take_cells(const XdFolderView *folder_view, GHashTable *taken, GdkRectangle area) {
    int c0 = MAX((area.x - PAD) / folder_view->cell_w, 0),
        c1 = (area.x + area.width - 1 - PAD) / folder_view->cell_w;
    int r0 = MAX((area.y - PAD) / folder_view->cell_h, 0),
        r1 = (area.y + area.height - 1 - PAD) / folder_view->cell_h;
    for (int row = r0; row <= r1; row++)
        for (int col = c0; col <= c1 && col < grid_cols(folder_view); col++)
            g_hash_table_add(taken, cell_key(row, col));
}

/* The first free cell from `start` on (row by row): its index. */
static int free_cell(const XdFolderView *folder_view, GHashTable *taken, int start) {
    int cols = grid_cols(folder_view);
    int cell = MAX(start, 0);
    while (g_hash_table_contains(taken, cell_key(cell / cols, cell % cols)))
        cell++;
    return cell;
}

static void set_position(XdFolderView *folder_view, const char *key, int x, int y) {
    int *position = g_new(int, 2);
    position[0] = x;
    position[1] = y;
    g_hash_table_replace(folder_view->positions, g_strdup(key), position);
}

/* Icons where they were put; the rest, in order, in the free cells
 * (which is how they're arranged when none were put anywhere yet). They
 * keep those places, so a narrower window doesn't move them. */
static void layout_placed(XdFolderView *folder_view) {
    int count = (int)folder_view->shown->len;
    if (folder_view->positions_mode != folder_view->mode) {
        /* Put in another view's cells: arranged afresh for this one. */
        g_hash_table_remove_all(folder_view->positions);
        folder_view->positions_mode = folder_view->mode;
    }
    g_array_set_size(folder_view->rects, (guint)count);
    GHashTable *taken = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (int i = 0; i < count; i++) {
        item *it = g_ptr_array_index(folder_view->shown, (guint)i);
        const int *position =
            it->key != nullptr ? g_hash_table_lookup(folder_view->positions, it->key) : nullptr;
        GdkRectangle area = {0, 0, 0, 0};
        if (position != nullptr) {
            area = (GdkRectangle){MAX(position[0], 0), MAX(position[1], 0), folder_view->cell_w,
                                  folder_view->cell_h};
            take_cells(folder_view, taken, area);
        }
        g_array_index(folder_view->rects, GdkRectangle, i) = area;
    }
    int cols = grid_cols(folder_view);
    int cell = 0;
    int right = 0, bottom = 0;
    for (int i = 0; i < count; i++) {
        GdkRectangle *area = &g_array_index(folder_view->rects, GdkRectangle, i);
        if (area->width == 0) {
            cell = free_cell(folder_view, taken, cell);
            *area = (GdkRectangle){PAD + (cell % cols) * folder_view->cell_w,
                                   PAD + (cell / cols) * folder_view->cell_h, folder_view->cell_w,
                                   folder_view->cell_h};
            take_cells(folder_view, taken, *area);
            item *it = g_ptr_array_index(folder_view->shown, (guint)i);
            if (it->key != nullptr)
                set_position(folder_view, it->key, area->x, area->y);
        }
        right = MAX(right, area->x + area->width);
        bottom = MAX(bottom, area->y + area->height);
    }
    g_hash_table_unref(taken);
    folder_view->content_w = MAX(widget_w(folder_view), right + PAD);
    folder_view->content_h = bottom + PAD;
}

/* Align to Grid: every icon onto the nearest free cell. */
static void snap_positions(XdFolderView *folder_view) {
    if (!folder_view->placed)
        return;
    GHashTable *taken = g_hash_table_new(g_direct_hash, g_direct_equal);
    int cols = grid_cols(folder_view);
    for (guint i = 0; i < folder_view->shown->len && i < folder_view->rects->len; i++) {
        item *it = g_ptr_array_index(folder_view->shown, i);
        GdkRectangle area = g_array_index(folder_view->rects, GdkRectangle, i);
        int col = CLAMP((int)lround((double)(area.x - PAD) / folder_view->cell_w), 0, cols - 1);
        int row = MAX((int)lround((double)(area.y - PAD) / folder_view->cell_h), 0);
        int cell = free_cell(folder_view, taken, row * cols + col);
        g_hash_table_add(taken, cell_key(cell / cols, cell % cols));
        if (it->key != nullptr)
            set_position(folder_view, it->key, PAD + (cell % cols) * folder_view->cell_w,
                         PAD + (cell / cols) * folder_view->cell_h);
    }
    g_hash_table_unref(taken);
}

static void layout_contents(XdFolderView *folder_view) {
    int width = widget_w(folder_view);
    int height = widget_h(folder_view);
    int count = (int)folder_view->shown->len;
    folder_view->placed =
        is_icon_mode(folder_view->mode) && !folder_view->grouped && !folder_view->auto_arrange &&
        (folder_view->place == XD_PLACE_FOLDER || folder_view->place == XD_PLACE_TRASH);
    switch (folder_view->mode) {
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES:
        folder_view->cell_w = folder_view->mode == XD_VIEW_THUMBNAILS ? folder_view->thumb + 24
                              : folder_view->mode == XD_VIEW_ICONS    ? ICONS_W
                                                                      : TILE_W;
        folder_view->cell_h = folder_view->mode == XD_VIEW_THUMBNAILS ? folder_view->thumb + 40
                              : folder_view->mode == XD_VIEW_ICONS    ? ICONS_H
                                                                      : TILE_H;
        folder_view->cols = MAX((width - PAD) / folder_view->cell_w, 1);
        folder_view->rows = (count + folder_view->cols - 1) / folder_view->cols;
        folder_view->content_w = width;
        folder_view->content_h = PAD * 2 + folder_view->rows * folder_view->cell_h;
        if (folder_view->grouped)
            layout_groups(folder_view);
        else if (folder_view->placed)
            layout_placed(folder_view);
        break;
    case XD_VIEW_LIST: {
        /* Windows' List view fills columns top to bottom and scrolls
         * sideways. Columns are as wide as the widest name (capped). */
        folder_view->rows = MAX((height - PAD * 2) / ROW_H, 1);
        folder_view->cols = (count + folder_view->rows - 1) / folder_view->rows;
        int widest = 60;
        for (int i = 0; i < count; i++) {
            item *it = g_ptr_array_index(folder_view->shown, (guint)i);
            if (it->name_w == 0)
                it->name_w = text_width(folder_view, item_label(folder_view, it));
            widest = MAX(widest, it->name_w);
        }
        folder_view->cell_w = MIN(widest + LIST_ICON + 16, LIST_COL_MAX);
        folder_view->cell_h = ROW_H;
        folder_view->content_w = PAD * 2 + folder_view->cols * folder_view->cell_w;
        folder_view->content_h = height;
        break;
    }
    case XD_VIEW_DETAILS: {
        /* The columns chosen that this place has (Control Panel: Name and
         * Comments), at their widths; Name takes the spare room until
         * it's given a width. */
        memset(folder_view->col_w, 0, sizeof(folder_view->col_w));
        int others = 0;
        for (int i = 0; i < folder_view->ncols; i++) {
            int column = folder_view->order[i];
            if (column == XD_SORT_NAME || !column_available(folder_view, column))
                continue;
            folder_view->col_w[column] = folder_view->width[column] > 0
                                             ? folder_view->width[column]
                                             : default_width(folder_view, column, width);
            others += folder_view->col_w[column];
        }
        folder_view->col_w[XD_SORT_NAME] = folder_view->width[XD_SORT_NAME] > 0
                                               ? folder_view->width[XD_SORT_NAME]
                                               : MAX(220, width - others);
        folder_view->cell_w = folder_view->col_w[XD_SORT_NAME] + others;
        folder_view->cell_h = ROW_H;
        folder_view->cols = 1;
        folder_view->rows = count;
        folder_view->content_w = folder_view->cell_w;
        folder_view->content_h = HEADER_H + count * ROW_H + PAD;
        if (folder_view->grouped)
            layout_detail_groups(folder_view);
        break;
    }
    case XD_VIEW_COUNT:
        break;
    }
    configure(folder_view->hadj, width, folder_view->content_w);
    configure(folder_view->vadj, height, folder_view->content_h);
}

/* Rectangle of item i in content coordinates. */
static GdkRectangle item_rect(const XdFolderView *folder_view, int i) {
    switch (folder_view->mode) {
    case XD_VIEW_LIST:
        return (GdkRectangle){PAD + (i / folder_view->rows) * folder_view->cell_w,
                              PAD + (i % folder_view->rows) * ROW_H, folder_view->cell_w - 4,
                              ROW_H};
    case XD_VIEW_DETAILS:
        if (!folder_view->grouped)
            return (GdkRectangle){0, HEADER_H + i * ROW_H, folder_view->cell_w, ROW_H};
        break; /* the rows' own places, below */
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES:
    case XD_VIEW_COUNT:
        break;
    }
    if (uses_rects(folder_view) && i >= 0 && (guint)i < folder_view->rects->len)
        return g_array_index(folder_view->rects, GdkRectangle, i);
    return (GdkRectangle){PAD + (i % folder_view->cols) * folder_view->cell_w,
                          PAD + (i / folder_view->cols) * folder_view->cell_h, folder_view->cell_w,
                          folder_view->cell_h};
}

static GdkRectangle band_rect(const XdFolderView *folder_view);

static double hval(const XdFolderView *folder_view) {
    return folder_view->hadj ? gtk_adjustment_get_value(folder_view->hadj) : 0;
}
static double vval(const XdFolderView *folder_view) {
    return folder_view->vadj ? gtk_adjustment_get_value(folder_view->vadj) : 0;
}

static int index_at(XdFolderView *folder_view, double wx, double wy) {
    double x = wx + hval(folder_view);
    double y = wy + vval(folder_view);
    int count = (int)folder_view->shown->len;
    int i = -1;
    switch (folder_view->mode) {
    case XD_VIEW_LIST: {
        int col = ifloor((x - PAD) / folder_view->cell_w);
        int row = ifloor((y - PAD) / ROW_H);
        if (col >= 0 && row >= 0 && row < folder_view->rows)
            i = col * folder_view->rows + row;
        break;
    }
    case XD_VIEW_DETAILS:
        if (wy < HEADER_H)
            break;
        if (!folder_view->grouped) {
            i = ifloor((y - HEADER_H) / ROW_H);
            break;
        }
        [[fallthrough]]; /* rows in groups: their own places */
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES: {
        if (uses_rects(folder_view)) {
            for (guint k = 0; k < folder_view->rects->len; k++) {
                GdkRectangle area = g_array_index(folder_view->rects, GdkRectangle, k);
                if (x >= area.x && x < area.x + area.width && y >= area.y &&
                    y < area.y + area.height)
                    i = (int)k;
            }
            break;
        }
        int col = ifloor((x - PAD) / folder_view->cell_w);
        int row = ifloor((y - PAD) / folder_view->cell_h);
        if (col >= 0 && col < folder_view->cols && row >= 0)
            i = row * folder_view->cols + col;
        break;
    }
    case XD_VIEW_COUNT:
        break;
    }
    return i >= 0 && i < count ? i : -1;
}

static void ensure_visible(XdFolderView *folder_view, int i) {
    if (i < 0)
        return;
    GdkRectangle area = item_rect(folder_view, i);
    if (folder_view->placed && folder_view->hadj != nullptr) {
        /* Icons put anywhere may be off to the side too. */
        double val = hval(folder_view);
        double page = widget_w(folder_view);
        if (area.x < val)
            gtk_adjustment_set_value(folder_view->hadj, area.x - PAD);
        else if (area.x + area.width > val + page)
            gtk_adjustment_set_value(folder_view->hadj, area.x + area.width - page + PAD);
    }
    if (folder_view->mode == XD_VIEW_LIST && folder_view->hadj != nullptr) {
        double val = hval(folder_view);
        double page = widget_w(folder_view);
        if (area.x < val)
            gtk_adjustment_set_value(folder_view->hadj, area.x - PAD);
        else if (area.x + area.width > val + page)
            gtk_adjustment_set_value(folder_view->hadj, area.x + area.width - page + PAD);
    } else if (folder_view->vadj != nullptr) {
        double top = folder_view->mode == XD_VIEW_DETAILS ? HEADER_H : 0;
        double val = vval(folder_view);
        double page = widget_h(folder_view);
        if (area.y - top < val)
            gtk_adjustment_set_value(folder_view->vadj, area.y - top);
        else if (area.y + area.height > val + page)
            gtk_adjustment_set_value(folder_view->vadj, area.y + area.height - page + PAD);
    }
}

/* ---- drawing ------------------------------------------------------------------------ */

typedef struct colors {
    GdkRGBA base, text, dim, sel_bg, sel_fg, header_bg, line;
} colors;

static GdkRGBA lookup(GtkStyleContext *sc, const char *name, const char *fallback) {
    GdkRGBA color;
    if (!gtk_style_context_lookup_color(sc, name, &color))
        gdk_rgba_parse(&color, fallback);
    return color;
}

static colors theme_colors(XdFolderView *folder_view) {
    GtkStyleContext *sc = gtk_widget_get_style_context(GTK_WIDGET(folder_view));
    colors view_colors = {
        .base = lookup(sc, "theme_base_color", "#ffffff"),
        .text = lookup(sc, "theme_text_color", "#000000"),
        .sel_bg = lookup(sc, "theme_selected_bg_color", "#316ac5"),
        .sel_fg = lookup(sc, "theme_selected_fg_color", "#ffffff"),
        .header_bg = lookup(sc, "theme_bg_color", "#ece9d8"),
        .line = lookup(sc, "borders", "#aca899"),
    };
    view_colors.dim = view_colors.text;
    view_colors.dim.alpha = 0.6;
    return view_colors;
}

static void set_color(cairo_t *cairo, GdkRGBA color) { gdk_cairo_set_source_rgba(cairo, &color); }

/* File names wrap without inserted hyphens, as in Windows. */
static void no_hyphens(PangoLayout *layout) {
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
}

static void draw_text(XdFolderView *folder_view, cairo_t *cairo, const char *text, int x, int y,
                      int width, int lines, PangoAlignment align, GdkRGBA color) {
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(folder_view), text);
    pango_layout_set_width(layout, MAX(width, 1) * PANGO_SCALE);
    pango_layout_set_alignment(layout, align);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    no_hyphens(layout);
    if (lines > 1) {
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(layout, -lines);
    }
    set_color(cairo, color);
    cairo_move_to(cairo, x, y);
    pango_cairo_show_layout(cairo, layout);
    g_object_unref(layout);
}

/* Draws a label with its selection highlight; returns nothing. */
static void draw_label_box(XdFolderView *folder_view, cairo_t *cairo, const char *text, int x,
                           int y, int width, int lines, PangoAlignment align, gboolean selected,
                           const colors *view_colors, gboolean focused) {
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(folder_view), text);
    pango_layout_set_width(layout, MAX(width, 1) * PANGO_SCALE);
    pango_layout_set_alignment(layout, align);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    no_hyphens(layout);
    if (folder_view->underline_label) {
        PangoAttrList *attrs = pango_layout_get_attributes(layout);
        attrs = attrs != nullptr ? pango_attr_list_copy(attrs) : pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
    }
    if (lines > 1) {
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(layout, -lines);
    }
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    int bx = x + logical.x - 2;
    int bw = logical.width + 4;
    if (selected) {
        set_color(cairo, view_colors->sel_bg);
        cairo_rectangle(cairo, bx, y, bw, logical.height);
        cairo_fill(cairo);
    }
    if (focused) {
        static const double dash[] = {1, 1};
        set_color(cairo, selected ? view_colors->sel_fg : view_colors->text);
        cairo_set_line_width(cairo, 1);
        cairo_set_dash(cairo, dash, 2, 0);
        cairo_rectangle(cairo, bx + 0.5, y + 0.5, bw - 1, logical.height - 1);
        cairo_stroke(cairo);
        cairo_set_dash(cairo, nullptr, 0, 0);
    }
    set_color(cairo, selected ? view_colors->sel_fg : view_colors->text);
    cairo_move_to(cairo, x, y);
    pango_cairo_show_layout(cairo, layout);
    g_object_unref(layout);
}

static void paint_pixbuf(cairo_t *cairo, GdkPixbuf *pb, double x, double y, gboolean selected,
                         gboolean faded, const colors *view_colors) {
    if (pb == nullptr)
        return;
    gdk_cairo_set_source_pixbuf(cairo, pb, x, y);
    cairo_paint_with_alpha(cairo, faded ? 0.5 : 1.0);
    if (selected) {
        /* Windows tints selected icons with the highlight colour. */
        GdkRGBA tint = view_colors->sel_bg;
        tint.alpha = 0.4;
        cairo_save(cairo);
        cairo_rectangle(cairo, x, y, gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
        cairo_clip(cairo);
        set_color(cairo, tint);
        gdk_cairo_set_source_pixbuf(cairo, pb, x, y);
        cairo_pattern_t *mask = cairo_get_source(cairo);
        cairo_pattern_reference(mask);
        set_color(cairo, tint);
        cairo_mask(cairo, mask);
        cairo_pattern_destroy(mask);
        cairo_restore(cairo);
    }
}

/* XP's sharing overlay: an open hand, palm up, in a blue cuff, holding
 * the bottom of an icon of `px` whose top left is (x, y). Drawn on a
 * 32-unit grid. */
static void draw_share_hand(cairo_t *cairo, double x, double y, double px) {
    double unit = px / 32.0;
    cairo_save(cairo);
    cairo_translate(cairo, round(x), round(y));
    cairo_scale(cairo, unit, unit);
    cairo_set_line_width(cairo, MAX(1.0, 1.0 / unit));
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    /* The palm and fingers, reaching right under the icon. */
    cairo_move_to(cairo, 6, 25);
    cairo_line_to(cairo, 13, 24);
    cairo_curve_to(cairo, 15, 21, 18, 20, 19.5, 21.5); /* the thumb */
    cairo_line_to(cairo, 17, 24.5);
    cairo_line_to(cairo, 25, 24.5);
    cairo_curve_to(cairo, 27.5, 24.5, 27.5, 27, 25, 27);
    cairo_line_to(cairo, 23, 27);
    cairo_curve_to(cairo, 25, 27.5, 24.5, 30, 22, 30);
    cairo_line_to(cairo, 6, 30.5);
    cairo_close_path(cairo);
    cairo_set_source_rgb(cairo, 0.98, 0.82, 0.62);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0.45, 0.25, 0.10);
    cairo_stroke(cairo);
    /* The cuff. */
    cairo_rectangle(cairo, 0.5, 23.5, 6, 8);
    cairo_set_source_rgb(cairo, 0.16, 0.36, 0.80);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0.05, 0.15, 0.45);
    cairo_stroke(cairo);
    cairo_restore(cairo);
}

static void draw_item(XdFolderView *folder_view, cairo_t *cairo, int i, const colors *view_colors,
                      gboolean has_focus) {
    item *it = g_ptr_array_index(folder_view->shown, (guint)i);
    GdkRectangle area = item_rect(folder_view, i);
    /* The folder a drag would drop into shows as selected. */
    gboolean sel = it->selected || i == folder_view->drop_index;
    gboolean faded = FALSE;
    if (folder_view->cut != nullptr) {
        char *uri = g_file_get_uri(it->file);
        faded = g_hash_table_contains(folder_view->cut, uri);
        g_free(uri);
    }
    gboolean foc = has_focus && i == folder_view->focus;
    folder_view->underline_label = folder_view->single_click && i == folder_view->hover;
    switch (folder_view->mode) {
    case XD_VIEW_THUMBNAILS: {
        int box = folder_view->thumb;
        int bx = area.x + (area.width - box) / 2;
        int by = area.y + 4;
        /* XP's thumbnail frame: a white square with a thin border. */
        set_color(cairo, view_colors->base);
        cairo_rectangle(cairo, bx, by, box, box);
        cairo_fill(cairo);
        set_color(cairo, sel ? view_colors->sel_bg : view_colors->line);
        cairo_set_line_width(cairo, sel ? 2 : 1);
        cairo_rectangle(cairo, bx + 0.5, by + 0.5, box - 1, box - 1);
        cairo_stroke(cairo);
        GdkPixbuf *pb = item_thumb(folder_view, it, box - 8);
        if (pb == nullptr)
            pb = item_icon(folder_view, it, MIN(box / 2, 64));
        if (pb != nullptr)
            paint_pixbuf(cairo, pb, bx + (box - gdk_pixbuf_get_width(pb)) / 2.0,
                         by + (box - gdk_pixbuf_get_height(pb)) / 2.0, FALSE, faded, view_colors);
        draw_label_box(folder_view, cairo, item_label(folder_view, it), area.x + 2, by + box + 4,
                       area.width - 4, 1, PANGO_ALIGN_CENTER, sel, view_colors, foc);
        break;
    }
    case XD_VIEW_ICONS: {
        int px = 32;
        GdkPixbuf *pb = item_icon(folder_view, it, px);
        paint_pixbuf(cairo, pb, area.x + (area.width - px) / 2.0, area.y + 4, sel, faded,
                     view_colors);
        if (it->shared)
            draw_share_hand(cairo, area.x + (area.width - px) / 2.0, area.y + 4, px);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cairo, area.x + (area.width - px) / 2.0, area.y + 4, px);
        draw_label_box(folder_view, cairo, item_label(folder_view, it), area.x + 2, area.y + px + 8,
                       area.width - 4, 2, PANGO_ALIGN_CENTER, sel, view_colors, foc);
        break;
    }
    case XD_VIEW_TILES: {
        int px = 48;
        GdkPixbuf *pb = item_icon(folder_view, it, px);
        paint_pixbuf(cairo, pb, area.x + 6, area.y + (area.height - px) / 2.0, sel, faded,
                     view_colors);
        if (it->shared)
            draw_share_hand(cairo, area.x + 6, area.y + (area.height - px) / 2.0, px);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cairo, area.x + 6, area.y + (area.height - px) / 2.0, px);
        int tx = area.x + px + 14;
        int tw = area.width - px - 22;
        draw_label_box(folder_view, cairo, item_label(folder_view, it), tx, area.y + 8, tw, 1,
                       PANGO_ALIGN_LEFT, sel, view_colors, foc);
        draw_text(folder_view, cairo, it->type_desc, tx, area.y + 26, tw, 1, PANGO_ALIGN_LEFT,
                  view_colors->dim);
        if (it->extra != nullptr)
            draw_text(folder_view, cairo, it->extra, tx, area.y + 42, tw, 1, PANGO_ALIGN_LEFT,
                      view_colors->dim);
        else if (!it->is_dir)
            draw_text(folder_view, cairo, it->size_text, tx, area.y + 42, tw, 1, PANGO_ALIGN_LEFT,
                      view_colors->dim);
        break;
    }
    case XD_VIEW_LIST: {
        GdkPixbuf *pb = item_icon(folder_view, it, LIST_ICON);
        paint_pixbuf(cairo, pb, area.x + 1, area.y + 1, sel, faded, view_colors);
        if (it->shared)
            draw_share_hand(cairo, area.x + 1, area.y + 1, LIST_ICON);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cairo, area.x + 1, area.y + 1, LIST_ICON);
        draw_label_box(folder_view, cairo, item_label(folder_view, it), area.x + LIST_ICON + 5,
                       area.y + 1, area.width - LIST_ICON - 6, 1, PANGO_ALIGN_LEFT, sel,
                       view_colors, foc);
        break;
    }
    case XD_VIEW_DETAILS: {
        GdkPixbuf *pb = item_icon(folder_view, it, LIST_ICON);
        int x = area.x;
        for (int k = 0; k < folder_view->ncols; k++) {
            int col = folder_view->order[k];
            int cw = folder_view->col_w[col];
            if (cw == 0)
                continue; /* not here */
            if (col == XD_SORT_NAME) {
                cairo_save(cairo);
                cairo_rectangle(cairo, x, area.y, cw, area.height);
                cairo_clip(cairo);
                paint_pixbuf(cairo, pb, x + 4, area.y + 1, sel, faded, view_colors);
                if (it->shared)
                    draw_share_hand(cairo, x + 4, area.y + 1, LIST_ICON);
                if (it->shortcut)
                    xd_draw_shortcut_arrow(cairo, x + 4, area.y + 1, LIST_ICON);
                draw_label_box(folder_view, cairo, item_label(folder_view, it), x + LIST_ICON + 8,
                               area.y + 1, cw - LIST_ICON - 12, 1, PANGO_ALIGN_LEFT, sel,
                               view_colors, foc);
                cairo_restore(cairo);
            } else {
                draw_text(folder_view, cairo, cell_text(it, col), x + 6, area.y + 1, cw - 10, 1,
                          col == XD_SORT_SIZE ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT,
                          view_colors->text);
            }
            x += cw;
        }
        break;
    }
    case XD_VIEW_COUNT:
        break;
    }
}

/* XP's group heading: bold, in the selection colour, over a line that
 * fades out to the right. */
static void draw_heading(XdFolderView *folder_view, cairo_t *cairo, const heading *hd, int width,
                         const colors *view_colors) {
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(folder_view), hd->text);
    PangoFontDescription *fd = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(layout)));
    pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    set_color(cairo, view_colors->sel_bg);
    cairo_move_to(cairo, PAD + 4, hd->y + GROUP_H - 6 - th);
    pango_cairo_show_layout(cairo, layout);
    g_object_unref(layout);
    double ly = hd->y + GROUP_H - 3.5;
    cairo_pattern_t *pattern =
        cairo_pattern_create_linear(PAD, 0, PAD + MIN(width - 2 * PAD, 500), 0);
    cairo_pattern_add_color_stop_rgba(pattern, 0, view_colors->sel_bg.red,
                                      view_colors->sel_bg.green, view_colors->sel_bg.blue, 1);
    cairo_pattern_add_color_stop_rgba(pattern, 1, view_colors->sel_bg.red,
                                      view_colors->sel_bg.green, view_colors->sel_bg.blue, 0);
    cairo_set_source(cairo, pattern);
    cairo_set_line_width(cairo, 1);
    cairo_move_to(cairo, PAD, ly);
    cairo_line_to(cairo, PAD + MIN(width - 2 * PAD, 500), ly);
    cairo_stroke(cairo);
    cairo_pattern_destroy(pattern);
}

/* One column's heading at x (widget coordinates). */
static void draw_header_cell(XdFolderView *folder_view, cairo_t *cairo, int col, double x,
                             const colors *view_colors) {
    int cw = folder_view->col_w[col];
    cairo_save(cairo);
    cairo_rectangle(cairo, x, 0, cw, HEADER_H);
    cairo_clip(cairo);
    gboolean sorted = (int)folder_view->sort == col;
    draw_text(folder_view, cairo, column_title(folder_view, col), (int)x + 6, 3,
              cw - (sorted ? 24 : 12), 1,
              col == XD_SORT_SIZE ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT, view_colors->text);
    set_color(cairo, view_colors->line);
    cairo_rectangle(cairo, x + cw - 1, 3, 1, HEADER_H - 6);
    cairo_fill(cairo);
    if (sorted && cw > 20) {
        /* Sort arrow: up for ascending, down for descending. */
        double ax = x + cw - 14;
        double ay = HEADER_H / 2.0;
        cairo_move_to(cairo, ax, folder_view->desc ? ay - 2 : ay + 2);
        cairo_line_to(cairo, ax + 8, folder_view->desc ? ay - 2 : ay + 2);
        cairo_line_to(cairo, ax + 4, folder_view->desc ? ay + 2 : ay - 2);
        cairo_close_path(cairo);
        set_color(cairo, view_colors->dim);
        cairo_fill(cairo);
    }
    cairo_restore(cairo);
}

static int header_drop_slot(XdFolderView *folder_view);

static void draw_header(XdFolderView *folder_view, cairo_t *cairo, const colors *view_colors) {
    int width = widget_w(folder_view);
    set_color(cairo, view_colors->header_bg);
    cairo_rectangle(cairo, 0, 0, width, HEADER_H);
    cairo_fill(cairo);
    set_color(cairo, view_colors->line);
    cairo_rectangle(cairo, 0, HEADER_H - 1, width, 1);
    cairo_fill(cairo);
    double x = -hval(folder_view);
    for (int k = 0; k < folder_view->ncols; k++) {
        int col = folder_view->order[k];
        if (folder_view->col_w[col] == 0)
            continue; /* not shown here */
        draw_header_cell(folder_view, cairo, col, x, view_colors);
        x += folder_view->col_w[col];
    }
    if (folder_view->hdr_dragging && folder_view->hdr_press >= 0) {
        /* The heading being moved follows the pointer, see-through, and
         * a line marks where it would go, as in XP. */
        double cx = column_x(folder_view, folder_view->hdr_press) - hval(folder_view) +
                    (folder_view->hdr_x - folder_view->hdr_press_x);
        cairo_push_group(cairo);
        set_color(cairo, view_colors->header_bg);
        cairo_rectangle(cairo, cx, 0, folder_view->col_w[folder_view->hdr_press], HEADER_H);
        cairo_fill(cairo);
        draw_header_cell(folder_view, cairo, folder_view->hdr_press, cx, view_colors);
        cairo_pop_group_to_source(cairo);
        cairo_paint_with_alpha(cairo, 0.6);
        int slot = header_drop_slot(folder_view);
        double mx = -hval(folder_view);
        for (int k = 0; k < slot && k < folder_view->ncols; k++)
            mx += folder_view->col_w[folder_view->order[k]];
        set_color(cairo, view_colors->sel_bg);
        cairo_rectangle(cairo, mx - 1, 0, 2, HEADER_H);
        cairo_fill(cairo);
    }
}

static gboolean folder_view_draw(GtkWidget *widget, cairo_t *cairo) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    colors view_colors = theme_colors(folder_view);
    int width = widget_w(folder_view);
    int height = widget_h(folder_view);
    set_color(cairo, view_colors.base);
    cairo_paint(cairo);

    gboolean has_focus = gtk_widget_has_focus(widget);
    double ox = hval(folder_view);
    double oy = vval(folder_view);
    int count = (int)folder_view->shown->len;

    /* Visible index range. */
    int first = 0;
    int last = count - 1;
    switch (folder_view->mode) {
    case XD_VIEW_LIST:
        first = MAX(0, ifloor((ox - PAD) / folder_view->cell_w)) * folder_view->rows;
        last = MIN(count - 1, (ifloor((ox + width) / folder_view->cell_w) + 1) * folder_view->rows);
        break;
    case XD_VIEW_DETAILS:
        if (folder_view->grouped)
            break; /* all, culled below */
        first = MAX(0, ifloor(oy / ROW_H));
        last = MIN(count - 1, ifloor((oy + height) / ROW_H) + 1);
        break;
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES:
        if (uses_rects(folder_view))
            break; /* all, culled below */
        first = MAX(0, ifloor((oy - PAD) / folder_view->cell_h)) * folder_view->cols;
        last =
            MIN(count - 1, (ifloor((oy + height) / folder_view->cell_h) + 1) * folder_view->cols +
                               folder_view->cols - 1);
        break;
    case XD_VIEW_COUNT:
        break;
    }

    cairo_save(cairo);
    if (folder_view->mode == XD_VIEW_DETAILS) {
        cairo_rectangle(cairo, 0, HEADER_H, width, height - HEADER_H);
        cairo_clip(cairo);
    }
    cairo_translate(cairo, -ox, -oy);
    if (folder_view->grouped) {
        for (guint k = 0; k < folder_view->headings->len; k++) {
            const heading *hd = &g_array_index(folder_view->headings, heading, k);
            if (hd->y + GROUP_H >= oy && hd->y <= oy + height)
                draw_heading(folder_view, cairo, hd, width, &view_colors);
        }
    }
    for (int i = first; i <= last; i++) {
        if (uses_rects(folder_view)) {
            GdkRectangle item_area = item_rect(folder_view, i);
            if (item_area.y + item_area.height < oy || item_area.y > oy + height ||
                item_area.x + item_area.width < ox || item_area.x > ox + width)
                continue;
        }
        draw_item(folder_view, cairo, i, &view_colors, has_focus);
    }
    cairo_restore(cairo);

    if (folder_view->banding) {
        /* XP's translucent selection rectangle. */
        GdkRectangle band = band_rect(folder_view);
        GdkRGBA fill = view_colors.sel_bg;
        fill.alpha = 0.25;
        gdk_cairo_set_source_rgba(cairo, &fill);
        cairo_rectangle(cairo, band.x - ox, band.y - oy, band.width, band.height);
        cairo_fill(cairo);
        set_color(cairo, view_colors.sel_bg);
        cairo_set_line_width(cairo, 1);
        cairo_rectangle(cairo, band.x - ox + 0.5, band.y - oy + 0.5, MAX(band.width - 1, 0),
                        MAX(band.height - 1, 0));
        cairo_stroke(cairo);
    }
    if (folder_view->mode == XD_VIEW_DETAILS)
        draw_header(folder_view, cairo, &view_colors);
    if (count == 0 && folder_view->dir != nullptr && folder_view->cancel == nullptr)
        draw_text(folder_view, cairo, "This folder is empty.", 0,
                  (folder_view->mode == XD_VIEW_DETAILS ? HEADER_H : 0) + 12, width, 1,
                  PANGO_ALIGN_CENTER, view_colors.dim);
    return FALSE;
}

/* ---- loading ------------------------------------------------------------------------ */

static item *item_at(XdFolderView *folder_view, int i);
static void set_hover(XdFolderView *folder_view, int i);
static gboolean reload_timeout(gpointer data);

/* Selects the kept files that are here; returns how many. */
static guint apply_keep(XdFolderView *folder_view) {
    if (folder_view->keep == nullptr)
        return 0;
    guint found = 0;
    for (guint k = 0; k < folder_view->shown->len; k++) {
        item *it = g_ptr_array_index(folder_view->shown, k);
        char *uri = g_file_get_uri(it->file);
        it->selected = g_hash_table_contains(folder_view->keep, uri);
        if (it->selected) {
            found++;
            if (folder_view->keep_focus == nullptr || strcmp(uri, folder_view->keep_focus) == 0 ||
                found == 1) {
                folder_view->focus = (int)k;
                folder_view->anchor = (int)k;
            }
        }
        g_free(uri);
    }
    return found;
}

static void forget_keep(XdFolderView *folder_view) {
    g_clear_pointer(&folder_view->keep, g_hash_table_unref);
    g_clear_pointer(&folder_view->keep_focus, g_free);
    folder_view->keep_sticky = FALSE;
}

/* XP's hand under shared folders: Samba's user shares, listed in the
 * background (the hands appear once they're known). */
static void mark_shared(XdFolderView *folder_view) {
    GHashTable *paths = folder_view->place == XD_PLACE_FOLDER && folder_view->dir != nullptr &&
                                g_file_is_native(folder_view->dir)
                            ? xd_usershare_shared_paths()
                            : nullptr;
    gboolean any = paths != nullptr && g_hash_table_size(paths) > 0;
    for (guint i = 0; i < folder_view->all->len; i++) {
        item *it = g_ptr_array_index(folder_view->all, i);
        it->shared = FALSE;
        if (any && it->is_dir) {
            char *path = g_file_get_path(it->file);
            it->shared = path != nullptr && g_hash_table_contains(paths, path);
            g_free(path);
        }
    }
}

static void shares_changed(gpointer data) {
    XdFolderView *folder_view = data;
    mark_shared(folder_view);
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}

static void loading_done(XdFolderView *folder_view) {
    g_clear_object(&folder_view->cancel);
    /* The selection survives a refresh, as it is now (unless files are
     * already waiting to be selected). */
    if (folder_view->keep == nullptr && folder_view->shown->len > 0) {
        folder_view->keep = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
        for (guint k = 0; k < folder_view->shown->len; k++) {
            item *it = g_ptr_array_index(folder_view->shown, k);
            if (it->selected)
                g_hash_table_add(folder_view->keep, g_file_get_uri(it->file));
        }
        item *focused = item_at(folder_view, folder_view->focus);
        folder_view->keep_focus = focused != nullptr ? g_file_get_uri(focused->file) : nullptr;
    }
    /* The new items replace the old: indices into `shown` start over
     * (the selection and focus come back from `keep`). */
    g_ptr_array_set_size(folder_view->shown, 0);
    g_ptr_array_unref(folder_view->all);
    folder_view->all = folder_view->loading;
    folder_view->loading = g_ptr_array_new_with_free_func(item_free);
    mark_shared(folder_view);
    folder_view->focus = -1;
    folder_view->anchor = -1;
    folder_view->pressed = -1;
    folder_view->drop_index = -1;
    folder_view->drag_pending = FALSE;
    set_hover(folder_view, -1);
    refilter(folder_view);
    if (folder_view->keep != nullptr) {
        guint found = apply_keep(folder_view);
        if (!folder_view->keep_sticky || found >= g_hash_table_size(folder_view->keep))
            forget_keep(folder_view);
        if (found > 0)
            ensure_visible(folder_view, folder_view->focus);
    }
    g_signal_emit(folder_view, signals[SIG_CONTENTS], 0);
    g_signal_emit(folder_view, signals[SIG_SELECTION], 0);
    if (folder_view->reload_again && folder_view->reload_source == 0) {
        folder_view->reload_again = FALSE;
        folder_view->reload_source = g_timeout_add(300, reload_timeout, folder_view);
    }
}

/* A Recycle Bin item: its original name, the folder it came from (in the
 * Type column) and when it was deleted (in the date column). */
static item *trash_item(GFile *dir, GFileInfo *info) {
    item *it = item_new(dir, info);
    char *orig = nullptr;
    GDateTime *deleted = nullptr;
    if (xd_trash_item_info(it->file, &orig, &deleted)) {
        char *base = g_path_get_basename(orig);
        char *folder = g_path_get_dirname(orig);
        g_free(it->name);
        it->name = g_filename_display_name(base);
        g_free(it->collate_key);
        it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
        g_free(it->type_desc);
        it->type_desc = g_filename_display_name(folder);
        g_free(base);
        g_free(folder);
    }
    if (deleted != nullptr) {
        it->mtime = g_date_time_to_unix(deleted);
        g_free(it->date_text);
        it->date_text = g_date_time_format(deleted, "%x %H:%M");
        g_date_time_unref(deleted);
    }
    g_free(orig);
    it->hidden = FALSE; /* everything in the bin is shown */
    return it;
}

/* My Computer: the drives, with their sizes in the size and date columns. */
static void load_drives(XdFolderView *folder_view) {
    GPtrArray *drives = xd_computer_drives();
    for (guint i = 0; i < drives->len; i++) {
        XdDrive *drive = g_ptr_array_index(drives, i);
        item *it = XD_NEW(item);
        it->file = g_object_ref(drive->root);
        it->name = g_strdup(drive->name);
        it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
        it->type_desc = g_strdup(drive->kind);
        it->is_dir = TRUE;
        it->icon = drive->icon != nullptr ? g_object_ref(drive->icon) : nullptr;
        it->order = (int)i;
        it->size = drive->total;
        it->size_text = drive->total > 0 ? xd_format_drive_size(drive->total) : g_strdup("");
        it->date_text = drive->total > 0 ? xd_format_drive_size(drive->free) : g_strdup("");
        it->mtime = (gint64)drive->free;
        if (drive->total > 0) {
            char *free_text = xd_format_drive_size(drive->free);
            char *total_text = xd_format_drive_size(drive->total);
            it->extra = g_strdup_printf("%s free of %s", free_text, total_text);
            g_free(free_text);
            g_free(total_text);
        }
        g_ptr_array_add(folder_view->loading, it);
    }
    g_ptr_array_unref(drives);
}

/* Control Panel: categories (folders that open their page) or applets,
 * with their comments in the Type column. */
static void load_control(XdFolderView *folder_view) {
    GPtrArray *items = xd_control_items(folder_view->dir);
    for (guint i = 0; i < items->len; i++) {
        XdControlItem *control_item = g_ptr_array_index(items, i);
        item *it = XD_NEW(item);
        it->file = g_object_ref(control_item->file);
        it->name = g_strdup(control_item->name);
        it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
        it->type_desc = g_strdup(control_item->comment);
        it->size_text = g_strdup("");
        it->is_dir = control_item->category;
        it->icon = g_object_ref(control_item->icon);
        it->order = (int)i;
        g_ptr_array_add(folder_view->loading, it);
    }
    g_ptr_array_unref(items);
}

/* My Network Places: its items, with their network location in the date
 * column (and Tiles' third line). */
static void load_network(XdFolderView *folder_view) {
    GPtrArray *items = xd_network_items(folder_view->dir);
    for (guint i = 0; i < items->len; i++) {
        XdNetworkItem *network_item = g_ptr_array_index(items, i);
        item *it = XD_NEW(item);
        it->file = g_object_ref(network_item->file);
        it->name = g_strdup(network_item->name);
        it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
        it->type_desc = g_strdup(network_item->comment);
        it->size_text = g_strdup("");
        it->date_text = g_strdup(network_item->location);
        if (network_item->location[0] != '\0')
            it->extra = g_strdup(network_item->location);
        it->is_dir = network_item->folder;
        it->icon = g_object_ref(network_item->icon);
        it->order = (int)i;
        g_ptr_array_add(folder_view->loading, it);
    }
    g_ptr_array_unref(items);
}

static void next_files_ready(GObject *src, GAsyncResult *res, gpointer data);
static void enumerate_ready(GObject *src, GAsyncResult *res, gpointer data);

/* The Recycle Bin lists each trash in turn: true if another one started. */
static gboolean list_next_trash(XdFolderView *folder_view) {
    if (folder_view->more_listed == nullptr || folder_view->more_listed->len == 0)
        return FALSE;
    GFile *dir = g_object_ref(g_ptr_array_index(folder_view->more_listed, 0));
    g_ptr_array_remove_index(folder_view->more_listed, 0);
    g_file_enumerate_children_async(dir, ATTRIBUTES, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                    folder_view->cancel, enumerate_ready, folder_view);
    g_object_unref(dir);
    return TRUE;
}

static void request_more(GFileEnumerator *file_enumerator, XdFolderView *folder_view) {
    g_file_enumerator_next_files_async(file_enumerator, BATCH, G_PRIORITY_DEFAULT,
                                       folder_view->cancel, next_files_ready, folder_view);
}

static void next_files_ready(GObject *src, GAsyncResult *res, gpointer data) {
    GFileEnumerator *file_enumerator = G_FILE_ENUMERATOR(src);
    GError *err = nullptr;
    GList *infos = g_file_enumerator_next_files_finish(file_enumerator, res, &err);
    if (err != nullptr) {
        /* Cancelled means the view moved on or is gone: don't touch it. */
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            XdFolderView *folder_view = data;
            g_signal_emit(folder_view, signals[SIG_ERROR], 0, err->message);
            loading_done(folder_view);
        }
        g_error_free(err);
        g_object_unref(file_enumerator);
        return;
    }
    XdFolderView *folder_view = data;
    if (infos == nullptr) {
        g_object_unref(file_enumerator);
        if (!list_next_trash(folder_view))
            loading_done(folder_view);
        return;
    }
    GFile *dir = g_file_enumerator_get_container(file_enumerator);
    for (GList *list = infos; list != nullptr; list = list->next)
        g_ptr_array_add(folder_view->loading, folder_view->place == XD_PLACE_TRASH
                                                  ? trash_item(dir, list->data)
                                                  : item_new(dir, list->data));
    g_list_free_full(infos, g_object_unref);
    request_more(file_enumerator, folder_view);
}

static void enumerate_ready(GObject *src, GAsyncResult *res, gpointer data) {
    GError *err = nullptr;
    GFileEnumerator *file_enumerator = g_file_enumerate_children_finish(G_FILE(src), res, &err);
    if (file_enumerator == nullptr) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            XdFolderView *folder_view = data;
            /* Another drive's trash that went away: list the rest. */
            if (folder_view->place == XD_PLACE_TRASH &&
                !g_file_equal(G_FILE(src), folder_view->listed)) {
                if (!list_next_trash(folder_view))
                    loading_done(folder_view);
                g_error_free(err);
                return;
            }
            g_signal_emit(folder_view, signals[SIG_ERROR], 0, err->message);
            loading_done(folder_view);
        }
        g_error_free(err);
        return;
    }
    request_more(file_enumerator, data);
}

/* Lists the folder: a new one from empty (`fresh`), or the same one again,
 * still showing the old list until the new one is in. */
static void start_loading(XdFolderView *folder_view, gboolean fresh) {
    if (folder_view->cancel != nullptr)
        g_cancellable_cancel(folder_view->cancel);
    g_clear_object(&folder_view->cancel);
    g_ptr_array_set_size(folder_view->loading, 0);
    folder_view->reload_again = FALSE;
    if (fresh) {
        g_ptr_array_set_size(folder_view->shown, 0);
        g_ptr_array_set_size(folder_view->all, 0);
        folder_view->focus = -1;
        folder_view->anchor = -1;
        folder_view->pressed = -1;
        folder_view->drop_index = -1;
        folder_view->drag_pending = FALSE;
        set_hover(folder_view, -1);
    }
    folder_view->cancel = g_cancellable_new();
    if (folder_view->place == XD_PLACE_COMPUTER || folder_view->place == XD_PLACE_CONTROL ||
        folder_view->place == XD_PLACE_NETWORK) {
        if (folder_view->place == XD_PLACE_COMPUTER)
            load_drives(folder_view);
        else if (folder_view->place == XD_PLACE_NETWORK)
            load_network(folder_view);
        else
            load_control(folder_view);
        loading_done(folder_view);
        return;
    }
    g_clear_pointer(&folder_view->more_listed, g_ptr_array_unref);
    if (folder_view->place == XD_PLACE_TRASH) {
        folder_view->more_listed = xd_trash_dirs();
        g_ptr_array_remove_index(folder_view->more_listed, 0); /* the home one is `listed` */
    }
    g_file_enumerate_children_async(folder_view->listed, ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
                                    G_PRIORITY_DEFAULT, folder_view->cancel, enumerate_ready,
                                    folder_view);
    if (fresh) {
        layout_contents(folder_view);
        gtk_widget_queue_draw(GTK_WIDGET(folder_view));
    }
}

static gboolean reload_timeout(gpointer data) {
    XdFolderView *folder_view = data;
    folder_view->reload_source = 0;
    /* Still listing (a big folder that keeps changing): restarting would
     * never let it finish. List it again once this one is in. */
    if (folder_view->cancel != nullptr)
        folder_view->reload_again = TRUE;
    else
        xd_folder_view_reload(folder_view);
    return G_SOURCE_REMOVE;
}

static void drives_changed([[maybe_unused]] GVolumeMonitor *volume_monitor,
                           [[maybe_unused]] gpointer mount, gpointer data) {
    XdFolderView *folder_view = data;
    if ((folder_view->place == XD_PLACE_COMPUTER || folder_view->place == XD_PLACE_NETWORK) &&
        folder_view->reload_source == 0)
        folder_view->reload_source = g_timeout_add(300, reload_timeout, folder_view);
}

/* A drive renamed (its label), mounted or checked: UDisks2 says so
 * before GIO does, if GIO ever does. */
static void udisks_changed([[maybe_unused]] GDBusObjectManagerClient *object_manager,
                           [[maybe_unused]] GDBusObjectProxy *object,
                           [[maybe_unused]] GDBusProxy *iface, [[maybe_unused]] GVariant *changed,
                           [[maybe_unused]] const char *const *invalidated, gpointer data) {
    drives_changed(nullptr, nullptr, data);
}

/* A volume appearing or going: an encrypted drive's cleartext when it's
 * unlocked or locked. */
static void udisks_object_changed([[maybe_unused]] GDBusObjectManager *object_manager,
                                  [[maybe_unused]] GDBusObject *dbus_object, gpointer data) {
    drives_changed(nullptr, nullptr, data);
}

static void apps_changed([[maybe_unused]] GAppInfoMonitor *app_info_monitor, gpointer data) {
    XdFolderView *folder_view = data;
    if (folder_view->place == XD_PLACE_CONTROL && folder_view->reload_source == 0)
        folder_view->reload_source = g_timeout_add(300, reload_timeout, folder_view);
}

static void monitor_changed([[maybe_unused]] GFileMonitor *file_monitor,
                            [[maybe_unused]] GFile *file, [[maybe_unused]] GFile *other,
                            [[maybe_unused]] GFileMonitorEvent ev, gpointer data) {
    XdFolderView *folder_view = data;
    /* Coalesce bursts (a copy creates many events) into one reload. */
    if (folder_view->reload_source == 0)
        folder_view->reload_source = g_timeout_add(300, reload_timeout, folder_view);
}

/* ---- selection and input ------------------------------------------------------------ */

static item *item_at(XdFolderView *folder_view, int i) {
    return i >= 0 && i < (int)folder_view->shown->len
               ? g_ptr_array_index(folder_view->shown, (guint)i)
               : nullptr;
}

static void select_only(XdFolderView *folder_view, int i) {
    for (guint k = 0; k < folder_view->shown->len; k++)
        ((item *)g_ptr_array_index(folder_view->shown, k))->selected = (int)k == i;
}

static void select_range(XdFolderView *folder_view, int first, int last) {
    int lo = MIN(first, last);
    int hi = MAX(first, last);
    for (guint k = 0; k < folder_view->shown->len; k++)
        ((item *)g_ptr_array_index(folder_view->shown, k))->selected = (int)k >= lo && (int)k <= hi;
}

static void selection_changed(XdFolderView *folder_view) {
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
    g_signal_emit(folder_view, signals[SIG_SELECTION], 0);
}

static void activate(XdFolderView *folder_view, int i) {
    item *it = item_at(folder_view, i);
    if (it != nullptr)
        g_signal_emit(folder_view, signals[SIG_ACTIVATED], 0, it->file, it->is_dir);
}

/* ---- the Details header ------------------------------------------------------------ */

/* The column under widget x in the header (-1: none), and whether x is on
 * its right-hand divider, which resizes it. */
static int header_column_at(XdFolderView *folder_view, double wx, gboolean *divider) {
    double x = wx + hval(folder_view);
    *divider = FALSE;
    int left = 0;
    for (int k = 0; k < folder_view->ncols; k++) {
        int cw = folder_view->col_w[folder_view->order[k]];
        if (cw == 0)
            continue;
        if (fabs(x - (left + cw)) <= DIVIDER_HIT) {
            *divider = TRUE;
            return folder_view->order[k];
        }
        left += cw;
    }
    left = 0;
    for (int k = 0; k < folder_view->ncols; k++) {
        int cw = folder_view->col_w[folder_view->order[k]];
        if (x >= left && x < left + cw)
            return folder_view->order[k];
        left += cw;
    }
    return -1;
}

/* Where a heading being dragged would go: before the column whose middle
 * is past the pointer (an index into `order`). */
static int header_drop_slot(XdFolderView *folder_view) {
    double x = folder_view->hdr_x + hval(folder_view);
    int left = 0;
    for (int k = 0; k < folder_view->ncols; k++) {
        int cw = folder_view->col_w[folder_view->order[k]];
        if (cw > 0 && x < left + cw / 2.0)
            return k;
        left += cw;
    }
    return folder_view->ncols;
}

static void columns_changed(XdFolderView *folder_view) {
    layout_contents(folder_view);
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}

/* Name fills the window until a column is sized by hand; from then on it
 * keeps its width, as the others do. */
static void freeze_name_width(XdFolderView *folder_view) {
    if (folder_view->width[XD_SORT_NAME] == 0)
        folder_view->width[XD_SORT_NAME] = MAX(folder_view->col_w[XD_SORT_NAME], COL_MIN);
}

/* Double-clicking a divider fits the column to its widest entry. */
static void fit_column(XdFolderView *folder_view, int col) {
    int widest = 0;
    for (guint k = 0; k < folder_view->shown->len; k++) {
        item *it = g_ptr_array_index(folder_view->shown, k);
        if (col == XD_SORT_NAME) {
            if (it->name_w == 0)
                it->name_w = text_width(folder_view, item_label(folder_view, it));
            widest = MAX(widest, it->name_w + LIST_ICON + 20);
        } else {
            widest = MAX(widest, text_width(folder_view, cell_text(it, col)) + 16);
        }
    }
    if (widest == 0)
        widest = text_width(folder_view, column_title(folder_view, col)) +
                 24; /* nothing listed: the heading */
    freeze_name_width(folder_view);
    folder_view->width[col] = MAX(widest, COL_MIN);
    columns_changed(folder_view);
}

/* A heading dropped at `slot`: its column moves there. */
static void move_column(XdFolderView *folder_view, int col, int slot) {
    int from = -1;
    for (int k = 0; k < folder_view->ncols; k++)
        if (folder_view->order[k] == col)
            from = k;
    if (from < 0 || slot == from || slot == from + 1)
        return;
    memmove(&folder_view->order[from], &folder_view->order[from + 1],
            sizeof(int) * (size_t)(folder_view->ncols - from - 1));
    if (slot > from)
        slot--;
    memmove(&folder_view->order[slot + 1], &folder_view->order[slot],
            sizeof(int) * (size_t)(folder_view->ncols - 1 - slot));
    folder_view->order[slot] = col;
    columns_changed(folder_view);
}

static void on_column_toggled(GtkCheckMenuItem *mi, gpointer data) {
    XdFolderView *folder_view = data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "xd-column"));
    int at = -1;
    for (int k = 0; k < folder_view->ncols; k++)
        if (folder_view->order[k] == col)
            at = k;
    if (gtk_check_menu_item_get_active(mi) && at < 0) {
        folder_view->order[folder_view->ncols++] = col; /* a column added goes on the right */
    } else if (!gtk_check_menu_item_get_active(mi) && at >= 0) {
        memmove(&folder_view->order[at], &folder_view->order[at + 1],
                sizeof(int) * (size_t)(folder_view->ncols - at - 1));
        folder_view->ncols--;
    }
    columns_changed(folder_view);
}

/* GTK deactivates a menu before running the chosen item: destroy it once
 * that's done. */
static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu_later(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

/* Right-clicking the header: the columns this folder can show, ticked
 * when shown (Name always is). */
static void header_menu(XdFolderView *folder_view, GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();
    for (int col = 0; col < XD_SORT_COUNT; col++) {
        if (!column_available(folder_view, col))
            continue;
        GtkWidget *mi = gtk_check_menu_item_new_with_label(column_title(folder_view, col));
        gboolean shown = FALSE;
        for (int k = 0; k < folder_view->ncols; k++)
            shown = shown || folder_view->order[k] == col;
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), shown);
        gtk_widget_set_sensitive(mi, col != XD_SORT_NAME);
        g_object_set_data(G_OBJECT(mi), "xd-column", GINT_TO_POINTER(col));
        g_signal_connect(mi, "toggled", G_CALLBACK(on_column_toggled), folder_view);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(folder_view), nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* The resize pointer over a divider. */
static void set_header_cursor(XdFolderView *folder_view, gboolean resize) {
    if (resize == folder_view->hdr_cursor)
        return;
    folder_view->hdr_cursor = resize;
    GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(folder_view));
    if (win == nullptr)
        return;
    GdkCursor *cur =
        resize ? gdk_cursor_new_from_name(gdk_window_get_display(win), "col-resize") : nullptr;
    gdk_window_set_cursor(win, cur);
    g_clear_object(&cur);
}

/* A press on the header: a divider starts resizing (double-click fits),
 * a heading may be clicked (sorts on release) or dragged elsewhere, and
 * the right button chooses the columns. */
static gboolean header_press(XdFolderView *folder_view, GdkEventButton *ev) {
    gboolean divider = FALSE;
    int col = header_column_at(folder_view, ev->x, &divider);
    if (ev->button == GDK_BUTTON_SECONDARY) {
        if (ev->type == GDK_BUTTON_PRESS)
            header_menu(folder_view, ev);
        return TRUE;
    }
    if (ev->button != GDK_BUTTON_PRIMARY)
        return TRUE;
    if (ev->type == GDK_2BUTTON_PRESS) {
        folder_view->hdr_resize = -1;
        folder_view->hdr_press = -1;
        if (divider && col >= 0)
            fit_column(folder_view, col);
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS || col < 0)
        return TRUE;
    if (divider) {
        freeze_name_width(folder_view);
        folder_view->hdr_resize = col;
        folder_view->hdr_resize_w = folder_view->col_w[col];
    } else {
        folder_view->hdr_press = col;
        folder_view->hdr_dragging = FALSE;
    }
    folder_view->hdr_press_x = folder_view->hdr_x = ev->x;
    return TRUE;
}

static gboolean header_motion(XdFolderView *folder_view, GdkEventMotion *ev) {
    gboolean held = (ev->state & GDK_BUTTON1_MASK) != 0;
    if (folder_view->hdr_resize >= 0 && held) {
        folder_view->width[folder_view->hdr_resize] =
            MAX(COL_MIN, folder_view->hdr_resize_w + (int)(ev->x - folder_view->hdr_press_x));
        columns_changed(folder_view);
        return TRUE;
    }
    if (folder_view->hdr_press >= 0 && held) {
        folder_view->hdr_x = ev->x;
        if (!folder_view->hdr_dragging &&
            gtk_drag_check_threshold(GTK_WIDGET(folder_view), (int)folder_view->hdr_press_x, 0,
                                     (int)ev->x, 0))
            folder_view->hdr_dragging = TRUE;
        if (folder_view->hdr_dragging)
            gtk_widget_queue_draw(GTK_WIDGET(folder_view));
        return TRUE;
    }
    gboolean divider = FALSE;
    if (folder_view->mode == XD_VIEW_DETAILS && ev->y >= 0 && ev->y < HEADER_H && !held)
        header_column_at(folder_view, ev->x, &divider);
    set_header_cursor(folder_view, divider);
    return FALSE;
}

static gboolean header_release(XdFolderView *folder_view, GdkEventButton *ev) {
    if (folder_view->hdr_resize >= 0) {
        folder_view->hdr_resize = -1;
        return TRUE;
    }
    if (folder_view->hdr_press < 0)
        return FALSE;
    int col = folder_view->hdr_press;
    folder_view->hdr_press = -1;
    if (folder_view->hdr_dragging) {
        folder_view->hdr_dragging = FALSE;
        folder_view->hdr_x = ev->x;
        int slot = header_drop_slot(folder_view);
        move_column(folder_view, col, slot);
        gtk_widget_queue_draw(GTK_WIDGET(folder_view));
        return TRUE;
    }
    /* A click sorts; clicking the sorted column reverses it. */
    gboolean divider = FALSE;
    if (ev->y >= 0 && ev->y < HEADER_H && header_column_at(folder_view, ev->x, &divider) == col)
        xd_folder_view_set_sort(folder_view, (XdSortColumn)col,
                                (int)folder_view->sort == col ? !folder_view->desc : FALSE);
    return TRUE;
}

static gboolean folder_view_button_press(GtkWidget *widget, GdkEventButton *ev) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    gtk_widget_grab_focus(widget);
    forget_keep(folder_view);
    if (folder_view->mode == XD_VIEW_DETAILS && ev->y < HEADER_H)
        return header_press(folder_view, ev);
    int i = index_at(folder_view, ev->x, ev->y);
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
        /* With single-click, the first click's release opened it. */
        if (!folder_view->single_click)
            activate(folder_view, i);
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    /* The middle button opens an item in a new tab, as in Windows 11,
     * leaving the selection alone. */
    if (ev->button == GDK_BUTTON_MIDDLE) {
        item *mid = item_at(folder_view, i);
        if (mid != nullptr)
            g_signal_emit(folder_view, signals[SIG_MIDDLE], 0, mid->file, mid->is_dir);
        return TRUE;
    }
    gboolean ctrl = ev->state & GDK_CONTROL_MASK;
    gboolean shift = ev->state & GDK_SHIFT_MASK;
    folder_view->pressed = ev->button == GDK_BUTTON_PRIMARY && !ctrl && !shift ? i : -1;
    item *it = item_at(folder_view, i);
    if (it == nullptr && ev->button == GDK_BUTTON_PRIMARY) {
        /* A drag from empty space selects what it covers. */
        if (!ctrl)
            select_only(folder_view, -1);
        folder_view->banding = TRUE;
        folder_view->band_x0 = folder_view->band_x1 = ev->x + hval(folder_view);
        folder_view->band_y0 = folder_view->band_y1 = ev->y + vval(folder_view);
        folder_view->band_toggle = ctrl;
        if (folder_view->band_base == nullptr)
            folder_view->band_base = g_array_new(FALSE, FALSE, sizeof(gboolean));
        g_array_set_size(folder_view->band_base, folder_view->shown->len);
        for (guint k = 0; k < folder_view->shown->len; k++)
            g_array_index(folder_view->band_base, gboolean, k) =
                ((item *)g_ptr_array_index(folder_view->shown, k))->selected;
    } else if (it == nullptr) {
        if (!ctrl)
            select_only(folder_view, -1);
    } else if (ctrl && ev->button == GDK_BUTTON_PRIMARY) {
        it->selected = !it->selected;
        folder_view->anchor = i;
    } else if (shift && folder_view->anchor >= 0) {
        select_range(folder_view, folder_view->anchor, i);
    } else if (!(ev->button == GDK_BUTTON_SECONDARY && it->selected)) {
        select_only(folder_view, i);
        folder_view->anchor = i;
    }
    if (i >= 0)
        folder_view->focus = i;
    folder_view->drag_pending =
        it != nullptr && ev->button == GDK_BUTTON_PRIMARY && !ctrl && !shift;
    folder_view->drag_x = ev->x;
    folder_view->drag_y = ev->y;
    folder_view->drag_cx = ev->x + hval(folder_view);
    folder_view->drag_cy = ev->y + vval(folder_view);
    folder_view->drag_button = ev->button;
    selection_changed(folder_view);
    if (ev->button == GDK_BUTTON_SECONDARY)
        g_signal_emit(folder_view, signals[SIG_CONTEXT], 0, (GdkEvent *)ev);
    return TRUE;
}

/* ---- dragging files out ---------------------------------------------------------------- */

static GdkRectangle band_rect(const XdFolderView *folder_view) {
    double x = MIN(folder_view->band_x0, folder_view->band_x1),
           y = MIN(folder_view->band_y0, folder_view->band_y1);
    return (GdkRectangle){ifloor(x), ifloor(y),
                          ifloor(fabs(folder_view->band_x1 - folder_view->band_x0)),
                          ifloor(fabs(folder_view->band_y1 - folder_view->band_y0))};
}

/* Items touching the band are selected (toggled, with Ctrl). */
static void band_update(XdFolderView *folder_view) {
    GdkRectangle band = band_rect(folder_view);
    for (guint k = 0; k < folder_view->shown->len; k++) {
        item *it = g_ptr_array_index(folder_view->shown, k);
        GdkRectangle area = item_rect(folder_view, (int)k);
        gboolean hit = gdk_rectangle_intersect(&band, &area, nullptr);
        gboolean base =
            k < folder_view->band_base->len && g_array_index(folder_view->band_base, gboolean, k);
        it->selected = folder_view->band_toggle ? base != hit : hit;
    }
    selection_changed(folder_view);
}

static gboolean folder_view_button_release(GtkWidget *widget, GdkEventButton *ev) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    if (ev->button == GDK_BUTTON_PRIMARY && header_release(folder_view, ev))
        return TRUE;
    /* Single-click opens on release, unless the press became a drag. */
    gboolean click = folder_view->drag_pending && ev->button == GDK_BUTTON_PRIMARY;
    folder_view->drag_pending = FALSE;
    if (folder_view->banding) {
        folder_view->banding = FALSE;
        gtk_widget_queue_draw(widget);
    }
    int pressed = folder_view->pressed;
    folder_view->pressed = -1;
    if (folder_view->single_click && click && pressed >= 0 &&
        index_at(folder_view, ev->x, ev->y) == pressed)
        activate(folder_view, pressed);
    return FALSE;
}

/* Redraws item i only (an index past the end draws nothing). */
static void queue_draw_item(XdFolderView *folder_view, int i) {
    if (i < 0)
        return;
    GdkRectangle area = item_rect(folder_view, i);
    gtk_widget_queue_draw_area(GTK_WIDGET(folder_view), area.x - ifloor(hval(folder_view)),
                               area.y - ifloor(vval(folder_view)), area.width, area.height);
}

/* Single-click: underline the item under the pointer, with a hand. */
static void set_hover(XdFolderView *folder_view, int i) {
    if (i == folder_view->hover)
        return;
    /* Only the two labels change, not the whole folder. */
    queue_draw_item(folder_view, folder_view->hover);
    queue_draw_item(folder_view, i);
    folder_view->hover = i;
    GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(folder_view));
    if (win == nullptr)
        return;
    GdkCursor *hand = i >= 0 ? gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer")
                             : nullptr;
    gdk_window_set_cursor(win, hand);
    g_clear_object(&hand);
}

static gboolean folder_view_leave(GtkWidget *widget, [[maybe_unused]] GdkEventCrossing *ev) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    if (folder_view->hdr_resize < 0)
        set_header_cursor(folder_view, FALSE);
    set_hover(folder_view, -1);
    return FALSE;
}

static gboolean folder_view_motion(GtkWidget *widget, GdkEventMotion *ev) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    if (header_motion(folder_view, ev))
        return TRUE;
    if (folder_view->single_click)
        set_hover(folder_view,
                  ev->state & GDK_BUTTON1_MASK ? -1 : index_at(folder_view, ev->x, ev->y));
    if (folder_view->banding && (ev->state & GDK_BUTTON1_MASK)) {
        /* Past an edge, the view scrolls along. */
        if (folder_view->vadj != nullptr && (ev->y < 0 || ev->y > widget_h(folder_view)))
            gtk_adjustment_set_value(folder_view->vadj,
                                     vval(folder_view) +
                                         (ev->y < 0 ? ev->y : ev->y - widget_h(folder_view)));
        if (folder_view->hadj != nullptr && (ev->x < 0 || ev->x > widget_w(folder_view)))
            gtk_adjustment_set_value(folder_view->hadj,
                                     hval(folder_view) +
                                         (ev->x < 0 ? ev->x : ev->x - widget_w(folder_view)));
        folder_view->band_x1 = ev->x + hval(folder_view);
        folder_view->band_y1 = ev->y + vval(folder_view);
        band_update(folder_view);
        return TRUE;
    }
    if (!folder_view->drag_pending || !(ev->state & GDK_BUTTON1_MASK))
        return FALSE;
    if (!gtk_drag_check_threshold(widget, (int)folder_view->drag_x, (int)folder_view->drag_y,
                                  (int)ev->x, (int)ev->y))
        return TRUE;
    folder_view->drag_pending = FALSE;
    /* Control Panel's applets can be copied out (a shortcut to them), not
     * moved; its categories aren't files at all. */
    GdkDragAction actions = (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
    /* Network places and drives are connections, not files to copy. */
    if (folder_view->place == XD_PLACE_NETWORK)
        return TRUE;
    if (folder_view->place == XD_PLACE_CONTROL) {
        actions = (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_LINK);
        for (guint k = 0; k < folder_view->shown->len; k++) {
            item *sel = g_ptr_array_index(folder_view->shown, k);
            if (sel->selected && sel->is_dir)
                return TRUE;
        }
    }
    GtkTargetList *targets = gtk_target_list_new(nullptr, 0);
    gtk_target_list_add_uri_targets(targets, 0);
    gtk_drag_begin_with_coordinates(widget, targets, actions, (int)folder_view->drag_button,
                                    (GdkEvent *)ev, (int)folder_view->drag_x,
                                    (int)folder_view->drag_y);
    gtk_target_list_unref(targets);
    return TRUE;
}

static void folder_view_drag_begin(GtkWidget *widget, GdkDragContext *context) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    guint count = xd_folder_view_get_selected_count(folder_view);
    gtk_drag_set_icon_name(context, count > 1 ? "edit-copy" : "text-x-generic", 0, 0);
}

static void folder_view_drag_data_get(GtkWidget *widget, [[maybe_unused]] GdkDragContext *drag,
                                      GtkSelectionData *data, [[maybe_unused]] guint info,
                                      [[maybe_unused]] guint time) {
    GList *files = xd_folder_view_get_selected_files(XD_FOLDER_VIEW(widget));
    guint count = g_list_length(files);
    char **uris = g_new0(char *, count + 1);
    guint k = 0;
    for (GList *list = files; list != nullptr; list = list->next)
        uris[k++] = g_file_get_uri(list->data);
    gtk_selection_data_set_uris(data, uris);
    g_strfreev(uris);
    g_list_free_full(files, g_object_unref);
}

/* ---- dropping files in ------------------------------------------------------------------ */

static void forget_drag(XdFolderView *folder_view) {
    g_clear_pointer(&folder_view->drag_uris, g_strfreev);
    g_clear_pointer(&folder_view->over_event, gdk_event_free);
    folder_view->drag_requested = FALSE;
    if (folder_view->drop_index != -1) {
        folder_view->drop_index = -1;
        gtk_widget_queue_draw(GTK_WIDGET(folder_view));
    }
}

static char *filesystem_id(GFile *file) {
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_ID_FILESYSTEM,
                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    char *id = info != nullptr
                   ? g_strdup(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM))
                   : nullptr;
    g_clear_object(&info);
    return id;
}

/* The folder a drop at (x, y) goes into: a folder item under the pointer
 * (not one being dragged), else this folder. Sets drop_index. */
static GFile *drop_folder(XdFolderView *folder_view, GdkDragContext *ctx, int x, int y) {
    int i = index_at(folder_view, x, y);
    item *it = item_at(folder_view, i);
    gboolean own = gtk_drag_get_source_widget(ctx) == GTK_WIDGET(folder_view);
    int target = it != nullptr && it->is_dir && !(own && it->selected) &&
                         folder_view->place != XD_PLACE_TRASH
                     ? i
                     : -1;
    if (target != folder_view->drop_index) {
        folder_view->drop_index = target;
        gtk_widget_queue_draw(GTK_WIDGET(folder_view));
    }
    return target >= 0 ? it->file : folder_view->dir;
}

/* Windows' rules: Ctrl copies, Shift moves, Ctrl+Shift or Alt makes
 * shortcuts; otherwise files move within a drive and copy between drives.
 * 0 when the drop would do nothing (onto itself, or into the folder the
 * files are already in). */
static constexpr GdkDragAction NO_ACTION = (GdkDragAction)0;

/* Our own icons dropped back into this folder with Auto Arrange off: they
 * move to where they're dropped, rather than nothing happening. */
static gboolean can_reposition(XdFolderView *folder_view, GdkDragContext *ctx, GFile *dest) {
    return folder_view->placed && gtk_drag_get_source_widget(ctx) == GTK_WIDGET(folder_view) &&
           folder_view->dir != nullptr && dest != nullptr && g_file_equal(dest, folder_view->dir);
}

/* A drag from another program offering files it can't list yet: a
 * VirtualBox host's, whose files only come at the drop. */
static bool files_to_come(GdkDragContext *ctx, char **uris) {
    if ((uris != nullptr && uris[0] != nullptr) || gtk_drag_get_source_widget(ctx) != nullptr)
        return false;
    GdkAtom uri_list = gdk_atom_intern_static_string("text/uri-list");
    for (GList *target = gdk_drag_context_list_targets(ctx); target != nullptr;
         target = target->next) {
        if (GDK_POINTER_TO_ATOM(target->data) == uri_list)
            return true;
    }
    return false;
}

static GdkDragAction drop_action(XdFolderView *folder_view, GdkDragContext *ctx, char **uris,
                                 GFile *dest) {
    if (dest == nullptr || xd_place_of(dest) == XD_PLACE_COMPUTER ||
        xd_place_of(dest) == XD_PLACE_CONTROL || xd_place_of(dest) == XD_PLACE_NETWORK)
        return NO_ACTION;
    /* As Windows: what comes from outside is copied; the drop has the
     * list (and drop_action is asked again then). */
    if (files_to_come(ctx, uris) && xd_place_of(dest) != XD_PLACE_TRASH)
        return (gdk_drag_context_get_actions(ctx) & GDK_ACTION_COPY) ? GDK_ACTION_COPY
                                                                     : GDK_ACTION_MOVE;
    if (uris == nullptr || uris[0] == nullptr)
        return NO_ACTION;
    if (xd_place_of(dest) == XD_PLACE_TRASH) {
        /* Into the Recycle Bin: a delete, carried as a move. */
        if (gtk_drag_get_source_widget(ctx) == GTK_WIDGET(folder_view) &&
            folder_view->place == XD_PLACE_TRASH)
            return NO_ACTION;
        return (gdk_drag_context_get_actions(ctx) & GDK_ACTION_MOVE) ? GDK_ACTION_MOVE
                                                                      : GDK_ACTION_COPY;
    }
    /* The keyboard's modifiers as they are now (the keymap follows them
     * even while the pointer stands still), else the pointer's. */
    GdkModifierType mods = (GdkModifierType)0;
    if (folder_view->keymap != nullptr) {
        guint state = gdk_keymap_get_modifier_state(folder_view->keymap);
        mods = (GdkModifierType)state;
    } else {
        GdkDevice *dev = gdk_drag_context_get_device(ctx);
        GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(folder_view));
        if (dev != nullptr && win != nullptr)
            gdk_window_get_device_position(win, dev, nullptr, nullptr, &mods);
    }
    gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
    gboolean shift = (mods & GDK_SHIFT_MASK) != 0;
    gboolean alt = (mods & GDK_MOD1_MASK) != 0;
    GdkDragAction action;
    if ((ctrl && shift) || alt) {
        action = GDK_ACTION_LINK;
    } else if (ctrl) {
        action = GDK_ACTION_COPY;
    } else if (shift) {
        action = GDK_ACTION_MOVE;
    } else {
        GFile *first = g_file_new_for_uri(uris[0]);
        char *source_fs = filesystem_id(first);
        char *dest_fs = filesystem_id(dest);
        action = source_fs != nullptr && g_strcmp0(source_fs, dest_fs) == 0 ? GDK_ACTION_MOVE
                                                                            : GDK_ACTION_COPY;
        g_free(source_fs);
        g_free(dest_fs);
        g_object_unref(first);
    }
    gboolean all_here = TRUE;
    for (char **uri = uris; *uri != nullptr; uri++) {
        GFile *file = g_file_new_for_uri(*uri);
        GFile *parent = g_file_get_parent(file);
        gboolean into_itself = g_file_equal(file, dest) || g_file_has_prefix(dest, file);
        all_here = all_here && parent != nullptr && g_file_equal(parent, dest);
        g_clear_object(&parent);
        g_object_unref(file);
        if (into_itself)
            return NO_ACTION;
    }
    if (all_here && action == GDK_ACTION_MOVE)
        return can_reposition(folder_view, ctx, dest) ? GDK_ACTION_MOVE : NO_ACTION;
    GdkDragAction offered = gdk_drag_context_get_actions(ctx);
    /* Wayland has no "link" action: a shortcut is made from a copy. */
    GdkDragAction needed = action == GDK_ACTION_LINK ? GDK_ACTION_COPY : action;
    if (!(offered & needed))
        action = (offered & GDK_ACTION_COPY) ? GDK_ACTION_COPY : NO_ACTION;
    return action;
}

/* What to tell the source: Wayland's actions are copy, move and ask. */
static GdkDragAction protocol_action(GdkDragContext *ctx, GdkDragAction action) {
    if (action == GDK_ACTION_LINK && !(gdk_drag_context_get_actions(ctx) & GDK_ACTION_LINK))
        return GDK_ACTION_COPY;
    return action;
}

/* Ctrl, Shift or Alt pressed or released during a drag: the action is
 * chosen again where the pointer is, without waiting for it to move. GDK
 * passes a drop target's answer on only after a motion, so the last one
 * is seen again. */
static void keymap_changed([[maybe_unused]] GdkKeymap *keymap, gpointer data) {
    XdFolderView *folder_view = data;
    if (folder_view->over_event == nullptr || folder_view->drag_uris == nullptr ||
        folder_view->dropping)
        return;
    GdkEvent *again = gdk_event_copy(folder_view->over_event);
    gdk_event_put(again);
    gdk_event_free(again);
}

static gboolean folder_view_drag_motion(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y,
                                        guint time) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    if (folder_view->keymap == nullptr) {
        folder_view->keymap = gdk_keymap_get_for_display(gtk_widget_get_display(widget));
        folder_view->keymap_id = g_signal_connect(folder_view->keymap, "state-changed",
                                                  G_CALLBACK(keymap_changed), folder_view);
    }
    GdkEvent *motion = gtk_get_current_event();
    if (motion != nullptr && motion->type == GDK_DRAG_MOTION) {
        g_clear_pointer(&folder_view->over_event, gdk_event_free);
        folder_view->over_event = motion;
    } else if (motion != nullptr) {
        gdk_event_free(motion);
    }
    GdkAtom target = gtk_drag_dest_find_target(widget, ctx, nullptr);
    if (target == GDK_NONE || folder_view->dir == nullptr) {
        gdk_drag_status(ctx, NO_ACTION, time);
        return TRUE;
    }
    GFile *dest = drop_folder(folder_view, ctx, x, y);
    if (folder_view->drag_uris == nullptr) {
        /* The action depends on the files: ask for them first. */
        if (!folder_view->drag_requested) {
            folder_view->drag_requested = TRUE;
            gtk_drag_get_data(widget, ctx, target, time);
        }
        gdk_drag_status(ctx, NO_ACTION, time);
        return TRUE;
    }
    gdk_drag_status(
        ctx, protocol_action(ctx, drop_action(folder_view, ctx, folder_view->drag_uris, dest)),
        time);
    return TRUE;
}

static void folder_view_drag_leave(GtkWidget *widget, [[maybe_unused]] GdkDragContext *ctx,
                                   [[maybe_unused]] guint time) {
    forget_drag(XD_FOLDER_VIEW(widget));
}

static gboolean folder_view_drag_drop(GtkWidget *widget, GdkDragContext *ctx,
                                      [[maybe_unused]] gint x, [[maybe_unused]] gint y,
                                      guint time) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    GdkAtom target = gtk_drag_dest_find_target(widget, ctx, nullptr);
    if (target == GDK_NONE)
        return FALSE;
    folder_view->dropping = TRUE;
    gtk_drag_get_data(widget, ctx, target, time);
    return TRUE;
}

/* Moves the selected icons by as far as the pointer went from where the
 * drag began (onto the nearest free cells with Align to Grid). */
static void reposition(XdFolderView *folder_view, int x, int y) {
    int dx = ifloor(x + hval(folder_view) - folder_view->drag_cx);
    int dy = ifloor(y + vval(folder_view) - folder_view->drag_cy);
    GHashTable *taken = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < folder_view->shown->len && i < folder_view->rects->len; i++)
        if (!((item *)g_ptr_array_index(folder_view->shown, i))->selected)
            take_cells(folder_view, taken, g_array_index(folder_view->rects, GdkRectangle, i));
    int cols = grid_cols(folder_view);
    for (guint i = 0; i < folder_view->shown->len && i < folder_view->rects->len; i++) {
        item *it = g_ptr_array_index(folder_view->shown, i);
        if (!it->selected || it->key == nullptr)
            continue;
        GdkRectangle area = g_array_index(folder_view->rects, GdkRectangle, i);
        int nx = MAX(area.x + dx, 0), ny = MAX(area.y + dy, 0);
        if (folder_view->align_grid) {
            int col = CLAMP((int)lround((double)(nx - PAD) / folder_view->cell_w), 0, cols - 1);
            int row = MAX((int)lround((double)(ny - PAD) / folder_view->cell_h), 0);
            int cell = free_cell(folder_view, taken, row * cols + col);
            g_hash_table_add(taken, cell_key(cell / cols, cell % cols));
            nx = PAD + (cell % cols) * folder_view->cell_w;
            ny = PAD + (cell / cols) * folder_view->cell_h;
        }
        set_position(folder_view, it->key, nx, ny);
    }
    g_hash_table_unref(taken);
    layout_contents(folder_view);
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}

static void folder_view_drag_data_received(GtkWidget *widget, GdkDragContext *ctx, gint x,
                                           gint y, GtkSelectionData *sel,
                                           [[maybe_unused]] guint info, guint time) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    char **uris = gtk_selection_data_get_uris(sel);
    GFile *dest = drop_folder(folder_view, ctx, x, y);
    GdkDragAction action = drop_action(folder_view, ctx, uris, dest);
    if (!folder_view->dropping) {
        g_strfreev(folder_view->drag_uris);
        folder_view->drag_uris = uris != nullptr ? uris : g_new0(char *, 1);
        gdk_drag_status(ctx, protocol_action(ctx, action), time);
        return;
    }
    folder_view->dropping = FALSE;
    if (action == GDK_ACTION_MOVE && can_reposition(folder_view, ctx, dest)) {
        forget_drag(folder_view);
        gtk_drag_finish(ctx, TRUE, FALSE, time);
        reposition(folder_view, x, y);
        g_strfreev(uris);
        return;
    }
    GFile *target = g_object_ref(dest);
    forget_drag(folder_view);
    /* We do the move ourselves: the source mustn't delete anything. */
    gtk_drag_finish(ctx, action != NO_ACTION, FALSE, time);
    if (action != NO_ACTION)
        g_signal_emit(folder_view, signals[SIG_DROPPED], 0, uris, target, action);
    g_object_unref(target);
    g_strfreev(uris);
}

/* Arrow keys among icons put anywhere: the nearest one that way. */
static int spatial_target(XdFolderView *folder_view, guint key) {
    GdkRectangle focus_area = item_rect(folder_view, folder_view->focus);
    double fx = focus_area.x + focus_area.width / 2.0, fy = focus_area.y + focus_area.height / 2.0;
    int best = -1;
    double best_score = 0;
    for (int i = 0; i < (int)folder_view->shown->len; i++) {
        if (i == folder_view->focus)
            continue;
        GdkRectangle item_area = item_rect(folder_view, i);
        double cx = item_area.x + item_area.width / 2.0, cy = item_area.y + item_area.height / 2.0;
        double along = key == GDK_KEY_Up || key == GDK_KEY_Page_Up       ? fy - cy
                       : key == GDK_KEY_Down || key == GDK_KEY_Page_Down ? cy - fy
                       : key == GDK_KEY_Left                             ? fx - cx
                                                                         : cx - fx;
        double across = key == GDK_KEY_Left || key == GDK_KEY_Right ? fabs(cy - fy) : fabs(cx - fx);
        if (along < 1)
            continue;
        double score = along + 2 * across;
        if (best < 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

static int move_target(XdFolderView *folder_view, guint key) {
    int count = (int)folder_view->shown->len;
    if (folder_view->placed && folder_view->focus >= 0 &&
        (key == GDK_KEY_Up || key == GDK_KEY_Down || key == GDK_KEY_Left || key == GDK_KEY_Right ||
         key == GDK_KEY_Page_Up || key == GDK_KEY_Page_Down))
        return spatial_target(folder_view, key);
    int from = MAX(folder_view->focus, 0);
    int step_down = folder_view->mode == XD_VIEW_LIST || folder_view->mode == XD_VIEW_DETAILS
                        ? 1
                        : folder_view->cols;
    int step_right = folder_view->mode == XD_VIEW_LIST      ? folder_view->rows
                     : folder_view->mode == XD_VIEW_DETAILS ? 0
                                                            : 1;
    int page =
        folder_view->mode == XD_VIEW_LIST ? folder_view->rows
        : folder_view->mode == XD_VIEW_DETAILS
            ? MAX((widget_h(folder_view) - HEADER_H) / ROW_H - 1, 1)
            : MAX(widget_h(folder_view) / MAX(folder_view->cell_h, 1), 1) * folder_view->cols;
    /* Nothing focused yet: any arrow starts at the first item, as in
     * Windows (not one step past it). */
    if (folder_view->focus < 0 &&
        (key == GDK_KEY_Up || key == GDK_KEY_Down || key == GDK_KEY_Left || key == GDK_KEY_Right ||
         key == GDK_KEY_Page_Up || key == GDK_KEY_Page_Down))
        return count > 0 ? 0 : -1;
    switch (key) {
    case GDK_KEY_Up:
        return folder_view->focus < 0 ? 0 : from - step_down;
    case GDK_KEY_Down:
        return folder_view->focus < 0 ? 0 : from + step_down;
    case GDK_KEY_Left:
        return step_right == 0 ? -1 : from - step_right;
    case GDK_KEY_Right:
        return step_right == 0 ? -1 : from + step_right;
    case GDK_KEY_Home:
        return 0;
    case GDK_KEY_End:
        return count - 1;
    case GDK_KEY_Page_Up:
        return from - page;
    case GDK_KEY_Page_Down:
        return from + page;
    default:
        return -2;
    }
}

static gboolean folder_view_key_press(GtkWidget *widget, GdkEventKey *ev) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(widget);
    forget_keep(folder_view);
    int count = (int)folder_view->shown->len;
    gboolean ctrl = ev->state & GDK_CONTROL_MASK;
    gboolean shift = ev->state & GDK_SHIFT_MASK;
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        activate(folder_view, folder_view->focus);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Menu || (shift && ev->keyval == GDK_KEY_F10)) {
        g_signal_emit(folder_view, signals[SIG_CONTEXT], 0, (GdkEvent *)nullptr);
        return TRUE;
    }
    if (ctrl && (ev->keyval == GDK_KEY_a || ev->keyval == GDK_KEY_A)) {
        xd_folder_view_select_all(folder_view);
        return TRUE;
    }
    item *focused = item_at(folder_view, folder_view->focus);
    if (ctrl && ev->keyval == GDK_KEY_space && focused != nullptr) {
        focused->selected = !focused->selected;
        selection_changed(folder_view);
        return TRUE;
    }
    int target = move_target(folder_view, ev->keyval);
    if (target == -2 || count == 0)
        return FALSE;
    if (target < 0 && target != -1)
        target = 0;
    if (target == -1)
        return TRUE;
    target = CLAMP(target, 0, count - 1);
    folder_view->focus = target;
    if (shift && folder_view->anchor >= 0) {
        select_range(folder_view, folder_view->anchor, target);
    } else if (!ctrl) {
        select_only(folder_view, target);
        folder_view->anchor = target;
    }
    ensure_visible(folder_view, target);
    selection_changed(folder_view);
    return TRUE;
}

/* ---- GtkScrollable ------------------------------------------------------------ */

static void adj_changed([[maybe_unused]] GtkAdjustment *adjustment, gpointer data) {
    gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void set_adjustment(XdFolderView *folder_view, GtkAdjustment **slot, gulong *id,
                           GtkAdjustment *adjustment) {
    if (*slot != nullptr) {
        g_signal_handler_disconnect(*slot, *id);
        g_object_unref(*slot);
    }
    if (adjustment == nullptr)
        adjustment = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    *slot = g_object_ref_sink(adjustment);
    *id = g_signal_connect(adjustment, "value-changed", G_CALLBACK(adj_changed), folder_view);
    layout_contents(folder_view);
}

static void folder_view_set_property(GObject *obj, guint id, const GValue *value,
                                     GParamSpec *pspec) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(obj);
    switch (id) {
    case PROP_HADJ:
        set_adjustment(folder_view, &folder_view->hadj, &folder_view->hadj_id,
                       g_value_get_object(value));
        break;
    case PROP_VADJ:
        set_adjustment(folder_view, &folder_view->vadj, &folder_view->vadj_id,
                       g_value_get_object(value));
        break;
    case PROP_HPOLICY: {
        gint policy = g_value_get_enum(value);
        folder_view->hpolicy = (GtkScrollablePolicy)policy;
        break;
    }
    case PROP_VPOLICY: {
        gint policy = g_value_get_enum(value);
        folder_view->vpolicy = (GtkScrollablePolicy)policy;
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void folder_view_get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(obj);
    switch (id) {
    case PROP_HADJ:
        g_value_set_object(value, folder_view->hadj);
        break;
    case PROP_VADJ:
        g_value_set_object(value, folder_view->vadj);
        break;
    case PROP_HPOLICY:
        g_value_set_enum(value, (gint)folder_view->hpolicy);
        break;
    case PROP_VPOLICY:
        g_value_set_enum(value, (gint)folder_view->vpolicy);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void folder_view_size_allocate(GtkWidget *widget, GtkAllocation *alloc) {
    GTK_WIDGET_CLASS(xd_folder_view_parent_class)->size_allocate(widget, alloc);
    layout_contents(XD_FOLDER_VIEW(widget));
}

static void folder_view_dispose(GObject *obj) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(obj);
    if (folder_view->theme_id != 0) {
        g_signal_handler_disconnect(gtk_icon_theme_get_default(), folder_view->theme_id);
        folder_view->theme_id = 0;
    }
    if (folder_view->cancel != nullptr)
        g_cancellable_cancel(folder_view->cancel);
    g_clear_object(&folder_view->cancel);
    g_clear_pointer(&folder_view->thumbs, xd_thumb_queue_free);
    if (folder_view->reload_source != 0) {
        g_source_remove(folder_view->reload_source);
        folder_view->reload_source = 0;
    }
    if (folder_view->monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(folder_view->monitor, folder_view);
        g_file_monitor_cancel(folder_view->monitor);
        g_clear_object(&folder_view->monitor);
    }
    if (folder_view->hadj != nullptr) {
        g_signal_handler_disconnect(folder_view->hadj, folder_view->hadj_id);
        g_clear_object(&folder_view->hadj);
    }
    if (folder_view->vadj != nullptr) {
        g_signal_handler_disconnect(folder_view->vadj, folder_view->vadj_id);
        g_clear_object(&folder_view->vadj);
    }
    g_clear_object(&folder_view->dir);
    g_clear_object(&folder_view->listed);
    if (folder_view->watching_drives) {
        g_signal_handlers_disconnect_by_data(xd_computer_monitor(), folder_view);
        if (xd_udisks_manager() != nullptr)
            g_signal_handlers_disconnect_by_data(xd_udisks_manager(), folder_view);
        folder_view->watching_drives = FALSE;
    }
    if (folder_view->apps_monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(folder_view->apps_monitor, folder_view);
        g_clear_object(&folder_view->apps_monitor);
    }
    forget_keep(folder_view);
    forget_drag(folder_view);
    if (folder_view->keymap != nullptr) {
        g_signal_handler_disconnect(folder_view->keymap, folder_view->keymap_id);
        folder_view->keymap = nullptr;
    }
    g_clear_pointer(&folder_view->cut, g_hash_table_unref);
    g_clear_pointer(&folder_view->band_base, g_array_unref);
    xd_usershare_unwatch(folder_view);
    G_OBJECT_CLASS(xd_folder_view_parent_class)->dispose(obj);
}

static void folder_view_finalize(GObject *obj) {
    XdFolderView *folder_view = XD_FOLDER_VIEW(obj);
    g_ptr_array_unref(folder_view->shown);
    g_ptr_array_unref(folder_view->all);
    g_ptr_array_unref(folder_view->loading);
    g_clear_pointer(&folder_view->more_listed, g_ptr_array_unref);
    clear_headings(folder_view);
    g_array_unref(folder_view->headings);
    g_array_unref(folder_view->rects);
    g_hash_table_unref(folder_view->positions);
    G_OBJECT_CLASS(xd_folder_view_parent_class)->finalize(obj);
}

static void xd_folder_view_class_init(XdFolderViewClass *klass) {
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    oc->set_property = folder_view_set_property;
    oc->get_property = folder_view_get_property;
    oc->dispose = folder_view_dispose;
    oc->finalize = folder_view_finalize;
    wc->draw = folder_view_draw;
    wc->button_press_event = folder_view_button_press;
    wc->button_release_event = folder_view_button_release;
    wc->motion_notify_event = folder_view_motion;
    wc->leave_notify_event = folder_view_leave;
    wc->drag_begin = folder_view_drag_begin;
    wc->drag_data_get = folder_view_drag_data_get;
    wc->drag_motion = folder_view_drag_motion;
    wc->drag_leave = folder_view_drag_leave;
    wc->drag_drop = folder_view_drag_drop;
    wc->drag_data_received = folder_view_drag_data_received;
    wc->key_press_event = folder_view_key_press;
    wc->size_allocate = folder_view_size_allocate;
    g_object_class_override_property(oc, PROP_HADJ, "hadjustment");
    g_object_class_override_property(oc, PROP_VADJ, "vadjustment");
    g_object_class_override_property(oc, PROP_HPOLICY, "hscroll-policy");
    g_object_class_override_property(oc, PROP_VPOLICY, "vscroll-policy");

    signals[SIG_ACTIVATED] = g_signal_new("item-activated", G_TYPE_FROM_CLASS(klass),
                                          G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                          G_TYPE_NONE, 2, G_TYPE_FILE, G_TYPE_BOOLEAN);
    signals[SIG_SELECTION] = g_signal_new("selection-changed", G_TYPE_FROM_CLASS(klass),
                                          G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                          G_TYPE_NONE, 0);
    signals[SIG_CONTENTS] = g_signal_new("contents-changed", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                         G_TYPE_NONE, 0);
    signals[SIG_ERROR] = g_signal_new("load-error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                      0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1,
                                      G_TYPE_STRING);
    signals[SIG_CONTEXT] = g_signal_new("context-menu", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                        0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1,
                                        GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
    signals[SIG_DROPPED] = g_signal_new("files-dropped", G_TYPE_FROM_CLASS(klass),
                                        G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                        G_TYPE_NONE, 3, G_TYPE_STRV, G_TYPE_FILE,
                                        GDK_TYPE_DRAG_ACTION);
    signals[SIG_MIDDLE] = g_signal_new("item-middle-click", G_TYPE_FROM_CLASS(klass),
                                       G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                       G_TYPE_NONE, 2, G_TYPE_FILE, G_TYPE_BOOLEAN);
}

static void xd_folder_view_init(XdFolderView *folder_view) {
    folder_view->all = g_ptr_array_new_with_free_func(item_free);
    folder_view->loading = g_ptr_array_new_with_free_func(item_free);
    folder_view->shown = g_ptr_array_new();
    folder_view->rects = g_array_new(FALSE, TRUE, sizeof(GdkRectangle));
    folder_view->headings = g_array_new(FALSE, TRUE, sizeof(heading));
    folder_view->mode = XD_VIEW_ICONS;
    folder_view->thumb = XD_THUMB_DEFAULT;
    folder_view->focus = -1;
    folder_view->anchor = -1;
    folder_view->drop_index = -1;
    folder_view->hover = -1;
    folder_view->pressed = -1;
    folder_view->hdr_resize = -1;
    folder_view->hdr_press = -1;
    folder_view->auto_arrange = TRUE;
    folder_view->align_grid = TRUE;
    folder_view->positions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    folder_view->theme_id = g_signal_connect(gtk_icon_theme_get_default(), "changed",
                                             G_CALLBACK(icon_theme_changed), folder_view);
    xd_usershare_watch(shares_changed, folder_view);
    default_columns(folder_view);
    gtk_widget_set_can_focus(GTK_WIDGET(folder_view), TRUE);
    gtk_drag_dest_set(GTK_WIDGET(folder_view), (GtkDestDefaults)0, nullptr, 0,
                      (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
    gtk_drag_dest_add_uri_targets(GTK_WIDGET(folder_view));
    gtk_widget_add_events(GTK_WIDGET(folder_view),
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                              GDK_BUTTON1_MOTION_MASK | GDK_KEY_PRESS_MASK |
                              GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
}

/* ---- public API ------------------------------------------------------------------ */

GtkWidget *xd_folder_view_new(void) { return g_object_new(XD_TYPE_FOLDER_VIEW, nullptr); }

void xd_folder_view_set_location(XdFolderView *folder_view, GFile *dir) {
    g_return_if_fail(G_IS_FILE(dir));
    g_set_object(&folder_view->dir, dir);
    forget_keep(folder_view);
    if (folder_view->thumbs != nullptr)
        xd_thumb_queue_cancel(folder_view->thumbs);
    /* Icons' places are the folder's own: given just before, or none. */
    if (!folder_view->positions_given)
        g_hash_table_remove_all(folder_view->positions);
    folder_view->positions_given = FALSE;
    if (folder_view->monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(folder_view->monitor, folder_view);
        g_file_monitor_cancel(folder_view->monitor);
        g_clear_object(&folder_view->monitor);
    }
    folder_view->place = xd_place_of(dir);
    g_clear_object(&folder_view->listed);
    folder_view->listed =
        folder_view->place == XD_PLACE_TRASH ? xd_trash_files_dir() : g_object_ref(dir);
    if (folder_view->place == XD_PLACE_COMPUTER || folder_view->place == XD_PLACE_NETWORK) {
        /* Drives mapped or disconnected, places added (by any window). */
        char *config = xd_net_config_path();
        GFile *file = g_file_new_for_path(config);
        folder_view->monitor =
            g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
        if (folder_view->monitor != nullptr)
            g_signal_connect(folder_view->monitor, "changed", G_CALLBACK(monitor_changed),
                             folder_view);
        g_object_unref(file);
        g_free(config);
    }
    if (folder_view->place == XD_PLACE_COMPUTER || folder_view->place == XD_PLACE_NETWORK) {
        if (!folder_view->watching_drives) {
            GVolumeMonitor *vm = xd_computer_monitor();
            g_signal_connect(vm, "mount-added", G_CALLBACK(drives_changed), folder_view);
            g_signal_connect(vm, "mount-removed", G_CALLBACK(drives_changed), folder_view);
            g_signal_connect(vm, "mount-changed", G_CALLBACK(drives_changed), folder_view);
            GDBusObjectManager *udisks = xd_udisks_manager();
            if (udisks != nullptr) {
                g_signal_connect(udisks, "interface-proxy-properties-changed",
                                 G_CALLBACK(udisks_changed), folder_view);
                g_signal_connect(udisks, "object-added", G_CALLBACK(udisks_object_changed),
                                 folder_view);
                g_signal_connect(udisks, "object-removed", G_CALLBACK(udisks_object_changed),
                                 folder_view);
            }
            folder_view->watching_drives = TRUE;
        }
    } else if (folder_view->place == XD_PLACE_CONTROL) {
        if (folder_view->apps_monitor == nullptr) {
            /* GIO notices applications being installed or removed. */
            folder_view->apps_monitor = g_app_info_monitor_get();
            g_signal_connect(folder_view->apps_monitor, "changed", G_CALLBACK(apps_changed),
                             folder_view);
        }
    } else {
        folder_view->monitor = g_file_monitor_directory(
            folder_view->listed, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
        if (folder_view->monitor != nullptr)
            g_signal_connect(folder_view->monitor, "changed", G_CALLBACK(monitor_changed),
                             folder_view);
    }
    if (folder_view->hadj != nullptr)
        gtk_adjustment_set_value(folder_view->hadj, 0);
    if (folder_view->vadj != nullptr)
        gtk_adjustment_set_value(folder_view->vadj, 0);
    start_loading(folder_view, TRUE);
}

GFile *xd_folder_view_get_location(XdFolderView *folder_view) { return folder_view->dir; }

void xd_folder_view_reload(XdFolderView *folder_view) {
    if (folder_view->dir == nullptr)
        return;
    /* The old items stay until the new ones are in: no blank view, and
     * the scroll position is kept. */
    start_loading(folder_view, FALSE);
}

void xd_folder_view_set_mode(XdFolderView *folder_view, XdViewMode mode) {
    if (mode >= XD_VIEW_COUNT || mode == folder_view->mode)
        return;
    folder_view->mode = mode;
    if (folder_view->hadj != nullptr)
        gtk_adjustment_set_value(folder_view->hadj, 0);
    if (wants_groups(folder_view) != folder_view->grouped)
        refilter(folder_view); /* in groups or out of them: another order */
    else
        layout_contents(folder_view);
    ensure_visible(folder_view, folder_view->focus);
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}

XdViewMode xd_folder_view_get_mode(XdFolderView *folder_view) { return folder_view->mode; }

void xd_folder_view_set_thumbnail_size(XdFolderView *folder_view, int px) {
    px = CLAMP(px, XD_THUMB_MIN, XD_THUMB_MAX);
    if (px == folder_view->thumb)
        return;
    folder_view->thumb = px;
    layout_contents(folder_view);
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}

int xd_folder_view_get_thumbnail_size(XdFolderView *folder_view) { return folder_view->thumb; }

void xd_folder_view_set_sort(XdFolderView *folder_view, XdSortColumn column, gboolean descending) {
    if (column >= XD_SORT_COUNT)
        return;
    folder_view->sort = column;
    folder_view->desc = descending;
    /* Arranging by a column puts icons in its order, once, when they
     * aren't arranged automatically. */
    if (!folder_view->auto_arrange)
        g_hash_table_remove_all(folder_view->positions);
    refilter(folder_view);
}

XdSortColumn xd_folder_view_get_sort(XdFolderView *folder_view) { return folder_view->sort; }
gboolean xd_folder_view_get_sort_descending(XdFolderView *folder_view) { return folder_view->desc; }

void xd_folder_view_set_groups(XdFolderView *folder_view, gboolean groups) {
    folder_view->groups = groups;
    refilter(folder_view);
}

gboolean xd_folder_view_get_groups(XdFolderView *folder_view) { return folder_view->groups; }

void xd_folder_view_set_show_hidden(XdFolderView *folder_view, gboolean show) {
    folder_view->show_hidden = show;
    refilter(folder_view);
    g_signal_emit(folder_view, signals[SIG_CONTENTS], 0);
}

void xd_folder_view_set_hide_extensions(XdFolderView *folder_view, gboolean hide) {
    if (folder_view->hide_extensions == hide)
        return;
    folder_view->hide_extensions = hide;
    for (guint i = 0; i < folder_view->all->len; i++)
        ((item *)g_ptr_array_index(folder_view->all, i))->name_w = 0; /* List view measures names */
    layout_contents(folder_view);
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}

void xd_folder_view_set_single_click(XdFolderView *folder_view, gboolean single) {
    folder_view->single_click = single;
    if (!single)
        set_hover(folder_view, -1);
}

void xd_folder_view_select_all(XdFolderView *folder_view) {
    for (guint k = 0; k < folder_view->shown->len; k++)
        ((item *)g_ptr_array_index(folder_view->shown, k))->selected = TRUE;
    selection_changed(folder_view);
}

void xd_folder_view_invert_selection(XdFolderView *folder_view) {
    for (guint k = 0; k < folder_view->shown->len; k++) {
        item *it = g_ptr_array_index(folder_view->shown, k);
        it->selected = !it->selected;
    }
    selection_changed(folder_view);
}

guint xd_folder_view_get_item_count(XdFolderView *folder_view) { return folder_view->shown->len; }

GList *xd_folder_view_get_selected_files(XdFolderView *folder_view) {
    GList *out = nullptr;
    for (guint i = folder_view->shown->len; i > 0; i--) {
        item *it = g_ptr_array_index(folder_view->shown, i - 1);
        if (it->selected)
            out = g_list_prepend(out, g_object_ref(it->file));
    }
    return out;
}

guint xd_folder_view_get_selected_count(XdFolderView *folder_view) {
    guint count = 0;
    for (guint k = 0; k < folder_view->shown->len; k++)
        count += ((item *)g_ptr_array_index(folder_view->shown, k))->selected ? 1u : 0u;
    return count;
}

guint64 xd_folder_view_get_size(XdFolderView *folder_view, gboolean selected_only) {
    guint64 total = 0;
    for (guint k = 0; k < folder_view->shown->len; k++) {
        item *it = g_ptr_array_index(folder_view->shown, k);
        if (!it->is_dir && (!selected_only || it->selected))
            total += it->size;
    }
    return total;
}

void xd_folder_view_select_files(XdFolderView *folder_view, GList *files) {
    forget_keep(folder_view);
    if (files == nullptr)
        return;
    folder_view->keep = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (GList *list = files; list != nullptr; list = list->next)
        g_hash_table_add(folder_view->keep, g_file_get_uri(list->data));
    folder_view->keep_focus = g_file_get_uri(files->data);
    folder_view->keep_sticky = TRUE;
    guint found = apply_keep(folder_view);
    if (found > 0)
        ensure_visible(folder_view, folder_view->focus);
    /* What isn't here yet is selected when it appears. */
    if (found >= g_hash_table_size(folder_view->keep))
        forget_keep(folder_view);
    selection_changed(folder_view);
}

gboolean xd_folder_view_get_name_area(XdFolderView *folder_view, GFile *file, GdkRectangle *area) {
    int i = -1;
    for (guint k = 0; k < folder_view->shown->len; k++) {
        if (g_file_equal(((item *)g_ptr_array_index(folder_view->shown, k))->file, file)) {
            i = (int)k;
            break;
        }
    }
    if (i < 0)
        return FALSE;
    ensure_visible(folder_view, i);
    GdkRectangle item_area = item_rect(folder_view, i);
    GdkRectangle name_area = {item_area.x + 2, item_area.y, item_area.width - 4, ROW_H + 4};
    switch (folder_view->mode) {
    case XD_VIEW_THUMBNAILS:
        name_area.y = item_area.y + folder_view->thumb + 8;
        break;
    case XD_VIEW_ICONS:
        name_area.y = item_area.y + 32 + 8;
        break;
    case XD_VIEW_TILES:
        name_area = (GdkRectangle){item_area.x + 48 + 14, item_area.y + 6,
                                   item_area.width - 48 - 22, ROW_H + 4};
        break;
    case XD_VIEW_LIST:
        name_area = (GdkRectangle){item_area.x + LIST_ICON + 3, item_area.y - 2,
                                   MAX(item_area.width - LIST_ICON - 4, 160), ROW_H + 4};
        break;
    case XD_VIEW_DETAILS:
        name_area = (GdkRectangle){
            item_area.x + column_x(folder_view, XD_SORT_NAME) + LIST_ICON + 6, item_area.y - 2,
            folder_view->col_w[XD_SORT_NAME] - LIST_ICON - 10, ROW_H + 4};
        break;
    case XD_VIEW_COUNT:
        break;
    }
    name_area.x -= ifloor(hval(folder_view));
    name_area.y -= ifloor(vval(folder_view));
    *area = name_area;
    return TRUE;
}

void xd_folder_view_set_columns(XdFolderView *folder_view, const char *spec) {
    if (!xd_columns_parse(spec, XD_SORT_COUNT, folder_view->order, &folder_view->ncols,
                          folder_view->width))
        default_columns(folder_view);
    columns_changed(folder_view);
}

char *xd_folder_view_get_columns(XdFolderView *folder_view) {
    gboolean is_default = folder_view->ncols == XD_SORT_MODIFIED + 1;
    for (int k = 0; is_default && k < folder_view->ncols; k++)
        is_default = folder_view->order[k] == k;
    for (int column = 0; is_default && column < XD_SORT_COUNT; column++)
        is_default = folder_view->width[column] == 0;
    return is_default
               ? nullptr
               : xd_columns_format(folder_view->order, folder_view->ncols, folder_view->width);
}

void xd_folder_view_set_auto_arrange(XdFolderView *folder_view, gboolean on) {
    if (folder_view->auto_arrange == on)
        return;
    folder_view->auto_arrange = on;
    /* Turned off, the icons stay where they are now (laid out as they
     * were arranged); turned on, they're arranged again. */
    g_hash_table_remove_all(folder_view->positions);
    folder_view->positions_mode = folder_view->mode;
    columns_changed(folder_view);
}

gboolean xd_folder_view_get_auto_arrange(XdFolderView *folder_view) {
    return folder_view->auto_arrange;
}

void xd_folder_view_set_align_to_grid(XdFolderView *folder_view, gboolean on) {
    folder_view->align_grid = on;
    if (on && folder_view->placed) {
        snap_positions(folder_view);
        columns_changed(folder_view);
    }
}

gboolean xd_folder_view_get_align_to_grid(XdFolderView *folder_view) {
    return folder_view->align_grid;
}

void xd_folder_view_set_positions(XdFolderView *folder_view, XdViewMode mode, char **names,
                                  const int *xy, gsize count) {
    g_hash_table_remove_all(folder_view->positions);
    folder_view->positions_mode = mode;
    for (gsize i = 0; i < count; i++)
        set_position(folder_view, names[i], xy[2 * i], xy[2 * i + 1]);
    folder_view->positions_given = TRUE;
    columns_changed(folder_view);
}

gsize xd_folder_view_get_positions(XdFolderView *folder_view, XdViewMode *mode, char ***names,
                                   int **xy) {
    *mode = folder_view->positions_mode;
    *names = g_new0(char *, folder_view->all->len + 1);
    *xy = g_new0(int, 2 * (gsize)folder_view->all->len + 1);
    gsize count = 0;
    for (guint i = 0; i < folder_view->all->len && !folder_view->auto_arrange; i++) {
        item *it = g_ptr_array_index(folder_view->all, i);
        const int *position =
            it->key != nullptr ? g_hash_table_lookup(folder_view->positions, it->key) : nullptr;
        if (position == nullptr)
            continue;
        (*names)[count] = g_strdup(it->key);
        (*xy)[2 * count] = position[0];
        (*xy)[2 * count + 1] = position[1];
        count++;
    }
    return count;
}

void xd_folder_view_set_cut_files(XdFolderView *folder_view, GList *files) {
    g_clear_pointer(&folder_view->cut, g_hash_table_unref);
    if (files != nullptr) {
        folder_view->cut = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
        for (GList *list = files; list != nullptr; list = list->next)
            g_hash_table_add(folder_view->cut, g_file_get_uri(list->data));
    }
    gtk_widget_queue_draw(GTK_WIDGET(folder_view));
}
