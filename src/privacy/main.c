/* SPDX-License-Identifier: MIT
 * xd-privacy: Privacy Properties, a Linux applet in an XP property sheet.
 *
 *   General              recent documents (XP's "List my most recently
 *                        opened documents" and Clear List), and emptying
 *                        old Recycle Bin items and temporary files
 *   Location             whether programs may find out where you are, and
 *                        which ones asked
 *   Program Permissions  what programs were allowed through the portals:
 *                        camera, microphone, notifications...
 *
 * The settings are GNOME's (org.gnome.desktop.privacy and
 * org.gnome.system.location), which GTK, xdg-desktop-portal and GeoClue's
 * agents already honour; xd-shell honours them in its own recent lists
 * and empties the old files itself (src/shell/housekeeping.c), as
 * gsd-housekeeping does under GNOME. Answers given to programs are in
 * xdg-desktop-portal's permission store (permissions.c); changing or
 * removing one takes effect at once, as in GNOME Settings.
 */
#include <stdlib.h>
#include <string.h>

#include <gio/gdesktopappinfo.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "permissions.h"

#define PRIVACY_SCHEMA "org.gnome.desktop.privacy"
#define LOCATION_SCHEMA "org.gnome.system.location"
#define STORE "org.freedesktop.impl.portal.PermissionStore"
#define STORE_PATH "/org/freedesktop/impl/portal/PermissionStore"
#define GEOCLUE "org.freedesktop.GeoClue2"
#define GEOCLUE_PATH "/org/freedesktop/GeoClue2/Manager"
#define GEOCLUE_MANAGER "org.freedesktop.GeoClue2.Manager"

/* "Keep the list for": GNOME's choices; -1 is for ever. */
static const int keep_days[] = {1, 7, 30, -1};

enum { L_ICON, L_NAME, L_ACCESS, L_USED, L_ENTRY, L_COLS };
enum { P_ICON, P_NAME, P_ACCESS, P_ENTRY, P_APP, P_COLS };

typedef struct props {
    GtkWidget *window, *apply;
    bool updating; /* we're setting the widgets ourselves */
    GSettings *privacy, *location; /* nullptr: schema not installed */
    /* General */
    GtkWidget *remember, *keep, *clear, *trash, *temp, *age;
    /* Location */
    GtkWidget *loc_enabled, *loc_status, *loc_view;
    GtkWidget *loc_allow, *loc_deny, *loc_remove;
    GtkListStore *loc_list;
    /* Program Permissions */
    GtkWidget *perm_view, *perm_note;
    GtkWidget *perm_allow, *perm_deny, *perm_remove;
    GtkTreeStore *perm_tree;
    /* the permission store */
    GDBusConnection *bus;
    guint store_sub;
    GPtrArray *entries; /* XdPermEntry, as last listed; nullptr: no store */
    /* GeoClue on the system bus */
    GDBusConnection *system;
    GDBusProxy *geoclue;
    guint geoclue_watch;
    bool geoclue_installed;
} props;

static void changed(props *p) {
    if (!p->updating)
        gtk_widget_set_sensitive(p->apply, TRUE);
}

static GSettings *settings_for(const char *id, const char *const *keys) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema =
        src != nullptr ? g_settings_schema_source_lookup(src, id, TRUE) : nullptr;
    if (schema == nullptr)
        return nullptr;
    bool all = true;
    for (const char *const *k = keys; *k != nullptr; k++)
        all = all && g_settings_schema_has_key(schema, *k);
    GSettings *s = all ? g_settings_new_full(schema, nullptr, nullptr) : nullptr;
    g_settings_schema_unref(schema);
    return s;
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
}

static GtkWidget *icon_row(const char *icon, GtkWidget *text) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    return row;
}

static GtkWidget *button(const char *text, GCallback cb, props *p) {
    GtkWidget *b = gtk_button_new_with_mnemonic(text);
    gtk_widget_set_size_request(b, 75, -1);
    g_signal_connect_swapped(b, "clicked", cb, p);
    return b;
}

static void error_box(props *p, const char *what, const GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error->message);
    xd_message_box(GTK_WINDOW(p->window), "Privacy", XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

/* ---- General ------------------------------------------------------------------- */

static void general_sensitivity(props *p) {
    bool remember = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->remember));
    gtk_widget_set_sensitive(p->remember, p->privacy != nullptr);
    gtk_widget_set_sensitive(p->keep, p->privacy != nullptr && remember);
    gtk_widget_set_sensitive(p->trash, p->privacy != nullptr);
    gtk_widget_set_sensitive(p->temp, p->privacy != nullptr);
    gtk_widget_set_sensitive(p->age, p->privacy != nullptr &&
                                         (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->trash)) ||
                                          gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->temp))));
}

static void on_general(props *p) {
    general_sensitivity(p);
    changed(p);
}

/* Shows the settings; a "keep" value of GNOME's own that isn't one of
 * ours gets an entry of its own. */
static void show_general(props *p) {
    p->updating = true;
    if (p->privacy != nullptr) {
        int days = g_settings_get_int(p->privacy, "recent-files-max-age");
        char id[16];
        g_snprintf(id, sizeof id, "%d", days);
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->keep), id)) {
            char *text = g_strdup_printf("%d days", days);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->keep), id, text);
            g_free(text);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->keep), id);
        }
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(p->remember),
            g_settings_get_boolean(p->privacy, "remember-recent-files") && days != 0);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->trash),
                                     g_settings_get_boolean(p->privacy, "remove-old-trash-files"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->temp),
                                     g_settings_get_boolean(p->privacy, "remove-old-temp-files"));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->age),
                                  g_settings_get_uint(p->privacy, "old-files-age"));
    } else {
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->keep), "-1");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->remember), TRUE);
    }
    general_sensitivity(p);
    p->updating = false;
}

/* XP's Clear List: at once, not on OK. The list is rewritten empty
 * through GBookmarkFile; GTK programs see the file change and reload. */
static void on_clear(props *p) {
    char *path = g_build_filename(g_get_user_data_dir(), "recently-used.xbel", nullptr);
    GBookmarkFile *bf = g_bookmark_file_new();
    GError *error = nullptr;
    if (g_file_test(path, G_FILE_TEST_EXISTS) && !g_bookmark_file_to_file(bf, path, &error)) {
        error_box(p, "The list of recent documents could not be cleared.", error);
        g_error_free(error);
    }
    g_bookmark_file_free(bf);
    g_free(path);
}

static GtkWidget *general_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *recent = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(
        GTK_BOX(recent),
        icon_row("document-open-recent",
                 label("This list provides quick access to the documents you opened most "
                       "recently. Clearing this list does not delete the documents.")),
        FALSE, FALSE, 0);
    p->remember = gtk_check_button_new_with_mnemonic("_List my most recently opened documents");
    gtk_box_pack_start(GTK_BOX(recent), p->remember, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *keep_label = gtk_label_new_with_mnemonic("_Keep the list for:");
    p->keep = gtk_combo_box_text_new();
    for (size_t i = 0; i < G_N_ELEMENTS(keep_days); i++) {
        char id[16];
        g_snprintf(id, sizeof id, "%d", keep_days[i]);
        char *text = keep_days[i] < 0    ? g_strdup("Until I clear it")
                     : keep_days[i] == 1 ? g_strdup("1 day")
                                         : g_strdup_printf("%d days", keep_days[i]);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->keep), id, text);
        g_free(text);
    }
    gtk_label_set_mnemonic_widget(GTK_LABEL(keep_label), p->keep);
    gtk_widget_set_margin_start(keep_label, 20);
    gtk_box_pack_start(GTK_BOX(row), keep_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->keep, FALSE, FALSE, 0);
    p->clear = button("_Clear List", G_CALLBACK(on_clear), p);
    gtk_box_pack_end(GTK_BOX(row), p->clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(recent), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Recent documents", recent), FALSE, FALSE, 0);

    GtkWidget *old = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(old),
                       icon_row("user-trash-full",
                                label("Files you deleted stay in the Recycle Bin, and programs "
                                      "leave temporary files behind, until they're removed. "
                                      "XeonDesktop can remove old ones once a day.")),
                       FALSE, FALSE, 0);
    p->trash = gtk_check_button_new_with_mnemonic("_Empty old items from the Recycle Bin");
    p->temp = gtk_check_button_new_with_mnemonic("_Delete old temporary files");
    gtk_box_pack_start(GTK_BOX(old), p->trash, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(old), p->temp, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *age_label = gtk_label_new_with_mnemonic("Older _than:");
    p->age = gtk_spin_button_new_with_range(1, 365, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(p->age), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(age_label), p->age);
    gtk_widget_set_margin_start(age_label, 20);
    gtk_box_pack_start(GTK_BOX(row), age_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->age, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("days"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(old), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Recycle Bin and temporary files", old), FALSE, FALSE,
                       0);

    show_general(p);
    g_signal_connect_swapped(p->remember, "toggled", G_CALLBACK(on_general), p);
    g_signal_connect_swapped(p->keep, "changed", G_CALLBACK(on_general), p);
    g_signal_connect_swapped(p->trash, "toggled", G_CALLBACK(on_general), p);
    g_signal_connect_swapped(p->temp, "toggled", G_CALLBACK(on_general), p);
    g_signal_connect_swapped(p->age, "value-changed", G_CALLBACK(on_general), p);
    return page;
}

/* ---- the permission store ------------------------------------------------------- */

/* A program's name and icon, from its desktop entry if it has one. */
static char *app_name(const char *app, GIcon **icon) {
    *icon = nullptr;
    if (app[0] == '\0') {
        *icon = g_themed_icon_new("application-x-executable");
        return g_strdup("Programs outside a sandbox");
    }
    char *id = g_strconcat(app, ".desktop", nullptr);
    GDesktopAppInfo *info = g_desktop_app_info_new(id);
    g_free(id);
    char *name;
    if (info != nullptr) {
        name = g_strdup(g_app_info_get_display_name(G_APP_INFO(info)));
        GIcon *i = g_app_info_get_icon(G_APP_INFO(info));
        *icon = i != nullptr ? g_object_ref(i) : nullptr;
        g_object_unref(info);
    } else {
        name = g_strdup(app);
    }
    if (*icon == nullptr)
        *icon = g_themed_icon_new("application-x-executable");
    return name;
}

static const char *access_text(const XdPermEntry *e) {
    if (e->values != nullptr && e->values[0] != nullptr && strcmp(e->values[0], "ask") == 0)
        return "Ask";
    return xd_perm_allowed(e->kind, (const char *const *)e->values) ? "Allowed" : "Denied";
}

/* The entry a list row stands for, or nullptr (a program's row). */
static const XdPermEntry *row_entry(props *p, GtkTreeModel *model, GtkTreeIter *it, int col) {
    int i = -1;
    gtk_tree_model_get(model, it, col, &i, -1);
    return p->entries != nullptr && i >= 0 && (guint)i < p->entries->len
               ? g_ptr_array_index(p->entries, i)
               : nullptr;
}

static const XdPermEntry *selected(props *p, GtkWidget *view, int col, char **app) {
    GtkTreeModel *model;
    GtkTreeIter it;
    if (app != nullptr)
        *app = nullptr;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model,
                                         &it))
        return nullptr;
    if (app != nullptr && col == P_ENTRY)
        gtk_tree_model_get(model, &it, P_APP, app, -1);
    return row_entry(p, model, &it, col);
}

static void loc_buttons(props *p) {
    const XdPermEntry *e = selected(p, p->loc_view, L_ENTRY, nullptr);
    bool allowed = e != nullptr && xd_perm_allowed(e->kind, (const char *const *)e->values);
    gtk_widget_set_sensitive(p->loc_allow, e != nullptr && !allowed);
    gtk_widget_set_sensitive(p->loc_deny, e != nullptr && allowed);
    gtk_widget_set_sensitive(p->loc_remove, e != nullptr);
}

static void perm_buttons(props *p) {
    char *app = nullptr;
    const XdPermEntry *e = selected(p, p->perm_view, P_ENTRY, &app);
    bool allowed = e != nullptr && xd_perm_allowed(e->kind, (const char *const *)e->values);
    gtk_widget_set_sensitive(p->perm_allow, e != nullptr && !allowed);
    gtk_widget_set_sensitive(p->perm_deny, e != nullptr && allowed);
    /* A program's row: Remove forgets all its answers. */
    gtk_widget_set_sensitive(p->perm_remove, e != nullptr || app != nullptr);
    g_free(app);
}

/* Selects the row for (app, kind) again after the lists are refilled;
 * kind nullptr for a program's row. */
static void reselect(GtkWidget *view, int entry_col, int app_col, props *p, const char *app,
                     const XdPermKind *kind) {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter top, child;
    for (bool ok = gtk_tree_model_get_iter_first(model, &top); ok;
         ok = gtk_tree_model_iter_next(model, &top)) {
        const XdPermEntry *e = row_entry(p, model, &top, entry_col);
        char *top_app = nullptr;
        if (app_col >= 0)
            gtk_tree_model_get(model, &top, app_col, &top_app, -1);
        bool same_app = top_app != nullptr ? strcmp(top_app, app) == 0
                                           : e != nullptr && strcmp(e->app, app) == 0;
        g_free(top_app);
        if (!same_app)
            continue;
        GtkTreeIter *hit = nullptr;
        if (kind == nullptr || (e != nullptr && e->kind == kind)) {
            hit = &top;
        } else {
            for (bool c = gtk_tree_model_iter_children(model, &child, &top); c && hit == nullptr;
                 c = gtk_tree_model_iter_next(model, &child)) {
                const XdPermEntry *ce = row_entry(p, model, &child, entry_col);
                if (ce != nullptr && ce->kind == kind)
                    hit = &child;
            }
        }
        if (hit != nullptr) {
            GtkTreePath *path = gtk_tree_model_get_path(model, hit);
            gtk_tree_view_expand_to_path(GTK_TREE_VIEW(view), path);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), path, nullptr, FALSE);
            gtk_tree_path_free(path);
            return;
        }
    }
}

/* Shows `entries` (taken) instead of what was listed before. */
static void fill_lists(props *p, GPtrArray *entries) {
    /* What was selected, to select it again. */
    char *loc_app = nullptr, *perm_app = nullptr;
    const XdPermEntry *le = selected(p, p->loc_view, L_ENTRY, nullptr);
    const XdPermEntry *pe = selected(p, p->perm_view, P_ENTRY, &perm_app);
    loc_app = le != nullptr ? g_strdup(le->app) : nullptr;
    const XdPermKind *perm_kind = pe != nullptr ? pe->kind : nullptr;
    if (pe != nullptr && perm_app == nullptr)
        perm_app = g_strdup(pe->app);

    /* The rows hold indices into the old list until they're gone. */
    gtk_list_store_clear(p->loc_list);
    gtk_tree_store_clear(p->perm_tree);
    if (p->entries != nullptr)
        g_ptr_array_unref(p->entries);
    p->entries = entries;
    if (p->entries != nullptr) {
        GtkTreeIter app_row;
        const char *last_app = nullptr;
        for (guint i = 0; i < p->entries->len; i++) {
            const XdPermEntry *e = g_ptr_array_index(p->entries, i);
            GIcon *icon;
            char *name = app_name(e->app, &icon);
            if (e->kind == &xd_perm_kinds[0]) {
                gint64 used = xd_perm_last_used(e);
                GDateTime *dt = used > 0 ? g_date_time_new_from_unix_local(used) : nullptr;
                char *when = dt != nullptr ? g_date_time_format(dt, "%x %X") : g_strdup("");
                if (dt != nullptr)
                    g_date_time_unref(dt);
                gtk_list_store_insert_with_values(p->loc_list, nullptr, -1, L_ICON, icon, L_NAME,
                                                  name, L_ACCESS, access_text(e), L_USED, when,
                                                  L_ENTRY, (int)i, -1);
                g_free(when);
            }
            if (last_app == nullptr || strcmp(last_app, e->app) != 0) {
                gtk_tree_store_insert_with_values(p->perm_tree, &app_row, nullptr, -1, P_ICON,
                                                  icon, P_NAME, name, P_ACCESS, "", P_ENTRY, -1,
                                                  P_APP, e->app, -1);
                last_app = e->app;
            }
            gtk_tree_store_insert_with_values(p->perm_tree, nullptr, &app_row, -1, P_NAME,
                                              e->kind->label, P_ACCESS, access_text(e), P_ENTRY,
                                              (int)i, -1);
            g_object_unref(icon);
            g_free(name);
        }
    }
    gtk_tree_view_expand_all(GTK_TREE_VIEW(p->perm_view));
    if (loc_app != nullptr)
        reselect(p->loc_view, L_ENTRY, -1, p, loc_app, &xd_perm_kinds[0]);
    if (perm_app != nullptr)
        reselect(p->perm_view, P_ENTRY, P_APP, p, perm_app, perm_kind);
    g_free(loc_app);
    g_free(perm_app);

    gtk_label_set_text(GTK_LABEL(p->perm_note),
                       p->entries == nullptr
                           ? "The portal permission store (xdg-desktop-portal) can't be reached."
                       : p->entries->len == 0 ? "No program has asked for any of these yet."
                                              : "");
    gtk_widget_set_visible(p->perm_note, p->entries == nullptr || p->entries->len == 0);
    loc_buttons(p);
    perm_buttons(p);
}

static void refresh(props *p) {
    GError *error = nullptr;
    GPtrArray *entries = p->bus != nullptr ? xd_perm_list(p->bus, nullptr, &error) : nullptr;
    if (error != nullptr) {
        g_printerr("xd-privacy: %s\n", error->message);
        g_error_free(error);
    }
    fill_lists(p, entries);
}

static void store_changed([[maybe_unused]] GDBusConnection *c, [[maybe_unused]] const char *sender,
                          [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                          [[maybe_unused]] const char *signal, [[maybe_unused]] GVariant *params,
                          gpointer data) {
    refresh(data);
}

static void set_entry(props *p, const XdPermEntry *e, bool allow) {
    if (e == nullptr)
        return;
    GError *error = nullptr;
    if (!xd_perm_set(p->bus, e, allow, nullptr, &error)) {
        error_box(p, "The permission could not be changed.", error);
        g_error_free(error);
    }
    refresh(p);
}

static void remove_entries(props *p, const XdPermEntry *e, const char *app) {
    GError *error = nullptr;
    for (guint i = 0; p->entries != nullptr && i < p->entries->len && error == nullptr; i++) {
        const XdPermEntry *x = g_ptr_array_index(p->entries, i);
        if (x == e || (e == nullptr && app != nullptr && strcmp(x->app, app) == 0))
            xd_perm_remove(p->bus, x, nullptr, &error);
    }
    if (error != nullptr) {
        error_box(p, "The permission could not be removed.", error);
        g_error_free(error);
    }
    refresh(p);
}

static void on_loc_allow(props *p) { set_entry(p, selected(p, p->loc_view, L_ENTRY, nullptr), true); }
static void on_loc_deny(props *p) { set_entry(p, selected(p, p->loc_view, L_ENTRY, nullptr), false); }
static void on_loc_remove(props *p) {
    const XdPermEntry *e = selected(p, p->loc_view, L_ENTRY, nullptr);
    if (e != nullptr)
        remove_entries(p, e, nullptr);
}
static void on_perm_allow(props *p) {
    set_entry(p, selected(p, p->perm_view, P_ENTRY, nullptr), true);
}
static void on_perm_deny(props *p) {
    set_entry(p, selected(p, p->perm_view, P_ENTRY, nullptr), false);
}
static void on_perm_remove(props *p) {
    char *app = nullptr;
    const XdPermEntry *e = selected(p, p->perm_view, P_ENTRY, &app);
    if (e != nullptr || app != nullptr)
        remove_entries(p, e, app);
    g_free(app);
}

static GtkWidget *scrolled(GtkWidget *view, int height) {
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, height);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    return scroll;
}

/* A column with the program's icon before its name. */
static void name_column(GtkWidget *view, int icon_col, int name_col) {
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Program");
    GtkCellRenderer *ir = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(c, ir, FALSE);
    gtk_tree_view_column_add_attribute(c, ir, "gicon", icon_col);
    GtkCellRenderer *tr = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(c, tr, TRUE);
    gtk_tree_view_column_add_attribute(c, tr, "text", name_col);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
}

static void text_column(GtkWidget *view, const char *title, int col) {
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
        title, gtk_cell_renderer_text_new(), "text", col, nullptr);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
}

static GtkWidget *button_row(GtkWidget *a, GtkWidget *b, GtkWidget *c) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_end(GTK_BOX(row), c, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), a, FALSE, FALSE, 0);
    return row;
}

/* ---- Location ------------------------------------------------------------------- */

static void show_geoclue(props *p) {
    bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->loc_enabled));
    const char *text;
    GVariant *in_use = p->geoclue != nullptr && g_dbus_proxy_get_name_owner(p->geoclue) != nullptr
                           ? g_dbus_proxy_get_cached_property(p->geoclue, "InUse")
                           : nullptr;
    if (!p->geoclue_installed)
        text = "GeoClue, the service that finds where you are, isn't installed, so programs "
               "can't use your location.";
    else if (!on)
        text = "Programs are told your location is unavailable.";
    else if (in_use != nullptr && g_variant_get_boolean(in_use))
        text = "A program is using your location now.";
    else
        text = "No program is using your location now.";
    if (in_use != nullptr)
        g_variant_unref(in_use);
    gtk_label_set_text(GTK_LABEL(p->loc_status), text);
}

static void geoclue_props([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *c,
                          [[maybe_unused]] const char *const *inv, gpointer data) {
    show_geoclue(data);
}

static void geoclue_proxy_ready([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    props *p = data;
    GDBusProxy *proxy = g_dbus_proxy_new_finish(res, nullptr);
    if (proxy == nullptr)
        return;
    g_clear_object(&p->geoclue);
    p->geoclue = proxy;
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(geoclue_props), p);
    show_geoclue(p);
}

static void geoclue_appeared(GDBusConnection *c, [[maybe_unused]] const char *name,
                             [[maybe_unused]] const char *owner, gpointer data) {
    props *p = data;
    p->geoclue_installed = true;
    /* Never started for us: only read while it runs anyway. */
    g_dbus_proxy_new(c, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, nullptr, GEOCLUE, GEOCLUE_PATH,
                     GEOCLUE_MANAGER, nullptr, geoclue_proxy_ready, p);
}

static void geoclue_vanished([[maybe_unused]] GDBusConnection *c,
                             [[maybe_unused]] const char *name, gpointer data) {
    props *p = data;
    g_clear_object(&p->geoclue);
    show_geoclue(p);
}

/* Whether GeoClue is installed: running, or startable on demand. */
static bool geoclue_activatable(GDBusConnection *system) {
    GVariant *r = g_dbus_connection_call_sync(
        system, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "ListActivatableNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 5000,
        nullptr, nullptr);
    if (r == nullptr)
        return false;
    const char **names = nullptr;
    g_variant_get(r, "(^a&s)", &names);
    bool found = names != nullptr && g_strv_contains(names, GEOCLUE);
    g_free(names);
    g_variant_unref(r);
    return found;
}

static void on_loc_enabled(props *p) {
    show_geoclue(p);
    changed(p);
}

static void on_loc_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    loc_buttons(data);
}

static GtkWidget *location_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       icon_row("mark-location",
                                label("Programs can ask where you are, to show a map or the "
                                      "local weather. Each one asks you first.")),
                       FALSE, FALSE, 0);
    p->loc_enabled = gtk_check_button_new_with_mnemonic("Allow programs to _use my location");
    gtk_box_pack_start(GTK_BOX(page), p->loc_enabled, FALSE, FALSE, 0);
    p->loc_status = label("");
    gtk_widget_set_margin_start(p->loc_status, 20);
    gtk_box_pack_start(GTK_BOX(page), p->loc_status, FALSE, FALSE, 0);

    GtkWidget *asked = gtk_label_new_with_mnemonic("Programs that have as_ked for your location:");
    gtk_label_set_xalign(GTK_LABEL(asked), 0);
    gtk_box_pack_start(GTK_BOX(page), asked, FALSE, FALSE, 4);
    p->loc_list = gtk_list_store_new(L_COLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_INT);
    p->loc_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->loc_list));
    g_object_unref(p->loc_list);
    name_column(p->loc_view, L_ICON, L_NAME);
    text_column(p->loc_view, "Access", L_ACCESS);
    text_column(p->loc_view, "Last Used", L_USED);
    gtk_label_set_mnemonic_widget(GTK_LABEL(asked), p->loc_view);
    gtk_box_pack_start(GTK_BOX(page), scrolled(p->loc_view, 130), TRUE, TRUE, 0);
    p->loc_allow = button("A_llow", G_CALLBACK(on_loc_allow), p);
    p->loc_deny = button("_Deny", G_CALLBACK(on_loc_deny), p);
    p->loc_remove = button("_Remove", G_CALLBACK(on_loc_remove), p);
    gtk_box_pack_start(GTK_BOX(page), button_row(p->loc_allow, p->loc_deny, p->loc_remove), FALSE,
                       FALSE, 0);

    p->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->loc_enabled),
                                 p->location != nullptr &&
                                     g_settings_get_boolean(p->location, "enabled"));
    gtk_widget_set_sensitive(p->loc_enabled, p->location != nullptr);
    p->updating = false;
    g_signal_connect_swapped(p->loc_enabled, "toggled", G_CALLBACK(on_loc_enabled), p);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->loc_view)), "changed",
                     G_CALLBACK(on_loc_selection), p);
    return page;
}

/* ---- Program Permissions -------------------------------------------------------- */

static void on_perm_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    perm_buttons(data);
}

static GtkWidget *permissions_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       icon_row("preferences-security",
                                label("Programs installed as Flatpaks, and others that go "
                                      "through the desktop portals, ask before they use your "
                                      "camera, send notifications or run in the background. "
                                      "Your answers are kept here; a program whose answer you "
                                      "remove asks again.")),
                       FALSE, FALSE, 0);
    GtkWidget *answers = gtk_label_new_with_mnemonic("Your a_nswers:");
    gtk_label_set_xalign(GTK_LABEL(answers), 0);
    gtk_box_pack_start(GTK_BOX(page), answers, FALSE, FALSE, 4);
    p->perm_tree = gtk_tree_store_new(P_COLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                      G_TYPE_INT, G_TYPE_STRING);
    p->perm_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->perm_tree));
    g_object_unref(p->perm_tree);
    name_column(p->perm_view, P_ICON, P_NAME);
    text_column(p->perm_view, "Access", P_ACCESS);
    gtk_label_set_mnemonic_widget(GTK_LABEL(answers), p->perm_view);
    gtk_box_pack_start(GTK_BOX(page), scrolled(p->perm_view, 170), TRUE, TRUE, 0);
    p->perm_note = label("");
    gtk_widget_set_no_show_all(p->perm_note, TRUE);
    gtk_box_pack_start(GTK_BOX(page), p->perm_note, FALSE, FALSE, 0);
    p->perm_allow = button("A_llow", G_CALLBACK(on_perm_allow), p);
    p->perm_deny = button("_Deny", G_CALLBACK(on_perm_deny), p);
    p->perm_remove = button("_Remove", G_CALLBACK(on_perm_remove), p);
    gtk_box_pack_start(GTK_BOX(page), button_row(p->perm_allow, p->perm_deny, p->perm_remove),
                       FALSE, FALSE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->perm_view)), "changed",
                     G_CALLBACK(on_perm_selection), p);
    return page;
}

/* ---- applying ------------------------------------------------------------------- */

static void apply(props *p) {
    if (p->privacy != nullptr) {
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->keep));
        g_settings_set_boolean(p->privacy, "remember-recent-files",
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->remember)));
        if (id != nullptr)
            g_settings_set_int(p->privacy, "recent-files-max-age", atoi(id));
        g_settings_set_boolean(p->privacy, "remove-old-trash-files",
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->trash)));
        g_settings_set_boolean(p->privacy, "remove-old-temp-files",
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->temp)));
        g_settings_set_uint(p->privacy, "old-files-age",
                            (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->age)));
    }
    if (p->location != nullptr)
        g_settings_set_boolean(p->location, "enabled",
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->loc_enabled)));
    g_settings_sync();
    gtk_widget_set_sensitive(p->apply, FALSE);
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply))
        apply(p);
    gtk_widget_destroy(GTK_WIDGET(d));
}

int main(int argc, char **argv) {
    int page = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
    }
    xd_gtk_app_preinit("org.xeondesktop.Privacy");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props p;
    p.privacy = settings_for(PRIVACY_SCHEMA,
                             (const char *const[]){"remember-recent-files", "recent-files-max-age",
                                                   "remove-old-trash-files",
                                                   "remove-old-temp-files", "old-files-age",
                                                   nullptr});
    p.location = settings_for(LOCATION_SCHEMA, (const char *const[]){"enabled", nullptr});

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), "Privacy Properties");
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "preferences-security");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(&p),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), location_page(&p),
                             gtk_label_new_with_mnemonic("L_ocation"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), permissions_page(&p),
                             gtk_label_new_with_mnemonic("Program _Permissions"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));

    GError *error = nullptr;
    p.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (p.bus == nullptr) {
        g_printerr("xd-privacy: %s\n", error->message);
        g_clear_error(&error);
    } else {
        p.store_sub = g_dbus_connection_signal_subscribe(
            p.bus, STORE, STORE, "Changed", STORE_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            store_changed, &p, nullptr);
    }
    p.system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (p.system != nullptr) {
        p.geoclue_installed = geoclue_activatable(p.system);
        p.geoclue_watch = g_bus_watch_name_on_connection(p.system, GEOCLUE,
                                                         G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                         geoclue_appeared, geoclue_vanished, &p,
                                                         nullptr);
    }
    show_geoclue(&p);
    refresh(&p);

    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs),
                                  XD_CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(tabs)) - 1));
    gtk_main();

    if (p.store_sub != 0)
        g_dbus_connection_signal_unsubscribe(p.bus, p.store_sub);
    if (p.geoclue_watch != 0)
        g_bus_unwatch_name(p.geoclue_watch);
    g_clear_object(&p.geoclue);
    g_clear_object(&p.system);
    g_clear_object(&p.bus);
    if (p.entries != nullptr)
        g_ptr_array_unref(p.entries);
    g_clear_object(&p.privacy);
    g_clear_object(&p.location);
    return EXIT_SUCCESS;
}
