/* SPDX-License-Identifier: MIT
 * xd-files: the file manager, modelled on Windows XP's Explorer windows.
 *
 * GTK3 window: menu bar, toolbar (Back, Forward, Up, Folders, Views),
 * address bar, the Folders tree on the left, XdFolderView on the right and
 * a status bar with the thumbnail size slider.
 */
#include <gtk/gtk.h>
#include <stdlib.h>

#include "folderview.h"

enum { COL_NAME, COL_ICON, COL_FILE, COL_LOADED, COL_COUNT };

static constexpr GdkModifierType NO_MODS = (GdkModifierType)0;

typedef struct explorer {
    GtkWidget *window;
    XdFolderView *view;
    GtkWidget *address;
    GtkWidget *tree;
    GtkTreeStore *store;
    GtkWidget *tree_pane;
    GtkWidget *status;
    GtkWidget *status_size;
    GtkWidget *slider;
    GtkWidget *back_button, *forward_button, *up_button;
    GtkWidget *folders_toggle;
    GtkWidget *view_items[XD_VIEW_COUNT];
    GList *back;    /* GFile*, most recent first */
    GList *forward;
    gboolean syncing; /* updating widgets programmatically */
} explorer;

static void navigate(explorer *e, GFile *dir, gboolean record);
static int initial_view = XD_VIEW_ICONS; /* from --view */

/* ---- status and chrome --------------------------------------------------------- */

static void update_status(explorer *e) {
    guint sel = xd_folder_view_get_selected_count(e->view);
    guint total = xd_folder_view_get_item_count(e->view);
    char *text = sel > 0 ? g_strdup_printf("%u object%s selected", sel, sel == 1 ? "" : "s")
                         : g_strdup_printf("%u object%s", total, total == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(e->status), text);
    g_free(text);
    char *size = xd_format_size(xd_folder_view_get_size(e->view, sel > 0));
    gtk_label_set_text(GTK_LABEL(e->status_size), size);
    g_free(size);
}

static void update_chrome(explorer *e) {
    GFile *dir = xd_folder_view_get_location(e->view);
    if (dir == nullptr)
        return; /* nothing loaded yet */
    char *parse = g_file_get_parse_name(dir);
    e->syncing = TRUE;
    gtk_entry_set_text(GTK_ENTRY(e->address), parse);
    e->syncing = FALSE;
    char *base = g_file_get_basename(dir);
    gtk_window_set_title(GTK_WINDOW(e->window),
                         g_file_has_parent(dir, nullptr) && base != nullptr ? base : parse);
    g_free(base);
    g_free(parse);
    gtk_widget_set_sensitive(e->back_button, e->back != nullptr);
    gtk_widget_set_sensitive(e->forward_button, e->forward != nullptr);
    gtk_widget_set_sensitive(e->up_button, g_file_has_parent(dir, nullptr));
    gtk_widget_set_visible(e->slider, xd_folder_view_get_mode(e->view) == XD_VIEW_THUMBNAILS);
}

/* ---- Folders tree ---------------------------------------------------------------- */

static void add_placeholder(explorer *e, GtkTreeIter *parent) {
    GtkTreeIter child;
    gtk_tree_store_append(e->store, &child, parent);
    gtk_tree_store_set(e->store, &child, COL_NAME, "", -1);
}

static void add_folder(explorer *e, GtkTreeIter *parent, GtkTreeIter *out, GFile *file,
                       const char *name, const char *icon) {
    gtk_tree_store_append(e->store, out, parent);
    gtk_tree_store_set(e->store, out, COL_NAME, name, COL_ICON, icon, COL_FILE, file, COL_LOADED,
                       FALSE, -1);
    add_placeholder(e, out); /* shows the expander until loaded */
}

/* Fills a node's children on first expansion (synchronous; folders only). */
static void load_children(explorer *e, GtkTreeIter *iter) {
    gboolean loaded = FALSE;
    GFile *dir = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(e->store), iter, COL_LOADED, &loaded, COL_FILE, &dir, -1);
    if (loaded || dir == nullptr) {
        g_clear_object(&dir);
        return;
    }
    gtk_tree_store_set(e->store, iter, COL_LOADED, TRUE, -1);
    GtkTreeIter child;
    while (gtk_tree_model_iter_children(GTK_TREE_MODEL(e->store), &child, iter))
        gtk_tree_store_remove(e->store, &child);

    GFileEnumerator *en = g_file_enumerate_children(
        dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME
             "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    if (en != nullptr) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(en, nullptr, nullptr)) != nullptr) {
            if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY &&
                !g_file_info_get_is_hidden(info))
                g_ptr_array_add(names, g_strdup(g_file_info_get_name(info)));
            g_object_unref(info);
        }
        g_object_unref(en);
        g_ptr_array_sort_values(names, (GCompareFunc)g_utf8_collate);
        for (guint i = 0; i < names->len; i++) {
            const char *name = g_ptr_array_index(names, i);
            GFile *child_file = g_file_get_child(dir, name);
            char *display = g_filename_display_name(name);
            add_folder(e, iter, &child, child_file, display, "folder");
            g_free(display);
            g_object_unref(child_file);
        }
        g_ptr_array_unref(names);
    }
    g_object_unref(dir);
}

static void on_row_expand([[maybe_unused]] GtkTreeView *tv, GtkTreeIter *iter,
                          [[maybe_unused]] GtkTreePath *path, gpointer data) {
    load_children(data, iter);
}

static void on_tree_selection(GtkTreeSelection *sel, gpointer data) {
    explorer *e = data;
    if (e->syncing)
        return;
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    GFile *file = nullptr;
    gtk_tree_model_get(model, &iter, COL_FILE, &file, -1);
    if (file != nullptr) {
        if (!g_file_equal(file, xd_folder_view_get_location(e->view)))
            navigate(e, file, TRUE);
        g_object_unref(file);
    }
}

static void build_tree_roots(explorer *e) {
    GtkTreeIter it;
    GFile *home = g_file_new_for_path(g_get_home_dir());
    add_folder(e, nullptr, &it, home, "Home", "user-home");
    g_object_unref(home);
    const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    if (docs != nullptr) {
        GFile *f = g_file_new_for_path(docs);
        add_folder(e, nullptr, &it, f, "My Documents", "folder-documents");
        g_object_unref(f);
    }
    const char *desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (desktop != nullptr) {
        GFile *f = g_file_new_for_path(desktop);
        add_folder(e, nullptr, &it, f, "Desktop", "user-desktop");
        g_object_unref(f);
    }
    GFile *root = g_file_new_for_path("/");
    add_folder(e, nullptr, &it, root, "File System", "drive-harddisk");
    g_object_unref(root);
}

/* Selects the current folder in the tree if it's an already-loaded node. */
static gboolean select_matching(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                gpointer data) {
    explorer *e = data;
    GFile *file = nullptr;
    gtk_tree_model_get(model, iter, COL_FILE, &file, -1);
    gboolean match = file != nullptr && g_file_equal(file, xd_folder_view_get_location(e->view));
    g_clear_object(&file);
    if (match) {
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(e->tree), path);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(e->tree)), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(e->tree), path, nullptr, FALSE, 0, 0);
    }
    return match;
}

static void sync_tree(explorer *e) {
    e->syncing = TRUE;
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(e->tree)));
    gtk_tree_model_foreach(GTK_TREE_MODEL(e->store), select_matching, e);
    e->syncing = FALSE;
}

/* ---- navigation ------------------------------------------------------------------ */

static void free_history(GList **list) {
    g_list_free_full(*list, g_object_unref);
    *list = nullptr;
}

static void navigate(explorer *e, GFile *dir, gboolean record) {
    GFile *current = xd_folder_view_get_location(e->view);
    if (record && current != nullptr && !g_file_equal(current, dir)) {
        e->back = g_list_prepend(e->back, g_object_ref(current));
        free_history(&e->forward);
    }
    xd_folder_view_set_location(e->view, dir);
    update_chrome(e);
    sync_tree(e);
}

static void go_back(explorer *e) {
    if (e->back == nullptr)
        return;
    GFile *target = e->back->data;
    e->back = g_list_delete_link(e->back, e->back);
    e->forward = g_list_prepend(e->forward, g_object_ref(xd_folder_view_get_location(e->view)));
    navigate(e, target, FALSE);
    g_object_unref(target);
}

static void go_forward(explorer *e) {
    if (e->forward == nullptr)
        return;
    GFile *target = e->forward->data;
    e->forward = g_list_delete_link(e->forward, e->forward);
    e->back = g_list_prepend(e->back, g_object_ref(xd_folder_view_get_location(e->view)));
    navigate(e, target, FALSE);
    g_object_unref(target);
}

static void go_up(explorer *e) {
    GFile *parent = g_file_get_parent(xd_folder_view_get_location(e->view));
    if (parent != nullptr) {
        navigate(e, parent, TRUE);
        g_object_unref(parent);
    }
}

static void go_home(explorer *e) {
    GFile *home = g_file_new_for_path(g_get_home_dir());
    navigate(e, home, TRUE);
    g_object_unref(home);
}

static void on_activated([[maybe_unused]] XdFolderView *view, GFile *file, gboolean is_dir,
                         gpointer data) {
    explorer *e = data;
    if (is_dir) {
        navigate(e, file, TRUE);
        return;
    }
    /* Open with the file type's default application. */
    char *uri = g_file_get_uri(file);
    GError *err = nullptr;
    if (!g_app_info_launch_default_for_uri(uri, nullptr, &err)) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(e->window), GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                                err != nullptr ? err->message : "Cannot open file");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_clear_error(&err);
    }
    g_free(uri);
}

static void on_address_activate(GtkEntry *entry, gpointer data) {
    explorer *e = data;
    GFile *f = g_file_parse_name(gtk_entry_get_text(entry));
    GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, nullptr);
    if (type == G_FILE_TYPE_DIRECTORY) {
        navigate(e, f, TRUE);
        gtk_widget_grab_focus(GTK_WIDGET(e->view));
    } else if (type != G_FILE_TYPE_UNKNOWN) {
        on_activated(e->view, f, FALSE, e);
    } else {
        /* Windows' message for a bad address. */
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(e->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Cannot find '%s'. Make sure the path is correct and try again.",
            gtk_entry_get_text(entry));
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
    g_object_unref(f);
}

static void on_load_error([[maybe_unused]] XdFolderView *view, const char *message,
                          gpointer data) {
    explorer *e = data;
    gtk_label_set_text(GTK_LABEL(e->status), message);
}

static void on_contents([[maybe_unused]] XdFolderView *view, gpointer data) {
    update_status(data);
}

static void on_selection([[maybe_unused]] XdFolderView *view, gpointer data) {
    update_status(data);
}

/* ---- view menu and toolbar ------------------------------------------------------ */

static const char *const view_names[XD_VIEW_COUNT] = {"Thumbnails", "Tiles", "Icons", "List",
                                                      "Details"};

static void set_mode(explorer *e, XdViewMode mode) {
    xd_folder_view_set_mode(e->view, mode);
    e->syncing = TRUE;
    for (int i = 0; i < XD_VIEW_COUNT; i++)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(e->view_items[i]), i == (int)mode);
    e->syncing = FALSE;
    update_chrome(e);
}

static void on_view_item(GtkCheckMenuItem *item, gpointer data) {
    explorer *e = data;
    if (e->syncing || !gtk_check_menu_item_get_active(item))
        return;
    int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-mode"));
    set_mode(e, (XdViewMode)mode);
}

static void on_slider(GtkRange *range, gpointer data) {
    explorer *e = data;
    double px = gtk_range_get_value(range);
    xd_folder_view_set_thumbnail_size(e->view, (int)px);
}

static void on_folders_toggle(GtkToggleToolButton *b, gpointer data) {
    explorer *e = data;
    gtk_widget_set_visible(e->tree_pane, gtk_toggle_tool_button_get_active(b));
}

static void on_hidden(GtkCheckMenuItem *item, gpointer data) {
    explorer *e = data;
    xd_folder_view_set_show_hidden(e->view, gtk_check_menu_item_get_active(item));
}

static void cb_back([[maybe_unused]] GtkWidget *w, gpointer d) { go_back(d); }
static void cb_forward([[maybe_unused]] GtkWidget *w, gpointer d) { go_forward(d); }
static void cb_up([[maybe_unused]] GtkWidget *w, gpointer d) { go_up(d); }
static void cb_home([[maybe_unused]] GtkWidget *w, gpointer d) { go_home(d); }
static void cb_refresh([[maybe_unused]] GtkWidget *w, gpointer d) {
    xd_folder_view_reload(((explorer *)d)->view);
}
static void cb_select_all([[maybe_unused]] GtkWidget *w, gpointer d) {
    xd_folder_view_select_all(((explorer *)d)->view);
}
static void cb_invert([[maybe_unused]] GtkWidget *w, gpointer d) {
    xd_folder_view_invert_selection(((explorer *)d)->view);
}
static void cb_close([[maybe_unused]] GtkWidget *w, gpointer d) {
    gtk_widget_destroy(((explorer *)d)->window);
}
static void cb_go([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    on_address_activate(GTK_ENTRY(e->address), e);
}
static void cb_about([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    gtk_show_about_dialog(GTK_WINDOW(e->window), "program-name", "XeonDesktop Files",
                          "comments", "The XeonDesktop file manager.", "license-type",
                          GTK_LICENSE_MIT_X11, "logo-icon-name", "system-file-manager", nullptr);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, explorer *e,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    if (cb != nullptr)
        g_signal_connect(item, "activate", cb, e);
    else
        gtk_widget_set_sensitive(item, FALSE);
    if (accel != nullptr && key != 0)
        gtk_widget_add_accelerator(item, "activate", accel, key, mods, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static GtkWidget *submenu(GtkWidget *bar, const char *label) {
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    return menu;
}

static GtkWidget *build_menus(explorer *e, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    menu_item(file, "_Close", G_CALLBACK(cb_close), e, accel, GDK_KEY_w, GDK_CONTROL_MASK);

    GtkWidget *edit = submenu(bar, "_Edit");
    menu_item(edit, "Cu_t", nullptr, e, nullptr, 0, NO_MODS);
    menu_item(edit, "_Copy", nullptr, e, nullptr, 0, NO_MODS);
    menu_item(edit, "_Paste", nullptr, e, nullptr, 0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit), gtk_separator_menu_item_new());
    menu_item(edit, "Select _All", G_CALLBACK(cb_select_all), e, accel, GDK_KEY_a,
              GDK_CONTROL_MASK);
    menu_item(edit, "_Invert Selection", G_CALLBACK(cb_invert), e, nullptr, 0, NO_MODS);

    GtkWidget *view = submenu(bar, "_View");
    GSList *group = nullptr;
    for (int i = 0; i < XD_VIEW_COUNT; i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(group, view_names[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "xd-mode", GINT_TO_POINTER(i));
        g_signal_connect(item, "toggled", G_CALLBACK(on_view_item), e);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), item);
        e->view_items[i] = item;
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(view), gtk_separator_menu_item_new());
    GtkWidget *hidden = gtk_check_menu_item_new_with_mnemonic("Show _Hidden Files");
    g_signal_connect(hidden, "toggled", G_CALLBACK(on_hidden), e);
    gtk_widget_add_accelerator(hidden, "activate", accel, GDK_KEY_h, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), hidden);
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), e, accel, GDK_KEY_F5, NO_MODS);

    GtkWidget *go = submenu(bar, "_Go");
    menu_item(go, "_Back", G_CALLBACK(cb_back), e, accel, GDK_KEY_Left, GDK_MOD1_MASK);
    menu_item(go, "_Forward", G_CALLBACK(cb_forward), e, accel, GDK_KEY_Right, GDK_MOD1_MASK);
    menu_item(go, "_Up One Level", G_CALLBACK(cb_up), e, accel, GDK_KEY_Up, GDK_MOD1_MASK);
    menu_item(go, "_Home Folder", G_CALLBACK(cb_home), e, accel, GDK_KEY_Home, GDK_MOD1_MASK);

    GtkWidget *help = submenu(bar, "_Help");
    menu_item(help, "_About XeonDesktop Files", G_CALLBACK(cb_about), e, nullptr, 0, NO_MODS);
    return bar;
}

static GtkWidget *tool_button(GtkWidget *toolbar, const char *icon, const char *label,
                              GCallback cb, explorer *e) {
    GtkToolItem *b = gtk_tool_button_new(nullptr, label);
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(b), icon);
    gtk_tool_item_set_is_important(b, TRUE); /* XP shows Back's label */
    gtk_tool_item_set_tooltip_text(b, label);
    g_signal_connect(b, "clicked", cb, e);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), b, -1);
    return GTK_WIDGET(b);
}

static void popup_views(GtkWidget *button, gpointer data) {
    explorer *e = data;
    GtkWidget *menu = gtk_menu_new();
    for (int i = 0; i < XD_VIEW_COUNT; i++) {
        GtkWidget *item = gtk_menu_item_new_with_label(view_names[i]);
        g_object_set_data(G_OBJECT(item), "xd-mode", GINT_TO_POINTER(i));
        g_signal_connect_swapped(item, "activate", G_CALLBACK(gtk_check_menu_item_set_active),
                                 e->view_items[i]);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), button, GDK_GRAVITY_SOUTH_WEST,
                             GDK_GRAVITY_NORTH_WEST, nullptr);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    explorer *e = data;
    /* Backspace goes up, as in Windows, unless typing in the address bar. */
    if (ev->keyval == GDK_KEY_BackSpace && !gtk_widget_has_focus(e->address)) {
        go_up(e);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_l && (ev->state & GDK_CONTROL_MASK)) {
        gtk_widget_grab_focus(e->address);
        return TRUE;
    }
    (void)w;
    return FALSE;
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    explorer *e = data;
    free_history(&e->back);
    free_history(&e->forward);
    g_free(e);
}

static void open_window(GtkApplication *app, GFile *dir) {
    explorer *e = g_new0(explorer, 1);
    e->window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(e->window), 820, 560);
    gtk_window_set_icon_name(GTK_WINDOW(e->window), "system-file-manager");
    g_signal_connect(e->window, "destroy", G_CALLBACK(on_destroy), e);
    g_signal_connect(e->window, "key-press-event", G_CALLBACK(on_key), e);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(e->window), accel);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(e->window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), build_menus(e, accel), FALSE, FALSE, 0);

    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_BOTH_HORIZ);
    e->back_button = tool_button(toolbar, "go-previous", "Back", G_CALLBACK(cb_back), e);
    e->forward_button = tool_button(toolbar, "go-next", "Forward", G_CALLBACK(cb_forward), e);
    e->up_button = tool_button(toolbar, "go-up", "Up", G_CALLBACK(cb_up), e);
    gtk_tool_item_set_is_important(GTK_TOOL_ITEM(e->forward_button), FALSE);
    gtk_tool_item_set_is_important(GTK_TOOL_ITEM(e->up_button), FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
    GtkToolItem *search = gtk_tool_button_new(nullptr, "Search");
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(search), "system-search");
    gtk_tool_item_set_is_important(search, TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(search), FALSE); /* comes later */
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), search, -1);
    GtkToolItem *folders = gtk_toggle_tool_button_new();
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(folders), "Folders");
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(folders), "folder");
    gtk_tool_item_set_is_important(folders, TRUE);
    gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(folders), TRUE);
    g_signal_connect(folders, "toggled", G_CALLBACK(on_folders_toggle), e);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), folders, -1);
    e->folders_toggle = GTK_WIDGET(folders);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
    GtkToolItem *views = gtk_tool_button_new(nullptr, "Views");
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(views), "view-grid-symbolic");
    gtk_tool_item_set_is_important(views, TRUE);
    g_signal_connect(views, "clicked", G_CALLBACK(popup_views), e);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), views, -1);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    /* Address bar. */
    GtkWidget *abar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(abar), 2);
    gtk_box_pack_start(GTK_BOX(abar), gtk_label_new("Address"), FALSE, FALSE, 4);
    e->address = gtk_entry_new();
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(e->address), GTK_ENTRY_ICON_PRIMARY, "folder");
    g_signal_connect(e->address, "activate", G_CALLBACK(on_address_activate), e);
    gtk_box_pack_start(GTK_BOX(abar), e->address, TRUE, TRUE, 0);
    GtkWidget *go = gtk_button_new_with_label("Go");
    gtk_button_set_image(GTK_BUTTON(go), gtk_image_new_from_icon_name("go-jump", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(go), TRUE);
    g_signal_connect(go, "clicked", G_CALLBACK(cb_go), e);
    gtk_box_pack_start(GTK_BOX(abar), go, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), abar, FALSE, FALSE, 0);

    /* Folders tree | contents. */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);
    e->store = gtk_tree_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_FILE,
                                  G_TYPE_BOOLEAN);
    e->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(e->store));
    g_object_unref(e->store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(e->tree), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", COL_ICON);
    gtk_tree_view_column_add_attribute(col, text, "text", COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(e->tree), col);
    g_signal_connect(e->tree, "test-expand-row", G_CALLBACK(on_row_expand), e);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(e->tree)), "changed",
                     G_CALLBACK(on_tree_selection), e);
    build_tree_roots(e);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *title = gtk_label_new("Folders");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_margin_start(title, 6);
    gtk_widget_set_margin_top(title, 3);
    gtk_widget_set_margin_bottom(title, 3);
    gtk_box_pack_start(GTK_BOX(left), title, FALSE, FALSE, 0);
    GtkWidget *tree_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(tree_scroll), e->tree);
    gtk_box_pack_start(GTK_BOX(left), tree_scroll, TRUE, TRUE, 0);
    gtk_widget_set_size_request(left, 200, -1);
    e->tree_pane = left;
    gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);

    e->view = XD_FOLDER_VIEW(xd_folder_view_new());
    g_signal_connect(e->view, "item-activated", G_CALLBACK(on_activated), e);
    g_signal_connect(e->view, "contents-changed", G_CALLBACK(on_contents), e);
    g_signal_connect(e->view, "selection-changed", G_CALLBACK(on_selection), e);
    g_signal_connect(e->view, "load-error", G_CALLBACK(on_load_error), e);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(e->view));
    gtk_paned_pack2(GTK_PANED(paned), scroll, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 210);

    /* Status bar: object count, size, thumbnail slider. */
    GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(sbar), 2);
    e->status = gtk_label_new("");
    gtk_widget_set_halign(e->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sbar), e->status, TRUE, TRUE, 4);
    e->slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, (double)XD_THUMB_MIN,
                                         (double)XD_THUMB_MAX, 8.0);
    gtk_scale_set_draw_value(GTK_SCALE(e->slider), FALSE);
    gtk_range_set_value(GTK_RANGE(e->slider), (double)XD_THUMB_DEFAULT);
    gtk_widget_set_size_request(e->slider, 140, -1);
    gtk_widget_set_tooltip_text(e->slider, "Thumbnail size");
    gtk_widget_set_no_show_all(e->slider, TRUE);
    g_signal_connect(e->slider, "value-changed", G_CALLBACK(on_slider), e);
    gtk_box_pack_end(GTK_BOX(sbar), e->slider, FALSE, FALSE, 4);
    e->status_size = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(sbar), e->status_size, FALSE, FALSE, 8);
    gtk_box_pack_end(GTK_BOX(vbox), sbar, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 0);

    gtk_widget_show_all(e->window);
    set_mode(e, (XdViewMode)initial_view);
    navigate(e, dir, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(e->view));
}

static void on_activate(GtkApplication *app, [[maybe_unused]] gpointer data) {
    GFile *home = g_file_new_for_path(g_get_home_dir());
    open_window(app, home);
    g_object_unref(home);
}

static void on_open(GtkApplication *app, GFile **files, gint n, [[maybe_unused]] const char *hint,
                    [[maybe_unused]] gpointer data) {
    for (gint i = 0; i < n; i++)
        open_window(app, files[i]);
}

/* --view=thumbnails|tiles|icons|list|details */
static gint on_options([[maybe_unused]] GApplication *app, GVariantDict *opts,
                       [[maybe_unused]] gpointer data) {
    const char *view = nullptr;
    if (g_variant_dict_lookup(opts, "view", "&s", &view)) {
        for (int i = 0; i < XD_VIEW_COUNT; i++) {
            if (g_ascii_strcasecmp(view, view_names[i]) == 0) {
                initial_view = i;
                return -1;
            }
        }
        g_printerr("xd-files: unknown view '%s'\n", view);
        return EXIT_FAILURE;
    }
    return -1; /* continue */
}

int main(int argc, char **argv) {
    /* Each window is its own process, like Windows' "separate process"
     * option: a hang in one folder can't freeze the others. */
    GtkApplication *app = gtk_application_new("org.xeondesktop.Files",
                                              G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    g_signal_connect(app, "open", G_CALLBACK(on_open), nullptr);
    g_application_add_main_option(G_APPLICATION(app), "view", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_STRING, "Thumbnails, Tiles, Icons, List or Details",
                                  "VIEW");
    g_signal_connect(app, "handle-local-options", G_CALLBACK(on_options), nullptr);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
