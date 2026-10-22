/* SPDX-License-Identifier: MIT
 * xd-fonts: Windows XP's Fonts folder, as a window of its own. Lists the
 * faces fontconfig knows (Large Icons, List, Details; Hide Variations
 * shows each family's regular face), opens XP's font viewer on one,
 * prints its sample sheet (File > Print, the viewer's Print...),
 * installs fonts into the user's font folder ($XDG_DATA_HOME/fonts) and
 * deletes them from there. System fonts can't be deleted: that's the
 * package manager's job.
 *
 *   xd-fonts         the folder
 *   xd-fonts FILE    the font viewer on FILE (what XP's fontview.exe did)
 */
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <glib/gstdio.h>
#include <stdlib.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "fontlist.h"
#include "viewer.h"

static constexpr GdkModifierType NO_MODS = (GdkModifierType)0;

enum { F_ICON, F_NAME, F_FILE, F_BASENAME, F_SIZE, F_DATE, F_INDEX, F_USER, F_PRIMARY, F_COUNT };
enum { VIEW_LARGE, VIEW_LIST, VIEW_DETAILS, VIEW_COUNT };
static const char *const view_labels[VIEW_COUNT] = {"Lar_ge Icons", "_List", "_Details"};

typedef struct fonts {
    GtkWidget *window, *stack, *status;
    GtkWidget *views[VIEW_COUNT]; /* two icon views and a tree view */
    GtkWidget *pages[VIEW_COUNT]; /* their scrolled windows, in the stack */
    GtkWidget *view_items[VIEW_COUNT];
    GtkWidget *open_item, *print_item, *delete_item;
    GtkListStore *store;
    GtkTreeModel *filter;
    int view;
    bool hide_variations;
    bool syncing;
} fonts;

/* ---- the list ----------------------------------------------------------------------- */

static char *size_text(goffset size) {
    guint64 kb = ((guint64)size + 1023) / 1024;
    return g_strdup_printf("%" G_GUINT64_FORMAT " KB", kb);
}

static void fill(fonts *font_folder) {
    gtk_list_store_clear(font_folder->store);
    GPtrArray *list = xd_fonts_list();
    for (guint i = 0; i < list->len; i++) {
        const xd_font *font = g_ptr_array_index(list, i);
        char *type = g_content_type_guess(font->file, nullptr, 0, nullptr);
        GIcon *icon = g_content_type_get_icon(type);
        char *base = g_path_get_basename(font->file);
        GStatBuf st;
        bool ok = g_stat(font->file, &st) == 0;
        char *size = ok ? size_text(st.st_size) : g_strdup("");
        char *date = nullptr;
        if (ok) {
            GDateTime *dt = g_date_time_new_from_unix_local(st.st_mtime);
            date = dt != nullptr ? g_date_time_format(dt, "%x %H:%M") : nullptr;
            if (dt != nullptr)
                g_date_time_unref(dt);
        }
        gtk_list_store_insert_with_values(
            font_folder->store, nullptr, -1, F_ICON, icon, F_NAME, font->name, F_FILE, font->file,
            F_BASENAME, base, F_SIZE, size, F_DATE, date != nullptr ? date : "", F_INDEX,
            font->index, F_USER, font->user, F_PRIMARY, font->primary, -1);
        g_free(date);
        g_free(size);
        g_free(base);
        g_object_unref(icon);
        g_free(type);
    }
    g_ptr_array_unref(list);
}

static gboolean visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    fonts *font_folder = data;
    gboolean primary = TRUE;
    if (font_folder->hide_variations)
        gtk_tree_model_get(model, iter, F_PRIMARY, &primary, -1);
    return primary;
}

/* The selected rows, as paths in the filter model. */
static GList *selected_paths(fonts *font_folder) {
    GtkWidget *view = font_folder->views[font_folder->view];
    if (GTK_IS_ICON_VIEW(view))
        return gtk_icon_view_get_selected_items(GTK_ICON_VIEW(view));
    return gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)),
                                                nullptr);
}

static guint count_rows(GtkTreeModel *model) {
    return (guint)gtk_tree_model_iter_n_children(model, nullptr);
}

static void update_status(fonts *font_folder) {
    GList *sel = selected_paths(font_folder);
    guint count = g_list_length(sel);
    bool deletable = count > 0;
    for (GList *list = sel; list != nullptr; list = list->next) {
        GtkTreeIter iter;
        gboolean user = FALSE;
        if (gtk_tree_model_get_iter(font_folder->filter, &iter, list->data))
            gtk_tree_model_get(font_folder->filter, &iter, F_USER, &user, -1);
        deletable = deletable && user;
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    gtk_widget_set_sensitive(font_folder->delete_item, deletable);
    gtk_widget_set_sensitive(font_folder->open_item, count > 0);
    gtk_widget_set_sensitive(font_folder->print_item, count > 0);
    char *text = count > 0 ? g_strdup_printf("%u object(s) selected", count)
                           : g_strdup_printf("%u font(s)", count_rows(font_folder->filter));
    gtk_label_set_text(GTK_LABEL(font_folder->status), text);
    g_free(text);
}

static void reload(fonts *font_folder) {
    fill(font_folder);
    update_status(font_folder);
}

/* ---- opening ---------------------------------------------------------------------------- */

static void open_path(fonts *font_folder, GtkTreePath *path) {
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(font_folder->filter, &iter, path))
        return;
    char *file = nullptr;
    int index = 0;
    gtk_tree_model_get(font_folder->filter, &iter, F_FILE, &file, F_INDEX, &index, -1);
    xd_font_viewer_new(GTK_WINDOW(font_folder->window), file, index);
    g_free(file);
}

static void cb_open([[maybe_unused]] GtkWidget *widget, gpointer data) {
    fonts *font_folder = data;
    GList *sel = selected_paths(font_folder);
    for (GList *list = sel; list != nullptr; list = list->next)
        open_path(font_folder, list->data);
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
}

/* Print: each selected font's sample sheet, as XP printed them. */
static void cb_print([[maybe_unused]] GtkWidget *widget, gpointer data) {
    fonts *font_folder = data;
    GList *sel = selected_paths(font_folder);
    for (GList *list = sel; list != nullptr; list = list->next) {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(font_folder->filter, &iter, list->data))
            continue;
        char *file = nullptr;
        int index = 0;
        gtk_tree_model_get(font_folder->filter, &iter, F_FILE, &file, F_INDEX, &index, -1);
        xd_font_print(GTK_WINDOW(font_folder->window), file, index);
        g_free(file);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
}

static void on_icon_activated([[maybe_unused]] GtkIconView *icon_view, GtkTreePath *path,
                              gpointer data) {
    open_path(data, path);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tree_view, GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    open_path(data, path);
}

/* ---- installing ------------------------------------------------------------------------- */

/* Copies one font into the user's font folder; on failure, appends why. */
static bool install_one(const char *src, const char *dir, GString *errors) {
    char *base = g_path_get_basename(src);
    int count = 0;
    FcPattern *pat = FcFreeTypeQuery((const FcChar8 *)src, 0, nullptr, &count);
    bool ok = pat != nullptr;
    if (pat != nullptr)
        FcPatternDestroy(pat);
    if (!ok) {
        g_string_append_printf(errors, "%s is not a valid font file.\n", base);
        g_free(base);
        return false;
    }
    char *dest = g_build_filename(dir, base, nullptr);
    GFile *from = g_file_new_for_path(src);
    GFile *to = g_file_new_for_path(dest);
    GError *err = nullptr;
    if (g_file_test(dest, G_FILE_TEST_EXISTS)) {
        g_string_append_printf(errors, "%s is already installed. To install a new version, "
                                       "first delete the old one.\n",
                               base);
        ok = false;
    } else if (!g_file_copy(from, to, G_FILE_COPY_NONE, nullptr, nullptr, nullptr, &err)) {
        g_string_append_printf(errors, "%s could not be installed: %s\n", base, err->message);
        g_error_free(err);
        ok = false;
    }
    g_object_unref(to);
    g_object_unref(from);
    g_free(dest);
    g_free(base);
    return ok;
}

static void install(fonts *font_folder, GSList *files) {
    char *dir = xd_fonts_user_dir();
    GString *errors = g_string_new(nullptr);
    int installed = 0;
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_string_append_printf(errors, "The font folder %s could not be created.\n", dir);
    } else {
        for (GSList *list = files; list != nullptr; list = list->next)
            installed += install_one(list->data, dir, errors) ? 1 : 0;
    }
    if (installed > 0) {
        xd_fonts_rescan();
        reload(font_folder);
    }
    if (errors->len > 0)
        xd_message_box(GTK_WINDOW(font_folder->window), "Fonts", XD_MB_ERROR, XD_MB_OK, FALSE,
                       errors->str);
    g_string_free(errors, TRUE);
    g_free(dir);
}

static void cb_install([[maybe_unused]] GtkWidget *widget, gpointer data) {
    fonts *font_folder = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Add Fonts", GTK_WINDOW(font_folder->window), GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
        GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 600, 420);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dlg), TRUE);
    GtkFileFilter *fonts_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(fonts_filter, "Font files (*.ttf, *.otf, *.ttc)");
    static const char *const patterns[] = {"*.ttf", "*.TTF", "*.otf", "*.OTF", "*.ttc", "*.TTC",
                                           "*.otc", "*.OTC"};
    for (size_t i = 0; i < XD_ARRAY_LEN(patterns); i++)
        gtk_file_filter_add_pattern(fonts_filter, patterns[i]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), fonts_filter);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        gtk_widget_destroy(dlg);
        install(font_folder, files);
        g_slist_free_full(files, g_free);
        return;
    }
    gtk_widget_destroy(dlg);
}

/* ---- deleting --------------------------------------------------------------------------- */

static void cb_delete([[maybe_unused]] GtkWidget *widget, gpointer data) {
    fonts *font_folder = data;
    if (!gtk_widget_get_sensitive(font_folder->delete_item))
        return;
    /* The files (a collection holds several faces). */
    GHashTable *files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GList *sel = selected_paths(font_folder);
    for (GList *list = sel; list != nullptr; list = list->next) {
        GtkTreeIter iter;
        char *file = nullptr;
        gboolean user = FALSE;
        if (!gtk_tree_model_get_iter(font_folder->filter, &iter, list->data))
            continue;
        gtk_tree_model_get(font_folder->filter, &iter, F_FILE, &file, F_USER, &user, -1);
        if (user && xd_font_is_user(file))
            g_hash_table_add(files, file);
        else
            g_free(file);
    }
    guint count = g_list_length(sel);
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    if (g_hash_table_size(files) == 0) {
        g_hash_table_unref(files);
        return;
    }
    const char *question = count == 1 ? "Are you sure you want to delete this font?"
                                      : "Are you sure you want to delete these fonts?";
    if (xd_message_box(GTK_WINDOW(font_folder->window), "Fonts", XD_MB_QUESTION, XD_MB_YES_NO, TRUE,
                       question) != GTK_RESPONSE_YES) {
        g_hash_table_unref(files);
        return;
    }
    /* To the Recycle Bin, so a mistake can be undone. */
    GString *errors = g_string_new(nullptr);
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, files);
    while (g_hash_table_iter_next(&it, &key, nullptr)) {
        GFile *file = g_file_new_for_path(key);
        GError *err = nullptr;
        if (!g_file_trash(file, nullptr, &err)) {
            char *base = g_path_get_basename(key);
            g_string_append_printf(errors, "%s could not be deleted: %s\n", base, err->message);
            g_free(base);
            g_error_free(err);
        }
        g_object_unref(file);
    }
    xd_fonts_rescan();
    reload(font_folder);
    if (errors->len > 0)
        xd_message_box(GTK_WINDOW(font_folder->window), "Fonts", XD_MB_ERROR, XD_MB_OK, FALSE,
                       errors->str);
    g_string_free(errors, TRUE);
    g_hash_table_unref(files);
}

/* ---- views ------------------------------------------------------------------------------- */

static void set_view(fonts *font_folder, int view) {
    font_folder->view = view;
    gtk_stack_set_visible_child(GTK_STACK(font_folder->stack), font_folder->pages[view]);
    font_folder->syncing = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(font_folder->view_items[view]), TRUE);
    font_folder->syncing = false;
    update_status(font_folder);
}

static void on_view_item(GtkCheckMenuItem *item, gpointer data) {
    fonts *font_folder = data;
    if (font_folder->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_view(font_folder, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-view")));
}

static void on_hide_variations(GtkCheckMenuItem *item, gpointer data) {
    fonts *font_folder = data;
    font_folder->hide_variations = gtk_check_menu_item_get_active(item);
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(font_folder->filter));
    update_status(font_folder);
}

static void cb_refresh([[maybe_unused]] GtkWidget *widget, gpointer data) {
    xd_fonts_rescan();
    reload(data);
}

static void cb_close([[maybe_unused]] GtkWidget *widget, gpointer data) {
    gtk_widget_destroy(((fonts *)data)->window);
}

static void on_selection_changed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    update_status(data);
}

static void on_tree_selection([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    update_status(data);
}

/* GTK deactivates a menu before running the chosen item: destroy it after. */
static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

/* Right-click: Open, Print and Delete, on what's under the pointer. */
static void popup(fonts *font_folder, GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *open = gtk_menu_item_new_with_mnemonic("_Open");
    g_signal_connect(open, "activate", G_CALLBACK(cb_open), font_folder);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);
    GtkWidget *print = gtk_menu_item_new_with_mnemonic("_Print");
    g_signal_connect(print, "activate", G_CALLBACK(cb_print), font_folder);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), print);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *del = gtk_menu_item_new_with_mnemonic("_Delete");
    g_signal_connect(del, "activate", G_CALLBACK(cb_delete), font_folder);
    gtk_widget_set_sensitive(del, gtk_widget_get_sensitive(font_folder->delete_item));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), del);
    gtk_menu_attach_to_widget(GTK_MENU(menu), font_folder->views[font_folder->view], nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

static gboolean on_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    fonts *font_folder = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    /* Select what's under the pointer first, as Explorer does. */
    GtkTreePath *path = nullptr;
    if (GTK_IS_ICON_VIEW(widget)) {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget), (int)ev->x, (int)ev->y);
        if (path != nullptr && !gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), path)) {
            gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
            gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
        }
    } else if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path,
                                             nullptr, nullptr, nullptr)) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
        }
    }
    if (path == nullptr)
        return FALSE;
    gtk_tree_path_free(path);
    popup(font_folder, ev);
    return TRUE;
}

static GtkWidget *icon_view(fonts *font_folder, bool large) {
    GtkWidget *view = gtk_icon_view_new_with_model(font_folder->filter);
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(view), GTK_SELECTION_MULTIPLE);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "stock-size", large ? GTK_ICON_SIZE_DIALOG : GTK_ICON_SIZE_MENU, nullptr);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    if (large) {
        g_object_set(text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 90, "alignment",
                     PANGO_ALIGN_CENTER, "xalign", 0.5, nullptr);
        gtk_icon_view_set_item_width(GTK_ICON_VIEW(view), 96);
    } else {
        g_object_set(text, "xalign", 0.0, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 32,
                     nullptr);
        gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(view), GTK_ORIENTATION_HORIZONTAL);
        gtk_icon_view_set_item_padding(GTK_ICON_VIEW(view), 1);
        gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(view), 0);
    }
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(view), pix, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(view), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(view), pix, "gicon", F_ICON);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(view), text, "text", F_NAME);
    g_signal_connect(view, "item-activated", G_CALLBACK(on_icon_activated), font_folder);
    g_signal_connect(view, "selection-changed", G_CALLBACK(on_selection_changed), font_folder);
    g_signal_connect(view, "button-press-event", G_CALLBACK(on_button), font_folder);
    return view;
}

static void text_column(GtkWidget *tree, const char *title, int col, int width, bool icon) {
    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(column, title);
    if (icon) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(column, pix, FALSE);
        gtk_tree_view_column_add_attribute(column, pix, "gicon", F_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    if (col == F_SIZE)
        g_object_set(text, "xalign", 1.0, nullptr);
    gtk_tree_view_column_pack_start(column, text, TRUE);
    gtk_tree_view_column_add_attribute(column, text, "text", col);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
}

static GtkWidget *details_view(fonts *font_folder) {
    GtkWidget *view = gtk_tree_view_new_with_model(font_folder->filter);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(view), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(view), F_NAME);
    text_column(view, "Font Name", F_NAME, 280, true);
    text_column(view, "Filename", F_BASENAME, 170, false);
    text_column(view, "Size", F_SIZE, 70, false);
    text_column(view, "Modified", F_DATE, 130, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_tree_selection), font_folder);
    g_signal_connect(view, "row-activated", G_CALLBACK(on_row_activated), font_folder);
    g_signal_connect(view, "button-press-event", G_CALLBACK(on_button), font_folder);
    return view;
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, fonts *font_folder,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, font_folder);
    if (key != 0)
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

static GtkWidget *build_menus(fonts *font_folder, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    font_folder->open_item =
        menu_item(file, "_Open", G_CALLBACK(cb_open), font_folder, accel, 0, NO_MODS);
    font_folder->print_item =
        menu_item(file, "_Print", G_CALLBACK(cb_print), font_folder, accel, 0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "_Install New Font...", G_CALLBACK(cb_install), font_folder, accel, 0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    font_folder->delete_item = menu_item(file, "_Delete", G_CALLBACK(cb_delete), font_folder, accel,
                                         GDK_KEY_Delete, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "_Close", G_CALLBACK(cb_close), font_folder, accel, GDK_KEY_w,
              GDK_CONTROL_MASK);

    GtkWidget *view = submenu(bar, "_View");
    GSList *group = nullptr;
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *radio = gtk_radio_menu_item_new_with_mnemonic(group, view_labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
        g_object_set_data(G_OBJECT(radio), "xd-view", GINT_TO_POINTER(i));
        g_signal_connect(radio, "toggled", G_CALLBACK(on_view_item), font_folder);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), radio);
        font_folder->view_items[i] = radio;
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(view), gtk_separator_menu_item_new());
    GtkWidget *hide = gtk_check_menu_item_new_with_mnemonic(
        "_Hide Variations (Bold, Italic, etc.)");
    g_signal_connect(hide, "toggled", G_CALLBACK(on_hide_variations), font_folder);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), hide);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), gtk_separator_menu_item_new());
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), font_folder, accel, GDK_KEY_F5, NO_MODS);
    return bar;
}

static void open_folder(void) {
    static fonts font_folder;
    font_folder.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(font_folder.window), "Fonts");
    gtk_window_set_icon_name(GTK_WINDOW(font_folder.window), "preferences-desktop-font");
    gtk_window_set_default_size(GTK_WINDOW(font_folder.window), 720, 500);
    g_signal_connect(font_folder.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(font_folder.window), accel);

    font_folder.store = gtk_list_store_new(F_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                           G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                                           G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    font_folder.filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(font_folder.store), nullptr);
    g_object_unref(font_folder.store);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(font_folder.filter), visible,
                                           &font_folder, nullptr);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(font_folder.window), box);
    gtk_box_pack_start(GTK_BOX(box), build_menus(&font_folder, accel), FALSE, FALSE, 0);
    font_folder.stack = gtk_stack_new();
    font_folder.views[VIEW_LARGE] = icon_view(&font_folder, true);
    font_folder.views[VIEW_LIST] = icon_view(&font_folder, false);
    font_folder.views[VIEW_DETAILS] = details_view(&font_folder);
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(scroll), font_folder.views[i]);
        gtk_stack_add_named(GTK_STACK(font_folder.stack), scroll, view_labels[i]);
        font_folder.pages[i] = scroll;
    }
    gtk_box_pack_start(GTK_BOX(box), font_folder.stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    font_folder.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(font_folder.status), 0);
    gtk_widget_set_margin_start(font_folder.status, 6);
    gtk_widget_set_margin_top(font_folder.status, 2);
    gtk_widget_set_margin_bottom(font_folder.status, 2);
    gtk_box_pack_start(GTK_BOX(box), font_folder.status, FALSE, FALSE, 0);

    fill(&font_folder);
    gtk_widget_show_all(font_folder.window);
    set_view(&font_folder, VIEW_LARGE);
    gtk_widget_grab_focus(font_folder.views[VIEW_LARGE]);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Fonts");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    if (argc > 1) {
        /* The viewer alone; done when it closes. */
        GtkWidget *viewer = xd_font_viewer_new(nullptr, argv[1], 0);
        if (viewer == nullptr)
            return EXIT_FAILURE;
        g_signal_connect(viewer, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    } else {
        open_folder();
    }
    gtk_main();
    return EXIT_SUCCESS;
}
