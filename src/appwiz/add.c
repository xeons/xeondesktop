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
    result *r = data;
    g_free(r->name);
    g_free(r->summary);
    g_free(r->package_id);
    g_free(r->installation);
    g_free(r->remote);
    g_free(r->ref);
    g_free(r);
}

/* What's being searched for, so it can be searched again after an
 * install. */
static char *last_query;
static PkGroupEnum last_group;

/* ---- results ------------------------------------------------------------------ */

static void show_installed(result *r) {
    gtk_widget_set_visible(r->button, !r->installed);
    gtk_widget_set_visible(r->done, r->installed);
}

static void run_search(appwiz *w);

static void after_install(appwiz *w, bool ok, [[maybe_unused]] gpointer data) {
    if (!ok)
        return;
    xd_appwiz_reload(w);
    run_search(w);
}

static void on_install(GtkButton *b, gpointer data) {
    appwiz *w = data;
    result *r = g_object_get_data(G_OBJECT(b), "result");
    if (r->flatpak)
        xd_appwiz_install_flatpak(w, r->name, r->installation, r->remote, r->ref, after_install,
                                  nullptr);
    else
        xd_appwiz_install_package(w, r->name, r->package_id, after_install, nullptr);
}

static int sort_results(GtkListBoxRow *a, GtkListBoxRow *b, [[maybe_unused]] gpointer data) {
    result *x = g_object_get_data(G_OBJECT(a), "result");
    result *y = g_object_get_data(G_OBJECT(b), "result");
    if (x == nullptr || y == nullptr)
        return 0;
    /* What was typed exactly comes first. */
    const char *q = last_query != nullptr ? last_query : "";
    bool ex = g_ascii_strcasecmp(x->name, q) == 0, ey = g_ascii_strcasecmp(y->name, q) == 0;
    if (ex != ey)
        return ex ? -1 : 1;
    return g_ascii_strcasecmp(x->name, y->name);
}

static void set_results_status(appwiz *w) {
    const char *text = nullptr;
    char *owned = nullptr;
    if (w->searching > 0)
        text = "Searching...";
    else if (w->result_count >= MAX_RESULTS)
        text = owned = g_strdup_printf("Only the first %d programs are shown. Type more of the "
                                       "name to find the one you want.",
                                       MAX_RESULTS);
    else if (w->result_count == 0 && (last_query != nullptr || last_group != PK_GROUP_ENUM_UNKNOWN))
        text = "No programs were found.";
    gtk_label_set_text(GTK_LABEL(w->results_status), text != nullptr ? text : "");
    gtk_widget_set_visible(w->results_status, text != nullptr);
    g_free(owned);
}

static void add_result(appwiz *w, result *r) {
    /* PackageKit lists an installed package and the one in the sources. */
    result *old = g_hash_table_lookup(w->result_rows, r->name);
    if (old != nullptr) {
        old->installed = old->installed || r->installed;
        show_installed(old);
        result_free(r);
        return;
    }
    if (w->result_count >= MAX_RESULTS) {
        result_free(r);
        return;
    }
    w->result_count++;
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 4);
    GtkWidget *icon = gtk_image_new_from_icon_name(r->flatpak ? "application-x-executable"
                                                              : "package-x-generic",
                                                   GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_grid_attach(GTK_GRID(grid), icon, 0, 0, 1, 2);
    char *markup = g_markup_printf_escaped("<b>%s</b>", r->name);
    GtkWidget *name = xd_appwiz_label("", nullptr);
    gtk_label_set_markup(GTK_LABEL(name), markup);
    g_free(markup);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_grid_attach(GTK_GRID(grid), name, 1, 0, 1, 1);
    GtkWidget *summary = xd_appwiz_label(r->summary, "xd-arp-dim");
    gtk_label_set_line_wrap(GTK_LABEL(summary), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
    gtk_grid_attach(GTK_GRID(grid), summary, 1, 1, 1, 1);
    r->button = gtk_button_new_with_mnemonic("_Install");
    gtk_widget_set_size_request(r->button, 80, -1);
    gtk_widget_set_valign(r->button, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(r->button), "result", r);
    g_signal_connect(r->button, "clicked", G_CALLBACK(on_install), w);
    r->done = gtk_label_new("Installed");
    xd_appwiz_add_class(r->done, "xd-arp-dim");
    gtk_widget_set_no_show_all(r->button, TRUE);
    gtk_widget_set_no_show_all(r->done, TRUE);
    gtk_grid_attach(GTK_GRID(grid), r->button, 2, 0, 1, 2);
    gtk_grid_attach(GTK_GRID(grid), r->done, 3, 0, 1, 2);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), grid);
    g_object_set_data_full(G_OBJECT(row), "result", r, result_free);
    g_hash_table_insert(w->result_rows, g_strdup(r->name), r);
    gtk_container_add(GTK_CONTAINER(w->results), row);
    gtk_widget_show_all(row);
    show_installed(r);
}

/* ---- PackageKit ----------------------------------------------------------------- */

typedef struct search {
    appwiz *w;
    unsigned generation;
} search;

static search *new_search(appwiz *w) {
    search *s = g_new0(search, 1);
    *s = (search){w, w->search_generation};
    w->searching++;
    return s;
}

/* Whether the search is still the one shown. */
static bool search_done(search *s) {
    appwiz *w = s->w;
    bool current = s->generation == w->search_generation;
    g_free(s);
    if (current) {
        w->searching--;
        set_results_status(w);
    }
    return current;
}

static void on_pk_found(GObject *src, GAsyncResult *res, gpointer data) {
    search *s = data;
    appwiz *w = s->w;
    GError *error = nullptr;
    PkResults *r = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        g_free(s);
        return;
    }
    unsigned generation = s->generation;
    bool current = generation == w->search_generation;
    if (current && r != nullptr) {
        GPtrArray *pkgs = pk_results_get_package_array(r);
        for (guint i = 0; i < pkgs->len; i++) {
            PkPackage *p = g_ptr_array_index(pkgs, i);
            result *x = g_new0(result, 1);
            x->name = g_strdup(pk_package_get_name(p));
            x->summary = g_strdup(pk_package_get_summary(p));
            x->package_id = g_strdup(pk_package_get_id(p));
            x->installed = pk_package_get_info(p) == PK_INFO_ENUM_INSTALLED ||
                           xd_appwiz_package_installed(x->package_id);
            add_result(w, x);
        }
        g_ptr_array_unref(pkgs);
    } else if (current && error != nullptr) {
        g_printerr("xd-appwiz: PackageKit search: %s\n", error->message);
        gtk_label_set_text(GTK_LABEL(w->results_title),
                           "The software sources can't be searched: PackageKit isn't available.");
    }
    g_clear_error(&error);
    g_clear_object(&r);
    search_done(s);
}

/* ---- Flatpak ---------------------------------------------------------------------- */

typedef struct fp_query {
    char *text; /* casefolded */
} fp_query;

static void fp_query_free(gpointer data) {
    fp_query *q = data;
    g_free(q->text);
    g_free(q);
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
        result *r = g_new0(result, 1);
        r->flatpak = true;
        r->name = g_strdup(flatpak_ref_get_name(ref));
        char *size = xd_appwiz_format_size(flatpak_remote_ref_get_installed_size(rr));
        r->summary = g_strdup_printf("Flatpak from %s, %s", name, size);
        g_free(size);
        r->installation = g_strdup(flatpak_installation_get_id(inst));
        r->remote = g_strdup(name);
        r->ref = flatpak_ref_format_ref(ref);
        r->installed = g_hash_table_contains(installed, r->name);
        g_ptr_array_add(out, r);
    }
    if (refs != nullptr)
        g_ptr_array_unref(refs);
}

static void fp_search_thread(GTask *task, [[maybe_unused]] gpointer source, gpointer data,
                             GCancellable *cancel) {
    fp_query *q = data;
    GPtrArray *out = g_ptr_array_new_with_free_func(result_free);
    GPtrArray *all = xd_appwiz_installations(cancel);
    for (guint i = 0; i < all->len; i++) {
        FlatpakInstallation *inst = g_ptr_array_index(all, i);
        GPtrArray *remotes = flatpak_installation_list_remotes(inst, cancel, nullptr);
        GHashTable *installed = installed_apps(inst, cancel);
        for (guint j = 0; remotes != nullptr && j < remotes->len; j++) {
            FlatpakRemote *remote = g_ptr_array_index(remotes, j);
            if (!flatpak_remote_get_disabled(remote) && !flatpak_remote_get_noenumerate(remote))
                search_remote(inst, remote, q->text, installed, out, cancel);
        }
        g_hash_table_unref(installed);
        if (remotes != nullptr)
            g_ptr_array_unref(remotes);
    }
    g_ptr_array_unref(all);
    g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
}

static void on_fp_found([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    search *s = data;
    appwiz *w = s->w;
    GPtrArray *hits = g_task_propagate_pointer(G_TASK(res), nullptr);
    if (hits != nullptr && s->generation == w->search_generation) {
        /* The rows take the results. */
        g_ptr_array_set_free_func(hits, nullptr);
        for (guint i = 0; i < hits->len; i++)
            add_result(w, g_ptr_array_index(hits, i));
    }
    if (hits != nullptr)
        g_ptr_array_unref(hits);
    search_done(s);
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

static const char *group_name(PkGroupEnum g) {
    for (size_t i = 0; i < XD_ARRAY_LEN(groups); i++)
        if (groups[i].group == g)
            return groups[i].name;
    return pk_group_enum_to_string(g);
}

static void clear_results(appwiz *w) {
    w->search_generation++;
    w->searching = 0;
    w->result_count = 0;
    g_hash_table_remove_all(w->result_rows);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(w->results));
    for (GList *k = kids; k != nullptr; k = k->next)
        gtk_widget_destroy(k->data);
    g_list_free(kids);
}

static void run_search(appwiz *w) {
    clear_results(w);
    char *title;
    if (last_group != PK_GROUP_ENUM_UNKNOWN) {
        title = g_strdup_printf("Programs in %s:", group_name(last_group));
        const char *group = pk_group_enum_to_string(last_group);
        char *values[] = {(char *)(uintptr_t)group, nullptr};
        pk_client_search_groups_async(w->pk, pk_bitfield_value(PK_FILTER_ENUM_NONE), values,
                                      w->quit, nullptr, nullptr, on_pk_found, new_search(w));
    } else if (last_query != nullptr) {
        title = g_strdup_printf("Programs matching \"%s\":", last_query);
        char *values[] = {last_query, nullptr};
        pk_client_search_names_async(w->pk, pk_bitfield_value(PK_FILTER_ENUM_NONE), values,
                                     w->quit, nullptr, nullptr, on_pk_found, new_search(w));
        fp_query *q = g_new0(fp_query, 1);
        q->text = g_utf8_casefold(last_query, -1);
        GTask *task = g_task_new(nullptr, w->quit, on_fp_found, new_search(w));
        g_task_set_task_data(task, q, fp_query_free);
        g_task_run_in_thread(task, fp_search_thread);
        g_object_unref(task);
    } else {
        return;
    }
    gtk_label_set_text(GTK_LABEL(w->results_title), title);
    gtk_widget_show(w->results_title);
    g_free(title);
    set_results_status(w);
}

static void on_search([[maybe_unused]] GtkWidget *widget, gpointer data) {
    appwiz *w = data;
    char *q = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(w->search))));
    if (q[0] == '\0') {
        g_free(q);
        return;
    }
    g_free(last_query);
    last_query = q;
    last_group = PK_GROUP_ENUM_UNKNOWN;
    run_search(w);
}

/* ---- package files ---------------------------------------------------------------------- */

static void after_file(appwiz *w, bool ok, [[maybe_unused]] gpointer data) {
    if (ok) {
        xd_appwiz_reload(w);
        xd_message_box(GTK_WINDOW(w->window), XD_APPWIZ_TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       "The program was installed. You'll find it on the Start menu.");
    }
}

static void on_browse([[maybe_unused]] GtkButton *b, gpointer data) {
    appwiz *w = data;
    GtkWidget *d = gtk_file_chooser_dialog_new("Install a Program", GTK_WINDOW(w->window),
                                               GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
                                               GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT,
                                               nullptr);
    GtkFileFilter *programs = gtk_file_filter_new();
    gtk_file_filter_set_name(programs, "Programs (packages and Flatpaks)");
    static const char *const patterns[] = {"*.pkg.tar.zst", "*.pkg.tar.xz", "*.deb", "*.rpm",
                                           "*.flatpakref", "*.flatpak"};
    for (size_t i = 0; i < XD_ARRAY_LEN(patterns); i++)
        gtk_file_filter_add_pattern(programs, patterns[i]);
    /* And whatever else PackageKit's backend says it installs. */
    char **types = nullptr;
    g_object_get(w->control, "mime-types", &types, nullptr);
    for (char **t = types; t != nullptr && *t != nullptr; t++)
        gtk_file_filter_add_mime_type(programs, *t);
    g_strfreev(types);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), programs);
    GtkFileFilter *any = gtk_file_filter_new();
    gtk_file_filter_set_name(any, "All Files");
    gtk_file_filter_add_pattern(any, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), any);
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (downloads != nullptr)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), downloads);
    char *file = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT
                     ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d))
                     : nullptr;
    gtk_widget_destroy(d);
    if (file != nullptr)
        xd_appwiz_install_file(w, file, after_file, nullptr);
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

GtkWidget *xd_appwiz_add_page(appwiz *w) {
    w->result_rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
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
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), w);
    gtk_box_pack_end(GTK_BOX(file), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), file, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);

    GtkWidget *sources = section("system-software-update", "Add programs from your software sources",
                                 "Search your distribution's software sources and Flatpak "
                                 "remotes for new programs and features.",
                                 &col);
    GtkWidget *find = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    w->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->search), "Program name");
    gtk_widget_set_hexpand(w->search, TRUE);
    g_signal_connect(w->search, "activate", G_CALLBACK(on_search), w);
    gtk_box_pack_start(GTK_BOX(find), w->search, TRUE, TRUE, 0);
    GtkWidget *go = gtk_button_new_with_mnemonic("_Search");
    gtk_widget_set_size_request(go, 90, -1);
    g_signal_connect(go, "clicked", G_CALLBACK(on_search), w);
    gtk_box_pack_start(GTK_BOX(find), go, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), find, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sources, FALSE, FALSE, 0);

    w->results_title = xd_appwiz_label("", "xd-arp-heading");
    gtk_widget_set_no_show_all(w->results_title, TRUE);
    gtk_box_pack_start(GTK_BOX(box), w->results_title, FALSE, FALSE, 0);
    w->results = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(w->results), GTK_SELECTION_SINGLE);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(w->results), sort_results, w, nullptr);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), w->results);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    w->results_status = xd_appwiz_label("", "xd-arp-dim");
    gtk_widget_set_no_show_all(w->results_status, TRUE);
    gtk_box_pack_start(GTK_BOX(box), w->results_status, FALSE, FALSE, 0);
    return box;
}

/* ---- Windows Components ------------------------------------------------------------------ */

static void on_group(appwiz *w) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(w->groups));
    if (row == nullptr)
        return;
    gpointer group = g_object_get_data(G_OBJECT(row), "group");
    last_group = (PkGroupEnum)GPOINTER_TO_INT(group);
    g_clear_pointer(&last_query, g_free);
    gtk_entry_set_text(GTK_ENTRY(w->search), "");
    xd_appwiz_show_page(w, XD_PAGE_ADD);
    run_search(w);
}

static void on_group_activated([[maybe_unused]] GtkListBox *box,
                               [[maybe_unused]] GtkListBoxRow *row, gpointer data) {
    on_group(data);
}

static void on_properties(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    if (!pk_control_get_properties_finish(PK_CONTROL(src), res, &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            appwiz *w = data;
            gtk_label_set_text(GTK_LABEL(w->group_note),
                               "Components can't be shown: PackageKit isn't available.");
        }
        g_error_free(error);
        return;
    }
    appwiz *w = data;
    xd_appwiz_sources_changed(w);
    PkBitfield have = 0;
    g_object_get(w->control, "groups", &have, nullptr);
    int n = 0;
    for (size_t i = 0; i < XD_ARRAY_LEN(groups); i++) {
        if (!pk_bitfield_contain(have, groups[i].group))
            continue;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(row), 3);
        GtkWidget *img = gtk_image_new_from_icon_name(groups[i].icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), xd_appwiz_label(groups[i].name, nullptr), TRUE, TRUE, 0);
        GtkWidget *r = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(r), row);
        g_object_set_data(G_OBJECT(r), "group", GINT_TO_POINTER((int)groups[i].group));
        gtk_container_add(GTK_CONTAINER(w->groups), r);
        n++;
    }
    gtk_widget_show_all(w->groups);
    if (n == 0)
        gtk_label_set_text(GTK_LABEL(w->group_note),
                           "Your software sources don't sort programs into groups.");
    else
        gtk_widget_hide(w->group_note);
}

GtkWidget *xd_appwiz_components_page(appwiz *w) {
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
    w->groups = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(w->groups), FALSE);
    g_signal_connect(w->groups, "row-activated", G_CALLBACK(on_group_activated), w);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), w->groups);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    w->group_note = xd_appwiz_label("Loading...", "xd-arp-dim");
    gtk_box_pack_start(GTK_BOX(box), w->group_note, FALSE, FALSE, 0);
    GtkWidget *details = gtk_button_new_with_mnemonic("_Details...");
    gtk_widget_set_size_request(details, 90, -1);
    gtk_widget_set_halign(details, GTK_ALIGN_END);
    g_signal_connect_swapped(details, "clicked", G_CALLBACK(on_group), w);
    gtk_box_pack_start(GTK_BOX(box), details, FALSE, FALSE, 0);
    pk_control_get_properties_async(w->control, w->quit, on_properties, w);
    return box;
}
