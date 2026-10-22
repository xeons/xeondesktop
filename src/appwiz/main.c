/* SPDX-License-Identifier: MIT
 * xd-appwiz: Windows XP's Add or Remove Programs, through PackageKit and
 * Flatpak. A blue bar down the left picks the page:
 *
 *   Change or Remove Programs   the programs in the Start menu, each with
 *                               the package or Flatpak it came from: size,
 *                               how often it's used (the Start menu's
 *                               launch counts), when it was last used
 *                               (started by the shell, or opening a
 *                               document), support information and Remove
 *   Add New Programs            install a package file, or search the
 *                               software sources and Flatpak remotes
 *   Add/Remove Windows          PackageKit's package groups
 *   Components
 *
 * Nothing runs as root: PackageKit and Flatpak's system helper ask polkit,
 * so xd-polkit asks for an administrator's password when needed.
 */
#include <stdlib.h>
#include <string.h>

#include <gio/gdesktopappinfo.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/state.h"
#include "appwiz.h"

/* How many package ids to send to GetDetails at once. */
static constexpr guint DETAILS_BATCH = 16;

typedef enum source {
    SOURCE_NONE, /* the user's own shortcut, or a package manager we can't see */
    SOURCE_PACKAGE,
    SOURCE_FLATPAK,
} source;

/* An installed Flatpak app. */
typedef struct flatpak_app {
    char *installation, *origin, *ref, *version, *summary, *license, *publisher, *homepage;
    guint64 size;
} flatpak_app;

/* A program in the list: a desktop entry and what it came from. */
typedef struct program {
    xd_appwiz_key key; /* key.name is `name` */
    char *id, *name, *path, *exec;
    GIcon *icon;
    source src;
    char *package_id;   /* PackageKit's id, or the Flatpak ref */
    char *installation, *remote; /* Flatpak's */
    char *source;                /* the Source row's text, or nullptr */
    char *version, *publisher, *url, *description, *license;
    GtkWidget *row, *size_key, *size, *source_key, *source_value, *how, *remove;
    GPtrArray *extra; /* shown when selected */
} program;

static void program_free(gpointer data) {
    program *program_entry = data;
    g_free(program_entry->id);
    g_free(program_entry->name);
    g_free(program_entry->path);
    g_free(program_entry->exec);
    g_clear_object(&program_entry->icon);
    g_free(program_entry->package_id);
    g_free(program_entry->installation);
    g_free(program_entry->remote);
    g_free(program_entry->source);
    g_free(program_entry->version);
    g_free(program_entry->publisher);
    g_free(program_entry->url);
    g_free(program_entry->description);
    g_free(program_entry->license);
    if (program_entry->extra != nullptr)
        g_ptr_array_free(program_entry->extra, TRUE);
    g_free(program_entry);
}

static void flatpak_app_free(gpointer data) {
    flatpak_app *installed_app = data;
    g_free(installed_app->installation);
    g_free(installed_app->origin);
    g_free(installed_app->ref);
    g_free(installed_app->version);
    g_free(installed_app->summary);
    g_free(installed_app->license);
    g_free(installed_app->publisher);
    g_free(installed_app->homepage);
    g_free(installed_app);
}

/* ---- helpers --------------------------------------------------------------- */

void xd_appwiz_add_class(GtkWidget *widget, const char *css_class) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), css_class);
}

GtkWidget *xd_appwiz_label(const char *text, const char *css_class) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 70);
    if (css_class != nullptr)
        xd_appwiz_add_class(label, css_class);
    return label;
}

void xd_appwiz_error(appwiz *state, const char *what, const char *detail) {
    char *text = detail != nullptr && detail[0] != '\0' ? g_strdup_printf("%s\n\n%s", what, detail)
                                                        : g_strdup(what);
    xd_message_box(GTK_WINDOW(state->window), XD_APPWIZ_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

bool xd_appwiz_pk_failed(PkResults *results, GError *error, PkErrorEnum *code, char **message) {
    *code = PK_ERROR_ENUM_UNKNOWN;
    *message = nullptr;
    if (error != nullptr) {
        /* PackageKit's own errors come as PK_CLIENT_ERROR 0xff + code. */
        if (error->domain == PK_CLIENT_ERROR && error->code >= 0xff)
            *code = (PkErrorEnum)(error->code - 0xff);
        g_dbus_error_strip_remote_error(error);
        *message = g_strdup(error->message);
        return true;
    }
    PkError *pk_error = results != nullptr ? pk_results_get_error_code(results) : nullptr;
    if (pk_error != nullptr) {
        *code = pk_error_get_code(pk_error);
        *message = g_strdup(pk_error_get_details(pk_error));
        g_object_unref(pk_error);
        return true;
    }
    if (results != nullptr && pk_results_get_exit_code(results) != PK_EXIT_ENUM_SUCCESS) {
        *message = g_strdup(pk_exit_enum_to_string(pk_results_get_exit_code(results)));
        return true;
    }
    return false;
}

/* ---- Flatpak --------------------------------------------------------------- */

static void index_installation(appwiz *state, FlatpakInstallation *inst) {
    GPtrArray *refs = flatpak_installation_list_installed_refs_by_kind(inst, FLATPAK_REF_KIND_APP,
                                                                       state->quit, nullptr);
    if (refs == nullptr)
        return;
    const char *arch = flatpak_get_default_arch();
    for (guint i = 0; i < refs->len; i++) {
        FlatpakInstalledRef *installed_ref = g_ptr_array_index(refs, i);
        FlatpakRef *ref = FLATPAK_REF(installed_ref);
        const char *name = flatpak_ref_get_name(ref);
        flatpak_app *old = g_hash_table_lookup(state->flatpaks, name);
        /* One per app: the user's own first, then the default arch. */
        if (old != nullptr && (g_strcmp0(old->installation, "user") == 0 ||
                               g_strcmp0(flatpak_ref_get_arch(ref), arch) != 0))
            continue;
        flatpak_app *installed_app = g_new0(flatpak_app, 1);
        installed_app->installation = g_strdup(flatpak_installation_get_id(inst));
        installed_app->ref = flatpak_ref_format_ref(ref);
        installed_app->version = g_strdup(flatpak_installed_ref_get_appdata_version(installed_ref));
        installed_app->summary = g_strdup(flatpak_installed_ref_get_appdata_summary(installed_ref));
        installed_app->license = g_strdup(flatpak_installed_ref_get_appdata_license(installed_ref));
        installed_app->size = flatpak_installed_ref_get_installed_size(installed_ref);
        const char *origin = flatpak_installed_ref_get_origin(installed_ref);
        installed_app->origin = g_strdup(origin);
        FlatpakRemote *remote =
            origin != nullptr
                ? flatpak_installation_get_remote_by_name(inst, origin, state->quit, nullptr)
                : nullptr;
        if (remote != nullptr) {
            char *title = flatpak_remote_get_title(remote);
            installed_app->publisher =
                title != nullptr && title[0] != '\0' ? title : g_strdup(origin);
            if (installed_app->publisher != title)
                g_free(title);
            installed_app->homepage = flatpak_remote_get_homepage(remote);
            g_object_unref(remote);
        } else {
            installed_app->publisher = g_strdup(origin);
        }
        g_hash_table_replace(state->flatpaks, g_strdup(name), installed_app);
    }
    g_ptr_array_free(refs, TRUE);
}

static void index_flatpaks(appwiz *state) {
    g_hash_table_remove_all(state->flatpaks);
    GPtrArray *all = xd_appwiz_installations(state->quit);
    for (guint i = 0; i < all->len; i++)
        index_installation(state, g_ptr_array_index(all, i));
    g_ptr_array_free(all, TRUE);
}

/* ---- the list --------------------------------------------------------------- */

static program *row_program(GtkListBoxRow *row) {
    return row != nullptr ? g_object_get_data(G_OBJECT(row), "program") : nullptr;
}

static xd_appwiz_sort current_sort(appwiz *state) {
    switch (gtk_combo_box_get_active(GTK_COMBO_BOX(state->sort))) {
    case 1:
        return XD_SORT_SIZE;
    case 2:
        return XD_SORT_FREQUENCY;
    case 3:
        return XD_SORT_DATE;
    case 4:
        return XD_SORT_SOURCE;
    default:
        return XD_SORT_NAME;
    }
}

static int sort_rows(GtkListBoxRow *left_row, GtkListBoxRow *right_row, gpointer data) {
    program *left = row_program(left_row), *right = row_program(right_row);
    return left != nullptr && right != nullptr
               ? xd_appwiz_compare(&left->key, &right->key, current_sort(data))
               : 0;
}

static gboolean resort_now(gpointer data) {
    appwiz *state = data;
    state->sort_idle = 0;
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(state->list));
    return G_SOURCE_REMOVE;
}

static void resort_later(appwiz *state) {
    xd_appwiz_sort sort = current_sort(state);
    if (state->sort_idle == 0 && (sort == XD_SORT_SIZE || sort == XD_SORT_SOURCE))
        state->sort_idle = g_timeout_add(200, resort_now, state);
}

static void show_size(program *program_entry) {
    if (program_entry->key.size == 0)
        return;
    char *size_text = xd_appwiz_format_size(program_entry->key.size);
    gtk_label_set_text(GTK_LABEL(program_entry->size), size_text);
    gtk_widget_show(program_entry->size_key);
    gtk_widget_show(program_entry->size);
    g_free(size_text);
}

static void show_extra(program *program_entry, bool show) {
    for (guint i = 0; i < program_entry->extra->len; i++)
        gtk_widget_set_visible(g_ptr_array_index(program_entry->extra, i), show);
    /* Not known yet: no Source row. */
    if (program_entry->source == nullptr) {
        gtk_widget_hide(program_entry->source_key);
        gtk_widget_hide(program_entry->source_value);
    }
}

/* Remove, or why not: a desktop file that PackageKit then found no
 * package for can't be removed after all. */
static void show_removable(program *program_entry) {
    bool removable = program_entry->src != SOURCE_NONE;
    gtk_label_set_text(GTK_LABEL(program_entry->how),
                       removable ? "To remove this program from your computer, "
                                   "click Remove."
                                 : "This program wasn't installed with a package "
                                   "manager, so it can't be removed here.");
    gtk_widget_set_sensitive(program_entry->remove, removable);
}

/* Where the program came from: the package manager and repository, the
 * Flatpak remote and installation, or nothing. Not known while
 * PackageKit is still being asked, or when there's no PackageKit. */
static void find_source(appwiz *state, program *program_entry) {
    g_clear_pointer(&program_entry->source, g_free);
    switch (program_entry->src) {
    case SOURCE_PACKAGE:
        if (program_entry->package_id != nullptr) {
            char *backend = nullptr;
            g_object_get(state->control, "backend-name", &backend, nullptr);
            program_entry->source = xd_appwiz_package_source(backend, program_entry->package_id);
            g_free(backend);
        }
        break;
    case SOURCE_FLATPAK:
        program_entry->source =
            xd_appwiz_flatpak_source(program_entry->remote, program_entry->installation);
        break;
    case SOURCE_NONE:
        program_entry->source = g_strdup(xd_appwiz_other_source(program_entry->exec));
        break;
    }
    program_entry->key.source = program_entry->source;
    if (program_entry->source_value == nullptr)
        return; /* no row yet */
    gtk_label_set_text(GTK_LABEL(program_entry->source_value),
                       program_entry->source != nullptr ? program_entry->source : "");
    show_removable(program_entry);
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->list));
    show_extra(program_entry, sel != nullptr && GTK_WIDGET(sel) == program_entry->row);
}

void xd_appwiz_sources_changed(appwiz *state) {
    for (guint i = 0; i < state->programs->len; i++)
        find_source(state, g_ptr_array_index(state->programs, i));
    resort_later(state);
}

static void on_row_selected([[maybe_unused]] GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    appwiz *state = data;
    program *sel = row_program(row);
    for (guint i = 0; i < state->programs->len; i++) {
        program *program_entry = g_ptr_array_index(state->programs, i);
        show_extra(program_entry, program_entry == sel);
    }
}

static void add_info(GtkWidget *grid, int row, const char *key, const char *value) {
    if (value == nullptr || value[0] == '\0')
        return;
    GtkWidget *key_label = xd_appwiz_label(key, nullptr);
    gtk_widget_set_valign(key_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), key_label, 0, row, 1, 1);
    GtkWidget *value_label = xd_appwiz_label(value, nullptr);
    gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
    gtk_grid_attach(GTK_GRID(grid), value_label, 1, row, 1, 1);
}

/* XP's Support Info box. */
static void on_support(GtkButton *button, gpointer data) {
    appwiz *state = data;
    program *program_entry = g_object_get_data(G_OBJECT(button), "program");
    GtkWidget *support_dialog = gtk_dialog_new_with_buttons(
        "Support Info", GTK_WINDOW(state->window),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Close",
        GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(support_dialog), FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(support_dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 10);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon =
        program_entry->icon != nullptr
            ? gtk_image_new_from_gicon(program_entry->icon, GTK_ICON_SIZE_DIALOG)
            : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_box_pack_start(GTK_BOX(head), icon, FALSE, FALSE, 0);
    char *markup = g_markup_printf_escaped("<b>%s</b>", program_entry->name);
    GtkWidget *name = xd_appwiz_label("", nullptr);
    gtk_label_set_markup(GTK_LABEL(name), markup);
    g_free(markup);
    gtk_box_pack_start(GTK_BOX(head), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);
    char *intro = g_strdup_printf("Use the following information to get technical support for %s.",
                                  program_entry->name);
    gtk_box_pack_start(GTK_BOX(box), xd_appwiz_label(intro, nullptr), FALSE, FALSE, 0);
    g_free(intro);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    int grid_row = 0;
    char *pkg = nullptr;
    if (program_entry->src == SOURCE_PACKAGE && program_entry->package_id != nullptr)
        xd_appwiz_split_package_id(program_entry->package_id, &pkg, nullptr, nullptr);
    /* The software source is the closest thing to XP's Publisher. */
    add_info(grid, grid_row++, "Software source:", program_entry->publisher);
    add_info(grid, grid_row++, "Version:", program_entry->version);
    add_info(grid, grid_row++, program_entry->src == SOURCE_FLATPAK ? "Flatpak:" : "Package:",
             program_entry->src == SOURCE_FLATPAK ? program_entry->package_id : pkg);
    add_info(grid, grid_row++, "License:", program_entry->license);
    if (program_entry->url != nullptr && program_entry->url[0] != '\0') {
        gtk_grid_attach(GTK_GRID(grid), xd_appwiz_label("Support information:", nullptr), 0,
                        grid_row, 1, 1);
        GtkWidget *link = gtk_link_button_new(program_entry->url);
        gtk_widget_set_halign(link, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), link, 1, grid_row++, 1, 1);
    }
    add_info(grid, grid_row++, "Comments:", program_entry->description);
    if (program_entry->src == SOURCE_NONE)
        add_info(grid, grid_row++, "Comments:",
                 "This program wasn't installed by a package manager, so there is no support "
                 "information for it.");
    add_info(grid, grid_row, "Program file:", program_entry->path);
    g_free(pkg);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(support_dialog);
    gtk_dialog_run(GTK_DIALOG(support_dialog));
    gtk_widget_destroy(support_dialog);
}

static void after_change(appwiz *state, bool ok, [[maybe_unused]] gpointer data) {
    if (ok)
        xd_appwiz_reload(state);
}

static void on_remove(GtkButton *button, gpointer data) {
    appwiz *state = data;
    program *program_entry = g_object_get_data(G_OBJECT(button), "program");
    if (state->busy)
        return;
    if (program_entry->src == SOURCE_FLATPAK) {
        xd_appwiz_remove_flatpak(state, program_entry->name, program_entry->installation,
                                 program_entry->package_id, after_change, nullptr);
        return;
    }
    if (program_entry->package_id == nullptr) {
        xd_message_box(GTK_WINDOW(state->window), XD_APPWIZ_TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       state->pk_missing
                           ? "Programs can't be removed: PackageKit isn't available."
                           : "Add or Remove Programs is still finding out which package this "
                             "program came from. Try again in a moment.");
        return;
    }
    /* The package's other programs go with it. */
    GString *also = g_string_new(nullptr);
    for (guint i = 0; i < state->programs->len; i++) {
        program *other = g_ptr_array_index(state->programs, i);
        if (other != program_entry && g_strcmp0(other->package_id, program_entry->package_id) == 0)
            g_string_append_printf(also, "%s%s", also->len > 0 ? ", " : "", other->name);
    }
    xd_appwiz_remove_package(state, program_entry->name, program_entry->package_id,
                             also->len > 0 ? also->str : nullptr, after_change, nullptr);
    g_string_free(also, TRUE);
}

static GtkWidget *right_label(const char *text, const char *css_class) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 1);
    if (css_class != nullptr)
        xd_appwiz_add_class(label, css_class);
    return label;
}

static void extra(program *program_entry, GtkWidget *widget) {
    gtk_widget_set_no_show_all(widget, TRUE);
    g_ptr_array_add(program_entry->extra, widget);
}

/* XP's row: icon, name, Size; selected, also Used, Last Used On, the
 * support link and Remove. */
static GtkWidget *program_row(appwiz *state, program *program_entry) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 4);
    GtkWidget *icon =
        program_entry->icon != nullptr
            ? gtk_image_new_from_gicon(program_entry->icon, GTK_ICON_SIZE_DND)
            : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), icon, 0, 0, 1, 3);
    GtkWidget *name = xd_appwiz_label(program_entry->name, "xd-arp-name");
    gtk_label_set_line_wrap(GTK_LABEL(name), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_grid_attach(GTK_GRID(grid), name, 1, 0, 1, 1);

    program_entry->extra = g_ptr_array_new();
    program_entry->size_key = right_label("Size", "xd-arp-key");
    program_entry->size = right_label("", nullptr);
    gtk_widget_set_size_request(program_entry->size, 80, -1);
    gtk_widget_set_no_show_all(program_entry->size_key, TRUE);
    gtk_widget_set_no_show_all(program_entry->size, TRUE);
    gtk_grid_attach(GTK_GRID(grid), program_entry->size_key, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), program_entry->size, 3, 0, 1, 1);
    show_size(program_entry);

    int grid_row = 1;
    const char *used =
        xd_appwiz_usage_name(xd_appwiz_usage_for(program_entry->key.launches, state->have_counts));
    if (used != nullptr) {
        GtkWidget *key_label = right_label("Used", "xd-arp-key"),
                  *value_label = right_label(used, nullptr);
        extra(program_entry, key_label);
        extra(program_entry, value_label);
        gtk_grid_attach(GTK_GRID(grid), key_label, 2, grid_row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), value_label, 3, grid_row++, 1, 1);
    }
    if (program_entry->key.last_used > 0) {
        char *date = xd_appwiz_format_date(program_entry->key.last_used);
        GtkWidget *key_label = right_label("Last Used On", "xd-arp-key"),
                  *value_label = right_label(date, nullptr);
        g_free(date);
        extra(program_entry, key_label);
        extra(program_entry, value_label);
        gtk_grid_attach(GTK_GRID(grid), key_label, 2, grid_row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), value_label, 3, grid_row++, 1, 1);
    }
    /* Not XP's (its programs all came from Windows Installer or their
     * own setup): which package manager, or Flatpak, or none. */
    program_entry->source_key = right_label("Source", "xd-arp-key");
    program_entry->source_value =
        right_label(program_entry->source != nullptr ? program_entry->source : "", nullptr);
    extra(program_entry, program_entry->source_key);
    extra(program_entry, program_entry->source_value);
    gtk_grid_attach(GTK_GRID(grid), program_entry->source_key, 2, grid_row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), program_entry->source_value, 3, grid_row++, 1, 1);

    GtkWidget *link = gtk_button_new_with_label("Click here for support information.");
    gtk_button_set_relief(GTK_BUTTON(link), GTK_RELIEF_NONE);
    xd_appwiz_add_class(link, "xd-arp-link");
    gtk_widget_set_halign(link, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(link), "program", program_entry);
    g_signal_connect(link, "clicked", G_CALLBACK(on_support), state);
    extra(program_entry, link);
    gtk_grid_attach(GTK_GRID(grid), link, 1, 1, 1, 1);

    program_entry->how = xd_appwiz_label("", nullptr);
    extra(program_entry, program_entry->how);
    gtk_grid_attach(GTK_GRID(grid), program_entry->how, 1, XD_MAX(grid_row, 2), 1, 1);
    program_entry->remove = gtk_button_new_with_mnemonic("_Remove");
    gtk_widget_set_size_request(program_entry->remove, 80, -1);
    gtk_widget_set_halign(program_entry->remove, GTK_ALIGN_END);
    g_object_set_data(G_OBJECT(program_entry->remove), "program", program_entry);
    g_signal_connect(program_entry->remove, "clicked", G_CALLBACK(on_remove), state);
    extra(program_entry, program_entry->remove);
    gtk_grid_attach(GTK_GRID(grid), program_entry->remove, 2, XD_MAX(grid_row, 2), 2, 1);
    show_removable(program_entry);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), grid);
    g_object_set_data(G_OBJECT(row), "program", program_entry);
    program_entry->row = row;
    return row;
}

/* ---- PackageKit lookups ------------------------------------------------------ */

typedef struct lookup {
    appwiz *w;
    unsigned generation;
    program *p;
} lookup;

static void update_status(appwiz *state) {
    const char *text = nullptr;
    if (state->pk_missing)
        text = "Sizes and removal of packages need PackageKit, which isn't available.";
    else if (state->pending > 0 || state->lookups.length > 0)
        text = "Finding out which packages the programs came from...";
    gtk_label_set_text(GTK_LABEL(state->status), text != nullptr ? text : "");
    gtk_widget_set_visible(state->status, text != nullptr);
}

static void apply_details(appwiz *state, PkDetails *details) {
    const char *id = pk_details_get_package_id(details);
    char *version = nullptr, *repo = nullptr;
    xd_appwiz_split_package_id(id, nullptr, &version, &repo);
    for (guint i = 0; i < state->programs->len; i++) {
        program *program_entry = g_ptr_array_index(state->programs, i);
        if (program_entry->src != SOURCE_PACKAGE || g_strcmp0(program_entry->package_id, id) != 0)
            continue;
        program_entry->key.size = pk_details_get_size(details);
        g_free(program_entry->version);
        program_entry->version = g_strdup(version);
        g_free(program_entry->publisher);
        program_entry->publisher = repo != nullptr && repo[0] != '\0' ? g_strdup(repo) : nullptr;
        g_free(program_entry->url);
        program_entry->url = g_strdup(pk_details_get_url(details));
        g_free(program_entry->description);
        const char *desc = pk_details_get_description(details);
        program_entry->description =
            g_strdup(desc != nullptr && desc[0] != '\0' ? desc : pk_details_get_summary(details));
        g_free(program_entry->license);
        program_entry->license = g_strdup(pk_details_get_license(details));
        show_size(program_entry);
    }
    g_free(version);
    g_free(repo);
}

static void on_details(GObject *src, GAsyncResult *res, gpointer data) {
    lookup *pending_lookup = data;
    appwiz *state = pending_lookup->w;
    GError *error = nullptr;
    PkResults *results = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        state->pending--;
        if (pending_lookup->generation == state->generation && results != nullptr) {
            GPtrArray *details = pk_results_get_details_array(results);
            for (guint i = 0; i < details->len; i++) {
                PkDetails *detail = g_ptr_array_index(details, i);
                g_hash_table_replace(state->details, g_strdup(pk_details_get_package_id(detail)),
                                     g_object_ref(detail));
                apply_details(state, detail);
            }
            g_ptr_array_unref(details);
            resort_later(state);
        }
        update_status(state);
    }
    g_clear_error(&error);
    g_clear_object(&results);
    g_free(pending_lookup);
}

static void flush_details(appwiz *state) {
    if (state->detail_ids->len == 0)
        return;
    g_ptr_array_add(state->detail_ids, nullptr);
    char **ids = (char **)g_ptr_array_free(state->detail_ids, FALSE);
    state->detail_ids = g_ptr_array_new_with_free_func(g_free);
    lookup *pending_lookup = g_new0(lookup, 1);
    *pending_lookup = (lookup){state, state->generation, nullptr};
    state->pending++;
    pk_client_get_details_async(state->pk, ids, state->quit, nullptr, nullptr, on_details,
                                pending_lookup);
    g_strfreev(ids);
}

static void next_lookup(appwiz *state);

static void on_searched(GObject *src, GAsyncResult *res, gpointer data) {
    lookup *pending_lookup = data;
    appwiz *state = pending_lookup->w;
    GError *error = nullptr;
    PkResults *results = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        g_free(pending_lookup);
        return;
    }
    state->pending--;
    if (pending_lookup->generation == state->generation) {
        if (results == nullptr) {
            /* No PackageKit: stop asking. */
            g_printerr("xd-appwiz: PackageKit: %s\n", error->message);
            state->pk_missing = true;
            g_queue_clear(&state->lookups);
        } else {
            GPtrArray *pkgs = pk_results_get_package_array(results);
            if (pkgs->len > 0) {
                const char *id = pk_package_get_id(g_ptr_array_index(pkgs, 0));
                pending_lookup->p->package_id = g_strdup(id);
                find_source(state, pending_lookup->p);
                /* The package's other programs may have brought its
                 * details already. */
                PkDetails *known = g_hash_table_lookup(state->details, id);
                if (known != nullptr) {
                    apply_details(state, known);
                } else if (!g_hash_table_contains(state->asked, id)) {
                    g_hash_table_add(state->asked, g_strdup(id));
                    g_ptr_array_add(state->detail_ids, g_strdup(id));
                }
            } else {
                /* Not from a package after all. */
                pending_lookup->p->src = SOURCE_NONE;
                find_source(state, pending_lookup->p);
            }
            g_ptr_array_unref(pkgs);
            if (state->detail_ids->len >= DETAILS_BATCH)
                flush_details(state);
        }
        next_lookup(state);
    }
    g_clear_error(&error);
    g_clear_object(&results);
    g_free(pending_lookup);
}

/* One desktop file at a time: some backends (alpm) only match a single
 * path per SearchFiles. Each takes a few milliseconds. */
static void next_lookup(appwiz *state) {
    program *program_entry = g_queue_pop_head(&state->lookups);
    if (program_entry == nullptr) {
        flush_details(state);
        update_status(state);
        return;
    }
    char *values[] = {program_entry->path, nullptr};
    lookup *pending_lookup = g_new0(lookup, 1);
    *pending_lookup = (lookup){state, state->generation, program_entry};
    state->pending++;
    pk_client_search_files_async(state->pk, pk_bitfield_value(PK_FILTER_ENUM_INSTALLED), values,
                                 state->quit, nullptr, nullptr, on_searched, pending_lookup);
    update_status(state);
}

/* ---- loading ---------------------------------------------------------------- */

static void load_usage(appwiz *state) {
    g_clear_pointer(&state->counts, g_hash_table_unref);
    char **lines = nullptr;
    size_t count = 0;
    xd_state_read_lines("start-frequent", &lines, &count, 200);
    state->counts = xd_appwiz_parse_launch_counts(lines, count);
    state->have_counts = g_hash_table_size(state->counts) > 0;
    xd_free_lines(lines, count);

    g_clear_pointer(&state->launched, g_hash_table_unref);
    xd_state_read_lines("start-last-used", &lines, &count, 200);
    state->launched = xd_appwiz_parse_launch_times(lines, count);
    xd_free_lines(lines, count);

    g_clear_pointer(&state->recent, g_bookmark_file_free);
    state->recent = g_bookmark_file_new();
    char *xbel = g_build_filename(g_get_user_data_dir(), "recently-used.xbel", nullptr);
    if (!g_bookmark_file_load_from_file(state->recent, xbel, nullptr))
        g_clear_pointer(&state->recent, g_bookmark_file_free);
    g_free(xbel);
}

static program *new_program(appwiz *state, GDesktopAppInfo *info) {
    GAppInfo *app = G_APP_INFO(info);
    program *program_entry = g_new0(program, 1);
    program_entry->id = g_strdup(g_app_info_get_id(app));
    program_entry->name = g_strdup(g_app_info_get_name(app));
    program_entry->key.name = program_entry->name;
    program_entry->path = g_strdup(g_desktop_app_info_get_filename(info));
    program_entry->exec = g_strdup(g_app_info_get_executable(app));
    GIcon *icon = g_app_info_get_icon(app);
    program_entry->icon = icon != nullptr ? g_object_ref(icon) : nullptr;
    program_entry->key.launches = xd_appwiz_launches(state->counts, program_entry->id);
    /* Started from the Start menu or the taskbar, or opening a document
     * (from wherever it was started), whichever was later. */
    program_entry->key.last_used =
        XD_MAX(xd_appwiz_last_used(state->recent, program_entry->id, program_entry->exec),
               xd_appwiz_launch_time(state->launched, program_entry->id));

    char *flatpak = g_desktop_app_info_get_string(info, "X-Flatpak");
    flatpak_app *installed_app =
        flatpak != nullptr ? g_hash_table_lookup(state->flatpaks, flatpak) : nullptr;
    g_free(flatpak);
    if (installed_app != nullptr) {
        program_entry->src = SOURCE_FLATPAK;
        program_entry->package_id = g_strdup(installed_app->ref);
        program_entry->installation = g_strdup(installed_app->installation);
        program_entry->remote = g_strdup(installed_app->origin);
        program_entry->key.size = installed_app->size;
        program_entry->version = g_strdup(installed_app->version);
        program_entry->publisher = g_strdup(installed_app->publisher);
        program_entry->url = g_strdup(installed_app->homepage);
        program_entry->description = g_strdup(installed_app->summary);
        program_entry->license = g_strdup(installed_app->license);
    } else if (program_entry->path != nullptr &&
               !g_str_has_prefix(program_entry->path, g_get_user_data_dir()) &&
               !g_str_has_prefix(program_entry->path, g_get_home_dir())) {
        /* Packages don't install into home folders. */
        program_entry->src = SOURCE_PACKAGE;
    }
    find_source(state, program_entry);
    return program_entry;
}

static void load_programs(appwiz *state) {
    if (state->reload_idle != 0) {
        g_source_remove(state->reload_idle);
        state->reload_idle = 0;
    }
    state->generation++;
    state->pk_missing = false;
    g_queue_clear(&state->lookups);
    g_ptr_array_set_size(state->detail_ids, 0);
    g_hash_table_remove_all(state->asked);
    g_hash_table_remove_all(state->details);
    /* Unselected while every row is still there: removing the selected
     * row would report the selection change half way through. */
    gtk_list_box_unselect_all(GTK_LIST_BOX(state->list));
    GList *kids = gtk_container_get_children(GTK_CONTAINER(state->list));
    for (GList *child = kids; child != nullptr; child = child->next)
        gtk_widget_destroy(child->data);
    g_list_free(kids);
    g_ptr_array_set_size(state->programs, 0);

    load_usage(state);
    index_flatpaks(state);
    GList *all = g_app_info_get_all();
    for (GList *link = all; link != nullptr; link = link->next) {
        GAppInfo *app = link->data;
        if (!G_IS_DESKTOP_APP_INFO(app) || !g_app_info_should_show(app))
            continue;
        program *program_entry = new_program(state, G_DESKTOP_APP_INFO(app));
        g_ptr_array_add(state->programs, program_entry);
        gtk_container_add(GTK_CONTAINER(state->list), program_row(state, program_entry));
        if (program_entry->src == SOURCE_PACKAGE)
            g_queue_push_tail(&state->lookups, program_entry);
    }
    g_list_free_full(all, g_object_unref);
    gtk_widget_show_all(state->list);
    next_lookup(state);
}

void xd_appwiz_reload(appwiz *state) { load_programs(state); }

static gboolean reload_now(gpointer data) {
    appwiz *state = data;
    /* Not while something is being installed or removed: the list would
     * change under the question and the progress dialog. */
    if (state->busy)
        return G_SOURCE_CONTINUE;
    state->reload_idle = 0;
    load_programs(state);
    return G_SOURCE_REMOVE;
}

/* Desktop files come and go as programs are installed and removed. */
static void on_apps_changed([[maybe_unused]] GAppInfoMonitor *app_monitor, gpointer data) {
    appwiz *state = data;
    if (state->reload_idle == 0)
        state->reload_idle = g_timeout_add(500, reload_now, state);
}

static void on_sort_changed([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    appwiz *state = data;
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(state->list));
}

static GtkWidget *remove_page(appwiz *state) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title = xd_appwiz_label("Currently installed programs:", "xd-arp-heading");
    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    GtkWidget *sort_label = gtk_label_new_with_mnemonic("S_ort by:");
    state->sort = gtk_combo_box_text_new();
    static const char *const sorts[] = {"Name", "Size", "Frequency of use", "Date last used",
                                        "Source"};
    for (size_t i = 0; i < XD_ARRAY_LEN(sorts); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->sort), sorts[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->sort), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(sort_label), state->sort);
    g_signal_connect(state->sort, "changed", G_CALLBACK(on_sort_changed), state);
    gtk_box_pack_start(GTK_BOX(top), sort_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), state->sort, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    state->list = gtk_list_box_new();
    xd_appwiz_add_class(state->list, "xd-arp-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(state->list), sort_rows, state, nullptr);
    g_signal_connect(state->list, "row-selected", G_CALLBACK(on_row_selected), state);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), state->list);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    state->status = xd_appwiz_label("", "xd-arp-dim");
    gtk_widget_set_no_show_all(state->status, TRUE);
    gtk_box_pack_start(GTK_BOX(box), state->status, FALSE, FALSE, 0);
    return box;
}

/* ---- window ------------------------------------------------------------------ */

static const char css[] =
    ".xd-arp-bar { background-image: linear-gradient(to right, #7ba2e7, #6375d6); }\n"
    ".xd-arp-bar button { background: none; border: 1px solid transparent; box-shadow: none;"
    " border-radius: 0; padding: 6px 2px; }\n"
    ".xd-arp-bar button label { color: #ffffff; }\n"
    ".xd-arp-bar button:hover { border-color: #c6d3f7; }\n"
    ".xd-arp-bar button:checked { background-color: rgba(255,255,255,0.25);"
    " border-color: #ffffff; }\n"
    ".xd-arp-heading { font-weight: bold; }\n"
    ".xd-arp-key, .xd-arp-dim { color: #555555; }\n"
    "row:selected .xd-arp-key { color: inherit; }\n"
    "button.xd-arp-link { background: none; border: none; box-shadow: none; padding: 0 2px; }\n"
    "button.xd-arp-link label { color: #0033cc; }\n"
    "row:selected button.xd-arp-link label { color: @theme_selected_fg_color;"
    " text-decoration: underline; }\n"
    "button.xd-arp-link:hover label { text-decoration: underline; }\n"
    ".xd-arp-big { font-size: 13pt; }\n";

void xd_appwiz_show_page(appwiz *state, xd_appwiz_page page) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->tabs[page]), TRUE);
}

static void on_tab(GtkToggleButton *toggle, gpointer data) {
    appwiz *state = data;
    if (gtk_toggle_button_get_active(toggle))
        gtk_stack_set_visible_child_name(GTK_STACK(state->stack),
                                         g_object_get_data(G_OBJECT(toggle), "page"));
}

static GtkWidget *tab(appwiz *state, GtkWidget *group, const char *icon, const char *text,
                      const char *page) {
    GtkWidget *button = group != nullptr ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(group))
                                         : gtk_radio_button_new(nullptr);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    gtk_box_pack_start(GTK_BOX(col), img, FALSE, FALSE, 0);
    GtkWidget *label = gtk_label_new_with_mnemonic(text);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 10);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), button);
    gtk_box_pack_start(GTK_BOX(col), label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(button), col);
    g_object_set_data(G_OBJECT(button), "page", (gpointer)(uintptr_t)page);
    g_signal_connect(button, "toggled", G_CALLBACK(on_tab), state);
    return button;
}

static void usage(void) {
    g_printerr("usage: xd-appwiz [--add | --components]\n");
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.AddRemovePrograms");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    xd_appwiz_page start = XD_PAGE_REMOVE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--add") == 0) {
            start = XD_PAGE_ADD;
        } else if (strcmp(argv[i], "--components") == 0) {
            start = XD_PAGE_COMPONENTS;
        } else {
            usage();
            return EXIT_FAILURE;
        }
    }

    static appwiz state;
    state.quit = g_cancellable_new();
    state.pk = pk_client_new();
    pk_client_set_interactive(state.pk, FALSE);
    state.pk_task = pk_client_new();
    pk_client_set_interactive(state.pk_task, TRUE);
    state.control = pk_control_new();
    state.programs = g_ptr_array_new_with_free_func(program_free);
    state.flatpaks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, flatpak_app_free);
    state.detail_ids = g_ptr_array_new_with_free_func(g_free);
    state.asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    state.details = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    g_queue_init(&state.lookups);

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state.window), XD_APPWIZ_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(state.window), "system-software-install");
    gtk_window_set_default_size(GTK_WINDOW(state.window), 760, 540);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(state.window), body);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 4);
    gtk_widget_set_size_request(bar, 96, -1);
    GtkWidget *bar_bg = gtk_event_box_new();
    xd_appwiz_add_class(bar_bg, "xd-arp-bar");
    gtk_container_add(GTK_CONTAINER(bar_bg), bar);
    gtk_box_pack_start(GTK_BOX(body), bar_bg, FALSE, FALSE, 0);
    state.tabs[XD_PAGE_REMOVE] =
        tab(&state, nullptr, "system-software-install", "C_hange or Remove Programs", "remove");
    state.tabs[XD_PAGE_ADD] =
        tab(&state, state.tabs[0], "media-optical", "Add _New Programs", "add");
    state.tabs[XD_PAGE_COMPONENTS] = tab(&state, state.tabs[0], "applications-system",
                                         "Add/Remove _Windows Components", "components");
    for (size_t i = 0; i < XD_ARRAY_LEN(state.tabs); i++)
        gtk_box_pack_start(GTK_BOX(bar), state.tabs[i], FALSE, FALSE, 0);

    state.stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(state.stack), remove_page(&state), "remove");
    gtk_stack_add_named(GTK_STACK(state.stack), xd_appwiz_add_page(&state), "add");
    gtk_stack_add_named(GTK_STACK(state.stack), xd_appwiz_components_page(&state), "components");
    gtk_box_pack_start(GTK_BOX(body), state.stack, TRUE, TRUE, 0);

    g_signal_connect(state.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(state.window);
    xd_appwiz_show_page(&state, start);
    gtk_stack_set_visible_child_name(GTK_STACK(state.stack),
                                     g_object_get_data(G_OBJECT(state.tabs[start]), "page"));
    load_programs(&state);
    /* Typing goes to the page, not to the buttons down the left. */
    gtk_widget_grab_focus(start == XD_PAGE_ADD      ? state.search
                          : start == XD_PAGE_REMOVE ? state.list
                                                    : state.groups);
    GAppInfoMonitor *monitor = g_app_info_monitor_get();
    state.monitor_handler =
        g_signal_connect(monitor, "changed", G_CALLBACK(on_apps_changed), &state);
    gtk_main();

    g_cancellable_cancel(state.quit);
    g_signal_handler_disconnect(monitor, state.monitor_handler);
    g_object_unref(monitor);
    if (state.reload_idle != 0)
        g_source_remove(state.reload_idle);
    if (state.sort_idle != 0)
        g_source_remove(state.sort_idle);
    g_queue_clear(&state.lookups);
    g_ptr_array_free(state.programs, TRUE);
    g_ptr_array_free(state.detail_ids, TRUE);
    g_hash_table_unref(state.asked);
    g_hash_table_unref(state.details);
    g_hash_table_unref(state.flatpaks);
    g_clear_pointer(&state.counts, g_hash_table_unref);
    g_clear_pointer(&state.recent, g_bookmark_file_free);
    g_clear_pointer(&state.launched, g_hash_table_unref);
    g_clear_pointer(&state.result_rows, g_hash_table_unref);
    g_object_unref(state.pk);
    g_object_unref(state.pk_task);
    g_object_unref(state.control);
    g_object_unref(state.quit);
    return EXIT_SUCCESS;
}
