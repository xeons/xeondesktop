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
    char *installation, *ref, *version, *summary, *license, *publisher, *homepage;
    guint64 size;
} flatpak_app;

/* A program in the list: a desktop entry and what it came from. */
typedef struct program {
    xd_appwiz_key key; /* key.name is `name` */
    char *id, *name, *path, *exec;
    GIcon *icon;
    source src;
    char *package_id;   /* PackageKit's id, or the Flatpak ref */
    char *installation; /* Flatpak's */
    char *version, *publisher, *url, *description, *license;
    GtkWidget *row, *size_key, *size;
    GPtrArray *extra; /* shown when selected */
} program;

static void program_free(gpointer data) {
    program *p = data;
    g_free(p->id);
    g_free(p->name);
    g_free(p->path);
    g_free(p->exec);
    g_clear_object(&p->icon);
    g_free(p->package_id);
    g_free(p->installation);
    g_free(p->version);
    g_free(p->publisher);
    g_free(p->url);
    g_free(p->description);
    g_free(p->license);
    if (p->extra != nullptr)
        g_ptr_array_free(p->extra, TRUE);
    g_free(p);
}

static void flatpak_app_free(gpointer data) {
    flatpak_app *f = data;
    g_free(f->installation);
    g_free(f->ref);
    g_free(f->version);
    g_free(f->summary);
    g_free(f->license);
    g_free(f->publisher);
    g_free(f->homepage);
    g_free(f);
}

/* ---- helpers --------------------------------------------------------------- */

void xd_appwiz_add_class(GtkWidget *w, const char *css_class) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), css_class);
}

GtkWidget *xd_appwiz_label(const char *text, const char *css_class) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
    if (css_class != nullptr)
        xd_appwiz_add_class(l, css_class);
    return l;
}

void xd_appwiz_error(appwiz *w, const char *what, const char *detail) {
    char *s = detail != nullptr && detail[0] != '\0' ? g_strdup_printf("%s\n\n%s", what, detail)
                                                     : g_strdup(what);
    xd_message_box(GTK_WINDOW(w->window), XD_APPWIZ_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, s);
    g_free(s);
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
    PkError *e = results != nullptr ? pk_results_get_error_code(results) : nullptr;
    if (e != nullptr) {
        *code = pk_error_get_code(e);
        *message = g_strdup(pk_error_get_details(e));
        g_object_unref(e);
        return true;
    }
    if (results != nullptr && pk_results_get_exit_code(results) != PK_EXIT_ENUM_SUCCESS) {
        *message = g_strdup(pk_exit_enum_to_string(pk_results_get_exit_code(results)));
        return true;
    }
    return false;
}

/* ---- Flatpak --------------------------------------------------------------- */

static void index_installation(appwiz *w, FlatpakInstallation *inst) {
    GPtrArray *refs = flatpak_installation_list_installed_refs_by_kind(inst, FLATPAK_REF_KIND_APP,
                                                                       w->quit, nullptr);
    if (refs == nullptr)
        return;
    const char *arch = flatpak_get_default_arch();
    for (guint i = 0; i < refs->len; i++) {
        FlatpakInstalledRef *r = g_ptr_array_index(refs, i);
        FlatpakRef *ref = FLATPAK_REF(r);
        const char *name = flatpak_ref_get_name(ref);
        flatpak_app *old = g_hash_table_lookup(w->flatpaks, name);
        /* One per app: the user's own first, then the default arch. */
        if (old != nullptr && (g_strcmp0(old->installation, "user") == 0 ||
                               g_strcmp0(flatpak_ref_get_arch(ref), arch) != 0))
            continue;
        flatpak_app *f = g_new0(flatpak_app, 1);
        f->installation = g_strdup(flatpak_installation_get_id(inst));
        f->ref = flatpak_ref_format_ref(ref);
        f->version = g_strdup(flatpak_installed_ref_get_appdata_version(r));
        f->summary = g_strdup(flatpak_installed_ref_get_appdata_summary(r));
        f->license = g_strdup(flatpak_installed_ref_get_appdata_license(r));
        f->size = flatpak_installed_ref_get_installed_size(r);
        const char *origin = flatpak_installed_ref_get_origin(r);
        FlatpakRemote *remote = origin != nullptr
                                    ? flatpak_installation_get_remote_by_name(inst, origin,
                                                                              w->quit, nullptr)
                                    : nullptr;
        if (remote != nullptr) {
            char *title = flatpak_remote_get_title(remote);
            f->publisher = title != nullptr && title[0] != '\0' ? title : g_strdup(origin);
            if (f->publisher != title)
                g_free(title);
            f->homepage = flatpak_remote_get_homepage(remote);
            g_object_unref(remote);
        } else {
            f->publisher = g_strdup(origin);
        }
        g_hash_table_replace(w->flatpaks, g_strdup(name), f);
    }
    g_ptr_array_free(refs, TRUE);
}

static void index_flatpaks(appwiz *w) {
    g_hash_table_remove_all(w->flatpaks);
    GPtrArray *all = xd_appwiz_installations(w->quit);
    for (guint i = 0; i < all->len; i++)
        index_installation(w, g_ptr_array_index(all, i));
    g_ptr_array_free(all, TRUE);
}

/* ---- the list --------------------------------------------------------------- */

static program *row_program(GtkListBoxRow *row) {
    return row != nullptr ? g_object_get_data(G_OBJECT(row), "program") : nullptr;
}

static xd_appwiz_sort current_sort(appwiz *w) {
    switch (gtk_combo_box_get_active(GTK_COMBO_BOX(w->sort))) {
    case 1:
        return XD_SORT_SIZE;
    case 2:
        return XD_SORT_FREQUENCY;
    case 3:
        return XD_SORT_DATE;
    default:
        return XD_SORT_NAME;
    }
}

static int sort_rows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data) {
    program *x = row_program(a), *y = row_program(b);
    return x != nullptr && y != nullptr ? xd_appwiz_compare(&x->key, &y->key, current_sort(data))
                                        : 0;
}

static gboolean resort_now(gpointer data) {
    appwiz *w = data;
    w->sort_idle = 0;
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(w->list));
    return G_SOURCE_REMOVE;
}

static void resort_later(appwiz *w) {
    if (w->sort_idle == 0 && current_sort(w) == XD_SORT_SIZE)
        w->sort_idle = g_timeout_add(200, resort_now, w);
}

static void show_size(program *p) {
    if (p->key.size == 0)
        return;
    char *s = xd_appwiz_format_size(p->key.size);
    gtk_label_set_text(GTK_LABEL(p->size), s);
    gtk_widget_show(p->size_key);
    gtk_widget_show(p->size);
    g_free(s);
}

static void show_extra(program *p, bool show) {
    for (guint i = 0; i < p->extra->len; i++)
        gtk_widget_set_visible(g_ptr_array_index(p->extra, i), show);
}

static void on_row_selected([[maybe_unused]] GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    appwiz *w = data;
    program *sel = row_program(row);
    for (guint i = 0; i < w->programs->len; i++) {
        program *p = g_ptr_array_index(w->programs, i);
        show_extra(p, p == sel);
    }
}

static void add_info(GtkWidget *grid, int row, const char *key, const char *value) {
    if (value == nullptr || value[0] == '\0')
        return;
    GtkWidget *k = xd_appwiz_label(key, nullptr);
    gtk_widget_set_valign(k, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
    GtkWidget *v = xd_appwiz_label(value, nullptr);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
}

/* XP's Support Info box. */
static void on_support(GtkButton *b, gpointer data) {
    appwiz *w = data;
    program *p = g_object_get_data(G_OBJECT(b), "program");
    GtkWidget *d = gtk_dialog_new_with_buttons("Support Info", GTK_WINDOW(w->window),
                                               (GtkDialogFlags)(GTK_DIALOG_MODAL |
                                                                GTK_DIALOG_DESTROY_WITH_PARENT),
                                               "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 10);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = p->icon != nullptr ? gtk_image_new_from_gicon(p->icon, GTK_ICON_SIZE_DIALOG)
                                         : gtk_image_new_from_icon_name("application-x-executable",
                                                                        GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_box_pack_start(GTK_BOX(head), icon, FALSE, FALSE, 0);
    char *markup = g_markup_printf_escaped("<b>%s</b>", p->name);
    GtkWidget *name = xd_appwiz_label("", nullptr);
    gtk_label_set_markup(GTK_LABEL(name), markup);
    g_free(markup);
    gtk_box_pack_start(GTK_BOX(head), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);
    char *intro = g_strdup_printf("Use the following information to get technical support for %s.",
                                  p->name);
    gtk_box_pack_start(GTK_BOX(box), xd_appwiz_label(intro, nullptr), FALSE, FALSE, 0);
    g_free(intro);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    int r = 0;
    char *pkg = nullptr;
    if (p->src == SOURCE_PACKAGE && p->package_id != nullptr)
        xd_appwiz_split_package_id(p->package_id, &pkg, nullptr, nullptr);
    /* The software source is the closest thing to XP's Publisher. */
    add_info(grid, r++, "Software source:", p->publisher);
    add_info(grid, r++, "Version:", p->version);
    add_info(grid, r++, p->src == SOURCE_FLATPAK ? "Flatpak:" : "Package:",
             p->src == SOURCE_FLATPAK ? p->package_id : pkg);
    add_info(grid, r++, "License:", p->license);
    if (p->url != nullptr && p->url[0] != '\0') {
        gtk_grid_attach(GTK_GRID(grid), xd_appwiz_label("Support information:", nullptr), 0, r, 1,
                        1);
        GtkWidget *link = gtk_link_button_new(p->url);
        gtk_widget_set_halign(link, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), link, 1, r++, 1, 1);
    }
    add_info(grid, r++, "Comments:", p->description);
    if (p->src == SOURCE_NONE)
        add_info(grid, r++, "Comments:",
                 "This program wasn't installed by a package manager, so there is no support "
                 "information for it.");
    add_info(grid, r, "Program file:", p->path);
    g_free(pkg);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void after_change(appwiz *w, bool ok, [[maybe_unused]] gpointer data) {
    if (ok)
        xd_appwiz_reload(w);
}

static void on_remove(GtkButton *b, gpointer data) {
    appwiz *w = data;
    program *p = g_object_get_data(G_OBJECT(b), "program");
    if (w->busy)
        return;
    if (p->src == SOURCE_FLATPAK) {
        xd_appwiz_remove_flatpak(w, p->name, p->installation, p->package_id, after_change,
                                 nullptr);
        return;
    }
    if (p->package_id == nullptr) {
        xd_message_box(GTK_WINDOW(w->window), XD_APPWIZ_TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                       w->pk_missing
                           ? "Programs can't be removed: PackageKit isn't available."
                           : "Add or Remove Programs is still finding out which package this "
                             "program came from. Try again in a moment.");
        return;
    }
    /* The package's other programs go with it. */
    GString *also = g_string_new(nullptr);
    for (guint i = 0; i < w->programs->len; i++) {
        program *o = g_ptr_array_index(w->programs, i);
        if (o != p && g_strcmp0(o->package_id, p->package_id) == 0)
            g_string_append_printf(also, "%s%s", also->len > 0 ? ", " : "", o->name);
    }
    xd_appwiz_remove_package(w, p->name, p->package_id, also->len > 0 ? also->str : nullptr,
                             after_change, nullptr);
    g_string_free(also, TRUE);
}

static GtkWidget *right_label(const char *text, const char *css_class) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 1);
    if (css_class != nullptr)
        xd_appwiz_add_class(l, css_class);
    return l;
}

static void extra(program *p, GtkWidget *widget) {
    gtk_widget_set_no_show_all(widget, TRUE);
    g_ptr_array_add(p->extra, widget);
}

/* XP's row: icon, name, Size; selected, also Used, Last Used On, the
 * support link and Remove. */
static GtkWidget *program_row(appwiz *w, program *p) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 4);
    GtkWidget *icon = p->icon != nullptr ? gtk_image_new_from_gicon(p->icon, GTK_ICON_SIZE_DND)
                                         : gtk_image_new_from_icon_name("application-x-executable",
                                                                        GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), icon, 0, 0, 1, 3);
    GtkWidget *name = xd_appwiz_label(p->name, "xd-arp-name");
    gtk_label_set_line_wrap(GTK_LABEL(name), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_grid_attach(GTK_GRID(grid), name, 1, 0, 1, 1);

    p->extra = g_ptr_array_new();
    p->size_key = right_label("Size", "xd-arp-key");
    p->size = right_label("", nullptr);
    gtk_widget_set_size_request(p->size, 80, -1);
    gtk_widget_set_no_show_all(p->size_key, TRUE);
    gtk_widget_set_no_show_all(p->size, TRUE);
    gtk_grid_attach(GTK_GRID(grid), p->size_key, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->size, 3, 0, 1, 1);
    show_size(p);

    int r = 1;
    const char *used = xd_appwiz_usage_name(xd_appwiz_usage_for(p->key.launches, w->have_counts));
    if (used != nullptr) {
        GtkWidget *k = right_label("Used", "xd-arp-key"), *v = right_label(used, nullptr);
        extra(p, k);
        extra(p, v);
        gtk_grid_attach(GTK_GRID(grid), k, 2, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), v, 3, r++, 1, 1);
    }
    if (p->key.last_used > 0) {
        char *date = xd_appwiz_format_date(p->key.last_used);
        GtkWidget *k = right_label("Last Used On", "xd-arp-key"), *v = right_label(date, nullptr);
        g_free(date);
        extra(p, k);
        extra(p, v);
        gtk_grid_attach(GTK_GRID(grid), k, 2, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), v, 3, r++, 1, 1);
    }

    GtkWidget *link = gtk_button_new_with_label("Click here for support information.");
    gtk_button_set_relief(GTK_BUTTON(link), GTK_RELIEF_NONE);
    xd_appwiz_add_class(link, "xd-arp-link");
    gtk_widget_set_halign(link, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(link), "program", p);
    g_signal_connect(link, "clicked", G_CALLBACK(on_support), w);
    extra(p, link);
    gtk_grid_attach(GTK_GRID(grid), link, 1, 1, 1, 1);

    bool removable = p->src != SOURCE_NONE;
    GtkWidget *how = xd_appwiz_label(removable
                                         ? "To remove this program from your computer, click "
                                           "Remove."
                                         : "This program wasn't installed with a package "
                                           "manager, so it can't be removed here.",
                                     nullptr);
    extra(p, how);
    gtk_grid_attach(GTK_GRID(grid), how, 1, XD_MAX(r, 2), 1, 1);
    GtkWidget *remove = gtk_button_new_with_mnemonic("_Remove");
    gtk_widget_set_size_request(remove, 80, -1);
    gtk_widget_set_halign(remove, GTK_ALIGN_END);
    gtk_widget_set_sensitive(remove, removable);
    g_object_set_data(G_OBJECT(remove), "program", p);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), w);
    extra(p, remove);
    gtk_grid_attach(GTK_GRID(grid), remove, 2, XD_MAX(r, 2), 2, 1);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), grid);
    g_object_set_data(G_OBJECT(row), "program", p);
    p->row = row;
    return row;
}

/* ---- PackageKit lookups ------------------------------------------------------ */

typedef struct lookup {
    appwiz *w;
    unsigned generation;
    program *p;
} lookup;

static void update_status(appwiz *w) {
    const char *text = nullptr;
    if (w->pk_missing)
        text = "Sizes and removal of packages need PackageKit, which isn't available.";
    else if (w->pending > 0 || w->lookups.length > 0)
        text = "Finding out which packages the programs came from...";
    gtk_label_set_text(GTK_LABEL(w->status), text != nullptr ? text : "");
    gtk_widget_set_visible(w->status, text != nullptr);
}

static void apply_details(appwiz *w, PkDetails *d) {
    const char *id = pk_details_get_package_id(d);
    char *version = nullptr, *repo = nullptr;
    xd_appwiz_split_package_id(id, nullptr, &version, &repo);
    for (guint i = 0; i < w->programs->len; i++) {
        program *p = g_ptr_array_index(w->programs, i);
        if (p->src != SOURCE_PACKAGE || g_strcmp0(p->package_id, id) != 0)
            continue;
        p->key.size = pk_details_get_size(d);
        g_free(p->version);
        p->version = g_strdup(version);
        g_free(p->publisher);
        p->publisher = repo != nullptr && repo[0] != '\0' ? g_strdup(repo) : nullptr;
        g_free(p->url);
        p->url = g_strdup(pk_details_get_url(d));
        g_free(p->description);
        const char *desc = pk_details_get_description(d);
        p->description = g_strdup(desc != nullptr && desc[0] != '\0' ? desc
                                                                      : pk_details_get_summary(d));
        g_free(p->license);
        p->license = g_strdup(pk_details_get_license(d));
        show_size(p);
    }
    g_free(version);
    g_free(repo);
}

static void on_details(GObject *src, GAsyncResult *res, gpointer data) {
    lookup *l = data;
    appwiz *w = l->w;
    GError *error = nullptr;
    PkResults *r = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        w->pending--;
        if (l->generation == w->generation && r != nullptr) {
            GPtrArray *details = pk_results_get_details_array(r);
            for (guint i = 0; i < details->len; i++) {
                PkDetails *d = g_ptr_array_index(details, i);
                g_hash_table_replace(w->details, g_strdup(pk_details_get_package_id(d)),
                                     g_object_ref(d));
                apply_details(w, d);
            }
            g_ptr_array_unref(details);
            resort_later(w);
        }
        update_status(w);
    }
    g_clear_error(&error);
    g_clear_object(&r);
    g_free(l);
}

static void flush_details(appwiz *w) {
    if (w->detail_ids->len == 0)
        return;
    g_ptr_array_add(w->detail_ids, nullptr);
    char **ids = (char **)g_ptr_array_free(w->detail_ids, FALSE);
    w->detail_ids = g_ptr_array_new_with_free_func(g_free);
    lookup *l = g_new0(lookup, 1);
    *l = (lookup){w, w->generation, nullptr};
    w->pending++;
    pk_client_get_details_async(w->pk, ids, w->quit, nullptr, nullptr, on_details, l);
    g_strfreev(ids);
}

static void next_lookup(appwiz *w);

static void on_searched(GObject *src, GAsyncResult *res, gpointer data) {
    lookup *l = data;
    appwiz *w = l->w;
    GError *error = nullptr;
    PkResults *r = pk_client_generic_finish(PK_CLIENT(src), res, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        g_free(l);
        return;
    }
    w->pending--;
    if (l->generation == w->generation) {
        if (r == nullptr) {
            /* No PackageKit: stop asking. */
            g_printerr("xd-appwiz: PackageKit: %s\n", error->message);
            w->pk_missing = true;
            g_queue_clear(&w->lookups);
        } else {
            GPtrArray *pkgs = pk_results_get_package_array(r);
            if (pkgs->len > 0) {
                const char *id = pk_package_get_id(g_ptr_array_index(pkgs, 0));
                l->p->package_id = g_strdup(id);
                /* The package's other programs may have brought its
                 * details already. */
                PkDetails *known = g_hash_table_lookup(w->details, id);
                if (known != nullptr) {
                    apply_details(w, known);
                } else if (!g_hash_table_contains(w->asked, id)) {
                    g_hash_table_add(w->asked, g_strdup(id));
                    g_ptr_array_add(w->detail_ids, g_strdup(id));
                }
            } else {
                /* Not from a package after all. */
                l->p->src = SOURCE_NONE;
            }
            g_ptr_array_unref(pkgs);
            if (w->detail_ids->len >= DETAILS_BATCH)
                flush_details(w);
        }
        next_lookup(w);
    }
    g_clear_error(&error);
    g_clear_object(&r);
    g_free(l);
}

/* One desktop file at a time: some backends (alpm) only match a single
 * path per SearchFiles. Each takes a few milliseconds. */
static void next_lookup(appwiz *w) {
    program *p = g_queue_pop_head(&w->lookups);
    if (p == nullptr) {
        flush_details(w);
        update_status(w);
        return;
    }
    char *values[] = {p->path, nullptr};
    lookup *l = g_new0(lookup, 1);
    *l = (lookup){w, w->generation, p};
    w->pending++;
    pk_client_search_files_async(w->pk, pk_bitfield_value(PK_FILTER_ENUM_INSTALLED), values,
                                 w->quit, nullptr, nullptr, on_searched, l);
    update_status(w);
}

/* ---- loading ---------------------------------------------------------------- */

static void load_usage(appwiz *w) {
    g_clear_pointer(&w->counts, g_hash_table_unref);
    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines("start-frequent", &lines, &n, 200);
    w->counts = xd_appwiz_parse_launch_counts(lines, n);
    w->have_counts = g_hash_table_size(w->counts) > 0;
    xd_free_lines(lines, n);

    g_clear_pointer(&w->launched, g_hash_table_unref);
    xd_state_read_lines("start-last-used", &lines, &n, 200);
    w->launched = xd_appwiz_parse_launch_times(lines, n);
    xd_free_lines(lines, n);

    g_clear_pointer(&w->recent, g_bookmark_file_free);
    w->recent = g_bookmark_file_new();
    char *xbel = g_build_filename(g_get_user_data_dir(), "recently-used.xbel", nullptr);
    if (!g_bookmark_file_load_from_file(w->recent, xbel, nullptr))
        g_clear_pointer(&w->recent, g_bookmark_file_free);
    g_free(xbel);
}

static program *new_program(appwiz *w, GDesktopAppInfo *info) {
    GAppInfo *app = G_APP_INFO(info);
    program *p = g_new0(program, 1);
    p->id = g_strdup(g_app_info_get_id(app));
    p->name = g_strdup(g_app_info_get_name(app));
    p->key.name = p->name;
    p->path = g_strdup(g_desktop_app_info_get_filename(info));
    p->exec = g_strdup(g_app_info_get_executable(app));
    GIcon *icon = g_app_info_get_icon(app);
    p->icon = icon != nullptr ? g_object_ref(icon) : nullptr;
    p->key.launches = xd_appwiz_launches(w->counts, p->id);
    /* Started from the Start menu or the taskbar, or opening a document
     * (from wherever it was started), whichever was later. */
    p->key.last_used = XD_MAX(xd_appwiz_last_used(w->recent, p->id, p->exec),
                              xd_appwiz_launch_time(w->launched, p->id));

    char *flatpak = g_desktop_app_info_get_string(info, "X-Flatpak");
    flatpak_app *f = flatpak != nullptr ? g_hash_table_lookup(w->flatpaks, flatpak) : nullptr;
    g_free(flatpak);
    if (f != nullptr) {
        p->src = SOURCE_FLATPAK;
        p->package_id = g_strdup(f->ref);
        p->installation = g_strdup(f->installation);
        p->key.size = f->size;
        p->version = g_strdup(f->version);
        p->publisher = g_strdup(f->publisher);
        p->url = g_strdup(f->homepage);
        p->description = g_strdup(f->summary);
        p->license = g_strdup(f->license);
    } else if (p->path != nullptr && !g_str_has_prefix(p->path, g_get_user_data_dir()) &&
               !g_str_has_prefix(p->path, g_get_home_dir())) {
        /* Packages don't install into home folders. */
        p->src = SOURCE_PACKAGE;
    }
    return p;
}

static void load_programs(appwiz *w) {
    if (w->reload_idle != 0) {
        g_source_remove(w->reload_idle);
        w->reload_idle = 0;
    }
    w->generation++;
    w->pk_missing = false;
    g_queue_clear(&w->lookups);
    g_ptr_array_set_size(w->detail_ids, 0);
    g_hash_table_remove_all(w->asked);
    g_hash_table_remove_all(w->details);
    /* Unselected while every row is still there: removing the selected
     * row would report the selection change half way through. */
    gtk_list_box_unselect_all(GTK_LIST_BOX(w->list));
    GList *kids = gtk_container_get_children(GTK_CONTAINER(w->list));
    for (GList *k = kids; k != nullptr; k = k->next)
        gtk_widget_destroy(k->data);
    g_list_free(kids);
    g_ptr_array_set_size(w->programs, 0);

    load_usage(w);
    index_flatpaks(w);
    GList *all = g_app_info_get_all();
    for (GList *a = all; a != nullptr; a = a->next) {
        GAppInfo *app = a->data;
        if (!G_IS_DESKTOP_APP_INFO(app) || !g_app_info_should_show(app))
            continue;
        program *p = new_program(w, G_DESKTOP_APP_INFO(app));
        g_ptr_array_add(w->programs, p);
        gtk_container_add(GTK_CONTAINER(w->list), program_row(w, p));
        if (p->src == SOURCE_PACKAGE)
            g_queue_push_tail(&w->lookups, p);
    }
    g_list_free_full(all, g_object_unref);
    gtk_widget_show_all(w->list);
    next_lookup(w);
}

void xd_appwiz_reload(appwiz *w) { load_programs(w); }

static gboolean reload_now(gpointer data) {
    appwiz *w = data;
    /* Not while something is being installed or removed: the list would
     * change under the question and the progress dialog. */
    if (w->busy)
        return G_SOURCE_CONTINUE;
    w->reload_idle = 0;
    load_programs(w);
    return G_SOURCE_REMOVE;
}

/* Desktop files come and go as programs are installed and removed. */
static void on_apps_changed([[maybe_unused]] GAppInfoMonitor *m, gpointer data) {
    appwiz *w = data;
    if (w->reload_idle == 0)
        w->reload_idle = g_timeout_add(500, reload_now, w);
}

static void on_sort_changed([[maybe_unused]] GtkComboBox *c, gpointer data) {
    appwiz *w = data;
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(w->list));
}

static GtkWidget *remove_page(appwiz *w) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title = xd_appwiz_label("Currently installed programs:", "xd-arp-heading");
    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    GtkWidget *sort_label = gtk_label_new_with_mnemonic("S_ort by:");
    w->sort = gtk_combo_box_text_new();
    static const char *const sorts[] = {"Name", "Size", "Frequency of use", "Date last used"};
    for (size_t i = 0; i < XD_ARRAY_LEN(sorts); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->sort), sorts[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->sort), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(sort_label), w->sort);
    g_signal_connect(w->sort, "changed", G_CALLBACK(on_sort_changed), w);
    gtk_box_pack_start(GTK_BOX(top), sort_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), w->sort, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    w->list = gtk_list_box_new();
    xd_appwiz_add_class(w->list, "xd-arp-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(w->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(w->list), sort_rows, w, nullptr);
    g_signal_connect(w->list, "row-selected", G_CALLBACK(on_row_selected), w);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), w->list);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    w->status = xd_appwiz_label("", "xd-arp-dim");
    gtk_widget_set_no_show_all(w->status, TRUE);
    gtk_box_pack_start(GTK_BOX(box), w->status, FALSE, FALSE, 0);
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

void xd_appwiz_show_page(appwiz *w, xd_appwiz_page page) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->tabs[page]), TRUE);
}

static void on_tab(GtkToggleButton *b, gpointer data) {
    appwiz *w = data;
    if (gtk_toggle_button_get_active(b))
        gtk_stack_set_visible_child_name(GTK_STACK(w->stack),
                                         g_object_get_data(G_OBJECT(b), "page"));
}

static GtkWidget *tab(appwiz *w, GtkWidget *group, const char *icon, const char *text,
                      const char *page) {
    GtkWidget *b = group != nullptr
                       ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(group))
                       : gtk_radio_button_new(nullptr);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    gtk_box_pack_start(GTK_BOX(col), img, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new_with_mnemonic(text);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 10);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), b);
    gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), col);
    g_object_set_data(G_OBJECT(b), "page", (gpointer)(uintptr_t)page);
    g_signal_connect(b, "toggled", G_CALLBACK(on_tab), w);
    return b;
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

    static appwiz w;
    w.quit = g_cancellable_new();
    w.pk = pk_client_new();
    pk_client_set_interactive(w.pk, FALSE);
    w.pk_task = pk_client_new();
    pk_client_set_interactive(w.pk_task, TRUE);
    w.control = pk_control_new();
    w.programs = g_ptr_array_new_with_free_func(program_free);
    w.flatpaks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, flatpak_app_free);
    w.detail_ids = g_ptr_array_new_with_free_func(g_free);
    w.asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    w.details = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    g_queue_init(&w.lookups);

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    w.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w.window), XD_APPWIZ_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(w.window), "system-software-install");
    gtk_window_set_default_size(GTK_WINDOW(w.window), 760, 540);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(w.window), body);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 4);
    gtk_widget_set_size_request(bar, 96, -1);
    GtkWidget *bar_bg = gtk_event_box_new();
    xd_appwiz_add_class(bar_bg, "xd-arp-bar");
    gtk_container_add(GTK_CONTAINER(bar_bg), bar);
    gtk_box_pack_start(GTK_BOX(body), bar_bg, FALSE, FALSE, 0);
    w.tabs[XD_PAGE_REMOVE] = tab(&w, nullptr, "system-software-install",
                                 "C_hange or Remove Programs", "remove");
    w.tabs[XD_PAGE_ADD] = tab(&w, w.tabs[0], "media-optical", "Add _New Programs", "add");
    w.tabs[XD_PAGE_COMPONENTS] = tab(&w, w.tabs[0], "applications-system",
                                     "Add/Remove _Windows Components", "components");
    for (size_t i = 0; i < XD_ARRAY_LEN(w.tabs); i++)
        gtk_box_pack_start(GTK_BOX(bar), w.tabs[i], FALSE, FALSE, 0);

    w.stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(w.stack), remove_page(&w), "remove");
    gtk_stack_add_named(GTK_STACK(w.stack), xd_appwiz_add_page(&w), "add");
    gtk_stack_add_named(GTK_STACK(w.stack), xd_appwiz_components_page(&w), "components");
    gtk_box_pack_start(GTK_BOX(body), w.stack, TRUE, TRUE, 0);

    g_signal_connect(w.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(w.window);
    xd_appwiz_show_page(&w, start);
    gtk_stack_set_visible_child_name(GTK_STACK(w.stack),
                                     g_object_get_data(G_OBJECT(w.tabs[start]), "page"));
    load_programs(&w);
    /* Typing goes to the page, not to the buttons down the left. */
    gtk_widget_grab_focus(start == XD_PAGE_ADD ? w.search : start == XD_PAGE_REMOVE ? w.list
                                                                                    : w.groups);
    GAppInfoMonitor *monitor = g_app_info_monitor_get();
    w.monitor_handler = g_signal_connect(monitor, "changed", G_CALLBACK(on_apps_changed), &w);
    gtk_main();

    g_cancellable_cancel(w.quit);
    g_signal_handler_disconnect(monitor, w.monitor_handler);
    g_object_unref(monitor);
    if (w.reload_idle != 0)
        g_source_remove(w.reload_idle);
    if (w.sort_idle != 0)
        g_source_remove(w.sort_idle);
    g_queue_clear(&w.lookups);
    g_ptr_array_free(w.programs, TRUE);
    g_ptr_array_free(w.detail_ids, TRUE);
    g_hash_table_unref(w.asked);
    g_hash_table_unref(w.details);
    g_hash_table_unref(w.flatpaks);
    g_clear_pointer(&w.counts, g_hash_table_unref);
    g_clear_pointer(&w.recent, g_bookmark_file_free);
    g_clear_pointer(&w.launched, g_hash_table_unref);
    g_clear_pointer(&w.result_rows, g_hash_table_unref);
    g_object_unref(w.pk);
    g_object_unref(w.pk_task);
    g_object_unref(w.control);
    g_object_unref(w.quit);
    return EXIT_SUCCESS;
}
