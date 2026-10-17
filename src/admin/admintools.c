/* SPDX-License-Identifier: MIT
 * xd-admintools: Windows XP's Administrative Tools folder (Control Panel
 * > Performance and Maintenance): shortcuts to the system tools, each its
 * own program. A tool lists itself with `X-XeonDesktop-AdminTool=true` in
 * its desktop entry; Device Manager is System Properties' action.
 */
#include <gio/gdesktopappinfo.h>
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"

#define ADMIN_TOOL_KEY "X-XeonDesktop-AdminTool"

enum { COL_ICON, COL_NAME, COL_COMMENT, COL_INFO, COL_ACTION, NCOLS };

typedef struct admintools {
    GtkWidget *window, *view, *status;
    GtkListStore *store;
} admintools;

static void add_tool(admintools *a, GDesktopAppInfo *info, const char *action, const char *name,
                     const char *comment) {
    GIcon *icon = g_app_info_get_icon(G_APP_INFO(info));
    if (icon != nullptr)
        g_object_ref(icon);
    else
        icon = g_themed_icon_new("applications-system");
    gtk_list_store_insert_with_values(a->store, nullptr, -1, COL_ICON, icon, COL_NAME, name,
                                      COL_COMMENT, comment != nullptr ? comment : "", COL_INFO,
                                      info, COL_ACTION, action, -1);
    g_object_unref(icon);
}

static void fill(admintools *a) {
    gtk_list_store_clear(a->store);
    GList *all = g_app_info_get_all();
    for (GList *l = all; l != nullptr; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data))
            continue;
        GDesktopAppInfo *info = l->data;
        if (!g_desktop_app_info_get_boolean(info, ADMIN_TOOL_KEY))
            continue;
        add_tool(a, info, nullptr, g_app_info_get_name(G_APP_INFO(info)),
                 g_app_info_get_description(G_APP_INFO(info)));
    }
    g_list_free_full(all, g_object_unref);
    /* Device Manager: System Properties' desktop action. */
    GDesktopAppInfo *sysdm = g_desktop_app_info_new("org.xeondesktop.SystemProperties.desktop");
    if (sysdm != nullptr && g_desktop_app_info_has_key(sysdm, "Actions")) {
        const char *const *actions = g_desktop_app_info_list_actions(sysdm);
        if (actions != nullptr && g_strv_contains(actions, "DeviceManager"))
            add_tool(a, sysdm, "DeviceManager", "Device Manager",
                     "Lists the hardware in this computer and shows each device's properties");
    }
    g_clear_object(&sysdm);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(a->store), COL_NAME,
                                         GTK_SORT_ASCENDING);
    int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(a->store), nullptr);
    char *text = g_strdup_printf("%d object%s", n, n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(a->status), text);
    g_free(text);
}

static void open_path(admintools *a, GtkTreePath *path) {
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(a->store), &it, path))
        return;
    GDesktopAppInfo *info = nullptr;
    char *action = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(a->store), &it, COL_INFO, &info, COL_ACTION, &action, -1);
    GdkAppLaunchContext *ctx =
        gdk_display_get_app_launch_context(gtk_widget_get_display(a->window));
    if (action != nullptr) {
        g_desktop_app_info_launch_action(info, action, G_APP_LAUNCH_CONTEXT(ctx));
    } else {
        GError *error = nullptr;
        if (!g_app_info_launch(G_APP_INFO(info), nullptr, G_APP_LAUNCH_CONTEXT(ctx), &error)) {
            xd_message_box(GTK_WINDOW(a->window), "Administrative Tools", XD_MB_ERROR, XD_MB_OK,
                           FALSE, error->message);
            g_error_free(error);
        }
    }
    g_object_unref(ctx);
    g_object_unref(info);
    g_free(action);
}

static void on_activated([[maybe_unused]] GtkIconView *v, GtkTreePath *path, gpointer data) {
    open_path(data, path);
}

static void on_selection(GtkIconView *v, gpointer data) {
    admintools *a = data;
    GList *sel = gtk_icon_view_get_selected_items(v);
    if (sel != nullptr) {
        GtkTreeIter it;
        char *comment = nullptr;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(a->store), &it, sel->data))
            gtk_tree_model_get(GTK_TREE_MODEL(a->store), &it, COL_COMMENT, &comment, -1);
        gtk_label_set_text(GTK_LABEL(a->status), comment != nullptr ? comment : "");
        g_free(comment);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    admintools *a = data;
    if (ev->keyval == GDK_KEY_F5) {
        fill(a);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_widget_destroy(w);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.AdministrativeTools");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static admintools a;
    a.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a.window), "Administrative Tools");
    gtk_window_set_icon_name(GTK_WINDOW(a.window), "preferences-system");
    gtk_window_set_default_size(GTK_WINDOW(a.window), 560, 360);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(a.window), vbox);

    a.store = gtk_list_store_new(NCOLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_DESKTOP_APP_INFO, G_TYPE_STRING);
    /* XP's Tiles view: a big icon with its name beside it. */
    a.view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(a.store));
    g_object_unref(a.store);
    gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(a.view), GTK_ORIENTATION_HORIZONTAL);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(a.view), 220);
    gtk_icon_view_set_activate_on_single_click(GTK_ICON_VIEW(a.view), FALSE);
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "stock-size", GTK_ICON_SIZE_DIALOG, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(a.view), icon, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(a.view), icon, "gicon", COL_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "xalign", 0.0, "wrap-mode", PANGO_WRAP_WORD, "wrap-width", 150, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(a.view), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(a.view), text, "text", COL_NAME);
    g_signal_connect(a.view, "item-activated", G_CALLBACK(on_activated), &a);
    g_signal_connect(a.view, "selection-changed", G_CALLBACK(on_selection), &a);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), a.view);
    gtk_box_pack_start(GTK_BOX(vbox), sc, TRUE, TRUE, 0);
    a.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a.status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(a.status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(a.status, 6);
    gtk_widget_set_margin_top(a.status, 2);
    gtk_widget_set_margin_bottom(a.status, 2);
    gtk_box_pack_start(GTK_BOX(vbox), a.status, FALSE, FALSE, 0);
    fill(&a);
    g_signal_connect(a.window, "key-press-event", G_CALLBACK(on_key), &a);
    g_signal_connect(a.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(a.window);
    gtk_main();
    return EXIT_SUCCESS;
}
