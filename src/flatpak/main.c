/* SPDX-License-Identifier: MIT
 * xd-flatpak: what Flatpak applications may use outside their sandbox,
 * and where Flatpak gets them from. Linux has no Windows original; the
 * layout is an XP property sheet's.
 *
 *   Applications  the installed apps (the user's and the system's
 *                 installations, through libflatpak); Permissions...
 *                 opens an app's (appperms.c), Reset removes the user's
 *                 changes to it
 *   Remotes       the installations' remotes: add, remove, enable and
 *                 disable them (remotes.c)
 *
 *   xd-flatpak [--page N]   the Nth tab shown, from 0 (Applications)
 *
 * Changes to permissions are the user's own override files, which
 * `flatpak override --user` writes too (perms.h); they take effect when
 * the app next starts. The system-wide overrides an administrator sets
 * with `flatpak override` count, but aren't changed here. Portal
 * permissions (screenshots, location...) are another matter, kept by
 * xdg-desktop-portal's permission store.
 */
#include <stdlib.h>

#include "flatpak.h"
#include "perms.h"

enum { COL_ICON, COL_NAME, COL_ID, COL_WHERE, COL_APP, COL_WEIGHT, COL_COUNT };

typedef struct window {
    GtkWidget *window, *view, *permissions, *reset;
    GtkListStore *apps;
    GPtrArray *list; /* xd_fp_app * */
} window;

/* ---- shared helpers --------------------------------------------------------- */

GtkWidget *xd_fp_frame(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

GtkWidget *xd_fp_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    return label;
}

GtkWidget *xd_fp_scrolled(GtkWidget *view, int height) {
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, height);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    return scroll;
}

GtkTreeViewColumn *xd_fp_text_column(GtkWidget *view, const char *title, int column,
                                     int weight_column) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    GtkTreeViewColumn *view_column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", column, nullptr);
    if (weight_column >= 0)
        gtk_tree_view_column_add_attribute(view_column, renderer, "weight", weight_column);
    gtk_tree_view_column_set_resizable(view_column, TRUE);
    gtk_tree_view_column_set_expand(view_column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), view_column);
    return view_column;
}

GtkWidget *xd_fp_button(const char *mnemonic) {
    GtkWidget *button = gtk_button_new_with_mnemonic(mnemonic);
    gtk_widget_set_size_request(button, 75, -1);
    return button;
}

void xd_fp_error(GtkWindow *parent, const char *what, const GError *error) {
    char *text = error != nullptr ? g_strdup_printf("%s\n\n%s", what, error->message)
                                  : g_strdup(what);
    xd_message_box(parent, XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

GPtrArray *xd_fp_installations(void) {
    GPtrArray *all = g_ptr_array_new_with_free_func(g_object_unref);
    GError *error = nullptr;
    FlatpakInstallation *user = flatpak_installation_new_user(nullptr, &error);
    if (user != nullptr) {
        g_ptr_array_add(all, user);
    } else {
        g_printerr("xd-flatpak: %s\n", error->message);
        g_clear_error(&error);
    }
    GPtrArray *system = flatpak_get_system_installations(nullptr, &error);
    if (system == nullptr) {
        g_printerr("xd-flatpak: %s\n", error->message);
        g_clear_error(&error);
    }
    for (guint i = 0; system != nullptr && i < system->len; i++)
        g_ptr_array_add(all, g_object_ref(g_ptr_array_index(system, i)));
    if (system != nullptr)
        g_ptr_array_unref(system);
    return all;
}

const char *xd_fp_installation_label(FlatpakInstallation *inst) {
    if (flatpak_installation_get_is_user(inst))
        return "Just me";
    const char *id = flatpak_installation_get_id(inst);
    if (id == nullptr || strcmp(id, "default") == 0)
        return "All users";
    const char *name = flatpak_installation_get_display_name(inst);
    return name != nullptr ? name : id;
}

char *xd_fp_user_override_path(const char *app_id) {
    /* The user installation's directory: $FLATPAK_USER_DIR, or
     * $XDG_DATA_HOME/flatpak, as flatpak finds it. */
    static char *dir;
    if (dir == nullptr) {
        FlatpakInstallation *user = flatpak_installation_new_user(nullptr, nullptr);
        if (user != nullptr) {
            GFile *file = flatpak_installation_get_path(user);
            dir = g_file_get_path(file);
            g_object_unref(file);
            g_object_unref(user);
        }
        if (dir == nullptr)
            dir = g_build_filename(g_get_user_data_dir(), "flatpak", nullptr);
    }
    return xd_fp_override_path(dir, app_id);
}

void xd_fp_app_free(xd_fp_app *app) {
    if (app == nullptr)
        return;
    g_free(app->id);
    g_free(app->name);
    g_free(app->branch);
    g_free(app->arch);
    g_clear_object(&app->icon);
    g_clear_object(&app->inst);
    g_clear_object(&app->ref);
    g_free(app);
}

/* ---- the app list ----------------------------------------------------------- */

/* The icon an app exports, by its file: the exports aren't in the icon
 * theme's path unless the session's XDG_DATA_DIRS has them. */
static GIcon *export_icon(const char *deploy, const char *icon) {
    if (icon == nullptr || deploy == nullptr)
        return nullptr;
    static const char *const sizes[] = {"32x32", "48x48", "64x64", "128x128", "256x256",
                                        "scalable"};
    for (size_t i = 0; i < G_N_ELEMENTS(sizes); i++) {
        for (int svg = 0; svg < 2; svg++) {
            char *file = g_strconcat(icon, svg ? ".svg" : ".png", nullptr);
            char *path = g_build_filename(deploy, "export", "share", "icons", "hicolor",
                                          sizes[i], "apps", file, nullptr);
            g_free(file);
            bool there = g_file_test(path, G_FILE_TEST_EXISTS);
            GIcon *gicon = nullptr;
            if (there) {
                GFile *icon_file = g_file_new_for_path(path);
                gicon = g_file_icon_new(icon_file);
                g_object_unref(icon_file);
            }
            g_free(path);
            if (gicon != nullptr)
                return gicon;
        }
    }
    return nullptr;
}

static xd_fp_app *app_new(FlatpakInstallation *inst, FlatpakInstalledRef *installed) {
    FlatpakRef *ref = FLATPAK_REF(installed);
    xd_fp_app *app = g_new0(xd_fp_app, 1);
    app->id = g_strdup(flatpak_ref_get_name(ref));
    app->branch = g_strdup(flatpak_ref_get_branch(ref));
    app->arch = g_strdup(flatpak_ref_get_arch(ref));
    app->inst = g_object_ref(inst);
    app->ref = g_object_ref(installed);
    const char *deploy = flatpak_installed_ref_get_deploy_dir(installed);
    char *file = g_strconcat(app->id, ".desktop", nullptr);
    char *desktop = deploy != nullptr ? g_build_filename(deploy, "export", "share", "applications",
                                                         file, nullptr)
                                      : nullptr;
    g_free(file);
    GKeyFile *kf = g_key_file_new();
    char *icon = nullptr, *name = nullptr;
    if (desktop != nullptr && g_key_file_load_from_file(kf, desktop, G_KEY_FILE_NONE, nullptr)) {
        name = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_NAME, nullptr, nullptr);
        icon = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON,
                                     nullptr);
    }
    g_key_file_free(kf);
    g_free(desktop);
    const char *appdata = flatpak_installed_ref_get_appdata_name(installed);
    app->name = appdata != nullptr && *appdata != '\0' ? g_strdup(appdata)
                : name != nullptr                      ? g_strdup(name)
                                                       : g_strdup(app->id);
    g_free(name);
    app->icon = export_icon(deploy, icon);
    g_free(icon);
    return app;
}

static int compare_apps(gconstpointer left, gconstpointer right) {
    const xd_fp_app *left_app = *(xd_fp_app *const *)left, *right_app = *(xd_fp_app *const *)right;
    int order = g_utf8_collate(left_app->name, right_app->name);
    return order != 0 ? order : g_strcmp0(left_app->id, right_app->id);
}

/* Each installation's apps, one row per app (its current branch when it
 * has several). */
static GPtrArray *load_apps(void) {
    GPtrArray *apps = g_ptr_array_new_with_free_func((GDestroyNotify)xd_fp_app_free);
    GPtrArray *insts = xd_fp_installations();
    for (guint i = 0; i < insts->len; i++) {
        FlatpakInstallation *inst = g_ptr_array_index(insts, i);
        GError *error = nullptr;
        GPtrArray *refs = flatpak_installation_list_installed_refs_by_kind(
            inst, FLATPAK_REF_KIND_APP, nullptr, &error);
        if (refs == nullptr) {
            g_printerr("xd-flatpak: %s\n", error->message);
            g_error_free(error);
            continue;
        }
        GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal); /* id -> index */
        for (guint j = 0; j < refs->len; j++) {
            FlatpakInstalledRef *installed = g_ptr_array_index(refs, j);
            const char *id = flatpak_ref_get_name(FLATPAK_REF(installed));
            gpointer at;
            if (g_hash_table_lookup_extended(seen, id, nullptr, &at)) {
                if (!flatpak_installed_ref_get_is_current(installed))
                    continue;
                xd_fp_app_free(g_ptr_array_index(apps, GPOINTER_TO_UINT(at)));
                xd_fp_app *app = app_new(inst, installed);
                g_ptr_array_index(apps, GPOINTER_TO_UINT(at)) = app;
                g_hash_table_insert(seen, app->id, at);
                continue;
            }
            xd_fp_app *app = app_new(inst, installed);
            g_hash_table_insert(seen, app->id, GUINT_TO_POINTER(apps->len));
            g_ptr_array_add(apps, app);
        }
        g_hash_table_unref(seen);
        g_ptr_array_unref(refs);
    }
    g_ptr_array_unref(insts);
    g_ptr_array_sort(apps, compare_apps);
    return apps;
}

static bool has_override(const char *app_id) {
    char *path = xd_fp_user_override_path(app_id);
    bool there = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return there;
}

/* Fills the list, keeping the selection (by app and installation). */
static void fill_apps(window *sheet) {
    char *keep_id = nullptr;
    FlatpakInstallation *keep_inst = nullptr;
    GtkTreeIter it;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(sheet->view)),
                                        &model, &it)) {
        const xd_fp_app *app;
        gtk_tree_model_get(model, &it, COL_APP, &app, -1);
        keep_id = g_strdup(app->id);
        keep_inst = g_object_ref(app->inst);
    }
    gtk_list_store_clear(sheet->apps);
    if (sheet->list != nullptr)
        g_ptr_array_unref(sheet->list);
    sheet->list = load_apps();
    GtkTreeIter select;
    bool found = false;
    for (guint i = 0; i < sheet->list->len; i++) {
        const xd_fp_app *app = g_ptr_array_index(sheet->list, i);
        GIcon *fallback = app->icon == nullptr ? g_themed_icon_new("application-x-executable")
                                               : nullptr;
        gtk_list_store_insert_with_values(
            sheet->apps, &it, -1, COL_ICON, app->icon != nullptr ? app->icon : fallback, COL_NAME,
            app->name, COL_ID, app->id, COL_WHERE, xd_fp_installation_label(app->inst), COL_APP,
            app, COL_WEIGHT, has_override(app->id) ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, -1);
        if (fallback != nullptr)
            g_object_unref(fallback);
        if (!found && keep_id != nullptr && strcmp(app->id, keep_id) == 0 &&
            flatpak_installation_get_is_user(app->inst) ==
                flatpak_installation_get_is_user(keep_inst)) {
            select = it;
            found = true;
        }
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(sheet->view));
    if (found)
        gtk_tree_selection_select_iter(sel, &select);
    else if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(sheet->apps), &it))
        gtk_tree_selection_select_iter(sel, &it);
    g_free(keep_id);
    if (keep_inst != nullptr)
        g_object_unref(keep_inst);
}

static const xd_fp_app *selected_app(window *sheet) {
    GtkTreeIter it;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(sheet->view)),
                                         &model, &it))
        return nullptr;
    const xd_fp_app *app;
    gtk_tree_model_get(model, &it, COL_APP, &app, -1);
    return app;
}

static void on_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    window *sheet = data;
    const xd_fp_app *app = selected_app(sheet);
    gtk_widget_set_sensitive(sheet->permissions, app != nullptr);
    gtk_widget_set_sensitive(sheet->reset, app != nullptr && has_override(app->id));
}

static void open_permissions(window *sheet) {
    const xd_fp_app *app = selected_app(sheet);
    if (app == nullptr)
        return;
    if (xd_fp_app_dialog(GTK_WINDOW(sheet->window), app))
        fill_apps(sheet);
    on_selected(nullptr, sheet);
}

static void on_permissions([[maybe_unused]] GtkButton *button, gpointer data) {
    open_permissions(data);
}

static void on_activated([[maybe_unused]] GtkTreeView *tree_view,
                         [[maybe_unused]] GtkTreePath *path,
                         [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    open_permissions(data);
}

static void on_reset([[maybe_unused]] GtkButton *button, gpointer data) {
    window *sheet = data;
    const xd_fp_app *app = selected_app(sheet);
    if (app == nullptr)
        return;
    char *message = g_strdup_printf("Do you want to give %s back the permissions it came with?\n\n"
                                    "The changes you made to its permissions will be removed. They "
                                    "take effect the next time it starts.",
                                    app->name);
    int response = xd_message_box(GTK_WINDOW(sheet->window), XD_FLATPAK_TITLE, XD_MB_QUESTION,
                                  XD_MB_YES_NO, TRUE, message);
    g_free(message);
    if (response != GTK_RESPONSE_YES)
        return;
    char *path = xd_fp_user_override_path(app->id);
    GKeyFile *empty = g_key_file_new();
    GError *error = nullptr;
    if (!xd_fp_override_save(empty, path, &error)) {
        xd_fp_error(GTK_WINDOW(sheet->window), "The permissions could not be reset.", error);
        g_error_free(error);
    }
    g_key_file_free(empty);
    g_free(path);
    fill_apps(sheet);
    on_selected(nullptr, sheet);
}

static GtkWidget *apps_page(window *sheet) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("security-high", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(intro), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(intro),
                       xd_fp_label("Flatpak applications run in a sandbox and can use only what "
                                   "they ask for. To change what an application can use, select "
                                   "it and click Permissions. Your changes apply to you only, "
                                   "and take effect the next time the application starts."),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), intro, FALSE, FALSE, 0);

    GtkWidget *widget = gtk_label_new_with_mnemonic("_Installed applications:");
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_box_pack_start(GTK_BOX(page), widget, FALSE, FALSE, 0);
    sheet->apps = gtk_list_store_new(COL_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT);
    sheet->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(sheet->apps));
    g_object_unref(sheet->apps);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), sheet->view);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(sheet->view), COL_NAME);

    GtkTreeViewColumn *name = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(name, "Name");
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(name, icon, FALSE);
    gtk_tree_view_column_add_attribute(name, icon, "gicon", COL_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(name, text, TRUE);
    gtk_tree_view_column_add_attribute(name, text, "text", COL_NAME);
    gtk_tree_view_column_add_attribute(name, text, "weight", COL_WEIGHT);
    gtk_tree_view_column_set_resizable(name, TRUE);
    gtk_tree_view_column_set_expand(name, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(sheet->view), name);
    xd_fp_text_column(sheet->view, "Application ID", COL_ID, -1);
    GtkTreeViewColumn *where = xd_fp_text_column(sheet->view, "Installed for", COL_WHERE, -1);
    gtk_tree_view_column_set_expand(where, FALSE);
    gtk_box_pack_start(GTK_BOX(page), xd_fp_scrolled(sheet->view, 220), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       xd_fp_label("Applications whose permissions you changed are shown in "
                                   "bold."),
                       FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    sheet->reset = xd_fp_button("_Reset");
    sheet->permissions = xd_fp_button("_Permissions...");
    gtk_widget_set_tooltip_text(sheet->reset, "Give the application back the permissions it came "
                                              "with");
    g_signal_connect(sheet->permissions, "clicked", G_CALLBACK(on_permissions), sheet);
    g_signal_connect(sheet->reset, "clicked", G_CALLBACK(on_reset), sheet);
    gtk_box_pack_end(GTK_BOX(row), sheet->reset, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), sheet->permissions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(sheet->view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_selected), sheet);
    g_signal_connect(sheet->view, "row-activated", G_CALLBACK(on_activated), sheet);
    fill_apps(sheet);
    on_selected(nullptr, sheet);
    return page;
}

int main(int argc, char **argv) {
    int page = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
    }
    xd_gtk_app_preinit("org.xeondesktop.Flatpak");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static window sheet;
    sheet.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(sheet.window), XD_FLATPAK_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(sheet.window), "security-high");
    gtk_window_set_default_size(GTK_WINDOW(sheet.window), 560, -1);
    gtk_dialog_add_button(GTK_DIALOG(sheet.window), "Close", GTK_RESPONSE_CLOSE);
    gtk_dialog_set_default_response(GTK_DIALOG(sheet.window), GTK_RESPONSE_CLOSE);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), apps_page(&sheet),
                             gtk_label_new_with_mnemonic("_Applications"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), xd_fp_remotes_page(sheet.window),
                             gtk_label_new_with_mnemonic("Re_motes"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(sheet.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(sheet.window, GTK_NOTEBOOK(tabs));
    g_signal_connect(sheet.window, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    g_signal_connect(sheet.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(sheet.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs),
                                  CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(tabs)) - 1));
    gtk_widget_grab_focus(sheet.view);
    gtk_main();
    if (sheet.list != nullptr)
        g_ptr_array_unref(sheet.list);
    return EXIT_SUCCESS;
}
