/* SPDX-License-Identifier: MIT
 * xd-folders: Windows XP's Folder Options. The settings go to [Folders]
 * in desktop.ini, which xd-files follows live; file associations are
 * GIO's (mimeapps.list).
 *
 *   General     open folders in the same or their own window (or a new
 *               tab, xd-files' own); single- or double-click to open (XP's
 *               task pane has no counterpart)
 *   View        Apply to All Folders / Reset All Folders, and the
 *               advanced settings tree
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
    GtkWidget *same_window, *own_window, *new_tabs, *single_click, *double_click;
    /* View */
    GtkTreeStore *adv;
    GtkWidget *adv_view;
    /* File Types */
    GtkListStore *types;
    GtkWidget *types_view, *details_frame, *opens_icon, *opens_name, *change, *details, *delete;
    bool types_loaded;
} props;

static void changed(props *p) {
    if (!p->updating)
        gtk_widget_set_sensitive(p->apply, TRUE);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
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

static void on_toggle_changed([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    changed(data);
}

/* ---- General ----------------------------------------------------------------------- */

static void show_general(props *p, const xd_folder_settings *f) {
    p->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(f->new_window ? p->own_window : p->same_window),
                                 TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->new_tabs), f->new_tabs);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(f->single_click ? p->single_click : p->double_click), TRUE);
    p->updating = false;
}

static void general_defaults([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    xd_settings d;
    xd_settings_defaults(&d);
    show_general(p, &d.folders);
    changed(p);
}

static GtkWidget *radio(GtkWidget *box, GtkWidget *group, const char *label, props *p) {
    GtkWidget *r = group != nullptr
                       ? gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(group),
                                                                        label)
                       : gtk_radio_button_new_with_mnemonic(nullptr, label);
    if (p != nullptr)
        g_signal_connect(r, "toggled", G_CALLBACK(on_toggle_changed), p);
    gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
    return r;
}

static GtkWidget *general_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    /* XP's task pane: xd-files has none, so classic folders it is. */
    GtkWidget *tasks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *common = radio(tasks, nullptr, "Show common _tasks in folders", nullptr);
    GtkWidget *classic = radio(tasks, common, "Use Windows c_lassic folders", nullptr);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(classic), TRUE);
    gtk_widget_set_sensitive(tasks, FALSE);
    gtk_box_pack_start(GTK_BOX(page), picture_frame("Tasks", "folder", tasks), FALSE, FALSE, 0);

    GtkWidget *browse = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    p->same_window = radio(browse, nullptr, "Open each folder in the s_ame window", p);
    p->own_window = radio(browse, p->same_window, "Open each folder in its o_wn window", p);
    /* Not in XP, which had no tabs: xd-files' own (Windows 11's), off by
     * default. What would open a new window from a folder window (its
     * own window above, a new network place or drive) opens a tab in it;
     * the desktop and the Start menu still open windows, as Windows 11's. */
    p->new_tabs = gtk_check_button_new_with_mnemonic("Open folders in _new tabs instead of new "
                                                      "windows");
    g_signal_connect(p->new_tabs, "toggled", G_CALLBACK(on_toggle_changed), p);
    gtk_widget_set_margin_top(p->new_tabs, 4);
    gtk_box_pack_start(GTK_BOX(browse), p->new_tabs, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), picture_frame("Browse folders", "window-new", browse),
                       FALSE, FALSE, 0);

    GtkWidget *click = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    p->single_click = radio(click, nullptr, "_Single-click to open an item (point to select)", p);
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
    p->double_click = radio(click, p->single_click,
                            "_Double-click to open an item (single-click to select)", p);
    gtk_box_pack_start(GTK_BOX(page), picture_frame("Click items as follows", "input-mouse", click),
                       FALSE, FALSE, 0);

    GtkWidget *defaults;
    gtk_box_pack_end(GTK_BOX(page), restore_defaults_row(&defaults), FALSE, FALSE, 0);
    g_signal_connect(defaults, "clicked", G_CALLBACK(general_defaults), p);
    return page;
}

/* ---- View: advanced settings ---------------------------------------------------------- */

static void adv_row(props *p, GtkTreeIter *parent, GtkTreeIter *out, int kind, const char *label,
                    int option) {
    gtk_tree_store_append(p->adv, out, parent);
    gtk_tree_store_set(p->adv, out, ADV_ICON, kind == ROW_GROUP ? "folder" : nullptr, ADV_LABEL,
                       label, ADV_KIND, kind, ADV_ACTIVE, FALSE, ADV_OPTION, option,
                       ADV_IS_TOGGLE, kind != ROW_GROUP, ADV_IS_RADIO, kind == ROW_RADIO, -1);
}

static void fill_advanced(props *p) {
    GtkTreeIter files, hidden, row;
    adv_row(p, nullptr, &files, ROW_GROUP, "Files and Folders", OPT_NONE);
    adv_row(p, &files, &row, ROW_CHECK, "Display the full path in the address bar",
            OPT_FULL_ADDRESS);
    adv_row(p, &files, &row, ROW_CHECK, "Display the full path in the title bar", OPT_FULL_TITLE);
    adv_row(p, &files, &hidden, ROW_GROUP, "Hidden files and folders", OPT_NONE);
    adv_row(p, &hidden, &row, ROW_RADIO, "Do not show hidden files and folders", OPT_HIDDEN_NO);
    adv_row(p, &hidden, &row, ROW_RADIO, "Show hidden files and folders", OPT_HIDDEN_YES);
    adv_row(p, &files, &row, ROW_CHECK, "Hide extensions for known file types", OPT_HIDE_EXT);
    adv_row(p, &files, &row, ROW_CHECK, "Remember each folder's view settings", OPT_REMEMBER);
}

typedef struct adv_walk {
    xd_folder_settings *f;
    bool write; /* settings to rows, else rows to settings */
    props *p;
} adv_walk;

static bool *option_field(xd_folder_settings *f, int option, bool *negated) {
    *negated = false;
    switch (option) {
    case OPT_FULL_ADDRESS:
        return &f->full_path_address;
    case OPT_FULL_TITLE:
        return &f->full_path_title;
    case OPT_HIDDEN_NO:
        *negated = true;
        return &f->show_hidden;
    case OPT_HIDDEN_YES:
        return &f->show_hidden;
    case OPT_HIDE_EXT:
        return &f->hide_extensions;
    case OPT_REMEMBER:
        return &f->remember_views;
    default:
        return nullptr;
    }
}

static gboolean adv_walk_row(GtkTreeModel *m, [[maybe_unused]] GtkTreePath *path,
                             GtkTreeIter *iter, gpointer data) {
    adv_walk *w = data;
    int option = OPT_NONE;
    gboolean active = FALSE;
    gtk_tree_model_get(m, iter, ADV_OPTION, &option, ADV_ACTIVE, &active, -1);
    bool negated;
    bool *field = option_field(w->f, option, &negated);
    if (field == nullptr)
        return FALSE;
    if (w->write)
        gtk_tree_store_set(w->p->adv, iter, ADV_ACTIVE, *field != negated, -1);
    else if (active)
        *field = !negated;
    return FALSE;
}

static void show_advanced(props *p, xd_folder_settings *f) {
    adv_walk w = {f, true, p};
    gtk_tree_model_foreach(GTK_TREE_MODEL(p->adv), adv_walk_row, &w);
}

static void read_advanced(props *p, xd_folder_settings *f) {
    adv_walk w = {f, false, p};
    gtk_tree_model_foreach(GTK_TREE_MODEL(p->adv), adv_walk_row, &w);
}

/* A check box flips; a radio button turns its siblings off. */
static void toggle_row(props *p, GtkTreeIter *iter) {
    int kind = ROW_GROUP;
    gboolean active = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(p->adv), iter, ADV_KIND, &kind, ADV_ACTIVE, &active, -1);
    if (kind == ROW_CHECK) {
        gtk_tree_store_set(p->adv, iter, ADV_ACTIVE, !active, -1);
    } else if (kind == ROW_RADIO && !active) {
        GtkTreeIter parent, sibling;
        if (gtk_tree_model_iter_parent(GTK_TREE_MODEL(p->adv), &parent, iter) &&
            gtk_tree_model_iter_children(GTK_TREE_MODEL(p->adv), &sibling, &parent)) {
            do {
                int k = ROW_GROUP;
                gtk_tree_model_get(GTK_TREE_MODEL(p->adv), &sibling, ADV_KIND, &k, -1);
                if (k == ROW_RADIO)
                    gtk_tree_store_set(p->adv, &sibling, ADV_ACTIVE, FALSE, -1);
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(p->adv), &sibling));
        }
        gtk_tree_store_set(p->adv, iter, ADV_ACTIVE, TRUE, -1);
    } else {
        return;
    }
    changed(p);
}

/* A click anywhere on an option's row (or Enter) sets it. */
static void on_adv_activated([[maybe_unused]] GtkTreeView *view, GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *col, gpointer data) {
    props *p = data;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(p->adv), &iter, path))
        toggle_row(p, &iter);
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

static void advanced_defaults([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    xd_settings d;
    xd_settings_defaults(&d);
    show_advanced(p, &d.folders);
    changed(p);
}

/* ---- View: folder views --------------------------------------------------------------- */

/* Sets the view folders without their own use and forgets every
 * folder's own; xd-files picks it up for the next folder it opens. */
static void reset_folder_views(props *p, const char *view) {
    xd_settings s;
    xd_settings_load(&s);
    g_strlcpy(s.folders.default_view, view, sizeof(s.folders.default_view));
    bool ok = xd_settings_save(&s) && xd_state_write_lines(FOLDER_VIEWS, nullptr, 0);
    if (!ok)
        xd_message_box(GTK_WINDOW(p->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The folder view settings could not be saved.");
}

static void on_apply_all([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    if (xd_message_box(GTK_WINDOW(p->window), "Folder Views", XD_MB_QUESTION, XD_MB_YES_NO, FALSE,
                       "This will set all of your folders to match the current folder's view "
                       "settings.\n\nDo you want to continue?") == GTK_RESPONSE_YES)
        reset_folder_views(p, p->current_view);
}

static void on_reset_all([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    if (xd_message_box(GTK_WINDOW(p->window), "Folder Views", XD_MB_QUESTION, XD_MB_YES_NO, FALSE,
                       "This will reset all of your folders to their default view settings.\n\n"
                       "Do you want to continue?") == GTK_RESPONSE_YES)
        reset_folder_views(p, "");
}

static GtkWidget *view_page(props *p) {
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
    gtk_widget_set_sensitive(all, p->current_view != nullptr);
    g_signal_connect(all, "clicked", G_CALLBACK(on_apply_all), p);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_all), p);
    gtk_box_pack_start(GTK_BOX(buttons), all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(views), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), picture_frame("Folder views", "view-list-details", views),
                       FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new_with_mnemonic("_Advanced settings:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, FALSE, 0);
    p->adv = gtk_tree_store_new(ADV_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                                G_TYPE_BOOLEAN, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    fill_advanced(p);
    p->adv_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->adv));
    g_object_unref(p->adv);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->adv_view), FALSE);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(p->adv_view), TRUE);
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
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->adv_view), col);
    /* A click on the row (box or text) activates it, which toggles it:
     * no "toggled" handler, or the box would toggle twice. */
    g_signal_connect(p->adv_view, "row-activated", G_CALLBACK(on_adv_activated), p);
    g_signal_connect(p->adv_view, "key-press-event", G_CALLBACK(on_adv_key), p);
    /* Always open, without expanders, as in XP. */
    gtk_tree_view_expand_all(GTK_TREE_VIEW(p->adv_view));
    gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(p->adv_view), FALSE);
    gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(p->adv_view), 18);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->adv_view);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 190);
    gtk_container_add(GTK_CONTAINER(scroll), p->adv_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *defaults;
    gtk_box_pack_start(GTK_BOX(page), restore_defaults_row(&defaults), FALSE, FALSE, 0);
    g_signal_connect(defaults, "clicked", G_CALLBACK(advanced_defaults), p);
    return page;
}

/* ---- File Types ------------------------------------------------------------------------ */

/* Rows for `type`'s extensions, one per extension as XP lists them; with
 * `only` (upper case), just that one. */
static void add_type_rows(props *p, const char *type, const char *only) {
    char **exts = xd_content_type_extensions(type);
    if (exts[0] != nullptr) {
        char *desc = g_content_type_get_description(type);
        GIcon *icon = g_content_type_get_icon(type);
        for (char **e = exts; *e != nullptr; e++) {
            char *upper = g_utf8_strup(*e, -1);
            if (only == nullptr || strcmp(upper, only) == 0)
                gtk_list_store_insert_with_values(p->types, nullptr, -1, FT_ICON, icon, FT_EXT,
                                                  upper, FT_DESC, desc, FT_TYPE, type, -1);
            g_free(upper);
        }
        g_object_unref(icon);
        g_free(desc);
    }
    g_strfreev(exts);
}

static void fill_types(props *p) {
    GList *types = g_content_types_get_registered();
    for (GList *l = types; l != nullptr; l = l->next)
        add_type_rows(p, l->data, nullptr);
    g_list_free_full(types, g_free);
}

static bool selected_type(props *p, char **ext, char **desc, char **type) {
    GtkTreeModel *m;
    GtkTreeIter iter;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->types_view));
    if (!gtk_tree_selection_get_selected(sel, &m, &iter))
        return false;
    gtk_tree_model_get(m, &iter, FT_EXT, ext, FT_DESC, desc, FT_TYPE, type, -1);
    return true;
}

static void show_type_details(props *p) {
    char *ext = nullptr, *desc = nullptr, *type = nullptr;
    if (!selected_type(p, &ext, &desc, &type)) {
        gtk_frame_set_label(GTK_FRAME(p->details_frame), "Details");
        gtk_label_set_text(GTK_LABEL(p->opens_name), "");
        gtk_image_clear(GTK_IMAGE(p->opens_icon));
        gtk_label_set_text(GTK_LABEL(p->details), "");
        gtk_widget_set_sensitive(p->change, FALSE);
        gtk_widget_set_sensitive(p->delete, FALSE);
        return;
    }
    char *title = g_strdup_printf("Details for '%s' extension", ext);
    gtk_frame_set_label(GTK_FRAME(p->details_frame), title);
    GAppInfo *app = g_app_info_get_default_for_type(type, FALSE);
    if (app != nullptr) {
        gtk_label_set_text(GTK_LABEL(p->opens_name), g_app_info_get_name(app));
        GIcon *icon = g_app_info_get_icon(app);
        if (icon != nullptr)
            gtk_image_set_from_gicon(GTK_IMAGE(p->opens_icon), icon, GTK_ICON_SIZE_SMALL_TOOLBAR);
        else
            gtk_image_clear(GTK_IMAGE(p->opens_icon));
        g_object_unref(app);
    } else {
        gtk_label_set_text(GTK_LABEL(p->opens_name), "Unknown application");
        gtk_image_clear(GTK_IMAGE(p->opens_icon));
    }
    char *text = g_strdup_printf("Files with extension '%s' are of type '%s' (%s).", ext, desc,
                                 type);
    gtk_label_set_text(GTK_LABEL(p->details), text);
    gtk_widget_set_sensitive(p->change, TRUE);
    gtk_widget_set_sensitive(p->delete, TRUE);
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
static void on_change([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    char *ext = nullptr, *desc = nullptr, *type = nullptr;
    if (!selected_type(p, &ext, &desc, &type))
        return;
    GtkWidget *dlg = gtk_app_chooser_dialog_new_for_content_type(
        GTK_WINDOW(p->window), (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        type);
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
            xd_message_box(GTK_WINDOW(p->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, msg);
            g_free(msg);
            g_error_free(err);
        }
        g_clear_object(&app);
    }
    gtk_widget_destroy(dlg);
    show_type_details(p);
    g_free(ext);
    g_free(desc);
    g_free(type);
}

/* The database is read when the tab is first shown. */
static void load_types(props *p) {
    if (p->types_loaded)
        return;
    p->types_loaded = true;
    GdkWindow *win = gtk_widget_get_window(p->window);
    GdkCursor *wait = win != nullptr ? gdk_cursor_new_from_name(gdk_window_get_display(win),
                                                                "wait")
                                     : nullptr;
    if (win != nullptr) {
        gdk_window_set_cursor(win, wait);
        gdk_display_flush(gdk_window_get_display(win));
    }
    fill_types(p);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(p->types), FT_EXT,
                                         GTK_SORT_ASCENDING);
    if (win != nullptr)
        gdk_window_set_cursor(win, nullptr);
    g_clear_object(&wait);
}

static void on_types_mapped([[maybe_unused]] GtkWidget *w, gpointer data) { load_types(data); }

/* Takes the rows of `ext` (upper case) as `type` out of the list. */
static void remove_rows(props *p, const char *ext, const char *type) {
    GtkTreeModel *m = GTK_TREE_MODEL(p->types);
    GtkTreeIter iter;
    bool ok = gtk_tree_model_get_iter_first(m, &iter);
    while (ok) {
        char *e = nullptr, *t = nullptr;
        gtk_tree_model_get(m, &iter, FT_EXT, &e, FT_TYPE, &t, -1);
        bool match = g_strcmp0(e, ext) == 0 && g_strcmp0(t, type) == 0;
        g_free(e);
        g_free(t);
        ok = match ? gtk_list_store_remove(p->types, &iter) : gtk_tree_model_iter_next(m, &iter);
    }
}

/* Selects `ext` (upper case) as `type`, if it's listed. */
static void select_row(props *p, const char *ext, const char *type) {
    GtkTreeModel *m = GTK_TREE_MODEL(p->types);
    GtkTreeIter iter;
    for (bool ok = gtk_tree_model_get_iter_first(m, &iter); ok;
         ok = gtk_tree_model_iter_next(m, &iter)) {
        char *e = nullptr, *t = nullptr;
        gtk_tree_model_get(m, &iter, FT_EXT, &e, FT_TYPE, &t, -1);
        bool match = g_strcmp0(e, ext) == 0 && g_strcmp0(t, type) == 0;
        g_free(e);
        g_free(t);
        if (match) {
            GtkTreePath *path = gtk_tree_model_get_path(m, &iter);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(p->types_view), path, nullptr, FALSE);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(p->types_view), path, nullptr, TRUE, 0.5f,
                                         0);
            gtk_tree_path_free(path);
            break;
        }
    }
    show_type_details(p);
}

/* The type that registers `ext` (upper case) now, or nullptr; g_free. */
static char *type_of_extension(props *p, const char *ext, char **desc) {
    GtkTreeModel *m = GTK_TREE_MODEL(p->types);
    GtkTreeIter iter;
    for (bool ok = gtk_tree_model_get_iter_first(m, &iter); ok;
         ok = gtk_tree_model_iter_next(m, &iter)) {
        char *e = nullptr, *type = nullptr;
        gtk_tree_model_get(m, &iter, FT_EXT, &e, FT_TYPE, &type, FT_DESC, desc, -1);
        bool match = g_strcmp0(e, ext) == 0;
        g_free(e);
        if (match)
            return type;
        g_free(type);
        g_free(*desc);
        *desc = nullptr;
    }
    return nullptr;
}

static void error_box(props *p, const char *what, const GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error != nullptr ? error->message : "");
    xd_message_box(GTK_WINDOW(p->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

static bool update_database(props *p) {
    GError *error = nullptr;
    if (xd_mime_update_database(g_get_user_data_dir(), &error))
        return true;
    error_box(p, "The file types could not be updated (update-mime-database, from "
                 "shared-mime-info).",
              error);
    g_error_free(error);
    return false;
}

/* The New dialog's state. */
typedef struct new_ext {
    GtkWidget *entry, *advanced, *assoc_label, *assoc;
} new_ext;

static void on_advanced([[maybe_unused]] GtkButton *b, gpointer data) {
    new_ext *n = data;
    bool show = !gtk_widget_get_visible(n->assoc);
    gtk_widget_set_visible(n->assoc_label, show);
    gtk_widget_set_visible(n->assoc, show);
    gtk_button_set_label(GTK_BUTTON(n->advanced), show ? "<< _Advanced" : "_Advanced >>");
}

/* A pair for the Associated File Type list. */
typedef struct type_name {
    char *desc, *type;
} type_name;

static int compare_names(gconstpointer a, gconstpointer b) {
    const type_name *x = *(type_name *const *)a, *y = *(type_name *const *)b;
    return g_utf8_collate(x->desc, y->desc);
}

static void type_name_free(gpointer data) {
    type_name *t = data;
    g_free(t->desc);
    g_free(t->type);
    g_free(t);
}

/* XP's Associated File Type list: <New>, then the types by name. */
static GtkWidget *types_combo(props *p) {
    GPtrArray *names = g_ptr_array_new_with_free_func(type_name_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GtkTreeModel *m = GTK_TREE_MODEL(p->types);
    GtkTreeIter iter;
    for (bool ok = gtk_tree_model_get_iter_first(m, &iter); ok;
         ok = gtk_tree_model_iter_next(m, &iter)) {
        type_name *t = g_new0(type_name, 1);
        gtk_tree_model_get(m, &iter, FT_TYPE, &t->type, FT_DESC, &t->desc, -1);
        if (t->type == nullptr || g_hash_table_contains(seen, t->type)) {
            type_name_free(t);
            continue;
        }
        if (t->desc == nullptr)
            t->desc = g_strdup(t->type);
        g_hash_table_add(seen, t->type);
        g_ptr_array_add(names, t);
    }
    g_hash_table_unref(seen);
    g_ptr_array_sort(names, compare_names);
    GtkWidget *c = gtk_combo_box_text_new();
    /* Long names don't widen the dialog. */
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(c));
    for (GList *l = cells; l != nullptr; l = l->next)
        g_object_set(l->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 36, nullptr);
    g_list_free(cells);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), "", "<New>");
    for (guint i = 0; i < names->len; i++) {
        const type_name *t = g_ptr_array_index(names, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), t->type, t->desc);
    }
    g_ptr_array_unref(names);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), "");
    return c;
}

/* Registers `ext` for the type chosen (or a new one), moving it from the
 * type that has it now if the user agrees. */
static bool create_extension(props *p, GtkWindow *parent, const char *ext, const char *chosen) {
    char *upper = g_utf8_strup(ext, -1);
    char *type = chosen[0] != '\0' ? g_strdup(chosen) : xd_mime_type_for_new_extension(ext);
    char *comment = chosen[0] != '\0' ? nullptr : g_strdup_printf("%s File", upper);
    char *owner_desc = nullptr;
    char *owner = type_of_extension(p, upper, &owner_desc);
    char **dirs = xd_mime_data_dirs();
    GError *error = nullptr;
    bool back = false;  /* the user said No: back to the dialog */
    bool moved = false; /* the database files changed */
    if (owner == nullptr || strcmp(owner, type) != 0) {
        bool free_ext = owner == nullptr;
        if (owner != nullptr) {
            char *desc = comment != nullptr ? g_strdup(comment)
                                            : g_content_type_get_description(type);
            char *q = g_strdup_printf("The extension '%s' is already associated with the file "
                                      "type '%s'. Do you want to associate it with '%s' "
                                      "instead?",
                                      upper, owner_desc, desc);
            g_free(desc);
            back = xd_message_box(parent, TITLE, XD_MB_QUESTION, XD_MB_YES_NO, TRUE, q) !=
                   GTK_RESPONSE_YES;
            g_free(q);
            bool gone = false;
            if (!back) {
                free_ext = xd_mime_remove_extension((const char *const *)dirs, owner, ext, &gone,
                                                    &error);
                if (!free_ext)
                    error_box(p, "The extension could not be removed from its file type.",
                              error);
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
                error_box(p, "The extension could not be registered.", error);
            g_clear_error(&error);
        }
    }
    if (moved && update_database(p)) {
        if (owner != nullptr)
            remove_rows(p, upper, owner);
        remove_rows(p, upper, type);
        add_type_rows(p, type, upper);
    }
    if (!back)
        select_row(p, upper, type);
    g_strfreev(dirs);
    g_free(owner);
    g_free(owner_desc);
    g_free(comment);
    g_free(type);
    g_free(upper);
    return !back;
}

/* New: XP's Create New Extension. */
static void on_new([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), "Create New Extension");
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(p->window));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    new_ext n = {0};
    GtkWidget *l = gtk_label_new_with_mnemonic("File _Extension:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    n.entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(n.entry), 33);
    gtk_entry_set_activates_default(GTK_ENTRY(n.entry), TRUE);
    gtk_widget_set_hexpand(n.entry, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), n.entry);
    gtk_grid_attach(GTK_GRID(grid), l, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), n.entry, 1, 0, 1, 1);
    n.assoc_label = gtk_label_new_with_mnemonic("Associated File _Type:");
    gtk_label_set_xalign(GTK_LABEL(n.assoc_label), 0);
    n.assoc = types_combo(p);
    gtk_widget_set_size_request(n.assoc, 260, -1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(n.assoc_label), n.assoc);
    gtk_grid_attach(GTK_GRID(grid), n.assoc_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), n.assoc, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid, TRUE, TRUE, 0);
    /* Advanced >> on the left of the buttons, as XP has it. */
    n.advanced = gtk_button_new_with_mnemonic("_Advanced >>");
    g_signal_connect(n.advanced, "clicked", G_CALLBACK(on_advanced), &n);
    enum { RESPONSE_ADVANCED = 1 };
    gtk_dialog_add_action_widget(GTK_DIALOG(d), n.advanced, RESPONSE_ADVANCED);
    gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(gtk_widget_get_parent(n.advanced)),
                                       n.advanced, TRUE);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_widget_show_all(d);
    gtk_widget_hide(n.assoc_label);
    gtk_widget_hide(n.assoc);

    for (int r; (r = gtk_dialog_run(GTK_DIALOG(d))) == GTK_RESPONSE_OK || r == RESPONSE_ADVANCED;) {
        if (r == RESPONSE_ADVANCED)
            continue; /* on_advanced did it */
        char *ext = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(n.entry))));
        const char *bare = ext[0] == '.' ? ext + 1 : ext;
        bool valid = xd_mime_extension_valid(bare);
        if (!valid)
            xd_message_box(GTK_WINDOW(d), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type an extension of letters and digits, without spaces or dots, "
                           "such as XYZ.");
        const char *chosen = gtk_combo_box_get_active_id(GTK_COMBO_BOX(n.assoc));
        bool finished = valid && create_extension(p, GTK_WINDOW(d), bare,
                                                  chosen != nullptr ? chosen : "");
        g_free(ext);
        if (finished)
            break;
    }
    gtk_widget_destroy(d);
}

/* Delete: the extension leaves its type; a type of the user's own that
 * has no extension left goes, with its program. */
static void on_delete([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    char *ext = nullptr, *desc = nullptr, *type = nullptr;
    if (!selected_type(p, &ext, &desc, &type))
        return;
    int r = xd_message_box(GTK_WINDOW(p->window), "File Types", XD_MB_WARNING, XD_MB_YES_NO, TRUE,
                           "If you remove a file name extension, you will not be able to open "
                           "files with this extension by double-clicking their icons.\n\n"
                           "Are you sure you want to remove this extension?");
    if (r == GTK_RESPONSE_YES) {
        char **dirs = xd_mime_data_dirs();
        GError *error = nullptr;
        bool gone = false;
        if (!xd_mime_remove_extension((const char *const *)dirs, type, ext, &gone, &error)) {
            error_box(p, "The extension could not be removed.", error);
            g_error_free(error);
        } else {
            if (gone)
                g_app_info_reset_type_associations(type);
            if (update_database(p)) {
                remove_rows(p, ext, type);
                show_type_details(p);
            }
        }
        g_strfreev(dirs);
    }
    g_free(ext);
    g_free(desc);
    g_free(type);
}

static GtkTreeViewColumn *types_column(props *p, const char *title, int text_col, bool icon) {
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
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->types_view), col);
    return col;
}

static GtkWidget *types_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *label = gtk_label_new_with_mnemonic("Registered file _types:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, FALSE, 0);

    p->types = gtk_list_store_new(FT_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING);
    p->types_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->types));
    g_object_unref(p->types);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(p->types_view), FT_EXT);
    GtkTreeViewColumn *ext = types_column(p, "Extensions", FT_EXT, true);
    gtk_tree_view_column_set_fixed_width(ext, 110);
    gtk_tree_view_column_set_sizing(ext, GTK_TREE_VIEW_COLUMN_FIXED);
    types_column(p, "File Types", FT_DESC, false);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->types_view);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->types_view)), "changed",
                     G_CALLBACK(on_type_selected), p);
    g_signal_connect(p->types_view, "map", G_CALLBACK(on_types_mapped), p);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 170);
    gtk_container_add(GTK_CONTAINER(scroll), p->types_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    /* New and Delete change the user's MIME database (filetypes.h). */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->delete = gtk_button_new_with_mnemonic("_Delete");
    GtkWidget *add = gtk_button_new_with_mnemonic("N_ew");
    gtk_widget_set_size_request(p->delete, 75, -1);
    gtk_widget_set_size_request(add, 75, -1);
    g_signal_connect(add, "clicked", G_CALLBACK(on_new), p);
    g_signal_connect(p->delete, "clicked", G_CALLBACK(on_delete), p);
    gtk_box_pack_end(GTK_BOX(row), p->delete, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *opens = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(opens), gtk_label_new("Opens with:"), FALSE, FALSE, 0);
    p->opens_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(p->opens_icon), 16); /* even for an icon file */
    gtk_box_pack_start(GTK_BOX(opens), p->opens_icon, FALSE, FALSE, 0);
    p->opens_name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->opens_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->opens_name), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(opens), p->opens_name, TRUE, TRUE, 0);
    p->change = gtk_button_new_with_mnemonic("_Change...");
    g_signal_connect(p->change, "clicked", G_CALLBACK(on_change), p);
    gtk_box_pack_end(GTK_BOX(opens), p->change, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details), opens, FALSE, FALSE, 0);
    p->details = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(p->details), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(p->details), 52);
    gtk_label_set_xalign(GTK_LABEL(p->details), 0);
    gtk_label_set_lines(GTK_LABEL(p->details), 3);
    gtk_box_pack_start(GTK_BOX(details), p->details, FALSE, FALSE, 0);
    p->details_frame = frame("Details", details);
    gtk_box_pack_start(GTK_BOX(page), p->details_frame, FALSE, FALSE, 0);
    show_type_details(p);
    return page;
}

/* ---- applying ---------------------------------------------------------------------------- */

static void show_settings(props *p) {
    xd_settings s;
    xd_settings_load(&s);
    show_general(p, &s.folders);
    p->updating = true;
    show_advanced(p, &s.folders);
    p->updating = false;
}

/* Saves over the file as it is now: other applets and xd-files' Show
 * Hidden Files may have changed it since. */
static bool apply(props *p) {
    xd_settings s;
    xd_settings_load(&s);
    xd_folder_settings *f = &s.folders;
    f->new_window = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->own_window));
    f->new_tabs = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->new_tabs));
    f->single_click = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->single_click));
    f->full_path_address = f->full_path_title = f->show_hidden = f->hide_extensions =
        f->remember_views = false;
    read_advanced(p, f);
    if (!xd_settings_save(&s)) {
        xd_message_box(GTK_WINDOW(p->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The folder options could not be saved.");
        return false;
    }
    gtk_widget_set_sensitive(p->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !apply(p))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
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
    static props p;
    for (size_t i = 0; view != nullptr && i < XD_ARRAY_LEN(view_names); i++)
        if (g_ascii_strcasecmp(view, view_names[i]) == 0)
            p.current_view = view_names[i];
    g_free(view);

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "folder");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(&p),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), view_page(&p),
                             gtk_label_new_with_mnemonic("_View"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), types_page(&p),
                             gtk_label_new_with_mnemonic("File _Types"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));
    show_settings(&p);
    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_main();
    return EXIT_SUCCESS;
}
