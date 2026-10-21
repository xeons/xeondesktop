/* SPDX-License-Identifier: MIT
 * XP's task pane. See taskpane.h.
 *
 * Laid out as XP's (shellstyle.dll's stylesheet, or a package's import of
 * it): the groups one under another inside the pane's padding, each a
 * title bar and, unless folded, a body of tasks (an icon and a link that
 * wraps) or lines of text. Everything is drawn by libxdtheme in device
 * pixels; GTK's logical coordinates are multiplied by the scale.
 */
#include "taskpane.h"

#include <pango/pangocairo.h>

#include "../settings/settings.h"
#include "../theme/panes.h"

typedef struct item {
    char *icon, *label;
    XdTaskFunc func;
    gpointer data;
    GDestroyNotify free;
    gboolean line, name;
    xd_rect row;  /* the hit area: icon and text */
    xd_rect text; /* where the text goes */
} item;

typedef struct group {
    char *title;
    gboolean special, folded;
    GPtrArray *items;
    xd_rect header, body;
} group;

typedef struct pane {
    GtkWidget *area, *scroll;
    GPtrArray *groups;
    GHashTable *folded; /* title -> GINT_TO_POINTER(1 + folded) */
    xd_theme theme;
    int theme_scale; /* 0: not made yet */
    xd_taskpane_style style;
    int hot_group, hot_item; /* -1: the header; -2: nothing */
    int press_group, press_item;
    /* While a task runs, the contents it replaces are kept (its data
     * among them) until it returns. */
    gboolean running;
    GPtrArray *stale; /* GPtrArray* of groups */
} pane;

/* ---- the drawing area: its height follows its width ---------------------------- */

#define XD_TYPE_TASK_AREA (xd_task_area_get_type())
G_DECLARE_FINAL_TYPE(XdTaskArea, xd_task_area, XD, TASK_AREA, GtkDrawingArea)

struct _XdTaskArea {
    GtkDrawingArea parent;
    pane *pane;
};

G_DEFINE_TYPE(XdTaskArea, xd_task_area, GTK_TYPE_DRAWING_AREA)

static int layout(pane *p, int width);
static void make_theme(pane *p);

static GtkSizeRequestMode area_request_mode([[maybe_unused]] GtkWidget *w) {
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static int scale_of(pane *p) { return MAX(1, gtk_widget_get_scale_factor(p->area)); }

static void area_height_for_width(GtkWidget *w, int width, int *min, int *nat) {
    pane *p = XD_TASK_AREA(w)->pane;
    int s = scale_of(p);
    int h = layout(p, width * s);
    *min = *nat = (h + s - 1) / s;
}

static void area_width(GtkWidget *w, int *min, int *nat) {
    pane *p = XD_TASK_AREA(w)->pane;
    make_theme(p);
    /* Room for the title bars' text and arrows at the least. */
    *min = *nat = 120 + (p->style.padding.left + p->style.padding.right) / scale_of(p);
}

static void area_height(GtkWidget *w, int *min, int *nat) {
    area_height_for_width(w, 200, min, nat);
}

static void xd_task_area_class_init(XdTaskAreaClass *klass) {
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    wc->get_request_mode = area_request_mode;
    wc->get_preferred_height_for_width = area_height_for_width;
    wc->get_preferred_width = area_width;
    wc->get_preferred_height = area_height;
}

static void xd_task_area_init([[maybe_unused]] XdTaskArea *self) {}

/* ---- the theme -------------------------------------------------------------------- */

static void make_theme(pane *p) {
    int s = scale_of(p);
    if (p->theme_scale == s)
        return;
    xd_settings set;
    xd_settings_load(&set);
    xd_scheme scheme;
    xd_settings_scheme(&set, &scheme);
    xd_theme_init(&p->theme, set.style, &scheme, s);
    xd_theme_taskpane_style(&p->theme, &p->style);
    p->theme_scale = s;
}

void xd_task_pane_reload_theme(GtkWidget *w) {
    pane *p = g_object_get_data(G_OBJECT(w), "xd-task-pane");
    p->theme_scale = 0;
    gtk_widget_queue_resize(p->area);
}

/* ---- layout ----------------------------------------------------------------------- */

static PangoLayout *text_layout(cairo_t *cr, pane *p, const char *text, const xd_font_spec *font,
                                int width, gboolean underline) {
    xd_text_style ts = xd_text(&p->theme, XD_FONT_MESSAGE, (xd_rgba){0, 0, 0, 1}, XD_ALIGN_START);
    ts.font = font;
    PangoLayout *l = xd_text_layout(cr, text, &ts);
    pango_layout_set_width(l, MAX(width, 1) * PANGO_SCALE);
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    if (underline) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    return l;
}

static const xd_font_spec *item_font(pane *p, const group *g, const item *it) {
    if (it->line)
        return it->name ? &p->style.details_name_font : &p->style.details_font;
    return &p->style.link_font[g->special][0];
}

/* Places the groups and their items for `width` device pixels; returns
 * the height they take. */
static int layout(pane *p, int width) {
    make_theme(p);
    const xd_taskpane_style *s = &p->style;
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(scratch);
    int x = s->padding.left, w = MAX(width - s->padding.left - s->padding.right, 1);
    int y = s->padding.top;
    for (guint gi = 0; gi < p->groups->len; gi++) {
        group *g = g_ptr_array_index(p->groups, gi);
        if (gi > 0)
            y += s->group_spacing;
        g->header = (xd_rect){x, y, w, s->header_height};
        y += s->header_height;
        if (g->folded) {
            g->body = (xd_rect){x, y, w, 0};
            continue;
        }
        const xd_insets *b = &s->body[g->special];
        int top = y;
        y += b->top;
        for (guint ii = 0; ii < g->items->len; ii++) {
            item *it = g_ptr_array_index(g->items, ii);
            int icon = it->icon != nullptr ? s->icon_size + s->icon_gap[g->special] : 0;
            int tx = x + b->left + icon, tw = MAX(x + w - b->right - tx, 1);
            y += it->line ? s->details_spacing : s->link_spacing[g->special];
            PangoLayout *l = text_layout(cr, p, it->label, item_font(p, g, it), tw, FALSE);
            int lw = 0, lh = 0;
            pango_layout_get_pixel_size(l, &lw, &lh);
            g_object_unref(l);
            int rh = MAX(lh, it->icon != nullptr ? s->icon_size : 0);
            /* One line beside the icon is centred on it. */
            int ty = y + (it->icon != nullptr && lh < s->icon_size ? (s->icon_size - lh) / 2 : 0);
            it->text = (xd_rect){tx, ty, tw, lh};
            it->row = (xd_rect){x + b->left, y, icon + MIN(lw, tw), rh};
            y += rh;
        }
        y += b->bottom;
        g->body = (xd_rect){x, top, w, y - top};
    }
    y += s->padding.bottom;
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);
    return y;
}

/* ---- drawing ---------------------------------------------------------------------- */

static void draw_icon(cairo_t *cr, const char *name, int x, int y, int size) {
    GError *err = nullptr;
    GdkPixbuf *pb = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), name, size,
                                             GTK_ICON_LOOKUP_FORCE_SIZE, &err);
    if (pb == nullptr) {
        g_clear_error(&err);
        return;
    }
    gdk_cairo_set_source_pixbuf(cr, pb, x, y);
    cairo_paint(cr);
    g_object_unref(pb);
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    pane *p = data;
    int s = scale_of(p);
    int width = gtk_widget_get_allocated_width(w) * s;
    int height = gtk_widget_get_allocated_height(w) * s;
    (void)layout(p, width);
    const xd_theme *t = &p->theme;
    const xd_taskpane_style *st = &p->style;
    cairo_save(cr);
    cairo_scale(cr, 1.0 / s, 1.0 / s);
    xd_theme_draw_taskpane(t, cr, (xd_rect){0, 0, width, height});
    for (guint gi = 0; gi < p->groups->len; gi++) {
        group *g = g_ptr_array_index(p->groups, gi);
        uint8_t state = XD_STATE_NORMAL;
        if (p->hot_group == (int)gi && p->hot_item == -1)
            state = p->press_group == (int)gi && p->press_item == -1 ? XD_STATE_PRESSED
                                                                     : XD_STATE_HOT;
        xd_theme_draw_taskpane_header(t, cr, st, g->header, g->special, !g->folded, state,
                                      g->title);
        if (g->folded)
            continue;
        xd_theme_draw_taskpane_body(t, cr, g->body, g->special);
        for (guint ii = 0; ii < g->items->len; ii++) {
            item *it = g_ptr_array_index(g->items, ii);
            bool hot = p->hot_group == (int)gi && p->hot_item == (int)ii;
            if (it->icon != nullptr)
                draw_icon(cr, it->icon, it->row.x, it->row.y, st->icon_size);
            xd_rgba color = it->line ? st->details_color : st->link_color[g->special][hot];
            const xd_font_spec *font = it->line ? item_font(p, g, it)
                                                : &st->link_font[g->special][hot];
            PangoLayout *l = text_layout(cr, p, it->label, font, it->text.w,
                                         !it->line && st->link_underline[g->special][hot]);
            xd_set_color(cr, color);
            cairo_move_to(cr, it->text.x, it->text.y);
            pango_cairo_show_layout(cr, l);
            g_object_unref(l);
        }
    }
    cairo_restore(cr);
    return TRUE;
}

/* ---- the pointer ------------------------------------------------------------------ */

static bool inside(xd_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* The header (item -1) or task under (x, y), device pixels. */
static bool hit(pane *p, int x, int y, int *gout, int *iout) {
    for (guint gi = 0; gi < p->groups->len; gi++) {
        group *g = g_ptr_array_index(p->groups, gi);
        if (inside(g->header, x, y)) {
            *gout = (int)gi;
            *iout = -1;
            return true;
        }
        if (g->folded)
            continue;
        for (guint ii = 0; ii < g->items->len; ii++) {
            item *it = g_ptr_array_index(g->items, ii);
            if (it->func != nullptr && inside(it->row, x, y)) {
                *gout = (int)gi;
                *iout = (int)ii;
                return true;
            }
        }
    }
    return false;
}

static void set_hot(pane *p, int g, int i) {
    if (p->hot_group == g && p->hot_item == i)
        return;
    p->hot_group = g;
    p->hot_item = i;
    GdkWindow *win = gtk_widget_get_window(p->area);
    if (win != nullptr) {
        /* The hand over links and title bars, as XP's. */
        GdkCursor *c = g >= 0 ? gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer")
                              : nullptr;
        gdk_window_set_cursor(win, c);
        if (c != nullptr)
            g_object_unref(c);
    }
    gtk_widget_queue_draw(p->area);
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    pane *p = data;
    int s = gtk_widget_get_scale_factor(w), g, i;
    if (hit(p, (int)(ev->x * s), (int)(ev->y * s), &g, &i))
        set_hot(p, g, i);
    else
        set_hot(p, -2, -2);
    return FALSE;
}

static gboolean on_leave([[maybe_unused]] GtkWidget *w, [[maybe_unused]] GdkEventCrossing *ev,
                         gpointer data) {
    set_hot(data, -2, -2);
    return FALSE;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    pane *p = data;
    int s = gtk_widget_get_scale_factor(w);
    if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (!hit(p, (int)(ev->x * s), (int)(ev->y * s), &p->press_group, &p->press_item))
        p->press_group = p->press_item = -2;
    gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    pane *p = data;
    int s = gtk_widget_get_scale_factor(w), g, i;
    if (ev->button != 1)
        return FALSE;
    bool same = hit(p, (int)(ev->x * s), (int)(ev->y * s), &g, &i) && g == p->press_group &&
                i == p->press_item;
    p->press_group = p->press_item = -2;
    gtk_widget_queue_draw(w);
    if (!same)
        return TRUE;
    group *gr = g_ptr_array_index(p->groups, (guint)g);
    if (i == -1) {
        gr->folded = !gr->folded;
        g_hash_table_replace(p->folded, g_strdup(gr->title), GINT_TO_POINTER(1 + gr->folded));
        gtk_widget_queue_resize(p->area);
        return TRUE;
    }
    item *it = g_ptr_array_index(gr->items, (guint)i);
    /* The task may refill the pane: keep what it needs. */
    XdTaskFunc func = it->func;
    gpointer arg = it->data;
    set_hot(p, -2, -2);
    p->running = TRUE;
    func(p->scroll, arg);
    p->running = FALSE;
    g_ptr_array_set_size(p->stale, 0);
    return TRUE;
}

/* ---- contents --------------------------------------------------------------------- */

static void free_item(gpointer data) {
    item *it = data;
    if (it->free != nullptr && it->data != nullptr)
        it->free(it->data);
    g_free(it->icon);
    g_free(it->label);
    g_free(it);
}

static void free_group(gpointer data) {
    group *g = data;
    g_ptr_array_unref(g->items);
    g_free(g->title);
    g_free(g);
}

static void free_pane(gpointer data) {
    pane *p = data;
    g_ptr_array_unref(p->groups);
    g_ptr_array_unref(p->stale);
    g_hash_table_unref(p->folded);
    g_free(p);
}

GtkWidget *xd_task_pane_new(void) {
    pane *p = g_new0(pane, 1);
    p->groups = g_ptr_array_new_with_free_func(free_group);
    p->stale = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    p->folded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    p->hot_group = p->hot_item = p->press_group = p->press_item = -2;
    XdTaskArea *area = g_object_new(XD_TYPE_TASK_AREA, nullptr);
    area->pane = p;
    p->area = GTK_WIDGET(area);
    gtk_widget_add_events(p->area, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK |
                                       GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(p->area, "draw", G_CALLBACK(on_draw), p);
    g_signal_connect(p->area, "motion-notify-event", G_CALLBACK(on_motion), p);
    g_signal_connect(p->area, "leave-notify-event", G_CALLBACK(on_leave), p);
    g_signal_connect(p->area, "button-press-event", G_CALLBACK(on_press), p);
    g_signal_connect(p->area, "button-release-event", G_CALLBACK(on_release), p);
    p->scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(p->scroll), p->area);
    gtk_widget_show_all(p->scroll);
    gtk_widget_hide(p->scroll);
    gtk_widget_set_name(p->scroll, "xd-task-pane");
    g_object_set_data_full(G_OBJECT(p->scroll), "xd-task-pane", p, free_pane);
    return p->scroll;
}

static pane *pane_of(GtkWidget *w) { return g_object_get_data(G_OBJECT(w), "xd-task-pane"); }

void xd_task_pane_begin(GtkWidget *w) {
    pane *p = pane_of(w);
    if (p->running) {
        g_ptr_array_add(p->stale, p->groups);
        p->groups = g_ptr_array_new_with_free_func(free_group);
    } else {
        g_ptr_array_set_size(p->groups, 0);
    }
    p->hot_group = p->hot_item = p->press_group = p->press_item = -2;
}

int xd_task_pane_group(GtkWidget *w, const char *title, gboolean special,
                       gboolean folded_at_first) {
    pane *p = pane_of(w);
    group *g = g_new0(group, 1);
    g->title = g_strdup(title);
    g->special = special;
    gpointer kept = g_hash_table_lookup(p->folded, title);
    g->folded = kept != nullptr ? GPOINTER_TO_INT(kept) - 1 : folded_at_first;
    g->items = g_ptr_array_new_with_free_func(free_item);
    g_ptr_array_add(p->groups, g);
    return (int)p->groups->len - 1;
}

static item *add_item(pane *p, int group_index, const char *label) {
    group *g = g_ptr_array_index(p->groups, (guint)group_index);
    item *it = g_new0(item, 1);
    it->label = g_strdup(label);
    g_ptr_array_add(g->items, it);
    return it;
}

void xd_task_pane_task(GtkWidget *w, int group_index, const char *icon_name, const char *label,
                       XdTaskFunc func, gpointer data, GDestroyNotify free) {
    item *it = add_item(pane_of(w), group_index, label);
    it->icon = g_strdup(icon_name);
    it->func = func;
    it->data = data;
    it->free = free;
}

void xd_task_pane_line(GtkWidget *w, int group_index, const char *text, gboolean name) {
    item *it = add_item(pane_of(w), group_index, text);
    it->line = TRUE;
    it->name = name;
}

void xd_task_pane_end(GtkWidget *w) {
    pane *p = pane_of(w);
    gtk_widget_queue_resize(p->area);
    gtk_widget_queue_draw(p->area);
}
