/* SPDX-License-Identifier: MIT */
#include <gio/gdesktopappinfo.h>
#include <math.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "folderview.h"
#include "places.h"

#define ATTRIBUTES                                                             \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME  \
    "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE      \
    "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_ICON \
    "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP \
    "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK \
    "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_THUMBNAIL_PATH

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

typedef struct item {
    GFile *file;
    char *name;
    char *collate_key;
    char *type_desc;
    char *size_text;
    char *date_text;
    guint64 size;
    gint64 mtime;
    gboolean is_dir;
    gboolean hidden;
    GIcon *icon;
    char *thumb_path;
    GdkPixbuf *icon_pb;
    int icon_px;
    GdkPixbuf *thumb_pb;
    int thumb_px;
    int name_w; /* List view: measured label width, 0 = not yet */
    gboolean selected;
    gboolean shortcut; /* a symbolic link or a launcher: XP's arrow */
    int order;    /* My Computer: the drives' own order */
    char *extra;  /* Tiles' third line instead of the size (a drive's free space) */
} item;

struct _XdFolderView {
    GtkDrawingArea parent;
    GtkAdjustment *hadj, *vadj;
    gulong hadj_id, vadj_id;
    GtkScrollablePolicy hpolicy, vpolicy;

    GFile *dir;
    /* Special folders: My Computer lists drives; the Recycle Bin lists
     * the trash's files folder (`listed`) under their original names. */
    XdPlace place;
    GFile *listed;
    gboolean watching_drives;
    GCancellable *cancel;
    GFileMonitor *monitor;
    guint reload_source;
    GPtrArray *all;   /* item*, owned */
    GPtrArray *shown; /* item*, borrowed: filtered and sorted */

    XdViewMode mode;
    int thumb;
    XdSortColumn sort;
    gboolean desc;
    gboolean show_hidden;
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
    int col_w[XD_SORT_COUNT];
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

static void item_free(gpointer p) {
    item *it = p;
    g_clear_object(&it->file);
    g_clear_object(&it->icon);
    g_clear_object(&it->icon_pb);
    g_clear_object(&it->thumb_pb);
    g_free(it->name);
    g_free(it->collate_key);
    g_free(it->type_desc);
    g_free(it->size_text);
    g_free(it->date_text);
    g_free(it->thumb_path);
    g_free(it->extra);
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
    const char *ctype = g_file_info_get_content_type(info);
    it->type_desc = it->is_dir ? g_strdup("File Folder")
                    : ctype != nullptr ? g_content_type_get_description(ctype)
                                       : g_strdup("File");
    it->size_text = it->is_dir ? g_strdup("") : xd_format_size(it->size);
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
    return it;
}

static GdkPixbuf *item_icon(XdFolderView *v, item *it, int px) {
    if (it->icon_pb != nullptr && it->icon_px == px)
        return it->icon_pb;
    g_clear_object(&it->icon_pb);
    it->icon_px = px;
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GtkIconInfo *info = it->icon != nullptr
                            ? gtk_icon_theme_lookup_by_gicon(theme, it->icon, px,
                                                             GTK_ICON_LOOKUP_FORCE_SIZE)
                            : nullptr;
    if (info == nullptr)
        info = gtk_icon_theme_lookup_icon(theme, it->is_dir ? "folder" : "text-x-generic", px,
                                          GTK_ICON_LOOKUP_FORCE_SIZE);
    if (info != nullptr) {
        it->icon_pb = gtk_icon_info_load_icon(info, nullptr);
        g_object_unref(info);
    }
    (void)v;
    return it->icon_pb;
}

/* A cached thumbnail scaled to fit px x px, or nullptr. Only reads the
 * freedesktop cache; generating thumbnails is xd-thumbnailer's job. */
static GdkPixbuf *item_thumb(item *it, int px) {
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

static int compare_items(gconstpointer pa, gconstpointer pb, gpointer data) {
    const item *a = *(const item *const *)pa;
    const item *b = *(const item *const *)pb;
    const XdFolderView *v = data;
    if (v->place == XD_PLACE_COMPUTER)
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
    for (guint i = 0; i < v->all->len; i++) {
        item *it = g_ptr_array_index(v->all, i);
        if (v->show_hidden || !it->hidden)
            g_ptr_array_add(v->shown, it);
        else
            it->selected = FALSE;
    }
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

static void layout_contents(XdFolderView *v) {
    int w = widget_w(v);
    int h = widget_h(v);
    int n = (int)v->shown->len;
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
                it->name_w = text_width(v, it->name);
            widest = MAX(widest, it->name_w);
        }
        v->cell_w = MIN(widest + LIST_ICON + 16, LIST_COL_MAX);
        v->cell_h = ROW_H;
        v->content_w = PAD * 2 + v->cols * v->cell_w;
        v->content_h = h;
        break;
    }
    case XD_VIEW_DETAILS: {
        /* Windows' default column widths; Name takes the spare room. */
        v->col_w[XD_SORT_SIZE] = 80;
        v->col_w[XD_SORT_TYPE] = 150;
        v->col_w[XD_SORT_MODIFIED] = 130;
        int others = v->col_w[XD_SORT_SIZE] + v->col_w[XD_SORT_TYPE] + v->col_w[XD_SORT_MODIFIED];
        v->col_w[XD_SORT_NAME] = MAX(220, w - others);
        v->cell_w = v->col_w[XD_SORT_NAME] + others;
        v->cell_h = ROW_H;
        v->cols = 1;
        v->rows = n;
        v->content_w = v->cell_w;
        v->content_h = HEADER_H + n * ROW_H + PAD;
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
        return (GdkRectangle){0, HEADER_H + i * ROW_H, v->cell_w, ROW_H};
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES:
    case XD_VIEW_COUNT:
        break;
    }
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
        if (wy >= HEADER_H)
            i = ifloor((y - HEADER_H) / ROW_H);
        break;
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES: {
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
        GdkPixbuf *pb = item_thumb(it, box - 8);
        if (pb == nullptr)
            pb = item_icon(v, it, MIN(box / 2, 64));
        if (pb != nullptr)
            paint_pixbuf(cr, pb, bx + (box - gdk_pixbuf_get_width(pb)) / 2.0,
                         by + (box - gdk_pixbuf_get_height(pb)) / 2.0, FALSE, faded, c);
        draw_label_box(v, cr, it->name, r.x + 2, by + box + 4, r.width - 4, 1, PANGO_ALIGN_CENTER,
                       sel, c, foc);
        break;
    }
    case XD_VIEW_ICONS: {
        int px = 32;
        GdkPixbuf *pb = item_icon(v, it, px);
        paint_pixbuf(cr, pb, r.x + (r.width - px) / 2.0, r.y + 4, sel, faded, c);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cr, r.x + (r.width - px) / 2.0, r.y + 4, px);
        draw_label_box(v, cr, it->name, r.x + 2, r.y + px + 8, r.width - 4, 2, PANGO_ALIGN_CENTER,
                       sel, c, foc);
        break;
    }
    case XD_VIEW_TILES: {
        int px = 48;
        GdkPixbuf *pb = item_icon(v, it, px);
        paint_pixbuf(cr, pb, r.x + 6, r.y + (r.height - px) / 2.0, sel, faded, c);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cr, r.x + 6, r.y + (r.height - px) / 2.0, px);
        int tx = r.x + px + 14;
        int tw = r.width - px - 22;
        draw_label_box(v, cr, it->name, tx, r.y + 8, tw, 1, PANGO_ALIGN_LEFT, sel, c, foc);
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
        if (it->shortcut)
            xd_draw_shortcut_arrow(cr, r.x + 1, r.y + 1, LIST_ICON);
        draw_label_box(v, cr, it->name, r.x + LIST_ICON + 5, r.y + 1,
                       r.width - LIST_ICON - 6, 1, PANGO_ALIGN_LEFT, sel, c, foc);
        break;
    }
    case XD_VIEW_DETAILS: {
        GdkPixbuf *pb = item_icon(v, it, LIST_ICON);
        paint_pixbuf(cr, pb, r.x + 4, r.y + 1, sel, faded, c);
        if (it->shortcut)
            xd_draw_shortcut_arrow(cr, r.x + 4, r.y + 1, LIST_ICON);
        int x = r.x;
        draw_label_box(v, cr, it->name, x + LIST_ICON + 8, r.y + 1,
                       v->col_w[XD_SORT_NAME] - LIST_ICON - 12, 1, PANGO_ALIGN_LEFT, sel, c, foc);
        x += v->col_w[XD_SORT_NAME];
        draw_text(v, cr, it->size_text, x + 4, r.y + 1, v->col_w[XD_SORT_SIZE] - 10, 1,
                  PANGO_ALIGN_RIGHT, c->text);
        x += v->col_w[XD_SORT_SIZE];
        draw_text(v, cr, it->type_desc, x + 6, r.y + 1, v->col_w[XD_SORT_TYPE] - 10, 1,
                  PANGO_ALIGN_LEFT, c->text);
        x += v->col_w[XD_SORT_TYPE];
        draw_text(v, cr, it->date_text != nullptr ? it->date_text : "", x + 6, r.y + 1,
                  v->col_w[XD_SORT_MODIFIED] - 10, 1, PANGO_ALIGN_LEFT, c->text);
        break;
    }
    case XD_VIEW_COUNT:
        break;
    }
}

static void draw_header(XdFolderView *v, cairo_t *cr, const colors *c) {
    static const char *const folder_titles[XD_SORT_COUNT] = {"Name", "Size", "Type",
                                                             "Date Modified"};
    static const char *const computer_titles[XD_SORT_COUNT] = {"Name", "Total Size", "Type",
                                                               "Free Space"};
    static const char *const trash_titles[XD_SORT_COUNT] = {"Name", "Size", "Original Location",
                                                            "Date Deleted"};
    const char *const *titles = v->place == XD_PLACE_COMPUTER ? computer_titles
                                : v->place == XD_PLACE_TRASH  ? trash_titles
                                                              : folder_titles;
    int w = widget_w(v);
    set_color(cr, c->header_bg);
    cairo_rectangle(cr, 0, 0, w, HEADER_H);
    cairo_fill(cr);
    set_color(cr, c->line);
    cairo_rectangle(cr, 0, HEADER_H - 1, w, 1);
    cairo_fill(cr);
    double x = -hval(v);
    for (int col = 0; col < XD_SORT_COUNT; col++) {
        int cw = v->col_w[col];
        draw_text(v, cr, titles[col], (int)x + 6, 3, cw - 24, 1,
                  col == XD_SORT_SIZE ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT, c->text);
        set_color(cr, c->line);
        cairo_rectangle(cr, x + cw - 1, 3, 1, HEADER_H - 6);
        cairo_fill(cr);
        if ((int)v->sort == col) {
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
        x += cw;
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
        first = MAX(0, ifloor(oy / ROW_H));
        last = MIN(n - 1, ifloor((oy + h) / ROW_H) + 1);
        break;
    case XD_VIEW_THUMBNAILS:
    case XD_VIEW_ICONS:
    case XD_VIEW_TILES:
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
    for (int i = first; i <= last; i++)
        draw_item(v, cr, i, &c, has_focus);
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

static void loading_done(XdFolderView *v) {
    g_clear_object(&v->cancel);
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
        g_ptr_array_add(v->all, it);
    }
    g_ptr_array_unref(drives);
}

static void next_files_ready(GObject *src, GAsyncResult *res, gpointer data);

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
        loading_done(v);
        return;
    }
    GFile *dir = g_file_enumerator_get_container(e);
    for (GList *l = infos; l != nullptr; l = l->next)
        g_ptr_array_add(v->all, v->place == XD_PLACE_TRASH ? trash_item(dir, l->data)
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
            g_signal_emit(v, signals[SIG_ERROR], 0, err->message);
            loading_done(v);
        }
        g_error_free(err);
        return;
    }
    request_more(e, data);
}

static void start_loading(XdFolderView *v) {
    if (v->cancel != nullptr)
        g_cancellable_cancel(v->cancel);
    g_clear_object(&v->cancel);
    g_ptr_array_set_size(v->shown, 0);
    g_ptr_array_set_size(v->all, 0);
    v->focus = -1;
    v->anchor = -1;
    v->cancel = g_cancellable_new();
    if (v->place == XD_PLACE_COMPUTER) {
        load_drives(v);
        loading_done(v);
        return;
    }
    g_file_enumerate_children_async(v->listed, ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
                                    G_PRIORITY_DEFAULT, v->cancel, enumerate_ready, v);
    layout_contents(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

static gboolean reload_timeout(gpointer data) {
    XdFolderView *v = data;
    v->reload_source = 0;
    xd_folder_view_reload(v);
    return G_SOURCE_REMOVE;
}

static void drives_changed([[maybe_unused]] GVolumeMonitor *m, [[maybe_unused]] gpointer mount,
                           gpointer data) {
    XdFolderView *v = data;
    if (v->place == XD_PLACE_COMPUTER && v->reload_source == 0)
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

static gboolean folder_view_button_press(GtkWidget *widget, GdkEventButton *ev) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    gtk_widget_grab_focus(widget);
    forget_keep(v);
    if (v->mode == XD_VIEW_DETAILS && ev->y < HEADER_H && ev->type == GDK_BUTTON_PRESS) {
        /* Header click sorts; clicking the sorted column reverses it. */
        double x = ev->x + hval(v);
        for (int col = 0; col < XD_SORT_COUNT; col++) {
            if (x < v->col_w[col]) {
                xd_folder_view_set_sort(v, (XdSortColumn)col,
                                        (int)v->sort == col ? !v->desc : FALSE);
                return TRUE;
            }
            x -= v->col_w[col];
        }
        return TRUE;
    }
    int i = index_at(v, ev->x, ev->y);
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
        activate(v, i);
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    gboolean ctrl = ev->state & GDK_CONTROL_MASK;
    gboolean shift = ev->state & GDK_SHIFT_MASK;
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

static gboolean folder_view_button_release(GtkWidget *widget,
                                           [[maybe_unused]] GdkEventButton *ev) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
    v->drag_pending = FALSE;
    if (v->banding) {
        v->banding = FALSE;
        gtk_widget_queue_draw(widget);
    }
    return FALSE;
}

static gboolean folder_view_motion(GtkWidget *widget, GdkEventMotion *ev) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
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
    GtkTargetList *targets = gtk_target_list_new(nullptr, 0);
    gtk_target_list_add_uri_targets(targets, 0);
    gtk_drag_begin_with_coordinates(widget, targets,
                                    (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_MOVE |
                                                    GDK_ACTION_LINK),
                                    (int)v->drag_button, (GdkEvent *)ev, (int)v->drag_x,
                                    (int)v->drag_y);
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

static GdkDragAction drop_action(XdFolderView *v, GdkDragContext *ctx, char **uris, GFile *dest) {
    if (dest == nullptr || uris == nullptr || uris[0] == nullptr ||
        xd_place_of(dest) == XD_PLACE_COMPUTER)
        return NO_ACTION;
    if (xd_place_of(dest) == XD_PLACE_TRASH) {
        /* Into the Recycle Bin: a delete, carried as a move. */
        if (gtk_drag_get_source_widget(ctx) == GTK_WIDGET(v) && v->place == XD_PLACE_TRASH)
            return NO_ACTION;
        return (gdk_drag_context_get_actions(ctx) & GDK_ACTION_MOVE) ? GDK_ACTION_MOVE
                                                                      : GDK_ACTION_COPY;
    }
    GdkModifierType mods = (GdkModifierType)0;
    GdkDevice *dev = gdk_drag_context_get_device(ctx);
    GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(v));
    if (dev != nullptr && win != nullptr)
        gdk_window_get_device_position(win, dev, nullptr, nullptr, &mods);
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
        return NO_ACTION;
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

static gboolean folder_view_drag_motion(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y,
                                        guint time) {
    XdFolderView *v = XD_FOLDER_VIEW(widget);
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
    GFile *target = g_object_ref(dest);
    forget_drag(v);
    /* We do the move ourselves: the source mustn't delete anything. */
    gtk_drag_finish(ctx, action != NO_ACTION, FALSE, time);
    if (action != NO_ACTION)
        g_signal_emit(v, signals[SIG_DROPPED], 0, uris, target, action);
    g_object_unref(target);
    g_strfreev(uris);
}

static int move_target(XdFolderView *v, guint key) {
    int n = (int)v->shown->len;
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
    if (v->cancel != nullptr)
        g_cancellable_cancel(v->cancel);
    g_clear_object(&v->cancel);
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
        v->watching_drives = FALSE;
    }
    forget_keep(v);
    forget_drag(v);
    g_clear_pointer(&v->cut, g_hash_table_unref);
    g_clear_pointer(&v->band_base, g_array_unref);
    G_OBJECT_CLASS(xd_folder_view_parent_class)->dispose(obj);
}

static void folder_view_finalize(GObject *obj) {
    XdFolderView *v = XD_FOLDER_VIEW(obj);
    g_ptr_array_unref(v->shown);
    g_ptr_array_unref(v->all);
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
    v->shown = g_ptr_array_new();
    v->mode = XD_VIEW_ICONS;
    v->thumb = XD_THUMB_DEFAULT;
    v->focus = -1;
    v->anchor = -1;
    v->drop_index = -1;
    gtk_widget_set_can_focus(GTK_WIDGET(v), TRUE);
    gtk_drag_dest_set(GTK_WIDGET(v), (GtkDestDefaults)0, nullptr, 0,
                      (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
    gtk_drag_dest_add_uri_targets(GTK_WIDGET(v));
    gtk_widget_add_events(GTK_WIDGET(v), GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                             GDK_BUTTON1_MOTION_MASK | GDK_KEY_PRESS_MASK);
}

/* ---- public API ------------------------------------------------------------------ */

GtkWidget *xd_folder_view_new(void) { return g_object_new(XD_TYPE_FOLDER_VIEW, nullptr); }

void xd_folder_view_set_location(XdFolderView *v, GFile *dir) {
    g_return_if_fail(G_IS_FILE(dir));
    g_set_object(&v->dir, dir);
    forget_keep(v);
    if (v->monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(v->monitor, v);
        g_file_monitor_cancel(v->monitor);
        g_clear_object(&v->monitor);
    }
    v->place = xd_place_of(dir);
    g_clear_object(&v->listed);
    v->listed = v->place == XD_PLACE_TRASH ? xd_trash_files_dir() : g_object_ref(dir);
    if (v->place == XD_PLACE_COMPUTER) {
        if (!v->watching_drives) {
            GVolumeMonitor *vm = xd_computer_monitor();
            g_signal_connect(vm, "mount-added", G_CALLBACK(drives_changed), v);
            g_signal_connect(vm, "mount-removed", G_CALLBACK(drives_changed), v);
            g_signal_connect(vm, "mount-changed", G_CALLBACK(drives_changed), v);
            v->watching_drives = TRUE;
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
    start_loading(v);
}

GFile *xd_folder_view_get_location(XdFolderView *v) { return v->dir; }

void xd_folder_view_reload(XdFolderView *v) {
    if (v->dir == nullptr)
        return;
    /* The selection survives (unless files are already waiting to be). */
    if (v->keep == nullptr) {
        v->keep = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
        for (guint k = 0; k < v->shown->len; k++) {
            item *it = g_ptr_array_index(v->shown, k);
            if (it->selected)
                g_hash_table_add(v->keep, g_file_get_uri(it->file));
        }
        item *f = item_at(v, v->focus);
        v->keep_focus = f != nullptr ? g_file_get_uri(f->file) : nullptr;
    }
    double h = hval(v);
    double y = vval(v);
    start_loading(v);
    /* Keep the scroll position across a live refresh. */
    if (v->hadj != nullptr)
        gtk_adjustment_set_value(v->hadj, h);
    if (v->vadj != nullptr)
        gtk_adjustment_set_value(v->vadj, y);
}

void xd_folder_view_set_mode(XdFolderView *v, XdViewMode mode) {
    if (mode >= XD_VIEW_COUNT || mode == v->mode)
        return;
    v->mode = mode;
    if (v->hadj != nullptr)
        gtk_adjustment_set_value(v->hadj, 0);
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
    refilter(v);
}

XdSortColumn xd_folder_view_get_sort(XdFolderView *v) { return v->sort; }
gboolean xd_folder_view_get_sort_descending(XdFolderView *v) { return v->desc; }

void xd_folder_view_set_show_hidden(XdFolderView *v, gboolean show) {
    v->show_hidden = show;
    refilter(v);
    g_signal_emit(v, signals[SIG_CONTENTS], 0);
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
        a = (GdkRectangle){r.x + LIST_ICON + 6, r.y - 2, v->col_w[XD_SORT_NAME] - LIST_ICON - 10,
                           ROW_H + 4};
        break;
    case XD_VIEW_COUNT:
        break;
    }
    a.x -= ifloor(hval(v));
    a.y -= ifloor(vval(v));
    *area = a;
    return TRUE;
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
