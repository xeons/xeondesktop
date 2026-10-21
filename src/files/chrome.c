/* SPDX-License-Identifier: MIT
 * Vista's and 7's Explorer chrome. See chrome.h.
 *
 * Each widget gets a "draw" handler that paints the package's part behind
 * what GTK draws, and CSS that takes GTK's own background away from it
 * (only for the parts the package styles).
 */
#include "chrome.h"

#include "../settings/settings.h"
#include "../theme/panes.h"

typedef struct chrome {
    xd_theme theme;
    int scale; /* the theme's; 0: not made yet */
    GtkWidget *tree;
    GtkTreePath *hot_row;
} chrome;

static void free_chrome(gpointer data) {
    chrome *c = data;
    if (c->hot_row != nullptr)
        gtk_tree_path_free(c->hot_row);
    g_free(c);
}

static const xd_theme *theme_for(chrome *c, int scale) {
    if (c->scale != scale) {
        xd_settings set;
        xd_settings_load(&set);
        xd_scheme scheme;
        xd_settings_scheme(&set, &scheme);
        xd_theme_init(&c->theme, set.style, &scheme, scale);
        c->scale = scale;
    }
    return &c->theme;
}

/* Paints `part` into the widget's rectangle `r` (logical pixels). */
static bool paint(chrome *c, GtkWidget *w, cairo_t *cr, xd_explorer_part part, uint8_t state,
                  GdkRectangle r) {
    int s = MAX(1, gtk_widget_get_scale_factor(w));
    const xd_theme *t = theme_for(c, s);
    cairo_save(cr);
    cairo_scale(cr, 1.0 / s, 1.0 / s);
    bool drew = xd_theme_draw_explorer_part(t, cr, part, state,
                                            (xd_rect){r.x * s, r.y * s, r.width * s, r.height * s});
    cairo_restore(cr);
    return drew;
}

static GdkRectangle whole(GtkWidget *w) {
    return (GdkRectangle){0, 0, gtk_widget_get_allocated_width(w),
                          gtk_widget_get_allocated_height(w)};
}

static uint8_t state_of(GtkWidget *w) {
    GtkStateFlags f = gtk_widget_get_state_flags(w);
    uint8_t s = XD_STATE_NORMAL;
    if (f & GTK_STATE_FLAG_PRELIGHT)
        s |= XD_STATE_HOT;
    if (f & GTK_STATE_FLAG_ACTIVE)
        s |= XD_STATE_PRESSED;
    if (f & GTK_STATE_FLAG_CHECKED)
        s |= XD_STATE_CHECKED;
    if (f & GTK_STATE_FLAG_INSENSITIVE)
        s |= XD_STATE_DISABLED;
    if (gtk_widget_has_focus(w))
        s |= XD_STATE_FOCUSED;
    return s;
}

static void add_css(GtkWidget *w, const char *css) {
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(w), GTK_STYLE_PROVIDER(p),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static char *css_color(const xd_theme *t, xd_explorer_part part, uint8_t state) {
    xd_rgba c;
    if (!xd_theme_explorer_text_color(t, part, state, &c))
        return g_strdup("");
    return g_strdup_printf("color: rgb(%d,%d,%d);", (int)(c.r * 255 + 0.5), (int)(c.g * 255 + 0.5),
                           (int)(c.b * 255 + 0.5));
}

/* ---- the command bar ---- */

static gboolean on_bar_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)paint(data, w, cr, XD_EXPLORER_COMMAND_BAR, XD_STATE_NORMAL, whole(w));
    return FALSE;
}

static gboolean on_button_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    uint8_t state = state_of(w);
    if (g_object_get_data(G_OBJECT(w), "xd-hot") != nullptr)
        state |= XD_STATE_HOT;
    (void)paint(data, w, cr, XD_EXPLORER_COMMAND_BUTTON, state, whole(w));
    return FALSE;
}

/* The pointer over a button: tracked here, as GTK leaves toolbar buttons'
 * prelight unset under some compositors' crossing events. */
static gboolean on_button_cross(GtkWidget *w, GdkEventCrossing *ev,
                                [[maybe_unused]] gpointer data) {
    if (ev->detail == GDK_NOTIFY_INFERIOR)
        return FALSE;
    g_object_set_data(G_OBJECT(w), "xd-hot", GINT_TO_POINTER(ev->type == GDK_ENTER_NOTIFY));
    gtk_widget_queue_draw(w);
    return FALSE;
}

static void attach_buttons(GtkWidget *item, gpointer data) {
    GtkWidget *button = GTK_IS_BIN(item) ? gtk_bin_get_child(GTK_BIN(item)) : nullptr;
    if (button == nullptr || !GTK_IS_BUTTON(button))
        return;
    g_signal_connect(button, "draw", G_CALLBACK(on_button_draw), data);
    g_signal_connect(button, "enter-notify-event", G_CALLBACK(on_button_cross), nullptr);
    g_signal_connect(button, "leave-notify-event", G_CALLBACK(on_button_cross), nullptr);
    add_css(button, "button { background: none; border-color: transparent; box-shadow: none; }");
}

/* ---- the address box ---- */

static gboolean on_address_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    chrome *c = data;
    /* 7's box is translucent, made to lie on glass: on our window, it
     * lies on the text field's own colour. */
    GdkRectangle r = whole(w);
    if (gtk_widget_is_sensitive(w)) {
        int s = MAX(1, gtk_widget_get_scale_factor(w));
        xd_set_color(cr, xd_sys(theme_for(c, s), XD_COLOR_WINDOW));
        cairo_rectangle(cr, r.x + 1, r.y + 1, r.width - 2, r.height - 2);
        cairo_fill(cr);
    }
    (void)paint(c, w, cr, XD_EXPLORER_ADDRESS_BOX, state_of(w), r);
    return FALSE;
}

/* ---- the Folders tree ---- */

static void paint_row(chrome *c, GtkTreeView *tv, cairo_t *cr, GtkTreePath *path, uint8_t state) {
    GdkRectangle r;
    gtk_tree_view_get_background_area(tv, path, gtk_tree_view_get_column(tv, 0), &r);
    int x, y;
    gtk_tree_view_convert_bin_window_to_widget_coords(tv, r.x, r.y, &x, &y);
    /* The whole row, as 7's navigation pane selects. */
    r = (GdkRectangle){0, y, gtk_widget_get_allocated_width(GTK_WIDGET(tv)), r.height};
    (void)paint(c, GTK_WIDGET(tv), cr, XD_EXPLORER_TREE_ITEM, state, r);
}

static gboolean on_tree_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    chrome *c = data;
    GtkTreeView *tv = GTK_TREE_VIEW(w);
    int s = MAX(1, gtk_widget_get_scale_factor(w));
    /* The tree's own background is taken away (it would cover the rows'):
     * it's painted here. */
    xd_set_color(cr, xd_sys(theme_for(c, s), XD_COLOR_WINDOW));
    cairo_paint(cr);
    GtkTreeModel *model = nullptr;
    GtkTreeIter iter;
    GtkTreePath *selected = nullptr;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tv), &model, &iter))
        selected = gtk_tree_model_get_path(model, &iter);
    bool focus = gtk_widget_has_focus(w);
    if (c->hot_row != nullptr) {
        bool both = selected != nullptr && gtk_tree_path_compare(selected, c->hot_row) == 0;
        paint_row(c, tv, cr, c->hot_row,
                  XD_STATE_HOT | (both ? XD_STATE_CHECKED : 0) | (focus ? XD_STATE_FOCUSED : 0));
        if (both) {
            gtk_tree_path_free(selected);
            selected = nullptr;
        }
    }
    if (selected != nullptr) {
        paint_row(c, tv, cr, selected, XD_STATE_CHECKED | (focus ? XD_STATE_FOCUSED : 0));
        gtk_tree_path_free(selected);
    }
    return FALSE;
}

static void set_hot_row(chrome *c, GtkTreePath *path) {
    bool same = (path == nullptr && c->hot_row == nullptr) ||
                (path != nullptr && c->hot_row != nullptr &&
                 gtk_tree_path_compare(path, c->hot_row) == 0);
    if (c->hot_row != nullptr)
        gtk_tree_path_free(c->hot_row);
    c->hot_row = path;
    if (!same)
        gtk_widget_queue_draw(c->tree);
}

static gboolean on_tree_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    GtkTreePath *path = nullptr;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, nullptr,
                                       nullptr, nullptr))
        path = nullptr;
    set_hot_row(data, path);
    return FALSE;
}

static gboolean on_tree_leave([[maybe_unused]] GtkWidget *w,
                              [[maybe_unused]] GdkEventCrossing *ev, gpointer data) {
    set_hot_row(data, nullptr);
    return FALSE;
}

/* ---- the details pane ---- */

static gboolean on_status_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)paint(data, w, cr, XD_EXPLORER_DETAILS_PANE, XD_STATE_NORMAL, whole(w));
    return FALSE;
}

void xd_explorer_chrome_attach(GtkWidget *toolbar, GtkWidget *address, GtkWidget *tree,
                               GtkWidget *status_bar) {
    chrome *c = g_new0(chrome, 1);
    c->tree = tree;
    g_object_set_data_full(G_OBJECT(toolbar), "xd-explorer-chrome", c, free_chrome);
    const xd_theme *t = theme_for(c, MAX(1, gtk_widget_get_scale_factor(toolbar)));
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    cairo_t *cr = cairo_create(probe);
    /* What the package styles: a part it draws. */
    bool bar = xd_theme_draw_explorer_part(t, cr, XD_EXPLORER_COMMAND_BAR, 0, (xd_rect){0, 0, 4, 4});
    bool box = xd_theme_draw_explorer_part(t, cr, XD_EXPLORER_ADDRESS_BOX, 0, (xd_rect){0, 0, 4, 4});
    bool rows = xd_theme_draw_explorer_part(t, cr, XD_EXPLORER_TREE_ITEM, XD_STATE_CHECKED,
                                            (xd_rect){0, 0, 4, 4});
    bool details = xd_theme_draw_explorer_part(t, cr, XD_EXPLORER_DETAILS_PANE, 0,
                                               (xd_rect){0, 0, 4, 4});
    cairo_destroy(cr);
    cairo_surface_destroy(probe);
    if (bar) {
        g_signal_connect(toolbar, "draw", G_CALLBACK(on_bar_draw), c);
        char *color = css_color(t, XD_EXPLORER_COMMAND_BAR, 0);
        char *css = g_strdup_printf("toolbar { background: none; border: none; %s }", color);
        add_css(toolbar, css);
        g_free(css);
        g_free(color);
        gtk_container_foreach(GTK_CONTAINER(toolbar), attach_buttons, c);
    }
    if (box) {
        g_signal_connect(address, "draw", G_CALLBACK(on_address_draw), c);
        add_css(address, "entry { background: none; border-color: transparent; box-shadow: none; }");
    }
    if (rows) {
        g_signal_connect(tree, "draw", G_CALLBACK(on_tree_draw), c);
        gtk_widget_add_events(tree, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
        g_signal_connect(tree, "motion-notify-event", G_CALLBACK(on_tree_motion), c);
        g_signal_connect(tree, "leave-notify-event", G_CALLBACK(on_tree_leave), c);
        char *color = css_color(t, XD_EXPLORER_TREE_ITEM, XD_STATE_CHECKED | XD_STATE_FOCUSED);
        char *css = g_strdup_printf("treeview.view, treeview.view:selected, "
                                    "treeview.view:selected:focus { background: none; }"
                                    "treeview.view:selected { %s }",
                                    color);
        add_css(tree, css);
        g_free(css);
        g_free(color);
    }
    if (details)
        g_signal_connect(status_bar, "draw", G_CALLBACK(on_status_draw), c);
}
