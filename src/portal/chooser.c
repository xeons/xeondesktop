/* SPDX-License-Identifier: MIT */
#include "chooser.h"

#include <stdio.h>
#include <string.h>

#include <gdk/gdkwayland.h>
#include <gtk/gtk.h>

#include "../files/fileops.h"
#include "../files/folderview.h"
#include "../files/search.h"
#include "../files/places.h"
#include "../files/rename.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "../settings/state.h"
#include "../shell/recent.h"
#include "chooserlib.h"

/* The folder the dialog lists itself: XP's My Recent Documents. */
#define RECENT_URI "recent:///"

/* XP's dialog at 96 DPI (563 x 419), a little wider for the search box. */
enum { DIALOG_WIDTH = 620, DIALOG_HEIGHT = 419 };
/* Look in's indent per level, in px. */
enum { LOOK_IN_INDENT = 10 };
enum { RECENT_MAX = 100 };

enum { LOOK_ICON, LOOK_NAME, LOOK_FILE, LOOK_INDENT, LOOK_COUNT };

typedef struct chooser_choice {
    char *id;
    GtkWidget *widget; /* a GtkCheckButton, or a GtkComboBoxText with the options' ids */
    bool is_check;
} chooser_choice;

/* A Places bar button and where it goes. */
typedef struct chooser_place {
    GtkWidget *button;
    GFile *location;
} chooser_place;

enum { PLACE_COUNT = 5 };

struct xd_chooser {
    xd_chooser_mode mode;
    bool multiple;
    bool directory; /* choosing a folder (directory=true, or SaveFiles) */
    bool finished;
    char *title;
    char **save_names; /* SaveFiles: the files' names */
    xd_chooser_done done;
    gpointer done_data;

    GtkWidget *window;
    XdFolderView *view;
    GtkWidget *overlay;
    GtkWidget *name_combo, *name_entry;
    GtkWidget *type_combo;
    GtkWidget *accept_button;
    GtkWidget *read_only;
    GPtrArray *choices; /* chooser_choice* */

    GPtrArray *filters; /* xd_chooser_filter* */
    bool filters_given; /* by the program (else just "All Files") */
    int filter_index;
    xd_chooser_filter *pattern; /* a pattern typed in File name, while it's shown */

    GList *back; /* GFile*, the latest first */
    bool updating;         /* setting the combos and buttons, not the user */

    /* Look in, its buttons, the search box and the Places bar. */
    GtkWidget *look_in;
    GtkListStore *look_in_store;
    GtkWidget *back_button, *up_button, *new_folder_button;
    chooser_place places[PLACE_COUNT];
    GtkWidget *search;
    /* 7's search box, matched as xd-files' box matches names (search.h);
     * nullptr: nothing typed. */
    xd_search_query *search_query;

    /* A new folder being named in place. */
    GtkWidget *rename_entry;
    GFile *renaming;
    GFile *pending_rename;
};

/* ---- locations ---------------------------------------------------------------------- */

static GFile *new_special_dir(GUserDirectory which) {
    const char *path = g_get_user_special_dir(which);
    return g_file_new_for_path(path != nullptr ? path : g_get_home_dir());
}

static bool is_recent(GFile *location) { return g_file_has_uri_scheme(location, "recent"); }

static GFile *get_location(const xd_chooser *chooser) {
    return xd_folder_view_get_location(chooser->view);
}

/* The folder shown as a path, or nullptr (My Computer and the like). */
static char *get_folder_path(const xd_chooser *chooser) {
    GFile *location = get_location(chooser);
    if (location == nullptr || xd_place_of(location) != XD_PLACE_FOLDER || is_recent(location))
        return nullptr;
    return g_file_get_path(location);
}

/* What the folder is called in Look in and the search box. Free with
 * g_free. */
static char *get_location_name(GFile *location) {
    if (is_recent(location))
        return g_strdup("My Recent Documents");
    const char *special = xd_place_title(location);
    if (special != nullptr)
        return g_strdup(special);
    char *path = g_file_get_path(location);
    char *name = nullptr;
    if (path != nullptr) {
        if (g_strcmp0(path, g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP)) == 0)
            name = g_strdup("Desktop");
        else if (g_strcmp0(path, g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS)) == 0)
            name = g_strdup("My Documents");
        else if (strcmp(path, "/") == 0)
            name = g_strdup("File System");
    }
    g_free(path);
    if (name == nullptr) {
        GFileInfo *info = g_file_query_info(location, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                            G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
        name = info != nullptr ? g_strdup(g_file_info_get_display_name(info))
                               : g_file_get_basename(location);
        g_clear_object(&info);
    }
    return name;
}

static GIcon *new_location_icon(GFile *location) {
    if (is_recent(location))
        return g_themed_icon_new("document-open-recent");
    const char *special = xd_place_icon(location);
    if (special != nullptr)
        return g_themed_icon_new(special);
    GFileInfo *info = g_file_query_info(location, G_FILE_ATTRIBUTE_STANDARD_ICON,
                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    GIcon *icon = info != nullptr ? g_file_info_get_icon(info) : nullptr;
    icon = icon != nullptr ? g_object_ref(icon) : g_themed_icon_new("folder");
    g_clear_object(&info);
    return icon;
}

/* Up One Level: drives go up to My Computer and My Computer to the
 * Desktop, as in XP. nullptr at the top. */
static GFile *get_up_location(GFile *location) {
    GFile *desktop = new_special_dir(G_USER_DIRECTORY_DESKTOP);
    XdPlace place = xd_place_of(location);
    GFile *up = nullptr;
    if (is_recent(location) || g_file_equal(location, desktop))
        up = nullptr;
    else if (place == XD_PLACE_COMPUTER || place == XD_PLACE_NETWORK)
        up = g_object_ref(desktop);
    else if (place != XD_PLACE_FOLDER)
        up = nullptr;
    else if (xd_place_is_drive_root(location))
        up = xd_place_computer();
    else
        up = g_file_get_parent(location);
    g_object_unref(desktop);
    return up;
}

/* ---- what's shown --------------------------------------------------------------------- */

static const xd_chooser_filter *get_current_filter(const xd_chooser *chooser) {
    if (chooser->pattern != nullptr)
        return chooser->pattern;
    if (chooser->filter_index < 0 || chooser->filter_index >= (int)chooser->filters->len)
        return nullptr;
    return g_ptr_array_index(chooser->filters, (guint)chooser->filter_index);
}

static gboolean keep_item(const char *name, const char *content_type, gboolean is_directory,
                          gpointer data) {
    const xd_chooser *chooser = data;
    if (chooser->search_query != nullptr) {
        xd_search_item item = {.name = name, .is_dir = is_directory,
                               .content_type = content_type};
        bool found = xd_search_query_matches(chooser->search_query, &item);
        xd_search_item_clear(&item);
        if (!found)
            return FALSE;
    }
    if (is_directory)
        return TRUE;
    if (chooser->directory)
        return FALSE;
    return xd_chooser_filter_matches(get_current_filter(chooser), name, content_type);
}

/* A filter of the patterns typed in File name ("*.txt;*.log"). */
static xd_chooser_filter *new_pattern_filter(const char *patterns) {
    GVariantBuilder rules;
    g_variant_builder_init(&rules, G_VARIANT_TYPE("a(us)"));
    char **parts = g_strsplit(patterns, ";", -1);
    for (char **part = parts; *part != nullptr; part++) {
        char *stripped = g_strstrip(*part);
        if (*stripped != '\0')
            g_variant_builder_add(&rules, "(us)", 0u, stripped);
    }
    g_strfreev(parts);
    return xd_chooser_filter_parse(g_variant_new("(sa(us))", patterns, &rules));
}

/* ---- the Look in box and the Places bar ----------------------------------------------- */

static void add_look_in_row(xd_chooser *chooser, GFile *location, const char *name, int level) {
    GIcon *icon = new_location_icon(location);
    char *shown = name != nullptr ? g_strdup(name) : get_location_name(location);
    GtkTreeIter iter;
    gtk_list_store_append(chooser->look_in_store, &iter);
    gtk_list_store_set(chooser->look_in_store, &iter, LOOK_ICON, icon, LOOK_NAME, shown,
                       LOOK_FILE, location, LOOK_INDENT, level * LOOK_IN_INDENT, -1);
    g_free(shown);
    g_object_unref(icon);
}

/* The folders from `anchor` (excluded) down to `location`, at the levels
 * below `level`, if `location` is inside it. True if it was. */
static bool add_look_in_chain(xd_chooser *chooser, GFile *anchor, GFile *location, int level) {
    if (!g_file_has_prefix(location, anchor))
        return false;
    GPtrArray *chain = g_ptr_array_new_with_free_func(g_object_unref);
    GFile *step = g_object_ref(location);
    while (step != nullptr && !g_file_equal(step, anchor)) {
        g_ptr_array_insert(chain, 0, step);
        step = g_file_get_parent(step);
    }
    g_clear_object(&step);
    for (guint i = 0; i < chain->len; i++)
        add_look_in_row(chooser, g_ptr_array_index(chain, i), nullptr, level + 1 + (int)i);
    g_ptr_array_unref(chain);
    return true;
}

/* Which anchor `location` is listed under: the deepest that holds it. */
static bool is_deepest_anchor(GFile *anchor, GFile *location, GFile *const *anchors,
                              size_t count) {
    if (!g_file_equal(anchor, location) && !g_file_has_prefix(location, anchor))
        return false;
    for (size_t i = 0; i < count; i++) {
        if (anchors[i] != anchor && g_file_has_prefix(anchors[i], anchor) &&
            (g_file_equal(anchors[i], location) || g_file_has_prefix(location, anchors[i])))
            return false;
    }
    return true;
}

/* XP's Look in: the Desktop, My Documents, My Computer and its drives,
 * My Network Places, with the folder shown and those it's in. */
static void fill_look_in(xd_chooser *chooser) {
    GFile *location = get_location(chooser);
    gtk_list_store_clear(chooser->look_in_store);
    GFile *desktop = new_special_dir(G_USER_DIRECTORY_DESKTOP);
    GFile *documents = new_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    GPtrArray *drives = xd_computer_drives();
    GPtrArray *anchors = g_ptr_array_new();
    g_ptr_array_add(anchors, desktop);
    g_ptr_array_add(anchors, documents);
    for (guint i = 0; i < drives->len; i++) {
        XdDrive *drive = g_ptr_array_index(drives, i);
        if (g_file_is_native(drive->root) && strcmp(drive->kind, "File Folder") != 0)
            g_ptr_array_add(anchors, drive->root);
    }
    GFile *const *all = (GFile *const *)anchors->pdata;
    size_t count = anchors->len;

    add_look_in_row(chooser, desktop, "Desktop", 0);
    if (is_deepest_anchor(desktop, location, all, count))
        add_look_in_chain(chooser, desktop, location, 0);
    add_look_in_row(chooser, documents, "My Documents", 1);
    if (is_deepest_anchor(documents, location, all, count))
        add_look_in_chain(chooser, documents, location, 1);
    GFile *computer = xd_place_computer();
    add_look_in_row(chooser, computer, "My Computer", 1);
    g_object_unref(computer);
    for (guint i = 0; i < drives->len; i++) {
        XdDrive *drive = g_ptr_array_index(drives, i);
        if (strcmp(drive->kind, "File Folder") == 0)
            continue; /* the documents, listed above */
        add_look_in_row(chooser, drive->root, drive->name, 2);
        if (is_deepest_anchor(drive->root, location, all, count))
            add_look_in_chain(chooser, drive->root, location, 2);
    }
    GFile *network = g_file_new_for_uri(XD_NETWORK_URI);
    add_look_in_row(chooser, network, "My Network Places", 1);
    g_object_unref(network);
    if (is_recent(location))
        add_look_in_row(chooser, location, nullptr, 1);

    /* The folder shown is the row chosen. */
    GtkTreeModel *model = GTK_TREE_MODEL(chooser->look_in_store);
    GtkTreeIter iter;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more;
         more = gtk_tree_model_iter_next(model, &iter)) {
        GFile *file = nullptr;
        gtk_tree_model_get(model, &iter, LOOK_FILE, &file, -1);
        bool current = file != nullptr && g_file_equal(file, location);
        g_clear_object(&file);
        if (current) {
            gtk_combo_box_set_active_iter(GTK_COMBO_BOX(chooser->look_in), &iter);
            break;
        }
    }
    g_ptr_array_unref(anchors);
    g_ptr_array_unref(drives);
    g_object_unref(documents);
    g_object_unref(desktop);
}

static void go_to(xd_chooser *chooser, GFile *location, bool remember);

static void update_places(xd_chooser *chooser) {
    GFile *location = get_location(chooser);
    for (size_t i = 0; i < PLACE_COUNT; i++) {
        chooser_place *place = &chooser->places[i];
        if (place->button != nullptr)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(place->button),
                                         g_file_equal(place->location, location));
    }
}

/* After moving to another folder: the boxes, buttons and panes say so. */
static void update_location_widgets(xd_chooser *chooser) {
    chooser->updating = true;
    GFile *location = get_location(chooser);
    GFile *up = get_up_location(location);
    gtk_widget_set_sensitive(chooser->up_button, up != nullptr);
    g_clear_object(&up);
    gtk_widget_set_sensitive(chooser->back_button, chooser->back != nullptr);
    char *path = get_folder_path(chooser);
    gtk_widget_set_sensitive(chooser->new_folder_button, path != nullptr);
    g_free(path);
    /* 7's search box, which searches the folder shown. */
    char *name = get_location_name(location);
    char *hint = g_strdup_printf("Search %s", name);
    gtk_entry_set_placeholder_text(GTK_ENTRY(chooser->search), hint);
    g_free(hint);
    g_free(name);
    gtk_entry_set_text(GTK_ENTRY(chooser->search), "");
    fill_look_in(chooser);
    update_places(chooser);
    chooser->updating = false;
}

/* The files My Recent Documents lists. */
static GPtrArray *get_recent_files(void) {
    GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
    char **paths = nullptr;
    size_t count = 0;
    xd_recent_load(&paths, &count, RECENT_MAX);
    for (size_t i = 0; i < count; i++)
        g_ptr_array_add(files, g_file_new_for_path(paths[i]));
    xd_free_lines(paths, count);
    return files;
}

static void go_to(xd_chooser *chooser, GFile *location, bool remember) {
    GFile *current = get_location(chooser);
    if (current != nullptr && g_file_equal(current, location)) {
        update_location_widgets(chooser);
        return;
    }
    if (remember && current != nullptr)
        chooser->back = g_list_prepend(chooser->back, g_object_ref(current));
    g_clear_pointer(&chooser->search_query, xd_search_query_unref);
    if (is_recent(location)) {
        GPtrArray *files = get_recent_files();
        xd_folder_view_set_files(chooser->view, location, files);
        g_ptr_array_unref(files);
    } else {
        xd_folder_view_set_location(chooser->view, location);
    }
    update_location_widgets(chooser);
}

static void go_back(xd_chooser *chooser) {
    if (chooser->back == nullptr)
        return;
    GFile *previous = chooser->back->data;
    chooser->back = g_list_delete_link(chooser->back, chooser->back);
    go_to(chooser, previous, false);
    g_object_unref(previous);
}

static void go_up(xd_chooser *chooser) {
    GFile *up = get_up_location(get_location(chooser));
    if (up != nullptr) {
        go_to(chooser, up, true);
        g_object_unref(up);
    }
}

/* ---- finishing ---------------------------------------------------------------------- */

static void free_choice(gpointer data) {
    chooser_choice *choice = data;
    g_free(choice->id);
    g_free(choice);
}

static void free_chooser(xd_chooser *chooser) {
    g_free(chooser->title);
    g_strfreev(chooser->save_names);
    g_ptr_array_unref(chooser->filters);
    xd_chooser_filter_free(chooser->pattern);
    g_ptr_array_unref(chooser->choices);
    g_list_free_full(chooser->back, g_object_unref);
    g_clear_object(&chooser->look_in_store);
    for (size_t i = 0; i < PLACE_COUNT; i++)
        g_clear_object(&chooser->places[i].location);
    xd_search_query_unref(chooser->search_query);
    g_clear_object(&chooser->renaming);
    g_clear_object(&chooser->pending_rename);
    g_free(chooser);
}

/* Ends the dialog with `response`; `uris` (nullptr-terminated) for OK. */
static void finish(xd_chooser *chooser, unsigned response, char **uris) {
    if (chooser->finished)
        return;
    chooser->finished = true;
    GVariant *results = nullptr;
    if (response == XD_CHOOSER_OK) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "uris",
                              g_variant_new_strv((const char *const *)uris, -1));
        if (chooser->choices->len > 0) {
            GVariantBuilder picked;
            g_variant_builder_init(&picked, G_VARIANT_TYPE("a(ss)"));
            for (guint i = 0; i < chooser->choices->len; i++) {
                const chooser_choice *choice = g_ptr_array_index(chooser->choices, i);
                const char *value =
                    choice->is_check
                        ? (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(choice->widget))
                               ? "true"
                               : "false")
                        : gtk_combo_box_get_active_id(GTK_COMBO_BOX(choice->widget));
                g_variant_builder_add(&picked, "(ss)", choice->id, value != nullptr ? value : "");
            }
            g_variant_builder_add(&builder, "{sv}", "choices", g_variant_builder_end(&picked));
        }
        if (chooser->filters_given && !chooser->directory && chooser->filter_index >= 0) {
            const xd_chooser_filter *filter =
                g_ptr_array_index(chooser->filters, (guint)chooser->filter_index);
            g_variant_builder_add(&builder, "{sv}", "current_filter", filter->source);
        }
        if (chooser->mode == XD_CHOOSER_OPEN)
            g_variant_builder_add(
                &builder, "{sv}", "writable",
                g_variant_new_boolean(chooser->read_only == nullptr ||
                                      !gtk_toggle_button_get_active(
                                          GTK_TOGGLE_BUTTON(chooser->read_only))));
        results = g_variant_builder_end(&builder);
    }
    gtk_widget_hide(chooser->window);
    chooser->done(response, results, chooser->done_data);
    gtk_widget_destroy(chooser->window);
    free_chooser(chooser);
}

/* Finishes with these paths, as file:// URIs. */
static void finish_with_paths(xd_chooser *chooser, char *const *paths) {
    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    for (char *const *path = paths; *path != nullptr; path++) {
        char *uri = g_filename_to_uri(*path, nullptr, nullptr);
        if (uri != nullptr)
            g_ptr_array_add(uris, uri);
    }
    g_ptr_array_add(uris, nullptr);
    finish(chooser, XD_CHOOSER_OK, (char **)uris->pdata);
    g_ptr_array_unref(uris);
}

static void show_warning(xd_chooser *chooser, const char *name, const char *problem) {
    char *text = g_strdup_printf("%s\n%s", name, problem);
    xd_message_box(GTK_WINDOW(chooser->window), chooser->title, XD_MB_WARNING, XD_MB_OK, FALSE,
                   text);
    g_free(text);
}

/* Save As over a file that's there: XP asks in a box titled as the
 * dialog. */
static bool can_replace(xd_chooser *chooser, const char *path) {
    char *shown = g_filename_display_basename(path);
    char *text = g_strdup_printf("%s already exists.\nDo you want to replace it?", shown);
    int answer = xd_message_box(GTK_WINDOW(chooser->window), chooser->title, XD_MB_WARNING,
                                XD_MB_YES_NO, TRUE, text);
    g_free(text);
    g_free(shown);
    return answer == GTK_RESPONSE_YES;
}

/* SaveFiles: the files' names in `folder`, each made unique if taken. */
static void finish_save_files(xd_chooser *chooser, const char *folder) {
    GFile *dir = g_file_new_for_path(folder);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (char **name = chooser->save_names; name != nullptr && *name != nullptr; name++) {
        char *path = g_build_filename(folder, *name, nullptr);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            g_free(path);
            const char *dot = strrchr(*name, '.');
            bool has_extension = dot != nullptr && dot != *name;
            char *base = has_extension ? g_strndup(*name, (gsize)(dot - *name)) : g_strdup(*name);
            char *unique = xd_unique_name(dir, base, has_extension ? dot : nullptr);
            path = g_build_filename(folder, unique, nullptr);
            g_free(unique);
            g_free(base);
        }
        g_ptr_array_add(paths, path);
    }
    g_ptr_array_add(paths, nullptr);
    finish_with_paths(chooser, (char *const *)paths->pdata);
    g_ptr_array_unref(paths);
    g_object_unref(dir);
}

static void set_name_text(xd_chooser *chooser, const char *text) {
    gtk_entry_set_text(GTK_ENTRY(chooser->name_entry), text);
}

/* Open, Save or Select Folder: what File name says, as XP read it. */
static void accept(xd_chooser *chooser) {
    if (chooser->renaming != nullptr)
        return;
    char *folder = get_folder_path(chooser);
    const char *text = gtk_entry_get_text(GTK_ENTRY(chooser->name_entry));
    xd_typed_name typed = xd_chooser_parse_name(folder != nullptr ? folder : g_get_home_dir(),
                                                text, chooser->multiple);
    switch (typed.kind) {
    case XD_TYPED_EMPTY: {
        /* Nothing typed: into the folder selected, or (choosing a folder)
         * the folder shown. */
        GList *selected = xd_folder_view_get_selected_files(chooser->view);
        GFile *first = selected != nullptr ? selected->data : nullptr;
        char *first_path = first != nullptr ? g_file_get_path(first) : nullptr;
        bool into = first != nullptr && (first_path == nullptr ||
                                         g_file_test(first_path, G_FILE_TEST_IS_DIR));
        if (chooser->directory && first_path != nullptr && into) {
            char *paths[] = {first_path, nullptr};
            if (chooser->mode == XD_CHOOSER_SAVE_FILES)
                finish_save_files(chooser, first_path);
            else
                finish_with_paths(chooser, paths);
        } else if (into) {
            go_to(chooser, first, true);
        } else if (chooser->directory && folder != nullptr) {
            char *paths[] = {folder, nullptr};
            if (chooser->mode == XD_CHOOSER_SAVE_FILES)
                finish_save_files(chooser, folder);
            else
                finish_with_paths(chooser, paths);
        }
        g_free(first_path);
        g_list_free_full(selected, g_object_unref);
        break;
    }
    case XD_TYPED_PATTERN: {
        xd_chooser_filter_free(chooser->pattern);
        chooser->pattern = new_pattern_filter(typed.pattern);
        GFile *dir = g_file_new_for_path(typed.paths[0]);
        go_to(chooser, dir, true);
        g_object_unref(dir);
        xd_folder_view_refilter(chooser->view);
        break;
    }
    case XD_TYPED_FOLDER:
        if (chooser->directory && !g_str_has_suffix(text, "/")) {
            if (chooser->mode == XD_CHOOSER_SAVE_FILES)
                finish_save_files(chooser, typed.paths[0]);
            else
                finish_with_paths(chooser, typed.paths);
        } else {
            GFile *dir = g_file_new_for_path(typed.paths[0]);
            go_to(chooser, dir, true);
            g_object_unref(dir);
            set_name_text(chooser, "");
        }
        break;
    case XD_TYPED_FILE: {
        if (chooser->mode == XD_CHOOSER_OPEN || chooser->directory) {
            for (char **path = typed.paths; *path != nullptr; path++) {
                if (!g_file_test(*path, G_FILE_TEST_EXISTS)) {
                    char *shown = g_filename_display_basename(*path);
                    if (chooser->directory)
                        show_warning(chooser, shown,
                                     "Path does not exist.\nPlease verify the correct path was "
                                     "given.");
                    else
                        show_warning(chooser, shown,
                                     "File not found.\nPlease verify the correct file name was "
                                     "given.");
                    g_free(shown);
                    xd_typed_name_free(&typed);
                    g_free(folder);
                    return;
                }
            }
            finish_with_paths(chooser, typed.paths);
            break;
        }
        /* Save As: the type's extension added, as XP did. */
        char *path = xd_chooser_add_extension(typed.paths[0], get_current_filter(chooser));
        char *parent = g_path_get_dirname(path);
        if (!g_file_test(parent, G_FILE_TEST_IS_DIR)) {
            char *shown = g_filename_display_name(path);
            show_warning(chooser, shown,
                         "Path does not exist.\nPlease verify the correct path was given.");
            g_free(shown);
        } else if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            GFile *dir = g_file_new_for_path(path);
            go_to(chooser, dir, true);
            g_object_unref(dir);
            set_name_text(chooser, "");
        } else if (!g_file_test(path, G_FILE_TEST_EXISTS) || can_replace(chooser, path)) {
            char *paths[] = {path, nullptr};
            finish_with_paths(chooser, paths);
        }
        g_free(parent);
        g_free(path);
        break;
    }
    }
    xd_typed_name_free(&typed);
    g_free(folder);
}

/* ---- the folder view's signals ------------------------------------------------------------ */

static void on_item_activated([[maybe_unused]] XdFolderView *view, GFile *file,
                              gboolean is_directory, gpointer data) {
    xd_chooser *chooser = data;
    if (is_directory) {
        go_to(chooser, file, true);
        return;
    }
    if (chooser->directory)
        return;
    if (chooser->mode == XD_CHOOSER_SAVE) {
        accept(chooser); /* the name is in File name already; Save As asks to replace */
        return;
    }
    GList *selected = xd_folder_view_get_selected_files(chooser->view);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (GList *link = selected; link != nullptr; link = link->next) {
        char *path = g_file_get_path(link->data);
        if (path != nullptr)
            g_ptr_array_add(paths, path);
    }
    if (paths->len == 0) {
        char *path = g_file_get_path(file);
        if (path != nullptr)
            g_ptr_array_add(paths, path);
    }
    g_ptr_array_add(paths, nullptr);
    if (paths->len > 1)
        finish_with_paths(chooser, (char *const *)paths->pdata);
    g_ptr_array_unref(paths);
    g_list_free_full(selected, g_object_unref);
}

/* The name as File name shows it: just the name in the folder shown, the
 * whole path elsewhere (My Recent Documents). */
static char *get_shown_name(xd_chooser *chooser, GFile *file) {
    GFile *parent = g_file_get_parent(file);
    bool here = parent != nullptr && g_file_equal(parent, get_location(chooser));
    g_clear_object(&parent);
    if (here)
        return g_file_get_basename(file);
    return g_file_get_path(file);
}

/* Selecting files puts their names in File name ("a.txt" "b.txt" for
 * several); folders only when choosing one. */
static void on_selection_changed([[maybe_unused]] XdFolderView *view, gpointer data) {
    xd_chooser *chooser = data;
    if (chooser->updating || chooser->renaming != nullptr)
        return;
    GList *selected = xd_folder_view_get_selected_files(chooser->view);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (GList *link = selected; link != nullptr; link = link->next) {
        char *path = g_file_get_path(link->data);
        bool is_directory = path == nullptr || g_file_test(path, G_FILE_TEST_IS_DIR);
        g_free(path);
        if (is_directory != chooser->directory)
            continue;
        char *name = get_shown_name(chooser, link->data);
        if (name != nullptr)
            g_ptr_array_add(names, name);
    }
    if (names->len == 1) {
        set_name_text(chooser, g_ptr_array_index(names, 0));
    } else if (names->len > 1) {
        GString *text = g_string_new(nullptr);
        for (guint i = 0; i < names->len; i++)
            g_string_append_printf(text, "%s\"%s\"", i > 0 ? " " : "",
                                   (const char *)g_ptr_array_index(names, i));
        set_name_text(chooser, text->str);
        g_string_free(text, TRUE);
    }
    g_ptr_array_unref(names);
    g_list_free_full(selected, g_object_unref);
}

static void on_load_error([[maybe_unused]] XdFolderView *view, const char *message,
                          gpointer data) {
    xd_chooser *chooser = data;
    char *name = get_location_name(get_location(chooser));
    char *text = g_strdup_printf("%s is not accessible.\n\n%s", name, message);
    xd_message_box(GTK_WINDOW(chooser->window), chooser->title, XD_MB_ERROR, XD_MB_OK, FALSE,
                   text);
    g_free(text);
    g_free(name);
}

/* ---- Create New Folder, named in place ----------------------------------------------- */

static void place_rename_entry(xd_chooser *chooser) {
    GdkRectangle area;
    if (chooser->renaming == nullptr ||
        !xd_folder_view_get_name_area(chooser->view, chooser->renaming, &area))
        return;
    int x = 0, y = 0;
    gtk_widget_translate_coordinates(GTK_WIDGET(chooser->view), chooser->overlay, area.x, area.y,
                                     &x, &y);
    gtk_widget_set_margin_start(chooser->rename_entry, MAX(x - 2, 0));
    gtk_widget_set_margin_top(chooser->rename_entry, MAX(y - 2, 0));
    gtk_widget_set_size_request(chooser->rename_entry, MAX(area.width + 24, 100), -1);
}

static void begin_rename(xd_chooser *chooser, GFile *folder) {
    GdkRectangle area;
    if (!xd_folder_view_get_name_area(chooser->view, folder, &area)) {
        g_set_object(&chooser->pending_rename, folder); /* when it's listed */
        return;
    }
    g_clear_object(&chooser->pending_rename);
    GList *one = g_list_append(nullptr, folder);
    xd_folder_view_select_files(chooser->view, one);
    g_list_free(one);
    g_set_object(&chooser->renaming, folder);
    char *name = xd_rename_text(folder);
    gtk_entry_set_text(GTK_ENTRY(chooser->rename_entry), name);
    g_free(name);
    place_rename_entry(chooser);
    gtk_widget_show(chooser->rename_entry);
    gtk_widget_grab_focus(chooser->rename_entry);
    gtk_editable_select_region(GTK_EDITABLE(chooser->rename_entry), 0, -1);
}

static void end_rename(xd_chooser *chooser) {
    g_clear_object(&chooser->renaming);
    gtk_widget_hide(chooser->rename_entry);
    gtk_widget_grab_focus(GTK_WIDGET(chooser->view));
}

static void commit_rename(xd_chooser *chooser) {
    if (chooser->renaming == nullptr)
        return;
    const char *text = gtk_entry_get_text(GTK_ENTRY(chooser->rename_entry));
    char *old = xd_rename_text(chooser->renaming);
    if (*text != '\0' && strcmp(text, old) != 0) {
        GError *error = nullptr;
        GFile *renamed = strchr(text, '/') == nullptr
                             ? xd_rename(chooser->renaming, text, &error)
                             : nullptr;
        if (renamed != nullptr) {
            GList *one = g_list_append(nullptr, renamed);
            xd_folder_view_select_files(chooser->view, one);
            g_list_free(one);
            g_object_unref(renamed);
        } else {
            char *message =
                error == nullptr
                    ? g_strdup("A file name cannot contain the following character:\n        /")
                    : g_strdup_printf("Cannot rename %s: %s", old, error->message);
            g_clear_error(&error);
            xd_message_box(GTK_WINDOW(chooser->window), "Error Renaming File or Folder",
                           XD_MB_ERROR, XD_MB_OK, FALSE, message);
            g_free(message);
        }
    }
    g_free(old);
    end_rename(chooser);
}

static void on_rename_activate([[maybe_unused]] GtkEntry *entry, gpointer data) {
    commit_rename(data);
}

static gboolean on_rename_key([[maybe_unused]] GtkWidget *widget, GdkEventKey *event,
                              gpointer data) {
    if (event->keyval == GDK_KEY_Escape) {
        end_rename(data);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_rename_focus_out([[maybe_unused]] GtkWidget *widget,
                                    [[maybe_unused]] GdkEventFocus *event, gpointer data) {
    commit_rename(data);
    return FALSE;
}

static void on_contents_changed([[maybe_unused]] XdFolderView *view, gpointer data) {
    xd_chooser *chooser = data;
    if (chooser->pending_rename != nullptr) {
        GFile *folder = g_object_ref(chooser->pending_rename);
        begin_rename(chooser, folder);
        g_object_unref(folder);
    }
}

static void create_folder(xd_chooser *chooser) {
    char *path = get_folder_path(chooser);
    if (path == nullptr)
        return;
    GFile *dir = g_file_new_for_path(path);
    char *name = xd_unique_name(dir, "New Folder", nullptr);
    GFile *folder = g_file_get_child(dir, name);
    GError *error = nullptr;
    if (g_file_make_directory(folder, nullptr, &error)) {
        xd_folder_view_reload(chooser->view);
        begin_rename(chooser, folder);
    } else {
        char *message = g_strdup_printf("Unable to create the folder: %s", error->message);
        xd_message_box(GTK_WINDOW(chooser->window), "Error", XD_MB_ERROR, XD_MB_OK, FALSE,
                       message);
        g_free(message);
        g_error_free(error);
    }
    g_object_unref(folder);
    g_free(name);
    g_object_unref(dir);
    g_free(path);
}

/* ---- the controls' signals --------------------------------------------------------------- */

static void on_accept_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    accept(data);
}

static void on_cancel_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    finish(data, XD_CHOOSER_CANCELLED, nullptr);
}

static gboolean on_delete([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] GdkEvent *event,
                          gpointer data) {
    finish(data, XD_CHOOSER_CANCELLED, nullptr);
    return TRUE;
}

static void on_back_clicked([[maybe_unused]] GtkButton *button, gpointer data) { go_back(data); }
static void on_up_clicked([[maybe_unused]] GtkButton *button, gpointer data) { go_up(data); }
static void on_new_folder_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    create_folder(data);
}

/* Keys nothing else took: Esc cancels; Backspace and Alt+Up go up a
 * level, as in XP; Alt+Left goes back, as in Explorer. */
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    xd_chooser *chooser = data;
    GdkModifierType mods = event->state & gtk_accelerator_get_default_mod_mask();
    if (event->keyval == GDK_KEY_Escape && mods == 0) {
        finish(chooser, XD_CHOOSER_CANCELLED, nullptr);
        return TRUE;
    }
    if (GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(widget))))
        return FALSE;
    if (event->keyval == GDK_KEY_BackSpace && mods == 0) {
        go_up(chooser);
        return TRUE;
    }
    if (mods == GDK_MOD1_MASK && event->keyval == GDK_KEY_Up) {
        go_up(chooser);
        return TRUE;
    }
    if (mods == GDK_MOD1_MASK && event->keyval == GDK_KEY_Left) {
        go_back(chooser);
        return TRUE;
    }
    return FALSE;
}

static void on_look_in_changed(GtkComboBox *combo, gpointer data) {
    xd_chooser *chooser = data;
    GtkTreeIter iter;
    if (chooser->updating || !gtk_combo_box_get_active_iter(combo, &iter))
        return;
    GFile *file = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(chooser->look_in_store), &iter, LOOK_FILE, &file, -1);
    if (file != nullptr) {
        go_to(chooser, file, true);
        g_object_unref(file);
    }
}

static void on_place_toggled(GtkToggleButton *button, gpointer data) {
    xd_chooser *chooser = data;
    if (chooser->updating)
        return;
    for (size_t i = 0; i < PLACE_COUNT; i++) {
        if (chooser->places[i].button == GTK_WIDGET(button)) {
            go_to(chooser, chooser->places[i].location, true);
            break;
        }
    }
    /* Pressing the place shown keeps it pressed. */
    update_location_widgets(chooser);
}

static void on_type_changed(GtkComboBox *combo, gpointer data) {
    xd_chooser *chooser = data;
    int index = gtk_combo_box_get_active(combo);
    if (index < 0 || index == chooser->filter_index)
        return;
    g_clear_pointer(&chooser->pattern, xd_chooser_filter_free);
    chooser->filter_index = index;
    xd_folder_view_refilter(chooser->view);
}

static void on_view_mode(GtkCheckMenuItem *item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item))
        return;
    xd_chooser *chooser = data;
    int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-mode"));
    xd_folder_view_set_mode(chooser->view, (XdViewMode)mode);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    xd_chooser *chooser = data;
    if (chooser->updating)
        return;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    g_clear_pointer(&chooser->search_query, xd_search_query_unref);
    if (*text != '\0') {
        /* Part of a name, wildcards, "phrases", /patterns/ and the name
         * and kind filters; a pattern that isn't one filters nothing. */
        xd_search_criteria flags;
        xd_search_criteria_init(&flags);
        chooser->search_query = xd_search_query_parse(text, &flags, nullptr, 0, nullptr);
    }
    xd_folder_view_set_highlight(chooser->view, chooser->search_query);
    xd_folder_view_refilter(chooser->view);
}

/* ---- building the dialog ---------------------------------------------------------------- */

static GtkWidget *new_tool_button(const char *icon, const char *tooltip, GCallback clicked,
                                  xd_chooser *chooser) {
    GtkWidget *button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(button, tooltip);
    /* Toolbar buttons aren't tab stops in Windows. */
    gtk_widget_set_can_focus(button, FALSE);
    if (clicked != nullptr)
        g_signal_connect(button, "clicked", clicked, chooser);
    return button;
}

/* XP's View Menu: the folder view's five views, in its order. */
static GtkWidget *new_view_button(xd_chooser *chooser, XdViewMode current) {
    static const char *const names[] = {"Thumbnails", "Tiles", "Icons", "List", "Details"};
    GtkWidget *menu = gtk_menu_new();
    GSList *group = nullptr;
    for (size_t i = 0; i < XD_VIEW_COUNT; i++) {
        XdViewMode mode = (XdViewMode)i;
        const char *name = names[i];
        GtkWidget *item = gtk_radio_menu_item_new_with_label(group, name);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "xd-mode", GINT_TO_POINTER((int)mode));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), mode == current);
        g_signal_connect(item, "toggled", G_CALLBACK(on_view_mode), chooser);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    GtkWidget *button = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(button),
                         gtk_image_new_from_icon_name("view-list", GTK_ICON_SIZE_SMALL_TOOLBAR));
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(button, "View Menu");
    gtk_widget_set_can_focus(button, FALSE);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
    return button;
}

/* XP's Places bar, in the scheme's button shadow colour with white
 * labels, as comdlg32 draws it. */
static GtkWidget *new_places_bar(xd_chooser *chooser) {
    static const char *const names[PLACE_COUNT] = {"My Recent\nDocuments", "Desktop",
                                                   "My Documents", "My Computer",
                                                   "My Network\nPlaces"};
    static const char *const icons[PLACE_COUNT] = {"document-open-recent", "user-desktop",
                                                   "folder-documents", "computer",
                                                   "network-workgroup"};
    chooser->places[0].location = g_file_new_for_uri(RECENT_URI);
    chooser->places[1].location = new_special_dir(G_USER_DIRECTORY_DESKTOP);
    chooser->places[2].location = new_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    chooser->places[3].location = xd_place_computer();
    chooser->places[4].location = g_file_new_for_uri(XD_NETWORK_URI);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "xd-places");
    for (size_t i = 0; i < PLACE_COUNT; i++) {
        GtkWidget *button = gtk_toggle_button_new();
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name(icons[i], GTK_ICON_SIZE_DND),
                           FALSE, FALSE, 0);
        GtkWidget *label = gtk_label_new(names[i]);
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), box);
        gtk_widget_set_can_focus(button, FALSE);
        g_signal_connect(button, "toggled", G_CALLBACK(on_place_toggled), chooser);
        gtk_box_pack_start(GTK_BOX(bar), button, FALSE, FALSE, 0);
        chooser->places[i].button = button;
    }

    xd_settings settings;
    xd_settings_load(&settings);
    xd_scheme scheme;
    xd_settings_scheme(&settings, &scheme);
    xd_rgb shadow = scheme.colors[XD_COLOR_BUTTON_SHADOW];
    char *css = g_strdup_printf(
        ".xd-places { background-color: #%02x%02x%02x; padding: 2px; }"
        ".xd-places button { color: #ffffff; border-radius: 0; padding: 4px 2px;"
        " background-image: none; }"
        ".xd-places button label { color: #ffffff; }"
        ".xd-places button:checked { background-color: rgba(0,0,0,0.25); }",
        shadow.r, shadow.g, shadow.b);
    /* One for the process, given each dialog's colours (the scheme may
     * have changed since the last). */
    static GtkCssProvider *provider;
    if (provider == nullptr) {
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                                  GTK_STYLE_PROVIDER(provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    g_free(css);
    gtk_widget_set_size_request(bar, 86, -1);
    return bar;
}

static GtkWidget *new_look_in(xd_chooser *chooser) {
    chooser->look_in_store =
        gtk_list_store_new(LOOK_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_FILE, G_TYPE_INT);
    GtkWidget *combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(chooser->look_in_store));
    /* The indent is an empty cell of its own: as the icon's padding it
     * went on both sides, pushing deeper folders' names away from their
     * icons. */
    GtkCellRenderer *indent = gtk_cell_renderer_text_new();
    g_object_set(indent, "xpad", 0, "ypad", 0, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), indent, FALSE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), indent, "width", LOOK_INDENT,
                                   nullptr);
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "xpad", 2, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), icon, FALSE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), icon, "gicon", LOOK_ICON, nullptr);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), text, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), text, "text", LOOK_NAME, nullptr);
    /* Packed cells are as wide in every row as in the widest (fixed size),
     * which would give every row the deepest one's indent. */
    GtkCellArea *area = gtk_cell_layout_get_area(GTK_CELL_LAYOUT(combo));
    GtkCellRenderer *cells[] = {indent, icon, text};
    for (size_t i = 0; i < G_N_ELEMENTS(cells); i++)
        gtk_cell_area_cell_set(area, cells[i], "fixed-size", FALSE, nullptr);
    g_signal_connect(combo, "changed", G_CALLBACK(on_look_in_changed), chooser);
    chooser->look_in = combo;
    return combo;
}

/* The folder view, in a scrolled window under an overlay for renaming. */
static GtkWidget *new_file_list(xd_chooser *chooser, const xd_settings *settings) {
    chooser->view = XD_FOLDER_VIEW(xd_folder_view_new());
    xd_folder_view_set_mode(chooser->view, XD_VIEW_LIST);
    xd_folder_view_set_multiple(chooser->view, chooser->multiple);
    xd_folder_view_set_show_hidden(chooser->view, settings->folders.show_hidden);
    xd_folder_view_set_hide_extensions(chooser->view, settings->folders.hide_extensions);
    xd_folder_view_set_filter(chooser->view, keep_item, chooser);
    g_signal_connect(chooser->view, "item-activated", G_CALLBACK(on_item_activated), chooser);
    g_signal_connect(chooser->view, "selection-changed", G_CALLBACK(on_selection_changed),
                     chooser);
    g_signal_connect(chooser->view, "contents-changed", G_CALLBACK(on_contents_changed),
                     chooser);
    g_signal_connect(chooser->view, "load-error", G_CALLBACK(on_load_error), chooser);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(chooser->view));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);

    chooser->overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(chooser->overlay), scroll);
    chooser->rename_entry = gtk_entry_new();
    gtk_widget_set_halign(chooser->rename_entry, GTK_ALIGN_START);
    gtk_widget_set_valign(chooser->rename_entry, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(chooser->rename_entry, TRUE);
    g_signal_connect(chooser->rename_entry, "activate", G_CALLBACK(on_rename_activate), chooser);
    g_signal_connect(chooser->rename_entry, "key-press-event", G_CALLBACK(on_rename_key),
                     chooser);
    g_signal_connect(chooser->rename_entry, "focus-out-event", G_CALLBACK(on_rename_focus_out),
                     chooser);
    gtk_overlay_add_overlay(GTK_OVERLAY(chooser->overlay), chooser->rename_entry);
    return chooser->overlay;
}

/* The program's choices: a check box for each without options, else a
 * labelled combo. */
static void add_choices(xd_chooser *chooser, GVariant *choices, GtkGrid *grid, int *row) {
    if (choices == nullptr || !g_variant_is_of_type(choices, G_VARIANT_TYPE("a(ssa(ss)s)")))
        return;
    GVariantIter iter;
    g_variant_iter_init(&iter, choices);
    const char *id = nullptr, *label = nullptr, *selected = nullptr;
    GVariantIter *options = nullptr;
    while (g_variant_iter_next(&iter, "(&s&sa(ss)&s)", &id, &label, &options, &selected)) {
        chooser_choice *choice = g_new0(chooser_choice, 1);
        choice->id = g_strdup(id);
        if (g_variant_iter_n_children(options) == 0) {
            choice->is_check = true;
            choice->widget = gtk_check_button_new_with_mnemonic(label);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(choice->widget),
                                         g_strcmp0(selected, "true") == 0);
            gtk_grid_attach(grid, choice->widget, 1, *row, 1, 1);
        } else {
            choice->widget = gtk_combo_box_text_new();
            const char *option_id = nullptr, *option_label = nullptr;
            while (g_variant_iter_next(options, "(&s&s)", &option_id, &option_label))
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(choice->widget), option_id,
                                          option_label);
            if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(choice->widget), selected))
                gtk_combo_box_set_active(GTK_COMBO_BOX(choice->widget), 0);
            GtkWidget *caption = gtk_label_new_with_mnemonic(label);
            gtk_label_set_xalign(GTK_LABEL(caption), 0);
            gtk_label_set_mnemonic_widget(GTK_LABEL(caption), choice->widget);
            gtk_grid_attach(grid, caption, 0, *row, 1, 1);
            gtk_grid_attach(grid, choice->widget, 1, *row, 1, 1);
        }
        g_variant_iter_free(options);
        g_ptr_array_add(chooser->choices, choice);
        (*row)++;
    }
}

static GtkWidget *new_caption(const char *text, GtkWidget *target) {
    GtkWidget *label = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), target);
    return label;
}

/* File name, Files of type, the choices and Open as read-only, in a grid. */
static GtkWidget *new_name_grid(xd_chooser *chooser, GVariant *choices) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    int row = 0;
    const char *name_caption = chooser->directory ? "_Folder:" : "File _name:";
    gtk_grid_attach(GTK_GRID(grid), new_caption(name_caption, chooser->name_entry), 0, row, 1, 1);
    gtk_widget_set_hexpand(chooser->name_combo, TRUE);
    gtk_grid_attach(GTK_GRID(grid), chooser->name_combo, 1, row, 1, 1);
    if (!chooser->directory) {
        row++;
        const char *type_caption =
            chooser->mode == XD_CHOOSER_SAVE ? "Save as _type:" : "Files of _type:";
        gtk_grid_attach(GTK_GRID(grid), new_caption(type_caption, chooser->type_combo), 0, row,
                        1, 1);
        gtk_grid_attach(GTK_GRID(grid), chooser->type_combo, 1, row, 1, 1);
    }
    row++;
    if (chooser->read_only != nullptr) {
        gtk_grid_attach(GTK_GRID(grid), chooser->read_only, 1, row, 1, 1);
        row++;
    }
    add_choices(chooser, choices, GTK_GRID(grid), &row);
    return grid;
}

static GtkWidget *new_accept_buttons(xd_chooser *chooser, const char *accept_label,
                                     GtkOrientation orientation) {
    chooser->accept_button = gtk_button_new_with_mnemonic(accept_label);
    gtk_widget_set_can_default(chooser->accept_button, TRUE);
    g_signal_connect(chooser->accept_button, "clicked", G_CALLBACK(on_accept_clicked), chooser);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel_clicked), chooser);
    GtkWidget *box = gtk_box_new(orientation, 6);
    gtk_box_set_homogeneous(GTK_BOX(box), TRUE);
    gtk_box_pack_start(GTK_BOX(box), chooser->accept_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), cancel, FALSE, FALSE, 0);
    gtk_widget_set_size_request(chooser->accept_button, 75, -1);
    gtk_widget_set_valign(box, GTK_ALIGN_START);
    return box;
}

/* XP's layout: Look in and its buttons, the Places bar beside the list,
 * and the name and type boxes with Open and Cancel at their right; with
 * Windows 7's search box at the end of the Look in row. */
static GtkWidget *build_dialog(xd_chooser *chooser, const xd_settings *settings,
                               GVariant *choices, const char *accept_label) {
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(top), 6);

    GtkWidget *look_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *look_in = new_look_in(chooser);
    const char *look_caption = chooser->mode == XD_CHOOSER_OPEN ? "Look _in:" : "Save _in:";
    GtkWidget *caption = new_caption(look_caption, look_in);
    gtk_widget_set_size_request(caption, 86, -1);
    gtk_box_pack_start(GTK_BOX(look_row), caption, FALSE, FALSE, 0);
    /* Look in takes the row's slack; the search box keeps to the right. */
    gtk_widget_set_size_request(look_in, 230, -1);
    gtk_box_pack_start(GTK_BOX(look_row), look_in, TRUE, TRUE, 0);
    chooser->back_button = new_tool_button("go-previous", "Go To Last Folder Visited",
                                           G_CALLBACK(on_back_clicked), chooser);
    chooser->up_button =
        new_tool_button("go-up", "Up One Level", G_CALLBACK(on_up_clicked), chooser);
    chooser->new_folder_button = new_tool_button("folder-new", "Create New Folder",
                                                 G_CALLBACK(on_new_folder_clicked), chooser);
    gtk_box_pack_start(GTK_BOX(look_row), chooser->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(look_row), chooser->up_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(look_row), chooser->new_folder_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(look_row), new_view_button(chooser, XD_VIEW_LIST), FALSE, FALSE,
                       0);
    chooser->search = gtk_search_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(chooser->search), 12);
    gtk_widget_set_size_request(chooser->search, 150, -1);
    g_signal_connect(chooser->search, "search-changed", G_CALLBACK(on_search_changed), chooser);
    gtk_widget_set_margin_start(chooser->search, 8);
    gtk_box_pack_end(GTK_BOX(look_row), chooser->search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), look_row, FALSE, FALSE, 0);

    GtkWidget *middle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(middle), new_places_bar(chooser), FALSE, FALSE, 0);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(right), new_file_list(chooser, settings), TRUE, TRUE, 0);
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(bottom), new_name_grid(chooser, choices), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bottom),
                       new_accept_buttons(chooser, accept_label, GTK_ORIENTATION_VERTICAL), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), bottom, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(middle), right, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(top), middle, TRUE, TRUE, 0);
    return top;
}

/* Files of type: the program's filters, else All Files. */
static void fill_types(xd_chooser *chooser, GVariant *filters, GVariant *current) {
    chooser->filters = xd_chooser_filters_parse(filters);
    chooser->filters_given = chooser->filters->len > 0;
    chooser->filter_index = 0;
    if (chooser->filters_given && current != nullptr) {
        int found = xd_chooser_filters_find(chooser->filters, current);
        if (found < 0) {
            xd_chooser_filter *extra = xd_chooser_filter_parse(current);
            if (extra != nullptr) {
                g_ptr_array_add(chooser->filters, extra);
                found = (int)chooser->filters->len - 1;
            }
        }
        if (found >= 0)
            chooser->filter_index = found;
    }
    if (!chooser->filters_given) {
        GVariant *all = g_variant_new("(s@a(us))", "All Files",
                                      g_variant_new_array(G_VARIANT_TYPE("(us)"), nullptr, 0));
        g_ptr_array_add(chooser->filters, xd_chooser_filter_parse(all));
    }
    chooser->type_combo = gtk_combo_box_text_new();
    for (guint i = 0; i < chooser->filters->len; i++) {
        const xd_chooser_filter *filter = g_ptr_array_index(chooser->filters, i);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(chooser->type_combo), filter->name);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(chooser->type_combo), chooser->filter_index);
    g_signal_connect(chooser->type_combo, "changed", G_CALLBACK(on_type_changed), chooser);
}

/* File name's list: the recent files (Open), as XP's remembered names. */
static void fill_name_history(xd_chooser *chooser) {
    if (chooser->mode != XD_CHOOSER_OPEN || chooser->directory)
        return;
    char **paths = nullptr;
    size_t count = 0;
    xd_recent_load(&paths, &count, 10);
    for (size_t i = 0; i < count; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(chooser->name_combo), paths[i]);
    xd_free_lines(paths, count);
}

/* A Flatpak's or other Wayland program's window, as xdg-foreign names it:
 * the dialog opens over it and stays above it. */
static void set_parent(xd_chooser *chooser, const char *parent_window) {
    GdkWindow *window = gtk_widget_get_window(chooser->window);
    if (window == nullptr || !g_str_has_prefix(parent_window, "wayland:") ||
        !GDK_IS_WAYLAND_WINDOW(window))
        return;
    char *handle = g_strdup(parent_window + strlen("wayland:"));
    if (!gdk_wayland_window_set_transient_for_exported(window, handle))
        fprintf(stderr, "xdg-desktop-portal-xeondesktop: cannot open over the window %s\n",
                handle);
    g_free(handle);
}

xd_chooser *xd_chooser_show(const xd_chooser_request *request, xd_chooser_done done,
                            gpointer data) {
    xd_chooser *chooser = g_new0(xd_chooser, 1);
    chooser->mode = request->mode;
    chooser->done = done;
    chooser->done_data = data;
    chooser->choices = g_ptr_array_new_with_free_func(free_choice);
    xd_settings settings;
    xd_settings_load(&settings);

    GVariant *options = request->options;
    gboolean flag = FALSE;
    chooser->multiple = request->mode == XD_CHOOSER_OPEN &&
                        g_variant_lookup(options, "multiple", "b", &flag) && flag;
    chooser->directory = request->mode == XD_CHOOSER_SAVE_FILES ||
                         (request->mode == XD_CHOOSER_OPEN &&
                          g_variant_lookup(options, "directory", "b", &flag) && flag);
    gboolean modal = TRUE;
    g_variant_lookup(options, "modal", "b", &modal);
    const char *accept_label = nullptr;
    g_variant_lookup(options, "accept_label", "&s", &accept_label);
    const char *current_name = nullptr, *current_folder = nullptr, *current_file = nullptr;
    g_variant_lookup(options, "current_name", "&s", &current_name);
    g_variant_lookup(options, "current_folder", "^&ay", &current_folder);
    g_variant_lookup(options, "current_file", "^&ay", &current_file);
    g_variant_lookup(options, "files", "^aay", &chooser->save_names);
    GVariant *filters = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"));
    GVariant *current_filter =
        g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"));
    GVariant *choices = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"));

    const char *default_title = chooser->directory && chooser->mode == XD_CHOOSER_OPEN
                                    ? "Select Folder"
                                : chooser->mode == XD_CHOOSER_OPEN ? "Open"
                                                                   : "Save As";
    chooser->title =
        g_strdup(request->title != nullptr && *request->title != '\0' ? request->title
                                                                      : default_title);
    if (accept_label == nullptr || *accept_label == '\0')
        accept_label = chooser->directory && chooser->mode == XD_CHOOSER_OPEN ? "Select Folder"
                       : chooser->mode == XD_CHOOSER_OPEN                    ? "_Open"
                                                                             : "_Save";

    fill_types(chooser, filters, current_filter);
    chooser->name_combo = gtk_combo_box_text_new_with_entry();
    chooser->name_entry = gtk_bin_get_child(GTK_BIN(chooser->name_combo));
    gtk_entry_set_activates_default(GTK_ENTRY(chooser->name_entry), TRUE);
    fill_name_history(chooser);
    if (chooser->mode == XD_CHOOSER_OPEN && !chooser->directory)
        chooser->read_only = gtk_check_button_new_with_mnemonic("Open as _read-only");

    chooser->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(chooser->window), chooser->title);
    gtk_window_set_type_hint(GTK_WINDOW(chooser->window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_modal(GTK_WINDOW(chooser->window), modal);
    gtk_window_set_default_size(GTK_WINDOW(chooser->window), DIALOG_WIDTH, DIALOG_HEIGHT);
    GtkWidget *content = build_dialog(chooser, &settings, choices, accept_label);
    gtk_container_add(GTK_CONTAINER(chooser->window), content);
    gtk_window_set_default(GTK_WINDOW(chooser->window), chooser->accept_button);
    g_signal_connect(chooser->window, "delete-event", G_CALLBACK(on_delete), chooser);
    g_signal_connect_after(chooser->window, "key-press-event", G_CALLBACK(on_key_press),
                           chooser);

    /* Where it starts: the file being saved again, the folder asked
     * for, else My Documents. */
    GFile *start = nullptr;
    if (current_file != nullptr && *current_file == '/') {
        char *folder = g_path_get_dirname(current_file);
        start = g_file_new_for_path(folder);
        g_free(folder);
        char *name = g_filename_display_basename(current_file);
        set_name_text(chooser, name);
        g_free(name);
    } else if (current_folder != nullptr && *current_folder == '/' &&
               g_file_test(current_folder, G_FILE_TEST_IS_DIR)) {
        start = g_file_new_for_path(current_folder);
    } else {
        start = new_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    }
    if (current_name != nullptr && current_file == nullptr)
        set_name_text(chooser, current_name);
    go_to(chooser, start, false);
    g_object_unref(start);

    gtk_widget_show_all(content);
    gtk_widget_realize(chooser->window);
    set_parent(chooser, request->parent_window != nullptr ? request->parent_window : "");
    gtk_window_present(GTK_WINDOW(chooser->window));
    gtk_widget_grab_focus(chooser->name_entry);
    /* XP selects the name's base so typing replaces it, the extension
     * staying. */
    const char *name = gtk_entry_get_text(GTK_ENTRY(chooser->name_entry));
    const char *dot = strrchr(name, '.');
    gtk_editable_select_region(GTK_EDITABLE(chooser->name_entry), 0,
                               dot != nullptr && dot != name
                                   ? (int)g_utf8_pointer_to_offset(name, dot)
                                   : -1);

    if (filters != nullptr)
        g_variant_unref(filters);
    if (current_filter != nullptr)
        g_variant_unref(current_filter);
    if (choices != nullptr)
        g_variant_unref(choices);
    return chooser;
}

void xd_chooser_close(xd_chooser *chooser) { finish(chooser, XD_CHOOSER_ENDED, nullptr); }
