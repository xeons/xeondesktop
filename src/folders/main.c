/* SPDX-License-Identifier: MIT
 * xd-folders: Windows XP's Folder Options. The settings go to [Folders]
 * in desktop.ini, which xd-files follows live; file associations are
 * GIO's (mimeapps.list).
 *
 *   General     XP's task pane in folders or classic folders; open
 *               folders in the same or their own window (or a new tab,
 *               xd-files' own); single- or double-click to open
 *   View        Apply to All Folders / Reset All Folders, and the
 *               advanced settings tree
 *   Search      Windows 7's Search tab, for xd-files' search box (in the
 *               2000 and XP styles only its Linux additions: /pattern/
 *               and the colour matches are highlighted in)
 *   File Types  shared-mime-info's types by extension, and the program
 *               each opens with (Change... sets the default); New and
 *               Delete change the user's MIME database (filetypes.h)
 *
 * --view=VIEW is the view of the folder it was opened from (xd-files'
 * Tools menu), for Apply to All Folders; without it, the button is
 * greyed, as in XP's Control Panel.
 */
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "../settings/state.h"
#include "filetypes.h"

#define TITLE "Folder Options"
/* xd-files' state file of each folder's view. */
#define FOLDER_VIEWS "folder-views"

static const char *const view_names[] = {"thumbnails", "tiles", "icons", "list", "details"};

/* The advanced settings tree's rows. */
enum { ROW_GROUP, ROW_CHECK, ROW_RADIO };
enum {
    OPT_NONE,
    OPT_FULL_ADDRESS,
    OPT_FULL_TITLE,
    OPT_HIDDEN_NO,
    OPT_HIDDEN_YES,
    OPT_HIDE_EXT,
    OPT_REMEMBER,
};
enum { ADV_ICON, ADV_LABEL, ADV_KIND, ADV_ACTIVE, ADV_OPTION, ADV_IS_TOGGLE, ADV_IS_RADIO,
       ADV_COUNT };

/* The File Types list. */
enum { FT_ICON, FT_EXT, FT_DESC, FT_TYPE, FT_COUNT };

typedef struct props {
    GtkWidget *window, *apply;
    const char *current_view; /* --view, canonical name, or nullptr */
    bool updating;
    /* General */
    GtkWidget *common_tasks, *classic, *same_window, *own_window, *new_tabs, *single_click,
        *double_click;
    /* View */
    GtkTreeStore *adv;
    GtkWidget *adv_view;
    /* File Types */
    GtkListStore *types;
    GtkWidget *types_view, *details_frame, *opens_icon, *opens_name, *change, *details, *delete;
    bool types_loaded;
    /* Search (7's options nullptr in the other styles) */
    GtkWidget *search_subfolders, *search_partial, *search_system, *search_archives,
        *search_contents, *search_regex, *highlight_color, *highlight_theme;
} props;

static void changed(props *folder_options) {
    if (!folder_options->updating)
        gtk_widget_set_sensitive(folder_options->apply, TRUE);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *titled_frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(titled_frame), child);
    return titled_frame;
}

/* A group box with XP's picture on the left. */
static GtkWidget *picture_frame(const char *title, const char *icon, GtkWidget *content) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(image, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), content, TRUE, TRUE, 0);
    return frame(title, row);
}

static GtkWidget *restore_defaults_row(GtkWidget **button) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    *button = gtk_button_new_with_mnemonic("_Restore Defaults");
    gtk_box_pack_end(GTK_BOX(row), *button, FALSE, FALSE, 0);
    return row;
}

static void on_toggle_changed([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    changed(data);
}

/* ---- General ----------------------------------------------------------------------- */

static void show_general(props *folder_options, const xd_folder_settings *folder_settings) {
    folder_options->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_settings->common_tasks
                                                       ? folder_options->common_tasks
                                                       : folder_options->classic),
                                 TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_settings->new_window
                                                       ? folder_options->own_window
                                                       : folder_options->same_window),
                                 TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_options->new_tabs),
                                 folder_settings->new_tabs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_settings->single_click
                                                       ? folder_options->single_click
                                                       : folder_options->double_click),
                                 TRUE);
    folder_options->updating = false;
}

static void general_defaults([[maybe_unused]] GtkButton *button, gpointer data) {
    props *folder_options = data;
    xd_settings defaults;
    xd_settings_defaults(&defaults);
    show_general(folder_options, &defaults.folders);
    changed(folder_options);
}

static GtkWidget *radio(GtkWidget *box, GtkWidget *group, const char *label,
                        props *folder_options) {
    GtkWidget *radio_button =
        group != nullptr
            ? gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(group), label)
            : gtk_radio_button_new_with_mnemonic(nullptr, label);
    if (folder_options != nullptr)
        g_signal_connect(radio_button, "toggled", G_CALLBACK(on_toggle_changed), folder_options);
    gtk_box_pack_start(GTK_BOX(box), radio_button, FALSE, FALSE, 0);
    return radio_button;
}

static GtkWidget *general_page(props *folder_options) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    /* XP's task pane in xd-files' windows where the Folders bar isn't. */
    GtkWidget *tasks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    folder_options->common_tasks =
        radio(tasks, nullptr, "Show common _tasks in folders", folder_options);
    folder_options->classic =
        radio(tasks, folder_options->common_tasks, "Use Windows c_lassic folders", folder_options);
    gtk_box_pack_start(GTK_BOX(page), picture_frame("Tasks", "folder", tasks), FALSE, FALSE, 0);

    GtkWidget *browse = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    folder_options->same_window =
        radio(browse, nullptr, "Open each folder in the s_ame window", folder_options);
    folder_options->own_window = radio(browse, folder_options->same_window,
                                       "Open each folder in its o_wn window", folder_options);
    /* Not in XP, which had no tabs: xd-files' own (Windows 11's), off by
     * default. What would open a new window from a folder window (its
     * own window above, a new network place or drive) opens a tab in it;
     * the desktop and the Start menu still open windows, as Windows 11's. */
    folder_options->new_tabs =
        gtk_check_button_new_with_mnemonic("Open folders in _new tabs instead of new "
                                           "windows");
    g_signal_connect(folder_options->new_tabs, "toggled", G_CALLBACK(on_toggle_changed),
                     folder_options);
    gtk_widget_set_margin_top(folder_options->new_tabs, 4);
    gtk_box_pack_start(GTK_BOX(browse), folder_options->new_tabs, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), picture_frame("Browse folders", "window-new", browse),
                       FALSE, FALSE, 0);

    GtkWidget *click = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    folder_options->single_click =
        radio(click, nullptr, "_Single-click to open an item (point to select)", folder_options);
    /* Titles are underlined only when pointed at; following a browser's
     * setting has no meaning here. */
    GtkWidget *under = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(under, 22);
    GtkWidget *browser = radio(under, nullptr, "Underline icon titles consistent with my _browser",
                               nullptr);
    GtkWidget *point = radio(under, browser, "Underline icon titles only when I _point at them",
                             nullptr);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(point), TRUE);
    gtk_widget_set_sensitive(under, FALSE);
    gtk_box_pack_start(GTK_BOX(click), under, FALSE, FALSE, 0);
    folder_options->double_click =
        radio(click, folder_options->single_click,
              "_Double-click to open an item (single-click to select)", folder_options);
    gtk_box_pack_start(GTK_BOX(page), picture_frame("Click items as follows", "input-mouse", click),
                       FALSE, FALSE, 0);

    GtkWidget *defaults;
    gtk_box_pack_end(GTK_BOX(page), restore_defaults_row(&defaults), FALSE, FALSE, 0);
    g_signal_connect(defaults, "clicked", G_CALLBACK(general_defaults), folder_options);
    return page;
}

/* ---- View: advanced settings ---------------------------------------------------------- */

static void adv_row(props *folder_options, GtkTreeIter *parent, GtkTreeIter *out, int kind,
                    const char *label, int option) {
    gtk_tree_store_append(folder_options->adv, out, parent);
    gtk_tree_store_set(folder_options->adv, out, ADV_ICON, kind == ROW_GROUP ? "folder" : nullptr,
                       ADV_LABEL, label, ADV_KIND, kind, ADV_ACTIVE, FALSE, ADV_OPTION, option,
                       ADV_IS_TOGGLE, kind != ROW_GROUP, ADV_IS_RADIO, kind == ROW_RADIO, -1);
}

static void fill_advanced(props *folder_options) {
    GtkTreeIter files, hidden, row;
    adv_row(folder_options, nullptr, &files, ROW_GROUP, "Files and Folders", OPT_NONE);
    adv_row(folder_options, &files, &row, ROW_CHECK, "Display the full path in the address bar",
            OPT_FULL_ADDRESS);
    adv_row(folder_options, &files, &row, ROW_CHECK, "Display the full path in the title bar",
            OPT_FULL_TITLE);
    adv_row(folder_options, &files, &hidden, ROW_GROUP, "Hidden files and folders", OPT_NONE);
    adv_row(folder_options, &hidden, &row, ROW_RADIO, "Do not show hidden files and folders",
            OPT_HIDDEN_NO);
    adv_row(folder_options, &hidden, &row, ROW_RADIO, "Show hidden files and folders",
            OPT_HIDDEN_YES);
    adv_row(folder_options, &files, &row, ROW_CHECK, "Hide extensions for known file types",
            OPT_HIDE_EXT);
    adv_row(folder_options, &files, &row, ROW_CHECK, "Remember each folder's view settings",
            OPT_REMEMBER);
}

typedef struct adv_walk {
    xd_folder_settings *f;
    bool write; /* settings to rows, else rows to settings */
    props *p;
} adv_walk;

static bool *option_field(xd_folder_settings *folder_settings, int option, bool *negated) {
    *negated = false;
    switch (option) {
    case OPT_FULL_ADDRESS:
        return &folder_settings->full_path_address;
    case OPT_FULL_TITLE:
        return &folder_settings->full_path_title;
    case OPT_HIDDEN_NO:
        *negated = true;
        return &folder_settings->show_hidden;
    case OPT_HIDDEN_YES:
        return &folder_settings->show_hidden;
    case OPT_HIDE_EXT:
        return &folder_settings->hide_extensions;
    case OPT_REMEMBER:
        return &folder_settings->remember_views;
    default:
        return nullptr;
    }
}

static gboolean adv_walk_row(GtkTreeModel *model, [[maybe_unused]] GtkTreePath *path,
                             GtkTreeIter *iter, gpointer data) {
    adv_walk *advanced_walk = data;
    int option = OPT_NONE;
    gboolean active = FALSE;
    gtk_tree_model_get(model, iter, ADV_OPTION, &option, ADV_ACTIVE, &active, -1);
    bool negated;
    bool *field = option_field(advanced_walk->f, option, &negated);
    if (field == nullptr)
        return FALSE;
    if (advanced_walk->write)
        gtk_tree_store_set(advanced_walk->p->adv, iter, ADV_ACTIVE, *field != negated, -1);
    else if (active)
        *field = !negated;
    return FALSE;
}

static void show_advanced(props *folder_options, xd_folder_settings *folder_settings) {
    adv_walk advanced_walk = {folder_settings, true, folder_options};
    gtk_tree_model_foreach(GTK_TREE_MODEL(folder_options->adv), adv_walk_row, &advanced_walk);
}

static void read_advanced(props *folder_options, xd_folder_settings *folder_settings) {
    adv_walk advanced_walk = {folder_settings, false, folder_options};
    gtk_tree_model_foreach(GTK_TREE_MODEL(folder_options->adv), adv_walk_row, &advanced_walk);
}

/* A check box flips; a radio button turns its siblings off. */
static void toggle_row(props *folder_options, GtkTreeIter *iter) {
    int kind = ROW_GROUP;
    gboolean active = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(folder_options->adv), iter, ADV_KIND, &kind, ADV_ACTIVE,
                       &active, -1);
    if (kind == ROW_CHECK) {
        gtk_tree_store_set(folder_options->adv, iter, ADV_ACTIVE, !active, -1);
    } else if (kind == ROW_RADIO && !active) {
        GtkTreeIter parent, sibling;
        if (gtk_tree_model_iter_parent(GTK_TREE_MODEL(folder_options->adv), &parent, iter) &&
            gtk_tree_model_iter_children(GTK_TREE_MODEL(folder_options->adv), &sibling, &parent)) {
            do {
                int k = ROW_GROUP;
                gtk_tree_model_get(GTK_TREE_MODEL(folder_options->adv), &sibling, ADV_KIND, &k, -1);
                if (k == ROW_RADIO)
                    gtk_tree_store_set(folder_options->adv, &sibling, ADV_ACTIVE, FALSE, -1);
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(folder_options->adv), &sibling));
        }
        gtk_tree_store_set(folder_options->adv, iter, ADV_ACTIVE, TRUE, -1);
    } else {
        return;
    }
    changed(folder_options);
}

/* A click anywhere on an option's row (or Enter) sets it. */
static void on_adv_activated([[maybe_unused]] GtkTreeView *view, GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *col, gpointer data) {
    props *folder_options = data;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(folder_options->adv), &iter, path))
        toggle_row(folder_options, &iter);
}

/* Space too, as in XP. */
static gboolean on_adv_key(GtkWidget *view, GdkEventKey *ev, gpointer data) {
    if (ev->keyval != GDK_KEY_space && ev->keyval != GDK_KEY_KP_Space)
        return FALSE;
    GtkTreePath *path = nullptr;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(view), &path, nullptr);
    if (path != nullptr) {
        on_adv_activated(GTK_TREE_VIEW(view), path, nullptr, data);
        gtk_tree_path_free(path);
    }
    return TRUE;
}

static void advanced_defaults([[maybe_unused]] GtkButton *button, gpointer data) {
    props *folder_options = data;
    xd_settings defaults;
    xd_settings_defaults(&defaults);
    show_advanced(folder_options, &defaults.folders);
    changed(folder_options);
}

/* ---- View: folder views --------------------------------------------------------------- */

/* Sets the view folders without their own use and forgets every
 * folder's own; xd-files picks it up for the next folder it opens. */
static void reset_folder_views(props *folder_options, const char *view) {
    xd_settings settings;
    xd_settings_load(&settings);
    g_strlcpy(settings.folders.default_view, view, sizeof(settings.folders.default_view));
    bool ok = xd_settings_save(&settings) && xd_state_write_lines(FOLDER_VIEWS, nullptr, 0);
    if (!ok)
        xd_message_box(GTK_WINDOW(folder_options->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The folder view settings could not be saved.");
}

static void on_apply_all([[maybe_unused]] GtkButton *button, gpointer data) {
    props *folder_options = data;
    if (xd_message_box(GTK_WINDOW(folder_options->window), "Folder Views", XD_MB_QUESTION,
                       XD_MB_YES_NO, FALSE,
                       "This will set all of your folders to match the current folder's view "
                       "settings.\n\nDo you want to continue?") == GTK_RESPONSE_YES)
        reset_folder_views(folder_options, folder_options->current_view);
}

static void on_reset_all([[maybe_unused]] GtkButton *button, gpointer data) {
    props *folder_options = data;
    if (xd_message_box(GTK_WINDOW(folder_options->window), "Folder Views", XD_MB_QUESTION,
                       XD_MB_YES_NO, FALSE,
                       "This will reset all of your folders to their default view settings.\n\n"
                       "Do you want to continue?") == GTK_RESPONSE_YES)
        reset_folder_views(folder_options, "");
}

static GtkWidget *view_page(props *folder_options) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *views = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *text = gtk_label_new("You can apply the view (such as Details or Tiles) that you "
                                    "are using for this folder to all folders.");
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 48);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_box_pack_start(GTK_BOX(views), text, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *all = gtk_button_new_with_mnemonic("Apply to A_ll Folders");
    GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset All Folders");
    gtk_widget_set_sensitive(all, folder_options->current_view != nullptr);
    g_signal_connect(all, "clicked", G_CALLBACK(on_apply_all), folder_options);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_all), folder_options);
    gtk_box_pack_start(GTK_BOX(buttons), all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(views), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), picture_frame("Folder views", "view-list-details", views),
                       FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new_with_mnemonic("_Advanced settings:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, FALSE, 0);
    folder_options->adv =
        gtk_tree_store_new(ADV_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN,
                           G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    fill_advanced(folder_options);
    folder_options->adv_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(folder_options->adv));
    g_object_unref(folder_options->adv);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(folder_options->adv_view), FALSE);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(folder_options->adv_view), TRUE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    GtkCellRenderer *name = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_pack_start(col, toggle, FALSE);
    gtk_tree_view_column_pack_start(col, name, TRUE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", ADV_ICON);
    gtk_tree_view_column_add_attribute(col, toggle, "visible", ADV_IS_TOGGLE);
    gtk_tree_view_column_add_attribute(col, toggle, "radio", ADV_IS_RADIO);
    gtk_tree_view_column_add_attribute(col, toggle, "active", ADV_ACTIVE);
    gtk_tree_view_column_add_attribute(col, name, "text", ADV_LABEL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(folder_options->adv_view), col);
    /* A click on the row (box or text) activates it, which toggles it:
     * no "toggled" handler, or the box would toggle twice. */
    g_signal_connect(folder_options->adv_view, "row-activated", G_CALLBACK(on_adv_activated),
                     folder_options);
    g_signal_connect(folder_options->adv_view, "key-press-event", G_CALLBACK(on_adv_key),
                     folder_options);
    /* Always open, without expanders, as in XP. */
    gtk_tree_view_expand_all(GTK_TREE_VIEW(folder_options->adv_view));
    gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(folder_options->adv_view), FALSE);
    gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(folder_options->adv_view), 18);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), folder_options->adv_view);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 190);
    gtk_container_add(GTK_CONTAINER(scroll), folder_options->adv_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *defaults;
    gtk_box_pack_start(GTK_BOX(page), restore_defaults_row(&defaults), FALSE, FALSE, 0);
    g_signal_connect(defaults, "clicked", G_CALLBACK(advanced_defaults), folder_options);
    return page;
}

/* ---- File Types ------------------------------------------------------------------------ */

/* Rows for `type`'s extensions, one per extension as XP lists them; with
 * `only` (upper case), just that one. */
static void add_type_rows(props *folder_options, const char *type, const char *only) {
    char **exts = xd_content_type_extensions(type);
    if (exts[0] != nullptr) {
        char *desc = g_content_type_get_description(type);
        GIcon *icon = g_content_type_get_icon(type);
        for (char **ext = exts; *ext != nullptr; ext++) {
            char *upper = g_utf8_strup(*ext, -1);
            if (only == nullptr || strcmp(upper, only) == 0)
                gtk_list_store_insert_with_values(folder_options->types, nullptr, -1, FT_ICON, icon,
                                                  FT_EXT, upper, FT_DESC, desc, FT_TYPE, type, -1);
            g_free(upper);
        }
        g_object_unref(icon);
        g_free(desc);
    }
    g_strfreev(exts);
}

static void fill_types(props *folder_options) {
    GList *types = g_content_types_get_registered();
    for (GList *node = types; node != nullptr; node = node->next)
        add_type_rows(folder_options, node->data, nullptr);
    g_list_free_full(types, g_free);
}

static bool selected_type(props *folder_options, char **ext, char **desc, char **type) {
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(folder_options->types_view));
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return false;
    gtk_tree_model_get(model, &iter, FT_EXT, ext, FT_DESC, desc, FT_TYPE, type, -1);
    return true;
}

static void show_type_details(props *folder_options) {
    char *ext = nullptr, *desc = nullptr, *type = nullptr;
    if (!selected_type(folder_options, &ext, &desc, &type)) {
        gtk_frame_set_label(GTK_FRAME(folder_options->details_frame), "Details");
        gtk_label_set_text(GTK_LABEL(folder_options->opens_name), "");
        gtk_image_clear(GTK_IMAGE(folder_options->opens_icon));
        gtk_label_set_text(GTK_LABEL(folder_options->details), "");
        gtk_widget_set_sensitive(folder_options->change, FALSE);
        gtk_widget_set_sensitive(folder_options->delete, FALSE);
        return;
    }
    char *title = g_strdup_printf("Details for '%s' extension", ext);
    gtk_frame_set_label(GTK_FRAME(folder_options->details_frame), title);
    GAppInfo *app = g_app_info_get_default_for_type(type, FALSE);
    if (app != nullptr) {
        gtk_label_set_text(GTK_LABEL(folder_options->opens_name), g_app_info_get_name(app));
        GIcon *icon = g_app_info_get_icon(app);
        if (icon != nullptr)
            gtk_image_set_from_gicon(GTK_IMAGE(folder_options->opens_icon), icon,
                                     GTK_ICON_SIZE_SMALL_TOOLBAR);
        else
            gtk_image_clear(GTK_IMAGE(folder_options->opens_icon));
        g_object_unref(app);
    } else {
        gtk_label_set_text(GTK_LABEL(folder_options->opens_name), "Unknown application");
        gtk_image_clear(GTK_IMAGE(folder_options->opens_icon));
    }
    char *text = g_strdup_printf("Files with extension '%s' are of type '%s' (%s).", ext, desc,
                                 type);
    gtk_label_set_text(GTK_LABEL(folder_options->details), text);
    gtk_widget_set_sensitive(folder_options->change, TRUE);
    gtk_widget_set_sensitive(folder_options->delete, TRUE);
    g_free(text);
    g_free(title);
    g_free(ext);
    g_free(desc);
    g_free(type);
}

static void on_type_selected([[maybe_unused]] GtkTreeSelection *sel, gpointer data) {
    show_type_details(data);
}

/* Change...: XP's Open With. The choice is the type's default at once
 * (mimeapps.list), as in XP. */
static void on_change([[maybe_unused]] GtkButton *button, gpointer data) {
    props *folder_options = data;
    char *ext = nullptr, *desc = nullptr, *type = nullptr;
    if (!selected_type(folder_options, &ext, &desc, &type))
        return;
    GtkWidget *dlg = gtk_app_chooser_dialog_new_for_content_type(
        GTK_WINDOW(folder_options->window),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), type);
    gtk_window_set_title(GTK_WINDOW(dlg), "Open With");
    char *heading = g_strdup_printf("Choose the program you want to use to open '%s' files:",
                                    desc);
    gtk_app_chooser_dialog_set_heading(GTK_APP_CHOOSER_DIALOG(dlg), heading);
    g_free(heading);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        GAppInfo *app = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dlg));
        GError *err = nullptr;
        if (app != nullptr && !g_app_info_set_as_default_for_type(app, type, &err)) {
            char *msg = g_strdup_printf("The program could not be set for '%s' files.\n\n%s",
                                        desc, err->message);
            xd_message_box(GTK_WINDOW(folder_options->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           msg);
            g_free(msg);
            g_error_free(err);
        }
        g_clear_object(&app);
    }
    gtk_widget_destroy(dlg);
    show_type_details(folder_options);
    g_free(ext);
    g_free(desc);
    g_free(type);
}

/* The database is read when the tab is first shown. */
static void load_types(props *folder_options) {
    if (folder_options->types_loaded)
        return;
    folder_options->types_loaded = true;
    GdkWindow *win = gtk_widget_get_window(folder_options->window);
    GdkCursor *wait = win != nullptr ? gdk_cursor_new_from_name(gdk_window_get_display(win),
                                                                "wait")
                                     : nullptr;
    if (win != nullptr) {
        gdk_window_set_cursor(win, wait);
        gdk_display_flush(gdk_window_get_display(win));
    }
    fill_types(folder_options);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(folder_options->types), FT_EXT,
                                         GTK_SORT_ASCENDING);
    if (win != nullptr)
        gdk_window_set_cursor(win, nullptr);
    g_clear_object(&wait);
}

static void on_types_mapped([[maybe_unused]] GtkWidget *widget, gpointer data) { load_types(data); }

/* Takes the rows of `ext` (upper case) as `type` out of the list. */
static void remove_rows(props *folder_options, const char *ext, const char *type) {
    GtkTreeModel *model = GTK_TREE_MODEL(folder_options->types);
    GtkTreeIter iter;
    bool ok = gtk_tree_model_get_iter_first(model, &iter);
    while (ok) {
        char *row_ext = nullptr, *row_type = nullptr;
        gtk_tree_model_get(model, &iter, FT_EXT, &row_ext, FT_TYPE, &row_type, -1);
        bool match = g_strcmp0(row_ext, ext) == 0 && g_strcmp0(row_type, type) == 0;
        g_free(row_ext);
        g_free(row_type);
        ok = match ? gtk_list_store_remove(folder_options->types, &iter)
                   : gtk_tree_model_iter_next(model, &iter);
    }
}

/* Selects `ext` (upper case) as `type`, if it's listed. */
static void select_row(props *folder_options, const char *ext, const char *type) {
    GtkTreeModel *model = GTK_TREE_MODEL(folder_options->types);
    GtkTreeIter iter;
    for (bool ok = gtk_tree_model_get_iter_first(model, &iter); ok;
         ok = gtk_tree_model_iter_next(model, &iter)) {
        char *row_ext = nullptr, *row_type = nullptr;
        gtk_tree_model_get(model, &iter, FT_EXT, &row_ext, FT_TYPE, &row_type, -1);
        bool match = g_strcmp0(row_ext, ext) == 0 && g_strcmp0(row_type, type) == 0;
        g_free(row_ext);
        g_free(row_type);
        if (match) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(folder_options->types_view), path, nullptr,
                                     FALSE);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(folder_options->types_view), path, nullptr,
                                         TRUE, 0.5f, 0);
            gtk_tree_path_free(path);
            break;
        }
    }
    show_type_details(folder_options);
}

/* The type that registers `ext` (upper case) now, or nullptr; g_free. */
static char *type_of_extension(props *folder_options, const char *ext, char **desc) {
    GtkTreeModel *model = GTK_TREE_MODEL(folder_options->types);
    GtkTreeIter iter;
    for (bool ok = gtk_tree_model_get_iter_first(model, &iter); ok;
         ok = gtk_tree_model_iter_next(model, &iter)) {
        char *row_ext = nullptr, *type = nullptr;
        gtk_tree_model_get(model, &iter, FT_EXT, &row_ext, FT_TYPE, &type, FT_DESC, desc, -1);
        bool match = g_strcmp0(row_ext, ext) == 0;
        g_free(row_ext);
        if (match)
            return type;
        g_free(type);
        g_free(*desc);
        *desc = nullptr;
    }
    return nullptr;
}

static void error_box(props *folder_options, const char *what, const GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error != nullptr ? error->message : "");
    xd_message_box(GTK_WINDOW(folder_options->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

static bool update_database(props *folder_options) {
    GError *error = nullptr;
    if (xd_mime_update_database(g_get_user_data_dir(), &error))
        return true;
    error_box(folder_options,
              "The file types could not be updated (update-mime-database, from "
              "shared-mime-info).",
              error);
    g_error_free(error);
    return false;
}

/* The New dialog's state. */
typedef struct new_ext {
    GtkWidget *entry, *advanced, *assoc_label, *assoc;
} new_ext;

static void on_advanced([[maybe_unused]] GtkButton *button, gpointer data) {
    new_ext *new_dialog = data;
    bool show = !gtk_widget_get_visible(new_dialog->assoc);
    gtk_widget_set_visible(new_dialog->assoc_label, show);
    gtk_widget_set_visible(new_dialog->assoc, show);
    gtk_button_set_label(GTK_BUTTON(new_dialog->advanced), show ? "<< _Advanced" : "_Advanced >>");
}

/* A pair for the Associated File Type list. */
typedef struct type_name {
    char *desc, *type;
} type_name;

static int compare_names(gconstpointer left, gconstpointer right) {
    const type_name *left_name = *(type_name *const *)left,
                    *right_name = *(type_name *const *)right;
    return g_utf8_collate(left_name->desc, right_name->desc);
}

static void type_name_free(gpointer data) {
    type_name *pair = data;
    g_free(pair->desc);
    g_free(pair->type);
    g_free(pair);
}

/* XP's Associated File Type list: <New>, then the types by name. */
static GtkWidget *types_combo(props *folder_options) {
    GPtrArray *names = g_ptr_array_new_with_free_func(type_name_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GtkTreeModel *model = GTK_TREE_MODEL(folder_options->types);
    GtkTreeIter iter;
    for (bool ok = gtk_tree_model_get_iter_first(model, &iter); ok;
         ok = gtk_tree_model_iter_next(model, &iter)) {
        type_name *pair = g_new0(type_name, 1);
        gtk_tree_model_get(model, &iter, FT_TYPE, &pair->type, FT_DESC, &pair->desc, -1);
        if (pair->type == nullptr || g_hash_table_contains(seen, pair->type)) {
            type_name_free(pair);
            continue;
        }
        if (pair->desc == nullptr)
            pair->desc = g_strdup(pair->type);
        g_hash_table_add(seen, pair->type);
        g_ptr_array_add(names, pair);
    }
    g_hash_table_unref(seen);
    g_ptr_array_sort(names, compare_names);
    GtkWidget *combo = gtk_combo_box_text_new();
    /* Long names don't widen the dialog. */
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo));
    for (GList *cell = cells; cell != nullptr; cell = cell->next)
        g_object_set(cell->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 36, nullptr);
    g_list_free(cells);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "", "<New>");
    for (guint i = 0; i < names->len; i++) {
        const type_name *pair = g_ptr_array_index(names, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), pair->type, pair->desc);
    }
    g_ptr_array_unref(names);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), "");
    return combo;
}

/* Registers `ext` for the type chosen (or a new one), moving it from the
 * type that has it now if the user agrees. */
static bool create_extension(props *folder_options, GtkWindow *parent, const char *ext,
                             const char *chosen) {
    char *upper = g_utf8_strup(ext, -1);
    char *type = chosen[0] != '\0' ? g_strdup(chosen) : xd_mime_type_for_new_extension(ext);
    char *comment = chosen[0] != '\0' ? nullptr : g_strdup_printf("%s File", upper);
    char *owner_desc = nullptr;
    char *owner = type_of_extension(folder_options, upper, &owner_desc);
    char **dirs = xd_mime_data_dirs();
    GError *error = nullptr;
    bool back = false;  /* the user said No: back to the dialog */
    bool moved = false; /* the database files changed */
    if (owner == nullptr || strcmp(owner, type) != 0) {
        bool free_ext = owner == nullptr;
        if (owner != nullptr) {
            char *desc = comment != nullptr ? g_strdup(comment)
                                            : g_content_type_get_description(type);
            char *question_text =
                g_strdup_printf("The extension '%s' is already associated with the file "
                                "type '%s'. Do you want to associate it with '%s' "
                                "instead?",
                                upper, owner_desc, desc);
            g_free(desc);
            back = xd_message_box(parent, TITLE, XD_MB_QUESTION, XD_MB_YES_NO, TRUE,
                                  question_text) != GTK_RESPONSE_YES;
            g_free(question_text);
            bool gone = false;
            if (!back) {
                free_ext = xd_mime_remove_extension((const char *const *)dirs, owner, ext, &gone,
                                                    &error);
                if (!free_ext)
                    error_box(folder_options,
                              "The extension could not be removed from its file type.", error);
                moved = free_ext;
            }
            if (gone)
                g_app_info_reset_type_associations(owner);
        }
        g_clear_error(&error);
        if (free_ext) {
            if (xd_mime_add_extension((const char *const *)dirs, type, ext, comment, &error))
                moved = true;
            else
                error_box(folder_options, "The extension could not be registered.", error);
            g_clear_error(&error);
        }
    }
    if (moved && update_database(folder_options)) {
        if (owner != nullptr)
            remove_rows(folder_options, upper, owner);
        remove_rows(folder_options, upper, type);
        add_type_rows(folder_options, type, upper);
    }
    if (!back)
        select_row(folder_options, upper, type);
    g_strfreev(dirs);
    g_free(owner);
    g_free(owner_desc);
    g_free(comment);
    g_free(type);
    g_free(upper);
    return !back;
}

/* New: XP's Create New Extension. */
static void on_new([[maybe_unused]] GtkButton *button, gpointer data) {
    props *folder_options = data;
    GtkWidget *dialog_widget = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog_widget), "Create New Extension");
    gtk_window_set_transient_for(GTK_WINDOW(dialog_widget), GTK_WINDOW(folder_options->window));
    gtk_window_set_modal(GTK_WINDOW(dialog_widget), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog_widget), FALSE);
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    new_ext new_dialog = {0};
    GtkWidget *label = gtk_label_new_with_mnemonic("File _Extension:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    new_dialog.entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(new_dialog.entry), 33);
    gtk_entry_set_activates_default(GTK_ENTRY(new_dialog.entry), TRUE);
    gtk_widget_set_hexpand(new_dialog.entry, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), new_dialog.entry);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), new_dialog.entry, 1, 0, 1, 1);
    new_dialog.assoc_label = gtk_label_new_with_mnemonic("Associated File _Type:");
    gtk_label_set_xalign(GTK_LABEL(new_dialog.assoc_label), 0);
    new_dialog.assoc = types_combo(folder_options);
    gtk_widget_set_size_request(new_dialog.assoc, 260, -1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(new_dialog.assoc_label), new_dialog.assoc);
    gtk_grid_attach(GTK_GRID(grid), new_dialog.assoc_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), new_dialog.assoc, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog_widget))), grid, TRUE,
                       TRUE, 0);
    /* Advanced >> on the left of the buttons, as XP has it. */
    new_dialog.advanced = gtk_button_new_with_mnemonic("_Advanced >>");
    g_signal_connect(new_dialog.advanced, "clicked", G_CALLBACK(on_advanced), &new_dialog);
    enum { RESPONSE_ADVANCED = 1 };
    gtk_dialog_add_action_widget(GTK_DIALOG(dialog_widget), new_dialog.advanced, RESPONSE_ADVANCED);
    gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(gtk_widget_get_parent(new_dialog.advanced)),
                                       new_dialog.advanced, TRUE);
    gtk_dialog_add_button(GTK_DIALOG(dialog_widget), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(dialog_widget), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog_widget), GTK_RESPONSE_OK);
    gtk_widget_show_all(dialog_widget);
    gtk_widget_hide(new_dialog.assoc_label);
    gtk_widget_hide(new_dialog.assoc);

    for (int response; (response = gtk_dialog_run(GTK_DIALOG(dialog_widget))) == GTK_RESPONSE_OK ||
                       response == RESPONSE_ADVANCED;) {
        if (response == RESPONSE_ADVANCED)
            continue; /* on_advanced did it */
        char *ext = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(new_dialog.entry))));
        const char *bare = ext[0] == '.' ? ext + 1 : ext;
        bool valid = xd_mime_extension_valid(bare);
        if (!valid)
            xd_message_box(GTK_WINDOW(dialog_widget), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type an extension of letters and digits, without spaces or dots, "
                           "such as XYZ.");
        const char *chosen = gtk_combo_box_get_active_id(GTK_COMBO_BOX(new_dialog.assoc));
        bool finished = valid && create_extension(folder_options, GTK_WINDOW(dialog_widget), bare,
                                                  chosen != nullptr ? chosen : "");
        g_free(ext);
        if (finished)
            break;
    }
    gtk_widget_destroy(dialog_widget);
}

/* Delete: the extension leaves its type; a type of the user's own that
 * has no extension left goes, with its program. */
static void on_delete([[maybe_unused]] GtkButton *button, gpointer data) {
    props *folder_options = data;
    char *ext = nullptr, *desc = nullptr, *type = nullptr;
    if (!selected_type(folder_options, &ext, &desc, &type))
        return;
    int response = xd_message_box(
        GTK_WINDOW(folder_options->window), "File Types", XD_MB_WARNING, XD_MB_YES_NO, TRUE,
        "If you remove a file name extension, you will not be able to open "
        "files with this extension by double-clicking their icons.\n\n"
        "Are you sure you want to remove this extension?");
    if (response == GTK_RESPONSE_YES) {
        char **dirs = xd_mime_data_dirs();
        GError *error = nullptr;
        bool gone = false;
        if (!xd_mime_remove_extension((const char *const *)dirs, type, ext, &gone, &error)) {
            error_box(folder_options, "The extension could not be removed.", error);
            g_error_free(error);
        } else {
            if (gone)
                g_app_info_reset_type_associations(type);
            if (update_database(folder_options)) {
                remove_rows(folder_options, ext, type);
                show_type_details(folder_options);
            }
        }
        g_strfreev(dirs);
    }
    g_free(ext);
    g_free(desc);
    g_free(type);
}

static GtkTreeViewColumn *types_column(props *folder_options, const char *title, int text_col,
                                       bool icon) {
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, title);
    if (icon) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(col, pix, FALSE);
        gtk_tree_view_column_add_attribute(col, pix, "gicon", FT_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", text_col);
    gtk_tree_view_column_set_sort_column_id(col, text_col);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(folder_options->types_view), col);
    return col;
}

static GtkWidget *types_page(props *folder_options) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *label = gtk_label_new_with_mnemonic("Registered file _types:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, FALSE, 0);

    folder_options->types =
        gtk_list_store_new(FT_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    folder_options->types_view =
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(folder_options->types));
    g_object_unref(folder_options->types);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(folder_options->types_view), FT_EXT);
    GtkTreeViewColumn *ext = types_column(folder_options, "Extensions", FT_EXT, true);
    gtk_tree_view_column_set_fixed_width(ext, 110);
    gtk_tree_view_column_set_sizing(ext, GTK_TREE_VIEW_COLUMN_FIXED);
    types_column(folder_options, "File Types", FT_DESC, false);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), folder_options->types_view);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(folder_options->types_view)),
                     "changed", G_CALLBACK(on_type_selected), folder_options);
    g_signal_connect(folder_options->types_view, "map", G_CALLBACK(on_types_mapped),
                     folder_options);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 170);
    gtk_container_add(GTK_CONTAINER(scroll), folder_options->types_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    /* New and Delete change the user's MIME database (filetypes.h). */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    folder_options->delete = gtk_button_new_with_mnemonic("_Delete");
    GtkWidget *add = gtk_button_new_with_mnemonic("N_ew");
    gtk_widget_set_size_request(folder_options->delete, 75, -1);
    gtk_widget_set_size_request(add, 75, -1);
    g_signal_connect(add, "clicked", G_CALLBACK(on_new), folder_options);
    g_signal_connect(folder_options->delete, "clicked", G_CALLBACK(on_delete), folder_options);
    gtk_box_pack_end(GTK_BOX(row), folder_options->delete, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *opens = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(opens), gtk_label_new("Opens with:"), FALSE, FALSE, 0);
    folder_options->opens_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(folder_options->opens_icon), 16); /* even for an icon file */
    gtk_box_pack_start(GTK_BOX(opens), folder_options->opens_icon, FALSE, FALSE, 0);
    folder_options->opens_name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(folder_options->opens_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(folder_options->opens_name), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(opens), folder_options->opens_name, TRUE, TRUE, 0);
    folder_options->change = gtk_button_new_with_mnemonic("_Change...");
    g_signal_connect(folder_options->change, "clicked", G_CALLBACK(on_change), folder_options);
    gtk_box_pack_end(GTK_BOX(opens), folder_options->change, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details), opens, FALSE, FALSE, 0);
    folder_options->details = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(folder_options->details), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(folder_options->details), 52);
    gtk_label_set_xalign(GTK_LABEL(folder_options->details), 0);
    gtk_label_set_lines(GTK_LABEL(folder_options->details), 3);
    gtk_box_pack_start(GTK_BOX(details), folder_options->details, FALSE, FALSE, 0);
    folder_options->details_frame = frame("Details", details);
    gtk_box_pack_start(GTK_BOX(page), folder_options->details_frame, FALSE, FALSE, 0);
    show_type_details(folder_options);
    return page;
}

/* ---- Search ---------------------------------------------------------------------------- */

static void show_search(props *folder_options, const xd_folder_settings *folder_settings) {
    folder_options->updating = true;
    if (folder_options->search_subfolders != nullptr) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_options->search_subfolders),
                                     folder_settings->search_subfolders);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_options->search_partial),
                                     folder_settings->search_partial);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_options->search_system),
                                     folder_settings->search_system);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_options->search_archives),
                                     folder_settings->search_archives && XD_HAVE_LIBARCHIVE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_options->search_contents),
                                     folder_settings->search_contents);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_options->search_regex),
                                     folder_settings->search_regex);
    }
    bool themed = folder_settings->search_highlight[0] == '\0';
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folder_options->highlight_theme), themed);
    GdkRGBA color;
    if (themed || !gdk_rgba_parse(&color, folder_settings->search_highlight))
        gdk_rgba_parse(&color, "#ff8c00");
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(folder_options->highlight_color), &color);
    gtk_widget_set_sensitive(folder_options->highlight_color, !themed);
    folder_options->updating = false;
}

static void read_search(props *folder_options, xd_folder_settings *folder_settings) {
    if (folder_options->search_subfolders != nullptr) {
        folder_settings->search_subfolders =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->search_subfolders));
        folder_settings->search_partial =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->search_partial));
        folder_settings->search_system =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->search_system));
        folder_settings->search_archives =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->search_archives));
        folder_settings->search_contents =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->search_contents));
        folder_settings->search_regex =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->search_regex));
    }
    folder_settings->search_highlight[0] = '\0';
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->highlight_theme))) {
        GdkRGBA color;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(folder_options->highlight_color), &color);
        g_snprintf(folder_settings->search_highlight, sizeof(folder_settings->search_highlight),
                   "#%02x%02x%02x", (unsigned)(color.red * 255.0 + 0.5),
                   (unsigned)(color.green * 255.0 + 0.5), (unsigned)(color.blue * 255.0 + 0.5));
    }
}

static void search_defaults([[maybe_unused]] GtkButton *button, gpointer data) {
    props *folder_options = data;
    xd_settings defaults;
    xd_settings_defaults(&defaults);
    show_search(folder_options, &defaults.folders);
    changed(folder_options);
}

static void on_highlight_theme(GtkToggleButton *toggle, gpointer data) {
    props *folder_options = data;
    gtk_widget_set_sensitive(folder_options->highlight_color,
                             !gtk_toggle_button_get_active(toggle));
    changed(folder_options);
}

static void on_highlight_color([[maybe_unused]] GtkColorButton *button, gpointer data) {
    changed(data);
}

static GtkWidget *search_check(props *folder_options, GtkWidget *box, const char *label) {
    GtkWidget *button = gtk_check_button_new_with_mnemonic(label);
    g_signal_connect(button, "toggled", G_CALLBACK(on_toggle_changed), folder_options);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    return button;
}

/* Windows 7's Search tab, for xd-files' search box; there's no index, so
 * its index options are left out, and natural language search isn't
 * done (greyed). The regular expressions and the highlight colour are
 * Linux additions, and the only part shown in the 2000 and XP styles,
 * whose Folder Options had no Search tab. */
static GtkWidget *search_page(props *folder_options, bool seven) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    if (seven) {
        GtkWidget *how = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        folder_options->search_subfolders = search_check(
            folder_options, how, "Include _subfolders in search results when searching in file "
                                 "folders");
        folder_options->search_partial = search_check(folder_options, how, "Find _partial matches");
        GtkWidget *natural = search_check(folder_options, how, "Use _natural language search");
        gtk_widget_set_sensitive(natural, FALSE);
        gtk_box_pack_start(GTK_BOX(page), frame("How to search", how), FALSE, FALSE, 0);

        GtkWidget *outside = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        folder_options->search_system =
            search_check(folder_options, outside, "Include s_ystem directories");
        folder_options->search_archives =
            search_check(folder_options, outside, "Include _compressed files (ZIP, CAB...)");
        gtk_widget_set_sensitive(folder_options->search_archives, XD_HAVE_LIBARCHIVE);
        folder_options->search_contents = search_check(
            folder_options, outside, "Al_ways search file names and contents (this might take "
                                     "several minutes)");
        gtk_box_pack_start(GTK_BOX(page),
                           frame("When searching non-indexed locations", outside), FALSE, FALSE,
                           0);
    }
    GtkWidget *linux_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    if (seven) {
        folder_options->search_regex = search_check(
            folder_options, linux_box, "Allow _regular expressions (/pattern/)");
        gtk_widget_set_tooltip_text(folder_options->search_regex,
                                    "In the search box, /pattern/ (or /pattern/i, ignoring case) "
                                    "matches names, and content:/pattern/ what's in the files.");
    }
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Highlight matches in:");
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    folder_options->highlight_color = gtk_color_button_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), folder_options->highlight_color);
    g_signal_connect(folder_options->highlight_color, "color-set", G_CALLBACK(on_highlight_color),
                     folder_options);
    gtk_box_pack_start(GTK_BOX(row), folder_options->highlight_color, FALSE, FALSE, 0);
    folder_options->highlight_theme = gtk_check_button_new_with_mnemonic("Use th_eme colour");
    g_signal_connect(folder_options->highlight_theme, "toggled", G_CALLBACK(on_highlight_theme),
                     folder_options);
    gtk_box_pack_start(GTK_BOX(row), folder_options->highlight_theme, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(linux_box), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Search results", linux_box), FALSE, FALSE, 0);

    GtkWidget *defaults;
    gtk_box_pack_end(GTK_BOX(page), restore_defaults_row(&defaults), FALSE, FALSE, 0);
    g_signal_connect(defaults, "clicked", G_CALLBACK(search_defaults), folder_options);
    return page;
}

/* ---- applying ---------------------------------------------------------------------------- */

static void show_settings(props *folder_options) {
    xd_settings settings;
    xd_settings_load(&settings);
    show_general(folder_options, &settings.folders);
    folder_options->updating = true;
    show_advanced(folder_options, &settings.folders);
    folder_options->updating = false;
    show_search(folder_options, &settings.folders);
}

/* Saves over the file as it is now: other applets and xd-files' Show
 * Hidden Files may have changed it since. */
static bool apply(props *folder_options) {
    xd_settings settings;
    xd_settings_load(&settings);
    xd_folder_settings *folder_settings = &settings.folders;
    folder_settings->common_tasks =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->common_tasks));
    folder_settings->new_window =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->own_window));
    folder_settings->new_tabs =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->new_tabs));
    folder_settings->single_click =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folder_options->single_click));
    folder_settings->full_path_address = folder_settings->full_path_title =
        folder_settings->show_hidden = folder_settings->hide_extensions =
            folder_settings->remember_views = false;
    read_advanced(folder_options, folder_settings);
    read_search(folder_options, folder_settings);
    if (!xd_settings_save(&settings)) {
        xd_message_box(GTK_WINDOW(folder_options->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The folder options could not be saved.");
        return false;
    }
    gtk_widget_set_sensitive(folder_options->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *dialog_box, int response, gpointer data) {
    props *folder_options = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(folder_options);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(folder_options->apply) &&
        !apply(folder_options))
        return;
    gtk_widget_destroy(GTK_WIDGET(dialog_box));
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.FolderOptions");
    char *view = nullptr;
    GOptionEntry options[] = {
        {"view", 0, 0, G_OPTION_ARG_STRING, &view,
         "The current folder's view, for Apply to All Folders", "VIEW"},
        {},
    };
    GError *error = nullptr;
    if (!gtk_init_with_args(&argc, &argv, nullptr, options, nullptr, &error)) {
        g_printerr("xd-folders: %s\n", error != nullptr ? error->message : "cannot start");
        g_clear_error(&error);
        return EXIT_FAILURE;
    }
    xd_gtk_app_follow_theme();
    static props folder_options;
    for (size_t i = 0; view != nullptr && i < XD_ARRAY_LEN(view_names); i++)
        if (g_ascii_strcasecmp(view, view_names[i]) == 0)
            folder_options.current_view = view_names[i];
    g_free(view);

    folder_options.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(folder_options.window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(folder_options.window), "folder");
    gtk_window_set_resizable(GTK_WINDOW(folder_options.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(folder_options.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(folder_options.window), "Cancel", GTK_RESPONSE_CANCEL);
    folder_options.apply =
        gtk_dialog_add_button(GTK_DIALOG(folder_options.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(folder_options.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(folder_options.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(&folder_options),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), view_page(&folder_options),
                             gtk_label_new_with_mnemonic("_View"));
    xd_settings style_settings;
    xd_settings_load(&style_settings);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs),
                             search_page(&folder_options, style_settings.style == XD_STYLE_AERO),
                             gtk_label_new_with_mnemonic("_Search"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), types_page(&folder_options),
                             gtk_label_new_with_mnemonic("File _Types"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(folder_options.window))),
                       tabs, TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(folder_options.window, GTK_NOTEBOOK(tabs));
    show_settings(&folder_options);
    g_signal_connect(folder_options.window, "response", G_CALLBACK(on_response), &folder_options);
    g_signal_connect(folder_options.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(folder_options.window);
    gtk_main();
    return EXIT_SUCCESS;
}
