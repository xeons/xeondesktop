/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "../core/alloc.h"
#include "folderview.h"

#define ATTRIBUTES                                                             \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME  \
    "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE      \
    "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_ICON \
    "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP \
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
} item;

struct _XdFolderView {
    GtkDrawingArea parent;
    GtkAdjustment *hadj, *vadj;
    gulong hadj_id, vadj_id;
    GtkScrollablePolicy hpolicy, vpolicy;

    GFile *dir;
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

    /* Layout (content coordinates). */
    int cell_w, cell_h, cols, rows;
    int content_w, content_h;
    int col_w[XD_SORT_COUNT];
};

enum { PROP_0, PROP_HADJ, PROP_VADJ, PROP_HPOLICY, PROP_VPOLICY };
enum { SIG_ACTIVATED, SIG_SELECTION, SIG_CONTENTS, SIG_ERROR, SIG_CONTEXT, SIG_COUNT };
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
                         const colors *c) {
    if (pb == nullptr)
        return;
    gdk_cairo_set_source_pixbuf(cr, pb, x, y);
    cairo_paint(cr);
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
    gboolean sel = it->selected;
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
                         by + (box - gdk_pixbuf_get_height(pb)) / 2.0, FALSE, c);
        draw_label_box(v, cr, it->name, r.x + 2, by + box + 4, r.width - 4, 1, PANGO_ALIGN_CENTER,
                       sel, c, foc);
        break;
    }
    case XD_VIEW_ICONS: {
        int px = 32;
        GdkPixbuf *pb = item_icon(v, it, px);
        paint_pixbuf(cr, pb, r.x + (r.width - px) / 2.0, r.y + 4, sel, c);
        draw_label_box(v, cr, it->name, r.x + 2, r.y + px + 8, r.width - 4, 2, PANGO_ALIGN_CENTER,
                       sel, c, foc);
        break;
    }
    case XD_VIEW_TILES: {
        int px = 48;
        GdkPixbuf *pb = item_icon(v, it, px);
        paint_pixbuf(cr, pb, r.x + 6, r.y + (r.height - px) / 2.0, sel, c);
        int tx = r.x + px + 14;
        int tw = r.width - px - 22;
        draw_label_box(v, cr, it->name, tx, r.y + 8, tw, 1, PANGO_ALIGN_LEFT, sel, c, foc);
        draw_text(v, cr, it->type_desc, tx, r.y + 26, tw, 1, PANGO_ALIGN_LEFT, c->dim);
        if (!it->is_dir)
            draw_text(v, cr, it->size_text, tx, r.y + 42, tw, 1, PANGO_ALIGN_LEFT, c->dim);
        break;
    }
    case XD_VIEW_LIST: {
        GdkPixbuf *pb = item_icon(v, it, LIST_ICON);
        paint_pixbuf(cr, pb, r.x + 1, r.y + 1, sel, c);
        draw_label_box(v, cr, it->name, r.x + LIST_ICON + 5, r.y + 1,
                       r.width - LIST_ICON - 6, 1, PANGO_ALIGN_LEFT, sel, c, foc);
        break;
    }
    case XD_VIEW_DETAILS: {
        GdkPixbuf *pb = item_icon(v, it, LIST_ICON);
        paint_pixbuf(cr, pb, r.x + 4, r.y + 1, sel, c);
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
    static const char *const titles[XD_SORT_COUNT] = {"Name", "Size", "Type", "Date Modified"};
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

    if (v->mode == XD_VIEW_DETAILS)
        draw_header(v, cr, &c);
    if (n == 0 && v->dir != nullptr && v->cancel == nullptr)
        draw_text(v, cr, "This folder is empty.", 0, (v->mode == XD_VIEW_DETAILS ? HEADER_H : 0) + 12,
                  w, 1, PANGO_ALIGN_CENTER, c.dim);
    return FALSE;
}

/* ---- loading ------------------------------------------------------------------------ */

static void loading_done(XdFolderView *v) {
    g_clear_object(&v->cancel);
    refilter(v);
    g_signal_emit(v, signals[SIG_CONTENTS], 0);
    g_signal_emit(v, signals[SIG_SELECTION], 0);
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
        g_ptr_array_add(v->all, item_new(dir, l->data));
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
    g_file_enumerate_children_async(v->dir, ATTRIBUTES, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                    v->cancel, enumerate_ready, v);
    layout_contents(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

static gboolean reload_timeout(gpointer data) {
    XdFolderView *v = data;
    v->reload_source = 0;
    xd_folder_view_reload(v);
    return G_SOURCE_REMOVE;
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
    if (it == nullptr) {
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
    selection_changed(v);
    if (ev->button == GDK_BUTTON_SECONDARY)
        g_signal_emit(v, signals[SIG_CONTEXT], 0, (GdkEvent *)ev);
    return TRUE;
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
    if (ctrl && ev->keyval == GDK_KEY_space && item_at(v, v->focus) != nullptr) {
        item *it = item_at(v, v->focus);
        it->selected = !it->selected;
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
}

static void xd_folder_view_init(XdFolderView *v) {
    v->all = g_ptr_array_new_with_free_func(item_free);
    v->shown = g_ptr_array_new();
    v->mode = XD_VIEW_ICONS;
    v->thumb = XD_THUMB_DEFAULT;
    v->focus = -1;
    v->anchor = -1;
    gtk_widget_set_can_focus(GTK_WIDGET(v), TRUE);
    gtk_widget_add_events(GTK_WIDGET(v), GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
}

/* ---- public API ------------------------------------------------------------------ */

GtkWidget *xd_folder_view_new(void) { return g_object_new(XD_TYPE_FOLDER_VIEW, nullptr); }

void xd_folder_view_set_location(XdFolderView *v, GFile *dir) {
    g_return_if_fail(G_IS_FILE(dir));
    g_set_object(&v->dir, dir);
    if (v->monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(v->monitor, v);
        g_file_monitor_cancel(v->monitor);
        g_clear_object(&v->monitor);
    }
    v->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (v->monitor != nullptr)
        g_signal_connect(v->monitor, "changed", G_CALLBACK(monitor_changed), v);
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
