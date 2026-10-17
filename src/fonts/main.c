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

static void fill(fonts *f) {
    gtk_list_store_clear(f->store);
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
        gtk_list_store_insert_with_values(f->store, nullptr, -1, F_ICON, icon, F_NAME, font->name,
                                          F_FILE, font->file, F_BASENAME, base, F_SIZE, size,
                                          F_DATE, date != nullptr ? date : "", F_INDEX,
                                          font->index, F_USER, font->user, F_PRIMARY,
                                          font->primary, -1);
        g_free(date);
        g_free(size);
        g_free(base);
        g_object_unref(icon);
        g_free(type);
    }
    g_ptr_array_unref(list);
}

static gboolean visible(GtkTreeModel *m, GtkTreeIter *iter, gpointer data) {
    fonts *f = data;
    gboolean primary = TRUE;
    if (f->hide_variations)
        gtk_tree_model_get(m, iter, F_PRIMARY, &primary, -1);
    return primary;
}

/* The selected rows, as paths in the filter model. */
static GList *selected_paths(fonts *f) {
    GtkWidget *v = f->views[f->view];
    if (GTK_IS_ICON_VIEW(v))
        return gtk_icon_view_get_selected_items(GTK_ICON_VIEW(v));
    return gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(v)),
                                                nullptr);
}

static guint count_rows(GtkTreeModel *m) {
    return (guint)gtk_tree_model_iter_n_children(m, nullptr);
}

static void update_status(fonts *f) {
    GList *sel = selected_paths(f);
    guint n = g_list_length(sel);
    bool deletable = n > 0;
    for (GList *l = sel; l != nullptr; l = l->next) {
        GtkTreeIter iter;
        gboolean user = FALSE;
        if (gtk_tree_model_get_iter(f->filter, &iter, l->data))
            gtk_tree_model_get(f->filter, &iter, F_USER, &user, -1);
        deletable = deletable && user;
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    gtk_widget_set_sensitive(f->delete_item, deletable);
    gtk_widget_set_sensitive(f->open_item, n > 0);
    gtk_widget_set_sensitive(f->print_item, n > 0);
    char *text = n > 0 ? g_strdup_printf("%u object(s) selected", n)
                       : g_strdup_printf("%u font(s)", count_rows(f->filter));
    gtk_label_set_text(GTK_LABEL(f->status), text);
    g_free(text);
}

static void reload(fonts *f) {
    fill(f);
    update_status(f);
}

/* ---- opening ---------------------------------------------------------------------------- */

static void open_path(fonts *f, GtkTreePath *path) {
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(f->filter, &iter, path))
        return;
    char *file = nullptr;
    int index = 0;
    gtk_tree_model_get(f->filter, &iter, F_FILE, &file, F_INDEX, &index, -1);
    xd_font_viewer_new(GTK_WINDOW(f->window), file, index);
    g_free(file);
}

static void cb_open([[maybe_unused]] GtkWidget *w, gpointer data) {
    fonts *f = data;
    GList *sel = selected_paths(f);
    for (GList *l = sel; l != nullptr; l = l->next)
        open_path(f, l->data);
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
}

/* Print: each selected font's sample sheet, as XP printed them. */
static void cb_print([[maybe_unused]] GtkWidget *w, gpointer data) {
    fonts *f = data;
    GList *sel = selected_paths(f);
    for (GList *l = sel; l != nullptr; l = l->next) {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(f->filter, &iter, l->data))
            continue;
        char *file = nullptr;
        int index = 0;
        gtk_tree_model_get(f->filter, &iter, F_FILE, &file, F_INDEX, &index, -1);
        xd_font_print(GTK_WINDOW(f->window), file, index);
        g_free(file);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
}

static void on_icon_activated([[maybe_unused]] GtkIconView *v, GtkTreePath *path, gpointer data) {
    open_path(data, path);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *v, GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
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

static void install(fonts *f, GSList *files) {
    char *dir = xd_fonts_user_dir();
    GString *errors = g_string_new(nullptr);
    int installed = 0;
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_string_append_printf(errors, "The font folder %s could not be created.\n", dir);
    } else {
        for (GSList *l = files; l != nullptr; l = l->next)
            installed += install_one(l->data, dir, errors) ? 1 : 0;
    }
    if (installed > 0) {
        xd_fonts_rescan();
        reload(f);
    }
    if (errors->len > 0)
        xd_message_box(GTK_WINDOW(f->window), "Fonts", XD_MB_ERROR, XD_MB_OK, FALSE,
                       errors->str);
    g_string_free(errors, TRUE);
    g_free(dir);
}

static void cb_install([[maybe_unused]] GtkWidget *w, gpointer data) {
    fonts *f = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Add Fonts", GTK_WINDOW(f->window),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
                                                 GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_ACCEPT,
                                                 nullptr);
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
        install(f, files);
        g_slist_free_full(files, g_free);
        return;
    }
    gtk_widget_destroy(dlg);
}

/* ---- deleting --------------------------------------------------------------------------- */

static void cb_delete([[maybe_unused]] GtkWidget *w, gpointer data) {
    fonts *f = data;
    if (!gtk_widget_get_sensitive(f->delete_item))
        return;
    /* The files (a collection holds several faces). */
    GHashTable *files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GList *sel = selected_paths(f);
    for (GList *l = sel; l != nullptr; l = l->next) {
        GtkTreeIter iter;
        char *file = nullptr;
        gboolean user = FALSE;
        if (!gtk_tree_model_get_iter(f->filter, &iter, l->data))
            continue;
        gtk_tree_model_get(f->filter, &iter, F_FILE, &file, F_USER, &user, -1);
        if (user && xd_font_is_user(file))
            g_hash_table_add(files, file);
        else
            g_free(file);
    }
    guint n = g_list_length(sel);
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    if (g_hash_table_size(files) == 0) {
        g_hash_table_unref(files);
        return;
    }
    const char *question = n == 1 ? "Are you sure you want to delete this font?"
                                  : "Are you sure you want to delete these fonts?";
    if (xd_message_box(GTK_WINDOW(f->window), "Fonts", XD_MB_QUESTION,
                       XD_MB_YES_NO, TRUE, question) != GTK_RESPONSE_YES) {
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
    reload(f);
    if (errors->len > 0)
        xd_message_box(GTK_WINDOW(f->window), "Fonts", XD_MB_ERROR, XD_MB_OK, FALSE,
                       errors->str);
    g_string_free(errors, TRUE);
    g_hash_table_unref(files);
}

/* ---- views ------------------------------------------------------------------------------- */

static void set_view(fonts *f, int view) {
    f->view = view;
    gtk_stack_set_visible_child(GTK_STACK(f->stack), f->pages[view]);
    f->syncing = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(f->view_items[view]), TRUE);
    f->syncing = false;
    update_status(f);
}

static void on_view_item(GtkCheckMenuItem *item, gpointer data) {
    fonts *f = data;
    if (f->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_view(f, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-view")));
}

static void on_hide_variations(GtkCheckMenuItem *item, gpointer data) {
    fonts *f = data;
    f->hide_variations = gtk_check_menu_item_get_active(item);
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(f->filter));
    update_status(f);
}

static void cb_refresh([[maybe_unused]] GtkWidget *w, gpointer data) {
    xd_fonts_rescan();
    reload(data);
}

static void cb_close([[maybe_unused]] GtkWidget *w, gpointer data) {
    gtk_widget_destroy(((fonts *)data)->window);
}

static void on_selection_changed([[maybe_unused]] GtkWidget *w, gpointer data) {
    update_status(data);
}

static void on_tree_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
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
static void popup(fonts *f, GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *open = gtk_menu_item_new_with_mnemonic("_Open");
    g_signal_connect(open, "activate", G_CALLBACK(cb_open), f);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);
    GtkWidget *print = gtk_menu_item_new_with_mnemonic("_Print");
    g_signal_connect(print, "activate", G_CALLBACK(cb_print), f);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), print);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *del = gtk_menu_item_new_with_mnemonic("_Delete");
    g_signal_connect(del, "activate", G_CALLBACK(cb_delete), f);
    gtk_widget_set_sensitive(del, gtk_widget_get_sensitive(f->delete_item));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), del);
    gtk_menu_attach_to_widget(GTK_MENU(menu), f->views[f->view], nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    fonts *f = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    /* Select what's under the pointer first, as Explorer does. */
    GtkTreePath *path = nullptr;
    if (GTK_IS_ICON_VIEW(w)) {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w), (int)ev->x, (int)ev->y);
        if (path != nullptr && !gtk_icon_view_path_is_selected(GTK_ICON_VIEW(w), path)) {
            gtk_icon_view_unselect_all(GTK_ICON_VIEW(w));
            gtk_icon_view_select_path(GTK_ICON_VIEW(w), path);
        }
    } else if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path,
                                             nullptr, nullptr, nullptr)) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
        if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
        }
    }
    if (path == nullptr)
        return FALSE;
    gtk_tree_path_free(path);
    popup(f, ev);
    return TRUE;
}

static GtkWidget *icon_view(fonts *f, bool large) {
    GtkWidget *v = gtk_icon_view_new_with_model(f->filter);
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(v), GTK_SELECTION_MULTIPLE);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "stock-size", large ? GTK_ICON_SIZE_DIALOG : GTK_ICON_SIZE_MENU, nullptr);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    if (large) {
        g_object_set(text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 90, "alignment",
                     PANGO_ALIGN_CENTER, "xalign", 0.5, nullptr);
        gtk_icon_view_set_item_width(GTK_ICON_VIEW(v), 96);
    } else {
        g_object_set(text, "xalign", 0.0, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 32,
                     nullptr);
        gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(v), GTK_ORIENTATION_HORIZONTAL);
        gtk_icon_view_set_item_padding(GTK_ICON_VIEW(v), 1);
        gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(v), 0);
    }
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v), pix, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(v), pix, "gicon", F_ICON);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(v), text, "text", F_NAME);
    g_signal_connect(v, "item-activated", G_CALLBACK(on_icon_activated), f);
    g_signal_connect(v, "selection-changed", G_CALLBACK(on_selection_changed), f);
    g_signal_connect(v, "button-press-event", G_CALLBACK(on_button), f);
    return v;
}

static void text_column(GtkWidget *tree, const char *title, int col, int width, bool icon) {
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, title);
    if (icon) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(c, pix, FALSE);
        gtk_tree_view_column_add_attribute(c, pix, "gicon", F_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    if (col == F_SIZE)
        g_object_set(text, "xalign", 1.0, nullptr);
    gtk_tree_view_column_pack_start(c, text, TRUE);
    gtk_tree_view_column_add_attribute(c, text, "text", col);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
}

static GtkWidget *details_view(fonts *f) {
    GtkWidget *v = gtk_tree_view_new_with_model(f->filter);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(v), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(v), F_NAME);
    text_column(v, "Font Name", F_NAME, 280, true);
    text_column(v, "Filename", F_BASENAME, 170, false);
    text_column(v, "Size", F_SIZE, 70, false);
    text_column(v, "Modified", F_DATE, 130, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(v));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_tree_selection), f);
    g_signal_connect(v, "row-activated", G_CALLBACK(on_row_activated), f);
    g_signal_connect(v, "button-press-event", G_CALLBACK(on_button), f);
    return v;
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, fonts *f,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, f);
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

static GtkWidget *build_menus(fonts *f, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    f->open_item = menu_item(file, "_Open", G_CALLBACK(cb_open), f, accel, 0, NO_MODS);
    f->print_item = menu_item(file, "_Print", G_CALLBACK(cb_print), f, accel, 0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "_Install New Font...", G_CALLBACK(cb_install), f, accel, 0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    f->delete_item = menu_item(file, "_Delete", G_CALLBACK(cb_delete), f, accel, GDK_KEY_Delete,
                               NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "_Close", G_CALLBACK(cb_close), f, accel, GDK_KEY_w, GDK_CONTROL_MASK);

    GtkWidget *view = submenu(bar, "_View");
    GSList *group = nullptr;
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *radio = gtk_radio_menu_item_new_with_mnemonic(group, view_labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
        g_object_set_data(G_OBJECT(radio), "xd-view", GINT_TO_POINTER(i));
        g_signal_connect(radio, "toggled", G_CALLBACK(on_view_item), f);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), radio);
        f->view_items[i] = radio;
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(view), gtk_separator_menu_item_new());
    GtkWidget *hide = gtk_check_menu_item_new_with_mnemonic(
        "_Hide Variations (Bold, Italic, etc.)");
    g_signal_connect(hide, "toggled", G_CALLBACK(on_hide_variations), f);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), hide);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), gtk_separator_menu_item_new());
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), f, accel, GDK_KEY_F5, NO_MODS);
    return bar;
}

static void open_folder(void) {
    static fonts f;
    f.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(f.window), "Fonts");
    gtk_window_set_icon_name(GTK_WINDOW(f.window), "preferences-desktop-font");
    gtk_window_set_default_size(GTK_WINDOW(f.window), 720, 500);
    g_signal_connect(f.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(f.window), accel);

    f.store = gtk_list_store_new(F_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                                 G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    f.filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(f.store), nullptr);
    g_object_unref(f.store);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(f.filter), visible, &f, nullptr);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(f.window), box);
    gtk_box_pack_start(GTK_BOX(box), build_menus(&f, accel), FALSE, FALSE, 0);
    f.stack = gtk_stack_new();
    f.views[VIEW_LARGE] = icon_view(&f, true);
    f.views[VIEW_LIST] = icon_view(&f, false);
    f.views[VIEW_DETAILS] = details_view(&f);
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(scroll), f.views[i]);
        gtk_stack_add_named(GTK_STACK(f.stack), scroll, view_labels[i]);
        f.pages[i] = scroll;
    }
    gtk_box_pack_start(GTK_BOX(box), f.stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    f.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(f.status), 0);
    gtk_widget_set_margin_start(f.status, 6);
    gtk_widget_set_margin_top(f.status, 2);
    gtk_widget_set_margin_bottom(f.status, 2);
    gtk_box_pack_start(GTK_BOX(box), f.status, FALSE, FALSE, 0);

    fill(&f);
    gtk_widget_show_all(f.window);
    set_view(&f, VIEW_LARGE);
    gtk_widget_grab_focus(f.views[VIEW_LARGE]);
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
