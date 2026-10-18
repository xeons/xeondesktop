/* SPDX-License-Identifier: MIT */
#include "wizard.h"

enum { RESP_BACK = 1, RESP_NEXT = 2, RESP_FINISH = 3 };

/* XP's wizard: the band of a Welcome page is 164 pixels wide, the
 * interior pages' header about 58 high. */
static constexpr int BAND_WIDTH = 164;
static constexpr int HEADER_HEIGHT = 58;

struct xd_wizard {
    GtkWidget *window, *stack, *back, *next, *finish;
    GPtrArray *pages; /* GtkWidget*, in index order */
    GArray *finals;   /* bool per page */
    GArray *history;  /* int: the pages Back returns to */
    char *icon_name;
    int current;
    xd_wizard_next_fn next_fn;
    xd_wizard_enter_fn enter_fn;
    xd_wizard_finish_fn finish_fn;
    gpointer data;
    GDestroyNotify destroy;
};

/* The white of Welcome pages and interior headers: the theme's Window
 * colour, as in XP. Added once per screen. */
static void add_css(void) {
    static bool done = false;
    if (done)
        return;
    done = true;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
                                    ".xd-wizard-white { background-color: @theme_base_color; "
                                    "color: @theme_text_color; }",
                                    -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void white(GtkWidget *w) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), "xd-wizard-white");
}

/* The band: XP's blue watermark, with the wizard's icon near the top. */
static gboolean draw_band(GtkWidget *area, cairo_t *cr, gpointer data) {
    xd_wizard *w = data;
    int width = gtk_widget_get_allocated_width(area);
    int height = gtk_widget_get_allocated_height(area);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, width * 0.4, height);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.35, 0.53, 0.89);
    cairo_pattern_add_color_stop_rgb(g, 0.55, 0.16, 0.33, 0.74);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.08, 0.20, 0.55);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
    /* A lighter swoosh, the way XP's watermarks have one. */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_move_to(cr, 0, height * 0.62);
    cairo_curve_to(cr, width * 0.4, height * 0.5, width * 0.7, height * 0.75, width,
                   height * 0.58);
    cairo_line_to(cr, width, height);
    cairo_line_to(cr, 0, height);
    cairo_close_path(cr);
    cairo_fill(cr);

    int scale = gtk_widget_get_scale_factor(area);
    GtkIconTheme *theme = gtk_icon_theme_get_for_screen(gtk_widget_get_screen(area));
    GdkPixbuf *icon = gtk_icon_theme_load_icon_for_scale(theme, w->icon_name, 96, scale,
                                                         GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
    if (icon != nullptr) {
        cairo_surface_t *s = gdk_cairo_surface_create_from_pixbuf(icon, scale, nullptr);
        cairo_set_source_surface(cr, s, (width - 96) / 2.0, 28);
        cairo_paint(cr);
        cairo_surface_destroy(s);
        g_object_unref(icon);
    }
    return FALSE;
}

static GtkWidget *wrap_label(const char *markup) {
    GtkWidget *l = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
    return l;
}

static int add_page(xd_wizard *w, GtkWidget *page, bool final) {
    int index = (int)w->pages->len;
    g_ptr_array_add(w->pages, page);
    g_array_append_val(w->finals, final);
    char name[16];
    g_snprintf(name, sizeof(name), "%d", index);
    gtk_widget_show_all(page);
    gtk_stack_add_named(GTK_STACK(w->stack), page, name);
    return index;
}

int xd_wizard_add_exterior(xd_wizard *w, const char *heading, GtkWidget *body, bool final) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    white(page);
    GtkWidget *band = gtk_drawing_area_new();
    gtk_widget_set_size_request(band, BAND_WIDTH, -1);
    g_signal_connect(band, "draw", G_CALLBACK(draw_band), w);
    gtk_box_pack_start(GTK_BOX(page), band, FALSE, FALSE, 0);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(right, 18);
    gtk_widget_set_margin_end(right, 18);
    gtk_widget_set_margin_top(right, 16);
    gtk_widget_set_margin_bottom(right, 12);
    /* Map Network Drive's one page has the band but no heading. */
    if (heading != nullptr) {
        char *markup = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>",
                                               heading);
        gtk_box_pack_start(GTK_BOX(right), wrap_label(markup), FALSE, FALSE, 0);
        g_free(markup);
    }
    if (body != nullptr)
        gtk_box_pack_start(GTK_BOX(right), body, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), right, TRUE, TRUE, 0);
    return add_page(w, page, final);
}

int xd_wizard_add_interior(xd_wizard *w, const char *heading, const char *subheading,
                           GtkWidget *body, bool final) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    white(header);
    gtk_widget_set_size_request(header, -1, HEADER_HEIGHT);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(texts, 20);
    char *markup = g_markup_printf_escaped("<b>%s</b>", heading);
    gtk_box_pack_start(GTK_BOX(texts), wrap_label(markup), FALSE, FALSE, 0);
    g_free(markup);
    if (subheading != nullptr) {
        markup = g_markup_escape_text(subheading, -1);
        GtkWidget *sub = wrap_label(markup);
        g_free(markup);
        gtk_widget_set_margin_start(sub, 16);
        gtk_box_pack_start(GTK_BOX(texts), sub, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(header), texts, TRUE, TRUE, 0);
    GtkWidget *icon = gtk_image_new_from_icon_name(w->icon_name, GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_margin_end(icon, 8);
    gtk_box_pack_end(GTK_BOX(header), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    if (body != nullptr) {
        gtk_widget_set_margin_start(body, 40);
        gtk_widget_set_margin_end(body, 20);
        gtk_widget_set_margin_top(body, 12);
        gtk_widget_set_margin_bottom(body, 8);
        gtk_box_pack_start(GTK_BOX(page), body, TRUE, TRUE, 0);
    }
    return add_page(w, page, final);
}

static void show_page(xd_wizard *w, int page) {
    if (page < 0 || page >= (int)w->pages->len)
        return;
    w->current = page;
    gtk_stack_set_visible_child(GTK_STACK(w->stack), g_ptr_array_index(w->pages, (guint)page));
    bool final = g_array_index(w->finals, bool, (guint)page);
    gtk_widget_set_visible(w->next, !final);
    gtk_widget_set_visible(w->finish, final);
    gtk_widget_set_sensitive(w->back, w->history->len > 0);
    gtk_widget_set_sensitive(w->next, TRUE);
    gtk_widget_set_sensitive(w->finish, TRUE);
    gtk_widget_grab_default(final ? w->finish : w->next);
    if (w->enter_fn != nullptr)
        w->enter_fn(w, page, w->data);
}

void xd_wizard_go(xd_wizard *w, int page) {
    g_array_append_val(w->history, w->current);
    show_page(w, page);
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    xd_wizard *w = data;
    switch (response) {
    case RESP_BACK:
        if (w->history->len > 0) {
            int prev = g_array_index(w->history, int, w->history->len - 1);
            g_array_set_size(w->history, w->history->len - 1);
            show_page(w, prev);
        }
        return;
    case RESP_NEXT: {
        int target = w->next_fn != nullptr ? w->next_fn(w, w->current, w->data) : w->current + 1;
        if (target >= 0)
            xd_wizard_go(w, target);
        return;
    }
    case RESP_FINISH:
        if (w->finish_fn != nullptr && !w->finish_fn(w, w->data))
            return;
        break;
    default:
        break;
    }
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_destroy([[maybe_unused]] GtkWidget *win, gpointer data) {
    xd_wizard *w = data;
    if (w->destroy != nullptr)
        w->destroy(w->data);
    g_ptr_array_unref(w->pages);
    g_array_unref(w->finals);
    g_array_unref(w->history);
    g_free(w->icon_name);
    g_free(w);
}

xd_wizard *xd_wizard_new(GtkWindow *parent, const char *title, const char *icon_name,
                         gpointer data, GDestroyNotify destroy) {
    add_css();
    xd_wizard *w = g_new0(xd_wizard, 1);
    w->pages = g_ptr_array_new();
    w->finals = g_array_new(FALSE, FALSE, sizeof(bool));
    w->history = g_array_new(FALSE, FALSE, sizeof(int));
    w->icon_name = g_strdup(icon_name);
    w->data = data;
    w->destroy = destroy;

    w->window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(w->window), title);
    gtk_window_set_icon_name(GTK_WINDOW(w->window), icon_name);
    gtk_window_set_resizable(GTK_WINDOW(w->window), FALSE);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(w->window), parent);
    w->back = gtk_dialog_add_button(GTK_DIALOG(w->window), "< _Back", RESP_BACK);
    w->next = gtk_dialog_add_button(GTK_DIALOG(w->window), "_Next >", RESP_NEXT);
    w->finish = gtk_dialog_add_button(GTK_DIALOG(w->window), "Finish", RESP_FINISH);
    GtkWidget *cancel = gtk_dialog_add_button(GTK_DIALOG(w->window), "Cancel",
                                              GTK_RESPONSE_CANCEL);
    /* The gap between Next and Cancel. */
    gtk_widget_set_margin_start(cancel, 8);
    GtkWidget *buttons[] = {w->back, w->next, w->finish, cancel};
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++)
        gtk_widget_set_size_request(buttons[i], 75, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(w->window));
    gtk_container_set_border_width(GTK_CONTAINER(content), 0);
    w->stack = gtk_stack_new();
    gtk_widget_set_size_request(w->stack, 500, 320);
    gtk_box_pack_start(GTK_BOX(content), w->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    g_signal_connect(w->window, "response", G_CALLBACK(on_response), w);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_destroy), w);
    return w;
}

GtkWidget *xd_wizard_window(xd_wizard *w) { return w->window; }

void xd_wizard_set_next_func(xd_wizard *w, xd_wizard_next_fn fn) { w->next_fn = fn; }
void xd_wizard_set_enter_func(xd_wizard *w, xd_wizard_enter_fn fn) { w->enter_fn = fn; }
void xd_wizard_set_finish_func(xd_wizard *w, xd_wizard_finish_fn fn) { w->finish_fn = fn; }

void xd_wizard_set_next_sensitive(xd_wizard *w, bool sensitive) {
    gtk_widget_set_sensitive(w->next, sensitive);
    gtk_widget_set_sensitive(w->finish, sensitive);
}

void xd_wizard_set_back_sensitive(xd_wizard *w, bool sensitive) {
    gtk_widget_set_sensitive(w->back, sensitive && w->history->len > 0);
}

int xd_wizard_page(xd_wizard *w) { return w->current; }

void xd_wizard_show(xd_wizard *w) {
    gtk_widget_show_all(w->window);
    show_page(w, 0);
}
