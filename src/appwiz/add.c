/* SPDX-License-Identifier: MIT
 * Add New Programs and Add/Remove Windows Components, for Add or Remove
 * Programs. XP offered "Add a program from CD-ROM or floppy disk" and
 * Windows Update; here it's a package file, or a search of the software
 * sources (PackageKit) and Flatpak remotes. Windows Components are
 * PackageKit's package groups.
 */
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "appwiz.h"

/* Searches that match half the distribution stop here. */
static constexpr int MAX_RESULTS = 200;

/* A program that can be installed. */
typedef struct result {
    char *name, *summary;
    bool flatpak, installed;
    char *package_id;                 /* PackageKit */
    char *installation, *remote, *ref; /* Flatpak */
    GtkWidget *button, *done;
} result;

static void result_free(gpointer data) {
    result *result_entry = data;
    g_free(result_entry->name);
    g_free(result_entry->summary);
    g_free(result_entry->package_id);
    g_free(result_entry->installation);
    g_free(result_entry->remote);
    g_free(result_entry->ref);
    g_free(result_entry);
}

/* What's being searched for, so it can be searched again after an
 * install. */
static char *last_query;
static PkGroupEnum last_group;

/* ---- results ------------------------------------------------------------------ */

static void show_installed(result *result_entry) {
    gtk_widget_set_visible(result_entry->button, !result_entry->installed);
    gtk_widget_set_visible(result_entry->done, result_entry->installed);
}

static void run_search(appwiz *state);

static void after_install(appwiz *state, bool ok, [[maybe_unused]] gpointer data) {
    if (!ok)
        return;
    xd_appwiz_reload(state);
    run_search(state);
}

static void on_install(GtkButton *button, gpointer data) {
    appwiz *state = data;
    result *result_entry = g_object_get_data(G_OBJECT(button), "result");
    if (result_entry->flatpak)
        xd_appwiz_install_flatpak(state, result_entry->name, result_entry->installation,
                                  result_entry->remote, result_entry->ref, after_install, nullptr);
    else
        xd_appwiz_install_package(state, result_entry->name, result_entry->package_id,
                                  after_install, nullptr);
}

static int sort_results(GtkListBoxRow *left_row, GtkListBoxRow *right_row,
                        [[maybe_unused]] gpointer data) {
    result *left = g_object_get_data(G_OBJECT(left_row), "result");
    result *right = g_object_get_data(G_OBJECT(right_row), "result");
    if (left == nullptr || right == nullptr)
        return 0;
    /* What was typed exactly comes first. */
    const char *query = last_query != nullptr ? last_query : "";
    bool ex = g_ascii_strcasecmp(left->name, query) == 0,
         ey = g_ascii_strcasecmp(right->name, query) == 0;
    if (ex != ey)
        return ex ? -1 : 1;
    return g_ascii_strcasecmp(left->name, right->name);
}

static void set_results_status(appwiz *state) {
    const char *text = nullptr;
    char *owned = nullptr;
    if (state->searching > 0)
        text = "Searching...";
    else if (state->result_count >= MAX_RESULTS)
        text = owned = g_strdup_printf("Only the first %d programs are shown. Type more of the "
                                       "name to find the one you want.",
                                       MAX_RESULTS);
    else if (state->result_count == 0 &&
             (last_query != nullptr || last_group != PK_GROUP_ENUM_UNKNOWN))
        text = "No programs were found.";
    gtk_label_set_text(GTK_LABEL(state->results_status), text != nullptr ? text : "");
    gtk_widget_set_visible(state->results_status, text != nullptr);
    g_free(owned);
}

static void add_result(appwiz *state, result *result_entry) {
    /* PackageKit lists an installed package and the one in the sources. */
    result *old = g_hash_table_lookup(state->result_rows, result_entry->name);
    if (old != nullptr) {
        old->installed = old->installed || result_entry->installed;
        show_installed(old);
        result_free(result_entry);
        return;
    }
    if (state->result_count >= MAX_RESULTS) {
        result_free(result_entry);
        return;
    }
    state->result_count++;
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 4);
    GtkWidget *icon = gtk_image_new_from_icon_name(
        result_entry->flatpak ? "application-x-executable" : "package-x-generic",
        GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_grid_attach(GTK_GRID(grid), icon, 0, 0, 1, 2);
    char *markup = g_markup_printf_escaped("<b>%s</b>", result_entry->name);
    GtkWidget *name = xd_appwiz_label("", nullptr);
    gtk_label_set_markup(GTK_LABEL(name), markup);
    g_free(markup);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_grid_attach(GTK_GRID(grid), name, 1, 0, 1, 1);
    GtkWidget *summary = xd_appwiz_label(result_entry->summary, "xd-arp-dim");
    gtk_label_set_line_wrap(GTK_LABEL(summary), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
    gtk_grid_attach(GTK_GRID(grid), summary, 1, 1, 1, 1);
    result_entry->button = gtk_button_new_with_mnemonic("_Install");
    gtk_widget_set_size_request(result_entry->button, 80, -1);
    gtk_widget_set_valign(result_entry->button, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(result_entry->button), "result", result_entry);
    g_signal_connect(result_entry->button, "clicked", G_CALLBACK(on_install), state);
    result_entry->done = gtk_label_new("Installed");
    xd_appwiz_add_class(result_entry->done, "xd-arp-dim");
    gtk_widget_set_no_show_all(result_entry->button, TRUE);
    gtk_widget_set_no_show_all(result_entry->done, TRUE);
    gtk_grid_attach(GTK_GRID(grid), result_entry->button, 2, 0, 1, 2);
    gtk_grid_attach(GTK_GRID(grid), result_entry->done, 3, 0, 1, 2);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), grid);
    g_object_set_data_full(G_OBJECT(row), "result", result_entry, result_free);
    g_hash_table_insert(state->result_rows, g_strdup(result_entry->name), result_entry);
    gtk_container_add(GTK_CONTAINER(state->results), row);
    gtk_widget_show_all(row);
    show_installed(result_entry);
}

/* ---- PackageKit ----------------------------------------------------------------- */

typedef struct search {
    appwiz *w;
    unsigned generation;
} search;

static search *new_search(appwiz *state) {
    search *pending_search = g_new0(search, 1);
    *pending_search = (search){state, state->search_generation};
    state->searching++;
    return pending_search;
}

/* Whether the search is still the one shown. */
static bool search_done(search *pending_search) {
    appwiz *state = pending_search->w;
    bool current = pending_search->generation == state->search_generation;
    g_free(pending_search);
    if (current) {
        state->searching--;
        set_results_status(state);
    }
    return current;
}

static void on_pk_found(GObject *src, GAsyncResult *res, gpointer data) {
    search *pending_search = data;
    appwiz *state = pending_search->w;
    GError *error = nullptr;
    PkResults *results = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        g_free(pending_search);
        return;
    }
    unsigned generation = pending_search->generation;
    bool current = generation == state->search_generation;
    if (current && results != nullptr) {
        GPtrArray *pkgs = pk_results_get_package_array(results);
        for (guint i = 0; i < pkgs->len; i++) {
            PkPackage *package = g_ptr_array_index(pkgs, i);
            result *package_result = g_new0(result, 1);
            package_result->name = g_strdup(pk_package_get_name(package));
            package_result->summary = g_strdup(pk_package_get_summary(package));
            package_result->package_id = g_strdup(pk_package_get_id(package));
            package_result->installed = pk_package_get_info(package) == PK_INFO_ENUM_INSTALLED ||
                                        xd_appwiz_package_installed(package_result->package_id);
            add_result(state, package_result);
        }
        g_ptr_array_unref(pkgs);
    } else if (current && error != nullptr) {
        g_printerr("xd-appwiz: PackageKit search: %s\n", error->message);
        gtk_label_set_text(GTK_LABEL(state->results_title),
                           "The software sources can't be searched: PackageKit isn't available.");
    }
    g_clear_error(&error);
    g_clear_object(&results);
    search_done(pending_search);
}

/* ---- Flatpak ---------------------------------------------------------------------- */

typedef struct fp_query {
    char *text; /* casefolded */
} fp_query;

static void fp_query_free(gpointer data) {
    fp_query *query = data;
    g_free(query->text);
    g_free(query);
}

static GHashTable *installed_apps(FlatpakInstallation *inst, GCancellable *cancel) {
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GPtrArray *refs = flatpak_installation_list_installed_refs_by_kind(inst, FLATPAK_REF_KIND_APP,
                                                                       cancel, nullptr);
    for (guint i = 0; refs != nullptr && i < refs->len; i++)
        g_hash_table_add(names, g_strdup(flatpak_ref_get_name(g_ptr_array_index(refs, i))));
    if (refs != nullptr)
        g_ptr_array_unref(refs);
    return names;
}

static void search_remote(FlatpakInstallation *inst, FlatpakRemote *remote, const char *text,
                          GHashTable *installed, GPtrArray *out, GCancellable *cancel) {
    const char *name = flatpak_remote_get_name(remote);
    /* What's cached first; the network only if there's nothing. */
    GPtrArray *refs = flatpak_installation_list_remote_refs_sync_full(
        inst, name, FLATPAK_QUERY_FLAGS_ONLY_CACHED, cancel, nullptr);
    if (refs == nullptr || refs->len == 0) {
        if (refs != nullptr)
            g_ptr_array_unref(refs);
        refs = flatpak_installation_list_remote_refs_sync_full(inst, name, FLATPAK_QUERY_FLAGS_NONE,
                                                               cancel, nullptr);
    }
    const char *arch = flatpak_get_default_arch();
    for (guint i = 0; refs != nullptr && i < refs->len; i++) {
        FlatpakRemoteRef *rr = g_ptr_array_index(refs, i);
        FlatpakRef *ref = FLATPAK_REF(rr);
        if (flatpak_ref_get_kind(ref) != FLATPAK_REF_KIND_APP ||
            g_strcmp0(flatpak_ref_get_arch(ref), arch) != 0)
            continue;
        char *folded = g_utf8_casefold(flatpak_ref_get_name(ref), -1);
        bool match = strstr(folded, text) != nullptr;
        g_free(folded);
        if (!match)
            continue;
        result *result_entry = g_new0(result, 1);
        result_entry->flatpak = true;
        result_entry->name = g_strdup(flatpak_ref_get_name(ref));
        char *size = xd_appwiz_format_size(flatpak_remote_ref_get_installed_size(rr));
        result_entry->summary = g_strdup_printf("Flatpak from %s, %s", name, size);
        g_free(size);
        result_entry->installation = g_strdup(flatpak_installation_get_id(inst));
        result_entry->remote = g_strdup(name);
        result_entry->ref = flatpak_ref_format_ref(ref);
        result_entry->installed = g_hash_table_contains(installed, result_entry->name);
        g_ptr_array_add(out, result_entry);
    }
    if (refs != nullptr)
        g_ptr_array_unref(refs);
}

static void fp_search_thread(GTask *task, [[maybe_unused]] gpointer source, gpointer data,
                             GCancellable *cancel) {
    fp_query *query = data;
    GPtrArray *out = g_ptr_array_new_with_free_func(result_free);
    GPtrArray *all = xd_appwiz_installations(cancel);
    for (guint i = 0; i < all->len; i++) {
        FlatpakInstallation *inst = g_ptr_array_index(all, i);
        GPtrArray *remotes = flatpak_installation_list_remotes(inst, cancel, nullptr);
        GHashTable *installed = installed_apps(inst, cancel);
        for (guint j = 0; remotes != nullptr && j < remotes->len; j++) {
            FlatpakRemote *remote = g_ptr_array_index(remotes, j);
            if (!flatpak_remote_get_disabled(remote) && !flatpak_remote_get_noenumerate(remote))
                search_remote(inst, remote, query->text, installed, out, cancel);
        }
        g_hash_table_unref(installed);
        if (remotes != nullptr)
            g_ptr_array_unref(remotes);
    }
    g_ptr_array_unref(all);
    g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
}

static void on_fp_found([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    search *pending_search = data;
    appwiz *state = pending_search->w;
    GPtrArray *hits = g_task_propagate_pointer(G_TASK(res), nullptr);
    if (hits != nullptr && pending_search->generation == state->search_generation) {
        /* The rows take the results. */
        g_ptr_array_set_free_func(hits, nullptr);
        for (guint i = 0; i < hits->len; i++)
            add_result(state, g_ptr_array_index(hits, i));
    }
    if (hits != nullptr)
        g_ptr_array_unref(hits);
    search_done(pending_search);
}

/* ---- searching ------------------------------------------------------------------------- */

static const struct {
    PkGroupEnum group;
    const char *name, *icon;
} groups[] = {
    {PK_GROUP_ENUM_ACCESSIBILITY, "Accessibility", "preferences-desktop-accessibility"},
    {PK_GROUP_ENUM_ACCESSORIES, "Accessories and Utilities", "applications-accessories"},
    {PK_GROUP_ENUM_ADMIN_TOOLS, "Administrative Tools", "applications-system"},
    {PK_GROUP_ENUM_COMMUNICATION, "Communication", "internet-mail"},
    {PK_GROUP_ENUM_DESKTOP_GNOME, "GNOME Desktop", "user-desktop"},
    {PK_GROUP_ENUM_DESKTOP_KDE, "KDE Desktop", "user-desktop"},
    {PK_GROUP_ENUM_DESKTOP_OTHER, "Other Desktops", "user-desktop"},
    {PK_GROUP_ENUM_DESKTOP_XFCE, "Xfce Desktop", "user-desktop"},
    {PK_GROUP_ENUM_DESKTOP_DDE, "Deepin Desktop", "user-desktop"},
    {PK_GROUP_ENUM_EDUCATION, "Education", "applications-science"},
    {PK_GROUP_ENUM_FONTS, "Fonts", "preferences-desktop-font"},
    {PK_GROUP_ENUM_GAMES, "Games", "applications-games"},
    {PK_GROUP_ENUM_GRAPHICS, "Graphics", "applications-graphics"},
    {PK_GROUP_ENUM_INTERNET, "Internet", "applications-internet"},
    {PK_GROUP_ENUM_LEGACY, "Legacy", "applications-other"},
    {PK_GROUP_ENUM_LOCALIZATION, "Languages", "preferences-desktop-locale"},
    {PK_GROUP_ENUM_MAPS, "Maps", "applications-other"},
    {PK_GROUP_ENUM_MULTIMEDIA, "Multimedia", "applications-multimedia"},
    {PK_GROUP_ENUM_NETWORK, "Networking Services", "network-workgroup"},
    {PK_GROUP_ENUM_OFFICE, "Office", "applications-office"},
    {PK_GROUP_ENUM_OTHER, "Other", "applications-other"},
    {PK_GROUP_ENUM_POWER_MANAGEMENT, "Power Management", "battery"},
    {PK_GROUP_ENUM_PROGRAMMING, "Programming", "applications-development"},
    {PK_GROUP_ENUM_PUBLISHING, "Publishing", "applications-office"},
    {PK_GROUP_ENUM_REPOS, "Software Sources", "system-software-install"},
    {PK_GROUP_ENUM_SECURITY, "Security", "security-high"},
    {PK_GROUP_ENUM_SERVERS, "Servers", "network-server"},
    {PK_GROUP_ENUM_SYSTEM, "System", "applications-system"},
    {PK_GROUP_ENUM_VIRTUALIZATION, "Virtualization", "computer"},
    {PK_GROUP_ENUM_SCIENCE, "Science", "applications-science"},
    {PK_GROUP_ENUM_DOCUMENTATION, "Documentation", "help-browser"},
    {PK_GROUP_ENUM_ELECTRONICS, "Electronics", "applications-engineering"},
    {PK_GROUP_ENUM_COLLECTIONS, "Collections", "package-x-generic"},
    {PK_GROUP_ENUM_VENDOR, "Vendor", "package-x-generic"},
};

static const char *group_name(PkGroupEnum pk_group) {
    for (size_t i = 0; i < XD_ARRAY_LEN(groups); i++)
        if (groups[i].group == pk_group)
            return groups[i].name;
    return pk_group_enum_to_string(pk_group);
}

static void clear_results(appwiz *state) {
    state->search_generation++;
    state->searching = 0;
    state->result_count = 0;
    g_hash_table_remove_all(state->result_rows);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(state->results));
    for (GList *child = kids; child != nullptr; child = child->next)
        gtk_widget_destroy(child->data);
    g_list_free(kids);
}

static void run_search(appwiz *state) {
    clear_results(state);
    char *title;
    if (last_group != PK_GROUP_ENUM_UNKNOWN) {
        title = g_strdup_printf("Programs in %s:", group_name(last_group));
        const char *group = pk_group_enum_to_string(last_group);
        char *values[] = {(char *)(uintptr_t)group, nullptr};
        pk_client_search_groups_async(state->pk, pk_bitfield_value(PK_FILTER_ENUM_NONE), values,
                                      state->quit, nullptr, nullptr, on_pk_found,
                                      new_search(state));
    } else if (last_query != nullptr) {
        title = g_strdup_printf("Programs matching \"%s\":", last_query);
        char *values[] = {last_query, nullptr};
        pk_client_search_names_async(state->pk, pk_bitfield_value(PK_FILTER_ENUM_NONE), values,
                                     state->quit, nullptr, nullptr, on_pk_found, new_search(state));
        fp_query *query = g_new0(fp_query, 1);
        query->text = g_utf8_casefold(last_query, -1);
        GTask *task = g_task_new(nullptr, state->quit, on_fp_found, new_search(state));
        g_task_set_task_data(task, query, fp_query_free);
        g_task_run_in_thread(task, fp_search_thread);
        g_object_unref(task);
    } else {
        return;
    }
    gtk_label_set_text(GTK_LABEL(state->results_title), title);
    gtk_widget_show(state->results_title);
    g_free(title);
    set_results_status(state);
}

static void on_search([[maybe_unused]] GtkWidget *widget, gpointer data) {
    appwiz *state = data;
    char *query = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(state->search))));
    if (query[0] == '\0') {
        g_free(query);
        return;
    }
    g_free(last_query);
    last_query = query;
    last_group = PK_GROUP_ENUM_UNKNOWN;
    run_search(state);
}

/* ---- package files ---------------------------------------------------------------------- */

static void after_file(appwiz *state, bool ok, [[maybe_unused]] gpointer data) {
    if (ok) {
        xd_appwiz_reload(state);
        xd_message_box(GTK_WINDOW(state->window), XD_APPWIZ_TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       "The program was installed. You'll find it on the Start menu.");
    }
}

static void on_browse([[maybe_unused]] GtkButton *button, gpointer data) {
    appwiz *state = data;
    GtkWidget *file_chooser = gtk_file_chooser_dialog_new(
        "Install a Program", GTK_WINDOW(state->window), GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
        GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter *programs = gtk_file_filter_new();
    gtk_file_filter_set_name(programs, "Programs (packages and Flatpaks)");
    static const char *const patterns[] = {"*.pkg.tar.zst", "*.pkg.tar.xz", "*.deb", "*.rpm",
                                           "*.flatpakref", "*.flatpak"};
    for (size_t i = 0; i < XD_ARRAY_LEN(patterns); i++)
        gtk_file_filter_add_pattern(programs, patterns[i]);
    /* And whatever else PackageKit's backend says it installs. */
    char **types = nullptr;
    g_object_get(state->control, "mime-types", &types, nullptr);
    for (char **mime_type = types; mime_type != nullptr && *mime_type != nullptr; mime_type++)
        gtk_file_filter_add_mime_type(programs, *mime_type);
    g_strfreev(types);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), programs);
    GtkFileFilter *any = gtk_file_filter_new();
    gtk_file_filter_set_name(any, "All Files");
    gtk_file_filter_add_pattern(any, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), any);
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (downloads != nullptr)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(file_chooser), downloads);
    char *file = gtk_dialog_run(GTK_DIALOG(file_chooser)) == GTK_RESPONSE_ACCEPT
                     ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(file_chooser))
                     : nullptr;
    gtk_widget_destroy(file_chooser);
    if (file != nullptr)
        xd_appwiz_install_file(state, file, after_file, nullptr);
    g_free(file);
}

/* ---- pages ------------------------------------------------------------------------------ */

/* A big icon, a bold title and text: one of XP's two ways to add. */
static GtkWidget *section(const char *icon, const char *title, const char *text,
                          GtkWidget **content) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(col), xd_appwiz_label(title, "xd-arp-heading"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), xd_appwiz_label(text, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
    *content = col;
    return row;
}

GtkWidget *xd_appwiz_add_page(appwiz *state) {
    state->result_rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *col;
    GtkWidget *file = section("media-optical", "Add a program from a package file",
                              "To install a program you downloaded (a .pkg.tar.zst, .deb or .rpm "
                              "package, or a .flatpakref or .flatpak), click Browse.",
                              &col);
    GtkWidget *browse = gtk_button_new_with_mnemonic("_Browse...");
    gtk_widget_set_size_request(browse, 90, -1);
    gtk_widget_set_valign(browse, GTK_ALIGN_END);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), state);
    gtk_box_pack_end(GTK_BOX(file), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), file, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);

    GtkWidget *sources = section("system-software-update", "Add programs from your software sources",
                                 "Search your distribution's software sources and Flatpak "
                                 "remotes for new programs and features.",
                                 &col);
    GtkWidget *find = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->search), "Program name");
    gtk_widget_set_hexpand(state->search, TRUE);
    g_signal_connect(state->search, "activate", G_CALLBACK(on_search), state);
    gtk_box_pack_start(GTK_BOX(find), state->search, TRUE, TRUE, 0);
    GtkWidget *go = gtk_button_new_with_mnemonic("_Search");
    gtk_widget_set_size_request(go, 90, -1);
    g_signal_connect(go, "clicked", G_CALLBACK(on_search), state);
    gtk_box_pack_start(GTK_BOX(find), go, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), find, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sources, FALSE, FALSE, 0);

    state->results_title = xd_appwiz_label("", "xd-arp-heading");
    gtk_widget_set_no_show_all(state->results_title, TRUE);
    gtk_box_pack_start(GTK_BOX(box), state->results_title, FALSE, FALSE, 0);
    state->results = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->results), GTK_SELECTION_SINGLE);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(state->results), sort_results, state, nullptr);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), state->results);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    state->results_status = xd_appwiz_label("", "xd-arp-dim");
    gtk_widget_set_no_show_all(state->results_status, TRUE);
    gtk_box_pack_start(GTK_BOX(box), state->results_status, FALSE, FALSE, 0);
    return box;
}

/* ---- Windows Components ------------------------------------------------------------------ */

static void on_group(appwiz *state) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->groups));
    if (row == nullptr)
        return;
    gpointer group = g_object_get_data(G_OBJECT(row), "group");
    last_group = (PkGroupEnum)GPOINTER_TO_INT(group);
    g_clear_pointer(&last_query, g_free);
    gtk_entry_set_text(GTK_ENTRY(state->search), "");
    xd_appwiz_show_page(state, XD_PAGE_ADD);
    run_search(state);
}

static void on_group_activated([[maybe_unused]] GtkListBox *box,
                               [[maybe_unused]] GtkListBoxRow *row, gpointer data) {
    on_group(data);
}

static void on_properties(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    if (!pk_control_get_properties_finish(PK_CONTROL(src), res, &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            appwiz *state = data;
            gtk_label_set_text(GTK_LABEL(state->group_note),
                               "Components can't be shown: PackageKit isn't available.");
        }
        g_error_free(error);
        return;
    }
    appwiz *state = data;
    xd_appwiz_sources_changed(state);
    PkBitfield have = 0;
    g_object_get(state->control, "groups", &have, nullptr);
    int count = 0;
    for (size_t i = 0; i < XD_ARRAY_LEN(groups); i++) {
        if (!pk_bitfield_contain(have, groups[i].group))
            continue;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(row), 3);
        GtkWidget *img = gtk_image_new_from_icon_name(groups[i].icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), xd_appwiz_label(groups[i].name, nullptr), TRUE, TRUE, 0);
        GtkWidget *list_row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(list_row), row);
        g_object_set_data(G_OBJECT(list_row), "group", GINT_TO_POINTER((int)groups[i].group));
        gtk_container_add(GTK_CONTAINER(state->groups), list_row);
        count++;
    }
    gtk_widget_show_all(state->groups);
    if (count == 0)
        gtk_label_set_text(GTK_LABEL(state->group_note),
                           "Your software sources don't sort programs into groups.");
    else
        gtk_widget_hide(state->group_note);
}

GtkWidget *xd_appwiz_components_page(appwiz *state) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(box), xd_appwiz_label("Windows Components", "xd-arp-big"), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       xd_appwiz_label("Your software sources sort their programs into groups. "
                                       "Pick a group and click Details to see what's in it and "
                                       "add programs from it.",
                                       nullptr),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), xd_appwiz_label("Components:", "xd-arp-heading"), FALSE,
                       FALSE, 0);
    state->groups = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(state->groups), FALSE);
    g_signal_connect(state->groups, "row-activated", G_CALLBACK(on_group_activated), state);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), state->groups);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    state->group_note = xd_appwiz_label("Loading...", "xd-arp-dim");
    gtk_box_pack_start(GTK_BOX(box), state->group_note, FALSE, FALSE, 0);
    GtkWidget *details = gtk_button_new_with_mnemonic("_Details...");
    gtk_widget_set_size_request(details, 90, -1);
    gtk_widget_set_halign(details, GTK_ALIGN_END);
    g_signal_connect_swapped(details, "clicked", G_CALLBACK(on_group), state);
    gtk_box_pack_start(GTK_BOX(box), details, FALSE, FALSE, 0);
    pk_control_get_properties_async(state->control, state->quit, on_properties, state);
    return box;
}
