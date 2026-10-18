/* SPDX-License-Identifier: MIT
 * Task Manager's Networking tab, as in Windows XP: a utilization graph for
 * each adapter, captioned with its name and scaled to the traffic, above
 * a list of Adapter Name, Network Utilization, Link Speed and State.
 * Utilization is the bytes sent and received against the link speed
 * (sysfs; nothing for an adapter that doesn't report one, such as Wi-Fi).
 */
#include "graph.h"
#include "netstat.h"
#include "taskmgr.h"

static constexpr int HISTORY = 1024;
static constexpr int SCALE_W = 44; /* the scale's labels left of a graph */

/* The graph's top, as a share of the link: XP zooms in on light traffic. */
static const double scales[] = {0.01, 0.05, 0.10, 0.25, 0.50, 1.0};

enum { NC_NAME, NC_UTIL, NC_SPEED, NC_STATE, NC_COUNT };

typedef struct adapter {
    char *name;
    xd_history hist;
    guint64 last_bytes;
    gboolean have_last;
    GtkWidget *scale, *area;
} adapter;

struct xd_net_tab {
    GtkWidget *box, *graphs, *list;
    GtkListStore *store;
    GPtrArray *adapters; /* adapter*, by name */
    gint64 last_time;    /* g_get_monotonic_time of the last sample */
};

static void adapter_free(gpointer p) {
    adapter *a = p;
    xd_history_clear(&a->hist);
    g_free(a->name);
    g_free(a);
}

/* The smallest scale that fits what the graph shows. */
static double graph_scale(const adapter *a, int samples) {
    double peak = 0;
    int n = MIN(samples, a->hist.len);
    for (int i = 0; i < n; i++) {
        int idx = ((a->hist.head - 1 - i) % a->hist.cap + a->hist.cap) % a->hist.cap;
        peak = MAX(peak, a->hist.value[idx]);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(scales); i++) {
        if (peak <= scales[i])
            return scales[i];
    }
    return 1.0;
}

static void scale_label(GtkWidget *w, cairo_t *cr, double y, double pct, double valign) {
    char text[16];
    g_snprintf(text, sizeof(text), pct > 0 && pct < 1 ? "%.1f %%" : "%.0f %%", pct);
    PangoLayout *l = gtk_widget_create_pango_layout(w, text);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double ty = CLAMP(y - th * valign, 0, MAX(height - th, 0));
    gtk_render_layout(gtk_widget_get_style_context(w), cr, width - tw - 4, ty, l);
    g_object_unref(l);
}

/* How many samples the graph shows (2px each). */
static int graph_samples(const adapter *a) {
    return gtk_widget_get_allocated_width(a->area) / 2 + 2;
}

/* The scale left of the graph: top, middle and 0. */
static gboolean draw_scale(GtkWidget *w, cairo_t *cr, gpointer data) {
    const adapter *a = data;
    double full = graph_scale(a, graph_samples(a));
    int height = gtk_widget_get_allocated_height(w);
    scale_label(w, cr, 0, full * 100, 0);
    scale_label(w, cr, height / 2.0, full * 50, 0.5);
    scale_label(w, cr, height, 0, 1);
    return TRUE;
}

static gboolean draw_graph(GtkWidget *w, cairo_t *cr, gpointer data) {
    const adapter *a = data;
    double x, y, wd, ht;
    xd_tm_draw_well(w, cr, &x, &y, &wd, &ht);
    xd_history_draw_scaled(cr, x, y, wd, ht, &a->hist, FALSE, graph_scale(a, graph_samples(a)));
    return TRUE;
}

static adapter *adapter_new(xd_net_tab *n, const char *name) {
    adapter *a = g_new0(adapter, 1);
    a->name = g_strdup(name);
    xd_history_init(&a->hist, HISTORY);
    a->scale = gtk_drawing_area_new();
    gtk_widget_set_size_request(a->scale, SCALE_W, -1);
    g_signal_connect(a->scale, "draw", G_CALLBACK(draw_scale), a);
    a->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(a->area, 120, 70);
    gtk_widget_set_hexpand(a->area, TRUE);
    gtk_widget_set_vexpand(a->area, TRUE);
    g_signal_connect(a->area, "draw", G_CALLBACK(draw_graph), a);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(row), a->scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), a->area, TRUE, TRUE, 0);
    GtkWidget *frame = xd_tm_group(name, row);
    gtk_box_pack_start(GTK_BOX(n->graphs), frame, TRUE, TRUE, 0);
    gtk_widget_show_all(frame);
    return a;
}

/* Same adapters as last time, in the same order? */
static gboolean same_adapters(const xd_net_tab *n, const GArray *ifs) {
    if (ifs->len != n->adapters->len)
        return FALSE;
    for (guint i = 0; i < ifs->len; i++) {
        const adapter *a = g_ptr_array_index(n->adapters, i);
        if (strcmp(a->name, g_array_index(ifs, xd_netif, i).name) != 0)
            return FALSE;
    }
    return TRUE;
}

static void rebuild(xd_net_tab *n, const GArray *ifs) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(n->graphs));
    for (GList *l = kids; l != nullptr; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    g_ptr_array_set_size(n->adapters, 0);
    gtk_list_store_clear(n->store);
    for (guint i = 0; i < ifs->len; i++) {
        g_ptr_array_add(n->adapters, adapter_new(n, g_array_index(ifs, xd_netif, i).name));
        gtk_list_store_insert_with_values(n->store, nullptr, -1, NC_NAME,
                                          g_array_index(ifs, xd_netif, i).name, -1);
    }
}

void xd_net_tab_sample(xd_net_tab *n, gboolean visible) {
    GArray *ifs = xd_netifs_read("/sys/class/net");
    gint64 now = g_get_monotonic_time();
    double seconds = n->last_time > 0 ? (double)(now - n->last_time) / G_USEC_PER_SEC : 0;
    n->last_time = now;
    if (!same_adapters(n, ifs))
        rebuild(n, ifs);
    GtkTreeIter iter;
    gboolean row = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(n->store), &iter);
    for (guint i = 0; i < ifs->len && row; i++) {
        const xd_netif *nif = &g_array_index(ifs, xd_netif, i);
        adapter *a = g_ptr_array_index(n->adapters, i);
        guint64 bytes = nif->rx_bytes + nif->tx_bytes;
        /* Counters restart when a driver reloads. */
        guint64 delta = a->have_last && bytes >= a->last_bytes ? bytes - a->last_bytes : 0;
        double share = a->have_last ? xd_netif_utilization(delta, seconds, nif->speed_mbps) : 0;
        a->last_bytes = bytes;
        a->have_last = TRUE;
        xd_history_push(&a->hist, share, 0);
        char *util = nif->speed_mbps > 0 ? xd_netif_utilization_text(share) : g_strdup("");
        char *speed = xd_netif_speed_text(nif->speed_mbps);
        gtk_list_store_set(n->store, &iter, NC_UTIL, util, NC_SPEED, speed, NC_STATE,
                           xd_netif_state_text(nif->state), -1);
        g_free(speed);
        g_free(util);
        if (visible) {
            gtk_widget_queue_draw(a->scale);
            gtk_widget_queue_draw(a->area);
        }
        row = gtk_tree_model_iter_next(GTK_TREE_MODEL(n->store), &iter);
    }
    xd_netifs_free(ifs);
}

static GtkTreeViewColumn *text_column(GtkWidget *tree, const char *title, int col, int width,
                                      double xalign) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", xalign, nullptr);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, nullptr);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_column_set_sort_column_id(c, col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    return c;
}

xd_net_tab *xd_net_tab_new(void) {
    xd_net_tab *n = g_new0(xd_net_tab, 1);
    n->adapters = g_ptr_array_new_with_free_func(adapter_free);
    n->box = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_container_set_border_width(GTK_CONTAINER(n->box), 8);
    n->graphs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), n->graphs);
    gtk_widget_set_size_request(sw, -1, 150);
    gtk_paned_pack1(GTK_PANED(n->box), sw, TRUE, FALSE);

    n->store = gtk_list_store_new(NC_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING);
    n->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(n->store));
    text_column(n->list, "Adapter Name", NC_NAME, 115, 0);
    text_column(n->list, "Network Utilization", NC_UTIL, 115, 1);
    text_column(n->list, "Link Speed", NC_SPEED, 75, 1);
    text_column(n->list, "State", NC_STATE, 90, 0);
    GtkWidget *list = xd_tm_scrolled(n->list);
    gtk_widget_set_size_request(list, -1, 80);
    gtk_widget_set_margin_top(list, 6);
    gtk_paned_pack2(GTK_PANED(n->box), list, FALSE, FALSE);
    return n;
}

GtkWidget *xd_net_tab_widget(xd_net_tab *n) { return n->box; }

void xd_net_tab_free(xd_net_tab *n) {
    if (n == nullptr)
        return;
    g_ptr_array_unref(n->adapters);
    g_object_unref(n->store);
    g_free(n);
}
