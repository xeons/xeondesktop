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

static int layout(pane *task_pane, int width);
static void make_theme(pane *task_pane);

static GtkSizeRequestMode area_request_mode([[maybe_unused]] GtkWidget *widget) {
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static int scale_of(pane *task_pane) {
    return MAX(1, gtk_widget_get_scale_factor(task_pane->area));
}

static void area_height_for_width(GtkWidget *widget, int width, int *min, int *nat) {
    pane *task_pane = XD_TASK_AREA(widget)->pane;
    int scale = scale_of(task_pane);
    int height = layout(task_pane, width * scale);
    *min = *nat = (height + scale - 1) / scale;
}

static void area_width(GtkWidget *widget, int *min, int *nat) {
    pane *task_pane = XD_TASK_AREA(widget)->pane;
    make_theme(task_pane);
    /* Room for the title bars' text and arrows at the least. */
    *min = *nat = 120 + (task_pane->style.padding.left + task_pane->style.padding.right) /
                            scale_of(task_pane);
}

static void area_height(GtkWidget *widget, int *min, int *nat) {
    area_height_for_width(widget, 200, min, nat);
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

static void make_theme(pane *task_pane) {
    int scale = scale_of(task_pane);
    if (task_pane->theme_scale == scale)
        return;
    xd_settings set;
    xd_settings_load(&set);
    xd_scheme scheme;
    xd_settings_scheme(&set, &scheme);
    xd_theme_init(&task_pane->theme, set.style, &scheme, scale);
    xd_theme_taskpane_style(&task_pane->theme, &task_pane->style);
    task_pane->theme_scale = scale;
}

void xd_task_pane_reload_theme(GtkWidget *widget) {
    pane *task_pane = g_object_get_data(G_OBJECT(widget), "xd-task-pane");
    task_pane->theme_scale = 0;
    gtk_widget_queue_resize(task_pane->area);
}

/* ---- layout ----------------------------------------------------------------------- */

static PangoLayout *text_layout(cairo_t *cairo, pane *task_pane, const char *text,
                                const xd_font_spec *font, int width, gboolean underline) {
    xd_text_style ts =
        xd_text(&task_pane->theme, XD_FONT_MESSAGE, (xd_rgba){0, 0, 0, 1}, XD_ALIGN_START);
    ts.font = font;
    PangoLayout *layout = xd_text_layout(cairo, text, &ts);
    pango_layout_set_width(layout, MAX(width, 1) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    if (underline) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
    }
    return layout;
}

static const xd_font_spec *item_font(pane *task_pane, const group *task_group, const item *it) {
    if (it->line)
        return it->name ? &task_pane->style.details_name_font : &task_pane->style.details_font;
    return &task_pane->style.link_font[task_group->special][0];
}

/* Places the groups and their items for `width` device pixels; returns
 * the height they take. */
static int layout(pane *task_pane, int width) {
    make_theme(task_pane);
    const xd_taskpane_style *taskpane_style = &task_pane->style;
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(scratch);
    int x = taskpane_style->padding.left,
        width_inside = MAX(width - taskpane_style->padding.left - taskpane_style->padding.right, 1);
    int y = taskpane_style->padding.top;
    for (guint gi = 0; gi < task_pane->groups->len; gi++) {
        group *task_group = g_ptr_array_index(task_pane->groups, gi);
        if (gi > 0)
            y += taskpane_style->group_spacing;
        task_group->header = (xd_rect){x, y, width_inside, taskpane_style->header_height};
        y += taskpane_style->header_height;
        if (task_group->folded) {
            task_group->body = (xd_rect){x, y, width_inside, 0};
            continue;
        }
        const xd_insets *insets = &taskpane_style->body[task_group->special];
        int top = y;
        y += insets->top;
        for (guint ii = 0; ii < task_group->items->len; ii++) {
            item *it = g_ptr_array_index(task_group->items, ii);
            int icon = it->icon != nullptr ? taskpane_style->icon_size +
                                                 taskpane_style->icon_gap[task_group->special]
                                           : 0;
            int tx = x + insets->left + icon, tw = MAX(x + width_inside - insets->right - tx, 1);
            y += it->line ? taskpane_style->details_spacing
                          : taskpane_style->link_spacing[task_group->special];
            PangoLayout *text = text_layout(cairo, task_pane, it->label,
                                            item_font(task_pane, task_group, it), tw, FALSE);
            int lw = 0, lh = 0;
            pango_layout_get_pixel_size(text, &lw, &lh);
            g_object_unref(text);
            int rh = MAX(lh, it->icon != nullptr ? taskpane_style->icon_size : 0);
            /* One line beside the icon is centred on it. */
            int ty = y + (it->icon != nullptr && lh < taskpane_style->icon_size
                              ? (taskpane_style->icon_size - lh) / 2
                              : 0);
            it->text = (xd_rect){tx, ty, tw, lh};
            it->row = (xd_rect){x + insets->left, y, icon + MIN(lw, tw), rh};
            y += rh;
        }
        y += insets->bottom;
        task_group->body = (xd_rect){x, top, width_inside, y - top};
    }
    y += taskpane_style->padding.bottom;
    cairo_destroy(cairo);
    cairo_surface_destroy(scratch);
    return y;
}

/* ---- drawing ---------------------------------------------------------------------- */

static void draw_icon(cairo_t *cairo, const char *name, int x, int y, int size) {
    GError *err = nullptr;
    GdkPixbuf *pb = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), name, size,
                                             GTK_ICON_LOOKUP_FORCE_SIZE, &err);
    if (pb == nullptr) {
        g_clear_error(&err);
        return;
    }
    gdk_cairo_set_source_pixbuf(cairo, pb, x, y);
    cairo_paint(cairo);
    g_object_unref(pb);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    pane *task_pane = data;
    int scale = scale_of(task_pane);
    int width = gtk_widget_get_allocated_width(widget) * scale;
    int height = gtk_widget_get_allocated_height(widget) * scale;
    (void)layout(task_pane, width);
    const xd_theme *theme = &task_pane->theme;
    const xd_taskpane_style *st = &task_pane->style;
    cairo_save(cairo);
    cairo_scale(cairo, 1.0 / scale, 1.0 / scale);
    xd_theme_draw_taskpane(theme, cairo, (xd_rect){0, 0, width, height});
    for (guint gi = 0; gi < task_pane->groups->len; gi++) {
        group *task_group = g_ptr_array_index(task_pane->groups, gi);
        uint8_t state = XD_STATE_NORMAL;
        if (task_pane->hot_group == (int)gi && task_pane->hot_item == -1)
            state = task_pane->press_group == (int)gi && task_pane->press_item == -1
                        ? XD_STATE_PRESSED
                        : XD_STATE_HOT;
        xd_theme_draw_taskpane_header(theme, cairo, st, task_group->header, task_group->special,
                                      !task_group->folded, state, task_group->title);
        if (task_group->folded)
            continue;
        xd_theme_draw_taskpane_body(theme, cairo, task_group->body, task_group->special);
        for (guint ii = 0; ii < task_group->items->len; ii++) {
            item *it = g_ptr_array_index(task_group->items, ii);
            bool hot = task_pane->hot_group == (int)gi && task_pane->hot_item == (int)ii;
            if (it->icon != nullptr)
                draw_icon(cairo, it->icon, it->row.x, it->row.y, st->icon_size);
            xd_rgba color = it->line ? st->details_color : st->link_color[task_group->special][hot];
            const xd_font_spec *font = it->line ? item_font(task_pane, task_group, it)
                                                : &st->link_font[task_group->special][hot];
            PangoLayout *text =
                text_layout(cairo, task_pane, it->label, font, it->text.w,
                            !it->line && st->link_underline[task_group->special][hot]);
            xd_set_color(cairo, color);
            cairo_move_to(cairo, it->text.x, it->text.y);
            pango_cairo_show_layout(cairo, text);
            g_object_unref(text);
        }
    }
    cairo_restore(cairo);
    return TRUE;
}

/* ---- the pointer ------------------------------------------------------------------ */

static bool inside(xd_rect rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

/* The header (item -1) or task under (x, y), device pixels. */
static bool hit(pane *task_pane, int x, int y, int *gout, int *iout) {
    for (guint gi = 0; gi < task_pane->groups->len; gi++) {
        group *task_group = g_ptr_array_index(task_pane->groups, gi);
        if (inside(task_group->header, x, y)) {
            *gout = (int)gi;
            *iout = -1;
            return true;
        }
        if (task_group->folded)
            continue;
        for (guint ii = 0; ii < task_group->items->len; ii++) {
            item *it = g_ptr_array_index(task_group->items, ii);
            if (it->func != nullptr && inside(it->row, x, y)) {
                *gout = (int)gi;
                *iout = (int)ii;
                return true;
            }
        }
    }
    return false;
}

static void set_hot(pane *task_pane, int group_index, int i) {
    if (task_pane->hot_group == group_index && task_pane->hot_item == i)
        return;
    task_pane->hot_group = group_index;
    task_pane->hot_item = i;
    GdkWindow *win = gtk_widget_get_window(task_pane->area);
    if (win != nullptr) {
        /* The hand over links and title bars, as XP's. */
        GdkCursor *cursor = group_index >= 0
                                ? gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer")
                                : nullptr;
        gdk_window_set_cursor(win, cursor);
        if (cursor != nullptr)
            g_object_unref(cursor);
    }
    gtk_widget_queue_draw(task_pane->area);
}

static gboolean on_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer data) {
    pane *task_pane = data;
    int scale = gtk_widget_get_scale_factor(widget), group_index, i;
    if (hit(task_pane, (int)(ev->x * scale), (int)(ev->y * scale), &group_index, &i))
        set_hot(task_pane, group_index, i);
    else
        set_hot(task_pane, -2, -2);
    return FALSE;
}

static gboolean on_leave([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] GdkEventCrossing *ev,
                         gpointer data) {
    set_hot(data, -2, -2);
    return FALSE;
}

static gboolean on_press(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    pane *task_pane = data;
    int scale = gtk_widget_get_scale_factor(widget);
    if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (!hit(task_pane, (int)(ev->x * scale), (int)(ev->y * scale), &task_pane->press_group,
             &task_pane->press_item))
        task_pane->press_group = task_pane->press_item = -2;
    gtk_widget_queue_draw(widget);
    return TRUE;
}

static gboolean on_release(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    pane *task_pane = data;
    int scale = gtk_widget_get_scale_factor(widget), group_index, i;
    if (ev->button != 1)
        return FALSE;
    bool same = hit(task_pane, (int)(ev->x * scale), (int)(ev->y * scale), &group_index, &i) &&
                group_index == task_pane->press_group && i == task_pane->press_item;
    task_pane->press_group = task_pane->press_item = -2;
    gtk_widget_queue_draw(widget);
    if (!same)
        return TRUE;
    group *gr = g_ptr_array_index(task_pane->groups, (guint)group_index);
    if (i == -1) {
        gr->folded = !gr->folded;
        g_hash_table_replace(task_pane->folded, g_strdup(gr->title),
                             GINT_TO_POINTER(1 + gr->folded));
        gtk_widget_queue_resize(task_pane->area);
        return TRUE;
    }
    item *it = g_ptr_array_index(gr->items, (guint)i);
    /* The task may refill the pane: keep what it needs. */
    XdTaskFunc func = it->func;
    gpointer arg = it->data;
    set_hot(task_pane, -2, -2);
    task_pane->running = TRUE;
    func(task_pane->scroll, arg);
    task_pane->running = FALSE;
    g_ptr_array_set_size(task_pane->stale, 0);
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
    group *task_group = data;
    g_ptr_array_unref(task_group->items);
    g_free(task_group->title);
    g_free(task_group);
}

static void free_pane(gpointer data) {
    pane *task_pane = data;
    g_ptr_array_unref(task_pane->groups);
    g_ptr_array_unref(task_pane->stale);
    g_hash_table_unref(task_pane->folded);
    g_free(task_pane);
}

GtkWidget *xd_task_pane_new(void) {
    pane *task_pane = g_new0(pane, 1);
    task_pane->groups = g_ptr_array_new_with_free_func(free_group);
    task_pane->stale = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    task_pane->folded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    task_pane->hot_group = task_pane->hot_item = task_pane->press_group = task_pane->press_item =
        -2;
    XdTaskArea *area = g_object_new(XD_TYPE_TASK_AREA, nullptr);
    area->pane = task_pane;
    task_pane->area = GTK_WIDGET(area);
    gtk_widget_add_events(task_pane->area, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK |
                                               GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(task_pane->area, "draw", G_CALLBACK(on_draw), task_pane);
    g_signal_connect(task_pane->area, "motion-notify-event", G_CALLBACK(on_motion), task_pane);
    g_signal_connect(task_pane->area, "leave-notify-event", G_CALLBACK(on_leave), task_pane);
    g_signal_connect(task_pane->area, "button-press-event", G_CALLBACK(on_press), task_pane);
    g_signal_connect(task_pane->area, "button-release-event", G_CALLBACK(on_release), task_pane);
    task_pane->scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(task_pane->scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(task_pane->scroll), task_pane->area);
    gtk_widget_show_all(task_pane->scroll);
    gtk_widget_hide(task_pane->scroll);
    gtk_widget_set_name(task_pane->scroll, "xd-task-pane");
    g_object_set_data_full(G_OBJECT(task_pane->scroll), "xd-task-pane", task_pane, free_pane);
    return task_pane->scroll;
}

static pane *pane_of(GtkWidget *widget) {
    return g_object_get_data(G_OBJECT(widget), "xd-task-pane");
}

void xd_task_pane_begin(GtkWidget *widget) {
    pane *task_pane = pane_of(widget);
    if (task_pane->running) {
        g_ptr_array_add(task_pane->stale, task_pane->groups);
        task_pane->groups = g_ptr_array_new_with_free_func(free_group);
    } else {
        g_ptr_array_set_size(task_pane->groups, 0);
    }
    task_pane->hot_group = task_pane->hot_item = task_pane->press_group = task_pane->press_item =
        -2;
}

int xd_task_pane_group(GtkWidget *widget, const char *title, gboolean special,
                       gboolean folded_at_first) {
    pane *task_pane = pane_of(widget);
    group *task_group = g_new0(group, 1);
    task_group->title = g_strdup(title);
    task_group->special = special;
    gpointer kept = g_hash_table_lookup(task_pane->folded, title);
    task_group->folded = kept != nullptr ? GPOINTER_TO_INT(kept) - 1 : folded_at_first;
    task_group->items = g_ptr_array_new_with_free_func(free_item);
    g_ptr_array_add(task_pane->groups, task_group);
    return (int)task_pane->groups->len - 1;
}

static item *add_item(pane *task_pane, int group_index, const char *label) {
    group *task_group = g_ptr_array_index(task_pane->groups, (guint)group_index);
    item *it = g_new0(item, 1);
    it->label = g_strdup(label);
    g_ptr_array_add(task_group->items, it);
    return it;
}

void xd_task_pane_task(GtkWidget *widget, int group_index, const char *icon_name, const char *label,
                       XdTaskFunc func, gpointer data, GDestroyNotify free) {
    item *it = add_item(pane_of(widget), group_index, label);
    it->icon = g_strdup(icon_name);
    it->func = func;
    it->data = data;
    it->free = free;
}

void xd_task_pane_line(GtkWidget *widget, int group_index, const char *text, gboolean name) {
    item *it = add_item(pane_of(widget), group_index, text);
    it->line = TRUE;
    it->name = name;
}

void xd_task_pane_end(GtkWidget *widget) {
    pane *task_pane = pane_of(widget);
    gtk_widget_queue_resize(task_pane->area);
    gtk_widget_queue_draw(task_pane->area);
}
