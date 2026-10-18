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
    SIG_COUNT
};
static guint signals[SIG_COUNT];

G_DEFINE_TYPE_WITH_CODE(XdFolderView, xd_folder_view, GTK_TYPE_DRAWING_AREA,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, nullptr))

/* ---- items --------------------------------------------------------------------- */

/* The name as shown (Folder Options can hide known extensions). */
static const char *item_label(const XdFolderView *v, const item *it) {
    return v->hide_extensions && it->short_name != nullptr ? it->short_name : it->name;
}

static void item_free(gpointer p) {
    item *it = p;
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
    size_t n = strlen(digits);
    for (size_t i = 0; i < n; i++) {
        g_string_append_c(out, digits[i]);
        if ((n - i - 1) % 3 == 0 && i + 1 < n)
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

static GdkPixbuf *item_icon(XdFolderView *v, item *it, int px) {
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
    (void)v;
    return it->icon_pb;
}

/* Another icon theme (Display Properties): every item's icon is drawn again
 * from it, as the rest of the window already is. */
static void icon_theme_changed([[maybe_unused]] GtkIconTheme *theme, gpointer data) {
    XdFolderView *v = data;
    for (guint i = 0; i < v->all->len; i++) {
        item *it = g_ptr_array_index(v->all, i);
        g_clear_object(&it->icon_pb);
        it->icon_px = 0;
    }
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

/* A thumbnail xd-thumbnailer made (nullptr `path`: it couldn't): the item
 * shows it, if it's still here. */
static void thumb_done(const char *uri, const char *path, gpointer data) {
    XdFolderView *v = data;
    GFile *file = g_file_new_for_uri(uri);
    for (guint i = 0; i < v->all->len; i++) {
        item *it = g_ptr_array_index(v->all, i);
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
        gtk_widget_queue_draw(GTK_WIDGET(v));
        break;
    }
    g_object_unref(file);
}

/* Asks for a thumbnail the cache doesn't have (or has only smaller, or of
 * an older version of the file). Asked while drawing, so what's on screen
 * is asked for last, and made first. */
static void want_thumb(XdFolderView *v, item *it, int px) {
    if (!it->thumb_want || it->thumb_failed)
        return;
    XdThumbSize size = xd_thumb_size_for(px);
    if (it->thumb_path != nullptr && !it->thumb_stale &&
        (size == XD_THUMB_NORMAL || strstr(it->thumb_path, "/normal/") == nullptr))
        return;
    if (v->thumbs == nullptr)
        v->thumbs = xd_thumb_queue_new(thumb_done, v);
    char *uri = g_file_get_uri(it->file);
    xd_thumb_queue_request(v->thumbs, uri, size);
    g_free(uri);
}

/* A cached thumbnail scaled to fit px x px, or nullptr (one is asked for:
 * the item is drawn again when it's made). */
static GdkPixbuf *item_thumb(XdFolderView *v, item *it, int px) {
    want_thumb(v, it, px);
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

static gboolean is_icon_mode(XdViewMode m) {
    return m == XD_VIEW_THUMBNAILS || m == XD_VIEW_ICONS || m == XD_VIEW_TILES;
}

/* My Computer always shows its groups, as XP does; other folders when
 * Show in Groups is on. The icon views and Details have them; XP's List
 * view never shows groups (its list control can't). */
static gboolean wants_groups(const XdFolderView *v) {
    return (is_icon_mode(v->mode) || v->mode == XD_VIEW_DETAILS) &&
           (v->place == XD_PLACE_COMPUTER || v->groups);
}

/* XP's headings: My Computer by kind of drive; folders by the sort
 * column (initial letter, type, size class, or how long ago). */
static void set_group(const XdFolderView *v, item *it, GDateTime *now) {
    g_free(it->group);
    it->group = nullptr;
    it->group_rank = 0;
    if (v->place == XD_PLACE_COMPUTER) {
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
    switch (v->sort) {
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
        gint64 when = v->sort == XD_SORT_CREATED    ? it->ctime
                      : v->sort == XD_SORT_ACCESSED ? it->atime
                                                    : it->mtime;
        GDateTime *t = g_date_time_new_from_unix_local(when);
        int rank = 0;
        if (t != nullptr && now != nullptr) {
            int y = g_date_time_get_year(now), ty = g_date_time_get_year(t);
            int doy = g_date_time_get_day_of_year(now), tdoy = g_date_time_get_day_of_year(t);
            int days = (y - ty) * 365 + (doy - tdoy);
            int dow = g_date_time_get_day_of_week(now) - 1; /* Monday 0 */
            int m = g_date_time_get_month(now), tm = g_date_time_get_month(t);
            if (days <= 0)
                rank = 7;
            else if (days == 1)
                rank = 6;
            else if (days <= dow)
                rank = 5;
            else if (days <= dow + 7)
                rank = 4;
            else if (y == ty && m == tm)
                rank = 3;
            else if ((y == ty && m - tm == 1) || (y - ty == 1 && m == 1 && tm == 12))
                rank = 2;
            else if (y == ty)
                rank = 1;
        }
        if (t != nullptr)
            g_date_time_unref(t);
        it->group = g_strdup(ages[rank]);
        it->group_rank = rank;
        return;
    }
    case XD_SORT_NAME:
    case XD_SORT_COUNT:
        break;
    }
    /* By name: the initial letter. */
    gunichar c = g_utf8_get_char_validated(it->name, -1);
    if (c != (gunichar)-1 && c != (gunichar)-2 && g_unichar_isalpha(c)) {
        char buf[8] = {0};
        g_unichar_to_utf8(g_unichar_toupper(c), buf);
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

static int compare_groups(const XdFolderView *v, const item *a, const item *b) {
    int c = (a->group_rank > b->group_rank) - (a->group_rank < b->group_rank);
    if (c == 0 && v->place != XD_PLACE_COMPUTER && !ranked_groups(v->sort))
        c = g_utf8_collate(a->group != nullptr ? a->group : "", b->group != nullptr ? b->group : "");
    return v->place != XD_PLACE_COMPUTER && v->desc ? -c : c;
}

static int compare_items(gconstpointer pa, gconstpointer pb, gpointer data) {
    const item *a = *(const item *const *)pa;
    const item *b = *(const item *const *)pb;
    const XdFolderView *v = data;
    if (v->grouped) {
        int g = compare_groups(v, a, b);
        if (g != 0)
            return g;
    }
    /* My Computer's drives and Control Panel's categories keep their own
     * order. */
    if (v->place == XD_PLACE_COMPUTER || v->place == XD_PLACE_NETWORK ||
        (v->place == XD_PLACE_CONTROL && a->is_dir && b->is_dir))
        return a->order - b->order;
    /* Folders first, always, as in Windows. */
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    int c = 0;
    switch (v->sort) {
    case XD_SORT_SIZE:
        c = (a->size > b->size) - (a->size < b->size);
        break;
    case XD_SORT_TYPE:
        c = g_utf8_collate(a->type_desc, b->type_desc);
        break;
    case XD_SORT_MODIFIED:
        c = (a->mtime > b->mtime) - (a->mtime < b->mtime);
        break;
    case XD_SORT_CREATED:
        c = (a->ctime > b->ctime) - (a->ctime < b->ctime);
        break;
    case XD_SORT_ACCESSED:
        c = (a->atime > b->atime) - (a->atime < b->atime);
        break;
    case XD_SORT_ATTRIBUTES:
        c = g_strcmp0(a->attrs, b->attrs);
        break;
    case XD_SORT_OWNER:
        c = g_utf8_collate(a->owner != nullptr ? a->owner : "", b->owner != nullptr ? b->owner : "");
        break;
    case XD_SORT_NAME:
    case XD_SORT_COUNT:
        break;
    }
    if (c == 0)
        c = strcmp(a->collate_key, b->collate_key);
    return v->desc ? -c : c;
}

static void layout_contents(XdFolderView *v);

static void refilter(XdFolderView *v) {
    g_ptr_array_set_size(v->shown, 0);
    v->grouped = wants_groups(v);
    GDateTime *now = v->grouped ? g_date_time_new_now_local() : nullptr;
    for (guint i = 0; i < v->all->len; i++) {
        item *it = g_ptr_array_index(v->all, i);
        if (v->grouped)
            set_group(v, it, now);
        if (v->show_hidden || !it->hidden)
            g_ptr_array_add(v->shown, it);
        else
            it->selected = FALSE;
    }
    if (now != nullptr)
        g_date_time_unref(now);
    g_ptr_array_sort_with_data(v->shown, compare_items, v);
    if (v->focus >= (int)v->shown->len)
        v->focus = (int)v->shown->len - 1;
    layout_contents(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

/* ---- layout ------------------------------------------------------------------------ */

/* floor() to int, clamped to a sane coordinate range. */
static int ifloor(double v) {
    double f = floor(v);
    if (!(f > -1e7))
        return -10000000;
    if (!(f < 1e7))
        return 10000000;
    return (int)f;
}

static int widget_w(XdFolderView *v) { return MAX(gtk_widget_get_allocated_width(GTK_WIDGET(v)), 1); }
static int widget_h(XdFolderView *v) { return MAX(gtk_widget_get_allocated_height(GTK_WIDGET(v)), 1); }

static int text_width(XdFolderView *v, const char *text) {
    PangoLayout *l = gtk_widget_create_pango_layout(GTK_WIDGET(v), text);
    int w = 0;
    int h = 0;
    pango_layout_get_pixel_size(l, &w, &h);
    g_object_unref(l);
    return w;
}

static void configure(GtkAdjustment *a, double page, double upper) {
    if (a == nullptr)
        return;
    upper = MAX(upper, page);
    double value = CLAMP(gtk_adjustment_get_value(a), 0.0, upper - page);
    gtk_adjustment_configure(a, value, 0.0, upper, ROW_H * 2.0, page * 0.9, page);
}

static void clear_headings(XdFolderView *v) {
    for (guint i = 0; i < v->headings->len; i++)
        g_free(g_array_index(v->headings, heading, i).text);
    g_array_set_size(v->headings, 0);
}

/* The icon views in groups: each under its heading, starting a new row. */
static void layout_groups(XdFolderView *v) {
    int n = (int)v->shown->len;
    g_array_set_size(v->rects, (guint)n);
    clear_headings(v);
    int y = PAD, col = 0;
    const char *current = nullptr;
    for (int i = 0; i < n; i++) {
        item *it = g_ptr_array_index(v->shown, (guint)i);
        const char *g = it->group != nullptr ? it->group : "";
        if (current == nullptr || strcmp(g, current) != 0) {
            if (current != nullptr)
                y += (col > 0 ? v->cell_h : 0) + GROUP_GAP;
            heading hd = {y, g_strdup(g)};
            g_array_append_val(v->headings, hd);
            y += GROUP_H;
            col = 0;
            current = g;
        }
        g_array_index(v->rects, GdkRectangle, i) =
            (GdkRectangle){PAD + col * v->cell_w, y, v->cell_w, v->cell_h};
        if (++col == v->cols) {
            col = 0;
            y += v->cell_h;
        }
    }
    v->content_h = y + (col > 0 ? v->cell_h : 0) + PAD;
}

/* Details in groups: each heading on a line of its own above its rows,
 * under the column header. */
static void layout_detail_groups(XdFolderView *v) {
    int n = (int)v->shown->len;
    g_array_set_size(v->rects, (guint)n);
    clear_headings(v);
    int y = HEADER_H;
    const char *current = nullptr;
    for (int i = 0; i < n; i++) {
        item *it = g_ptr_array_index(v->shown, (guint)i);
        const char *g = it->group != nullptr ? it->group : "";
        if (current == nullptr || strcmp(g, current) != 0) {
            if (current != nullptr)
                y += GROUP_GAP;
            heading hd = {y, g_strdup(g)};
            g_array_append_val(v->headings, hd);
            y += GROUP_H;
            current = g;
        }
        g_array_index(v->rects, GdkRectangle, i) = (GdkRectangle){0, y, v->cell_w, ROW_H};
        y += ROW_H;
    }
    v->content_h = y + PAD;
}

/* ---- Details columns ---------------------------------------------------------------- */

static const char *const folder_titles[XD_SORT_COUNT] = {
    "Name", "Size", "Type", "Date Modified", "Date Created", "Date Accessed", "Attributes", "Owner"};

static const char *column_title(const XdFolderView *v, int col) {
    static const char *const computer_titles[] = {"Name", "Total Size", "Type", "Free Space"};
    static const char *const trash_titles[] = {"Name", "Size", "Original Location",
                                               "Date Deleted"};
    static const char *const control_titles[] = {"Name", "", "Comments", ""};
    static const char *const network_titles[] = {"Name", "", "Comments", "Network Location"};
    if (col < 0 || col >= XD_SORT_COUNT)
        return "";
    if (col <= XD_SORT_MODIFIED) {
        switch (v->place) {
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
static gboolean column_available(const XdFolderView *v, int col) {
    switch (v->place) {
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
static int default_width(const XdFolderView *v, int col, int view_w) {
    switch ((XdSortColumn)col) {
    case XD_SORT_SIZE:
        return 80;
    case XD_SORT_TYPE:
        return v->place == XD_PLACE_CONTROL ? MAX(260, view_w * 3 / 5) : 150;
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

static void default_columns(XdFolderView *v) {
    v->ncols = 0;
    for (int c = 0; c <= XD_SORT_MODIFIED; c++)
        v->order[v->ncols++] = c;
    memset(v->width, 0, sizeof(v->width));
}

/* Where column `col` starts (content coordinates). */
static int column_x(const XdFolderView *v, int col) {
    int x = 0;
    for (int i = 0; i < v->ncols && v->order[i] != col; i++)
        x += v->col_w[v->order[i]];
    return x;
}

/* A date for a Details column, formatted the first time it's shown. */
static const char *date_cell(char **text, gint64 when) {
    if (*text == nullptr) {
        GDateTime *t = when != 0 ? g_date_time_new_from_unix_local(when) : nullptr;
        *text = t != nullptr ? g_date_time_format(t, "%x %H:%M") : g_strdup("");
        if (t != nullptr)
            g_date_time_unref(t);
    }
    return *text;
}

static const char *or_empty(const char *s) { return s != nullptr ? s : ""; }

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

static gboolean uses_rects(const XdFolderView *v) { return v->grouped || v->placed; }

static constexpr int GRID_COLS_MAX = 4095;

static gpointer cell_key(int row, int col) {
    return GUINT_TO_POINTER(((guint)row << 12 | (guint)col) + 1u);
}

static int grid_cols(const XdFolderView *v) { return CLAMP(v->cols, 1, GRID_COLS_MAX); }

/* Marks the grid cells `r` covers. */
static void take_cells(const XdFolderView *v, GHashTable *taken, GdkRectangle r) {
    int c0 = MAX((r.x - PAD) / v->cell_w, 0), c1 = (r.x + r.width - 1 - PAD) / v->cell_w;
    int r0 = MAX((r.y - PAD) / v->cell_h, 0), r1 = (r.y + r.height - 1 - PAD) / v->cell_h;
    for (int row = r0; row <= r1; row++)
        for (int col = c0; col <= c1 && col < grid_cols(v); col++)
            g_hash_table_add(taken, cell_key(row, col));
}

/* The first free cell from `start` on (row by row): its index. */
static int free_cell(const XdFolderView *v, GHashTable *taken, int start) {
    int cols = grid_cols(v);
    int cell = MAX(start, 0);
    while (g_hash_table_contains(taken, cell_key(cell / cols, cell % cols)))
        cell++;
    return cell;
}

static void set_position(XdFolderView *v, const char *key, int x, int y) {
    int *p = g_new(int, 2);
    p[0] = x;
    p[1] = y;
    g_hash_table_replace(v->positions, g_strdup(key), p);
}

/* Icons where they were put; the rest, in order, in the free cells
 * (which is how they're arranged when none were put anywhere yet). They
 * keep those places, so a narrower window doesn't move them. */
static void layout_placed(XdFolderView *v) {
    int n = (int)v->shown->len;
    if (v->positions_mode != v->mode) {
        /* Put in another view's cells: arranged afresh for this one. */
        g_hash_table_remove_all(v->positions);
        v->positions_mode = v->mode;
    }
    g_array_set_size(v->rects, (guint)n);
    GHashTable *taken = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (int i = 0; i < n; i++) {
        item *it = g_ptr_array_index(v->shown, (guint)i);
        const int *p = it->key != nullptr ? g_hash_table_lookup(v->positions, it->key) : nullptr;
        GdkRectangle r = {0, 0, 0, 0};
        if (p != nullptr) {
            r = (GdkRectangle){MAX(p[0], 0), MAX(p[1], 0), v->cell_w, v->cell_h};
            take_cells(v, taken, r);
        }
        g_array_index(v->rects, GdkRectangle, i) = r;
    }
    int cols = grid_cols(v);
    int cell = 0;
    int right = 0, bottom = 0;
    for (int i = 0; i < n; i++) {
        GdkRectangle *r = &g_array_index(v->rects, GdkRectangle, i);
        if (r->width == 0) {
            cell = free_cell(v, taken, cell);
            *r = (GdkRectangle){PAD + (cell % cols) * v->cell_w, PAD + (cell / cols) * v->cell_h,
                                v->cell_w, v->cell_h};
            take_cells(v, taken, *r);
            item *it = g_ptr_array_index(v->shown, (guint)i);
            if (it->key != nullptr)
                set_position(v, it->key, r->x, r->y);
        }
        right = MAX(right, r->x + r->width);
        bottom = MAX(bottom, r->y + r->height);
    }
    g_hash_table_unref(taken);
    v->content_w = MAX(widget_w(v), right + PAD);
    v->content_h = bottom + PAD;
}

/* Align to Grid: every icon onto the nearest free cell. */
static void snap_positions(XdFolderView *v) {
    if (!v->placed)
        return;
    GHashTable *taken = g_hash_table_new(g_direct_hash, g_direct_equal);
    int cols = grid_cols(v);
    for (guint i = 0; i < v->shown->len && i < v->rects->len; i++) {
        item *it = g_ptr_array_index(v->shown, i);
        GdkRectangle r = g_array_index(v->rects, GdkRectangle, i);
        int col = CLAMP((int)lround((double)(r.x - PAD) / v->cell_w), 0, cols - 1);
        int row = MAX((int)lround((double)(r.y - PAD) / v->cell_h), 0);
        int cell = free_cell(v, taken, row * cols + col);
        g_hash_table_add(taken, cell_key(cell / cols, cell % cols));
        if (it->key != nullptr)
            set_position(v, it->key, PAD + (cell % cols) * v->cell_w,
                         PAD + (cell / cols) * v->cell_h);
    }
    g_hash_table_unref(taken);
}

static void layout_contents(XdFolderView *v) {
    int w = widget_w(v);
    int h = widget_h(v);
    int n = (int)v->shown->len;
    v->placed = is_icon_mode(v->mode) && !v->grouped && !v->auto_arrange &&
                (v->place == XD_PLACE_FOLDER || v->place == XD_PLACE_TRASH);
    switch (v->mode) {
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES:
        v->cell_w = v->mode == XD_VIEW_THUMBNAILS ? v->thumb + 24
                    : v->mode == XD_VIEW_ICONS    ? ICONS_W
                                                  : TILE_W;
        v->cell_h = v->mode == XD_VIEW_THUMBNAILS ? v->thumb + 40
                    : v->mode == XD_VIEW_ICONS    ? ICONS_H
                                                  : TILE_H;
        v->cols = MAX((w - PAD) / v->cell_w, 1);
        v->rows = (n + v->cols - 1) / v->cols;
        v->content_w = w;
        v->content_h = PAD * 2 + v->rows * v->cell_h;
        if (v->grouped)
            layout_groups(v);
        else if (v->placed)
            layout_placed(v);
        break;
    case XD_VIEW_LIST: {
        /* Windows' List view fills columns top to bottom and scrolls
         * sideways. Columns are as wide as the widest name (capped). */
        v->rows = MAX((h - PAD * 2) / ROW_H, 1);
        v->cols = (n + v->rows - 1) / v->rows;
        int widest = 60;
        for (int i = 0; i < n; i++) {
            item *it = g_ptr_array_index(v->shown, (guint)i);
            if (it->name_w == 0)
                it->name_w = text_width(v, item_label(v, it));
            widest = MAX(widest, it->name_w);
        }
        v->cell_w = MIN(widest + LIST_ICON + 16, LIST_COL_MAX);
        v->cell_h = ROW_H;
        v->content_w = PAD * 2 + v->cols * v->cell_w;
        v->content_h = h;
        break;
    }
    case XD_VIEW_DETAILS: {
        /* The columns chosen that this place has (Control Panel: Name and
         * Comments), at their widths; Name takes the spare room until
         * it's given a width. */
        memset(v->col_w, 0, sizeof(v->col_w));
        int others = 0;
        for (int i = 0; i < v->ncols; i++) {
            int c = v->order[i];
            if (c == XD_SORT_NAME || !column_available(v, c))
                continue;
            v->col_w[c] = v->width[c] > 0 ? v->width[c] : default_width(v, c, w);
            others += v->col_w[c];
        }
        v->col_w[XD_SORT_NAME] = v->width[XD_SORT_NAME] > 0 ? v->width[XD_SORT_NAME]
                                                             : MAX(220, w - others);
        v->cell_w = v->col_w[XD_SORT_NAME] + others;
        v->cell_h = ROW_H;
        v->cols = 1;
        v->rows = n;
        v->content_w = v->cell_w;
        v->content_h = HEADER_H + n * ROW_H + PAD;
        if (v->grouped)
            layout_detail_groups(v);
        break;
    }
    case XD_VIEW_COUNT:
        break;
    }
    configure(v->hadj, w, v->content_w);
    configure(v->vadj, h, v->content_h);
}

/* Rectangle of item i in content coordinates. */
static GdkRectangle item_rect(const XdFolderView *v, int i) {
    switch (v->mode) {
    case XD_VIEW_LIST:
        return (GdkRectangle){PAD + (i / v->rows) * v->cell_w, PAD + (i % v->rows) * ROW_H,
                              v->cell_w - 4, ROW_H};
    case XD_VIEW_DETAILS:
        if (!v->grouped)
            return (GdkRectangle){0, HEADER_H + i * ROW_H, v->cell_w, ROW_H};
        break; /* the rows' own places, below */
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES:
    case XD_VIEW_COUNT:
        break;
    }
    if (uses_rects(v) && i >= 0 && (guint)i < v->rects->len)
        return g_array_index(v->rects, GdkRectangle, i);
    return (GdkRectangle){PAD + (i % v->cols) * v->cell_w, PAD + (i / v->cols) * v->cell_h,
                          v->cell_w, v->cell_h};
}

static GdkRectangle band_rect(const XdFolderView *v);

static double hval(const XdFolderView *v) { return v->hadj ? gtk_adjustment_get_value(v->hadj) : 0; }
static double vval(const XdFolderView *v) { return v->vadj ? gtk_adjustment_get_value(v->vadj) : 0; }

static int index_at(XdFolderView *v, double wx, double wy) {
    double x = wx + hval(v);
    double y = wy + vval(v);
    int n = (int)v->shown->len;
    int i = -1;
    switch (v->mode) {
    case XD_VIEW_LIST: {
        int col = ifloor((x - PAD) / v->cell_w);
        int row = ifloor((y - PAD) / ROW_H);
        if (col >= 0 && row >= 0 && row < v->rows)
            i = col * v->rows + row;
        break;
    }
    case XD_VIEW_DETAILS:
        if (wy < HEADER_H)
            break;
        if (!v->grouped) {
            i = ifloor((y - HEADER_H) / ROW_H);
            break;
        }
        [[fallthrough]]; /* rows in groups: their own places */
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES: {
        if (uses_rects(v)) {
            for (guint k = 0; k < v->rects->len; k++) {
                GdkRectangle r = g_array_index(v->rects, GdkRectangle, k);
                if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
                    i = (int)k;
            }
            break;
        }
        int col = ifloor((x - PAD) / v->cell_w);
        int row = ifloor((y - PAD) / v->cell_h);
        if (col >= 0 && col < v->cols && row >= 0)
            i = row * v->cols + col;
        break;
    }
    case XD_VIEW_COUNT:
        break;
    }
    return i >= 0 && i < n ? i : -1;
}

static void ensure_visible(XdFolderView *v, int i) {
    if (i < 0)
        return;
    GdkRectangle r = item_rect(v, i);
    if (v->placed && v->hadj != nullptr) {
        /* Icons put anywhere may be off to the side too. */
        double val = hval(v);
        double page = widget_w(v);
        if (r.x < val)
            gtk_adjustment_set_value(v->hadj, r.x - PAD);
        else if (r.x + r.width > val + page)
            gtk_adjustment_set_value(v->hadj, r.x + r.width - page + PAD);
    }
    if (v->mode == XD_VIEW_LIST && v->hadj != nullptr) {
        double val = hval(v);
        double page = widget_w(v);
        if (r.x < val)
            gtk_adjustment_set_value(v->hadj, r.x - PAD);
        else if (r.x + r.width > val + page)
            gtk_adjustment_set_value(v->hadj, r.x + r.width - page + PAD);
    } else if (v->vadj != nullptr) {
        double top = v->mode == XD_VIEW_DETAILS ? HEADER_H : 0;
        double val = vval(v);
        double page = widget_h(v);
        if (r.y - top < val)
            gtk_adjustment_set_value(v->vadj, r.y - top);
        else if (r.y + r.height > val + page)
            gtk_adjustment_set_value(v->vadj, r.y + r.height - page + PAD);
    }
}

/* ---- drawing ------------------------------------------------------------------------ */

typedef struct colors {
    GdkRGBA base, text, dim, sel_bg, sel_fg, header_bg, line;
} colors;

static GdkRGBA lookup(GtkStyleContext *sc, const char *name, const char *fallback) {
    GdkRGBA c;
    if (!gtk_style_context_lookup_color(sc, name, &c))
        gdk_rgba_parse(&c, fallback);
    return c;
}

static colors theme_colors(XdFolderView *v) {
    GtkStyleContext *sc = gtk_widget_get_style_context(GTK_WIDGET(v));
    colors c = {
        .base = lookup(sc, "theme_base_color", "#ffffff"),
        .text = lookup(sc, "theme_text_color", "#000000"),
        .sel_bg = lookup(sc, "theme_selected_bg_color", "#316ac5"),
        .sel_fg = lookup(sc, "theme_selected_fg_color", "#ffffff"),
        .header_bg = lookup(sc, "theme_bg_color", "#ece9d8"),
        .line = lookup(sc, "borders", "#aca899"),
    };
    c.dim = c.text;
    c.dim.alpha = 0.6;
    return c;
}

static void set_color(cairo_t *cr, GdkRGBA c) { gdk_cairo_set_source_rgba(cr, &c); }

/* File names wrap without inserted hyphens, as in Windows. */
static void no_hyphens(PangoLayout *l) {
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));
    pango_layout_set_attributes(l, attrs);
    pango_attr_list_unref(attrs);
}

static void draw_text(XdFolderView *v, cairo_t *cr, const char *text, int x, int y, int w,
                      int lines, PangoAlignment align, GdkRGBA color) {
    PangoLayout *l = gtk_widget_create_pango_layout(GTK_WIDGET(v), text);
    pango_layout_set_width(l, MAX(w, 1) * PANGO_SCALE);
    pango_layout_set_alignment(l, align);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    no_hyphens(l);
    if (lines > 1) {
        pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(l, -lines);
    }
    set_color(cr, color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

/* Draws a label with its selection highlight; returns nothing. */
static void draw_label_box(XdFolderView *v, cairo_t *cr, const char *text, int x, int y, int w,
                           int lines, PangoAlignment align, gboolean selected, const colors *c,
                           gboolean focused) {
    PangoLayout *l = gtk_widget_create_pango_layout(GTK_WIDGET(v), text);
    pango_layout_set_width(l, MAX(w, 1) * PANGO_SCALE);
    pango_layout_set_alignment(l, align);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    no_hyphens(l);
    if (v->underline_label) {
        PangoAttrList *attrs = pango_layout_get_attributes(l);
        attrs = attrs != nullptr ? pango_attr_list_copy(attrs) : pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    if (lines > 1) {
        pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(l, -lines);
    }
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, &ink, &logical);
    int bx = x + logical.x - 2;
    int bw = logical.width + 4;
    if (selected) {
        set_color(cr, c->sel_bg);
        cairo_rectangle(cr, bx, y, bw, logical.height);
        cairo_fill(cr);
    }
    if (focused) {
        static const double dash[] = {1, 1};
        set_color(cr, selected ? c->sel_fg : c->text);
        cairo_set_line_width(cr, 1);
        cairo_set_dash(cr, dash, 2, 0);
        cairo_rectangle(cr, bx + 0.5, y + 0.5, bw - 1, logical.height - 1);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
    }
    set_color(cr, selected ? c->sel_fg : c->text);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

static void paint_pixbuf(cairo_t *cr, GdkPixbuf *pb, double x, double y, gboolean selected,
                         gboolean faded, const colors *c) {
    if (pb == nullptr)
        return;
    gdk_cairo_set_source_pixbuf(cr, pb, x, y);
    cairo_paint_with_alpha(cr, faded ? 0.5 : 1.0);
    if (selected) {
        /* Windows tints selected icons with the highlight colour. */
        GdkRGBA tint = c->sel_bg;
        tint.alpha = 0.4;
        cairo_save(cr);
        cairo_rectangle(cr, x, y, gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
        cairo_clip(cr);
        set_color(cr, tint);
        gdk_cairo_set_source_pixbuf(cr, pb, x, y);
        cairo_pattern_t *mask = cairo_get_source(cr);
        cairo_pattern_reference(mask);
        set_color(cr, tint);
        cairo_mask(cr, mask);
        cairo_pattern_destroy(mask);
        cairo_restore(cr);
    }
}

/* XP's sharing overlay: an open hand, palm up, in a blue cuff, holding
 * the bottom of an icon of `px` whose top left is (x, y). Drawn on a
 * 32-unit grid. */
static void draw_share_hand(cairo_t *cr, double x, double y, double px) {
    double u = px / 32.0;
    cairo_save(cr);
    cairo_translate(cr, round(x), round(y));
    cairo_scale(cr, u, u);
    cairo_set_line_width(cr, MAX(1.0, 1.0 / u));
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    /* The palm and fingers, reaching right under the icon. */
    cairo_move_to(cr, 6, 25);
    cairo_line_to(cr, 13, 24);
    cairo_curve_to(cr, 15, 21, 18, 20, 19.5, 21.5); /* the thumb */
    cairo_line_to(cr, 17, 24.5);
    cairo_line_to(cr, 25, 24.5);
    cairo_curve_to(cr, 27.5, 24.5, 27.5, 27, 25, 27);
    cairo_line_to(cr, 23, 27);
    cairo_curve_to(cr, 25, 27.5, 24.5, 30, 22, 30);
    cairo_line_to(cr, 6, 30.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.98, 0.82, 0.62);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.45, 0.25, 0.10);
    cairo_stroke(cr);
    /* The cuff. */
    cairo_rectangle(cr, 0.5, 23.5, 6, 8);
    cairo_set_source_rgb(cr, 0.16, 0.36, 0.80);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.05, 0.15, 0.45);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_item(XdFolderView *v, cairo_t *cr, int i, const colors *c, gboolean has_focus) {
    item *it = g_ptr_array_index(v->shown, (guint)i);
    GdkRectangle r = item_rect(v, i);
    /* The folder a drag would drop into shows as selected. */
    gboolean sel = it->selected || i == v->drop_index;
    gboolean faded = FALSE;
    if (v->cut != nullptr) {
        char *uri = g_file_get_uri(it->file);
        faded = g_hash_table_contains(v->cut, uri);
        g_free(uri);
    }
    gboolean foc = has_focus && i == v->focus;
    v->underline_label = v->single_click && i == v->hover;
    switch (v->mode) {
    case XD_VIEW_THUMBNAILS: {
        int box = v->thumb;
        int bx = r.x + (r.width - box) / 2;
        int by = r.y + 4;
        /* XP's thumbnail frame: a white square with a thin border. */
        set_color(cr, c->base);
        cairo_rectangle(cr, bx, by, box, box);
        cairo_fill(cr);
        set_color(cr, sel ? c->sel_bg : c->line);
        cairo_set_line_width(cr, sel ? 2 : 1);
        cairo_rectangle(cr, bx + 0.5, by + 0.5, box - 1, box - 1);
        cairo_stroke(cr);
        GdkPixbuf *pb = item_thumb(v, it, box - 8);
        if (pb == nullptr)
            pb = item_icon(v, it, MIN(box / 2, 64));
        if (pb != nullptr)
            paint_pixbuf(cr, pb, bx + (box - gdk_pixbuf_get_width(pb)) / 2.0,
                         by + (box - gdk_pixbuf_get_height(pb)) / 2.0, FALSE, faded, c);
        draw_label_box(v, cr, item_label(v, it), r.x + 2, by + box + 4, r.width - 4, 1,
                       PANGO_ALIGN_CENTER, sel, c, foc);
        break;
    }
    case XD_VIEW_ICONS: {
        int px = 32;
        GdkPixbuf *pb = item_icon(v, it, px);
        paint_pixbuf(cr, pb, r.x + (r.width - px) / 2.0, r.y + 4, sel, faded, c);
        if (it->shared)
            draw_share_hand(cr, r.x + (r.width - px) / 2.0, r.y + 4, px);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cr, r.x + (r.width - px) / 2.0, r.y + 4, px);
        draw_label_box(v, cr, item_label(v, it), r.x + 2, r.y + px + 8, r.width - 4, 2,
                       PANGO_ALIGN_CENTER, sel, c, foc);
        break;
    }
    case XD_VIEW_TILES: {
        int px = 48;
        GdkPixbuf *pb = item_icon(v, it, px);
        paint_pixbuf(cr, pb, r.x + 6, r.y + (r.height - px) / 2.0, sel, faded, c);
        if (it->shared)
            draw_share_hand(cr, r.x + 6, r.y + (r.height - px) / 2.0, px);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cr, r.x + 6, r.y + (r.height - px) / 2.0, px);
        int tx = r.x + px + 14;
        int tw = r.width - px - 22;
        draw_label_box(v, cr, item_label(v, it), tx, r.y + 8, tw, 1, PANGO_ALIGN_LEFT, sel, c,
                       foc);
        draw_text(v, cr, it->type_desc, tx, r.y + 26, tw, 1, PANGO_ALIGN_LEFT, c->dim);
        if (it->extra != nullptr)
            draw_text(v, cr, it->extra, tx, r.y + 42, tw, 1, PANGO_ALIGN_LEFT, c->dim);
        else if (!it->is_dir)
            draw_text(v, cr, it->size_text, tx, r.y + 42, tw, 1, PANGO_ALIGN_LEFT, c->dim);
        break;
    }
    case XD_VIEW_LIST: {
        GdkPixbuf *pb = item_icon(v, it, LIST_ICON);
        paint_pixbuf(cr, pb, r.x + 1, r.y + 1, sel, faded, c);
        if (it->shared)
            draw_share_hand(cr, r.x + 1, r.y + 1, LIST_ICON);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cr, r.x + 1, r.y + 1, LIST_ICON);
        draw_label_box(v, cr, item_label(v, it), r.x + LIST_ICON + 5, r.y + 1,
                       r.width - LIST_ICON - 6, 1, PANGO_ALIGN_LEFT, sel, c, foc);
        break;
    }
    case XD_VIEW_DETAILS: {
        GdkPixbuf *pb = item_icon(v, it, LIST_ICON);
        int x = r.x;
        for (int k = 0; k < v->ncols; k++) {
            int col = v->order[k];
            int cw = v->col_w[col];
            if (cw == 0)
                continue; /* not here */
            if (col == XD_SORT_NAME) {
                cairo_save(cr);
                cairo_rectangle(cr, x, r.y, cw, r.height);
                cairo_clip(cr);
                paint_pixbuf(cr, pb, x + 4, r.y + 1, sel, faded, c);
                if (it->shared)
                    draw_share_hand(cr, x + 4, r.y + 1, LIST_ICON);
                if (it->shortcut)
                    xd_draw_shortcut_arrow(cr, x + 4, r.y + 1, LIST_ICON);
                draw_label_box(v, cr, item_label(v, it), x + LIST_ICON + 8, r.y + 1,
                               cw - LIST_ICON - 12, 1, PANGO_ALIGN_LEFT, sel, c, foc);
                cairo_restore(cr);
            } else {
                draw_text(v, cr, cell_text(it, col), x + 6, r.y + 1, cw - 10, 1,
                          col == XD_SORT_SIZE ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT, c->text);
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
static void draw_heading(XdFolderView *v, cairo_t *cr, const heading *hd, int w, const colors *c) {
    PangoLayout *l = gtk_widget_create_pango_layout(GTK_WIDGET(v), hd->text);
    PangoFontDescription *fd = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(l)));
    pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(l, &tw, &th);
    set_color(cr, c->sel_bg);
    cairo_move_to(cr, PAD + 4, hd->y + GROUP_H - 6 - th);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    double ly = hd->y + GROUP_H - 3.5;
    cairo_pattern_t *p = cairo_pattern_create_linear(PAD, 0, PAD + MIN(w - 2 * PAD, 500), 0);
    cairo_pattern_add_color_stop_rgba(p, 0, c->sel_bg.red, c->sel_bg.green, c->sel_bg.blue, 1);
    cairo_pattern_add_color_stop_rgba(p, 1, c->sel_bg.red, c->sel_bg.green, c->sel_bg.blue, 0);
    cairo_set_source(cr, p);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, PAD, ly);
    cairo_line_to(cr, PAD + MIN(w - 2 * PAD, 500), ly);
    cairo_stroke(cr);
    cairo_pattern_destroy(p);
}

/* One column's heading at x (widget coordinates). */
static void draw_header_cell(XdFolderView *v, cairo_t *cr, int col, double x, const colors *c) {
    int cw = v->col_w[col];
    cairo_save(cr);
    cairo_rectangle(cr, x, 0, cw, HEADER_H);
    cairo_clip(cr);
    gboolean sorted = (int)v->sort == col;
    draw_text(v, cr, column_title(v, col), (int)x + 6, 3, cw - (sorted ? 24 : 12), 1,
              col == XD_SORT_SIZE ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT, c->text);
    set_color(cr, c->line);
    cairo_rectangle(cr, x + cw - 1, 3, 1, HEADER_H - 6);
    cairo_fill(cr);
    if (sorted && cw > 20) {
        /* Sort arrow: up for ascending, down for descending. */
        double ax = x + cw - 14;
        double ay = HEADER_H / 2.0;
        cairo_move_to(cr, ax, v->desc ? ay - 2 : ay + 2);
        cairo_line_to(cr, ax + 8, v->desc ? ay - 2 : ay + 2);
        cairo_line_to(cr, ax + 4, v->desc ? ay + 2 : ay - 2);
        cairo_close_path(cr);
        set_color(cr, c->dim);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static int header_drop_slot(XdFolderView *v);

static void draw_header(XdFolderView *v, cairo_t *cr, const colors *c) {
    int w = widget_w(v);
    set_color(cr, c->header_bg);
    cairo_rectangle(cr, 0, 0, w, HEADER_H);
    cairo_fill(cr);
    set_color(cr, c->line);
    cairo_rectangle(cr, 0, HEADER_H - 1, w, 1);
    cairo_fill(cr);
    double x = -hval(v);
    for (int k = 0; k < v->ncols; k++) {
        int col = v->order[k];
        if (v->col_w[col] == 0)
            continue; /* not shown here */
        draw_header_cell(v, cr, col, x, c);
        x += v->col_w[col];
    }
    if (v->hdr_dragging && v->hdr_press >= 0) {
        /* The heading being moved follows the pointer, see-through, and
         * a line marks where it would go, as in XP. */
        double cx = column_x(v, v->hdr_press) - hval(v) + (v->hdr_x - v->hdr_press_x);
        cairo_push_group(cr);
        set_color(cr, c->header_bg);
        cairo_rectangle(cr, cx, 0, v->col_w[v->hdr_press], HEADER_H);
        cairo_fill(cr);
        draw_header_cell(v, cr, v->hdr_press, cx, c);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.6);
        int slot = header_drop_slot(v);
        double mx = -hval(v);
        for (int k = 0; k < slot && k < v->ncols; k++)
            mx += v->col_w[v->order[k]];
        set_color(cr, c->sel_bg);
        cairo_rectangle(cr, mx - 1, 0, 2, HEADER_H);
        cairo_fill(cr);
    }
}

static gboolean folder_view_draw(GtkWidget *widget, cairo_t *cr) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    colors c = theme_colors(v);
    int w = widget_w(v);
    int h = widget_h(v);
    set_color(cr, c.base);
    cairo_paint(cr);

    gboolean has_focus = gtk_widget_has_focus(widget);
    double ox = hval(v);
    double oy = vval(v);
    int n = (int)v->shown->len;

    /* Visible index range. */
    int first = 0;
    int last = n - 1;
    switch (v->mode) {
    case XD_VIEW_LIST:
        first = MAX(0, ifloor((ox - PAD) / v->cell_w)) * v->rows;
        last = MIN(n - 1, (ifloor((ox + w) / v->cell_w) + 1) * v->rows);
        break;
    case XD_VIEW_DETAILS:
        if (v->grouped)
            break; /* all, culled below */
        first = MAX(0, ifloor(oy / ROW_H));
        last = MIN(n - 1, ifloor((oy + h) / ROW_H) + 1);
        break;
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES:
        if (uses_rects(v))
            break; /* all, culled below */
        first = MAX(0, ifloor((oy - PAD) / v->cell_h)) * v->cols;
        last = MIN(n - 1, (ifloor((oy + h) / v->cell_h) + 1) * v->cols + v->cols - 1);
        break;
    case XD_VIEW_COUNT:
        break;
    }

    cairo_save(cr);
    if (v->mode == XD_VIEW_DETAILS) {
        cairo_rectangle(cr, 0, HEADER_H, w, h - HEADER_H);
        cairo_clip(cr);
    }
    cairo_translate(cr, -ox, -oy);
    if (v->grouped) {
        for (guint k = 0; k < v->headings->len; k++) {
            const heading *hd = &g_array_index(v->headings, heading, k);
            if (hd->y + GROUP_H >= oy && hd->y <= oy + h)
                draw_heading(v, cr, hd, w, &c);
        }
    }
    for (int i = first; i <= last; i++) {
        if (uses_rects(v)) {
            GdkRectangle r = item_rect(v, i);
            if (r.y + r.height < oy || r.y > oy + h || r.x + r.width < ox || r.x > ox + w)
                continue;
        }
        draw_item(v, cr, i, &c, has_focus);
    }
    cairo_restore(cr);

    if (v->banding) {
        /* XP's translucent selection rectangle. */
        GdkRectangle b = band_rect(v);
        GdkRGBA fill = c.sel_bg;
        fill.alpha = 0.25;
        gdk_cairo_set_source_rgba(cr, &fill);
        cairo_rectangle(cr, b.x - ox, b.y - oy, b.width, b.height);
        cairo_fill(cr);
        set_color(cr, c.sel_bg);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, b.x - ox + 0.5, b.y - oy + 0.5, MAX(b.width - 1, 0),
                        MAX(b.height - 1, 0));
        cairo_stroke(cr);
    }
    if (v->mode == XD_VIEW_DETAILS)
        draw_header(v, cr, &c);
    if (n == 0 && v->dir != nullptr && v->cancel == nullptr)
        draw_text(v, cr, "This folder is empty.", 0, (v->mode == XD_VIEW_DETAILS ? HEADER_H : 0) + 12,
                  w, 1, PANGO_ALIGN_CENTER, c.dim);
    return FALSE;
}

/* ---- loading ------------------------------------------------------------------------ */

static item *item_at(XdFolderView *v, int i);
static void set_hover(XdFolderView *v, int i);
static gboolean reload_timeout(gpointer data);

/* Selects the kept files that are here; returns how many. */
static guint apply_keep(XdFolderView *v) {
    if (v->keep == nullptr)
        return 0;
    guint found = 0;
    for (guint k = 0; k < v->shown->len; k++) {
        item *it = g_ptr_array_index(v->shown, k);
        char *uri = g_file_get_uri(it->file);
        it->selected = g_hash_table_contains(v->keep, uri);
        if (it->selected) {
            found++;
            if (v->keep_focus == nullptr || strcmp(uri, v->keep_focus) == 0 || found == 1) {
                v->focus = (int)k;
                v->anchor = (int)k;
            }
        }
        g_free(uri);
    }
    return found;
}

static void forget_keep(XdFolderView *v) {
    g_clear_pointer(&v->keep, g_hash_table_unref);
    g_clear_pointer(&v->keep_focus, g_free);
    v->keep_sticky = FALSE;
}

/* XP's hand under shared folders: Samba's user shares, listed in the
 * background (the hands appear once they're known). */
static void mark_shared(XdFolderView *v) {
    GHashTable *paths = v->place == XD_PLACE_FOLDER && v->dir != nullptr && g_file_is_native(v->dir)
                            ? xd_usershare_shared_paths()
                            : nullptr;
    gboolean any = paths != nullptr && g_hash_table_size(paths) > 0;
    for (guint i = 0; i < v->all->len; i++) {
        item *it = g_ptr_array_index(v->all, i);
        it->shared = FALSE;
        if (any && it->is_dir) {
            char *path = g_file_get_path(it->file);
            it->shared = path != nullptr && g_hash_table_contains(paths, path);
            g_free(path);
        }
    }
}

static void shares_changed(gpointer data) {
    XdFolderView *v = data;
    mark_shared(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

static void loading_done(XdFolderView *v) {
    g_clear_object(&v->cancel);
    /* The selection survives a refresh, as it is now (unless files are
     * already waiting to be selected). */
    if (v->keep == nullptr && v->shown->len > 0) {
        v->keep = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
        for (guint k = 0; k < v->shown->len; k++) {
            item *it = g_ptr_array_index(v->shown, k);
            if (it->selected)
                g_hash_table_add(v->keep, g_file_get_uri(it->file));
        }
        item *f = item_at(v, v->focus);
        v->keep_focus = f != nullptr ? g_file_get_uri(f->file) : nullptr;
    }
    /* The new items replace the old: indices into `shown` start over
     * (the selection and focus come back from `keep`). */
    g_ptr_array_set_size(v->shown, 0);
    g_ptr_array_unref(v->all);
    v->all = v->loading;
    v->loading = g_ptr_array_new_with_free_func(item_free);
    mark_shared(v);
    v->focus = -1;
    v->anchor = -1;
    v->pressed = -1;
    v->drop_index = -1;
    v->drag_pending = FALSE;
    set_hover(v, -1);
    refilter(v);
    if (v->keep != nullptr) {
        guint found = apply_keep(v);
        if (!v->keep_sticky || found >= g_hash_table_size(v->keep))
            forget_keep(v);
        if (found > 0)
            ensure_visible(v, v->focus);
    }
    g_signal_emit(v, signals[SIG_CONTENTS], 0);
    g_signal_emit(v, signals[SIG_SELECTION], 0);
    if (v->reload_again && v->reload_source == 0) {
        v->reload_again = FALSE;
        v->reload_source = g_timeout_add(300, reload_timeout, v);
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
static void load_drives(XdFolderView *v) {
    GPtrArray *drives = xd_computer_drives();
    for (guint i = 0; i < drives->len; i++) {
        XdDrive *d = g_ptr_array_index(drives, i);
        item *it = XD_NEW(item);
        it->file = g_object_ref(d->root);
        it->name = g_strdup(d->name);
        it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
        it->type_desc = g_strdup(d->kind);
        it->is_dir = TRUE;
        it->icon = d->icon != nullptr ? g_object_ref(d->icon) : nullptr;
        it->order = (int)i;
        it->size = d->total;
        it->size_text = d->total > 0 ? xd_format_drive_size(d->total) : g_strdup("");
        it->date_text = d->total > 0 ? xd_format_drive_size(d->free) : g_strdup("");
        it->mtime = (gint64)d->free;
        if (d->total > 0) {
            char *free_text = xd_format_drive_size(d->free);
            char *total_text = xd_format_drive_size(d->total);
            it->extra = g_strdup_printf("%s free of %s", free_text, total_text);
            g_free(free_text);
            g_free(total_text);
        }
        g_ptr_array_add(v->loading, it);
    }
    g_ptr_array_unref(drives);
}

/* Control Panel: categories (folders that open their page) or applets,
 * with their comments in the Type column. */
static void load_control(XdFolderView *v) {
    GPtrArray *items = xd_control_items(v->dir);
    for (guint i = 0; i < items->len; i++) {
        XdControlItem *c = g_ptr_array_index(items, i);
        item *it = XD_NEW(item);
        it->file = g_object_ref(c->file);
        it->name = g_strdup(c->name);
        it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
        it->type_desc = g_strdup(c->comment);
        it->size_text = g_strdup("");
        it->is_dir = c->category;
        it->icon = g_object_ref(c->icon);
        it->order = (int)i;
        g_ptr_array_add(v->loading, it);
    }
    g_ptr_array_unref(items);
}

/* My Network Places: its items, with their network location in the date
 * column (and Tiles' third line). */
static void load_network(XdFolderView *v) {
    GPtrArray *items = xd_network_items(v->dir);
    for (guint i = 0; i < items->len; i++) {
        XdNetworkItem *n = g_ptr_array_index(items, i);
        item *it = XD_NEW(item);
        it->file = g_object_ref(n->file);
        it->name = g_strdup(n->name);
        it->collate_key = g_utf8_collate_key_for_filename(it->name, -1);
        it->type_desc = g_strdup(n->comment);
        it->size_text = g_strdup("");
        it->date_text = g_strdup(n->location);
        if (n->location[0] != '\0')
            it->extra = g_strdup(n->location);
        it->is_dir = n->folder;
        it->icon = g_object_ref(n->icon);
        it->order = (int)i;
        g_ptr_array_add(v->loading, it);
    }
    g_ptr_array_unref(items);
}

static void next_files_ready(GObject *src, GAsyncResult *res, gpointer data);
static void enumerate_ready(GObject *src, GAsyncResult *res, gpointer data);

/* The Recycle Bin lists each trash in turn: true if another one started. */
static gboolean list_next_trash(XdFolderView *v) {
    if (v->more_listed == nullptr || v->more_listed->len == 0)
        return FALSE;
    GFile *dir = g_object_ref(g_ptr_array_index(v->more_listed, 0));
    g_ptr_array_remove_index(v->more_listed, 0);
    g_file_enumerate_children_async(dir, ATTRIBUTES, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                    v->cancel, enumerate_ready, v);
    g_object_unref(dir);
    return TRUE;
}

static void request_more(GFileEnumerator *e, XdFolderView *v) {
    g_file_enumerator_next_files_async(e, BATCH, G_PRIORITY_DEFAULT, v->cancel, next_files_ready,
                                       v);
}

static void next_files_ready(GObject *src, GAsyncResult *res, gpointer data) {
    GFileEnumerator *e = G_FILE_ENUMERATOR(src);
    GError *err = nullptr;
    GList *infos = g_file_enumerator_next_files_finish(e, res, &err);
    if (err != nullptr) {
        /* Cancelled means the view moved on or is gone: don't touch it. */
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            XdFolderView *v = data;
            g_signal_emit(v, signals[SIG_ERROR], 0, err->message);
            loading_done(v);
        }
        g_error_free(err);
        g_object_unref(e);
        return;
    }
    XdFolderView *v = data;
    if (infos == nullptr) {
        g_object_unref(e);
        if (!list_next_trash(v))
            loading_done(v);
        return;
    }
    GFile *dir = g_file_enumerator_get_container(e);
    for (GList *l = infos; l != nullptr; l = l->next)
        g_ptr_array_add(v->loading, v->place == XD_PLACE_TRASH ? trash_item(dir, l->data)
                                                               : item_new(dir, l->data));
    g_list_free_full(infos, g_object_unref);
    request_more(e, v);
}

static void enumerate_ready(GObject *src, GAsyncResult *res, gpointer data) {
    GError *err = nullptr;
    GFileEnumerator *e = g_file_enumerate_children_finish(G_FILE(src), res, &err);
    if (e == nullptr) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            XdFolderView *v = data;
            /* Another drive's trash that went away: list the rest. */
            if (v->place == XD_PLACE_TRASH && !g_file_equal(G_FILE(src), v->listed)) {
                if (!list_next_trash(v))
                    loading_done(v);
                g_error_free(err);
                return;
            }
            g_signal_emit(v, signals[SIG_ERROR], 0, err->message);
            loading_done(v);
        }
        g_error_free(err);
        return;
    }
    request_more(e, data);
}

/* Lists the folder: a new one from empty (`fresh`), or the same one again,
 * still showing the old list until the new one is in. */
static void start_loading(XdFolderView *v, gboolean fresh) {
    if (v->cancel != nullptr)
        g_cancellable_cancel(v->cancel);
    g_clear_object(&v->cancel);
    g_ptr_array_set_size(v->loading, 0);
    v->reload_again = FALSE;
    if (fresh) {
        g_ptr_array_set_size(v->shown, 0);
        g_ptr_array_set_size(v->all, 0);
        v->focus = -1;
        v->anchor = -1;
        v->pressed = -1;
        v->drop_index = -1;
        v->drag_pending = FALSE;
        set_hover(v, -1);
    }
    v->cancel = g_cancellable_new();
    if (v->place == XD_PLACE_COMPUTER || v->place == XD_PLACE_CONTROL ||
        v->place == XD_PLACE_NETWORK) {
        if (v->place == XD_PLACE_COMPUTER)
            load_drives(v);
        else if (v->place == XD_PLACE_NETWORK)
            load_network(v);
        else
            load_control(v);
        loading_done(v);
        return;
    }
    g_clear_pointer(&v->more_listed, g_ptr_array_unref);
    if (v->place == XD_PLACE_TRASH) {
        v->more_listed = xd_trash_dirs();
        g_ptr_array_remove_index(v->more_listed, 0); /* the home one is `listed` */
    }
    g_file_enumerate_children_async(v->listed, ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
                                    G_PRIORITY_DEFAULT, v->cancel, enumerate_ready, v);
    if (fresh) {
        layout_contents(v);
        gtk_widget_queue_draw(GTK_WIDGET(v));
    }
}

static gboolean reload_timeout(gpointer data) {
    XdFolderView *v = data;
    v->reload_source = 0;
    /* Still listing (a big folder that keeps changing): restarting would
     * never let it finish. List it again once this one is in. */
    if (v->cancel != nullptr)
        v->reload_again = TRUE;
    else
        xd_folder_view_reload(v);
    return G_SOURCE_REMOVE;
}

static void drives_changed([[maybe_unused]] GVolumeMonitor *m, [[maybe_unused]] gpointer mount,
                           gpointer data) {
    XdFolderView *v = data;
    if ((v->place == XD_PLACE_COMPUTER || v->place == XD_PLACE_NETWORK) && v->reload_source == 0)
        v->reload_source = g_timeout_add(300, reload_timeout, v);
}

/* A drive renamed (its label), mounted or checked: UDisks2 says so
 * before GIO does, if GIO ever does. */
static void udisks_changed([[maybe_unused]] GDBusObjectManagerClient *m,
                           [[maybe_unused]] GDBusObjectProxy *o,
                           [[maybe_unused]] GDBusProxy *iface,
                           [[maybe_unused]] GVariant *changed,
                           [[maybe_unused]] const char *const *invalidated, gpointer data) {
    drives_changed(nullptr, nullptr, data);
}

/* A volume appearing or going: an encrypted drive's cleartext when it's
 * unlocked or locked. */
static void udisks_object_changed([[maybe_unused]] GDBusObjectManager *m,
                                  [[maybe_unused]] GDBusObject *o, gpointer data) {
    drives_changed(nullptr, nullptr, data);
}

static void apps_changed([[maybe_unused]] GAppInfoMonitor *m, gpointer data) {
    XdFolderView *v = data;
    if (v->place == XD_PLACE_CONTROL && v->reload_source == 0)
        v->reload_source = g_timeout_add(300, reload_timeout, v);
}

static void monitor_changed([[maybe_unused]] GFileMonitor *m, [[maybe_unused]] GFile *file,
                            [[maybe_unused]] GFile *other, [[maybe_unused]] GFileMonitorEvent ev,
                            gpointer data) {
    XdFolderView *v = data;
    /* Coalesce bursts (a copy creates many events) into one reload. */
    if (v->reload_source == 0)
        v->reload_source = g_timeout_add(300, reload_timeout, v);
}

/* ---- selection and input ------------------------------------------------------------ */

static item *item_at(XdFolderView *v, int i) {
    return i >= 0 && i < (int)v->shown->len ? g_ptr_array_index(v->shown, (guint)i) : nullptr;
}

static void select_only(XdFolderView *v, int i) {
    for (guint k = 0; k < v->shown->len; k++)
        ((item *)g_ptr_array_index(v->shown, k))->selected = (int)k == i;
}

static void select_range(XdFolderView *v, int a, int b) {
    int lo = MIN(a, b);
    int hi = MAX(a, b);
    for (guint k = 0; k < v->shown->len; k++)
        ((item *)g_ptr_array_index(v->shown, k))->selected = (int)k >= lo && (int)k <= hi;
}

static void selection_changed(XdFolderView *v) {
    gtk_widget_queue_draw(GTK_WIDGET(v));
    g_signal_emit(v, signals[SIG_SELECTION], 0);
}

static void activate(XdFolderView *v, int i) {
    item *it = item_at(v, i);
    if (it != nullptr)
        g_signal_emit(v, signals[SIG_ACTIVATED], 0, it->file, it->is_dir);
}

/* ---- the Details header ------------------------------------------------------------ */

/* The column under widget x in the header (-1: none), and whether x is on
 * its right-hand divider, which resizes it. */
static int header_column_at(XdFolderView *v, double wx, gboolean *divider) {
    double x = wx + hval(v);
    *divider = FALSE;
    int left = 0;
    for (int k = 0; k < v->ncols; k++) {
        int cw = v->col_w[v->order[k]];
        if (cw == 0)
            continue;
        if (fabs(x - (left + cw)) <= DIVIDER_HIT) {
            *divider = TRUE;
            return v->order[k];
        }
        left += cw;
    }
    left = 0;
    for (int k = 0; k < v->ncols; k++) {
        int cw = v->col_w[v->order[k]];
        if (x >= left && x < left + cw)
            return v->order[k];
        left += cw;
    }
    return -1;
}

/* Where a heading being dragged would go: before the column whose middle
 * is past the pointer (an index into `order`). */
static int header_drop_slot(XdFolderView *v) {
    double x = v->hdr_x + hval(v);
    int left = 0;
    for (int k = 0; k < v->ncols; k++) {
        int cw = v->col_w[v->order[k]];
        if (cw > 0 && x < left + cw / 2.0)
            return k;
        left += cw;
    }
    return v->ncols;
}

static void columns_changed(XdFolderView *v) {
    layout_contents(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

/* Name fills the window until a column is sized by hand; from then on it
 * keeps its width, as the others do. */
static void freeze_name_width(XdFolderView *v) {
    if (v->width[XD_SORT_NAME] == 0)
        v->width[XD_SORT_NAME] = MAX(v->col_w[XD_SORT_NAME], COL_MIN);
}

/* Double-clicking a divider fits the column to its widest entry. */
static void fit_column(XdFolderView *v, int col) {
    int widest = 0;
    for (guint k = 0; k < v->shown->len; k++) {
        item *it = g_ptr_array_index(v->shown, k);
        if (col == XD_SORT_NAME) {
            if (it->name_w == 0)
                it->name_w = text_width(v, item_label(v, it));
            widest = MAX(widest, it->name_w + LIST_ICON + 20);
        } else {
            widest = MAX(widest, text_width(v, cell_text(it, col)) + 16);
        }
    }
    if (widest == 0)
        widest = text_width(v, column_title(v, col)) + 24; /* nothing listed: the heading */
    freeze_name_width(v);
    v->width[col] = MAX(widest, COL_MIN);
    columns_changed(v);
}

/* A heading dropped at `slot`: its column moves there. */
static void move_column(XdFolderView *v, int col, int slot) {
    int from = -1;
    for (int k = 0; k < v->ncols; k++)
        if (v->order[k] == col)
            from = k;
    if (from < 0 || slot == from || slot == from + 1)
        return;
    memmove(&v->order[from], &v->order[from + 1], sizeof(int) * (size_t)(v->ncols - from - 1));
    if (slot > from)
        slot--;
    memmove(&v->order[slot + 1], &v->order[slot], sizeof(int) * (size_t)(v->ncols - 1 - slot));
    v->order[slot] = col;
    columns_changed(v);
}

static void on_column_toggled(GtkCheckMenuItem *mi, gpointer data) {
    XdFolderView *v = data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "xd-column"));
    int at = -1;
    for (int k = 0; k < v->ncols; k++)
        if (v->order[k] == col)
            at = k;
    if (gtk_check_menu_item_get_active(mi) && at < 0) {
        v->order[v->ncols++] = col; /* a column added goes on the right */
    } else if (!gtk_check_menu_item_get_active(mi) && at >= 0) {
        memmove(&v->order[at], &v->order[at + 1], sizeof(int) * (size_t)(v->ncols - at - 1));
        v->ncols--;
    }
    columns_changed(v);
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
static void header_menu(XdFolderView *v, GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();
    for (int col = 0; col < XD_SORT_COUNT; col++) {
        if (!column_available(v, col))
            continue;
        GtkWidget *mi = gtk_check_menu_item_new_with_label(column_title(v, col));
        gboolean shown = FALSE;
        for (int k = 0; k < v->ncols; k++)
            shown = shown || v->order[k] == col;
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), shown);
        gtk_widget_set_sensitive(mi, col != XD_SORT_NAME);
        g_object_set_data(G_OBJECT(mi), "xd-column", GINT_TO_POINTER(col));
        g_signal_connect(mi, "toggled", G_CALLBACK(on_column_toggled), v);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(v), nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* The resize pointer over a divider. */
static void set_header_cursor(XdFolderView *v, gboolean resize) {
    if (resize == v->hdr_cursor)
        return;
    v->hdr_cursor = resize;
    GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(v));
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
static gboolean header_press(XdFolderView *v, GdkEventButton *ev) {
    gboolean divider = FALSE;
    int col = header_column_at(v, ev->x, &divider);
    if (ev->button == GDK_BUTTON_SECONDARY) {
        if (ev->type == GDK_BUTTON_PRESS)
            header_menu(v, ev);
        return TRUE;
    }
    if (ev->button != GDK_BUTTON_PRIMARY)
        return TRUE;
    if (ev->type == GDK_2BUTTON_PRESS) {
        v->hdr_resize = -1;
        v->hdr_press = -1;
        if (divider && col >= 0)
            fit_column(v, col);
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS || col < 0)
        return TRUE;
    if (divider) {
        freeze_name_width(v);
        v->hdr_resize = col;
        v->hdr_resize_w = v->col_w[col];
    } else {
        v->hdr_press = col;
        v->hdr_dragging = FALSE;
    }
    v->hdr_press_x = v->hdr_x = ev->x;
    return TRUE;
}

static gboolean header_motion(XdFolderView *v, GdkEventMotion *ev) {
    gboolean held = (ev->state & GDK_BUTTON1_MASK) != 0;
    if (v->hdr_resize >= 0 && held) {
        v->width[v->hdr_resize] = MAX(COL_MIN, v->hdr_resize_w + (int)(ev->x - v->hdr_press_x));
        columns_changed(v);
        return TRUE;
    }
    if (v->hdr_press >= 0 && held) {
        v->hdr_x = ev->x;
        if (!v->hdr_dragging &&
            gtk_drag_check_threshold(GTK_WIDGET(v), (int)v->hdr_press_x, 0, (int)ev->x, 0))
            v->hdr_dragging = TRUE;
        if (v->hdr_dragging)
            gtk_widget_queue_draw(GTK_WIDGET(v));
        return TRUE;
    }
    gboolean divider = FALSE;
    if (v->mode == XD_VIEW_DETAILS && ev->y >= 0 && ev->y < HEADER_H && !held)
        header_column_at(v, ev->x, &divider);
    set_header_cursor(v, divider);
    return FALSE;
}

static gboolean header_release(XdFolderView *v, GdkEventButton *ev) {
    if (v->hdr_resize >= 0) {
        v->hdr_resize = -1;
        return TRUE;
    }
    if (v->hdr_press < 0)
        return FALSE;
    int col = v->hdr_press;
    v->hdr_press = -1;
    if (v->hdr_dragging) {
        v->hdr_dragging = FALSE;
        v->hdr_x = ev->x;
        int slot = header_drop_slot(v);
        move_column(v, col, slot);
        gtk_widget_queue_draw(GTK_WIDGET(v));
        return TRUE;
    }
    /* A click sorts; clicking the sorted column reverses it. */
    gboolean divider = FALSE;
    if (ev->y >= 0 && ev->y < HEADER_H && header_column_at(v, ev->x, &divider) == col)
        xd_folder_view_set_sort(v, (XdSortColumn)col, (int)v->sort == col ? !v->desc : FALSE);
    return TRUE;
}

static gboolean folder_view_button_press(GtkWidget *widget, GdkEventButton *ev) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    gtk_widget_grab_focus(widget);
    forget_keep(v);
    if (v->mode == XD_VIEW_DETAILS && ev->y < HEADER_H)
        return header_press(v, ev);
    int i = index_at(v, ev->x, ev->y);
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
        /* With single-click, the first click's release opened it. */
        if (!v->single_click)
            activate(v, i);
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    gboolean ctrl = ev->state & GDK_CONTROL_MASK;
    gboolean shift = ev->state & GDK_SHIFT_MASK;
    v->pressed = ev->button == GDK_BUTTON_PRIMARY && !ctrl && !shift ? i : -1;
    item *it = item_at(v, i);
    if (it == nullptr && ev->button == GDK_BUTTON_PRIMARY) {
        /* A drag from empty space selects what it covers. */
        if (!ctrl)
            select_only(v, -1);
        v->banding = TRUE;
        v->band_x0 = v->band_x1 = ev->x + hval(v);
        v->band_y0 = v->band_y1 = ev->y + vval(v);
        v->band_toggle = ctrl;
        if (v->band_base == nullptr)
            v->band_base = g_array_new(FALSE, FALSE, sizeof(gboolean));
        g_array_set_size(v->band_base, v->shown->len);
        for (guint k = 0; k < v->shown->len; k++)
            g_array_index(v->band_base, gboolean, k) =
                ((item *)g_ptr_array_index(v->shown, k))->selected;
    } else if (it == nullptr) {
        if (!ctrl)
            select_only(v, -1);
    } else if (ctrl && ev->button == GDK_BUTTON_PRIMARY) {
        it->selected = !it->selected;
        v->anchor = i;
    } else if (shift && v->anchor >= 0) {
        select_range(v, v->anchor, i);
    } else if (!(ev->button == GDK_BUTTON_SECONDARY && it->selected)) {
        select_only(v, i);
        v->anchor = i;
    }
    if (i >= 0)
        v->focus = i;
    v->drag_pending = it != nullptr && ev->button == GDK_BUTTON_PRIMARY && !ctrl && !shift;
    v->drag_x = ev->x;
    v->drag_y = ev->y;
    v->drag_cx = ev->x + hval(v);
    v->drag_cy = ev->y + vval(v);
    v->drag_button = ev->button;
    selection_changed(v);
    if (ev->button == GDK_BUTTON_SECONDARY)
        g_signal_emit(v, signals[SIG_CONTEXT], 0, (GdkEvent *)ev);
    return TRUE;
}

/* ---- dragging files out ---------------------------------------------------------------- */

static GdkRectangle band_rect(const XdFolderView *v) {
    double x = MIN(v->band_x0, v->band_x1), y = MIN(v->band_y0, v->band_y1);
    return (GdkRectangle){ifloor(x), ifloor(y), ifloor(fabs(v->band_x1 - v->band_x0)),
                          ifloor(fabs(v->band_y1 - v->band_y0))};
}

/* Items touching the band are selected (toggled, with Ctrl). */
static void band_update(XdFolderView *v) {
    GdkRectangle band = band_rect(v);
    for (guint k = 0; k < v->shown->len; k++) {
        item *it = g_ptr_array_index(v->shown, k);
        GdkRectangle r = item_rect(v, (int)k);
        gboolean hit = gdk_rectangle_intersect(&band, &r, nullptr);
        gboolean base = k < v->band_base->len && g_array_index(v->band_base, gboolean, k);
        it->selected = v->band_toggle ? base != hit : hit;
    }
    selection_changed(v);
}

static gboolean folder_view_button_release(GtkWidget *widget, GdkEventButton *ev) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    if (ev->button == GDK_BUTTON_PRIMARY && header_release(v, ev))
        return TRUE;
    /* Single-click opens on release, unless the press became a drag. */
    gboolean click = v->drag_pending && ev->button == GDK_BUTTON_PRIMARY;
    v->drag_pending = FALSE;
    if (v->banding) {
        v->banding = FALSE;
        gtk_widget_queue_draw(widget);
    }
    int pressed = v->pressed;
    v->pressed = -1;
    if (v->single_click && click && pressed >= 0 && index_at(v, ev->x, ev->y) == pressed)
        activate(v, pressed);
    return FALSE;
}

/* Redraws item i only (an index past the end draws nothing). */
static void queue_draw_item(XdFolderView *v, int i) {
    if (i < 0)
        return;
    GdkRectangle r = item_rect(v, i);
    gtk_widget_queue_draw_area(GTK_WIDGET(v), r.x - ifloor(hval(v)), r.y - ifloor(vval(v)),
                               r.width, r.height);
}

/* Single-click: underline the item under the pointer, with a hand. */
static void set_hover(XdFolderView *v, int i) {
    if (i == v->hover)
        return;
    /* Only the two labels change, not the whole folder. */
    queue_draw_item(v, v->hover);
    queue_draw_item(v, i);
    v->hover = i;
    GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(v));
    if (win == nullptr)
        return;
    GdkCursor *hand = i >= 0 ? gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer")
                             : nullptr;
    gdk_window_set_cursor(win, hand);
    g_clear_object(&hand);
}

static gboolean folder_view_leave(GtkWidget *widget, [[maybe_unused]] GdkEventCrossing *ev) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    if (v->hdr_resize < 0)
        set_header_cursor(v, FALSE);
    set_hover(v, -1);
    return FALSE;
}

static gboolean folder_view_motion(GtkWidget *widget, GdkEventMotion *ev) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    if (header_motion(v, ev))
        return TRUE;
    if (v->single_click)
        set_hover(v, ev->state & GDK_BUTTON1_MASK ? -1 : index_at(v, ev->x, ev->y));
    if (v->banding && (ev->state & GDK_BUTTON1_MASK)) {
        /* Past an edge, the view scrolls along. */
        if (v->vadj != nullptr && (ev->y < 0 || ev->y > widget_h(v)))
            gtk_adjustment_set_value(v->vadj, vval(v) + (ev->y < 0 ? ev->y : ev->y - widget_h(v)));
        if (v->hadj != nullptr && (ev->x < 0 || ev->x > widget_w(v)))
            gtk_adjustment_set_value(v->hadj, hval(v) + (ev->x < 0 ? ev->x : ev->x - widget_w(v)));
        v->band_x1 = ev->x + hval(v);
        v->band_y1 = ev->y + vval(v);
        band_update(v);
        return TRUE;
    }
    if (!v->drag_pending || !(ev->state & GDK_BUTTON1_MASK))
        return FALSE;
    if (!gtk_drag_check_threshold(widget, (int)v->drag_x, (int)v->drag_y, (int)ev->x,
                                  (int)ev->y))
        return TRUE;
    v->drag_pending = FALSE;
    /* Control Panel's applets can be copied out (a shortcut to them), not
     * moved; its categories aren't files at all. */
    GdkDragAction actions = (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
    /* Network places and drives are connections, not files to copy. */
    if (v->place == XD_PLACE_NETWORK)
        return TRUE;
    if (v->place == XD_PLACE_CONTROL) {
        actions = (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_LINK);
        for (guint k = 0; k < v->shown->len; k++) {
            item *sel = g_ptr_array_index(v->shown, k);
            if (sel->selected && sel->is_dir)
                return TRUE;
        }
    }
    GtkTargetList *targets = gtk_target_list_new(nullptr, 0);
    gtk_target_list_add_uri_targets(targets, 0);
    gtk_drag_begin_with_coordinates(widget, targets, actions, (int)v->drag_button,
                                    (GdkEvent *)ev, (int)v->drag_x, (int)v->drag_y);
    gtk_target_list_unref(targets);
    return TRUE;
}

static void folder_view_drag_begin(GtkWidget *widget, GdkDragContext *context) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    guint n = xd_folder_view_get_selected_count(v);
    gtk_drag_set_icon_name(context, n > 1 ? "edit-copy" : "text-x-generic", 0, 0);
}

static void folder_view_drag_data_get(GtkWidget *widget, [[maybe_unused]] GdkDragContext *c,
                                      GtkSelectionData *data, [[maybe_unused]] guint info,
                                      [[maybe_unused]] guint time) {
    GList *files = xd_folder_view_get_selected_files(XD_FOLDER_VIEW(widget));
    guint n = g_list_length(files);
    char **uris = g_new0(char *, n + 1);
    guint k = 0;
    for (GList *l = files; l != nullptr; l = l->next)
        uris[k++] = g_file_get_uri(l->data);
    gtk_selection_data_set_uris(data, uris);
    g_strfreev(uris);
    g_list_free_full(files, g_object_unref);
}

/* ---- dropping files in ------------------------------------------------------------------ */

static void forget_drag(XdFolderView *v) {
    g_clear_pointer(&v->drag_uris, g_strfreev);
    g_clear_pointer(&v->over_event, gdk_event_free);
    v->drag_requested = FALSE;
    if (v->drop_index != -1) {
        v->drop_index = -1;
        gtk_widget_queue_draw(GTK_WIDGET(v));
    }
}

static char *filesystem_id(GFile *f) {
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_ID_FILESYSTEM,
                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    char *id = info != nullptr
                   ? g_strdup(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM))
                   : nullptr;
    g_clear_object(&info);
    return id;
}

/* The folder a drop at (x, y) goes into: a folder item under the pointer
 * (not one being dragged), else this folder. Sets drop_index. */
static GFile *drop_folder(XdFolderView *v, GdkDragContext *ctx, int x, int y) {
    int i = index_at(v, x, y);
    item *it = item_at(v, i);
    gboolean own = gtk_drag_get_source_widget(ctx) == GTK_WIDGET(v);
    int target = it != nullptr && it->is_dir && !(own && it->selected) &&
                         v->place != XD_PLACE_TRASH
                     ? i
                     : -1;
    if (target != v->drop_index) {
        v->drop_index = target;
        gtk_widget_queue_draw(GTK_WIDGET(v));
    }
    return target >= 0 ? it->file : v->dir;
}

/* Windows' rules: Ctrl copies, Shift moves, Ctrl+Shift or Alt makes
 * shortcuts; otherwise files move within a drive and copy between drives.
 * 0 when the drop would do nothing (onto itself, or into the folder the
 * files are already in). */
static constexpr GdkDragAction NO_ACTION = (GdkDragAction)0;

/* Our own icons dropped back into this folder with Auto Arrange off: they
 * move to where they're dropped, rather than nothing happening. */
static gboolean can_reposition(XdFolderView *v, GdkDragContext *ctx, GFile *dest) {
    return v->placed && gtk_drag_get_source_widget(ctx) == GTK_WIDGET(v) && v->dir != nullptr &&
           dest != nullptr && g_file_equal(dest, v->dir);
}

static GdkDragAction drop_action(XdFolderView *v, GdkDragContext *ctx, char **uris, GFile *dest) {
    if (dest == nullptr || uris == nullptr || uris[0] == nullptr ||
        xd_place_of(dest) == XD_PLACE_COMPUTER || xd_place_of(dest) == XD_PLACE_CONTROL ||
        xd_place_of(dest) == XD_PLACE_NETWORK)
        return NO_ACTION;
    if (xd_place_of(dest) == XD_PLACE_TRASH) {
        /* Into the Recycle Bin: a delete, carried as a move. */
        if (gtk_drag_get_source_widget(ctx) == GTK_WIDGET(v) && v->place == XD_PLACE_TRASH)
            return NO_ACTION;
        return (gdk_drag_context_get_actions(ctx) & GDK_ACTION_MOVE) ? GDK_ACTION_MOVE
                                                                      : GDK_ACTION_COPY;
    }
    /* The keyboard's modifiers as they are now (the keymap follows them
     * even while the pointer stands still), else the pointer's. */
    GdkModifierType mods = (GdkModifierType)0;
    if (v->keymap != nullptr) {
        guint state = gdk_keymap_get_modifier_state(v->keymap);
        mods = (GdkModifierType)state;
    } else {
        GdkDevice *dev = gdk_drag_context_get_device(ctx);
        GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(v));
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
        char *a = filesystem_id(first);
        char *b = filesystem_id(dest);
        action = a != nullptr && g_strcmp0(a, b) == 0 ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
        g_free(a);
        g_free(b);
        g_object_unref(first);
    }
    gboolean all_here = TRUE;
    for (char **u = uris; *u != nullptr; u++) {
        GFile *f = g_file_new_for_uri(*u);
        GFile *parent = g_file_get_parent(f);
        gboolean into_itself = g_file_equal(f, dest) || g_file_has_prefix(dest, f);
        all_here = all_here && parent != nullptr && g_file_equal(parent, dest);
        g_clear_object(&parent);
        g_object_unref(f);
        if (into_itself)
            return NO_ACTION;
    }
    if (all_here && action == GDK_ACTION_MOVE)
        return can_reposition(v, ctx, dest) ? GDK_ACTION_MOVE : NO_ACTION;
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
    XdFolderView *v = data;
    if (v->over_event == nullptr || v->drag_uris == nullptr || v->dropping)
        return;
    GdkEvent *again = gdk_event_copy(v->over_event);
    gdk_event_put(again);
    gdk_event_free(again);
}

static gboolean folder_view_drag_motion(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y,
                                        guint time) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    if (v->keymap == nullptr) {
        v->keymap = gdk_keymap_get_for_display(gtk_widget_get_display(widget));
        v->keymap_id = g_signal_connect(v->keymap, "state-changed", G_CALLBACK(keymap_changed), v);
    }
    GdkEvent *motion = gtk_get_current_event();
    if (motion != nullptr && motion->type == GDK_DRAG_MOTION) {
        g_clear_pointer(&v->over_event, gdk_event_free);
        v->over_event = motion;
    } else if (motion != nullptr) {
        gdk_event_free(motion);
    }
    GdkAtom target = gtk_drag_dest_find_target(widget, ctx, nullptr);
    if (target == GDK_NONE || v->dir == nullptr) {
        gdk_drag_status(ctx, NO_ACTION, time);
        return TRUE;
    }
    GFile *dest = drop_folder(v, ctx, x, y);
    if (v->drag_uris == nullptr) {
        /* The action depends on the files: ask for them first. */
        if (!v->drag_requested) {
            v->drag_requested = TRUE;
            gtk_drag_get_data(widget, ctx, target, time);
        }
        gdk_drag_status(ctx, NO_ACTION, time);
        return TRUE;
    }
    gdk_drag_status(ctx, protocol_action(ctx, drop_action(v, ctx, v->drag_uris, dest)), time);
    return TRUE;
}

static void folder_view_drag_leave(GtkWidget *widget, [[maybe_unused]] GdkDragContext *ctx,
                                   [[maybe_unused]] guint time) {
    forget_drag(XD_FOLDER_VIEW(widget));
}

static gboolean folder_view_drag_drop(GtkWidget *widget, GdkDragContext *ctx,
                                      [[maybe_unused]] gint x, [[maybe_unused]] gint y,
                                      guint time) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    GdkAtom target = gtk_drag_dest_find_target(widget, ctx, nullptr);
    if (target == GDK_NONE)
        return FALSE;
    v->dropping = TRUE;
    gtk_drag_get_data(widget, ctx, target, time);
    return TRUE;
}

/* Moves the selected icons by as far as the pointer went from where the
 * drag began (onto the nearest free cells with Align to Grid). */
static void reposition(XdFolderView *v, int x, int y) {
    int dx = ifloor(x + hval(v) - v->drag_cx);
    int dy = ifloor(y + vval(v) - v->drag_cy);
    GHashTable *taken = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < v->shown->len && i < v->rects->len; i++)
        if (!((item *)g_ptr_array_index(v->shown, i))->selected)
            take_cells(v, taken, g_array_index(v->rects, GdkRectangle, i));
    int cols = grid_cols(v);
    for (guint i = 0; i < v->shown->len && i < v->rects->len; i++) {
        item *it = g_ptr_array_index(v->shown, i);
        if (!it->selected || it->key == nullptr)
            continue;
        GdkRectangle r = g_array_index(v->rects, GdkRectangle, i);
        int nx = MAX(r.x + dx, 0), ny = MAX(r.y + dy, 0);
        if (v->align_grid) {
            int col = CLAMP((int)lround((double)(nx - PAD) / v->cell_w), 0, cols - 1);
            int row = MAX((int)lround((double)(ny - PAD) / v->cell_h), 0);
            int cell = free_cell(v, taken, row * cols + col);
            g_hash_table_add(taken, cell_key(cell / cols, cell % cols));
            nx = PAD + (cell % cols) * v->cell_w;
            ny = PAD + (cell / cols) * v->cell_h;
        }
        set_position(v, it->key, nx, ny);
    }
    g_hash_table_unref(taken);
    layout_contents(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

static void folder_view_drag_data_received(GtkWidget *widget, GdkDragContext *ctx, gint x,
                                           gint y, GtkSelectionData *sel,
                                           [[maybe_unused]] guint info, guint time) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    char **uris = gtk_selection_data_get_uris(sel);
    GFile *dest = drop_folder(v, ctx, x, y);
    GdkDragAction action = drop_action(v, ctx, uris, dest);
    if (!v->dropping) {
        g_strfreev(v->drag_uris);
        v->drag_uris = uris != nullptr ? uris : g_new0(char *, 1);
        gdk_drag_status(ctx, protocol_action(ctx, action), time);
        return;
    }
    v->dropping = FALSE;
    if (action == GDK_ACTION_MOVE && can_reposition(v, ctx, dest)) {
        forget_drag(v);
        gtk_drag_finish(ctx, TRUE, FALSE, time);
        reposition(v, x, y);
        g_strfreev(uris);
        return;
    }
    GFile *target = g_object_ref(dest);
    forget_drag(v);
    /* We do the move ourselves: the source mustn't delete anything. */
    gtk_drag_finish(ctx, action != NO_ACTION, FALSE, time);
    if (action != NO_ACTION)
        g_signal_emit(v, signals[SIG_DROPPED], 0, uris, target, action);
    g_object_unref(target);
    g_strfreev(uris);
}

/* Arrow keys among icons put anywhere: the nearest one that way. */
static int spatial_target(XdFolderView *v, guint key) {
    GdkRectangle f = item_rect(v, v->focus);
    double fx = f.x + f.width / 2.0, fy = f.y + f.height / 2.0;
    int best = -1;
    double best_score = 0;
    for (int i = 0; i < (int)v->shown->len; i++) {
        if (i == v->focus)
            continue;
        GdkRectangle r = item_rect(v, i);
        double cx = r.x + r.width / 2.0, cy = r.y + r.height / 2.0;
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

static int move_target(XdFolderView *v, guint key) {
    int n = (int)v->shown->len;
    if (v->placed && v->focus >= 0 &&
        (key == GDK_KEY_Up || key == GDK_KEY_Down || key == GDK_KEY_Left || key == GDK_KEY_Right ||
         key == GDK_KEY_Page_Up || key == GDK_KEY_Page_Down))
        return spatial_target(v, key);
    int f = MAX(v->focus, 0);
    int step_down = v->mode == XD_VIEW_LIST || v->mode == XD_VIEW_DETAILS ? 1 : v->cols;
    int step_right = v->mode == XD_VIEW_LIST ? v->rows : v->mode == XD_VIEW_DETAILS ? 0 : 1;
    int page = v->mode == XD_VIEW_LIST ? v->rows
               : v->mode == XD_VIEW_DETAILS
                   ? MAX((widget_h(v) - HEADER_H) / ROW_H - 1, 1)
                   : MAX(widget_h(v) / MAX(v->cell_h, 1), 1) * v->cols;
    /* Nothing focused yet: any arrow starts at the first item, as in
     * Windows (not one step past it). */
    if (v->focus < 0 && (key == GDK_KEY_Up || key == GDK_KEY_Down || key == GDK_KEY_Left ||
                         key == GDK_KEY_Right || key == GDK_KEY_Page_Up ||
                         key == GDK_KEY_Page_Down))
        return n > 0 ? 0 : -1;
    switch (key) {
    case GDK_KEY_Up:
        return v->focus < 0 ? 0 : f - step_down;
    case GDK_KEY_Down:
        return v->focus < 0 ? 0 : f + step_down;
    case GDK_KEY_Left:
        return step_right == 0 ? -1 : f - step_right;
    case GDK_KEY_Right:
        return step_right == 0 ? -1 : f + step_right;
    case GDK_KEY_Home:
        return 0;
    case GDK_KEY_End:
        return n - 1;
    case GDK_KEY_Page_Up:
        return f - page;
    case GDK_KEY_Page_Down:
        return f + page;
    default:
        return -2;
    }
}

static gboolean folder_view_key_press(GtkWidget *widget, GdkEventKey *ev) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    forget_keep(v);
    int n = (int)v->shown->len;
    gboolean ctrl = ev->state & GDK_CONTROL_MASK;
    gboolean shift = ev->state & GDK_SHIFT_MASK;
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        activate(v, v->focus);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Menu || (shift && ev->keyval == GDK_KEY_F10)) {
        g_signal_emit(v, signals[SIG_CONTEXT], 0, (GdkEvent *)nullptr);
        return TRUE;
    }
    if (ctrl && (ev->keyval == GDK_KEY_a || ev->keyval == GDK_KEY_A)) {
        xd_folder_view_select_all(v);
        return TRUE;
    }
    item *focused = item_at(v, v->focus);
    if (ctrl && ev->keyval == GDK_KEY_space && focused != nullptr) {
        focused->selected = !focused->selected;
        selection_changed(v);
        return TRUE;
    }
    int t = move_target(v, ev->keyval);
    if (t == -2 || n == 0)
        return FALSE;
    if (t < 0 && t != -1)
        t = 0;
    if (t == -1)
        return TRUE;
    t = CLAMP(t, 0, n - 1);
    v->focus = t;
    if (shift && v->anchor >= 0) {
        select_range(v, v->anchor, t);
    } else if (!ctrl) {
        select_only(v, t);
        v->anchor = t;
    }
    ensure_visible(v, t);
    selection_changed(v);
    return TRUE;
}

/* ---- GtkScrollable ------------------------------------------------------------ */

static void adj_changed([[maybe_unused]] GtkAdjustment *a, gpointer data) {
    gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void set_adjustment(XdFolderView *v, GtkAdjustment **slot, gulong *id, GtkAdjustment *a) {
    if (*slot != nullptr) {
        g_signal_handler_disconnect(*slot, *id);
        g_object_unref(*slot);
    }
    if (a == nullptr)
        a = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    *slot = g_object_ref_sink(a);
    *id = g_signal_connect(a, "value-changed", G_CALLBACK(adj_changed), v);
    layout_contents(v);
}

static void folder_view_set_property(GObject *obj, guint id, const GValue *value,
                                     GParamSpec *pspec) {
    XdFolderView *v = XD_FOLDER_VIEW(obj);
    switch (id) {
    case PROP_HADJ:
        set_adjustment(v, &v->hadj, &v->hadj_id, g_value_get_object(value));
        break;
    case PROP_VADJ:
        set_adjustment(v, &v->vadj, &v->vadj_id, g_value_get_object(value));
        break;
    case PROP_HPOLICY: {
        gint policy = g_value_get_enum(value);
        v->hpolicy = (GtkScrollablePolicy)policy;
        break;
    }
    case PROP_VPOLICY: {
        gint policy = g_value_get_enum(value);
        v->vpolicy = (GtkScrollablePolicy)policy;
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void folder_view_get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec) {
    XdFolderView *v = XD_FOLDER_VIEW(obj);
    switch (id) {
    case PROP_HADJ:
        g_value_set_object(value, v->hadj);
        break;
    case PROP_VADJ:
        g_value_set_object(value, v->vadj);
        break;
    case PROP_HPOLICY:
        g_value_set_enum(value, (gint)v->hpolicy);
        break;
    case PROP_VPOLICY:
        g_value_set_enum(value, (gint)v->vpolicy);
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
    XdFolderView *v = XD_FOLDER_VIEW(obj);
    if (v->theme_id != 0) {
        g_signal_handler_disconnect(gtk_icon_theme_get_default(), v->theme_id);
        v->theme_id = 0;
    }
    if (v->cancel != nullptr)
        g_cancellable_cancel(v->cancel);
    g_clear_object(&v->cancel);
    g_clear_pointer(&v->thumbs, xd_thumb_queue_free);
    if (v->reload_source != 0) {
        g_source_remove(v->reload_source);
        v->reload_source = 0;
    }
    if (v->monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(v->monitor, v);
        g_file_monitor_cancel(v->monitor);
        g_clear_object(&v->monitor);
    }
    if (v->hadj != nullptr) {
        g_signal_handler_disconnect(v->hadj, v->hadj_id);
        g_clear_object(&v->hadj);
    }
    if (v->vadj != nullptr) {
        g_signal_handler_disconnect(v->vadj, v->vadj_id);
        g_clear_object(&v->vadj);
    }
    g_clear_object(&v->dir);
    g_clear_object(&v->listed);
    if (v->watching_drives) {
        g_signal_handlers_disconnect_by_data(xd_computer_monitor(), v);
        if (xd_udisks_manager() != nullptr)
            g_signal_handlers_disconnect_by_data(xd_udisks_manager(), v);
        v->watching_drives = FALSE;
    }
    if (v->apps_monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(v->apps_monitor, v);
        g_clear_object(&v->apps_monitor);
    }
    forget_keep(v);
    forget_drag(v);
    if (v->keymap != nullptr) {
        g_signal_handler_disconnect(v->keymap, v->keymap_id);
        v->keymap = nullptr;
    }
    g_clear_pointer(&v->cut, g_hash_table_unref);
    g_clear_pointer(&v->band_base, g_array_unref);
    xd_usershare_unwatch(v);
    G_OBJECT_CLASS(xd_folder_view_parent_class)->dispose(obj);
}

static void folder_view_finalize(GObject *obj) {
    XdFolderView *v = XD_FOLDER_VIEW(obj);
    g_ptr_array_unref(v->shown);
    g_ptr_array_unref(v->all);
    g_ptr_array_unref(v->loading);
    g_clear_pointer(&v->more_listed, g_ptr_array_unref);
    clear_headings(v);
    g_array_unref(v->headings);
    g_array_unref(v->rects);
    g_hash_table_unref(v->positions);
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
}

static void xd_folder_view_init(XdFolderView *v) {
    v->all = g_ptr_array_new_with_free_func(item_free);
    v->loading = g_ptr_array_new_with_free_func(item_free);
    v->shown = g_ptr_array_new();
    v->rects = g_array_new(FALSE, TRUE, sizeof(GdkRectangle));
    v->headings = g_array_new(FALSE, TRUE, sizeof(heading));
    v->mode = XD_VIEW_ICONS;
    v->thumb = XD_THUMB_DEFAULT;
    v->focus = -1;
    v->anchor = -1;
    v->drop_index = -1;
    v->hover = -1;
    v->pressed = -1;
    v->hdr_resize = -1;
    v->hdr_press = -1;
    v->auto_arrange = TRUE;
    v->align_grid = TRUE;
    v->positions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    v->theme_id = g_signal_connect(gtk_icon_theme_get_default(), "changed",
                                   G_CALLBACK(icon_theme_changed), v);
    xd_usershare_watch(shares_changed, v);
    default_columns(v);
    gtk_widget_set_can_focus(GTK_WIDGET(v), TRUE);
    gtk_drag_dest_set(GTK_WIDGET(v), (GtkDestDefaults)0, nullptr, 0,
                      (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
    gtk_drag_dest_add_uri_targets(GTK_WIDGET(v));
    gtk_widget_add_events(GTK_WIDGET(v), GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                             GDK_BUTTON1_MOTION_MASK | GDK_KEY_PRESS_MASK |
                                             GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
}

/* ---- public API ------------------------------------------------------------------ */

GtkWidget *xd_folder_view_new(void) { return g_object_new(XD_TYPE_FOLDER_VIEW, nullptr); }

void xd_folder_view_set_location(XdFolderView *v, GFile *dir) {
    g_return_if_fail(G_IS_FILE(dir));
    g_set_object(&v->dir, dir);
    forget_keep(v);
    if (v->thumbs != nullptr)
        xd_thumb_queue_cancel(v->thumbs);
    /* Icons' places are the folder's own: given just before, or none. */
    if (!v->positions_given)
        g_hash_table_remove_all(v->positions);
    v->positions_given = FALSE;
    if (v->monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(v->monitor, v);
        g_file_monitor_cancel(v->monitor);
        g_clear_object(&v->monitor);
    }
    v->place = xd_place_of(dir);
    g_clear_object(&v->listed);
    v->listed = v->place == XD_PLACE_TRASH ? xd_trash_files_dir() : g_object_ref(dir);
    if (v->place == XD_PLACE_COMPUTER || v->place == XD_PLACE_NETWORK) {
        /* Drives mapped or disconnected, places added (by any window). */
        char *config = xd_net_config_path();
        GFile *f = g_file_new_for_path(config);
        v->monitor = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
        if (v->monitor != nullptr)
            g_signal_connect(v->monitor, "changed", G_CALLBACK(monitor_changed), v);
        g_object_unref(f);
        g_free(config);
    }
    if (v->place == XD_PLACE_COMPUTER || v->place == XD_PLACE_NETWORK) {
        if (!v->watching_drives) {
            GVolumeMonitor *vm = xd_computer_monitor();
            g_signal_connect(vm, "mount-added", G_CALLBACK(drives_changed), v);
            g_signal_connect(vm, "mount-removed", G_CALLBACK(drives_changed), v);
            g_signal_connect(vm, "mount-changed", G_CALLBACK(drives_changed), v);
            GDBusObjectManager *udisks = xd_udisks_manager();
            if (udisks != nullptr) {
                g_signal_connect(udisks, "interface-proxy-properties-changed",
                                 G_CALLBACK(udisks_changed), v);
                g_signal_connect(udisks, "object-added", G_CALLBACK(udisks_object_changed), v);
                g_signal_connect(udisks, "object-removed", G_CALLBACK(udisks_object_changed), v);
            }
            v->watching_drives = TRUE;
        }
    } else if (v->place == XD_PLACE_CONTROL) {
        if (v->apps_monitor == nullptr) {
            /* GIO notices applications being installed or removed. */
            v->apps_monitor = g_app_info_monitor_get();
            g_signal_connect(v->apps_monitor, "changed", G_CALLBACK(apps_changed), v);
        }
    } else {
        v->monitor = g_file_monitor_directory(v->listed, G_FILE_MONITOR_WATCH_MOVES, nullptr,
                                              nullptr);
        if (v->monitor != nullptr)
            g_signal_connect(v->monitor, "changed", G_CALLBACK(monitor_changed), v);
    }
    if (v->hadj != nullptr)
        gtk_adjustment_set_value(v->hadj, 0);
    if (v->vadj != nullptr)
        gtk_adjustment_set_value(v->vadj, 0);
    start_loading(v, TRUE);
}

GFile *xd_folder_view_get_location(XdFolderView *v) { return v->dir; }

void xd_folder_view_reload(XdFolderView *v) {
    if (v->dir == nullptr)
        return;
    /* The old items stay until the new ones are in: no blank view, and
     * the scroll position is kept. */
    start_loading(v, FALSE);
}

void xd_folder_view_set_mode(XdFolderView *v, XdViewMode mode) {
    if (mode >= XD_VIEW_COUNT || mode == v->mode)
        return;
    v->mode = mode;
    if (v->hadj != nullptr)
        gtk_adjustment_set_value(v->hadj, 0);
    if (wants_groups(v) != v->grouped)
        refilter(v); /* in groups or out of them: another order */
    else
        layout_contents(v);
    ensure_visible(v, v->focus);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

XdViewMode xd_folder_view_get_mode(XdFolderView *v) { return v->mode; }

void xd_folder_view_set_thumbnail_size(XdFolderView *v, int px) {
    px = CLAMP(px, XD_THUMB_MIN, XD_THUMB_MAX);
    if (px == v->thumb)
        return;
    v->thumb = px;
    layout_contents(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

void xd_folder_view_set_sort(XdFolderView *v, XdSortColumn column, gboolean descending) {
    if (column >= XD_SORT_COUNT)
        return;
    v->sort = column;
    v->desc = descending;
    /* Arranging by a column puts icons in its order, once, when they
     * aren't arranged automatically. */
    if (!v->auto_arrange)
        g_hash_table_remove_all(v->positions);
    refilter(v);
}

XdSortColumn xd_folder_view_get_sort(XdFolderView *v) { return v->sort; }
gboolean xd_folder_view_get_sort_descending(XdFolderView *v) { return v->desc; }

void xd_folder_view_set_groups(XdFolderView *v, gboolean groups) {
    v->groups = groups;
    refilter(v);
}

gboolean xd_folder_view_get_groups(XdFolderView *v) { return v->groups; }

void xd_folder_view_set_show_hidden(XdFolderView *v, gboolean show) {
    v->show_hidden = show;
    refilter(v);
    g_signal_emit(v, signals[SIG_CONTENTS], 0);
}

void xd_folder_view_set_hide_extensions(XdFolderView *v, gboolean hide) {
    if (v->hide_extensions == hide)
        return;
    v->hide_extensions = hide;
    for (guint i = 0; i < v->all->len; i++)
        ((item *)g_ptr_array_index(v->all, i))->name_w = 0; /* List view measures names */
    layout_contents(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

void xd_folder_view_set_single_click(XdFolderView *v, gboolean single) {
    v->single_click = single;
    if (!single)
        set_hover(v, -1);
}

void xd_folder_view_select_all(XdFolderView *v) {
    for (guint k = 0; k < v->shown->len; k++)
        ((item *)g_ptr_array_index(v->shown, k))->selected = TRUE;
    selection_changed(v);
}

void xd_folder_view_invert_selection(XdFolderView *v) {
    for (guint k = 0; k < v->shown->len; k++) {
        item *it = g_ptr_array_index(v->shown, k);
        it->selected = !it->selected;
    }
    selection_changed(v);
}

guint xd_folder_view_get_item_count(XdFolderView *v) { return v->shown->len; }

GList *xd_folder_view_get_selected_files(XdFolderView *v) {
    GList *out = nullptr;
    for (guint i = v->shown->len; i > 0; i--) {
        item *it = g_ptr_array_index(v->shown, i - 1);
        if (it->selected)
            out = g_list_prepend(out, g_object_ref(it->file));
    }
    return out;
}

guint xd_folder_view_get_selected_count(XdFolderView *v) {
    guint n = 0;
    for (guint k = 0; k < v->shown->len; k++)
        n += ((item *)g_ptr_array_index(v->shown, k))->selected ? 1u : 0u;
    return n;
}

guint64 xd_folder_view_get_size(XdFolderView *v, gboolean selected_only) {
    guint64 total = 0;
    for (guint k = 0; k < v->shown->len; k++) {
        item *it = g_ptr_array_index(v->shown, k);
        if (!it->is_dir && (!selected_only || it->selected))
            total += it->size;
    }
    return total;
}

void xd_folder_view_select_files(XdFolderView *v, GList *files) {
    forget_keep(v);
    if (files == nullptr)
        return;
    v->keep = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (GList *l = files; l != nullptr; l = l->next)
        g_hash_table_add(v->keep, g_file_get_uri(l->data));
    v->keep_focus = g_file_get_uri(files->data);
    v->keep_sticky = TRUE;
    guint found = apply_keep(v);
    if (found > 0)
        ensure_visible(v, v->focus);
    /* What isn't here yet is selected when it appears. */
    if (found >= g_hash_table_size(v->keep))
        forget_keep(v);
    selection_changed(v);
}

gboolean xd_folder_view_get_name_area(XdFolderView *v, GFile *file, GdkRectangle *area) {
    int i = -1;
    for (guint k = 0; k < v->shown->len; k++) {
        if (g_file_equal(((item *)g_ptr_array_index(v->shown, k))->file, file)) {
            i = (int)k;
            break;
        }
    }
    if (i < 0)
        return FALSE;
    ensure_visible(v, i);
    GdkRectangle r = item_rect(v, i);
    GdkRectangle a = {r.x + 2, r.y, r.width - 4, ROW_H + 4};
    switch (v->mode) {
    case XD_VIEW_THUMBNAILS:
        a.y = r.y + v->thumb + 8;
        break;
    case XD_VIEW_ICONS:
        a.y = r.y + 32 + 8;
        break;
    case XD_VIEW_TILES:
        a = (GdkRectangle){r.x + 48 + 14, r.y + 6, r.width - 48 - 22, ROW_H + 4};
        break;
    case XD_VIEW_LIST:
        a = (GdkRectangle){r.x + LIST_ICON + 3, r.y - 2, MAX(r.width - LIST_ICON - 4, 160),
                           ROW_H + 4};
        break;
    case XD_VIEW_DETAILS:
        a = (GdkRectangle){r.x + column_x(v, XD_SORT_NAME) + LIST_ICON + 6, r.y - 2,
                           v->col_w[XD_SORT_NAME] - LIST_ICON - 10, ROW_H + 4};
        break;
    case XD_VIEW_COUNT:
        break;
    }
    a.x -= ifloor(hval(v));
    a.y -= ifloor(vval(v));
    *area = a;
    return TRUE;
}

void xd_folder_view_set_columns(XdFolderView *v, const char *spec) {
    if (!xd_columns_parse(spec, XD_SORT_COUNT, v->order, &v->ncols, v->width))
        default_columns(v);
    columns_changed(v);
}

char *xd_folder_view_get_columns(XdFolderView *v) {
    gboolean is_default = v->ncols == XD_SORT_MODIFIED + 1;
    for (int k = 0; is_default && k < v->ncols; k++)
        is_default = v->order[k] == k;
    for (int c = 0; is_default && c < XD_SORT_COUNT; c++)
        is_default = v->width[c] == 0;
    return is_default ? nullptr : xd_columns_format(v->order, v->ncols, v->width);
}

void xd_folder_view_set_auto_arrange(XdFolderView *v, gboolean on) {
    if (v->auto_arrange == on)
        return;
    v->auto_arrange = on;
    /* Turned off, the icons stay where they are now (laid out as they
     * were arranged); turned on, they're arranged again. */
    g_hash_table_remove_all(v->positions);
    v->positions_mode = v->mode;
    columns_changed(v);
}

gboolean xd_folder_view_get_auto_arrange(XdFolderView *v) { return v->auto_arrange; }

void xd_folder_view_set_align_to_grid(XdFolderView *v, gboolean on) {
    v->align_grid = on;
    if (on && v->placed) {
        snap_positions(v);
        columns_changed(v);
    }
}

gboolean xd_folder_view_get_align_to_grid(XdFolderView *v) { return v->align_grid; }

void xd_folder_view_set_positions(XdFolderView *v, XdViewMode mode, char **names, const int *xy,
                                  gsize count) {
    g_hash_table_remove_all(v->positions);
    v->positions_mode = mode;
    for (gsize i = 0; i < count; i++)
        set_position(v, names[i], xy[2 * i], xy[2 * i + 1]);
    v->positions_given = TRUE;
    columns_changed(v);
}

gsize xd_folder_view_get_positions(XdFolderView *v, XdViewMode *mode, char ***names, int **xy) {
    *mode = v->positions_mode;
    *names = g_new0(char *, v->all->len + 1);
    *xy = g_new0(int, 2 * (gsize)v->all->len + 1);
    gsize n = 0;
    for (guint i = 0; i < v->all->len && !v->auto_arrange; i++) {
        item *it = g_ptr_array_index(v->all, i);
        const int *p = it->key != nullptr ? g_hash_table_lookup(v->positions, it->key) : nullptr;
        if (p == nullptr)
            continue;
        (*names)[n] = g_strdup(it->key);
        (*xy)[2 * n] = p[0];
        (*xy)[2 * n + 1] = p[1];
        n++;
    }
    return n;
}

void xd_folder_view_set_cut_files(XdFolderView *v, GList *files) {
    g_clear_pointer(&v->cut, g_hash_table_unref);
    if (files != nullptr) {
        v->cut = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
        for (GList *l = files; l != nullptr; l = l->next)
            g_hash_table_add(v->cut, g_file_get_uri(l->data));
    }
    gtk_widget_queue_draw(GTK_WIDGET(v));
}
