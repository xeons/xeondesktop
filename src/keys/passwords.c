/* SPDX-License-Identifier: MIT
 * The Passwords tab: the Secret Service's keyrings (gnome-keyring calls
 * them keyrings, the specification collections) and the passwords in
 * them, through libsecret.
 *
 * Unlocking may ask for the keyring's password: the service asks in its
 * own prompt (gnome-keyring's system prompt), so it runs asynchronously.
 * Everything else is a quick call to the service.
 */
#include <string.h>

#include <libsecret/secret.h>

#include "../gtkapp/gtkapp.h"
#include "keys.h"
#include "keyslib.h"

enum { KR_LABEL, KR_OBJECT, KR_NCOLS };
enum { IT_ICON, IT_NAME, IT_KIND, IT_MODIFIED, IT_OBJECT, IT_NCOLS };

typedef struct pw_page {
    keys_app *app;
    GtkWidget *page, *combo, *lock, *view, *list, *add, *remove, *props, *note;
    GtkListStore *keyrings, *items;
    SecretService *service;
    SecretCollection *current; /* a reference */
    gulong locked_id, items_id;
    GCancellable *cancel;
    bool filling;
} pw_page;

static GtkWindow *parent(pw_page *p) { return GTK_WINDOW(p->app->window); }

/* The selected password (a reference), or nullptr. */
static SecretItem *selected_item(pw_page *p) {
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    SecretItem *item = nullptr;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->view)), &m,
                                        &it))
        gtk_tree_model_get(m, &it, IT_OBJECT, &item, -1);
    return item;
}

static void sync_buttons(pw_page *p) {
    bool open = p->current != nullptr && !secret_collection_get_locked(p->current);
    SecretItem *item = selected_item(p);
    gtk_widget_set_sensitive(p->add, open);
    gtk_widget_set_sensitive(p->remove, open && item != nullptr);
    gtk_widget_set_sensitive(p->props, open && item != nullptr);
    if (item != nullptr)
        g_object_unref(item);
}

static void fill_items(pw_page *p) {
    /* The selection survives the refill, by the item's object path. */
    SecretItem *was = selected_item(p);
    char *keep = was != nullptr ? g_strdup(g_dbus_proxy_get_object_path(G_DBUS_PROXY(was)))
                                : nullptr;
    if (was != nullptr)
        g_object_unref(was);
    gtk_list_store_clear(p->items);
    if (p->current == nullptr || secret_collection_get_locked(p->current)) {
        g_free(keep);
        sync_buttons(p);
        return;
    }
    GList *items = secret_collection_get_items(p->current);
    for (GList *l = items; l != nullptr; l = l->next) {
        SecretItem *item = l->data;
        char *label = secret_item_get_label(item);
        char *schema = secret_item_get_schema_name(item);
        char *modified = xd_keys_date((gint64)secret_item_get_modified(item));
        GtkTreeIter it;
        gtk_list_store_insert_with_values(p->items, &it, -1, IT_ICON, "dialog-password",
                                          IT_NAME, xd_secret_display_name(label, schema),
                                          IT_KIND, xd_secret_kind(schema), IT_MODIFIED, modified,
                                          IT_OBJECT, item, -1);
        if (keep != nullptr &&
            strcmp(keep, g_dbus_proxy_get_object_path(G_DBUS_PROXY(item))) == 0)
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->view)),
                                           &it);
        g_free(modified);
        g_free(schema);
        g_free(label);
    }
    g_list_free_full(items, g_object_unref);
    g_free(keep);
    sync_buttons(p);
}

static void on_items_loaded(GObject *source, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    if (!secret_collection_load_items_finish(SECRET_COLLECTION(source), res, &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            keys_error(parent(data), "The passwords could not be read.", error);
        g_error_free(error);
        return;
    }
    pw_page *p = data;
    if (SECRET_COLLECTION(source) == p->current)
        fill_items(p);
}

static void load_items(pw_page *p) {
    secret_collection_load_items(p->current, p->cancel, on_items_loaded, p);
}

/* The lock button, the note and the list for the keyring's state. */
static void show_state(pw_page *p) {
    bool locked = p->current != nullptr && secret_collection_get_locked(p->current);
    gtk_button_set_label(GTK_BUTTON(p->lock), locked ? "_Unlock" : "_Lock");
    gtk_widget_set_sensitive(p->lock, p->current != nullptr);
    gtk_label_set_text(GTK_LABEL(p->note),
                       p->current == nullptr ? ""
                       : locked ? "This keyring is locked. Click Unlock to see its passwords."
                                : "These passwords are stored in this keyring:");
    gtk_widget_set_sensitive(p->view, !locked);
    fill_items(p);
    if (p->current != nullptr && !locked)
        load_items(p);
}

static void on_locked_changed([[maybe_unused]] GObject *o, [[maybe_unused]] GParamSpec *ps,
                              gpointer data) {
    show_state(data);
}

static void on_items_changed([[maybe_unused]] GObject *o, [[maybe_unused]] GParamSpec *ps,
                             gpointer data) {
    fill_items(data);
}

static void set_current(pw_page *p, SecretCollection *c) {
    if (p->current != nullptr) {
        g_signal_handler_disconnect(p->current, p->locked_id);
        g_signal_handler_disconnect(p->current, p->items_id);
        g_clear_object(&p->current);
    }
    if (c != nullptr) {
        p->current = g_object_ref(c);
        p->locked_id = g_signal_connect(c, "notify::locked", G_CALLBACK(on_locked_changed), p);
        p->items_id = g_signal_connect(c, "notify::items", G_CALLBACK(on_items_changed), p);
    }
    show_state(p);
}

static void on_keyring_changed(GtkComboBox *combo, gpointer data) {
    pw_page *p = data;
    GtkTreeIter it;
    SecretCollection *c = nullptr;
    if (p->filling)
        return;
    if (gtk_combo_box_get_active_iter(combo, &it))
        gtk_tree_model_get(GTK_TREE_MODEL(p->keyrings), &it, KR_OBJECT, &c, -1);
    set_current(p, c);
    if (c != nullptr)
        g_object_unref(c);
}

/* A keyring's name: its label, else the last part of its path. */
static char *keyring_name(SecretCollection *c) {
    char *label = secret_collection_get_label(c);
    if (label != nullptr && label[0] != '\0')
        return label;
    g_free(label);
    const char *path = g_dbus_proxy_get_object_path(G_DBUS_PROXY(c));
    const char *slash = strrchr(path, '/');
    return g_strdup(slash != nullptr ? slash + 1 : path);
}

static void fill_keyrings(pw_page *p) {
    char *keep = p->current != nullptr
                     ? g_strdup(g_dbus_proxy_get_object_path(G_DBUS_PROXY(p->current)))
                     : nullptr;
    p->filling = true;
    gtk_list_store_clear(p->keyrings);
    GList *colls = secret_service_get_collections(p->service);
    int active = -1, i = 0;
    for (GList *l = colls; l != nullptr; l = l->next, i++) {
        const char *path = g_dbus_proxy_get_object_path(G_DBUS_PROXY(l->data));
        char *name = keyring_name(l->data);
        gtk_list_store_insert_with_values(p->keyrings, nullptr, -1, KR_LABEL, name, KR_OBJECT,
                                          l->data, -1);
        g_free(name);
        /* The one shown before, else the login keyring, else the first. */
        if (keep != nullptr ? strcmp(path, keep) == 0
                            : active < 0 && g_str_has_suffix(path, "/login"))
            active = i;
    }
    g_list_free_full(colls, g_object_unref);
    g_free(keep);
    p->filling = false;
    gtk_combo_box_set_active(GTK_COMBO_BOX(p->combo), active >= 0 ? active : 0);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(p->combo)) < 0)
        set_current(p, nullptr);
    gtk_widget_set_sensitive(p->combo, i > 0);
    if (i == 0)
        gtk_label_set_text(GTK_LABEL(p->note), "The password service has no keyrings yet.");
}

static void on_collections_changed([[maybe_unused]] GObject *o, [[maybe_unused]] GParamSpec *ps,
                                   gpointer data) {
    fill_keyrings(data);
}

static void on_service(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source;
    GError *error = nullptr;
    SecretService *service = secret_service_get_finish(res, &error);
    if (service == nullptr) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(error);
            return;
        }
        pw_page *p = data;
        char *text = g_strdup_printf(
            "No password service is running, so there are no stored passwords to show. "
            "Programs keep passwords in a Secret Service such as GNOME Keyring or KeePassXC.\n\n%s",
            error->message);
        gtk_label_set_text(GTK_LABEL(p->note), text);
        g_free(text);
        g_error_free(error);
        return;
    }
    pw_page *p = data;
    p->service = service;
    g_signal_connect(service, "notify::collections", G_CALLBACK(on_collections_changed), p);
    fill_keyrings(p);
}

/* ---- Lock / Unlock ------------------------------------------------------------------ */

typedef struct lock_call {
    pw_page *p;
    bool unlock;
} lock_call;

static void on_lock_done(GObject *source, GAsyncResult *res, gpointer data) {
    lock_call *c = data;
    pw_page *p = c->p;
    bool unlock = c->unlock;
    g_free(c);
    GError *error = nullptr;
    if (unlock)
        secret_service_unlock_finish(SECRET_SERVICE(source), res, nullptr, &error);
    else
        secret_service_lock_finish(SECRET_SERVICE(source), res, nullptr, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }
    keys_busy(p->page, false);
    if (error != nullptr) {
        keys_error(parent(p),
                   unlock ? "The keyring could not be unlocked." : "The keyring could not be locked.",
                   error);
        g_error_free(error);
    }
    show_state(p);
}

static void on_lock(pw_page *p) {
    if (p->current == nullptr || p->service == nullptr)
        return;
    lock_call *c = g_new0(lock_call, 1);
    c->p = p;
    c->unlock = secret_collection_get_locked(p->current);
    GList *objects = g_list_append(nullptr, p->current);
    gtk_widget_set_sensitive(p->lock, FALSE);
    keys_busy(p->page, true);
    if (c->unlock)
        secret_service_unlock(p->service, objects, p->cancel, on_lock_done, c);
    else
        secret_service_lock(p->service, objects, p->cancel, on_lock_done, c);
    g_list_free(objects);
}

/* ---- Add ------------------------------------------------------------------------------ */

static void on_name_changed(GtkEditable *e, gpointer ok) {
    const char *t = gtk_entry_get_text(GTK_ENTRY(e));
    gtk_widget_set_sensitive(ok, t[0] != '\0');
}

static void on_add(pw_page *p) {
    if (p->current == nullptr)
        return;
    GtkWidget *box = nullptr;
    GtkWidget *d = keys_dialog(parent(p), "Add Password", nullptr, &box);
    char *keyring = keyring_name(p->current);
    char *text = g_strdup_printf("The password is stored in the %s keyring.", keyring);
    gtk_box_pack_start(GTK_BOX(box), keys_label(text), FALSE, FALSE, 0);
    g_free(text);
    g_free(keyring);
    GtkWidget *grid = keys_grid();
    GtkWidget *name = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(name), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(name), 28);
    GtkWidget *pass = keys_password_entry();
    GtkWidget *again = keys_password_entry();
    keys_grid_row(grid, 0, "_Name:", name);
    keys_grid_row(grid, 1, "_Password:", pass);
    keys_grid_row(grid, 2, "_Confirm password:", again);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    GtkWidget *ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_widget_set_sensitive(ok, FALSE);
    g_signal_connect(name, "changed", G_CALLBACK(on_name_changed), ok);
    gtk_widget_show_all(d);
    while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        if (!keys_check_match(GTK_WINDOW(d), "Add Password", pass, again))
            continue;
        SecretValue *value =
            secret_value_new(gtk_entry_get_text(GTK_ENTRY(pass)), -1, "text/plain");
        GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
        GError *error = nullptr;
        SecretItem *item =
            secret_item_create_sync(p->current, nullptr, attrs, gtk_entry_get_text(GTK_ENTRY(name)),
                                    value, SECRET_ITEM_CREATE_NONE, p->cancel, &error);
        g_hash_table_unref(attrs);
        secret_value_unref(value);
        if (item == nullptr) {
            keys_error(GTK_WINDOW(d), "The password could not be stored.", error);
            g_error_free(error);
            continue;
        }
        g_object_unref(item);
        break;
    }
    gtk_widget_destroy(d);
    load_items(p);
}

/* ---- Remove ----------------------------------------------------------------------------- */

static void on_remove(pw_page *p) {
    SecretItem *item = selected_item(p);
    if (item == nullptr)
        return;
    char *label = secret_item_get_label(item);
    char *schema = secret_item_get_schema_name(item);
    char *q = g_strdup_printf("Are you sure you want to delete the password \"%s\"? Programs that "
                              "use it will ask for it again.",
                              xd_secret_display_name(label, schema));
    if (xd_message_box(parent(p), "Confirm Password Delete", XD_MB_WARNING, XD_MB_YES_NO, TRUE, q) ==
        GTK_RESPONSE_YES) {
        GError *error = nullptr;
        if (!secret_item_delete_sync(item, p->cancel, &error)) {
            keys_error(parent(p), "The password could not be deleted.", error);
            g_error_free(error);
        }
        load_items(p);
    }
    g_free(q);
    g_free(schema);
    g_free(label);
    g_object_unref(item);
}

/* ---- Properties --------------------------------------------------------------------------- */

static void on_show_toggled(GtkToggleButton *b, gpointer entry) {
    gtk_entry_set_visibility(GTK_ENTRY(entry), gtk_toggle_button_get_active(b));
}

static void on_copy_clicked([[maybe_unused]] GtkButton *b, gpointer entry) {
    keys_copy(gtk_entry_get_text(GTK_ENTRY(entry)));
}

static gint by_key(gconstpointer a, gconstpointer b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static GtkWidget *attributes_view(SecretItem *item) {
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GHashTable *attrs = secret_item_get_attributes(item);
    GPtrArray *keys = g_ptr_array_new();
    GHashTableIter hi;
    gpointer k = nullptr;
    g_hash_table_iter_init(&hi, attrs);
    /* The schema is the Kind already. */
    while (g_hash_table_iter_next(&hi, &k, nullptr))
        if (strcmp(k, "xdg:schema") != 0)
            g_ptr_array_add(keys, k);
    g_ptr_array_sort(keys, by_key);
    for (guint i = 0; i < keys->len; i++)
        gtk_list_store_insert_with_values(store, nullptr, -1, 0, keys->pdata[i], 1,
                                          g_hash_table_lookup(attrs, keys->pdata[i]), -1);
    g_ptr_array_unref(keys);
    g_hash_table_unref(attrs);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    keys_text_column(view, "Attribute", 0, 120);
    keys_text_column(view, "Value", 1, 180);
    return keys_scrolled(view, 320, 90);
}

static void on_properties(pw_page *p) {
    SecretItem *item = selected_item(p);
    if (item == nullptr)
        return;
    GError *error = nullptr;
    keys_busy(p->page, true);
    bool loaded = secret_item_load_secret_sync(item, p->cancel, &error);
    keys_busy(p->page, false);
    if (!loaded) {
        keys_error(parent(p), "The password could not be read.", error);
        g_error_free(error);
        g_object_unref(item);
        return;
    }
    SecretValue *value = secret_item_get_secret(item);
    const char *text = value != nullptr ? secret_value_get_text(value) : nullptr;
    char *label = secret_item_get_label(item);
    char *schema = secret_item_get_schema_name(item);
    char *title = g_strdup_printf("%s Properties", xd_secret_display_name(label, schema));

    GtkWidget *box = nullptr;
    GtkWidget *d = keys_dialog(parent(p), title, nullptr, &box);
    GtkWidget *grid = keys_grid();
    GtkWidget *name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name), label != nullptr ? label : "");
    gtk_entry_set_activates_default(GTK_ENTRY(name), TRUE);
    keys_grid_row(grid, 0, "_Name:", name);
    keys_grid_row(grid, 1, "Kind:", nullptr);
    gtk_grid_attach(GTK_GRID(grid), keys_label(xd_secret_kind(schema)), 1, 1, 1, 1);

    GtkWidget *pass = keys_password_entry();
    GtkWidget *copy = gtk_button_new_with_mnemonic("C_opy");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), pass, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), copy, FALSE, FALSE, 0);
    keys_grid_row(grid, 2, "_Password:", row);
    gtk_label_set_mnemonic_widget(
        GTK_LABEL(gtk_grid_get_child_at(GTK_GRID(grid), 0, 2)), pass);
    GtkWidget *show = gtk_check_button_new_with_mnemonic("_Show password");
    gtk_grid_attach(GTK_GRID(grid), show, 1, 3, 1, 1);
    if (text != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(pass), text);
        g_signal_connect(show, "toggled", G_CALLBACK(on_show_toggled), pass);
        g_signal_connect(copy, "clicked", G_CALLBACK(on_copy_clicked), pass);
    } else {
        /* Not text (a key, say): shown as what it is, and left alone. */
        gsize len = 0;
        if (value != nullptr)
            secret_value_get(value, &len);
        char *what = g_strdup_printf("(binary data, %zu bytes)", len);
        gtk_entry_set_visibility(GTK_ENTRY(pass), TRUE);
        gtk_entry_set_text(GTK_ENTRY(pass), what);
        g_free(what);
        gtk_widget_set_sensitive(pass, FALSE);
        gtk_widget_set_sensitive(copy, FALSE);
        gtk_widget_set_sensitive(show, FALSE);
    }
    char *created = xd_keys_date((gint64)secret_item_get_created(item));
    char *modified = xd_keys_date((gint64)secret_item_get_modified(item));
    keys_grid_row(grid, 4, "Created:", nullptr);
    gtk_grid_attach(GTK_GRID(grid), keys_label(created), 1, 4, 1, 1);
    keys_grid_row(grid, 5, "Modified:", nullptr);
    gtk_grid_attach(GTK_GRID(grid), keys_label(modified), 1, 5, 1, 1);
    g_free(created);
    g_free(modified);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new("Details");
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 8);
    gtk_box_pack_start(GTK_BOX(inner),
                       keys_label("What programs look the password up by:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), attributes_view(item), TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);
    gtk_widget_show_all(d);

    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        const char *new_label = gtk_entry_get_text(GTK_ENTRY(name));
        if (g_strcmp0(new_label, label) != 0 &&
            !secret_item_set_label_sync(item, new_label, p->cancel, &error)) {
            keys_error(GTK_WINDOW(d), "The name could not be changed.", error);
            g_clear_error(&error);
        }
        const char *new_text = gtk_entry_get_text(GTK_ENTRY(pass));
        if (text != nullptr && strcmp(new_text, text) != 0) {
            SecretValue *v = secret_value_new(new_text, -1, "text/plain");
            if (!secret_item_set_secret_sync(item, v, p->cancel, &error)) {
                keys_error(GTK_WINDOW(d), "The password could not be changed.", error);
                g_clear_error(&error);
            }
            secret_value_unref(v);
        }
        fill_items(p);
    }
    gtk_widget_destroy(d);
    if (value != nullptr)
        secret_value_unref(value);
    g_free(title);
    g_free(schema);
    g_free(label);
    g_object_unref(item);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    on_properties(data);
}

static void on_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    sync_buttons(data);
}

static gboolean on_view_key([[maybe_unused]] GtkWidget *w, GdkEventKey *ev, gpointer data) {
    if (ev->keyval == GDK_KEY_Delete) {
        on_remove(data);
        return TRUE;
    }
    return FALSE;
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    pw_page *p = data;
    g_cancellable_cancel(p->cancel);
    set_current(p, nullptr);
    if (p->service != nullptr) {
        g_signal_handlers_disconnect_by_data(p->service, p);
        g_object_unref(p->service);
    }
    g_object_unref(p->cancel);
    g_object_unref(p->keyrings);
    g_object_unref(p->items);
    g_free(p);
}

GtkWidget *passwords_page_new(keys_app *app) {
    pw_page *p = g_new0(pw_page, 1);
    p->app = app;
    p->cancel = g_cancellable_new();
    p->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(p->page), 10);

    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-password", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(intro), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(intro),
                       keys_label("Programs store the passwords you tell them to remember in "
                                  "keyrings. A locked keyring must be unlocked with its own "
                                  "password before its passwords can be used or seen."),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p->page), intro, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *kl = gtk_label_new_with_mnemonic("_Keyring:");
    p->keyrings = gtk_list_store_new(KR_NCOLS, G_TYPE_STRING, G_TYPE_OBJECT);
    p->combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(p->keyrings));
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(p->combo), r, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(p->combo), r, "text", KR_LABEL);
    gtk_label_set_mnemonic_widget(GTK_LABEL(kl), p->combo);
    gtk_widget_set_sensitive(p->combo, FALSE);
    g_signal_connect(p->combo, "changed", G_CALLBACK(on_keyring_changed), p);
    p->lock = keys_button("_Unlock", G_CALLBACK(on_lock), p);
    gtk_widget_set_sensitive(p->lock, FALSE);
    gtk_box_pack_start(GTK_BOX(row), kl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->lock, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->page), row, FALSE, FALSE, 0);

    p->note = keys_label("Connecting to the password service...");
    gtk_box_pack_start(GTK_BOX(p->page), p->note, FALSE, FALSE, 0);

    p->items = gtk_list_store_new(IT_NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_OBJECT);
    p->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->items));
    GtkTreeViewColumn *c = keys_text_column(p->view, "Name", IT_NAME, 190);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(c, pix, FALSE);
    gtk_tree_view_column_add_attribute(c, pix, "icon-name", IT_ICON);
    /* The icon before the text. */
    gtk_cell_layout_reorder(GTK_CELL_LAYOUT(c), pix, 0);
    keys_text_column(p->view, "Kind", IT_KIND, 140);
    keys_text_column(p->view, "Modified", IT_MODIFIED, 125);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(p->items), IT_NAME,
                                         GTK_SORT_ASCENDING);
    g_signal_connect(p->view, "row-activated", G_CALLBACK(on_row_activated), p);
    g_signal_connect(p->view, "key-press-event", G_CALLBACK(on_view_key), p);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->view)), "changed",
                     G_CALLBACK(on_selection), p);
    p->list = keys_scrolled(p->view, 480, 190);
    gtk_box_pack_start(GTK_BOX(p->page), p->list, TRUE, TRUE, 0);

    p->add = keys_button("_Add...", G_CALLBACK(on_add), p);
    p->remove = keys_button("_Remove", G_CALLBACK(on_remove), p);
    p->props = keys_button("Pr_operties", G_CALLBACK(on_properties), p);
    gtk_box_pack_start(GTK_BOX(p->page), keys_button_row(p->add, p->remove, p->props, nullptr),
                       FALSE, FALSE, 0);
    sync_buttons(p);
    g_signal_connect(p->page, "destroy", G_CALLBACK(on_destroy), p);

    SecretServiceFlags flags =
        (SecretServiceFlags)(SECRET_SERVICE_OPEN_SESSION | SECRET_SERVICE_LOAD_COLLECTIONS);
    secret_service_get(flags, p->cancel, on_service, p);
    return p->page;
}
