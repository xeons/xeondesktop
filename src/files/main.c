/* SPDX-License-Identifier: MIT
 * xd-files: the file manager, modelled on Windows XP's Explorer windows.
 *
 * GTK3 window: menu bar, toolbar (Back, Forward, Up, Folders, Views),
 * address bar, the Folders tree on the left, XdFolderView on the right and
 * a status bar with the thumbnail size slider.
 */
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <stdlib.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "../settings/state.h"
#include "fileops.h"
#include "folderview.h"
#include "places.h"
#include "properties.h"
#include "sendto.h"
#include "undo.h"

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
    GtkWidget *arrange_item; /* View > Arrange Icons By */
    /* Control Panel: the Switch to Classic/Category View link (in a bar
     * above the view) and View menu item. */
    GtkWidget *control_bar, *control_link, *control_item, *control_sep;
    GtkWidget *hidden_item;  /* View > Show Hidden Files */
    GtkWidget *undo_item;    /* Edit > Undo ... */
    GtkWidget *send_to_item; /* File > Send To */
    /* Menu items that need a selection, or files on the clipboard. */
    GPtrArray *need_selection;
    GPtrArray *need_clipboard;
    /* Not for My Computer or the Recycle Bin (New, Paste, Rename...), and
     * only for the Recycle Bin (Restore, Empty Recycle Bin). */
    GPtrArray *folders_only;
    GPtrArray *trash_only;
    /* Renaming in place: an entry over the item's name. `pending_rename`
     * waits for a new item to appear. */
    GtkWidget *overlay;
    GtkWidget *rename_entry;
    GFile *renaming;
    GFile *pending_rename;
    gboolean committing;
    guint commit_idle;
    GList *back;    /* GFile*, most recent first */
    GList *forward;
    gboolean syncing; /* updating widgets programmatically */
} explorer;

static void navigate(explorer *e, GFile *dir, gboolean record);
static int initial_view = XD_VIEW_ICONS; /* from --view */
static gboolean view_forced;              /* --view given: it wins for the first folder */
static gboolean view_given;               /* --view given: Folder Options' default doesn't apply */
/* Folder Options ([Folders] in desktop.ini), followed live. */
static xd_folder_settings options;
static gboolean properties_only;          /* --properties: just the dialog */
/* --trash, --delete, --rename, --empty-trash, --copy-to, --move-to: one
 * job for the desktop (the shell isn't a GTK program), then exit. */
typedef enum {
    JOB_NONE,
    JOB_TRASH,
    JOB_DELETE,
    JOB_RENAME,
    JOB_EMPTY_TRASH,
    JOB_COPY,
    JOB_MOVE
} cli_job;
static cli_job job;
static char *job_dest; /* --copy-to / --move-to */

/* ---- status and chrome --------------------------------------------------------- */

/* XP's "(Disk free space: 12.3 GB)" for the folder's drive. */
static char *free_space_text(GFile *dir) {
    if (dir == nullptr || xd_place_of(dir) != XD_PLACE_FOLDER || !g_file_is_native(dir))
        return nullptr;
    GFileInfo *fs = g_file_query_filesystem_info(dir, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, nullptr,
                                                 nullptr);
    if (fs == nullptr)
        return nullptr;
    char *size = xd_format_drive_size(
        g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
    g_object_unref(fs);
    char *text = g_strdup_printf(" (Disk free space: %s)", size);
    g_free(size);
    return text;
}

static void update_status(explorer *e) {
    guint sel = xd_folder_view_get_selected_count(e->view);
    guint total = xd_folder_view_get_item_count(e->view);
    char *space = sel > 0 ? nullptr : free_space_text(xd_folder_view_get_location(e->view));
    char *text = sel > 0 ? g_strdup_printf("%u object%s selected", sel, sel == 1 ? "" : "s")
                         : g_strdup_printf("%u object%s%s", total, total == 1 ? "" : "s",
                                           space != nullptr ? space : "");
    g_free(space);
    gtk_label_set_text(GTK_LABEL(e->status), text);
    g_free(text);
    /* Control Panel's items have no size. */
    char *size = xd_place_of(xd_folder_view_get_location(e->view)) == XD_PLACE_CONTROL
                     ? g_strdup("")
                     : xd_format_size(xd_folder_view_get_size(e->view, sel > 0));
    gtk_label_set_text(GTK_LABEL(e->status_size), size);
    g_free(size);
}

/* Control Panel's "Switch to Classic View" / "Switch to Category View":
 * the link above the view and the View menu's item. */
static void update_control_switch(explorer *e) {
    gboolean control = xd_place_of(xd_folder_view_get_location(e->view)) == XD_PLACE_CONTROL;
    const char *label = xd_control_classic_view() ? "Switch to Category View"
                                                  : "Switch to Classic View";
    gtk_widget_set_visible(e->control_bar, control);
    gtk_button_set_label(GTK_BUTTON(e->control_link), label);
    gtk_widget_set_visible(e->control_item, control);
    gtk_widget_set_visible(e->control_sep, control);
    gtk_menu_item_set_label(GTK_MENU_ITEM(e->control_item), label);
}

static void update_chrome(explorer *e) {
    GFile *dir = xd_folder_view_get_location(e->view);
    if (dir == nullptr)
        return; /* nothing loaded yet */
    const char *special = xd_place_title(dir);
    char *parse = special != nullptr ? g_strdup(special) : g_file_get_parse_name(dir);
    char *base = special != nullptr ? nullptr : g_file_get_basename(dir);
    /* Folder Options: the folder's name alone, unless the full path. */
    const char *shown = options.full_path_address || base == nullptr ? parse : base;
    g_object_set_data_full(G_OBJECT(e->address), "xd-shown", g_strdup(shown), g_free);
    e->syncing = TRUE;
    gtk_entry_set_text(GTK_ENTRY(e->address), shown);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(e->address), GTK_ENTRY_ICON_PRIMARY,
                                      xd_place_icon(dir));
    e->syncing = FALSE;
    gboolean short_title = !options.full_path_title && g_file_has_parent(dir, nullptr) &&
                           base != nullptr;
    gtk_window_set_title(GTK_WINDOW(e->window), short_title ? base : parse);
    g_free(base);
    g_free(parse);
    gtk_widget_set_sensitive(e->back_button, e->back != nullptr);
    gtk_widget_set_sensitive(e->forward_button, e->forward != nullptr);
    /* A drive's Up is My Computer, as in XP; so is Control Panel's, whose
     * category pages go up to it. */
    gtk_widget_set_sensitive(e->up_button,
                             special == nullptr || xd_place_of(dir) == XD_PLACE_CONTROL);
    gtk_widget_set_visible(e->slider, xd_folder_view_get_mode(e->view) == XD_VIEW_THUMBNAILS);
    update_control_switch(e);
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
    if (xd_place_of(dir) == XD_PLACE_COMPUTER) {
        GPtrArray *drives = xd_computer_drives();
        for (guint i = 0; i < drives->len; i++) {
            XdDrive *d = g_ptr_array_index(drives, i);
            const char *const *names =
                d->icon != nullptr && G_IS_THEMED_ICON(d->icon)
                    ? g_themed_icon_get_names(G_THEMED_ICON(d->icon))
                    : nullptr;
            add_folder(e, iter, &child, d->root, d->name,
                       names != nullptr && names[0] != nullptr ? names[0] : "drive-harddisk");
        }
        g_ptr_array_unref(drives);
        g_object_unref(dir);
        return;
    }
    if (xd_place_of(dir) == XD_PLACE_TRASH) {
        g_object_unref(dir);
        return;
    }

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
    GFile *computer = xd_place_computer();
    add_folder(e, nullptr, &it, computer, "My Computer", "computer");
    g_object_unref(computer);
    GFile *control = xd_place_control();
    gtk_tree_store_append(e->store, &it, nullptr);
    gtk_tree_store_set(e->store, &it, COL_NAME, "Control Panel", COL_ICON, xd_place_icon(control),
                       COL_FILE, control, COL_LOADED, TRUE, -1);
    g_object_unref(control);
    GFile *trash = xd_place_trash();
    gtk_tree_store_append(e->store, &it, nullptr);
    gtk_tree_store_set(e->store, &it, COL_NAME, "Recycle Bin", COL_ICON, xd_place_icon(trash),
                       COL_FILE, trash, COL_LOADED, TRUE, -1);
    g_object_unref(trash);
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

/* ---- each folder's view, remembered (XP keeps 400) ---------------------------- */

enum { FOLDER_VIEWS_MAX = 400 };
/* Lines: uri, view, sort column, descending[, groups[, Details columns
 * ("-": the defaults), Auto Arrange, Align to Grid]]. */
#define FOLDER_VIEWS "folder-views"
/* Where icons were put with Auto Arrange off: a key file, a group per
 * folder (most recent last), with the view, the names (URI-escaped, so
 * any file name is plain text) and their x, y pairs. */
#define FOLDER_POSITIONS "folder-positions"

static void set_mode(explorer *e, XdViewMode mode);

typedef struct folder_view_setting {
    int mode, sort;
    gboolean desc, groups;
    char *columns; /* nullptr: the defaults */
    gboolean auto_arrange, align_grid;
} folder_view_setting;

static gboolean find_folder_view(const char *uri, folder_view_setting *out) {
    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines(FOLDER_VIEWS, &lines, &n, FOLDER_VIEWS_MAX);
    gboolean found = FALSE;
    for (size_t i = 0; i < n && !found; i++) {
        char **f = g_strsplit(lines[i], "\t", 8);
        guint nf = g_strv_length(f);
        if (nf >= 4 && strcmp(f[0], uri) == 0) {
            long mode = strtol(f[1], nullptr, 10), sort = strtol(f[2], nullptr, 10);
            if (mode >= 0 && mode < XD_VIEW_COUNT && sort >= 0 && sort < XD_SORT_COUNT) {
                *out = (folder_view_setting){
                    .mode = (int)mode,
                    .sort = (int)sort,
                    .desc = strcmp(f[3], "1") == 0,
                    .groups = nf >= 5 && strcmp(f[4], "1") == 0,
                    .columns = nf >= 6 && strcmp(f[5], "-") != 0 ? g_strdup(f[5]) : nullptr,
                    .auto_arrange = nf < 7 || strcmp(f[6], "1") == 0,
                    .align_grid = nf < 8 || strcmp(f[7], "1") == 0,
                };
                found = TRUE;
            }
        }
        g_strfreev(f);
    }
    xd_free_lines(lines, n);
    return found;
}

static GKeyFile *load_positions(char **path_out) {
    xd_strbuf path = {0};
    xd_state_path(FOLDER_POSITIONS, &path);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, xd_strbuf_cstr(&path), G_KEY_FILE_NONE, nullptr);
    if (path_out != nullptr)
        *path_out = g_strdup(xd_strbuf_cstr(&path));
    xd_strbuf_free(&path);
    return kf;
}

/* Keeps where the folder's icons are (Auto Arrange off), or forgets it. */
static void remember_positions(explorer *e, const char *uri) {
    char *path = nullptr;
    GKeyFile *kf = load_positions(&path);
    gboolean had = g_key_file_has_group(kf, uri);
    XdViewMode mode = XD_VIEW_ICONS;
    char **names = nullptr;
    int *xy = nullptr;
    gsize n = xd_folder_view_get_positions(e->view, &mode, &names, &xy);
    if (n > 0 || had) {
        g_key_file_remove_group(kf, uri, nullptr);
        if (n > 0) {
            char **escaped = g_new0(char *, n + 1);
            for (gsize i = 0; i < n; i++)
                escaped[i] = g_uri_escape_string(names[i], nullptr, FALSE);
            g_key_file_set_integer(kf, uri, "View", (int)mode);
            g_key_file_set_string_list(kf, uri, "Names", (const char *const *)escaped, n);
            g_key_file_set_integer_list(kf, uri, "Positions", xy, 2 * n);
            g_strfreev(escaped);
        }
        gsize groups = 0;
        char **all = g_key_file_get_groups(kf, &groups);
        for (gsize i = 0; i + FOLDER_VIEWS_MAX < groups; i++)
            g_key_file_remove_group(kf, all[i], nullptr); /* the least recent */
        g_strfreev(all);
        GError *err = nullptr;
        if (!g_key_file_save_to_file(kf, path, &err)) {
            g_printerr("xd-files: cannot save where the icons are: %s\n", err->message);
            g_error_free(err);
        }
    }
    g_strfreev(names);
    g_free(xy);
    g_key_file_unref(kf);
    g_free(path);
}

/* Puts the folder's icons back where they were (before it's listed). */
static void restore_positions(explorer *e, const char *uri) {
    GKeyFile *kf = load_positions(nullptr);
    gsize n = 0, nxy = 0;
    char **escaped = g_key_file_get_string_list(kf, uri, "Names", &n, nullptr);
    int *xy = g_key_file_get_integer_list(kf, uri, "Positions", &nxy, nullptr);
    int mode = g_key_file_get_integer(kf, uri, "View", nullptr);
    if (escaped != nullptr && xy != nullptr && nxy == 2 * n && mode >= 0 && mode < XD_VIEW_COUNT) {
        char **names = g_new0(char *, n + 1);
        gsize k = 0;
        int *kept = g_new0(int, 2 * n + 1);
        for (gsize i = 0; i < n; i++) {
            char *name = g_uri_unescape_string(escaped[i], nullptr);
            if (name == nullptr)
                continue;
            names[k] = name;
            kept[2 * k] = xy[2 * i];
            kept[2 * k + 1] = xy[2 * i + 1];
            k++;
        }
        xd_folder_view_set_positions(e->view, (XdViewMode)mode, names, kept, k);
        g_strfreev(names);
        g_free(kept);
    }
    g_strfreev(escaped);
    g_free(xy);
    g_key_file_unref(kf);
}

/* Keeps the folder's view (most recent first), unless it's the default
 * and the folder wasn't remembered before. */
static void remember_folder_view(explorer *e) {
    GFile *dir = xd_folder_view_get_location(e->view);
    /* Control Panel always opens in its view's own look (below); with
     * "Remember each folder's view settings" off nothing is kept. */
    if (dir == nullptr || xd_place_of(dir) == XD_PLACE_CONTROL || !options.remember_views)
        return;
    char *uri = g_file_get_uri(dir);
    remember_positions(e, uri);
    XdViewMode view_mode = xd_folder_view_get_mode(e->view);
    int mode = (int)view_mode;
    XdSortColumn sort_col = xd_folder_view_get_sort(e->view);
    int sort = (int)sort_col;
    gboolean desc = xd_folder_view_get_sort_descending(e->view);
    gboolean groups = xd_folder_view_get_groups(e->view);
    char *columns = xd_folder_view_get_columns(e->view);
    gboolean arrange = xd_folder_view_get_auto_arrange(e->view);
    gboolean align = xd_folder_view_get_align_to_grid(e->view);
    folder_view_setting old = {0};
    gboolean known = find_folder_view(uri, &old);
    gboolean is_default = mode == initial_view && sort == XD_SORT_NAME && !desc && !groups &&
                          columns == nullptr && arrange && align;
    gboolean same = known && old.mode == mode && old.sort == sort && old.desc == desc &&
                    old.groups == groups && g_strcmp0(old.columns, columns) == 0 &&
                    old.auto_arrange == arrange && old.align_grid == align;
    g_free(old.columns);
    if ((!known && is_default) || same) {
        g_free(columns);
        g_free(uri);
        return;
    }
    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines(FOLDER_VIEWS, &lines, &n, FOLDER_VIEWS_MAX);
    char **out = g_new0(char *, n + 2);
    size_t k = 0;
    out[k++] = g_strdup_printf("%s\t%d\t%d\t%d\t%d\t%s\t%d\t%d", uri, mode, sort, desc ? 1 : 0,
                               groups ? 1 : 0, columns != nullptr ? columns : "-",
                               arrange ? 1 : 0, align ? 1 : 0);
    g_free(columns);
    char *prefix = g_strconcat(uri, "\t", nullptr);
    for (size_t i = 0; i < n && k < FOLDER_VIEWS_MAX; i++)
        if (!g_str_has_prefix(lines[i], prefix))
            out[k++] = g_strdup(lines[i]);
    if (!xd_state_write_lines(FOLDER_VIEWS, out, k))
        g_printerr("xd-files: cannot save the folder's view\n");
    g_free(prefix);
    g_strfreev(out);
    xd_free_lines(lines, n);
    g_free(uri);
}

/* The folder's own view, if it has one. */
static void apply_folder_view(explorer *e, GFile *dir) {
    if (view_forced) {
        view_forced = FALSE; /* --view, for the folder it opened */
        return;
    }
    if (xd_place_of(dir) == XD_PLACE_CONTROL) {
        /* Category View's categories as tiles (XP's big links); applets
         * as icons, as Classic View and the category pages show them. */
        gboolean categories = xd_control_page(dir) == nullptr && !xd_control_classic_view();
        set_mode(e, categories ? XD_VIEW_TILES : XD_VIEW_ICONS);
        xd_folder_view_set_sort(e->view, XD_SORT_NAME, FALSE);
        xd_folder_view_set_groups(e->view, FALSE);
        xd_folder_view_set_columns(e->view, nullptr);
        xd_folder_view_set_auto_arrange(e->view, TRUE);
        return;
    }
    /* Not remembering: every folder keeps the view last chosen, as in XP. */
    if (!options.remember_views)
        return;
    char *uri = g_file_get_uri(dir);
    folder_view_setting fv = {0};
    if (find_folder_view(uri, &fv)) {
        set_mode(e, (XdViewMode)fv.mode);
        xd_folder_view_set_auto_arrange(e->view, fv.auto_arrange);
        xd_folder_view_set_align_to_grid(e->view, fv.align_grid);
        xd_folder_view_set_sort(e->view, (XdSortColumn)fv.sort, fv.desc);
        if (xd_folder_view_get_groups(e->view) != fv.groups)
            xd_folder_view_set_groups(e->view, fv.groups);
        xd_folder_view_set_columns(e->view, fv.columns);
        if (!fv.auto_arrange)
            restore_positions(e, uri);
        g_free(fv.columns);
    } else {
        char *columns = xd_folder_view_get_columns(e->view);
        if (xd_folder_view_get_mode(e->view) != (XdViewMode)initial_view ||
            xd_folder_view_get_sort(e->view) != XD_SORT_NAME ||
            xd_folder_view_get_sort_descending(e->view) || xd_folder_view_get_groups(e->view) ||
            columns != nullptr || !xd_folder_view_get_auto_arrange(e->view) ||
            !xd_folder_view_get_align_to_grid(e->view)) {
            set_mode(e, (XdViewMode)initial_view);
            xd_folder_view_set_auto_arrange(e->view, TRUE);
            xd_folder_view_set_align_to_grid(e->view, TRUE);
            xd_folder_view_set_sort(e->view, XD_SORT_NAME, FALSE);
            xd_folder_view_set_groups(e->view, FALSE);
            xd_folder_view_set_columns(e->view, nullptr);
        }
        g_free(columns);
    }
    g_free(uri);
}

/* ---- drives that aren't mounted yet --------------------------------------------- */

/* A mount waiting to finish; the window may close meanwhile. */
typedef struct mount_request {
    GtkWidget *window; /* weak */
    explorer *e;
    gboolean record;
    gboolean unmount; /* Eject on a disk that only unmounts */
    char *name;
} mount_request;

static void mount_request_free(mount_request *r) {
    if (r->window != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(r->window), (gpointer *)&r->window);
    g_free(r->name);
    g_free(r);
}

static void volume_mounted(GObject *src, GAsyncResult *res, gpointer data) {
    mount_request *r = data;
    GVolume *vol = G_VOLUME(src);
    GError *err = nullptr;
    if (!g_volume_mount_finish(vol, res, &err)) {
        /* "Handled": the user cancelled the password question. */
        if (r->window != nullptr && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
            char *msg = g_strdup_printf("%s is not accessible.\n\n%s", r->name, err->message);
            xd_message_box(GTK_WINDOW(r->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
            g_free(msg);
        }
        g_error_free(err);
    } else if (r->window != nullptr) {
        GMount *m = g_volume_get_mount(vol);
        if (m != nullptr) {
            GFile *root = g_mount_get_root(m);
            navigate(r->e, root, r->record);
            g_object_unref(root);
            g_object_unref(m);
        }
    }
    mount_request_free(r);
}

/* Opening a drive that isn't mounted mounts it first, as XP's drives are
 * simply there. */
static gboolean mount_and_open(explorer *e, GFile *item, gboolean record) {
    GVolume *vol = xd_computer_volume(item);
    if (vol == nullptr)
        return FALSE;
    mount_request *r = g_new0(mount_request, 1);
    r->window = e->window;
    g_object_add_weak_pointer(G_OBJECT(r->window), (gpointer *)&r->window);
    r->e = e;
    r->record = record;
    r->name = g_volume_get_name(vol);
    GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(e->window));
    g_volume_mount(vol, G_MOUNT_MOUNT_NONE, op, nullptr, volume_mounted, r);
    g_object_unref(op);
    g_object_unref(vol);
    return TRUE;
}

/* Eject (or, for a disk that can't be ejected, unmount), as My Computer's
 * Eject does. */
static void ejected(GObject *src, GAsyncResult *res, gpointer data) {
    mount_request *r = data;
    GError *err = nullptr;
    gboolean ok = G_IS_VOLUME(src)  ? g_volume_eject_with_operation_finish(G_VOLUME(src), res, &err)
                  : r->unmount
                      ? g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &err)
                      : g_mount_eject_with_operation_finish(G_MOUNT(src), res, &err);
    if (!ok) {
        if (r->window != nullptr && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
            xd_message_box(GTK_WINDOW(r->window), "Eject", XD_MB_ERROR, XD_MB_OK, FALSE,
                           err->message);
        g_error_free(err);
    }
    mount_request_free(r);
}

static void eject_item(explorer *e, GFile *item) {
    mount_request *r = g_new0(mount_request, 1);
    r->window = e->window;
    g_object_add_weak_pointer(G_OBJECT(r->window), (gpointer *)&r->window);
    r->e = e;
    GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(e->window));
    GVolume *vol = xd_computer_volume(item);
    GMount *m = vol == nullptr ? xd_computer_mount(item) : nullptr;
    if (vol != nullptr && g_volume_can_eject(vol)) {
        g_volume_eject_with_operation(vol, G_MOUNT_UNMOUNT_NONE, op, nullptr, ejected, r);
    } else if (m != nullptr && g_mount_can_eject(m)) {
        g_mount_eject_with_operation(m, G_MOUNT_UNMOUNT_NONE, op, nullptr, ejected, r);
    } else if (m != nullptr && g_mount_can_unmount(m)) {
        r->unmount = TRUE;
        g_mount_unmount_with_operation(m, G_MOUNT_UNMOUNT_NONE, op, nullptr, ejected, r);
    } else {
        mount_request_free(r);
    }
    g_clear_object(&vol);
    g_clear_object(&m);
    g_object_unref(op);
}

static void cb_eject([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    GList *files = xd_folder_view_get_selected_files(e->view);
    for (GList *l = files; l != nullptr; l = l->next)
        if (xd_computer_can_eject(l->data))
            eject_item(e, l->data);
    g_list_free_full(files, g_object_unref);
}

static void navigate(explorer *e, GFile *dir, gboolean record) {
    if (mount_and_open(e, dir, record))
        return;
    GFile *current = xd_folder_view_get_location(e->view);
    if (record && current != nullptr && !g_file_equal(current, dir)) {
        e->back = g_list_prepend(e->back, g_object_ref(current));
        free_history(&e->forward);
    }
    if (current != nullptr)
        remember_folder_view(e);
    apply_folder_view(e, dir);
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
    GFile *dir = xd_folder_view_get_location(e->view);
    if (dir == nullptr)
        return;
    if (xd_place_of(dir) == XD_PLACE_CONTROL) {
        /* A category's page is in Control Panel, which is in My Computer. */
        GFile *up = xd_control_page(dir) != nullptr ? xd_place_control() : xd_place_computer();
        navigate(e, up, TRUE);
        g_object_unref(up);
        return;
    }
    if (xd_place_of(dir) != XD_PLACE_FOLDER)
        return;
    GFile *parent = xd_place_is_drive_root(dir) ? xd_place_computer() : g_file_get_parent(dir);
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

static GFile *current_dir(explorer *e);
static void open_window(GtkApplication *app, GFile *dir);

/* Opens a Control Panel applet (its desktop file). FALSE if `file` isn't
 * one. */
static gboolean launch_applet(explorer *e, GFile *file) {
    GDesktopAppInfo *info = xd_control_item_applet(file);
    if (info == nullptr)
        return FALSE;
    GdkAppLaunchContext *ctx =
        gdk_display_get_app_launch_context(gtk_widget_get_display(e->window));
    GError *err = nullptr;
    if (!g_app_info_launch(G_APP_INFO(info), nullptr, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        char *msg = g_strdup_printf("Cannot open %s: %s", g_app_info_get_name(G_APP_INFO(info)),
                                    err->message);
        xd_message_box(GTK_WINDOW(e->window), "Control Panel", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
        g_error_free(err);
    }
    g_object_unref(ctx);
    g_object_unref(info);
    return TRUE;
}

static void on_activated([[maybe_unused]] XdFolderView *view, GFile *file, gboolean is_dir,
                         gpointer data) {
    explorer *e = data;
    if (xd_place_of(current_dir(e)) == XD_PLACE_TRASH) {
        /* Items in the Recycle Bin open their Properties, as in XP. */
        GList *one = g_list_append(nullptr, file);
        xd_properties_show(GTK_WINDOW(e->window), one);
        g_list_free(one);
        return;
    }
    if (is_dir && options.new_window) {
        open_window(gtk_window_get_application(GTK_WINDOW(e->window)), file);
        return;
    }
    if (is_dir) {
        navigate(e, file, TRUE);
        return;
    }
    if (xd_place_of(current_dir(e)) == XD_PLACE_CONTROL && launch_applet(e, file))
        return;
    /* Open with the file type's default application. */
    char *uri = g_file_get_uri(file);
    GError *err = nullptr;
    if (!g_app_info_launch_default_for_uri(uri, nullptr, &err)) {
        xd_message_box(GTK_WINDOW(e->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE,
                       err != nullptr ? err->message : "Cannot open file");
        g_clear_error(&err);
    }
    g_free(uri);
}

static void on_address_activate(GtkEntry *entry, gpointer data) {
    explorer *e = data;
    /* The folder's name alone (Folder Options) stands for itself. */
    const char *shown = g_object_get_data(G_OBJECT(entry), "xd-shown");
    if (!options.full_path_address && shown != nullptr &&
        strcmp(shown, gtk_entry_get_text(entry)) == 0 && current_dir(e) != nullptr) {
        xd_folder_view_reload(e->view);
        gtk_widget_grab_focus(GTK_WIDGET(e->view));
        return;
    }
    GFile *place = xd_place_parse(gtk_entry_get_text(entry));
    if (place != nullptr) {
        navigate(e, place, TRUE);
        g_object_unref(place);
        gtk_widget_grab_focus(GTK_WIDGET(e->view));
        return;
    }
    GFile *f = g_file_parse_name(gtk_entry_get_text(entry));
    GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, nullptr);
    if (type == G_FILE_TYPE_DIRECTORY) {
        navigate(e, f, TRUE);
        gtk_widget_grab_focus(GTK_WIDGET(e->view));
    } else if (type != G_FILE_TYPE_UNKNOWN) {
        on_activated(e->view, f, FALSE, e);
    } else {
        /* Windows' message for a bad address. */
        char *text = g_strdup_printf("Cannot find '%s'. Make sure the path is correct and try "
                                     "again.",
                                     gtk_entry_get_text(entry));
        xd_message_box(GTK_WINDOW(e->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
    }
    g_object_unref(f);
}

static void on_load_error([[maybe_unused]] XdFolderView *view, const char *message,
                          gpointer data) {
    explorer *e = data;
    gtk_label_set_text(GTK_LABEL(e->status), message);
}

static void begin_rename(explorer *e, GFile *file);

static void on_contents([[maybe_unused]] XdFolderView *view, gpointer data) {
    explorer *e = data;
    update_status(e);
    /* A new item to name, now that it's loaded. */
    if (e->pending_rename != nullptr) {
        GFile *f = g_object_ref(e->pending_rename);
        GdkRectangle a;
        if (xd_folder_view_get_name_area(e->view, f, &a))
            begin_rename(e, f);
        g_object_unref(f);
    }
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

/* Show Hidden Files is Folder Options' setting: every window follows. */
static void on_hidden(GtkCheckMenuItem *item, gpointer data) {
    explorer *e = data;
    gboolean show = gtk_check_menu_item_get_active(item);
    xd_folder_view_set_show_hidden(e->view, show);
    if (e->syncing || show == options.show_hidden)
        return;
    options.show_hidden = show;
    xd_settings s;
    xd_settings_load(&s);
    s.folders.show_hidden = show;
    if (!xd_settings_save(&s))
        g_printerr("xd-files: cannot save Show Hidden Files\n");
}

/* View > Arrange Icons By: Name, Size, Type, Modified (in My Computer
 * and the Recycle Bin the columns they show). */
static void on_arrange(GtkWidget *item, gpointer data) {
    explorer *e = data;
    if (GTK_IS_CHECK_MENU_ITEM(item) && !gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)))
        return;
    if (e->syncing)
        return;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-sort"));
    xd_folder_view_set_sort(e->view, (XdSortColumn)col, FALSE);
}

/* Arrange Icons by's check items after the columns ("xd-sort"). */
enum { ARRANGE_GROUPS = -1, ARRANGE_AUTO = -2, ARRANGE_ALIGN = -3 };

static void on_show_groups(GtkWidget *item, gpointer data) {
    explorer *e = data;
    if (e->syncing)
        return;
    xd_folder_view_set_groups(e->view, gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)));
    remember_folder_view(e);
}

static void on_auto_arrange(GtkWidget *item, gpointer data) {
    explorer *e = data;
    if (e->syncing)
        return;
    xd_folder_view_set_auto_arrange(e->view,
                                    gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)));
    remember_folder_view(e);
}

static void on_align_to_grid(GtkWidget *item, gpointer data) {
    explorer *e = data;
    if (e->syncing)
        return;
    xd_folder_view_set_align_to_grid(e->view,
                                     gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)));
    remember_folder_view(e);
}

/* Auto Arrange and Align to Grid are for the icon views, and not while
 * they're shown in groups (always arranged); nor for My Computer or
 * Control Panel. Align to Grid is for icons put anywhere. */
static gboolean can_place_icons(explorer *e) {
    XdViewMode mode = xd_folder_view_get_mode(e->view);
    XdPlace place = xd_place_of(xd_folder_view_get_location(e->view));
    return mode != XD_VIEW_LIST && mode != XD_VIEW_DETAILS && !xd_folder_view_get_groups(e->view) &&
           (place == XD_PLACE_FOLDER || place == XD_PLACE_TRASH);
}

/* Checks radio `col` of an Arrange Icons by menu, and the check items. */
static void check_radio(GtkWidget *menu, int col, explorer *e) {
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    e->syncing = TRUE;
    gboolean placeable = can_place_icons(e);
    gboolean arranged = xd_folder_view_get_auto_arrange(e->view);
    for (GList *l = items; l != nullptr; l = l->next) {
        if (!GTK_IS_CHECK_MENU_ITEM(l->data))
            continue; /* the separator */
        GtkCheckMenuItem *check = GTK_CHECK_MENU_ITEM(l->data);
        int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(l->data), "xd-sort"));
        if (which == col) {
            gtk_check_menu_item_set_active(check, TRUE);
        } else if (which == ARRANGE_GROUPS) {
            gtk_check_menu_item_set_active(check, xd_folder_view_get_groups(e->view));
            /* XP greys it in List view, which can't show groups. */
            gtk_widget_set_sensitive(l->data, xd_folder_view_get_mode(e->view) != XD_VIEW_LIST);
        } else if (which == ARRANGE_AUTO) {
            gtk_check_menu_item_set_active(check, arranged || !placeable);
            gtk_widget_set_sensitive(l->data, placeable);
        } else if (which == ARRANGE_ALIGN) {
            gtk_check_menu_item_set_active(check, xd_folder_view_get_align_to_grid(e->view));
            gtk_widget_set_sensitive(l->data, placeable && !arranged);
        }
    }
    e->syncing = FALSE;
    g_list_free(items);
}

static GtkWidget *arrange_menu(explorer *e, gboolean radios) {
    static const char *const names[XD_SORT_ARRANGE_COUNT] = {"_Name", "_Size", "T_ype",
                                                             "Modi_fied"};
    GtkWidget *menu = gtk_menu_new();
    GSList *group = nullptr;
    /* The menus are built before the view: it starts by name. */
    XdSortColumn sort = e->view != nullptr ? xd_folder_view_get_sort(e->view) : XD_SORT_NAME;
    int current = (int)sort;
    e->syncing = TRUE;
    for (int i = 0; i < XD_SORT_ARRANGE_COUNT; i++) {
        GtkWidget *item;
        if (radios) {
            item = gtk_radio_menu_item_new_with_mnemonic(group, names[i]);
            group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), i == current);
            g_signal_connect(item, "toggled", G_CALLBACK(on_arrange), e);
        } else {
            item = gtk_menu_item_new_with_mnemonic(names[i]);
            g_signal_connect(item, "activate", G_CALLBACK(on_arrange), e);
        }
        g_object_set_data(G_OBJECT(item), "xd-sort", GINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *groups = gtk_check_menu_item_new_with_mnemonic("Show in _Groups");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(groups),
                                   e->view != nullptr && xd_folder_view_get_groups(e->view));
    g_object_set_data(G_OBJECT(groups), "xd-sort", GINT_TO_POINTER(ARRANGE_GROUPS));
    g_signal_connect(groups, "toggled", G_CALLBACK(on_show_groups), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), groups);
    GtkWidget *arrange = gtk_check_menu_item_new_with_mnemonic("_Auto Arrange");
    g_object_set_data(G_OBJECT(arrange), "xd-sort", GINT_TO_POINTER(ARRANGE_AUTO));
    g_signal_connect(arrange, "toggled", G_CALLBACK(on_auto_arrange), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), arrange);
    GtkWidget *align = gtk_check_menu_item_new_with_mnemonic("A_lign to Grid");
    g_object_set_data(G_OBJECT(align), "xd-sort", GINT_TO_POINTER(ARRANGE_ALIGN));
    g_signal_connect(align, "toggled", G_CALLBACK(on_align_to_grid), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), align);
    e->syncing = FALSE;
    if (e->view != nullptr)
        check_radio(menu, current, e);
    gtk_widget_show_all(menu);
    return menu;
}

/* The radio follows the sort (a Details header click changes it). */
static void on_view_menu_show(GtkWidget *menu, gpointer data) {
    (void)menu;
    explorer *e = data;
    GtkWidget *sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(e->arrange_item));
    XdSortColumn sort = xd_folder_view_get_sort(e->view);
    check_radio(sub, (int)sort, e);
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
/* Properties of the selection, or of the folder itself when nothing is
 * selected. */
static void show_properties(explorer *e) {
    /* A drive's Properties would count everything on it; Control Panel's
     * items aren't files to show. */
    XdPlace place = xd_place_of(xd_folder_view_get_location(e->view));
    if (place == XD_PLACE_COMPUTER || place == XD_PLACE_CONTROL)
        return;
    GList *files = xd_folder_view_get_selected_files(e->view);
    if (files == nullptr) {
        GFile *dir = xd_folder_view_get_location(e->view);
        if (dir == nullptr || xd_place_of(dir) != XD_PLACE_FOLDER)
            return;
        files = g_list_append(nullptr, g_object_ref(dir));
    }
    xd_properties_show(GTK_WINDOW(e->window), files);
    g_list_free_full(files, g_object_unref);
}

static void cb_properties([[maybe_unused]] GtkWidget *w, gpointer d) { show_properties(d); }

/* Switch to Classic View / Switch to Category View: remembered for every
 * Control Panel window from now on. A category's page switches to
 * Classic View's Control Panel. */
static void cb_control_switch([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    GFile *dir = xd_folder_view_get_location(e->view);
    if (xd_place_of(dir) != XD_PLACE_CONTROL)
        return;
    xd_control_set_classic_view(!xd_control_classic_view());
    if (xd_control_page(dir) != nullptr) {
        GFile *root = xd_place_control();
        navigate(e, root, TRUE);
        g_object_unref(root);
        return;
    }
    apply_folder_view(e, dir);
    xd_folder_view_reload(e->view);
    update_chrome(e);
}

static gboolean on_control_link(GtkLinkButton *b, gpointer d) {
    cb_control_switch(GTK_WIDGET(b), d);
    return TRUE; /* not a link to open */
}

static void cb_open_selected([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    GList *files = xd_folder_view_get_selected_files(e->view);
    for (GList *l = files; l != nullptr; l = l->next) {
        /* My Computer's items are all drives (some not mounted yet), and
         * Control Panel's categories are pages of it. */
        gboolean is_dir = xd_place_of(current_dir(e)) == XD_PLACE_COMPUTER ||
                          xd_place_of(l->data) == XD_PLACE_CONTROL ||
                          g_file_query_file_type(l->data, G_FILE_QUERY_INFO_NONE, nullptr) ==
                              G_FILE_TYPE_DIRECTORY;
        on_activated(e->view, l->data, is_dir, e);
        if (is_dir && !options.new_window)
            break; /* navigating replaces the view */
    }
    g_list_free_full(files, g_object_unref);
}

static void cb_close([[maybe_unused]] GtkWidget *w, gpointer d) {
    gtk_widget_destroy(((explorer *)d)->window);
}
static void cb_go([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    on_address_activate(GTK_ENTRY(e->address), e);
}
/* Tools > Folder Options: xd-folders, told this folder's view so that
 * Apply to All Folders can use it. */
static void cb_folder_options([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    char *view = g_strdup_printf("--view=%s", view_names[xd_folder_view_get_mode(e->view)]);
    char *prog = g_strdup("xd-folders");
    char *argv[] = {prog, view, nullptr};
    GError *err = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &err)) {
        xd_message_box(GTK_WINDOW(e->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE,
                       err->message);
        g_error_free(err);
    }
    g_free(prog);
    g_free(view);
}

static void cb_about([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    gtk_show_about_dialog(GTK_WINDOW(e->window), "program-name", "XeonDesktop Files",
                          "comments", "The XeonDesktop file manager.", "license-type",
                          GTK_LICENSE_MIT_X11, "logo-icon-name", "system-file-manager", nullptr);
}

/* ---- file operations ------------------------------------------------------------- */

static GFile *current_dir(explorer *e) { return xd_folder_view_get_location(e->view); }

static XdPlace current_place(explorer *e) { return xd_place_of(current_dir(e)); }

static void after_trash_change([[maybe_unused]] GList *created, [[maybe_unused]] gpointer data) {
    xd_trash_tidy();
}

static void restore_selected(explorer *e) {
    GList *files = xd_folder_view_get_selected_files(e->view);
    xd_trash_restore(GTK_WINDOW(e->window), files);
    g_list_free_full(files, g_object_unref);
}

static void empty_trash(explorer *e) {
    xd_trash_empty(GTK_WINDOW(e->window), nullptr, nullptr);
}

/* Selects what an operation made, when it's in this folder. */
static void select_created(GList *created, gpointer data) {
    explorer *e = data;
    GFile *dir = current_dir(e);
    GList *here = nullptr;
    for (GList *l = created; l != nullptr; l = l->next) {
        GFile *parent = g_file_get_parent(l->data);
        if (parent != nullptr && dir != nullptr && g_file_equal(parent, dir))
            here = g_list_prepend(here, l->data);
        g_clear_object(&parent);
    }
    here = g_list_reverse(here);
    if (here != nullptr)
        xd_folder_view_select_files(e->view, here);
    g_list_free(here);
}

static void copy_or_cut(explorer *e, gboolean cut) {
    if (current_place(e) == XD_PLACE_COMPUTER || current_place(e) == XD_PLACE_CONTROL)
        return;
    GList *files = xd_folder_view_get_selected_files(e->view);
    if (files == nullptr)
        return;
    xd_clipboard_set_files(e->window, files, cut);
    xd_folder_view_set_cut_files(e->view, cut ? files : nullptr);
    g_list_free_full(files, g_object_unref);
}

static void after_paste(GList *created, gpointer data) {
    explorer *e = data;
    select_created(created, e);
}

static void after_cut_paste(GList *created, gpointer data) {
    explorer *e = data;
    /* Cut files are gone once pasted: so is the clipboard's list. */
    xd_clipboard_clear_cut(e->window);
    xd_folder_view_set_cut_files(e->view, nullptr);
    xd_trash_tidy(); /* in case they came out of the Recycle Bin */
    select_created(created, e);
}

/* A paste waiting for the clipboard, which may outlive the window. */
typedef struct paste_request {
    GtkWidget *window; /* weak */
    explorer *e;
    GFile *dest;
    gboolean shortcut;
} paste_request;

static void got_clipboard(GList *files, gboolean cut, gpointer data) {
    paste_request *r = data;
    if (r->window != nullptr) {
        g_object_remove_weak_pointer(G_OBJECT(r->window), (gpointer *)&r->window);
        if (files != nullptr) {
            XdFileOpKind kind = r->shortcut ? XD_FILEOP_LINK : cut ? XD_FILEOP_MOVE : XD_FILEOP_COPY;
            xd_fileop_start_undoable(GTK_WINDOW(r->window), kind, files, r->dest,
                                     kind == XD_FILEOP_MOVE ? after_cut_paste : after_paste, r->e);
        }
    }
    g_object_unref(r->dest);
    g_free(r);
}

static void paste_into(explorer *e, GFile *dest, gboolean shortcut) {
    if (dest == nullptr || xd_place_of(dest) != XD_PLACE_FOLDER)
        return;
    paste_request *r = g_new0(paste_request, 1);
    r->window = e->window;
    g_object_add_weak_pointer(G_OBJECT(r->window), (gpointer *)&r->window);
    r->e = e;
    r->dest = g_object_ref(dest);
    r->shortcut = shortcut;
    xd_clipboard_get_files(e->window, got_clipboard, r);
}

/* Context menu Paste on a single folder pastes into it. */
static GFile *paste_target(explorer *e) {
    GList *files = xd_folder_view_get_selected_files(e->view);
    GFile *target = nullptr;
    if (files != nullptr && files->next == nullptr &&
        g_file_query_file_type(files->data, G_FILE_QUERY_INFO_NONE, nullptr) ==
            G_FILE_TYPE_DIRECTORY)
        target = g_object_ref(files->data);
    g_list_free_full(files, g_object_unref);
    return target != nullptr ? target : g_object_ref(current_dir(e));
}

static void delete_selected(explorer *e, gboolean permanently) {
    XdPlace place = current_place(e);
    if (place == XD_PLACE_COMPUTER || place == XD_PLACE_CONTROL)
        return;
    GList *files = xd_folder_view_get_selected_files(e->view);
    gboolean in_trash = place == XD_PLACE_TRASH;
    /* Sending to the Recycle Bin can be undone; deleting can't. */
    xd_fileop_start_undoable(GTK_WINDOW(e->window),
                             permanently || in_trash ? XD_FILEOP_DELETE : XD_FILEOP_TRASH, files,
                             nullptr, in_trash ? after_trash_change : nullptr, e);
    g_list_free_full(files, g_object_unref);
}

/* Edit > Copy To Folder... / Move To Folder... */
static void copy_to_folder(explorer *e, gboolean move) {
    if (current_place(e) == XD_PLACE_CONTROL)
        return;
    GList *files = xd_folder_view_get_selected_files(e->view);
    if (files == nullptr)
        return;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        move ? "Move Items" : "Copy Items", GTK_WINDOW(e->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel", GTK_RESPONSE_CANCEL,
        move ? "_Move" : "_Copy", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_current_folder_file(GTK_FILE_CHOOSER(dlg), current_dir(e), nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GFile *dest = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
        if (dest != nullptr) {
            xd_fileop_start_undoable(GTK_WINDOW(e->window), move ? XD_FILEOP_MOVE : XD_FILEOP_COPY,
                                     files, dest, select_created, e);
            g_object_unref(dest);
        }
    }
    gtk_widget_destroy(dlg);
    g_list_free_full(files, g_object_unref);
}

/* ---- renaming in place ------------------------------------------------------------- */

static void place_rename_entry(explorer *e) {
    GdkRectangle a;
    if (e->renaming == nullptr || !xd_folder_view_get_name_area(e->view, e->renaming, &a))
        return;
    int x = 0, y = 0;
    gtk_widget_translate_coordinates(GTK_WIDGET(e->view), e->overlay, a.x, a.y, &x, &y);
    gtk_widget_set_margin_start(e->rename_entry, MAX(x, 0));
    gtk_widget_set_margin_top(e->rename_entry, MAX(y, 0));
    gtk_widget_set_size_request(e->rename_entry, MAX(a.width, 60), -1);
}

static void end_rename(explorer *e) {
    g_clear_object(&e->renaming);
    gtk_widget_hide(e->rename_entry);
    gtk_widget_grab_focus(GTK_WIDGET(e->view));
}

static void begin_rename(explorer *e, GFile *file) {
    g_clear_object(&e->pending_rename);
    GdkRectangle a;
    if (!xd_folder_view_get_name_area(e->view, file, &a)) {
        /* Not loaded yet (a new folder): when it appears. */
        e->pending_rename = g_object_ref(file);
        return;
    }
    GList *one = g_list_append(nullptr, file);
    xd_folder_view_select_files(e->view, one);
    g_list_free(one);
    g_set_object(&e->renaming, file);
    char *name = xd_rename_text(file);
    gtk_entry_set_text(GTK_ENTRY(e->rename_entry), name);
    g_free(name);
    place_rename_entry(e);
    gtk_widget_show(e->rename_entry);
    gtk_widget_grab_focus(e->rename_entry);
    gtk_editable_select_region(GTK_EDITABLE(e->rename_entry), 0, -1);
}

static void rename_selected(explorer *e) {
    if (current_place(e) != XD_PLACE_FOLDER)
        return;
    GList *files = xd_folder_view_get_selected_files(e->view);
    if (files != nullptr)
        begin_rename(e, files->data);
    g_list_free_full(files, g_object_unref);
}

/* Returns FALSE to keep editing (the name was refused). */
static gboolean commit_rename(explorer *e) {
    if (e->renaming == nullptr || e->committing)
        return TRUE;
    const char *text = gtk_entry_get_text(GTK_ENTRY(e->rename_entry));
    char *old = xd_rename_text(e->renaming);
    gboolean done = TRUE;
    e->committing = TRUE;
    if (*text != '\0' && strcmp(text, old) != 0) {
        if (strchr(text, '/') != nullptr) {
            xd_message_box(GTK_WINDOW(e->window), "Rename", XD_MB_ERROR, XD_MB_OK, FALSE,
                           "A file name cannot contain the following character:\n        /");
            done = FALSE;
        } else {
            GError *err = nullptr;
            GFile *renamed = xd_rename(e->renaming, text, &err);
            if (renamed != nullptr) {
                xd_undo_add_rename(renamed, old);
                GList *one = g_list_append(nullptr, renamed);
                xd_folder_view_select_files(e->view, one);
                g_list_free(one);
                g_object_unref(renamed);
            } else {
                char *msg =
                    g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)
                        ? g_strdup_printf("Cannot rename %s: A file with the name you specified "
                                          "already exists. Specify a different file name.",
                                          old)
                        : g_strdup_printf("Cannot rename %s: %s", old, err->message);
                xd_message_box(GTK_WINDOW(e->window), "Error Renaming File or Folder",
                               XD_MB_ERROR, XD_MB_OK, FALSE, msg);
                g_free(msg);
                g_error_free(err);
                done = FALSE;
            }
        }
    }
    e->committing = FALSE;
    g_free(old);
    if (done) {
        end_rename(e);
    } else {
        gtk_widget_grab_focus(e->rename_entry);
        gtk_editable_select_region(GTK_EDITABLE(e->rename_entry), 0, -1);
    }
    return done;
}

static void on_rename_activate([[maybe_unused]] GtkEntry *entry, gpointer data) {
    commit_rename(data);
}

static gboolean on_rename_key([[maybe_unused]] GtkWidget *w, GdkEventKey *ev, gpointer data) {
    explorer *e = data;
    if (ev->keyval == GDK_KEY_Escape) {
        end_rename(e);
        return TRUE;
    }
    return FALSE;
}

/* After a focus change has settled: commit unless the entry has the focus
 * again (a message box came and went). */
static void commit_after_focus_change(gpointer data) {
    explorer *e = data;
    e->commit_idle = 0;
    if (e->renaming != nullptr && !gtk_widget_has_focus(e->rename_entry) &&
        gtk_window_is_active(GTK_WINDOW(e->window)))
        commit_rename(e);
}

static gboolean on_rename_focus_out([[maybe_unused]] GtkWidget *w,
                                    [[maybe_unused]] GdkEventFocus *ev, gpointer data) {
    explorer *e = data;
    /* Clicking elsewhere in the window keeps the new name, as in Windows;
     * the window losing the focus leaves the name being edited. */
    if (!e->committing && e->renaming != nullptr &&
        gtk_window_is_active(GTK_WINDOW(e->window)))
        if (e->commit_idle == 0)
            e->commit_idle = g_idle_add_once(commit_after_focus_change, e);
    return FALSE;
}

static void on_view_scrolled([[maybe_unused]] GtkAdjustment *a, gpointer data) {
    place_rename_entry(data);
}

/* File > New > Folder / Text Document: made, then named in place. */
static void new_item(explorer *e, gboolean folder) {
    GFile *dir = current_dir(e);
    if (dir == nullptr || xd_place_of(dir) != XD_PLACE_FOLDER)
        return;
    char *name = folder ? xd_unique_name(dir, "New Folder", nullptr)
                        : xd_unique_name(dir, "New Text Document", ".txt");
    GFile *f = g_file_get_child(dir, name);
    g_free(name);
    GError *err = nullptr;
    gboolean ok;
    if (folder) {
        ok = g_file_make_directory(f, nullptr, &err);
    } else {
        GFileOutputStream *out = g_file_create(f, G_FILE_CREATE_NONE, nullptr, &err);
        ok = out != nullptr;
        g_clear_object(&out);
    }
    if (ok) {
        xd_folder_view_reload(e->view);
        begin_rename(e, f);
    } else {
        char *msg = g_strdup_printf("Unable to create the %s: %s", folder ? "folder" : "file",
                                    err->message);
        xd_message_box(GTK_WINDOW(e->window), "Error", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
        g_error_free(err);
    }
    g_object_unref(f);
}

static void create_shortcut(explorer *e) {
    if (current_place(e) != XD_PLACE_FOLDER)
        return;
    GList *files = xd_folder_view_get_selected_files(e->view);
    xd_fileop_start(GTK_WINDOW(e->window), XD_FILEOP_LINK, files, current_dir(e), select_created,
                    e);
    g_list_free_full(files, g_object_unref);
}

static void on_files_dropped([[maybe_unused]] XdFolderView *view, char **uris, GFile *folder,
                             GdkDragAction action, gpointer data) {
    explorer *e = data;
    GList *files = nullptr;
    for (char **u = uris; u != nullptr && *u != nullptr; u++)
        files = g_list_prepend(files, g_file_new_for_uri(*u));
    files = g_list_reverse(files);
    XdFileOpKind kind = xd_place_of(folder) == XD_PLACE_TRASH ? XD_FILEOP_TRASH
                        : action == GDK_ACTION_MOVE         ? XD_FILEOP_MOVE
                        : action == GDK_ACTION_LINK         ? XD_FILEOP_LINK
                                                            : XD_FILEOP_COPY;
    xd_fileop_start_undoable(GTK_WINDOW(e->window), kind, files, folder, select_created, e);
    g_list_free_full(files, g_object_unref);
}

/* Edit > Undo (Ctrl+Z): the latest operation, taken back. */
static void undo_last(explorer *e) {
    if (xd_undo_label() == nullptr)
        return;
    xd_undo(GTK_WINDOW(e->window), select_created, e);
}

static void cb_undo([[maybe_unused]] GtkWidget *w, gpointer d) { undo_last(d); }

static void cb_cut([[maybe_unused]] GtkWidget *w, gpointer d) { copy_or_cut(d, TRUE); }
static void cb_restore([[maybe_unused]] GtkWidget *w, gpointer d) { restore_selected(d); }
static void cb_empty_trash([[maybe_unused]] GtkWidget *w, gpointer d) { empty_trash(d); }
static void cb_copy([[maybe_unused]] GtkWidget *w, gpointer d) { copy_or_cut(d, FALSE); }
static void cb_paste([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    paste_into(e, current_dir(e), FALSE);
}
static void cb_paste_into([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    GFile *target = paste_target(e);
    paste_into(e, target, FALSE);
    g_object_unref(target);
}
static void cb_paste_shortcut([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    paste_into(e, current_dir(e), TRUE);
}
static void cb_copy_to([[maybe_unused]] GtkWidget *w, gpointer d) { copy_to_folder(d, FALSE); }
static void cb_move_to([[maybe_unused]] GtkWidget *w, gpointer d) { copy_to_folder(d, TRUE); }
static void cb_delete([[maybe_unused]] GtkWidget *w, gpointer d) { delete_selected(d, FALSE); }
static void cb_rename([[maybe_unused]] GtkWidget *w, gpointer d) { rename_selected(d); }
static void cb_new_folder([[maybe_unused]] GtkWidget *w, gpointer d) { new_item(d, TRUE); }
static void cb_new_text([[maybe_unused]] GtkWidget *w, gpointer d) { new_item(d, FALSE); }
static void cb_shortcut([[maybe_unused]] GtkWidget *w, gpointer d) { create_shortcut(d); }

/* The view's keys for files: Delete (Shift: permanently), F2, Ctrl+X/C/V. */
static gboolean file_key(explorer *e, GdkEventKey *ev) {
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (ev->state & GDK_SHIFT_MASK) != 0;
    gboolean selected = xd_folder_view_get_selected_count(e->view) > 0;
    switch (ev->keyval) {
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        if (selected)
            delete_selected(e, shift);
        return TRUE;
    case GDK_KEY_F2:
        rename_selected(e);
        return TRUE;
    case GDK_KEY_x:
    case GDK_KEY_X:
        if (!ctrl)
            return FALSE;
        copy_or_cut(e, TRUE);
        return TRUE;
    case GDK_KEY_c:
    case GDK_KEY_C:
        if (!ctrl)
            return FALSE;
        copy_or_cut(e, FALSE);
        return TRUE;
    case GDK_KEY_v:
    case GDK_KEY_V:
        if (!ctrl)
            return FALSE;
        paste_into(e, current_dir(e), FALSE);
        return TRUE;
    case GDK_KEY_z:
    case GDK_KEY_Z:
        if (!ctrl)
            return FALSE;
        undo_last(e);
        return TRUE;
    default:
        return FALSE;
    }
}

/* Shows an accelerator in a menu item without binding it: the keys work
 * in the folder view only, not in the address bar. */
static void show_accel(GtkWidget *item, guint key, GdkModifierType mods) {
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(item));
    if (GTK_IS_ACCEL_LABEL(label))
        gtk_accel_label_set_accel(GTK_ACCEL_LABEL(label), key, mods);
}

static GtkWidget *send_to_menu(explorer *e, GList *files) {
    return xd_send_to_menu(GTK_WINDOW(e->window), files, select_created, e);
}

static void update_menu_items(GtkWidget *menu, gpointer data) {
    (void)menu;
    explorer *e = data;
    gboolean selected = xd_folder_view_get_selected_count(e->view) > 0;
    for (guint i = 0; i < e->need_selection->len; i++)
        gtk_widget_set_sensitive(g_ptr_array_index(e->need_selection, i), selected);
    for (guint i = 0; i < e->need_clipboard->len; i++)
        gtk_widget_set_sensitive(g_ptr_array_index(e->need_clipboard, i),
                                 xd_clipboard_has_files());
    XdPlace place = current_place(e);
    /* Folders only, on top of the above (New has nothing else: it must
     * come back after the Recycle Bin, not keep its last state). */
    for (guint i = 0; i < e->folders_only->len; i++) {
        GtkWidget *w = g_ptr_array_index(e->folders_only, i);
        gboolean on = g_ptr_array_find(e->need_selection, w, nullptr)   ? selected
                      : g_ptr_array_find(e->need_clipboard, w, nullptr) ? xd_clipboard_has_files()
                                                                        : TRUE;
        gtk_widget_set_sensitive(w, place == XD_PLACE_FOLDER && on);
    }
    for (guint i = 0; i < e->trash_only->len; i++)
        gtk_widget_set_visible(g_ptr_array_index(e->trash_only, i), place == XD_PLACE_TRASH);
    /* "Undo Rename", "Undo Copy"..., or a greyed "Undo". */
    const char *undo = xd_undo_label();
    gtk_menu_item_set_label(GTK_MENU_ITEM(e->undo_item), undo != nullptr ? undo : "_Undo");
    gtk_widget_set_sensitive(e->undo_item, undo != nullptr);
    /* Send To, for what's selected now. */
    GList *files = place == XD_PLACE_FOLDER ? xd_folder_view_get_selected_files(e->view) : nullptr;
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(e->send_to_item),
                              files != nullptr ? send_to_menu(e, files) : nullptr);
    gtk_widget_set_sensitive(e->send_to_item, files != nullptr);
    g_list_free_full(files, g_object_unref);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, explorer *e,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods);

static GtkWidget *add_new_menu(explorer *e, GtkWidget *menu) {
    GtkWidget *top = gtk_menu_item_new_with_mnemonic("Ne_w");
    GtkWidget *sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
    menu_item(sub, "_Folder", G_CALLBACK(cb_new_folder), e, nullptr, 0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
    menu_item(sub, "Text Document", G_CALLBACK(cb_new_text), e, nullptr, 0, NO_MODS);
    return top;
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
    /* The Recycle Bin's own commands, shown only there. */
    GtkWidget *item = menu_item(file, "Empty Recycle _Bin", G_CALLBACK(cb_empty_trash), e,
                                nullptr, 0, NO_MODS);
    g_ptr_array_add(e->trash_only, item);
    item = menu_item(file, "R_estore", G_CALLBACK(cb_restore), e, nullptr, 0, NO_MODS);
    g_ptr_array_add(e->trash_only, item);
    g_ptr_array_add(e->need_selection, item);
    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(file), item);
    g_ptr_array_add(e->trash_only, item);
    e->send_to_item = gtk_menu_item_new_with_mnemonic("Se_nd To");
    gtk_menu_shell_append(GTK_MENU_SHELL(file), e->send_to_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    g_ptr_array_add(e->folders_only, add_new_menu(e, file));
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    item = menu_item(file, "Create _Shortcut", G_CALLBACK(cb_shortcut), e, nullptr, 0, NO_MODS);
    g_ptr_array_add(e->need_selection, item);
    g_ptr_array_add(e->folders_only, item);
    item = menu_item(file, "_Delete", G_CALLBACK(cb_delete), e, nullptr, 0, NO_MODS);
    show_accel(item, GDK_KEY_Delete, NO_MODS);
    g_ptr_array_add(e->need_selection, item);
    item = menu_item(file, "Rena_me", G_CALLBACK(cb_rename), e, nullptr, 0, NO_MODS);
    show_accel(item, GDK_KEY_F2, NO_MODS);
    g_ptr_array_add(e->need_selection, item);
    g_ptr_array_add(e->folders_only, item);
    menu_item(file, "P_roperties", G_CALLBACK(cb_properties), e, accel, GDK_KEY_Return,
              GDK_MOD1_MASK);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "_Close", G_CALLBACK(cb_close), e, accel, GDK_KEY_w, GDK_CONTROL_MASK);
    g_signal_connect(file, "show", G_CALLBACK(update_menu_items), e);

    GtkWidget *edit = submenu(bar, "_Edit");
    e->undo_item = menu_item(edit, "_Undo", G_CALLBACK(cb_undo), e, nullptr, 0, NO_MODS);
    show_accel(e->undo_item, GDK_KEY_z, GDK_CONTROL_MASK);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit), gtk_separator_menu_item_new());
    item = menu_item(edit, "Cu_t", G_CALLBACK(cb_cut), e, nullptr, 0, NO_MODS);
    show_accel(item, GDK_KEY_x, GDK_CONTROL_MASK);
    g_ptr_array_add(e->need_selection, item);
    item = menu_item(edit, "_Copy", G_CALLBACK(cb_copy), e, nullptr, 0, NO_MODS);
    show_accel(item, GDK_KEY_c, GDK_CONTROL_MASK);
    g_ptr_array_add(e->need_selection, item);
    item = menu_item(edit, "_Paste", G_CALLBACK(cb_paste), e, nullptr, 0, NO_MODS);
    show_accel(item, GDK_KEY_v, GDK_CONTROL_MASK);
    g_ptr_array_add(e->need_clipboard, item);
    g_ptr_array_add(e->folders_only, item);
    item = menu_item(edit, "Paste _Shortcut", G_CALLBACK(cb_paste_shortcut), e, nullptr, 0,
                     NO_MODS);
    g_ptr_array_add(e->need_clipboard, item);
    g_ptr_array_add(e->folders_only, item);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit), gtk_separator_menu_item_new());
    item = menu_item(edit, "Copy To _Folder...", G_CALLBACK(cb_copy_to), e, nullptr, 0, NO_MODS);
    g_ptr_array_add(e->need_selection, item);
    item = menu_item(edit, "Mo_ve To Folder...", G_CALLBACK(cb_move_to), e, nullptr, 0, NO_MODS);
    g_ptr_array_add(e->need_selection, item);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit), gtk_separator_menu_item_new());
    g_signal_connect(edit, "show", G_CALLBACK(update_menu_items), e);
    /* The folder view handles Ctrl+A itself; bound here, it would take
     * the key from the address bar. */
    show_accel(menu_item(edit, "Select _All", G_CALLBACK(cb_select_all), e, nullptr, 0, NO_MODS),
               GDK_KEY_a, GDK_CONTROL_MASK);
    menu_item(edit, "_Invert Selection", G_CALLBACK(cb_invert), e, nullptr, 0, NO_MODS);

    GtkWidget *view = submenu(bar, "_View");
    GSList *group = nullptr;
    for (int i = 0; i < XD_VIEW_COUNT; i++) {
        GtkWidget *radio = gtk_radio_menu_item_new_with_mnemonic(group, view_names[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
        g_object_set_data(G_OBJECT(radio), "xd-mode", GINT_TO_POINTER(i));
        g_signal_connect(radio, "toggled", G_CALLBACK(on_view_item), e);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), radio);
        e->view_items[i] = radio;
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(view), gtk_separator_menu_item_new());
    e->arrange_item = gtk_menu_item_new_with_mnemonic("Arran_ge Icons by");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(e->arrange_item), arrange_menu(e, TRUE));
    gtk_menu_shell_append(GTK_MENU_SHELL(view), e->arrange_item);
    g_signal_connect(view, "show", G_CALLBACK(on_view_menu_show), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), gtk_separator_menu_item_new());
    GtkWidget *hidden = gtk_check_menu_item_new_with_mnemonic("Show _Hidden Files");
    e->hidden_item = hidden;
    g_signal_connect(hidden, "toggled", G_CALLBACK(on_hidden), e);
    gtk_widget_add_accelerator(hidden, "activate", accel, GDK_KEY_h, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), hidden);
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), e, accel, GDK_KEY_F5, NO_MODS);
    e->control_sep = gtk_separator_menu_item_new();
    gtk_widget_set_no_show_all(e->control_sep, TRUE);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), e->control_sep);
    e->control_item = menu_item(view, "Switch to Classic View", G_CALLBACK(cb_control_switch), e,
                                nullptr, 0, NO_MODS);
    gtk_widget_set_no_show_all(e->control_item, TRUE);

    GtkWidget *go = submenu(bar, "_Go");
    menu_item(go, "_Back", G_CALLBACK(cb_back), e, accel, GDK_KEY_Left, GDK_MOD1_MASK);
    menu_item(go, "_Forward", G_CALLBACK(cb_forward), e, accel, GDK_KEY_Right, GDK_MOD1_MASK);
    menu_item(go, "_Up One Level", G_CALLBACK(cb_up), e, accel, GDK_KEY_Up, GDK_MOD1_MASK);
    menu_item(go, "_Home Folder", G_CALLBACK(cb_home), e, accel, GDK_KEY_Home, GDK_MOD1_MASK);

    GtkWidget *tools = submenu(bar, "_Tools");
    menu_item(tools, "Folder _Options...", G_CALLBACK(cb_folder_options), e, nullptr, 0, NO_MODS);

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

static void destroy_menu_later(GtkMenuShell *menu, gpointer data);

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
    /* A new menu each time: gone once it's closed. */
    gtk_menu_attach_to_widget(GTK_MENU(menu), button, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), button, GDK_GRAVITY_SOUTH_WEST,
                             GDK_GRAVITY_NORTH_WEST, nullptr);
}

/* GTK deactivates a menu before running the chosen item, so destroying it
 * on "deactivate" would drop the click; destroy it once that's done. */
static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu_later(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

/* Right-click menu: Open and Properties on items; view options and the
 * folder's Properties on the background. */
static void add_view_submenu(explorer *e, GtkWidget *menu) {
    GtkWidget *viewm = gtk_menu_new();
    GtkWidget *top = gtk_menu_item_new_with_mnemonic("_View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), viewm);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
    for (int i = 0; i < XD_VIEW_COUNT; i++) {
        GtkWidget *item = gtk_menu_item_new_with_label(view_names[i]);
        g_signal_connect_swapped(item, "activate", G_CALLBACK(gtk_check_menu_item_set_active),
                                 e->view_items[i]);
        gtk_menu_shell_append(GTK_MENU_SHELL(viewm), item);
    }
    GtkWidget *arrange = gtk_menu_item_new_with_mnemonic("Arran_ge Icons by");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(arrange), arrange_menu(e, FALSE));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), arrange);
}

static GtkWidget *bold_item(explorer *e, GtkWidget *menu, const char *label, GCallback cb) {
    GtkWidget *item = menu_item(menu, label, cb, e, nullptr, 0, NO_MODS);
    char *markup = g_strdup_printf("<b>%s</b>", label);
    gtk_label_set_markup_with_mnemonic(GTK_LABEL(gtk_bin_get_child(GTK_BIN(item))), markup);
    g_free(markup);
    return item;
}

/* Right-click menus. In folders: Open, Cut, Copy, Paste into a folder,
 * Create Shortcut, Delete, Rename; on the background View, Refresh,
 * Paste and New. My Computer's drives only open; the Recycle Bin's items
 * Restore, Cut and Delete, and its background empties it. Control
 * Panel's items open; its background switches between its two views. */
static void on_context_menu(XdFolderView *view, GdkEvent *event, gpointer data) {
    explorer *e = data;
    XdPlace place = current_place(e);
    GtkWidget *menu = gtk_menu_new();
    gboolean selected = xd_folder_view_get_selected_count(view) > 0;
    if (selected && place == XD_PLACE_TRASH) {
        bold_item(e, menu, "R_estore", G_CALLBACK(cb_restore));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "Cu_t", G_CALLBACK(cb_cut), e, nullptr, 0, NO_MODS);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "_Delete", G_CALLBACK(cb_delete), e, nullptr, 0, NO_MODS);
    } else if (selected && place == XD_PLACE_CONTROL) {
        bold_item(e, menu, "_Open", G_CALLBACK(cb_open_selected));
    } else if (place == XD_PLACE_CONTROL) {
        add_view_submenu(e, menu);
        menu_item(menu, "R_efresh", G_CALLBACK(cb_refresh), e, nullptr, 0, NO_MODS);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, xd_control_classic_view() ? "Switch to Category View"
                                                  : "Switch to Classic View",
                  G_CALLBACK(cb_control_switch), e, nullptr, 0, NO_MODS);
    } else if (selected && place == XD_PLACE_COMPUTER) {
        bold_item(e, menu, "_Open", G_CALLBACK(cb_open_selected));
        GList *files = xd_folder_view_get_selected_files(view);
        gboolean eject = FALSE;
        for (GList *l = files; l != nullptr; l = l->next)
            eject = eject || xd_computer_can_eject(l->data);
        g_list_free_full(files, g_object_unref);
        if (eject) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            menu_item(menu, "E_ject", G_CALLBACK(cb_eject), e, nullptr, 0, NO_MODS);
        }
    } else if (selected) {
        bold_item(e, menu, "_Open", G_CALLBACK(cb_open_selected));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        GtkWidget *send = gtk_menu_item_new_with_mnemonic("Se_nd To");
        GList *files = xd_folder_view_get_selected_files(view);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(send), send_to_menu(e, files));
        g_list_free_full(files, g_object_unref);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), send);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "Cu_t", G_CALLBACK(cb_cut), e, nullptr, 0, NO_MODS);
        menu_item(menu, "_Copy", G_CALLBACK(cb_copy), e, nullptr, 0, NO_MODS);
        /* On one folder, Paste puts the files in it. */
        GFile *target = paste_target(e);
        if (!g_file_equal(target, current_dir(e))) {
            GtkWidget *paste = menu_item(menu, "_Paste", G_CALLBACK(cb_paste_into), e, nullptr,
                                         0, NO_MODS);
            gtk_widget_set_sensitive(paste, xd_clipboard_has_files());
        }
        g_object_unref(target);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "Create _Shortcut", G_CALLBACK(cb_shortcut), e, nullptr, 0, NO_MODS);
        menu_item(menu, "_Delete", G_CALLBACK(cb_delete), e, nullptr, 0, NO_MODS);
        menu_item(menu, "Rena_me", G_CALLBACK(cb_rename), e, nullptr, 0, NO_MODS);
    } else if (place == XD_PLACE_TRASH) {
        GtkWidget *empty = menu_item(menu, "Empty Recycle _Bin", G_CALLBACK(cb_empty_trash), e,
                                     nullptr, 0, NO_MODS);
        gtk_widget_set_sensitive(empty, xd_folder_view_get_item_count(view) > 0);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        add_view_submenu(e, menu);
        menu_item(menu, "R_efresh", G_CALLBACK(cb_refresh), e, nullptr, 0, NO_MODS);
    } else {
        add_view_submenu(e, menu);
        menu_item(menu, "R_efresh", G_CALLBACK(cb_refresh), e, nullptr, 0, NO_MODS);
        if (place == XD_PLACE_FOLDER) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            GtkWidget *paste = menu_item(menu, "_Paste", G_CALLBACK(cb_paste), e, nullptr, 0,
                                         NO_MODS);
            gtk_widget_set_sensitive(paste, xd_clipboard_has_files());
            GtkWidget *paste_link = menu_item(menu, "Paste _Shortcut",
                                              G_CALLBACK(cb_paste_shortcut), e, nullptr, 0,
                                              NO_MODS);
            gtk_widget_set_sensitive(paste_link, xd_clipboard_has_files());
            /* XP offers the last operation's undo here too. */
            if (xd_undo_label() != nullptr)
                show_accel(menu_item(menu, xd_undo_label(), G_CALLBACK(cb_undo), e, nullptr, 0,
                                     NO_MODS),
                           GDK_KEY_z, GDK_CONTROL_MASK);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            add_new_menu(e, menu);
        }
    }
    if (place == XD_PLACE_FOLDER || (selected && place != XD_PLACE_CONTROL)) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "P_roperties", G_CALLBACK(cb_properties), e, nullptr, 0, NO_MODS);
    }
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(view), nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    if (event != nullptr)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(view), GDK_GRAVITY_NORTH_WEST,
                                 GDK_GRAVITY_NORTH_WEST, nullptr);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    explorer *e = data;
    /* Backspace goes up, as in Windows, unless typing in the address bar. */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(e->window));
    if (ev->keyval == GDK_KEY_BackSpace && !GTK_IS_EDITABLE(focus)) {
        go_up(e);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_l && (ev->state & GDK_CONTROL_MASK)) {
        gtk_widget_grab_focus(e->address);
        return TRUE;
    }
    if (focus == GTK_WIDGET(e->view) && file_key(e, ev))
        return TRUE;
    (void)w;
    return FALSE;
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    explorer *e = data;
    remember_folder_view(e);
    free_history(&e->back);
    free_history(&e->forward);
    g_ptr_array_unref(e->need_selection);
    g_ptr_array_unref(e->need_clipboard);
    g_ptr_array_unref(e->folders_only);
    g_ptr_array_unref(e->trash_only);
    g_clear_object(&e->renaming);
    g_clear_object(&e->pending_rename);
    g_clear_handle_id(&e->commit_idle, g_source_remove);
    g_free(e);
}

/* ---- Folder Options ---------------------------------------------------------------- */

static void apply_options(explorer *e) {
    xd_folder_view_set_show_hidden(e->view, options.show_hidden);
    xd_folder_view_set_hide_extensions(e->view, options.hide_extensions);
    xd_folder_view_set_single_click(e->view, options.single_click);
    e->syncing = TRUE;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(e->hidden_item), options.show_hidden);
    e->syncing = FALSE;
    update_chrome(e);
}

static void load_options(void) {
    xd_settings s;
    xd_settings_load(&s);
    options = s.folders;
    /* Apply to All Folders' view, for folders without their own. */
    if (!view_given) {
        initial_view = XD_VIEW_ICONS;
        for (int i = 0; i < XD_VIEW_COUNT; i++)
            if (g_ascii_strcasecmp(options.default_view, view_names[i]) == 0)
                initial_view = i;
    }
}

static gboolean on_settings_changed(int fd, [[maybe_unused]] GIOCondition cond, gpointer app) {
    if (!xd_settings_watch_changed(fd))
        return G_SOURCE_CONTINUE;
    load_options();
    for (GList *l = gtk_application_get_windows(app); l != nullptr; l = l->next) {
        explorer *e = g_object_get_data(G_OBJECT(l->data), "xd-explorer");
        if (e != nullptr)
            apply_options(e);
    }
    return G_SOURCE_CONTINUE;
}

static void open_window(GtkApplication *app, GFile *dir) {
    explorer *e = g_new0(explorer, 1);
    e->need_selection = g_ptr_array_new();
    e->need_clipboard = g_ptr_array_new();
    e->folders_only = g_ptr_array_new();
    e->trash_only = g_ptr_array_new();
    e->window = gtk_application_window_new(app);
    g_object_set_data(G_OBJECT(e->window), "xd-explorer", e);
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
    g_signal_connect(e->view, "context-menu", G_CALLBACK(on_context_menu), e);
    g_signal_connect(e->view, "contents-changed", G_CALLBACK(on_contents), e);
    g_signal_connect(e->view, "selection-changed", G_CALLBACK(on_selection), e);
    g_signal_connect(e->view, "load-error", G_CALLBACK(on_load_error), e);
    g_signal_connect(e->view, "files-dropped", G_CALLBACK(on_files_dropped), e);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(e->view));
    g_signal_connect(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(e->view)), "value-changed",
                     G_CALLBACK(on_view_scrolled), e);
    g_signal_connect(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(e->view)), "value-changed",
                     G_CALLBACK(on_view_scrolled), e);
    /* Renaming in place: an entry floating over the view. */
    e->overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(e->overlay), scroll);
    e->rename_entry = gtk_entry_new();
    gtk_widget_set_halign(e->rename_entry, GTK_ALIGN_START);
    gtk_widget_set_valign(e->rename_entry, GTK_ALIGN_START);
    gtk_entry_set_width_chars(GTK_ENTRY(e->rename_entry), 1);
    gtk_widget_set_no_show_all(e->rename_entry, TRUE);
    g_signal_connect(e->rename_entry, "activate", G_CALLBACK(on_rename_activate), e);
    g_signal_connect(e->rename_entry, "key-press-event", G_CALLBACK(on_rename_key), e);
    g_signal_connect(e->rename_entry, "focus-out-event", G_CALLBACK(on_rename_focus_out), e);
    gtk_overlay_add_overlay(GTK_OVERLAY(e->overlay), e->rename_entry);
    /* Control Panel's link to its other view (XP has it in the task pane). */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    e->control_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_no_show_all(e->control_bar, TRUE);
    GtkWidget *control_icon = gtk_image_new_from_icon_name("preferences-system",
                                                           GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_margin_start(control_icon, 6);
    gtk_box_pack_start(GTK_BOX(e->control_bar), control_icon, FALSE, FALSE, 0);
    e->control_link = gtk_link_button_new_with_label(XD_CONTROL_URI, "Switch to Classic View");
    gtk_widget_set_tooltip_text(e->control_link, nullptr);
    g_signal_connect(e->control_link, "activate-link", G_CALLBACK(on_control_link), e);
    gtk_box_pack_start(GTK_BOX(e->control_bar), e->control_link, FALSE, FALSE, 0);
    gtk_widget_show(control_icon);
    gtk_widget_show(e->control_link);
    gtk_box_pack_start(GTK_BOX(right), e->control_bar, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(right), e->overlay, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);
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
    apply_options(e);
    set_mode(e, (XdViewMode)initial_view);
    navigate(e, dir, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(e->view));
}

/* ---- jobs for the desktop ---------------------------------------------------------- */

static void job_done([[maybe_unused]] GList *created, gpointer app) {
    g_application_release(G_APPLICATION(app));
}

static void job_moved(GList *created, gpointer app) {
    xd_trash_tidy(); /* in case they came out of the Recycle Bin */
    job_done(created, app);
}

/* Windows' Rename box isn't a dialog (it's in place), but the desktop's
 * icons are drawn by the shell: a small one stands in. */
static void rename_dialog(GFile *file) {
    char *old = xd_rename_text(file);
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Rename", nullptr, GTK_DIALOG_MODAL, "OK",
                                                 GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
                                                 nullptr);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "system-file-manager");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *label = gtk_label_new_with_mnemonic("_New name:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), old);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_widget_show_all(dlg);
    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        if (*text == '\0' || strcmp(text, old) == 0)
            break;
        char *msg = nullptr;
        GError *err = nullptr;
        GFile *renamed = strchr(text, '/') != nullptr
                             ? nullptr
                             : xd_rename(file, text, &err);
        if (renamed != nullptr) {
            g_object_unref(renamed);
            break;
        }
        if (err == nullptr)
            msg = g_strdup("A file name cannot contain the following character:\n        /");
        else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS))
            msg = g_strdup_printf("Cannot rename %s: A file with the name you specified already "
                                  "exists. Specify a different file name.",
                                  old);
        else
            msg = g_strdup_printf("Cannot rename %s: %s", old, err->message);
        g_clear_error(&err);
        xd_message_box(GTK_WINDOW(dlg), "Error Renaming File or Folder", XD_MB_ERROR, XD_MB_OK,
                       FALSE, msg);
        g_free(msg);
    }
    gtk_widget_destroy(dlg);
    g_free(old);
}

static void run_job(GtkApplication *app, GFile **files, gint n) {
    GList *list = nullptr;
    for (gint i = n; i > 0; i--)
        list = g_list_prepend(list, files[i - 1]);
    switch (job) {
    case JOB_TRASH:
    case JOB_DELETE:
        g_application_hold(G_APPLICATION(app));
        if (!xd_fileop_start_checked(nullptr, job == JOB_TRASH ? XD_FILEOP_TRASH : XD_FILEOP_DELETE,
                                     list, nullptr, job_done, app))
            g_application_release(G_APPLICATION(app));
        break;
    case JOB_RENAME:
        if (n > 0)
            rename_dialog(files[0]);
        break;
    case JOB_EMPTY_TRASH:
        g_application_hold(G_APPLICATION(app));
        xd_trash_empty(nullptr, job_done, app);
        break;
    case JOB_COPY:
    case JOB_MOVE: {
        /* The desktop's Paste: the "Copying..." dialog and its questions. */
        if (list == nullptr || job_dest == nullptr)
            break;
        GFile *dest = g_file_new_for_commandline_arg(job_dest);
        g_application_hold(G_APPLICATION(app));
        xd_fileop_start(nullptr, job == JOB_COPY ? XD_FILEOP_COPY : XD_FILEOP_MOVE, list, dest,
                        job == JOB_MOVE ? job_moved : job_done, app);
        g_object_unref(dest);
        break;
    }
    case JOB_NONE:
        break;
    }
    g_list_free(list);
}

static void on_activate(GtkApplication *app, [[maybe_unused]] gpointer data) {
    if (job != JOB_NONE) {
        run_job(app, nullptr, 0);
        return;
    }
    if (properties_only)
        return; /* --properties without files: nothing to show */
    GFile *home = g_file_new_for_path(g_get_home_dir());
    open_window(app, home);
    g_object_unref(home);
}

static void on_open(GtkApplication *app, GFile **files, gint n, [[maybe_unused]] const char *hint,
                    [[maybe_unused]] gpointer data) {
    if (properties_only) {
        /* One dialog for all of them, as for a multiple selection. */
        GList *list = nullptr;
        for (gint i = n; i > 0; i--)
            list = g_list_prepend(list, files[i - 1]);
        GtkWidget *dlg = xd_properties_show(nullptr, list);
        g_list_free(list);
        if (dlg != nullptr)
            gtk_window_set_application(GTK_WINDOW(dlg), app);
        return;
    }
    if (job != JOB_NONE) {
        run_job(app, files, n);
        return;
    }
    for (gint i = 0; i < n; i++)
        open_window(app, files[i]);
}

/* --view=thumbnails|tiles|icons|list|details */
static gint on_options([[maybe_unused]] GApplication *app, GVariantDict *opts,
                       [[maybe_unused]] gpointer data) {
    const char *view = nullptr;
    if (g_variant_dict_contains(opts, "properties"))
        properties_only = TRUE;
    if (g_variant_dict_contains(opts, "trash"))
        job = JOB_TRASH;
    if (g_variant_dict_contains(opts, "delete"))
        job = JOB_DELETE;
    if (g_variant_dict_contains(opts, "rename"))
        job = JOB_RENAME;
    if (g_variant_dict_contains(opts, "empty-trash"))
        job = JOB_EMPTY_TRASH;
    const char *dest = nullptr;
    if (g_variant_dict_lookup(opts, "copy-to", "^&ay", &dest)) {
        job = JOB_COPY;
        job_dest = g_strdup(dest);
    }
    if (g_variant_dict_lookup(opts, "move-to", "^&ay", &dest)) {
        job = JOB_MOVE;
        g_free(job_dest);
        job_dest = g_strdup(dest);
    }
    if (g_variant_dict_lookup(opts, "view", "&s", &view)) {
        for (int i = 0; i < XD_VIEW_COUNT; i++) {
            if (g_ascii_strcasecmp(view, view_names[i]) == 0) {
                initial_view = i;
                view_forced = TRUE;
                view_given = TRUE;
                return -1;
            }
        }
        g_printerr("xd-files: unknown view '%s'\n", view);
        return EXIT_FAILURE;
    }
    return -1; /* continue */
}

static void on_startup(GApplication *app, [[maybe_unused]] gpointer data) {
    xd_gtk_app_follow_theme();
    load_options();
    int fd = xd_settings_watch();
    if (fd >= 0)
        g_unix_fd_add(fd, G_IO_IN, on_settings_changed, app);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Files");
    /* Each window is its own process, like Windows' "separate process"
     * option: a hang in one folder can't freeze the others. */
    GtkApplication *app = gtk_application_new("org.xeondesktop.Files",
                                              G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    g_signal_connect(app, "open", G_CALLBACK(on_open), nullptr);
    g_application_add_main_option(G_APPLICATION(app), "view", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_STRING, "Thumbnails, Tiles, Icons, List or Details",
                                  "VIEW");
    g_application_add_main_option(G_APPLICATION(app), "properties", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Show the Properties of FILE... and exit",
                                  nullptr);
    g_application_add_main_option(G_APPLICATION(app), "trash", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE,
                                  "Send FILE... to the Recycle Bin (asking first) and exit",
                                  nullptr);
    g_application_add_main_option(G_APPLICATION(app), "delete", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Delete FILE... (asking first) and exit",
                                  nullptr);
    g_application_add_main_option(G_APPLICATION(app), "rename", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Ask for a new name for FILE and exit",
                                  nullptr);
    g_application_add_main_option(G_APPLICATION(app), "empty-trash", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Empty the Recycle Bin (asking first)",
                                  nullptr);
    g_application_add_main_option(G_APPLICATION(app), "copy-to", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_FILENAME, "Copy FILE... into DIR and exit", "DIR");
    g_application_add_main_option(G_APPLICATION(app), "move-to", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_FILENAME, "Move FILE... into DIR and exit", "DIR");
    g_signal_connect(app, "handle-local-options", G_CALLBACK(on_options), nullptr);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
