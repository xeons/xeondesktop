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
    chrome *chrome_state = data;
    if (chrome_state->hot_row != nullptr)
        gtk_tree_path_free(chrome_state->hot_row);
    g_free(chrome_state);
}

static const xd_theme *theme_for(chrome *chrome_state, int scale) {
    if (chrome_state->scale != scale) {
        xd_settings set;
        xd_settings_load(&set);
        xd_scheme scheme;
        xd_settings_scheme(&set, &scheme);
        xd_theme_init(&chrome_state->theme, set.style, &scheme, scale);
        chrome_state->scale = scale;
    }
    return &chrome_state->theme;
}

/* Paints `part` into the widget's rectangle `area` (logical pixels). */
static bool paint(chrome *chrome_state, GtkWidget *widget, cairo_t *cairo, xd_explorer_part part,
                  uint8_t state, GdkRectangle area) {
    int scale = MAX(1, gtk_widget_get_scale_factor(widget));
    const xd_theme *theme = theme_for(chrome_state, scale);
    cairo_save(cairo);
    cairo_scale(cairo, 1.0 / scale, 1.0 / scale);
    bool drew = xd_theme_draw_explorer_part(
        theme, cairo, part, state,
        (xd_rect){area.x * scale, area.y * scale, area.width * scale, area.height * scale});
    cairo_restore(cairo);
    return drew;
}

static GdkRectangle whole(GtkWidget *widget) {
    return (GdkRectangle){0, 0, gtk_widget_get_allocated_width(widget),
                          gtk_widget_get_allocated_height(widget)};
}

static uint8_t state_of(GtkWidget *widget) {
    GtkStateFlags flags = gtk_widget_get_state_flags(widget);
    uint8_t state = XD_STATE_NORMAL;
    if (flags & GTK_STATE_FLAG_PRELIGHT)
        state |= XD_STATE_HOT;
    if (flags & GTK_STATE_FLAG_ACTIVE)
        state |= XD_STATE_PRESSED;
    if (flags & GTK_STATE_FLAG_CHECKED)
        state |= XD_STATE_CHECKED;
    if (flags & GTK_STATE_FLAG_INSENSITIVE)
        state |= XD_STATE_DISABLED;
    if (gtk_widget_has_focus(widget))
        state |= XD_STATE_FOCUSED;
    return state;
}

static void add_css(GtkWidget *widget, const char *css) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
                                   GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static char *css_color(const xd_theme *theme, xd_explorer_part part, uint8_t state) {
    xd_rgba color;
    if (!xd_theme_explorer_text_color(theme, part, state, &color))
        return g_strdup("");
    return g_strdup_printf("color: rgb(%d,%d,%d);", (int)(color.r * 255 + 0.5),
                           (int)(color.g * 255 + 0.5), (int)(color.b * 255 + 0.5));
}

/* ---- the command bar ---- */

static gboolean on_bar_draw(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    (void)paint(data, widget, cairo, XD_EXPLORER_COMMAND_BAR, XD_STATE_NORMAL, whole(widget));
    return FALSE;
}

static gboolean on_button_draw(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    uint8_t state = state_of(widget);
    if (g_object_get_data(G_OBJECT(widget), "xd-hot") != nullptr)
        state |= XD_STATE_HOT;
    (void)paint(data, widget, cairo, XD_EXPLORER_COMMAND_BUTTON, state, whole(widget));
    return FALSE;
}

/* The pointer over a button: tracked here, as GTK leaves toolbar buttons'
 * prelight unset under some compositors' crossing events. */
static gboolean on_button_cross(GtkWidget *widget, GdkEventCrossing *ev,
                                [[maybe_unused]] gpointer data) {
    if (ev->detail == GDK_NOTIFY_INFERIOR)
        return FALSE;
    g_object_set_data(G_OBJECT(widget), "xd-hot", GINT_TO_POINTER(ev->type == GDK_ENTER_NOTIFY));
    gtk_widget_queue_draw(widget);
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

static gboolean on_address_draw(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    chrome *chrome_state = data;
    /* 7's box is translucent, made to lie on glass: on our window, it
     * lies on the text field's own colour. */
    GdkRectangle area = whole(widget);
    if (gtk_widget_is_sensitive(widget)) {
        int scale = MAX(1, gtk_widget_get_scale_factor(widget));
        xd_set_color(cairo, xd_sys(theme_for(chrome_state, scale), XD_COLOR_WINDOW));
        cairo_rectangle(cairo, area.x + 1, area.y + 1, area.width - 2, area.height - 2);
        cairo_fill(cairo);
    }
    (void)paint(chrome_state, widget, cairo, XD_EXPLORER_ADDRESS_BOX, state_of(widget), area);
    return FALSE;
}

/* ---- the Folders tree ---- */

static void paint_row(chrome *chrome_state, GtkTreeView *tv, cairo_t *cairo, GtkTreePath *path,
                      uint8_t state) {
    GdkRectangle area;
    gtk_tree_view_get_background_area(tv, path, gtk_tree_view_get_column(tv, 0), &area);
    int x, y;
    gtk_tree_view_convert_bin_window_to_widget_coords(tv, area.x, area.y, &x, &y);
    /* The whole row, as 7's navigation pane selects. */
    area = (GdkRectangle){0, y, gtk_widget_get_allocated_width(GTK_WIDGET(tv)), area.height};
    (void)paint(chrome_state, GTK_WIDGET(tv), cairo, XD_EXPLORER_TREE_ITEM, state, area);
}

static gboolean on_tree_draw(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    chrome *chrome_state = data;
    GtkTreeView *tv = GTK_TREE_VIEW(widget);
    int scale = MAX(1, gtk_widget_get_scale_factor(widget));
    /* The tree's own background is taken away (it would cover the rows'):
     * it's painted here. */
    xd_set_color(cairo, xd_sys(theme_for(chrome_state, scale), XD_COLOR_WINDOW));
    cairo_paint(cairo);
    GtkTreeModel *model = nullptr;
    GtkTreeIter iter;
    GtkTreePath *selected = nullptr;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tv), &model, &iter))
        selected = gtk_tree_model_get_path(model, &iter);
    bool focus = gtk_widget_has_focus(widget);
    if (chrome_state->hot_row != nullptr) {
        bool both =
            selected != nullptr && gtk_tree_path_compare(selected, chrome_state->hot_row) == 0;
        paint_row(chrome_state, tv, cairo, chrome_state->hot_row,
                  XD_STATE_HOT | (both ? XD_STATE_CHECKED : 0) | (focus ? XD_STATE_FOCUSED : 0));
        if (both) {
            gtk_tree_path_free(selected);
            selected = nullptr;
        }
    }
    if (selected != nullptr) {
        paint_row(chrome_state, tv, cairo, selected,
                  XD_STATE_CHECKED | (focus ? XD_STATE_FOCUSED : 0));
        gtk_tree_path_free(selected);
    }
    return FALSE;
}

static void set_hot_row(chrome *chrome_state, GtkTreePath *path) {
    bool same = (path == nullptr && chrome_state->hot_row == nullptr) ||
                (path != nullptr && chrome_state->hot_row != nullptr &&
                 gtk_tree_path_compare(path, chrome_state->hot_row) == 0);
    if (chrome_state->hot_row != nullptr)
        gtk_tree_path_free(chrome_state->hot_row);
    chrome_state->hot_row = path;
    if (!same)
        gtk_widget_queue_draw(chrome_state->tree);
}

static gboolean on_tree_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer data) {
    GtkTreePath *path = nullptr;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path,
                                       nullptr, nullptr, nullptr))
        path = nullptr;
    set_hot_row(data, path);
    return FALSE;
}

static gboolean on_tree_leave([[maybe_unused]] GtkWidget *widget,
                              [[maybe_unused]] GdkEventCrossing *ev, gpointer data) {
    set_hot_row(data, nullptr);
    return FALSE;
}

/* ---- the details pane ---- */

static gboolean on_status_draw(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    (void)paint(data, widget, cairo, XD_EXPLORER_DETAILS_PANE, XD_STATE_NORMAL, whole(widget));
    return FALSE;
}

void xd_explorer_chrome_attach(GtkWidget *toolbar, GtkWidget *address, GtkWidget *tree,
                               GtkWidget *status_bar) {
    chrome *chrome_state = g_new0(chrome, 1);
    chrome_state->tree = tree;
    g_object_set_data_full(G_OBJECT(toolbar), "xd-explorer-chrome", chrome_state, free_chrome);
    const xd_theme *theme = theme_for(chrome_state, MAX(1, gtk_widget_get_scale_factor(toolbar)));
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    cairo_t *cairo = cairo_create(probe);
    /* What the package styles: a part it draws. */
    bool bar = xd_theme_draw_explorer_part(theme, cairo, XD_EXPLORER_COMMAND_BAR, 0,
                                           (xd_rect){0, 0, 4, 4});
    bool box = xd_theme_draw_explorer_part(theme, cairo, XD_EXPLORER_ADDRESS_BOX, 0,
                                           (xd_rect){0, 0, 4, 4});
    bool rows = xd_theme_draw_explorer_part(theme, cairo, XD_EXPLORER_TREE_ITEM, XD_STATE_CHECKED,
                                            (xd_rect){0, 0, 4, 4});
    bool details = xd_theme_draw_explorer_part(theme, cairo, XD_EXPLORER_DETAILS_PANE, 0,
                                               (xd_rect){0, 0, 4, 4});
    cairo_destroy(cairo);
    cairo_surface_destroy(probe);
    if (bar) {
        g_signal_connect(toolbar, "draw", G_CALLBACK(on_bar_draw), chrome_state);
        char *color = css_color(theme, XD_EXPLORER_COMMAND_BAR, 0);
        char *css = g_strdup_printf("toolbar { background: none; border: none; %s }", color);
        add_css(toolbar, css);
        g_free(css);
        g_free(color);
        gtk_container_foreach(GTK_CONTAINER(toolbar), attach_buttons, chrome_state);
    }
    if (box) {
        g_signal_connect(address, "draw", G_CALLBACK(on_address_draw), chrome_state);
        add_css(address, "entry { background: none; border-color: transparent; box-shadow: none; }");
    }
    if (rows) {
        g_signal_connect(tree, "draw", G_CALLBACK(on_tree_draw), chrome_state);
        gtk_widget_add_events(tree, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
        g_signal_connect(tree, "motion-notify-event", G_CALLBACK(on_tree_motion), chrome_state);
        g_signal_connect(tree, "leave-notify-event", G_CALLBACK(on_tree_leave), chrome_state);
        char *color = css_color(theme, XD_EXPLORER_TREE_ITEM, XD_STATE_CHECKED | XD_STATE_FOCUSED);
        char *css = g_strdup_printf("treeview.view, treeview.view:selected, "
                                    "treeview.view:selected:focus { background: none; }"
                                    "treeview.view:selected { %s }",
                                    color);
        add_css(tree, css);
        g_free(css);
        g_free(color);
    }
    if (details)
        g_signal_connect(status_bar, "draw", G_CALLBACK(on_status_draw), chrome_state);
}
