/* SPDX-License-Identifier: MIT
 * xd-files: the file manager, modelled on Windows XP's Explorer windows.
 *
 * GTK3 window: menu bar, toolbar (Back, Forward, Up, Folders, Views),
 * address bar, XP's task pane or the Folders tree on the left (show_side),
 * XdFolderView on the right and a status bar with the thumbnail size
 * slider.
 */
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <stdlib.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "../settings/state.h"
#include "chrome.h"
#include "drives.h"
#include "encrypted.h"
#include "extract.h"
#include "favorites.h"
#include "fileops.h"
#include "folderview.h"
#include "netdrives.h"
#include "netui.h"
#include "places.h"
#include "properties.h"
#include "sendto.h"
#include "taskpane.h"
#include "undo.h"

enum { COL_NAME, COL_ICON, COL_FILE, COL_LOADED, COL_COUNT };

static constexpr GdkModifierType NO_MODS = (GdkModifierType)0;
static constexpr GtkDialogFlags MODAL_DIALOG =
    (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);

typedef struct explorer explorer;

/* A tab: one folder with its own view (mode, sort, thumbnail size,
 * selection, scroll position), Back/Forward history and renaming in
 * place, as Windows 11's File Explorer keeps per tab. The window's chrome
 * (title, address bar, status bar, Folders tree, View menu) shows the
 * active one. */
typedef struct tab {
    explorer *e;
    /* Closed while the window stays open: kept (widgets gone) until the
     * window closes, so a mount or a name still on its way finds it and
     * does nothing. */
    gboolean closed;
    GtkWidget *page;  /* the notebook's page: the Control Panel bar and the view */
    GtkWidget *label; /* the tab: icon, name and close button */
    GtkWidget *label_icon, *label_text, *close_button;
    XdFolderView *view;
    /* Control Panel: the Switch to Classic/Category View link, in a bar
     * above the view. */
    GtkWidget *control_bar, *control_link;
    /* Renaming in place: an entry over the item's name. `pending_rename`
     * waits for a new item to appear. */
    GtkWidget *overlay;
    GtkWidget *rename_entry;
    GFile *renaming;
    GFile *pending_rename;
    gboolean committing;
    guint commit_idle;
    GList *back; /* GFile*, most recent first */
    GList *forward;
    /* A gvfs location's name as GIO gives it ("data on example.org"),
     * asked once per location; its own name is shown meanwhile. */
    GFile *named;
    char *gio_name;
    GCancellable *naming;
} tab;

struct explorer {
    GtkWidget *window;
    /* The tabs; the strip shows only with two or more (one folder looks
     * as XP's window does). */
    GtkWidget *notebook;
    tab *cur;          /* the active tab */
    GPtrArray *closed; /* tab*, the closed ones (see tab.closed) */
    GtkWidget *address;
    GtkWidget *tree;
    GtkTreeStore *store;
    GtkWidget *tree_pane;
    GtkWidget *tasks; /* XP's task pane, in the Folders bar's place when it's off */
    GtkWidget *side;  /* the Folders bar or the task pane: hidden with neither */
    GtkWidget *status;
    GtkWidget *status_size;
    GtkWidget *slider;
    GtkWidget *back_button, *forward_button, *up_button;
    GtkWidget *folders_toggle;
    GtkWidget *folders_item; /* View > Explorer Bar > Folders */
    GtkWidget *view_items[XD_VIEW_COUNT];
    GtkWidget *arrange_item; /* View > Arrange Icons By */
    /* Control Panel: View's Switch to Classic/Category View. */
    GtkWidget *control_item, *control_sep;
    GtkWidget *hidden_item;  /* View > Show Hidden Files */
    GtkWidget *undo_item;    /* Edit > Undo ... */
    GtkWidget *send_to_item; /* File > Send To */
    GtkWidget *add_favorite_item; /* Favorites > Add to Favorites... */
    GtkWidget *rename_item;       /* File > Rename: files, and drives in My Computer */
    /* Menu items that need a selection, or files on the clipboard. */
    GPtrArray *need_selection;
    GPtrArray *need_clipboard;
    /* Not for My Computer or the Recycle Bin (New, Paste, Rename...), and
     * only for the Recycle Bin (Restore, Empty Recycle Bin). */
    GPtrArray *folders_only;
    GPtrArray *trash_only;
    gboolean syncing; /* updating widgets programmatically */
};

static void update_tasks(explorer *e);

static XdPlace tab_place(tab *t) { return xd_place_of(xd_folder_view_get_location(t->view)); }

/* The tabs, in the strip's order (dragging reorders them). */
static guint tab_count(explorer *e) {
    return (guint)gtk_notebook_get_n_pages(GTK_NOTEBOOK(e->notebook));
}

static tab *tab_at(explorer *e, guint i) {
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(e->notebook), (int)i);
    return g_object_get_data(G_OBJECT(page), "xd-tab");
}

static void navigate(tab *t, GFile *dir, gboolean record);
static tab *add_tab(explorer *e, GFile *dir, gboolean activate);
static int initial_view = XD_VIEW_ICONS; /* from --view */
static gboolean view_forced;              /* --view given: it wins for the first folder */
static gboolean view_given;               /* --view given: Folder Options' default doesn't apply */
/* Folder Options ([Folders] in desktop.ini), followed live. */
static xd_folder_settings options;
static gboolean properties_only;          /* --properties: just the dialog */
static gboolean explore_option;           /* --explore: with the Folders bar, as XP's Explore */
/* --trash, --delete, --rename, --empty-trash, --copy-to, --move-to,
 * --map-drive, --disconnect-drive: one job for the desktop (the shell
 * isn't a GTK program), then exit. */
typedef enum {
    JOB_NONE,
    JOB_TRASH,
    JOB_DELETE,
    JOB_RENAME,
    JOB_EMPTY_TRASH,
    JOB_COPY,
    JOB_MOVE,
    JOB_MAP_DRIVE,
    JOB_DISCONNECT_DRIVE
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
    XdFolderView *view = e->cur->view;
    guint sel = xd_folder_view_get_selected_count(view);
    guint total = xd_folder_view_get_item_count(view);
    char *space = sel > 0 ? nullptr : free_space_text(xd_folder_view_get_location(view));
    char *text = sel > 0 ? g_strdup_printf("%u object%s selected", sel, sel == 1 ? "" : "s")
                         : g_strdup_printf("%u object%s%s", total, total == 1 ? "" : "s",
                                           space != nullptr ? space : "");
    g_free(space);
    gtk_label_set_text(GTK_LABEL(e->status), text);
    g_free(text);
    /* Control Panel's and My Network Places' items have no size. */
    XdPlace place = xd_place_of(xd_folder_view_get_location(view));
    char *size = place == XD_PLACE_CONTROL || place == XD_PLACE_NETWORK
                     ? g_strdup("")
                     : xd_format_size(xd_folder_view_get_size(view, sel > 0));
    gtk_label_set_text(GTK_LABEL(e->status_size), size);
    g_free(size);
    update_tasks(e);
}

static const char *control_switch_label(void) {
    return xd_control_classic_view() ? "Switch to Category View" : "Switch to Classic View";
}

/* Control Panel's "Switch to Classic View" / "Switch to Category View":
 * the link above a tab's view, and the View menu's item for the active
 * tab. */
static void update_control_switch(tab *t) {
    gboolean control = tab_place(t) == XD_PLACE_CONTROL;
    gtk_widget_set_visible(t->control_bar, control);
    gtk_button_set_label(GTK_BUTTON(t->control_link), control_switch_label());
    if (t != t->e->cur)
        return;
    gtk_widget_set_visible(t->e->control_item, control);
    gtk_widget_set_visible(t->e->control_sep, control);
    gtk_menu_item_set_label(GTK_MENU_ITEM(t->e->control_item), control_switch_label());
}

static void update_chrome(explorer *e);
static void update_tab_label(tab *t);

static void got_gio_name(GObject *src, GAsyncResult *res, gpointer data) {
    GError *err = nullptr;
    GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, &err);
    if (info == nullptr) {
        /* Cancelled: the tab or the window may be gone (don't touch it). */
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            tab *t = data;
            g_clear_object(&t->naming);
        }
        g_error_free(err);
        return;
    }
    tab *t = data;
    g_clear_object(&t->naming);
    const char *name = g_file_info_get_display_name(info);
    if (name != nullptr && *name != '\0') {
        g_set_object(&t->named, G_FILE(src));
        g_free(t->gio_name);
        t->gio_name = g_strdup(name);
        update_tab_label(t);
        if (t == t->e->cur)
            update_chrome(t->e);
    }
    g_object_unref(info);
}

/* The name to show for a location that isn't on a local disk: GIO's
 * display name (asked in the background), or nullptr until it's known. */
static const char *gio_name(tab *t, GFile *dir) {
    if (g_file_is_native(dir))
        return nullptr;
    if (t->named != nullptr && g_file_equal(t->named, dir))
        return t->gio_name;
    if (t->naming != nullptr) {
        g_cancellable_cancel(t->naming);
        g_clear_object(&t->naming);
    }
    g_clear_object(&t->named);
    g_clear_pointer(&t->gio_name, g_free);
    t->naming = g_cancellable_new();
    g_file_query_info_async(dir, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, G_FILE_QUERY_INFO_NONE,
                            G_PRIORITY_DEFAULT, t->naming, got_gio_name, t);
    return nullptr;
}

/* A tab's folder's names: `parse` the full one (a special place's title),
 * `base` its own (nullptr for the special places), `named` GIO's name of
 * a network folder when known. Free `parse` and `base` with g_free. */
static void folder_names(tab *t, GFile *dir, char **parse, char **base, const char **named) {
    const char *special = xd_place_title(dir);
    *parse = special != nullptr ? g_strdup(special) : g_file_get_parse_name(dir);
    *named = special != nullptr ? nullptr : gio_name(t, dir);
    *base = special != nullptr ? nullptr
            : *named != nullptr ? g_strdup(*named)
                                : g_file_get_basename(dir);
}

enum { TAB_NAME_CHARS = 20 };

/* The tab shows the folder's icon and name (the full path in its
 * tooltip), as Windows 11's tabs do. */
static void update_tab_label(tab *t) {
    GFile *dir = xd_folder_view_get_location(t->view);
    if (dir == nullptr)
        return;
    char *parse = nullptr, *base = nullptr;
    const char *named = nullptr;
    folder_names(t, dir, &parse, &base, &named);
    const char *name = base != nullptr ? base : parse;
    gtk_label_set_text(GTK_LABEL(t->label_text), name);
    /* The whole name up to a point, then "..." (a notebook gives a tab
     * only the least its label asks for). */
    gboolean is_long = g_utf8_strlen(name, -1) > TAB_NAME_CHARS;
    gtk_label_set_ellipsize(GTK_LABEL(t->label_text),
                            is_long ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE);
    gtk_label_set_width_chars(GTK_LABEL(t->label_text), is_long ? TAB_NAME_CHARS : -1);
    gtk_widget_set_tooltip_text(t->label, parse);
    gtk_image_set_from_icon_name(GTK_IMAGE(t->label_icon), xd_place_icon(dir), GTK_ICON_SIZE_MENU);
    g_free(base);
    g_free(parse);
}

/* The window's title, address bar and buttons, for the active tab. */
static void update_chrome(explorer *e) {
    tab *t = e->cur;
    GFile *dir = xd_folder_view_get_location(t->view);
    if (dir == nullptr)
        return; /* nothing loaded yet */
    char *parse = nullptr, *base = nullptr;
    const char *named = nullptr;
    folder_names(t, dir, &parse, &base, &named);
    /* Folder Options: the folder's name alone, unless the full path. */
    const char *shown = options.full_path_address || base == nullptr ? parse : base;
    g_object_set_data_full(G_OBJECT(e->address), "xd-shown", g_strdup(shown), g_free);
    e->syncing = TRUE;
    gtk_entry_set_text(GTK_ENTRY(e->address), shown);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(e->address), GTK_ENTRY_ICON_PRIMARY,
                                      xd_place_icon(dir));
    e->syncing = FALSE;
    gboolean short_title = !options.full_path_title && base != nullptr &&
                           (g_file_has_parent(dir, nullptr) || named != nullptr);
    gtk_window_set_title(GTK_WINDOW(e->window), short_title ? base : parse);
    g_free(base);
    g_free(parse);
    gtk_widget_set_sensitive(e->back_button, t->back != nullptr);
    gtk_widget_set_sensitive(e->forward_button, t->forward != nullptr);
    /* A drive's Up is My Computer, as in XP; so is Control Panel's, whose
     * category pages go up to it. */
    const char *special = xd_place_title(dir);
    char *uri = g_file_get_uri(dir);
    gtk_widget_set_sensitive(e->up_button,
                             special == nullptr || xd_place_of(dir) == XD_PLACE_CONTROL ||
                                 strcmp(uri, XD_ENTIRE_NETWORK_URI) == 0 ||
                                 strcmp(uri, XD_WINDOWS_NETWORK_URI) == 0);
    g_free(uri);
    gtk_widget_set_visible(e->slider, xd_folder_view_get_mode(t->view) == XD_VIEW_THUMBNAILS);
    update_control_switch(t);
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
        if (!g_file_equal(file, xd_folder_view_get_location(e->cur->view)))
            navigate(e->cur, file, TRUE);
        g_object_unref(file);
    }
}

/* The middle button opens a folder of the tree in a new tab, behind the
 * one shown (Windows 11's navigation pane). */
static gboolean on_tree_press(GtkWidget *tree, GdkEventButton *ev, gpointer data) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_MIDDLE)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), (int)ev->x, (int)ev->y, &path,
                                       nullptr, nullptr, nullptr))
        return TRUE;
    explorer *e = data;
    GtkTreeIter iter;
    GFile *file = nullptr;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(e->store), &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(e->store), &iter, COL_FILE, &file, -1);
    gtk_tree_path_free(path);
    if (file != nullptr) {
        add_tab(e, file, FALSE);
        g_object_unref(file);
    }
    return TRUE;
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
    gboolean match =
        file != nullptr && g_file_equal(file, xd_folder_view_get_location(e->cur->view));
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

static void set_mode(tab *t, XdViewMode mode);

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
static void remember_positions(tab *t, const char *uri) {
    char *path = nullptr;
    GKeyFile *kf = load_positions(&path);
    gboolean had = g_key_file_has_group(kf, uri);
    XdViewMode mode = XD_VIEW_ICONS;
    char **names = nullptr;
    int *xy = nullptr;
    gsize n = xd_folder_view_get_positions(t->view, &mode, &names, &xy);
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
static void restore_positions(tab *t, const char *uri) {
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
        xd_folder_view_set_positions(t->view, (XdViewMode)mode, names, kept, k);
        g_strfreev(names);
        g_free(kept);
    }
    g_strfreev(escaped);
    g_free(xy);
    g_key_file_unref(kf);
}

/* Keeps the folder's view (most recent first), unless it's the default
 * and the folder wasn't remembered before. */
static void remember_folder_view(tab *t) {
    GFile *dir = xd_folder_view_get_location(t->view);
    /* Control Panel always opens in its view's own look (below); with
     * "Remember each folder's view settings" off nothing is kept. */
    if (dir == nullptr || xd_place_of(dir) == XD_PLACE_CONTROL || !options.remember_views)
        return;
    char *uri = g_file_get_uri(dir);
    remember_positions(t, uri);
    XdViewMode view_mode = xd_folder_view_get_mode(t->view);
    int mode = (int)view_mode;
    XdSortColumn sort_col = xd_folder_view_get_sort(t->view);
    int sort = (int)sort_col;
    gboolean desc = xd_folder_view_get_sort_descending(t->view);
    gboolean groups = xd_folder_view_get_groups(t->view);
    char *columns = xd_folder_view_get_columns(t->view);
    gboolean arrange = xd_folder_view_get_auto_arrange(t->view);
    gboolean align = xd_folder_view_get_align_to_grid(t->view);
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
static void apply_folder_view(tab *t, GFile *dir) {
    if (view_forced) {
        view_forced = FALSE; /* --view, for the folder it opened */
        return;
    }
    if (xd_place_of(dir) == XD_PLACE_CONTROL) {
        /* Category View's categories as tiles (XP's big links); applets
         * as icons, as Classic View and the category pages show them. */
        gboolean categories = xd_control_page(dir) == nullptr && !xd_control_classic_view();
        set_mode(t, categories ? XD_VIEW_TILES : XD_VIEW_ICONS);
        xd_folder_view_set_sort(t->view, XD_SORT_NAME, FALSE);
        xd_folder_view_set_groups(t->view, FALSE);
        xd_folder_view_set_columns(t->view, nullptr);
        xd_folder_view_set_auto_arrange(t->view, TRUE);
        return;
    }
    /* Not remembering: every folder keeps the view last chosen, as in XP. */
    if (!options.remember_views)
        return;
    char *uri = g_file_get_uri(dir);
    folder_view_setting fv = {0};
    if (find_folder_view(uri, &fv)) {
        set_mode(t, (XdViewMode)fv.mode);
        xd_folder_view_set_auto_arrange(t->view, fv.auto_arrange);
        xd_folder_view_set_align_to_grid(t->view, fv.align_grid);
        xd_folder_view_set_sort(t->view, (XdSortColumn)fv.sort, fv.desc);
        if (xd_folder_view_get_groups(t->view) != fv.groups)
            xd_folder_view_set_groups(t->view, fv.groups);
        xd_folder_view_set_columns(t->view, fv.columns);
        if (!fv.auto_arrange)
            restore_positions(t, uri);
        g_free(fv.columns);
    } else {
        char *columns = xd_folder_view_get_columns(t->view);
        if (xd_folder_view_get_mode(t->view) != (XdViewMode)initial_view ||
            xd_folder_view_get_sort(t->view) != XD_SORT_NAME ||
            xd_folder_view_get_sort_descending(t->view) || xd_folder_view_get_groups(t->view) ||
            columns != nullptr || !xd_folder_view_get_auto_arrange(t->view) ||
            !xd_folder_view_get_align_to_grid(t->view)) {
            set_mode(t, (XdViewMode)initial_view);
            xd_folder_view_set_auto_arrange(t->view, TRUE);
            xd_folder_view_set_align_to_grid(t->view, TRUE);
            xd_folder_view_set_sort(t->view, XD_SORT_NAME, FALSE);
            xd_folder_view_set_groups(t->view, FALSE);
            xd_folder_view_set_columns(t->view, nullptr);
        }
        g_free(columns);
    }
    g_free(uri);
}

/* ---- drives that aren't mounted yet --------------------------------------------- */

/* A mount waiting to finish; the window may close meanwhile. */
typedef struct mount_request {
    GtkWidget *window; /* weak */
    tab *t;            /* nullptr: Eject */
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
    } else if (r->window != nullptr && !r->t->closed) {
        GMount *m = g_volume_get_mount(vol);
        if (m != nullptr) {
            GFile *root = g_mount_get_root(m);
            navigate(r->t, root, r->record);
            g_object_unref(root);
            g_object_unref(m);
        }
    }
    mount_request_free(r);
}

static void encrypted_opened(GFile *root, gpointer data) {
    tab *t = data;
    if (!t->closed)
        navigate(t, root, TRUE);
}

static void encrypted_opened_again(GFile *root, gpointer data) {
    tab *t = data;
    if (!t->closed)
        navigate(t, root, FALSE);
}

/* Opening a drive that isn't mounted mounts it first, as XP's drives are
 * simply there; an encrypted one is unlocked first. */
static gboolean mount_and_open(tab *t, GFile *item, gboolean record) {
    GtkWidget *window = t->e->window;
    if (xd_encrypted_needs_unlock(item)) {
        xd_encrypted_open(GTK_WINDOW(window), item,
                          record ? encrypted_opened : encrypted_opened_again, t);
        return TRUE;
    }
    GVolume *vol = xd_computer_volume(item);
    if (vol == nullptr)
        return FALSE;
    mount_request *r = g_new0(mount_request, 1);
    r->window = window;
    g_object_add_weak_pointer(G_OBJECT(r->window), (gpointer *)&r->window);
    r->t = t;
    r->record = record;
    r->name = g_volume_get_name(vol);
    GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(window));
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

static void cb_lock([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    for (GList *l = files; l != nullptr; l = l->next)
        if (xd_encrypted_can_lock(l->data))
            xd_encrypted_lock(GTK_WINDOW(e->window), l->data);
    g_list_free_full(files, g_object_unref);
}

static void cb_eject([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    for (GList *l = files; l != nullptr; l = l->next)
        if (xd_computer_can_eject(l->data))
            eject_item(e, l->data);
    g_list_free_full(files, g_object_unref);
}

/* ---- network folders not connected yet -------------------------------------------- */

static void show_location(tab *t, GFile *dir, gboolean record);

static void connected(tab *t, GFile *location, const GError *error, gboolean record) {
    if (t->closed)
        return;
    if (error == nullptr) {
        show_location(t, location, record);
        return;
    }
    /* "Handled": the user cancelled the password question. */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
        return;
    char *uri = g_file_get_uri(location);
    char *shown = xd_net_display_path(uri);
    char *msg = g_strdup_printf("%s is not accessible.\n\n%s", shown, error->message);
    xd_message_box(GTK_WINDOW(t->e->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
    g_free(msg);
    g_free(shown);
    g_free(uri);
}

/* xd_net_mount calls these only while the window is open. */
static void connected_recorded(GFile *location, const GError *error, gpointer data) {
    connected(data, location, error, TRUE);
}

static void connected_unrecorded(GFile *location, const GError *error, gpointer data) {
    connected(data, location, error, FALSE);
}

/* Connect As's user name for a mapped drive at `uri`, or nullptr. Free
 * with g_free. */
static char *mapped_user(const char *uri) {
    char *config = xd_net_config_path();
    GPtrArray *drives = xd_net_drives_load(config);
    g_free(config);
    char *user = nullptr;
    for (guint i = 0; i < drives->len && user == nullptr; i++) {
        XdNetDrive *d = g_ptr_array_index(drives, i);
        if (d->user != nullptr && xd_net_same_uri(d->uri, uri))
            user = g_strdup(d->user);
    }
    g_ptr_array_unref(drives);
    return user;
}

/* A network folder that isn't connected (a mapped drive at logon, a
 * share opened from the network, an address typed) is mounted first,
 * asking for a password in XP's "Connect to server" box, then opened. A
 * kind GIO can't open here is explained. */
static gboolean connect_and_open(tab *t, GFile *dir, gboolean record) {
    GtkWidget *window = t->e->window;
    if (g_file_is_native(dir) || xd_place_of(dir) != XD_PLACE_FOLDER)
        return FALSE;
    char *uri = g_file_get_uri(dir);
    char *scheme = g_file_get_uri_scheme(dir);
    gboolean supported = xd_net_scheme_supported(scheme);
    g_free(scheme);
    if (!supported) {
        char *msg = xd_net_unsupported_text(uri);
        xd_message_box(GTK_WINDOW(window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
        g_free(uri);
        return TRUE;
    }
    GError *err = nullptr;
    GMount *m = g_file_find_enclosing_mount(dir, nullptr, &err);
    gboolean not_mounted = m == nullptr && g_error_matches(err, G_IO_ERROR,
                                                          G_IO_ERROR_NOT_MOUNTED);
    g_clear_error(&err);
    g_clear_object(&m);
    if (not_mounted) {
        char *user = mapped_user(uri);
        xd_net_mount(GTK_WINDOW(window), dir, user, nullptr,
                     record ? connected_recorded : connected_unrecorded, t);
        g_free(user);
    }
    g_free(uri);
    return not_mounted;
}

static void navigate(tab *t, GFile *dir, gboolean record) {
    if (mount_and_open(t, dir, record) || connect_and_open(t, dir, record))
        return;
    show_location(t, dir, record);
}

static void show_location(tab *t, GFile *dir, gboolean record) {
    GFile *current = xd_folder_view_get_location(t->view);
    if (record && current != nullptr && !g_file_equal(current, dir)) {
        t->back = g_list_prepend(t->back, g_object_ref(current));
        free_history(&t->forward);
    }
    if (current != nullptr)
        remember_folder_view(t);
    apply_folder_view(t, dir);
    xd_folder_view_set_location(t->view, dir);
    update_tab_label(t);
    if (t == t->e->cur) {
        update_chrome(t->e);
        sync_tree(t->e);
    }
}

static void go_back(tab *t) {
    if (t->back == nullptr)
        return;
    GFile *target = t->back->data;
    t->back = g_list_delete_link(t->back, t->back);
    t->forward = g_list_prepend(t->forward, g_object_ref(xd_folder_view_get_location(t->view)));
    navigate(t, target, FALSE);
    g_object_unref(target);
}

static void go_forward(tab *t) {
    if (t->forward == nullptr)
        return;
    GFile *target = t->forward->data;
    t->forward = g_list_delete_link(t->forward, t->forward);
    t->back = g_list_prepend(t->back, g_object_ref(xd_folder_view_get_location(t->view)));
    navigate(t, target, FALSE);
    g_object_unref(target);
}

static void go_up(tab *t) {
    GFile *dir = xd_folder_view_get_location(t->view);
    if (dir == nullptr)
        return;
    if (xd_place_of(dir) == XD_PLACE_CONTROL) {
        /* A category's page is in Control Panel, which is in My Computer. */
        GFile *up = xd_control_page(dir) != nullptr ? xd_place_control() : xd_place_computer();
        navigate(t, up, TRUE);
        g_object_unref(up);
        return;
    }
    if (xd_place_of(dir) == XD_PLACE_NETWORK) {
        /* Entire Network is in My Network Places. */
        char *uri = g_file_get_uri(dir);
        if (strcmp(uri, XD_ENTIRE_NETWORK_URI) == 0) {
            GFile *up = g_file_new_for_uri(XD_NETWORK_URI);
            navigate(t, up, TRUE);
            g_object_unref(up);
        }
        g_free(uri);
        return;
    }
    if (xd_place_of(dir) != XD_PLACE_FOLDER)
        return;
    /* Windows' network goes up a level at a time: share, computer,
     * workgroup, Microsoft Windows Network, Entire Network. */
    gboolean smb = g_file_has_uri_scheme(dir, "smb");
    GFile *parent = smb && !g_file_has_parent(dir, nullptr)
                        ? g_file_new_for_uri(XD_ENTIRE_NETWORK_URI)
                    : !smb && xd_place_is_drive_root(dir) ? xd_place_computer()
                                                          : g_file_get_parent(dir);
    if (parent != nullptr) {
        navigate(t, parent, TRUE);
        g_object_unref(parent);
    }
}

static void go_home(tab *t) {
    GFile *home = g_file_new_for_path(g_get_home_dir());
    navigate(t, home, TRUE);
    g_object_unref(home);
}

static GFile *current_dir(explorer *e);
static explorer *open_window(GtkApplication *app, GFile *dir, gboolean explore);

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

/* What XP opens in a window of its own (a folder with "Open each folder
 * in its own window", My Network Places' and Map Network Drive's new
 * folders): a new tab in front instead, with Folder Options' "Open
 * folders in new tabs instead of new windows". */
static void open_elsewhere(explorer *e, GFile *location) {
    if (options.new_tabs)
        add_tab(e, location, TRUE);
    else
        open_window(gtk_window_get_application(GTK_WINDOW(e->window)), location, FALSE);
}

static void open_in_new_window(GFile *location, gpointer data) { open_elsewhere(data, location); }

/* Windows 2000's Add Network Place item: the wizard. */
static gboolean add_network_place_item(explorer *e, GFile *file) {
    char *uri = g_file_get_uri(file);
    gboolean is = strcmp(uri, XD_ADD_NETWORK_PLACE_URI) == 0;
    g_free(uri);
    if (is)
        xd_net_add_place(GTK_WINDOW(e->window), nullptr, open_in_new_window, e);
    return is;
}

static void activate_item(tab *t, GFile *file, gboolean is_dir) {
    explorer *e = t->e;
    XdPlace here = tab_place(t);
    if (here == XD_PLACE_NETWORK && add_network_place_item(e, file))
        return;
    if (here == XD_PLACE_TRASH) {
        /* Items in the Recycle Bin open their Properties, as in XP. */
        GList *one = g_list_append(nullptr, file);
        xd_properties_show(GTK_WINDOW(e->window), one);
        g_list_free(one);
        return;
    }
    if (is_dir && options.new_window) {
        open_elsewhere(e, file);
        return;
    }
    if (is_dir) {
        navigate(t, file, TRUE);
        return;
    }
    if (here == XD_PLACE_CONTROL && launch_applet(e, file))
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

static void on_activated([[maybe_unused]] XdFolderView *view, GFile *file, gboolean is_dir,
                         gpointer data) {
    activate_item(data, file, is_dir);
}

static gboolean is_directory(GFile *f) {
    return g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, nullptr) == G_FILE_TYPE_DIRECTORY;
}

/* Whether an item opens as a folder (in this window): My Computer's items
 * are all drives (some not mounted yet), My Network Places' are network
 * folders, and Control Panel's categories are pages of it. */
static gboolean opens_as_folder(tab *t, GFile *item, gboolean is_dir) {
    XdPlace here = tab_place(t);
    if (here == XD_PLACE_TRASH)
        return FALSE; /* its items open their Properties */
    return is_dir || here == XD_PLACE_COMPUTER || here == XD_PLACE_NETWORK ||
           xd_place_of(item) == XD_PLACE_CONTROL;
}

/* Whether an item can open in a tab: a folder, not Add Network Place
 * (a wizard). */
static gboolean opens_in_tab(tab *t, GFile *item, gboolean is_dir) {
    char *uri = g_file_get_uri(item);
    gboolean wizard = strcmp(uri, XD_ADD_NETWORK_PLACE_URI) == 0;
    g_free(uri);
    return !wizard && opens_as_folder(t, item, is_dir);
}

/* The middle button on a folder opens it in a new tab behind this one,
 * as Windows 11's does; on anything else it does nothing. */
static void on_middle_click([[maybe_unused]] XdFolderView *view, GFile *file, gboolean is_dir,
                            gpointer data) {
    tab *t = data;
    if (opens_in_tab(t, file, is_dir))
        add_tab(t->e, file, FALSE);
}

static void on_address_activate(GtkEntry *entry, gpointer data) {
    explorer *e = data;
    tab *t = e->cur;
    /* The folder's name alone (Folder Options) stands for itself. */
    const char *shown = g_object_get_data(G_OBJECT(entry), "xd-shown");
    if (!options.full_path_address && shown != nullptr &&
        strcmp(shown, gtk_entry_get_text(entry)) == 0 && current_dir(e) != nullptr) {
        xd_folder_view_reload(t->view);
        gtk_widget_grab_focus(GTK_WIDGET(t->view));
        return;
    }
    GFile *place = xd_place_parse(gtk_entry_get_text(entry));
    if (place != nullptr) {
        navigate(t, place, TRUE);
        g_object_unref(place);
        gtk_widget_grab_focus(GTK_WIDGET(t->view));
        return;
    }
    /* \\server\share, as Windows writes a share. */
    const char *typed = gtk_entry_get_text(entry);
    if (typed[0] == '\\' && typed[1] == '\\') {
        GError *err = nullptr;
        char *uri = xd_net_parse_folder(typed, FALSE, &err);
        if (uri == nullptr) {
            xd_message_box(GTK_WINDOW(e->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE,
                           err->message);
            g_error_free(err);
            return;
        }
        GFile *share = g_file_new_for_uri(uri);
        navigate(t, share, TRUE);
        g_object_unref(share);
        g_free(uri);
        gtk_widget_grab_focus(GTK_WIDGET(t->view));
        return;
    }
    GFile *f = g_file_parse_name(gtk_entry_get_text(entry));
    /* A network folder not connected yet: connected, then opened (asking
     * it what it is would only say it isn't mounted). */
    if (!g_file_is_native(f) && xd_place_of(f) == XD_PLACE_FOLDER) {
        GError *err = nullptr;
        GMount *m = g_file_find_enclosing_mount(f, nullptr, &err);
        gboolean connect = m == nullptr && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
        g_clear_error(&err);
        g_clear_object(&m);
        if (connect) {
            navigate(t, f, TRUE);
            g_object_unref(f);
            gtk_widget_grab_focus(GTK_WIDGET(t->view));
            return;
        }
    }
    GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, nullptr);
    if (type == G_FILE_TYPE_DIRECTORY) {
        navigate(t, f, TRUE);
        gtk_widget_grab_focus(GTK_WIDGET(t->view));
    } else if (type != G_FILE_TYPE_UNKNOWN) {
        activate_item(t, f, FALSE);
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
    tab *t = data;
    if (t == t->e->cur)
        gtk_label_set_text(GTK_LABEL(t->e->status), message);
}

static void begin_rename(tab *t, GFile *file);

static void on_contents([[maybe_unused]] XdFolderView *view, gpointer data) {
    tab *t = data;
    if (t == t->e->cur)
        update_status(t->e);
    /* A new item to name, now that it's loaded. */
    if (t->pending_rename != nullptr) {
        GFile *f = g_object_ref(t->pending_rename);
        GdkRectangle a;
        if (xd_folder_view_get_name_area(t->view, f, &a))
            begin_rename(t, f);
        g_object_unref(f);
    }
}

static void on_selection([[maybe_unused]] XdFolderView *view, gpointer data) {
    tab *t = data;
    if (t == t->e->cur)
        update_status(t->e);
}

/* ---- view menu and toolbar ------------------------------------------------------ */

static const char *const view_names[XD_VIEW_COUNT] = {"Thumbnails", "Tiles", "Icons", "List",
                                                      "Details"};

/* The View menu's radio items and the thumbnail slider show the active
 * tab's view. */
static void sync_view_menu(explorer *e) {
    XdViewMode mode = xd_folder_view_get_mode(e->cur->view);
    e->syncing = TRUE;
    for (int i = 0; i < XD_VIEW_COUNT; i++)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(e->view_items[i]), i == (int)mode);
    int px = xd_folder_view_get_thumbnail_size(e->cur->view);
    gtk_range_set_value(GTK_RANGE(e->slider), (double)px);
    e->syncing = FALSE;
}

static void set_mode(tab *t, XdViewMode mode) {
    xd_folder_view_set_mode(t->view, mode);
    if (t != t->e->cur)
        return;
    sync_view_menu(t->e);
    update_chrome(t->e);
}

static void on_view_item(GtkCheckMenuItem *item, gpointer data) {
    explorer *e = data;
    if (e->syncing || !gtk_check_menu_item_get_active(item))
        return;
    int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-mode"));
    set_mode(e->cur, (XdViewMode)mode);
}

static void on_slider(GtkRange *range, gpointer data) {
    explorer *e = data;
    if (e->syncing)
        return;
    double px = gtk_range_get_value(range);
    xd_folder_view_set_thumbnail_size(e->cur->view, (int)px);
}

/* The left pane: the Folders bar, or XP's task pane in its place
 * (Folder Options' "Show common tasks in folders"), or with "Use Windows
 * classic folders" nothing, the view taking the whole width. */
static void show_side(explorer *e) {
    gboolean folders =
        gtk_toggle_tool_button_get_active(GTK_TOGGLE_TOOL_BUTTON(e->folders_toggle));
    gtk_widget_set_visible(e->tree_pane, folders);
    gtk_widget_set_visible(e->tasks, !folders && options.common_tasks);
    gtk_widget_set_visible(e->side, folders || options.common_tasks);
    e->syncing = TRUE;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(e->folders_item), folders);
    e->syncing = FALSE;
}

static void on_folders_toggle([[maybe_unused]] GtkToggleToolButton *b, gpointer data) {
    show_side(data);
    update_tasks(data);
}

/* View > Explorer Bar > Folders: the Folders button's twin. */
static void on_folders_item(GtkCheckMenuItem *item, gpointer data) {
    explorer *e = data;
    if (!e->syncing)
        gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(e->folders_toggle),
                                          gtk_check_menu_item_get_active(item));
}

/* Show Hidden Files is Folder Options' setting: every window follows. */
static void on_hidden(GtkCheckMenuItem *item, gpointer data) {
    explorer *e = data;
    gboolean show = gtk_check_menu_item_get_active(item);
    for (guint i = 0; i < tab_count(e); i++)
        xd_folder_view_set_show_hidden(tab_at(e, i)->view, show);
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
    xd_folder_view_set_sort(e->cur->view, (XdSortColumn)col, FALSE);
}

/* Arrange Icons by's check items after the columns ("xd-sort"). */
enum { ARRANGE_GROUPS = -1, ARRANGE_AUTO = -2, ARRANGE_ALIGN = -3 };

static void on_show_groups(GtkWidget *item, gpointer data) {
    explorer *e = data;
    if (e->syncing)
        return;
    xd_folder_view_set_groups(e->cur->view,
                              gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)));
    remember_folder_view(e->cur);
}

static void on_auto_arrange(GtkWidget *item, gpointer data) {
    explorer *e = data;
    if (e->syncing)
        return;
    xd_folder_view_set_auto_arrange(e->cur->view,
                                    gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)));
    remember_folder_view(e->cur);
}

static void on_align_to_grid(GtkWidget *item, gpointer data) {
    explorer *e = data;
    if (e->syncing)
        return;
    xd_folder_view_set_align_to_grid(e->cur->view,
                                     gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)));
    remember_folder_view(e->cur);
}

/* Auto Arrange and Align to Grid are for the icon views, and not while
 * they're shown in groups (always arranged); nor for My Computer or
 * Control Panel. Align to Grid is for icons put anywhere. */
static gboolean can_place_icons(explorer *e) {
    XdFolderView *view = e->cur->view;
    XdViewMode mode = xd_folder_view_get_mode(view);
    XdPlace place = tab_place(e->cur);
    return mode != XD_VIEW_LIST && mode != XD_VIEW_DETAILS && !xd_folder_view_get_groups(view) &&
           (place == XD_PLACE_FOLDER || place == XD_PLACE_TRASH);
}

/* Checks radio `col` of an Arrange Icons by menu, and the check items. */
static void check_radio(GtkWidget *menu, int col, explorer *e) {
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    XdFolderView *view = e->cur->view;
    e->syncing = TRUE;
    gboolean placeable = can_place_icons(e);
    gboolean arranged = xd_folder_view_get_auto_arrange(view);
    for (GList *l = items; l != nullptr; l = l->next) {
        if (!GTK_IS_CHECK_MENU_ITEM(l->data))
            continue; /* the separator */
        GtkCheckMenuItem *check = GTK_CHECK_MENU_ITEM(l->data);
        int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(l->data), "xd-sort"));
        if (which == col) {
            gtk_check_menu_item_set_active(check, TRUE);
        } else if (which == ARRANGE_GROUPS) {
            gtk_check_menu_item_set_active(check, xd_folder_view_get_groups(view));
            /* XP greys it in List view, which can't show groups. */
            gtk_widget_set_sensitive(l->data, xd_folder_view_get_mode(view) != XD_VIEW_LIST);
        } else if (which == ARRANGE_AUTO) {
            gtk_check_menu_item_set_active(check, arranged || !placeable);
            gtk_widget_set_sensitive(l->data, placeable);
        } else if (which == ARRANGE_ALIGN) {
            gtk_check_menu_item_set_active(check, xd_folder_view_get_align_to_grid(view));
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
    XdSortColumn sort = e->cur != nullptr ? xd_folder_view_get_sort(e->cur->view) : XD_SORT_NAME;
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
                                   e->cur != nullptr && xd_folder_view_get_groups(e->cur->view));
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
    if (e->cur != nullptr)
        check_radio(menu, current, e);
    gtk_widget_show_all(menu);
    return menu;
}

/* The radio follows the sort (a Details header click changes it). */
static void on_view_menu_show(GtkWidget *menu, gpointer data) {
    (void)menu;
    explorer *e = data;
    GtkWidget *sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(e->arrange_item));
    XdSortColumn sort = xd_folder_view_get_sort(e->cur->view);
    check_radio(sub, (int)sort, e);
}

static void cb_back([[maybe_unused]] GtkWidget *w, gpointer d) { go_back(((explorer *)d)->cur); }
static void cb_forward([[maybe_unused]] GtkWidget *w, gpointer d) {
    go_forward(((explorer *)d)->cur);
}
static void cb_up([[maybe_unused]] GtkWidget *w, gpointer d) { go_up(((explorer *)d)->cur); }
static void cb_home([[maybe_unused]] GtkWidget *w, gpointer d) { go_home(((explorer *)d)->cur); }
static void cb_refresh([[maybe_unused]] GtkWidget *w, gpointer d) {
    xd_folder_view_reload(((explorer *)d)->cur->view);
}
static void cb_select_all([[maybe_unused]] GtkWidget *w, gpointer d) {
    xd_folder_view_select_all(((explorer *)d)->cur->view);
}
static void cb_invert([[maybe_unused]] GtkWidget *w, gpointer d) {
    xd_folder_view_invert_selection(((explorer *)d)->cur->view);
}
/* Properties of the selection, or of the folder itself when nothing is
 * selected. */
static void show_properties(explorer *e) {
    /* Control Panel's items aren't files to show. */
    XdPlace place = xd_place_of(xd_folder_view_get_location(e->cur->view));
    if (place == XD_PLACE_CONTROL || place == XD_PLACE_NETWORK)
        return;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    if (place == XD_PLACE_COMPUTER) {
        /* A drive's own Properties (counting everything on it would take
         * ages); My Documents is a folder's. One at a time, as XP. */
        if (files != nullptr && xd_properties_show_drive(GTK_WINDOW(e->window), files->data) ==
                                    nullptr) {
            GList *one = g_list_append(nullptr, files->data);
            xd_properties_show(GTK_WINDOW(e->window), one);
            g_list_free(one);
        }
        g_list_free_full(files, g_object_unref);
        return;
    }
    if (files == nullptr) {
        GFile *dir = xd_folder_view_get_location(e->cur->view);
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
    GFile *dir = xd_folder_view_get_location(e->cur->view);
    if (xd_place_of(dir) != XD_PLACE_CONTROL)
        return;
    xd_control_set_classic_view(!xd_control_classic_view());
    if (xd_control_page(dir) != nullptr) {
        GFile *root = xd_place_control();
        navigate(e->cur, root, TRUE);
        g_object_unref(root);
        return;
    }
    apply_folder_view(e->cur, dir);
    xd_folder_view_reload(e->cur->view);
    update_chrome(e);
}

static gboolean on_control_link(GtkLinkButton *b, gpointer d) {
    tab *t = d;
    cb_control_switch(GTK_WIDGET(b), t->e);
    return TRUE; /* not a link to open */
}

static void cb_open_selected([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    for (GList *l = files; l != nullptr; l = l->next) {
        gboolean is_dir = opens_as_folder(e->cur, l->data, is_directory(l->data));
        activate_item(e->cur, l->data, is_dir);
        if (is_dir && !options.new_window)
            break; /* navigating replaces the view */
    }
    g_list_free_full(files, g_object_unref);
}

/* Open in new tab (an item's right-click menu): the selected folders,
 * each in a tab of its own behind this one. */
static void cb_open_in_tabs([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    tab *t = e->cur;
    GList *files = xd_folder_view_get_selected_files(t->view);
    for (GList *l = files; l != nullptr; l = l->next)
        if (opens_in_tab(t, l->data, is_directory(l->data)))
            add_tab(e, l->data, FALSE);
    g_list_free_full(files, g_object_unref);
}

static void cb_close([[maybe_unused]] GtkWidget *w, gpointer d) {
    gtk_widget_destroy(((explorer *)d)->window);
}
static void cb_go([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    on_address_activate(GTK_ENTRY(e->address), e);
}
/* Search (F3): XP's search pane, here the Search window, looking in this
 * folder (My Computer's is every local drive, as XP's "Local Hard
 * Drives"). Places that aren't folders on disk start in its default. */
static void open_search(explorer *e) {
    GFile *dir = current_dir(e);
    char *path = nullptr;
    if (dir != nullptr && xd_place_of(dir) == XD_PLACE_COMPUTER)
        path = g_strdup("/");
    else if (dir != nullptr && xd_place_of(dir) == XD_PLACE_FOLDER)
        path = g_file_get_path(dir);
    char *prog = g_strdup("xd-search");
    char *argv[] = {prog, path, nullptr};
    GError *err = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &err)) {
        xd_message_box(GTK_WINDOW(e->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE,
                       err->message);
        g_error_free(err);
    }
    g_free(prog);
    g_free(path);
}

static void cb_search([[maybe_unused]] GtkWidget *w, gpointer d) { open_search(d); }

/* Tools > Folder Options: xd-folders, told this folder's view so that
 * Apply to All Folders can use it. */
static void cb_folder_options([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    char *view = g_strdup_printf("--view=%s", view_names[xd_folder_view_get_mode(e->cur->view)]);
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

/* ---- network drives and places ------------------------------------------------------ */

static void cb_map_drive([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    xd_net_map_drive(GTK_WINDOW(e->window), nullptr, nullptr, open_in_new_window, e);
}

static void cb_disconnect_drives([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    xd_net_disconnect_dialog(GTK_WINDOW(e->window), nullptr);
}

/* The network place (from Add Network Place) `f` is, or nullptr. Free
 * with g_free. */
static char *network_place_name(GFile *f) {
    char *config = xd_net_config_path();
    GPtrArray *places = xd_net_places_load(config);
    g_free(config);
    char *uri = g_file_get_uri(f);
    char *name = nullptr;
    for (guint i = 0; i < places->len && name == nullptr; i++) {
        XdNetPlace *pl = g_ptr_array_index(places, i);
        if (xd_net_same_uri(pl->uri, uri))
            name = g_strdup(pl->name);
    }
    g_free(uri);
    g_ptr_array_unref(places);
    return name;
}

/* A selected item Disconnect applies to: a network drive or share
 * (mapped, or open now), not a network place or My Network Places' own
 * items. */
static gboolean disconnectable(GFile *f) {
    if (g_file_is_native(f) || g_file_has_uri_scheme(f, "network"))
        return FALSE;
    char *place = network_place_name(f);
    g_free(place);
    return place == nullptr;
}

static void cb_disconnect_selected([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    for (GList *l = files; l != nullptr; l = l->next)
        if (disconnectable(l->data))
            xd_net_disconnect(GTK_WINDOW(e->window), l->data);
    g_list_free_full(files, g_object_unref);
}

/* Delete in My Network Places: the selected network places go (their
 * folders stay where they are). */
static void remove_network_places(explorer *e) {
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    char *first = nullptr;
    for (GList *l = files; l != nullptr; l = l->next) {
        char *name = network_place_name(l->data);
        if (name == nullptr)
            continue;
        g_ptr_array_add(uris, g_file_get_uri(l->data));
        if (first == nullptr)
            first = name;
        else
            g_free(name);
    }
    g_list_free_full(files, g_object_unref);
    if (uris->len > 0) {
        char *q = uris->len == 1
                      ? g_strdup_printf("Are you sure you want to remove '%s' from My Network "
                                        "Places?",
                                        first)
                      : g_strdup_printf("Are you sure you want to remove these %u network "
                                        "places?",
                                        uris->len);
        if (xd_message_box(GTK_WINDOW(e->window), "Confirm Network Place Delete", XD_MB_QUESTION,
                           XD_MB_YES_NO, FALSE, q) == GTK_RESPONSE_YES) {
            char *config = xd_net_config_path();
            for (guint i = 0; i < uris->len; i++) {
                GError *err = nullptr;
                if (!xd_net_place_remove(config, g_ptr_array_index(uris, i), &err)) {
                    xd_message_box(GTK_WINDOW(e->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE,
                                   err->message);
                    g_error_free(err);
                    break;
                }
            }
            g_free(config);
        }
        g_free(q);
    }
    g_free(first);
    g_ptr_array_unref(uris);
}

static void cb_remove_places([[maybe_unused]] GtkWidget *w, gpointer d) {
    remove_network_places(d);
}

static void cb_about([[maybe_unused]] GtkWidget *w, gpointer d) {
    explorer *e = d;
    gtk_show_about_dialog(GTK_WINDOW(e->window), "program-name", "XeonDesktop Files",
                          "comments", "The XeonDesktop file manager.", "license-type",
                          GTK_LICENSE_MIT_X11, "logo-icon-name", "system-file-manager", nullptr);
}

/* ---- Favorites ------------------------------------------------------------------- */

/* The name a folder's favorite has without a label of its own. */
static char *favorite_default_name(const char *uri) {
    XdFavorite f = {(char *)(uintptr_t)uri, nullptr};
    return xd_favorite_name(&f);
}

static void favorite_error(GtkWindow *parent, GError *err) {
    char *msg = g_strdup_printf("Cannot change your Favorites: %s", err->message);
    xd_message_box(parent, "Favorites", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
    g_free(msg);
    g_error_free(err);
}

/* Favorites > Add to Favorites...: XP's Add Favorite box, asking the name
 * to show. Only folders (they're GTK's bookmarks too, which file choosers
 * open; My Computer or the Recycle Bin they couldn't). */
static void add_favorite(explorer *e) {
    GFile *dir = current_dir(e);
    if (dir == nullptr || xd_place_of(dir) != XD_PLACE_FOLDER)
        return;
    char *uri = g_file_get_uri(dir);
    char *name = favorite_default_name(uri);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Add Favorite", GTK_WINDOW(e->window), MODAL_DIALOG,
        "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name("starred", GTK_ICON_SIZE_DIALOG),
                       FALSE, FALSE, 0);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *text = gtk_label_new("This folder will be added to your Favorites list.");
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_box_pack_start(GTK_BOX(right), text, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Name:");
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), name);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), right, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *typed = gtk_entry_get_text(GTK_ENTRY(entry));
        /* The folder's own name needs no label: it follows a rename. */
        const char *label_text = *typed == '\0' || strcmp(typed, name) == 0 ? nullptr : typed;
        char *path = xd_favorites_path();
        GError *err = nullptr;
        if (!xd_favorites_add(path, uri, label_text, &err))
            favorite_error(GTK_WINDOW(e->window), err);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
    g_free(name);
    g_free(uri);
}

/* Organize Favorites: rename in place, delete, drag to reorder. */
enum { FAV_NAME, FAV_URI, FAV_COUNT };

typedef struct organizer {
    GtkWidget *dialog, *list;
    GtkListStore *store;
    gboolean filling; /* the store being filled, not reordered */
} organizer;

static void organizer_fill(organizer *o) {
    o->filling = TRUE;
    gtk_list_store_clear(o->store);
    char *path = xd_favorites_path();
    GPtrArray *favs = xd_favorites_load(path);
    for (guint i = 0; i < favs->len; i++) {
        XdFavorite *f = g_ptr_array_index(favs, i);
        char *name = xd_favorite_name(f);
        gtk_list_store_insert_with_values(o->store, nullptr, -1, FAV_NAME, name, FAV_URI, f->uri,
                                          -1);
        g_free(name);
    }
    g_ptr_array_unref(favs);
    g_free(path);
    o->filling = FALSE;
}

static char *organizer_selected(organizer *o, GtkTreeIter *iter) {
    GtkTreeModel *model;
    char *uri = nullptr;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(o->list)),
                                        &model, iter))
        gtk_tree_model_get(model, iter, FAV_URI, &uri, -1);
    return uri;
}

static void on_favorite_edited([[maybe_unused]] GtkCellRendererText *cell, char *tree_path,
                               char *text, gpointer data) {
    organizer *o = data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(o->store), &iter, tree_path))
        return;
    char *uri = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(o->store), &iter, FAV_URI, &uri, -1);
    char *plain = favorite_default_name(uri);
    char *path = xd_favorites_path();
    GError *err = nullptr;
    if (*text != '\0' &&
        !xd_favorites_rename(path, uri, strcmp(text, plain) == 0 ? nullptr : text, &err))
        favorite_error(GTK_WINDOW(o->dialog), err);
    g_free(path);
    g_free(plain);
    g_free(uri);
    organizer_fill(o);
}

static void on_favorite_rename([[maybe_unused]] GtkButton *b, gpointer data) {
    organizer *o = data;
    GtkTreeIter iter;
    char *uri = organizer_selected(o, &iter);
    if (uri == nullptr)
        return;
    GtkTreePath *tp = gtk_tree_model_get_path(GTK_TREE_MODEL(o->store), &iter);
    gtk_widget_grab_focus(o->list);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(o->list), tp,
                             gtk_tree_view_get_column(GTK_TREE_VIEW(o->list), 0), TRUE);
    gtk_tree_path_free(tp);
    g_free(uri);
}

static void on_favorite_delete([[maybe_unused]] GtkButton *b, gpointer data) {
    organizer *o = data;
    GtkTreeIter iter;
    char *uri = organizer_selected(o, &iter);
    if (uri == nullptr)
        return;
    char *name = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(o->store), &iter, FAV_NAME, &name, -1);
    char *question = g_strdup_printf("Are you sure you want to remove '%s' from your Favorites?",
                                     name);
    if (xd_message_box(GTK_WINDOW(o->dialog), "Confirm Favorite Delete", XD_MB_QUESTION,
                       XD_MB_YES_NO, FALSE, question) == GTK_RESPONSE_YES) {
        char *path = xd_favorites_path();
        GError *err = nullptr;
        if (!xd_favorites_remove(path, uri, &err))
            favorite_error(GTK_WINDOW(o->dialog), err);
        g_free(path);
        organizer_fill(o);
    }
    g_free(question);
    g_free(name);
    g_free(uri);
}

/* A drag in the list moved a row (its old place is deleted last). */
static void on_favorites_reordered([[maybe_unused]] GtkTreeModel *model,
                                   [[maybe_unused]] GtkTreePath *tp, gpointer data) {
    organizer *o = data;
    if (o->filling)
        return;
    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    GtkTreeIter iter;
    for (gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(o->store), &iter); ok;
         ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(o->store), &iter)) {
        char *uri = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(o->store), &iter, FAV_URI, &uri, -1);
        if (uri != nullptr)
            g_ptr_array_add(uris, uri);
    }
    g_ptr_array_add(uris, nullptr);
    char *path = xd_favorites_path();
    GError *err = nullptr;
    if (!xd_favorites_reorder(path, (const char *const *)uris->pdata, &err))
        favorite_error(GTK_WINDOW(o->dialog), err);
    g_free(path);
    g_ptr_array_unref(uris);
}

static void organize_favorites(explorer *e) {
    organizer o = {0};
    o.dialog = gtk_dialog_new_with_buttons("Organize Favorites", GTK_WINDOW(e->window),
                                           MODAL_DIALOG,
                                           "Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(o.dialog), 520, 320);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *help = gtk_label_new("To rename or delete an item, select the item and click "
                                    "Rename or Delete. To change the order, drag an item.");
    gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(help), 26);
    gtk_label_set_xalign(GTK_LABEL(help), 0);
    gtk_box_pack_start(GTK_BOX(left), help, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(buttons), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(buttons), TRUE);
    GtkWidget *rename = gtk_button_new_with_mnemonic("_Rename");
    GtkWidget *del = gtk_button_new_with_mnemonic("_Delete");
    g_signal_connect(rename, "clicked", G_CALLBACK(on_favorite_rename), &o);
    g_signal_connect(del, "clicked", G_CALLBACK(on_favorite_delete), &o);
    gtk_grid_attach(GTK_GRID(buttons), rename, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(buttons), del, 1, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(left), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), left, FALSE, FALSE, 0);

    o.store = gtk_list_store_new(FAV_COUNT, G_TYPE_STRING, G_TYPE_STRING);
    o.list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(o.store));
    g_object_unref(o.store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(o.list), FALSE);
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(o.list), TRUE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "icon-name", "folder", nullptr);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "editable", TRUE, nullptr);
    g_signal_connect(text, "edited", G_CALLBACK(on_favorite_edited), &o);
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", FAV_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(o.list), col);
    organizer_fill(&o);
    g_signal_connect(o.store, "row-deleted", G_CALLBACK(on_favorites_reordered), &o);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), o.list);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(o.dialog))), box, TRUE,
                       TRUE, 0);
    gtk_widget_show_all(o.dialog);
    gtk_dialog_run(GTK_DIALOG(o.dialog));
    /* The store goes with the list: no more reordering to save. */
    g_signal_handlers_disconnect_by_data(o.store, &o);
    gtk_widget_destroy(o.dialog);
}

static void cb_add_favorite([[maybe_unused]] GtkWidget *w, gpointer d) { add_favorite(d); }
static void cb_organize_favorites([[maybe_unused]] GtkWidget *w, gpointer d) {
    organize_favorites(d);
}

static void on_favorite_item(GtkWidget *item, gpointer data) {
    explorer *e = data;
    const char *uri = g_object_get_data(G_OBJECT(item), "xd-favorite");
    GFile *f = g_file_new_for_uri(uri);
    navigate(e->cur, f, TRUE);
    g_object_unref(f);
}

/* The menu's favorites, read afresh each time it opens (a file chooser
 * may have added some). */
static void fill_favorites(GtkWidget *menu, gpointer data) {
    explorer *e = data;
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = items; l != nullptr; l = l->next)
        if (g_object_get_data(G_OBJECT(l->data), "xd-favorites-entry") != nullptr)
            gtk_widget_destroy(l->data);
    g_list_free(items);
    gtk_widget_set_sensitive(e->add_favorite_item,
                             xd_place_of(current_dir(e)) == XD_PLACE_FOLDER);
    char *path = xd_favorites_path();
    GPtrArray *favs = xd_favorites_load(path);
    g_free(path);
    for (guint i = 0; i < favs->len; i++) {
        XdFavorite *f = g_ptr_array_index(favs, i);
        if (i == 0) {
            GtkWidget *sep = gtk_separator_menu_item_new();
            g_object_set_data(G_OBJECT(sep), "xd-favorites-entry", GINT_TO_POINTER(1));
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
            gtk_widget_show(sep);
        }
        char *name = xd_favorite_name(f);
        GtkWidget *item = gtk_menu_item_new_with_label(name);
        g_free(name);
        g_object_set_data(G_OBJECT(item), "xd-favorites-entry", GINT_TO_POINTER(1));
        g_object_set_data_full(G_OBJECT(item), "xd-favorite", g_strdup(f->uri), g_free);
        g_signal_connect(item, "activate", G_CALLBACK(on_favorite_item), e);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        gtk_widget_show(item);
    }
    g_ptr_array_unref(favs);
}

/* ---- file operations ------------------------------------------------------------- */

static GFile *current_dir(explorer *e) { return xd_folder_view_get_location(e->cur->view); }

static XdPlace current_place(explorer *e) { return xd_place_of(current_dir(e)); }

static void after_trash_change([[maybe_unused]] GList *created, [[maybe_unused]] gpointer data) {
    xd_trash_tidy();
}

static void restore_selected(explorer *e) {
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
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
        xd_folder_view_select_files(e->cur->view, here);
    g_list_free(here);
}

/* Cut files are faded in every tab showing them. */
static void set_cut_files(explorer *e, GList *files) {
    for (guint i = 0; i < tab_count(e); i++)
        xd_folder_view_set_cut_files(tab_at(e, i)->view, files);
}

static void copy_or_cut(explorer *e, gboolean cut) {
    if (current_place(e) == XD_PLACE_COMPUTER || current_place(e) == XD_PLACE_CONTROL ||
        current_place(e) == XD_PLACE_NETWORK)
        return;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    if (files == nullptr)
        return;
    xd_clipboard_set_files(e->window, files, cut);
    set_cut_files(e, cut ? files : nullptr);
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
    set_cut_files(e, nullptr);
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
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
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
    if (place == XD_PLACE_NETWORK)
        remove_network_places(e);
    if (place == XD_PLACE_COMPUTER || place == XD_PLACE_CONTROL || place == XD_PLACE_NETWORK)
        return;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    gboolean in_trash = place == XD_PLACE_TRASH;
    /* Sending to the Recycle Bin can be undone; deleting can't. */
    xd_fileop_start_undoable(GTK_WINDOW(e->window),
                             permanently || in_trash ? XD_FILEOP_DELETE : XD_FILEOP_TRASH, files,
                             nullptr, in_trash ? after_trash_change : nullptr, e);
    g_list_free_full(files, g_object_unref);
}

/* Edit > Copy To Folder... / Move To Folder... */
static void copy_to_folder(explorer *e, gboolean move) {
    if (current_place(e) == XD_PLACE_CONTROL || current_place(e) == XD_PLACE_NETWORK)
        return;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
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

static void place_rename_entry(tab *t) {
    GdkRectangle a;
    if (t->renaming == nullptr || !xd_folder_view_get_name_area(t->view, t->renaming, &a))
        return;
    int x = 0, y = 0;
    gtk_widget_translate_coordinates(GTK_WIDGET(t->view), t->overlay, a.x, a.y, &x, &y);
    gtk_widget_set_margin_start(t->rename_entry, MAX(x, 0));
    gtk_widget_set_margin_top(t->rename_entry, MAX(y, 0));
    gtk_widget_set_size_request(t->rename_entry, MAX(a.width, 60), -1);
}

static void end_rename(tab *t) {
    g_clear_object(&t->renaming);
    gtk_widget_hide(t->rename_entry);
    /* Not a tab behind (committed as another came forward). */
    if (t == t->e->cur)
        gtk_widget_grab_focus(GTK_WIDGET(t->view));
}

static void begin_rename(tab *t, GFile *file) {
    g_clear_object(&t->pending_rename);
    GdkRectangle a;
    if (!xd_folder_view_get_name_area(t->view, file, &a)) {
        /* Not loaded yet (a new folder): when it appears. */
        t->pending_rename = g_object_ref(file);
        return;
    }
    GList *one = g_list_append(nullptr, file);
    xd_folder_view_select_files(t->view, one);
    g_list_free(one);
    g_set_object(&t->renaming, file);
    char *name = nullptr;
    if (tab_place(t) != XD_PLACE_COMPUTER || !xd_drive_label(file, &name))
        name = xd_rename_text(file);
    gtk_entry_set_text(GTK_ENTRY(t->rename_entry), name);
    g_free(name);
    place_rename_entry(t);
    gtk_widget_show(t->rename_entry);
    gtk_widget_grab_focus(t->rename_entry);
    gtk_editable_select_region(GTK_EDITABLE(t->rename_entry), 0, -1);
}

/* Whether Rename applies to the selection: files in a folder, or a
 * drive in My Computer whose label can be changed. */
static gboolean can_rename(explorer *e) {
    XdPlace place = current_place(e);
    if (place == XD_PLACE_FOLDER)
        return xd_folder_view_get_selected_count(e->cur->view) > 0;
    if (place != XD_PLACE_COMPUTER)
        return FALSE;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    gboolean can = files != nullptr && xd_drive_label(files->data, nullptr);
    g_list_free_full(files, g_object_unref);
    return can;
}

static void rename_selected(explorer *e) {
    if (!can_rename(e))
        return;
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    if (files != nullptr)
        begin_rename(e->cur, files->data);
    g_list_free_full(files, g_object_unref);
}

static char *drive_name(GFile *item) {
    GPtrArray *drives = xd_computer_drives();
    char *name = nullptr;
    for (guint i = 0; i < drives->len && name == nullptr; i++) {
        const XdDrive *d = g_ptr_array_index(drives, i);
        if (g_file_equal(d->root, item))
            name = g_strdup(d->name);
    }
    g_ptr_array_unref(drives);
    return name != nullptr ? name : g_file_get_parse_name(item);
}

/* A drive's new label goes to UDisks2, which may have polkit ask; My
 * Computer shows it when UDisks2 has it. An empty label is allowed, as
 * in XP (the drive goes back to its usual name). */
static void commit_drive_rename(tab *t) {
    char *old = nullptr;
    const char *text = gtk_entry_get_text(GTK_ENTRY(t->rename_entry));
    if (xd_drive_label(t->renaming, &old) && strcmp(text, old) != 0) {
        char *name = drive_name(t->renaming);
        xd_drive_set_label(GTK_WINDOW(t->e->window), t->renaming, name, text, nullptr, nullptr);
        g_free(name);
    }
    g_free(old);
    end_rename(t);
}

/* Returns FALSE to keep editing (the name was refused). */
static gboolean commit_rename(tab *t) {
    if (t->renaming == nullptr || t->committing)
        return TRUE;
    if (tab_place(t) == XD_PLACE_COMPUTER) {
        commit_drive_rename(t);
        return TRUE;
    }
    const char *text = gtk_entry_get_text(GTK_ENTRY(t->rename_entry));
    char *old = xd_rename_text(t->renaming);
    gboolean done = TRUE;
    t->committing = TRUE;
    if (*text != '\0' && strcmp(text, old) != 0) {
        if (strchr(text, '/') != nullptr) {
            xd_message_box(GTK_WINDOW(t->e->window), "Rename", XD_MB_ERROR, XD_MB_OK, FALSE,
                           "A file name cannot contain the following character:\n        /");
            done = FALSE;
        } else {
            GError *err = nullptr;
            GFile *renamed = xd_rename(t->renaming, text, &err);
            if (renamed != nullptr) {
                xd_undo_add_rename(renamed, old);
                GList *one = g_list_append(nullptr, renamed);
                xd_folder_view_select_files(t->view, one);
                g_list_free(one);
                g_object_unref(renamed);
            } else {
                char *msg =
                    g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)
                        ? g_strdup_printf("Cannot rename %s: A file with the name you specified "
                                          "already exists. Specify a different file name.",
                                          old)
                        : g_strdup_printf("Cannot rename %s: %s", old, err->message);
                xd_message_box(GTK_WINDOW(t->e->window), "Error Renaming File or Folder",
                               XD_MB_ERROR, XD_MB_OK, FALSE, msg);
                g_free(msg);
                g_error_free(err);
                done = FALSE;
            }
        }
    }
    t->committing = FALSE;
    g_free(old);
    if (done) {
        end_rename(t);
    } else {
        gtk_widget_grab_focus(t->rename_entry);
        gtk_editable_select_region(GTK_EDITABLE(t->rename_entry), 0, -1);
    }
    return done;
}

static void on_rename_activate([[maybe_unused]] GtkEntry *entry, gpointer data) {
    commit_rename(data);
}

static gboolean on_rename_key([[maybe_unused]] GtkWidget *w, GdkEventKey *ev, gpointer data) {
    tab *t = data;
    if (ev->keyval == GDK_KEY_Escape) {
        end_rename(t);
        return TRUE;
    }
    return FALSE;
}

/* After a focus change has settled: commit unless the entry has the focus
 * again (a message box came and went). */
static void commit_after_focus_change(gpointer data) {
    tab *t = data;
    t->commit_idle = 0;
    if (!t->closed && t->renaming != nullptr && !gtk_widget_has_focus(t->rename_entry) &&
        gtk_window_is_active(GTK_WINDOW(t->e->window)))
        commit_rename(t);
}

static gboolean on_rename_focus_out([[maybe_unused]] GtkWidget *w,
                                    [[maybe_unused]] GdkEventFocus *ev, gpointer data) {
    tab *t = data;
    /* Clicking elsewhere in the window keeps the new name, as in Windows;
     * the window losing the focus leaves the name being edited. */
    if (!t->closed && !t->committing && t->renaming != nullptr &&
        gtk_window_is_active(GTK_WINDOW(t->e->window)))
        if (t->commit_idle == 0)
            t->commit_idle = g_idle_add_once(commit_after_focus_change, t);
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
        xd_folder_view_reload(e->cur->view);
        begin_rename(e->cur, f);
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
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    xd_fileop_start(GTK_WINDOW(e->window), XD_FILEOP_LINK, files, current_dir(e), select_created,
                    e);
    g_list_free_full(files, g_object_unref);
}

static void on_files_dropped([[maybe_unused]] XdFolderView *view, char **uris, GFile *folder,
                             GdkDragAction action, gpointer data) {
    explorer *e = ((tab *)data)->e;
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

/* ---- XP's task pane ------------------------------------------------------------------ */

typedef struct task_place {
    explorer *e;
    GFile *where;
} task_place;

static void free_task_place(gpointer data) {
    task_place *tp = data;
    g_object_unref(tp->where);
    g_free(tp);
}

static void task_go([[maybe_unused]] GtkWidget *pane, gpointer data) {
    task_place *tp = data;
    navigate(tp->e->cur, tp->where, TRUE);
}

typedef struct task_program {
    explorer *e;
    char *argv[3];
} task_program;

static void free_task_program(gpointer data) {
    task_program *tp = data;
    g_free(tp->argv[0]);
    g_free(tp->argv[1]);
    g_free(tp);
}

static void task_run([[maybe_unused]] GtkWidget *pane, gpointer data) {
    task_program *tp = data;
    GError *err = nullptr;
    if (!g_spawn_async(nullptr, tp->argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &err)) {
        xd_message_box(GTK_WINDOW(tp->e->window), "Files", XD_MB_ERROR, XD_MB_OK, FALSE,
                       err->message);
        g_error_free(err);
    }
}

static void add_program_task(explorer *e, int g, const char *icon, const char *label,
                             const char *program, const char *arg) {
    task_program *tp = g_new0(task_program, 1);
    tp->e = e;
    tp->argv[0] = g_strdup(program);
    tp->argv[1] = g_strdup(arg);
    xd_task_pane_task(e->tasks, g, icon, label, task_run, tp, free_task_program);
}

/* A place in Other Places (takes `where`). */
static void add_place_task(explorer *e, int g, const char *icon, const char *label,
                           GFile *where) {
    task_place *tp = g_new0(task_place, 1);
    tp->e = e;
    tp->where = where;
    xd_task_pane_task(e->tasks, g, icon, label, task_go, tp, free_task_place);
}

static void add_task(explorer *e, int g, const char *icon, const char *label, XdTaskFunc func) {
    xd_task_pane_task(e->tasks, g, icon, label, func, e, nullptr);
}

/* Other Places: the folder above (a drive's is My Computer), then the
 * usual places, less the one shown. */
static void add_other_places(explorer *e, GFile *here, XdPlace place) {
    int g = xd_task_pane_group(e->tasks, "Other Places", FALSE, FALSE);
    if (place == XD_PLACE_FOLDER && !xd_place_is_drive_root(here)) {
        GFile *parent = g_file_get_parent(here);
        if (parent != nullptr) {
            char *name = g_file_get_basename(parent);
            const char *home = g_get_home_dir();
            char *path = g_file_get_path(parent);
            gboolean is_home = path != nullptr && g_strcmp0(path, home) == 0;
            add_place_task(e, g, is_home ? "user-home" : "folder",
                           g_strcmp0(name, "/") == 0 ? "File System" : name, parent);
            g_free(path);
            g_free(name);
        }
    }
    const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    GFile *docs_file = docs != nullptr ? g_file_new_for_path(docs) : nullptr;
    if (docs_file != nullptr && !g_file_equal(docs_file, here))
        add_place_task(e, g, "folder-documents", "My Documents", g_object_ref(docs_file));
    g_clear_object(&docs_file);
    if (place != XD_PLACE_COMPUTER)
        add_place_task(e, g, "computer", "My Computer", xd_place_computer());
    if (place == XD_PLACE_COMPUTER || place == XD_PLACE_NETWORK)
        add_place_task(e, g, "preferences-system", "Control Panel", xd_place_control());
    if (place != XD_PLACE_NETWORK)
        add_place_task(e, g, "network-workgroup", "My Network Places",
                       g_file_new_for_uri(XD_NETWORK_URI));
}

/* Details: the selected item's (or the folder's) name, type, date and
 * size, as XP lists them. */
static void add_details(explorer *e, GFile *here, guint selected) {
    int g = xd_task_pane_group(e->tasks, "Details", FALSE, TRUE);
    if (selected > 1) {
        char *text = g_strdup_printf("%u items selected.", selected);
        xd_task_pane_line(e->tasks, g, text, FALSE);
        g_free(text);
        return;
    }
    GFile *file = nullptr;
    if (selected == 1) {
        GList *sel = xd_folder_view_get_selected_files(e->cur->view);
        if (sel != nullptr)
            file = g_object_ref(sel->data);
        g_list_free_full(sel, g_object_unref);
    }
    if (file == nullptr) {
        const char *title = xd_place_title(here);
        if (title != nullptr) {
            xd_task_pane_line(e->tasks, g, title, TRUE);
            xd_task_pane_line(e->tasks, g, "System Folder", FALSE);
            return;
        }
        file = g_object_ref(here);
    }
    /* Only local files are asked about here: a network one could block. */
    GFileInfo *info =
        g_file_is_native(file)
            ? g_file_query_info(file,
                                G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
                                G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
                                G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                G_FILE_QUERY_INFO_NONE, nullptr, nullptr)
            : nullptr;
    if (info == nullptr) {
        char *name = g_file_get_parse_name(file);
        xd_task_pane_line(e->tasks, g, name, TRUE);
        g_free(name);
        g_object_unref(file);
        return;
    }
    xd_task_pane_line(e->tasks, g, g_file_info_get_display_name(info), TRUE);
    gboolean dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    const char *type = g_file_info_get_content_type(info);
    char *desc = dir ? g_strdup("File Folder")
                     : type != nullptr ? g_content_type_get_description(type) : g_strdup("File");
    xd_task_pane_line(e->tasks, g, desc, FALSE);
    g_free(desc);
    GDateTime *when = g_file_info_get_modification_date_time(info);
    if (when != nullptr) {
        GDateTime *local = g_date_time_to_local(when);
        char *date = g_date_time_format(local, "Date Modified: %A, %B %-d, %Y, %-I:%M %p");
        xd_task_pane_line(e->tasks, g, date, FALSE);
        g_free(date);
        g_date_time_unref(local);
        g_date_time_unref(when);
    }
    if (!dir) {
        char *size = xd_format_size((guint64)g_file_info_get_size(info));
        char *line = g_strdup_printf("Size: %s", size);
        xd_task_pane_line(e->tasks, g, line, FALSE);
        g_free(line);
        g_free(size);
    }
    g_object_unref(info);
    g_object_unref(file);
}

/* The pane's groups for the folder shown and its selection: XP's, less
 * the tasks we have nothing for (publishing to the Web, e-mailing). */
static void update_tasks(explorer *e) {
    if (e->tasks == nullptr || !gtk_widget_get_visible(e->tasks))
        return;
    GFile *here = xd_folder_view_get_location(e->cur->view);
    if (here == nullptr)
        return;
    XdPlace place = xd_place_of(here);
    guint selected = xd_folder_view_get_selected_count(e->cur->view);
    xd_task_pane_begin(e->tasks);
    if (place == XD_PLACE_COMPUTER) {
        int g = xd_task_pane_group(e->tasks, "System Tasks", TRUE, FALSE);
        add_program_task(e, g, "dialog-information", "View system information", "xd-sysdm",
                         nullptr);
        add_program_task(e, g, "system-software-install", "Add or remove programs",
                         "xd-appwiz", nullptr);
        add_place_task(e, g, "preferences-system", "Change a setting", xd_place_control());
    } else if (place == XD_PLACE_CONTROL) {
        int g = xd_task_pane_group(e->tasks, "Control Panel", TRUE, FALSE);
        add_task(e, g, "preferences-system", control_switch_label(), cb_control_switch);
    } else if (place == XD_PLACE_TRASH) {
        int g = xd_task_pane_group(e->tasks, "Recycle Bin Tasks", FALSE, FALSE);
        add_task(e, g, "user-trash", "Empty the Recycle Bin", cb_empty_trash);
        if (selected > 0)
            add_task(e, g, "edit-undo",
                     selected == 1 ? "Restore this item" : "Restore the selected items",
                     cb_restore);
    } else if (place == XD_PLACE_NETWORK) {
        int g = xd_task_pane_group(e->tasks, "Network Tasks", FALSE, FALSE);
        add_place_task(e, g, "folder-remote", "Add a network place",
                       g_file_new_for_uri(XD_ADD_NETWORK_PLACE_URI));
        add_program_task(e, g, "network-wired", "View network connections", "xd-netconn",
                         nullptr);
        add_task(e, g, "folder-remote", "Map a network drive", cb_map_drive);
    } else {
        int g = xd_task_pane_group(e->tasks, "File and Folder Tasks", FALSE, FALSE);
        if (selected == 0) {
            add_task(e, g, "folder-new", "Make a new folder", cb_new_folder);
        } else {
            gboolean folder = FALSE;
            if (selected == 1) {
                GList *sel = xd_folder_view_get_selected_files(e->cur->view);
                folder = sel != nullptr &&
                         g_file_query_file_type(sel->data, G_FILE_QUERY_INFO_NONE, nullptr) ==
                             G_FILE_TYPE_DIRECTORY;
                g_list_free_full(sel, g_object_unref);
            }
            const char *what = selected > 1 ? "the selected items"
                               : folder     ? "this folder"
                                            : "this file";
            char label[64];
            if (selected == 1) {
                g_snprintf(label, sizeof(label), "Rename %s", what);
                add_task(e, g, "edit-rename", label, cb_rename);
            }
            g_snprintf(label, sizeof(label), "Move %s", what);
            add_task(e, g, "edit-cut", label, cb_move_to);
            g_snprintf(label, sizeof(label), "Copy %s", what);
            add_task(e, g, "edit-copy", label, cb_copy_to);
            g_snprintf(label, sizeof(label), "Delete %s", what);
            add_task(e, g, "edit-delete", label, cb_delete);
        }
    }
    add_other_places(e, here, place);
    add_details(e, here, selected);
    xd_task_pane_end(e->tasks);
}

/* The view's keys for files: Delete (Shift: permanently), F2, Ctrl+X/C/V. */
static gboolean file_key(explorer *e, GdkEventKey *ev) {
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (ev->state & GDK_SHIFT_MASK) != 0;
    gboolean selected = xd_folder_view_get_selected_count(e->cur->view) > 0;
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
    gboolean selected = xd_folder_view_get_selected_count(e->cur->view) > 0;
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
    gtk_widget_set_sensitive(e->rename_item, can_rename(e));
    /* "Undo Rename", "Undo Copy"..., or a greyed "Undo". */
    const char *undo = xd_undo_label();
    gtk_menu_item_set_label(GTK_MENU_ITEM(e->undo_item), undo != nullptr ? undo : "_Undo");
    gtk_widget_set_sensitive(e->undo_item, undo != nullptr);
    /* Send To, for what's selected now. */
    GList *files =
        place == XD_PLACE_FOLDER ? xd_folder_view_get_selected_files(e->cur->view) : nullptr;
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

static void cb_new_tab(GtkWidget *w, gpointer d);
static void cb_close_tab(GtkWidget *w, gpointer d);

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
    e->rename_item = menu_item(file, "Rena_me", G_CALLBACK(cb_rename), e, nullptr, 0, NO_MODS);
    show_accel(e->rename_item, GDK_KEY_F2, NO_MODS);
    menu_item(file, "P_roperties", G_CALLBACK(cb_properties), e, accel, GDK_KEY_Return,
              GDK_MOD1_MASK);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    /* Windows 11's tabs: Ctrl+W closes the tab (the window with the
     * last, as Ctrl+W did before there were tabs). */
    menu_item(file, "New _Tab", G_CALLBACK(cb_new_tab), e, accel, GDK_KEY_t, GDK_CONTROL_MASK);
    menu_item(file, "Close T_ab", G_CALLBACK(cb_close_tab), e, accel, GDK_KEY_w,
              GDK_CONTROL_MASK);
    menu_item(file, "_Close", G_CALLBACK(cb_close), e, nullptr, 0, NO_MODS);
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
    /* At the top, as XP's. Its other Explorer bars (Search, Favorites,
     * Media, History) are elsewhere here: Search is its own window,
     * Favorites a menu. */
    GtkWidget *bars = gtk_menu_new();
    GtkWidget *bars_item = gtk_menu_item_new_with_mnemonic("_Explorer Bar");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(bars_item), bars);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), bars_item);
    e->folders_item = gtk_check_menu_item_new_with_mnemonic("F_olders");
    g_signal_connect(e->folders_item, "toggled", G_CALLBACK(on_folders_item), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(bars), e->folders_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), gtk_separator_menu_item_new());
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

    GtkWidget *favorites = submenu(bar, "F_avorites");
    e->add_favorite_item = menu_item(favorites, "_Add to Favorites...",
                                     G_CALLBACK(cb_add_favorite), e, nullptr, 0, NO_MODS);
    menu_item(favorites, "_Organize Favorites...", G_CALLBACK(cb_organize_favorites), e, nullptr,
              0, NO_MODS);
    g_signal_connect(favorites, "show", G_CALLBACK(fill_favorites), e);

    GtkWidget *tools = submenu(bar, "_Tools");
    menu_item(tools, "Map _Network Drive...", G_CALLBACK(cb_map_drive), e, nullptr, 0, NO_MODS);
    menu_item(tools, "_Disconnect Network Drive...", G_CALLBACK(cb_disconnect_drives), e, nullptr,
              0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(tools), gtk_separator_menu_item_new());
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

/* The selected archives (extract.h), or nullptr. */
static GList *selected_archives(explorer *e) {
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    GList *archives = nullptr;
    for (GList *l = files; l != nullptr; l = l->next)
        if (xd_is_archive(l->data))
            archives = g_list_prepend(archives, g_object_ref(l->data));
    g_list_free_full(files, g_object_unref);
    return g_list_reverse(archives);
}

/* Extract Here, or each to a folder of its name, in the folder shown. */
static void extract_selected(explorer *e, XdFileOpKind kind) {
    GList *archives = selected_archives(e);
    if (archives != nullptr)
        xd_fileop_start_undoable(GTK_WINDOW(e->window), kind, archives, current_dir(e),
                                 select_created, e);
    g_list_free_full(archives, g_object_unref);
}
static void cb_extract_here([[maybe_unused]] GtkWidget *w, gpointer d) {
    extract_selected(d, XD_FILEOP_EXTRACT_HERE);
}
static void cb_extract_folder([[maybe_unused]] GtkWidget *w, gpointer d) {
    extract_selected(d, XD_FILEOP_EXTRACT_FOLDER);
}

/* "Extract Here" and "Extract to "name"" when archives are selected
 * (libarchive's formats; "Extract Each to Its Own Folder" for several),
 * as archivers add them to Explorer's menu. */
static void add_extract_items(explorer *e, GtkWidget *menu) {
    if (!xd_extract_available())
        return;
    GList *archives = selected_archives(e);
    if (archives == nullptr)
        return;
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Extract _Here", G_CALLBACK(cb_extract_here), e, nullptr, 0, NO_MODS);
    char *label;
    if (archives->next == nullptr) {
        char *base = g_file_get_basename(archives->data);
        char *name = xd_archive_base_name(base);
        char *shown = g_filename_display_name(name);
        /* The name's own underscores aren't mnemonics. */
        char **parts = g_strsplit(shown, "_", -1);
        char *escaped = g_strjoinv("__", parts);
        label = g_strdup_printf("E_xtract to \"%s\"", escaped);
        g_free(escaped);
        g_strfreev(parts);
        g_free(shown);
        g_free(name);
        g_free(base);
    } else {
        label = g_strdup("E_xtract Each to Its Own Folder");
    }
    menu_item(menu, label, G_CALLBACK(cb_extract_folder), e, nullptr, 0, NO_MODS);
    g_free(label);
    g_list_free_full(archives, g_object_unref);
}

/* Windows 11's "Open in new tab", under Open, when a folder is
 * selected. */
static void add_open_in_tab(explorer *e, GtkWidget *menu) {
    GList *files = xd_folder_view_get_selected_files(e->cur->view);
    gboolean folder = FALSE;
    for (GList *l = files; l != nullptr && !folder; l = l->next)
        folder = opens_in_tab(e->cur, l->data, is_directory(l->data));
    g_list_free_full(files, g_object_unref);
    if (folder)
        menu_item(menu, "Open in ne_w tab", G_CALLBACK(cb_open_in_tabs), e, nullptr, 0, NO_MODS);
}

/* Right-click menus. In folders: Open, Cut, Copy, Paste into a folder,
 * Create Shortcut, Delete, Rename; on the background View, Refresh,
 * Paste and New. My Computer's drives only open; the Recycle Bin's items
 * Restore, Cut and Delete, and its background empties it. Control
 * Panel's items open; its background switches between its two views. */
static void on_context_menu(XdFolderView *view, GdkEvent *event, gpointer data) {
    explorer *e = ((tab *)data)->e;
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
    } else if (selected && place == XD_PLACE_NETWORK) {
        bold_item(e, menu, "_Open", G_CALLBACK(cb_open_selected));
        add_open_in_tab(e, menu);
        GList *files = xd_folder_view_get_selected_files(view);
        gboolean places = FALSE, drives = FALSE;
        for (GList *l = files; l != nullptr; l = l->next) {
            char *name = network_place_name(l->data);
            places = places || name != nullptr;
            drives = drives || (name == nullptr && disconnectable(l->data));
            g_free(name);
        }
        g_list_free_full(files, g_object_unref);
        if (drives || places)
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        if (drives)
            menu_item(menu, "Dis_connect", G_CALLBACK(cb_disconnect_selected), e, nullptr, 0,
                      NO_MODS);
        if (places)
            menu_item(menu, "_Delete", G_CALLBACK(cb_remove_places), e, nullptr, 0, NO_MODS);
    } else if (selected && place == XD_PLACE_COMPUTER) {
        bold_item(e, menu, "_Open", G_CALLBACK(cb_open_selected));
        add_open_in_tab(e, menu);
        GList *files = xd_folder_view_get_selected_files(view);
        /* Network drives disconnect, as XP's do; disks eject; an unlocked
         * encrypted drive locks again (unmounted first). */
        gboolean eject = FALSE, lock = FALSE, network = FALSE;
        for (GList *l = files; l != nullptr; l = l->next) {
            gboolean remote = !g_file_is_native(l->data);
            network = network || remote;
            eject = eject || (!remote && xd_computer_can_eject(l->data));
            lock = lock || (!remote && xd_encrypted_can_lock(l->data));
        }
        g_list_free_full(files, g_object_unref);
        if (eject || lock || network)
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        if (lock)
            menu_item(menu, "Loc_k", G_CALLBACK(cb_lock), e, nullptr, 0, NO_MODS);
        if (network)
            menu_item(menu, "Dis_connect", G_CALLBACK(cb_disconnect_selected), e, nullptr, 0,
                      NO_MODS);
        if (eject)
            menu_item(menu, "E_ject", G_CALLBACK(cb_eject), e, nullptr, 0, NO_MODS);
        /* XP's drives rename (their label). */
        if (can_rename(e)) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            menu_item(menu, "Rena_me", G_CALLBACK(cb_rename), e, nullptr, 0, NO_MODS);
        }
    } else if (selected) {
        bold_item(e, menu, "_Open", G_CALLBACK(cb_open_selected));
        add_open_in_tab(e, menu);
        add_extract_items(e, menu);
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
        if (place == XD_PLACE_COMPUTER || place == XD_PLACE_NETWORK) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            menu_item(menu, "Map _Network Drive...", G_CALLBACK(cb_map_drive), e, nullptr, 0,
                      NO_MODS);
            menu_item(menu, "Dis_connect Network Drive...", G_CALLBACK(cb_disconnect_drives), e,
                      nullptr, 0, NO_MODS);
        }
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
    if (place == XD_PLACE_FOLDER ||
        (selected && place != XD_PLACE_CONTROL && place != XD_PLACE_NETWORK)) {
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

/* ---- tabs ------------------------------------------------------------------------- */

/* XP's window, one folder, has no strip; it appears with a second tab
 * and goes again at one, as Windows 11's doesn't (its strip is the title
 * bar). */
static void update_strip(explorer *e) {
    gboolean several = tab_count(e) >= 2;
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(e->notebook), several);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(e->notebook), several);
}

/* The window shows the active tab: title, address bar, Back and Forward,
 * the View menu and slider, the status bar and the Folders tree. */
static void show_active(explorer *e) {
    sync_view_menu(e);
    update_chrome(e);
    update_status(e);
    if (xd_folder_view_get_location(e->cur->view) != nullptr)
        sync_tree(e);
}

static void on_switch_page([[maybe_unused]] GtkNotebook *nb, GtkWidget *page,
                           [[maybe_unused]] guint num, gpointer data) {
    explorer *e = data;
    tab *t = g_object_get_data(G_OBJECT(page), "xd-tab");
    if (t == nullptr || t->closed)
        return;
    e->cur = t;
    show_active(e);
    gtk_widget_grab_focus(GTK_WIDGET(t->view));
}

/* Ctrl+Tab and Ctrl+Shift+Tab: the next or previous tab, round. */
static void cycle_tabs(explorer *e, int step) {
    int n = (int)tab_count(e);
    int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(e->notebook));
    if (n > 1)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(e->notebook), ((i + step) % n + n) % n);
}

static void forget_tab(tab *t) {
    free_history(&t->back);
    free_history(&t->forward);
    g_clear_object(&t->renaming);
    g_clear_object(&t->pending_rename);
    g_clear_handle_id(&t->commit_idle, g_source_remove);
    if (t->naming != nullptr)
        g_cancellable_cancel(t->naming); /* its callback leaves `t` alone */
    g_clear_object(&t->naming);
    g_clear_object(&t->named);
    g_clear_pointer(&t->gio_name, g_free);
}

/* Closes a tab (its folder's view remembered, as on leaving it); the last
 * one closes the window, as Windows 11's Ctrl+W does. */
static void close_tab(tab *t) {
    explorer *e = t->e;
    if (t->closed)
        return;
    if (tab_count(e) <= 1) {
        gtk_widget_destroy(e->window);
        return;
    }
    remember_folder_view(t);
    GtkNotebook *nb = GTK_NOTEBOOK(e->notebook);
    int i = gtk_notebook_page_num(nb, t->page);
    /* The tab to its right comes forward, or the one to its left. */
    if (t == e->cur)
        gtk_notebook_set_current_page(nb, i + 1 < (int)tab_count(e) ? i + 1 : i - 1);
    t->closed = TRUE;
    gtk_notebook_remove_page(nb, i);
    forget_tab(t);
    t->page = t->label = t->label_icon = t->label_text = t->close_button = nullptr;
    t->control_bar = t->control_link = t->overlay = t->rename_entry = nullptr;
    t->view = nullptr;
    g_ptr_array_add(e->closed, t);
    update_strip(e);
}

static void cb_new_tab([[maybe_unused]] GtkWidget *w, gpointer d) {
    /* My Computer, as Windows 11's new tab opens its start page (Home,
     * or This PC). */
    GFile *computer = xd_place_computer();
    add_tab(d, computer, TRUE);
    g_object_unref(computer);
}

static void cb_close_tab([[maybe_unused]] GtkWidget *w, gpointer d) {
    close_tab(((explorer *)d)->cur);
}

/* A tab's right-click menu, Windows 11's. */
static void cb_tab_duplicate([[maybe_unused]] GtkWidget *w, gpointer d) {
    tab *t = d;
    /* Not while its drive is still being mounted. */
    if (!t->closed && xd_folder_view_get_location(t->view) != nullptr)
        add_tab(t->e, xd_folder_view_get_location(t->view), TRUE);
}

static void cb_tab_close([[maybe_unused]] GtkWidget *w, gpointer d) { close_tab(d); }

/* Close other tabs / Close tabs to the right. */
static void close_tabs_but(tab *keep, gboolean right_only) {
    if (keep->closed)
        return;
    explorer *e = keep->e;
    int from = right_only ? gtk_notebook_page_num(GTK_NOTEBOOK(e->notebook), keep->page) + 1 : 0;
    GPtrArray *doomed = g_ptr_array_new();
    for (guint i = (guint)from; i < tab_count(e); i++)
        if (tab_at(e, i) != keep)
            g_ptr_array_add(doomed, tab_at(e, i));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(e->notebook),
                                  gtk_notebook_page_num(GTK_NOTEBOOK(e->notebook), keep->page));
    for (guint i = 0; i < doomed->len; i++)
        close_tab(g_ptr_array_index(doomed, i));
    g_ptr_array_unref(doomed);
}

static void cb_tab_close_others([[maybe_unused]] GtkWidget *w, gpointer d) {
    close_tabs_but(d, FALSE);
}

static void cb_tab_close_right([[maybe_unused]] GtkWidget *w, gpointer d) {
    close_tabs_but(d, TRUE);
}

static void tab_menu(tab *t, GdkEventButton *ev) {
    explorer *e = t->e;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item = gtk_menu_item_new_with_mnemonic("_Duplicate tab");
    g_signal_connect(item, "activate", G_CALLBACK(cb_tab_duplicate), t);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_mnemonic("_Close tab");
    g_signal_connect(item, "activate", G_CALLBACK(cb_tab_close), t);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    item = gtk_menu_item_new_with_mnemonic("Close _other tabs");
    g_signal_connect(item, "activate", G_CALLBACK(cb_tab_close_others), t);
    gtk_widget_set_sensitive(item, tab_count(e) > 1);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    item = gtk_menu_item_new_with_mnemonic("Close tabs to the _right");
    g_signal_connect(item, "activate", G_CALLBACK(cb_tab_close_right), t);
    gtk_widget_set_sensitive(item, gtk_notebook_page_num(GTK_NOTEBOOK(e->notebook), t->page) + 1 <
                                       (int)tab_count(e));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_menu_attach_to_widget(GTK_MENU(menu), t->label, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* The tab under a click on the strip, or nullptr. The notebook draws
 * the tabs around their labels (no windows of their own, so the
 * notebook's hot-tracking and dragging see the pointer over them). */
static tab *tab_under(explorer *e, GdkEventButton *ev) {
    GdkWindow *target = gtk_widget_get_window(e->notebook);
    GdkWindow *w = ev->window;
    double x = ev->x, y = ev->y;
    while (w != nullptr && w != target) {
        gdk_window_coords_to_parent(w, x, y, &x, &y);
        w = gdk_window_get_parent(w);
    }
    if (w == nullptr)
        return nullptr;
    /* The tab's own padding around its label counts too. */
    enum { SLACK = 6 };
    for (guint i = 0; i < tab_count(e); i++) {
        tab *t = tab_at(e, i);
        GtkAllocation a;
        gtk_widget_get_allocation(t->label, &a);
        if (gtk_widget_get_mapped(t->label) && x >= a.x - SLACK && x < a.x + a.width + SLACK &&
            y >= a.y - SLACK && y < a.y + a.height + SLACK)
            return t;
    }
    return nullptr;
}

/* On a tab: the middle button closes it, the right one has its menu; the
 * left one is the notebook's (choosing, dragging to reorder). */
static gboolean on_strip_press([[maybe_unused]] GtkWidget *w, GdkEventButton *ev, gpointer data) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button == GDK_BUTTON_PRIMARY)
        return FALSE;
    tab *t = tab_under(data, ev);
    if (t == nullptr)
        return FALSE;
    if (ev->button == GDK_BUTTON_MIDDLE) {
        close_tab(t);
        return TRUE;
    }
    if (ev->button == GDK_BUTTON_SECONDARY) {
        tab_menu(t, ev);
        return TRUE;
    }
    return FALSE;
}

static void on_tab_close_clicked([[maybe_unused]] GtkButton *b, gpointer data) { close_tab(data); }

/* The tab itself: the folder's icon and name, and a close button. */
static void build_tab_label(tab *t) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    t->label_icon = gtk_image_new_from_icon_name("folder", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(box), t->label_icon, FALSE, FALSE, 0);
    t->label_text = gtk_label_new("");
    gtk_label_set_max_width_chars(GTK_LABEL(t->label_text), TAB_NAME_CHARS);
    gtk_label_set_xalign(GTK_LABEL(t->label_text), 0);
    gtk_box_pack_start(GTK_BOX(box), t->label_text, TRUE, TRUE, 0);
    t->close_button = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(t->close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(t->close_button, FALSE);
    gtk_widget_set_can_focus(t->close_button, FALSE);
    gtk_widget_set_tooltip_text(t->close_button, "Close tab");
    gtk_widget_set_valign(t->close_button, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(t->close_button), "xd-tab-close");
    g_signal_connect(t->close_button, "clicked", G_CALLBACK(on_tab_close_clicked), t);
    gtk_box_pack_start(GTK_BOX(box), t->close_button, FALSE, FALSE, 0);
    t->label = box;
    gtk_widget_show_all(t->label);
}

/* The tab's page: Control Panel's bar over the view, with the entry for
 * renaming in place floating over it. */
static void build_tab_page(tab *t) {
    t->view = XD_FOLDER_VIEW(xd_folder_view_new());
    g_signal_connect(t->view, "item-activated", G_CALLBACK(on_activated), t);
    g_signal_connect(t->view, "item-middle-click", G_CALLBACK(on_middle_click), t);
    g_signal_connect(t->view, "context-menu", G_CALLBACK(on_context_menu), t);
    g_signal_connect(t->view, "contents-changed", G_CALLBACK(on_contents), t);
    g_signal_connect(t->view, "selection-changed", G_CALLBACK(on_selection), t);
    g_signal_connect(t->view, "load-error", G_CALLBACK(on_load_error), t);
    g_signal_connect(t->view, "files-dropped", G_CALLBACK(on_files_dropped), t);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(t->view));
    g_signal_connect(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(t->view)), "value-changed",
                     G_CALLBACK(on_view_scrolled), t);
    g_signal_connect(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(t->view)), "value-changed",
                     G_CALLBACK(on_view_scrolled), t);
    /* Renaming in place: an entry floating over the view. */
    t->overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(t->overlay), scroll);
    t->rename_entry = gtk_entry_new();
    gtk_widget_set_halign(t->rename_entry, GTK_ALIGN_START);
    gtk_widget_set_valign(t->rename_entry, GTK_ALIGN_START);
    gtk_entry_set_width_chars(GTK_ENTRY(t->rename_entry), 1);
    gtk_widget_set_no_show_all(t->rename_entry, TRUE);
    g_signal_connect(t->rename_entry, "activate", G_CALLBACK(on_rename_activate), t);
    g_signal_connect(t->rename_entry, "key-press-event", G_CALLBACK(on_rename_key), t);
    g_signal_connect(t->rename_entry, "focus-out-event", G_CALLBACK(on_rename_focus_out), t);
    gtk_overlay_add_overlay(GTK_OVERLAY(t->overlay), t->rename_entry);
    /* Control Panel's link to its other view (XP has it in the task pane). */
    t->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    t->control_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_no_show_all(t->control_bar, TRUE);
    GtkWidget *control_icon = gtk_image_new_from_icon_name("preferences-system",
                                                           GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_margin_start(control_icon, 6);
    gtk_box_pack_start(GTK_BOX(t->control_bar), control_icon, FALSE, FALSE, 0);
    t->control_link = gtk_link_button_new_with_label(XD_CONTROL_URI, "Switch to Classic View");
    gtk_widget_set_tooltip_text(t->control_link, nullptr);
    g_signal_connect(t->control_link, "activate-link", G_CALLBACK(on_control_link), t);
    gtk_box_pack_start(GTK_BOX(t->control_bar), t->control_link, FALSE, FALSE, 0);
    gtk_widget_show(control_icon);
    gtk_widget_show(t->control_link);
    gtk_box_pack_start(GTK_BOX(t->page), t->control_bar, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(t->page), t->overlay, TRUE, TRUE, 0);
    g_object_set_data(G_OBJECT(t->page), "xd-tab", t);
    gtk_widget_show_all(t->page);
}

/* Folder Options a tab's view follows. */
static void apply_tab_options(tab *t) {
    xd_folder_view_set_show_hidden(t->view, options.show_hidden);
    xd_folder_view_set_hide_extensions(t->view, options.hide_extensions);
    xd_folder_view_set_single_click(t->view, options.single_click);
}

/* A new tab at the end of the strip (Windows 11's place for one), showing
 * `dir`; in front, or behind the one shown (a middle click). */
static tab *add_tab(explorer *e, GFile *dir, gboolean activate) {
    tab *t = g_new0(tab, 1);
    t->e = e;
    build_tab_page(t);
    build_tab_label(t);
    apply_tab_options(t);
    xd_folder_view_set_mode(t->view, (XdViewMode)initial_view);
    GtkNotebook *nb = GTK_NOTEBOOK(e->notebook);
    if (e->cur == nullptr)
        e->cur = t; /* the window's first */
    int n = gtk_notebook_append_page(nb, t->page, t->label);
    gtk_notebook_set_tab_reorderable(nb, t->page, TRUE);
    gtk_container_child_set(GTK_CONTAINER(nb), t->page, "tab-expand", FALSE, nullptr);
    navigate(t, dir, FALSE);
    update_strip(e);
    if (activate)
        gtk_notebook_set_current_page(nb, n);
    return t;
}

/* ---- keys and the window's end -------------------------------------------------------- */

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    explorer *e = data;
    GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
    /* Windows 11's tab keys: Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+1..8 for that
     * tab and Ctrl+9 for the last (Ctrl+T and Ctrl+W are File's). */
    if (mods == GDK_CONTROL_MASK && ev->keyval == GDK_KEY_Tab) {
        cycle_tabs(e, 1);
        return TRUE;
    }
    if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
        (ev->keyval == GDK_KEY_ISO_Left_Tab || ev->keyval == GDK_KEY_Tab)) {
        cycle_tabs(e, -1);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && ev->keyval >= GDK_KEY_1 && ev->keyval <= GDK_KEY_9) {
        int want = (int)(ev->keyval - GDK_KEY_1);
        int n = (int)tab_count(e);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(e->notebook),
                                      ev->keyval == GDK_KEY_9 ? n - 1 : MIN(want, n - 1));
        return TRUE;
    }
    /* Backspace goes up, as in Windows, unless typing in the address bar. */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(e->window));
    if (ev->keyval == GDK_KEY_BackSpace && !GTK_IS_EDITABLE(focus)) {
        go_up(e->cur);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_F3 && mods == 0) {
        open_search(e);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_l && (ev->state & GDK_CONTROL_MASK)) {
        gtk_widget_grab_focus(e->address);
        return TRUE;
    }
    if (focus == GTK_WIDGET(e->cur->view) && file_key(e, ev))
        return TRUE;
    (void)w;
    return FALSE;
}

/* The window's tabs go with it, all of them (Windows 11 doesn't ask). */
static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    explorer *e = data;
    /* Nothing of ours runs while the widgets are taken down. */
    g_signal_handlers_disconnect_by_data(e->notebook, e);
    g_signal_handlers_disconnect_by_data(gtk_tree_view_get_selection(GTK_TREE_VIEW(e->tree)), e);
    for (guint i = 0; i < tab_count(e); i++) {
        tab *t = tab_at(e, i);
        remember_folder_view(t);
        GObject *own[] = {G_OBJECT(t->view),
                          G_OBJECT(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(t->view))),
                          G_OBJECT(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(t->view))),
                          G_OBJECT(t->rename_entry),
                          G_OBJECT(t->control_link),
                          G_OBJECT(t->close_button)};
        for (size_t k = 0; k < G_N_ELEMENTS(own); k++)
            g_signal_handlers_disconnect_by_data(own[k], t);
        forget_tab(t);
        g_free(t);
    }
    g_ptr_array_unref(e->closed); /* frees them */
    g_ptr_array_unref(e->need_selection);
    g_ptr_array_unref(e->need_clipboard);
    g_ptr_array_unref(e->folders_only);
    g_ptr_array_unref(e->trash_only);
    g_free(e);
}

/* ---- Folder Options ---------------------------------------------------------------- */

static void apply_options(explorer *e) {
    for (guint i = 0; i < tab_count(e); i++)
        apply_tab_options(tab_at(e, i));
    e->syncing = TRUE;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(e->hidden_item), options.show_hidden);
    e->syncing = FALSE;
    show_side(e); /* Show common tasks in folders, or classic folders */
    update_tasks(e);
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
        if (e != nullptr) {
            apply_options(e);
            xd_task_pane_reload_theme(e->tasks); /* the appearance may have changed */
        }
    }
    return G_SOURCE_CONTINUE;
}

static explorer *open_window(GtkApplication *app, GFile *dir, gboolean explore) {
    explorer *e = g_new0(explorer, 1);
    e->closed = g_ptr_array_new_with_free_func(g_free);
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
    tool_button(toolbar, "system-search", "Search", G_CALLBACK(cb_search), e);
    GtkToolItem *folders = gtk_toggle_tool_button_new();
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(folders), "Folders");
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(folders), "folder");
    gtk_tool_item_set_is_important(folders, TRUE);
    /* On only to explore: XP opens a folder with the task pane on the
     * left, or with classic folders nothing (show_side). */
    gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(folders), explore);
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
    g_signal_connect(e->tree, "button-press-event", G_CALLBACK(on_tree_press), e);
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
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    e->side = side;
    gtk_box_pack_start(GTK_BOX(side), left, TRUE, TRUE, 0);
    e->tasks = xd_task_pane_new();
    gtk_widget_set_size_request(e->tasks, 200, -1);
    gtk_widget_set_no_show_all(e->tasks, TRUE); /* shown when Folders is off */
    gtk_box_pack_start(GTK_BOX(side), e->tasks, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(paned), side, FALSE, FALSE);

    /* The tabs, over the folder they show; the Folders tree beside them
     * is the window's, following the tab in front. */
    e->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(e->notebook), TRUE);
    gtk_widget_set_can_focus(e->notebook, FALSE);
    g_signal_connect_after(e->notebook, "switch-page", G_CALLBACK(on_switch_page), e);
    g_signal_connect(e->notebook, "button-press-event", G_CALLBACK(on_strip_press), e);
    gtk_paned_pack2(GTK_PANED(paned), e->notebook, TRUE, FALSE);
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
    /* Vista's and 7's look for these, where the theme package has it. */
    xd_explorer_chrome_attach(toolbar, e->address, e->tree, sbar);

    gtk_widget_show_all(e->window);
    add_tab(e, dir, TRUE);
    apply_options(e);
    show_active(e);
    gtk_widget_grab_focus(GTK_WIDGET(e->cur->view));
    return e;
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

/* The desktop's Map Network Drive: the new drive opens in a window. */
static void open_mapped(GFile *location, gpointer app) { open_window(app, location, FALSE); }

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
    case JOB_MAP_DRIVE:
        xd_net_map_drive(nullptr, G_APPLICATION(app), nullptr, open_mapped, app);
        break;
    case JOB_DISCONNECT_DRIVE:
        xd_net_disconnect_dialog(nullptr, G_APPLICATION(app));
        break;
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
    /* XP's Windows Explorer (explorer.exe on its own) opens with the
     * Folders bar; a folder opened from somewhere has the task pane. */
    GFile *home = g_file_new_for_path(g_get_home_dir());
    open_window(app, home, TRUE);
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
    /* Folders in tabs instead of windows: one window with a tab each,
     * the first in front. */
    if (options.new_tabs && n > 1) {
        explorer *e = open_window(app, files[0], explore_option);
        for (gint i = 1; i < n; i++)
            add_tab(e, files[i], FALSE);
        return;
    }
    for (gint i = 0; i < n; i++)
        open_window(app, files[i], explore_option);
}

/* --view=thumbnails|tiles|icons|list|details */
static gint on_options([[maybe_unused]] GApplication *app, GVariantDict *opts,
                       [[maybe_unused]] gpointer data) {
    const char *view = nullptr;
    if (g_variant_dict_contains(opts, "properties"))
        properties_only = TRUE;
    if (g_variant_dict_contains(opts, "explore"))
        explore_option = TRUE;
    if (g_variant_dict_contains(opts, "trash"))
        job = JOB_TRASH;
    if (g_variant_dict_contains(opts, "delete"))
        job = JOB_DELETE;
    if (g_variant_dict_contains(opts, "rename"))
        job = JOB_RENAME;
    if (g_variant_dict_contains(opts, "empty-trash"))
        job = JOB_EMPTY_TRASH;
    if (g_variant_dict_contains(opts, "map-drive"))
        job = JOB_MAP_DRIVE;
    if (g_variant_dict_contains(opts, "disconnect-drive"))
        job = JOB_DISCONNECT_DRIVE;
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

/* --reconnect: the shell's logon step. Mounts the mapped drives marked
 * Reconnect at logon, quietly (a server that's away or wants a password
 * not in the keyring is left disconnected), without GTK or a display. */
typedef struct reconnect_wait {
    GMainLoop *loop;
    gboolean finished;
} reconnect_wait;

static void reconnect_done([[maybe_unused]] guint connected, [[maybe_unused]] guint failed,
                           gpointer data) {
    reconnect_wait *w = data;
    w->finished = TRUE;
    g_main_loop_quit(w->loop);
}

static int reconnect_drives(void) {
    reconnect_wait w = {g_main_loop_new(nullptr, FALSE), FALSE};
    char *config = xd_net_config_path();
    /* A minute each at most: the shell doesn't wait for us anyway. */
    xd_net_reconnect_all(config, 60, reconnect_done, &w);
    if (!w.finished)
        g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    g_free(config);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--reconnect") == 0)
            return reconnect_drives();
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
    g_application_add_main_option(G_APPLICATION(app), "explore", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Open FILE... with the Folders bar", nullptr);
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
    g_application_add_main_option(G_APPLICATION(app), "map-drive", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Map Network Drive", nullptr);
    g_application_add_main_option(G_APPLICATION(app), "disconnect-drive", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Disconnect Network Drives", nullptr);
    g_application_add_main_option(G_APPLICATION(app), "reconnect", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE,
                                  "Reconnect the network drives mapped to reconnect at logon",
                                  nullptr);
    g_signal_connect(app, "handle-local-options", G_CALLBACK(on_options), nullptr);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
