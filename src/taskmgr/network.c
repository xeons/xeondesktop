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

static void adapter_free(gpointer adapter_data) {
    adapter *net_adapter = adapter_data;
    xd_history_clear(&net_adapter->hist);
    g_free(net_adapter->name);
    g_free(net_adapter);
}

/* The smallest scale that fits what the graph shows. */
static double graph_scale(const adapter *net_adapter, int samples) {
    double peak = 0;
    int count = MIN(samples, net_adapter->hist.len);
    for (int i = 0; i < count; i++) {
        int idx =
            ((net_adapter->hist.head - 1 - i) % net_adapter->hist.cap + net_adapter->hist.cap) %
            net_adapter->hist.cap;
        peak = MAX(peak, net_adapter->hist.value[idx]);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(scales); i++) {
        if (peak <= scales[i])
            return scales[i];
    }
    return 1.0;
}

static void scale_label(GtkWidget *widget, cairo_t *cairo, double y, double pct, double valign) {
    char text[16];
    g_snprintf(text, sizeof(text), pct > 0 && pct < 1 ? "%.1f %%" : "%.0f %%", pct);
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    double ty = CLAMP(y - th * valign, 0, MAX(height - th, 0));
    gtk_render_layout(gtk_widget_get_style_context(widget), cairo, width - tw - 4, ty, layout);
    g_object_unref(layout);
}

/* How many samples the graph shows (2px each). */
static int graph_samples(const adapter *net_adapter) {
    return gtk_widget_get_allocated_width(net_adapter->area) / 2 + 2;
}

/* The scale left of the graph: top, middle and 0. */
static gboolean draw_scale(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    const adapter *net_adapter = data;
    double full = graph_scale(net_adapter, graph_samples(net_adapter));
    int height = gtk_widget_get_allocated_height(widget);
    scale_label(widget, cairo, 0, full * 100, 0);
    scale_label(widget, cairo, height / 2.0, full * 50, 0.5);
    scale_label(widget, cairo, height, 0, 1);
    return TRUE;
}

static gboolean draw_graph(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    const adapter *net_adapter = data;
    double x, y, wd, ht;
    xd_tm_draw_well(widget, cairo, &x, &y, &wd, &ht);
    xd_history_draw_scaled(cairo, x, y, wd, ht, &net_adapter->hist, FALSE,
                           graph_scale(net_adapter, graph_samples(net_adapter)));
    return TRUE;
}

static adapter *adapter_new(xd_net_tab *net_tab, const char *name) {
    adapter *net_adapter = g_new0(adapter, 1);
    net_adapter->name = g_strdup(name);
    xd_history_init(&net_adapter->hist, HISTORY);
    net_adapter->scale = gtk_drawing_area_new();
    gtk_widget_set_size_request(net_adapter->scale, SCALE_W, -1);
    g_signal_connect(net_adapter->scale, "draw", G_CALLBACK(draw_scale), net_adapter);
    net_adapter->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(net_adapter->area, 120, 70);
    gtk_widget_set_hexpand(net_adapter->area, TRUE);
    gtk_widget_set_vexpand(net_adapter->area, TRUE);
    g_signal_connect(net_adapter->area, "draw", G_CALLBACK(draw_graph), net_adapter);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(row), net_adapter->scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), net_adapter->area, TRUE, TRUE, 0);
    GtkWidget *frame = xd_tm_group(name, row);
    gtk_box_pack_start(GTK_BOX(net_tab->graphs), frame, TRUE, TRUE, 0);
    gtk_widget_show_all(frame);
    return net_adapter;
}

/* Same adapters as last time, in the same order? */
static gboolean same_adapters(const xd_net_tab *net_tab, const GArray *ifs) {
    if (ifs->len != net_tab->adapters->len)
        return FALSE;
    for (guint i = 0; i < ifs->len; i++) {
        const adapter *net_adapter = g_ptr_array_index(net_tab->adapters, i);
        if (strcmp(net_adapter->name, g_array_index(ifs, xd_netif, i).name) != 0)
            return FALSE;
    }
    return TRUE;
}

static void rebuild(xd_net_tab *net_tab, const GArray *ifs) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(net_tab->graphs));
    for (GList *link = kids; link != nullptr; link = link->next)
        gtk_widget_destroy(link->data);
    g_list_free(kids);
    g_ptr_array_set_size(net_tab->adapters, 0);
    gtk_list_store_clear(net_tab->store);
    for (guint i = 0; i < ifs->len; i++) {
        g_ptr_array_add(net_tab->adapters,
                        adapter_new(net_tab, g_array_index(ifs, xd_netif, i).name));
        gtk_list_store_insert_with_values(net_tab->store, nullptr, -1, NC_NAME,
                                          g_array_index(ifs, xd_netif, i).name, -1);
    }
}

void xd_net_tab_sample(xd_net_tab *net_tab, gboolean visible) {
    GArray *ifs = xd_netifs_read("/sys/class/net");
    gint64 now = g_get_monotonic_time();
    double seconds =
        net_tab->last_time > 0 ? (double)(now - net_tab->last_time) / G_USEC_PER_SEC : 0;
    net_tab->last_time = now;
    if (!same_adapters(net_tab, ifs))
        rebuild(net_tab, ifs);
    GtkTreeIter iter;
    gboolean row = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(net_tab->store), &iter);
    for (guint i = 0; i < ifs->len && row; i++) {
        const xd_netif *nif = &g_array_index(ifs, xd_netif, i);
        adapter *net_adapter = g_ptr_array_index(net_tab->adapters, i);
        guint64 bytes = nif->rx_bytes + nif->tx_bytes;
        /* Counters restart when a driver reloads. */
        guint64 delta = net_adapter->have_last && bytes >= net_adapter->last_bytes
                            ? bytes - net_adapter->last_bytes
                            : 0;
        double share =
            net_adapter->have_last ? xd_netif_utilization(delta, seconds, nif->speed_mbps) : 0;
        net_adapter->last_bytes = bytes;
        net_adapter->have_last = TRUE;
        xd_history_push(&net_adapter->hist, share, 0);
        char *util = nif->speed_mbps > 0 ? xd_netif_utilization_text(share) : g_strdup("");
        char *speed = xd_netif_speed_text(nif->speed_mbps);
        gtk_list_store_set(net_tab->store, &iter, NC_UTIL, util, NC_SPEED, speed, NC_STATE,
                           xd_netif_state_text(nif->state), -1);
        g_free(speed);
        g_free(util);
        if (visible) {
            gtk_widget_queue_draw(net_adapter->scale);
            gtk_widget_queue_draw(net_adapter->area);
        }
        row = gtk_tree_model_iter_next(GTK_TREE_MODEL(net_tab->store), &iter);
    }
    xd_netifs_free(ifs);
}

static GtkTreeViewColumn *text_column(GtkWidget *tree, const char *title, int col, int width,
                                      double xalign) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", xalign, nullptr);
    GtkTreeViewColumn *column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", col, nullptr);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, width);
    gtk_tree_view_column_set_sort_column_id(column, col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    return column;
}

xd_net_tab *xd_net_tab_new(void) {
    xd_net_tab *net_tab = g_new0(xd_net_tab, 1);
    net_tab->adapters = g_ptr_array_new_with_free_func(adapter_free);
    net_tab->box = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_container_set_border_width(GTK_CONTAINER(net_tab->box), 8);
    net_tab->graphs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), net_tab->graphs);
    gtk_widget_set_size_request(sw, -1, 150);
    gtk_paned_pack1(GTK_PANED(net_tab->box), sw, TRUE, FALSE);

    net_tab->store =
        gtk_list_store_new(NC_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    net_tab->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(net_tab->store));
    text_column(net_tab->list, "Adapter Name", NC_NAME, 115, 0);
    text_column(net_tab->list, "Network Utilization", NC_UTIL, 115, 1);
    text_column(net_tab->list, "Link Speed", NC_SPEED, 75, 1);
    text_column(net_tab->list, "State", NC_STATE, 90, 0);
    GtkWidget *list = xd_tm_scrolled(net_tab->list);
    gtk_widget_set_size_request(list, -1, 80);
    gtk_widget_set_margin_top(list, 6);
    gtk_paned_pack2(GTK_PANED(net_tab->box), list, FALSE, FALSE);
    return net_tab;
}

GtkWidget *xd_net_tab_widget(xd_net_tab *net_tab) { return net_tab->box; }

void xd_net_tab_free(xd_net_tab *net_tab) {
    if (net_tab == nullptr)
        return;
    g_ptr_array_unref(net_tab->adapters);
    g_object_unref(net_tab->store);
    g_free(net_tab);
}
