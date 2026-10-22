/* SPDX-License-Identifier: MIT
 * The PGP Keys tab: GnuPG's keyring (gpgkeys.h). gpgme's calls are quick
 * for everything here (Ed25519 keys are made in a moment), so they run in
 * place under the hourglass; passphrases are asked in our own dialog
 * from inside them.
 */
#include <string.h>

#include "../gtkapp/gtkapp.h"
#include "gpgkeys.h"
#include "keys.h"
#include "keyslib.h"

enum { GK_ICON, GK_NAME, GK_EMAIL, GK_KEYID, GK_EXPIRES, GK_TRUST, GK_INDEX, GK_NCOLS };

typedef struct gpg_page {
    keys_app *app;
    GtkWidget *page, *view, *note, *new_key, *import, *export, *del, *props;
    GtkListStore *store;
    GPtrArray *keys; /* xd_gpg_key */
    xd_gpg *gpg;
    bool tried;      /* gpg was looked for */
} gpg_page;

static GtkWindow *parent(gpg_page *page) { return GTK_WINDOW(page->app->window); }

static xd_gpg_key *selected(gpg_page *page) {
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (page->keys == nullptr ||
        !gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)),
                                         &model, &it))
        return nullptr;
    int i = -1;
    gtk_tree_model_get(model, &it, GK_INDEX, &i, -1);
    return i >= 0 && (guint)i < page->keys->len ? page->keys->pdata[i] : nullptr;
}

static void sync_buttons(gpg_page *page) {
    bool have = page->gpg != nullptr;
    bool sel = selected(page) != nullptr;
    gtk_widget_set_sensitive(page->new_key, have);
    gtk_widget_set_sensitive(page->import, have);
    gtk_widget_set_sensitive(page->export, have && sel);
    gtk_widget_set_sensitive(page->del, have && sel);
    gtk_widget_set_sensitive(page->props, have && sel);
}

/* When the key expires, as the list shows it. g_free it. */
static char *expires_text(const xd_gpg_key *key) {
    if (key->expires == 0)
        return g_strdup("Never");
    char *day = xd_keys_day(key->expires);
    if (!key->expired)
        return day;
    char *text = g_strdup_printf("%s (expired)", day);
    g_free(day);
    return text;
}

static const char *trust_text(const xd_gpg_key *key) {
    if (key->revoked)
        return "Revoked";
    if (key->disabled)
        return "Disabled";
    return xd_gpg_validity_text(key->validity);
}

/* Reads the keyring again, keeping the selection; selects `select` (a
 * fingerprint) instead if given. */
static void refresh(gpg_page *page, const char *select) {
    xd_gpg_key *was = selected(page);
    char *keep = g_strdup(select != nullptr ? select : was != nullptr ? was->fingerprint : nullptr);
    gtk_list_store_clear(page->store);
    g_clear_pointer(&page->keys, g_ptr_array_unref);
    GError *error = nullptr;
    if (page->gpg != nullptr)
        page->keys = xd_gpg_list(page->gpg, &error);
    if (page->gpg != nullptr && page->keys == nullptr) {
        gtk_label_set_text(GTK_LABEL(page->note),
                           error != nullptr ? error->message : "The keyring could not be read.");
        g_clear_error(&error);
    }
    GtkTreeIter select_it;
    bool found = false;
    for (guint i = 0; page->keys != nullptr && i < page->keys->len; i++) {
        xd_gpg_key *key = page->keys->pdata[i];
        char *exp = expires_text(key);
        GtkTreeIter it;
        gtk_list_store_insert_with_values(
            page->store, &it, -1, GK_ICON, key->secret ? "application-pgp-keys" : "text-x-generic",
            GK_NAME, key->name[0] != '\0' ? key->name : key->email, GK_EMAIL, key->email, GK_KEYID,
            key->key_id, GK_EXPIRES, exp, GK_TRUST, trust_text(key), GK_INDEX, (int)i, -1);
        g_free(exp);
        if (keep != nullptr && g_strcmp0(keep, key->fingerprint) == 0) {
            select_it = it;
            found = true;
        }
    }
    if (found)
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)),
                                       &select_it);
    g_free(keep);
    sync_buttons(page);
}

/* gpg-agent's question, through loopback: our own passphrase box. */
static char *ask_passphrase(void *data, const char *hint, bool again) {
    gpg_page *page = data;
    /* The hint is "KEYID Name <email>": the key's name is enough. gpg
     * sends none for some questions (a new expiry date): the key at hand
     * is the selected one. */
    const char *who = strchr(hint, ' ');
    who = who != nullptr ? who + 1 : hint;
    xd_gpg_key *key = selected(page);
    if (who[0] == '\0' && key != nullptr)
        who = key->name[0] != '\0' ? key->name : key->email;
    char *text = g_strdup_printf(again ? "The passphrase was wrong. Type the passphrase for the "
                                         "secret key of %s:"
                                       : "Type the passphrase for the secret key of %s:",
                                 who);
    keys_busy(page->page, false);
    char *pass = keys_ask_passphrase(parent(page), "Passphrase", text);
    keys_busy(page->page, true);
    g_free(text);
    return pass;
}

static void failed(gpg_page *page, const char *what, GError *error) {
    if (!xd_gpg_cancelled(error))
        keys_error(parent(page), what, error);
    g_error_free(error);
}

/* ---- expiry choices ------------------------------------------------------------------- */

static const char *const lifetimes[] = {"Never", "1 year", "2 years", "3 years", "5 years"};
static const int lifetime_years[] = {0, 1, 2, 3, 5};

/* Seconds from now to `years` from now (0 for 0). */
static guint64 years_from_now(int years) {
    if (years <= 0)
        return 0;
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *then = g_date_time_add_years(now, years);
    guint64 seconds = (guint64)(g_date_time_difference(then, now) / G_TIME_SPAN_SECOND);
    g_date_time_unref(then);
    g_date_time_unref(now);
    return seconds;
}

static GtkWidget *lifetime_combo(int active) {
    GtkWidget *combo = gtk_combo_box_text_new();
    for (size_t i = 0; i < G_N_ELEMENTS(lifetimes); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), lifetimes[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    return combo;
}

static guint64 lifetime_seconds(GtkWidget *combo) {
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    return i > 0 && (size_t)i < G_N_ELEMENTS(lifetime_years) ? years_from_now(lifetime_years[i])
                                                              : 0;
}

/* ---- New ---------------------------------------------------------------------------------- */

static void on_new(gpg_page *page) {
    GtkWidget *box = nullptr;
    GtkWidget *key_dialog = keys_dialog(parent(page), "New PGP Key", "C_reate", &box);
    gtk_box_pack_start(GTK_BOX(box),
                       keys_label("A PGP key pair signs and decrypts e-mail and files. Others "
                                  "use its public key to check your signatures and to encrypt "
                                  "to you."),
                       FALSE, FALSE, 0);
    GtkWidget *grid = keys_grid();
    GtkWidget *name = gtk_entry_new();
    GtkWidget *email = gtk_entry_new();
    GtkWidget *comment = gtk_entry_new();
    for (GtkWidget *const *entry_widget = (GtkWidget *const[]){name, email, comment, nullptr};
         *entry_widget != nullptr; entry_widget++)
        gtk_entry_set_activates_default(GTK_ENTRY(*entry_widget), TRUE);
    const char *real = g_get_real_name();
    if (real != nullptr && strcmp(real, "Unknown") != 0)
        gtk_entry_set_text(GTK_ENTRY(name), real);
    GtkWidget *expires = lifetime_combo(0);
    GtkWidget *pass = keys_password_entry();
    GtkWidget *again = keys_password_entry();
    keys_grid_row(grid, 0, "Full _name:", name);
    keys_grid_row(grid, 1, "_E-mail address:", email);
    keys_grid_row(grid, 2, "_Comment:", comment);
    keys_grid_row(grid, 3, "E_xpires:", expires);
    keys_grid_row(grid, 4, "Pass_phrase:", pass);
    keys_grid_row(grid, 5, "Con_firm passphrase:", again);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(key_dialog);
    gtk_widget_grab_focus(gtk_entry_get_text(GTK_ENTRY(name))[0] != '\0' ? email : name);

    while (gtk_dialog_run(GTK_DIALOG(key_dialog)) == GTK_RESPONSE_OK) {
        const char *name_text = gtk_entry_get_text(GTK_ENTRY(name));
        const char *email_text = gtk_entry_get_text(GTK_ENTRY(email));
        if (name_text[0] == '\0' && email_text[0] == '\0') {
            xd_message_box(GTK_WINDOW(key_dialog), "New PGP Key", XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type your name or e-mail address: they tell others whose key it is.");
            continue;
        }
        if (email_text[0] != '\0' &&
            (strchr(email_text, '@') == nullptr || strchr(email_text, ' ') != nullptr)) {
            xd_message_box(GTK_WINDOW(key_dialog), "New PGP Key", XD_MB_ERROR, XD_MB_OK, FALSE,
                           "The e-mail address isn't valid. Type it as name@example.com.");
            continue;
        }
        if (!keys_check_match(GTK_WINDOW(key_dialog), "New PGP Key", pass, again))
            continue;
        char *uid = xd_gpg_user_id(name_text, email_text, gtk_entry_get_text(GTK_ENTRY(comment)));
        char *fpr = nullptr;
        GError *error = nullptr;
        keys_busy(key_dialog, true);
        bool ok = xd_gpg_create(page->gpg, uid, gtk_entry_get_text(GTK_ENTRY(pass)),
                                lifetime_seconds(expires), &fpr, &error);
        keys_busy(key_dialog, false);
        g_free(uid);
        if (!ok) {
            failed(page, "The key could not be made.", error);
            continue;
        }
        refresh(page, fpr);
        g_free(fpr);
        break;
    }
    gtk_widget_destroy(key_dialog);
}

/* ---- Import, Export ----------------------------------------------------------------------- */

static GtkFileFilter *key_filter(void) {
    GtkFileFilter *file_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(file_filter, "Key files (*.asc, *.gpg, *.pgp, *.key)");
    for (const char *const *pattern =
             (const char *const[]){"*.asc", "*.gpg", "*.pgp", "*.key", nullptr};
         *pattern != nullptr; pattern++)
        gtk_file_filter_add_pattern(file_filter, *pattern);
    gtk_file_filter_add_mime_type(file_filter, "application/pgp-keys");
    return file_filter;
}

static void on_import(gpg_page *page) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Import Keys", parent(page), GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), key_filter());
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);
    char *path = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);
    if (path == nullptr)
        return;
    int imported = 0, unchanged = 0;
    GError *error = nullptr;
    keys_busy(page->page, true);
    bool ok = xd_gpg_import_file(page->gpg, path, &imported, &unchanged, &error);
    keys_busy(page->page, false);
    g_free(path);
    if (!ok) {
        failed(page, "The keys could not be imported.", error);
        return;
    }
    refresh(page, nullptr);
    char *text = imported == 0 ? g_strdup(unchanged == 1 ? "The key was already in your keyring."
                                                        : "The keys were already in your keyring.")
                 : imported == 1 ? g_strdup("1 key was imported.")
                                 : g_strdup_printf("%d keys were imported.", imported);
    xd_message_box(parent(page), KEYS_TITLE, XD_MB_INFO, XD_MB_OK, FALSE, text);
    g_free(text);
}

static void on_export(gpg_page *page) {
    xd_gpg_key *key = selected(page);
    if (key == nullptr)
        return;
    GError *error = nullptr;
    char *armour = xd_gpg_export(page->gpg, key->fingerprint, &error);
    if (armour == nullptr) {
        failed(page, "The key could not be exported.", error);
        return;
    }
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Export Public Key", parent(page), GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel",
        GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), g_get_home_dir());
    char *base = g_strdup_printf("%s.asc", key->name[0] != '\0' ? key->name : key->key_id);
    g_strdelimit(base, "/", '_');
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), base);
    g_free(base);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), key_filter());
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path != nullptr && !g_file_set_contents(path, armour, -1, &error)) {
            keys_error(GTK_WINDOW(dlg), "The key could not be saved.", error);
            g_clear_error(&error);
        }
        g_free(path);
    }
    gtk_widget_destroy(dlg);
    g_free(armour);
}

/* ---- Delete ------------------------------------------------------------------------------- */

static void on_delete(gpg_page *page) {
    xd_gpg_key *key = selected(page);
    if (key == nullptr)
        return;
    const char *who = key->name[0] != '\0' ? key->name : key->email;
    char *message =
        key->secret
            ? g_strdup_printf("Are you sure you want to delete the key pair \"%s\"?\n\nIts "
                              "secret key is deleted too: what was encrypted to it can't be "
                              "decrypted any more, and it can't sign again.",
                              who)
            : g_strdup_printf("Are you sure you want to delete the public key \"%s\"?", who);
    bool yes = xd_message_box(parent(page), "Confirm Key Delete", XD_MB_WARNING, XD_MB_YES_NO, TRUE,
                              message) == GTK_RESPONSE_YES;
    g_free(message);
    if (!yes)
        return;
    GError *error = nullptr;
    keys_busy(page->page, true);
    bool ok = xd_gpg_delete(page->gpg, key->fingerprint, &error);
    keys_busy(page->page, false);
    if (!ok)
        failed(page, "The key could not be deleted.", error);
    refresh(page, nullptr);
}

/* ---- Properties ------------------------------------------------------------------------- */

/* The fingerprint in groups of four, as gpg prints it. g_free it. */
static char *grouped(const char *fpr) {
    GString *string = g_string_new(nullptr);
    for (size_t i = 0; fpr != nullptr && fpr[i] != '\0'; i++) {
        if (i > 0 && i % 4 == 0)
            g_string_append_c(string, ' ');
        if (i > 0 && i % 20 == 0)
            g_string_append_c(string, ' ');
        g_string_append_c(string, fpr[i]);
    }
    return g_string_free(string, FALSE);
}

static void value_row(GtkWidget *grid, int row, const char *label, const char *value) {
    keys_grid_row(grid, row, label, nullptr);
    GtkWidget *value_label = keys_label(value);
    gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
    gtk_grid_attach(GTK_GRID(grid), value_label, 1, row, 1, 1);
}

/* Change Expiry Date; true if it was changed. */
static bool change_expiry(gpg_page *page, GtkWindow *over, const xd_gpg_key *key) {
    GtkWidget *box = nullptr;
    GtkWidget *dialog_widget = keys_dialog(over, "Change Expiry Date", nullptr, &box);
    gtk_box_pack_start(GTK_BOX(box),
                       keys_label("After its expiry date, a key can't be used to sign or "
                                  "encrypt. You can move the date while you have the secret "
                                  "key."),
                       FALSE, FALSE, 0);
    GtkWidget *never = gtk_check_button_new_with_mnemonic("_Never expires");
    gtk_widget_set_halign(never, GTK_ALIGN_START);
    GtkWidget *cal = gtk_calendar_new();
    GDateTime *when = key->expires != 0 ? g_date_time_new_from_unix_local(key->expires) : nullptr;
    if (when == nullptr) {
        GDateTime *now = g_date_time_new_now_local();
        when = g_date_time_add_years(now, 1);
        g_date_time_unref(now);
    }
    gtk_calendar_select_month(GTK_CALENDAR(cal), (guint)g_date_time_get_month(when) - 1,
                              (guint)g_date_time_get_year(when));
    gtk_calendar_select_day(GTK_CALENDAR(cal), (guint)g_date_time_get_day_of_month(when));
    g_date_time_unref(when);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(never), key->expires == 0);
    g_object_bind_property(never, "active", cal, "sensitive",
                           G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);
    gtk_box_pack_start(GTK_BOX(box), never, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), cal, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog_widget);
    bool changed = false;
    while (gtk_dialog_run(GTK_DIALOG(dialog_widget)) == GTK_RESPONSE_OK) {
        guint64 seconds = 0;
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(never))) {
            guint y = 0, month = 0, day = 0;
            gtk_calendar_get_date(GTK_CALENDAR(cal), &y, &month, &day);
            /* The end of that day, here. */
            GDateTime *date_time =
                g_date_time_new_local((int)y, (int)month + 1, (int)day, 23, 59, 59);
            gint64 left = date_time != nullptr
                              ? g_date_time_to_unix(date_time) - g_get_real_time() / G_USEC_PER_SEC
                              : 0;
            if (date_time != nullptr)
                g_date_time_unref(date_time);
            if (left <= 0) {
                xd_message_box(GTK_WINDOW(dialog_widget), "Change Expiry Date", XD_MB_ERROR,
                               XD_MB_OK, FALSE, "Choose a date after today.");
                continue;
            }
            seconds = (guint64)left;
        }
        GError *error = nullptr;
        keys_busy(dialog_widget, true);
        bool ok = xd_gpg_set_expiry(page->gpg, key->fingerprint, seconds, &error);
        keys_busy(dialog_widget, false);
        if (!ok) {
            failed(page, "The expiry date could not be changed.", error);
            continue;
        }
        changed = true;
        break;
    }
    gtk_widget_destroy(dialog_widget);
    return changed;
}

typedef struct props_dlg {
    gpg_page *p;
    GtkWidget *dialog;
    char *fingerprint;
    bool changed;
} props_dlg;

static void on_change_expiry(props_dlg *pd) {
    xd_gpg_key *key = selected(pd->p);
    if (key == nullptr || !change_expiry(pd->p, GTK_WINDOW(pd->dialog), key))
        return;
    pd->changed = true;
    /* The dialog shows the old date: close it on the new one. */
    gtk_dialog_response(GTK_DIALOG(pd->dialog), GTK_RESPONSE_APPLY);
}

static void on_properties(gpg_page *page) {
    for (;;) {
        xd_gpg_key *key = selected(page);
        if (key == nullptr)
            return;
        const char *who = key->name[0] != '\0' ? key->name : key->email;
        char *title = g_strdup_printf("%s Properties", who);
        GtkWidget *dialog_widget = gtk_dialog_new();
        gtk_window_set_title(GTK_WINDOW(dialog_widget), title);
        g_free(title);
        gtk_window_set_transient_for(GTK_WINDOW(dialog_widget), parent(page));
        gtk_window_set_modal(GTK_WINDOW(dialog_widget), TRUE);
        gtk_window_set_resizable(GTK_WINDOW(dialog_widget), FALSE);
        GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(dialog_widget), "OK", GTK_RESPONSE_OK);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog_widget), GTK_RESPONSE_OK);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 10);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog_widget))), box,
                           TRUE, TRUE, 0);
        GtkWidget *grid = keys_grid();
        int row = 0;
        value_row(grid, row++, "Name:", key->name);
        value_row(grid, row++, "E-mail:", key->email);
        if (key->comment[0] != '\0')
            value_row(grid, row++, "Comment:", key->comment);
        if (g_strv_length(key->user_ids) > 1) {
            char *ids = g_strjoinv("\n", key->user_ids);
            value_row(grid, row++, "User IDs:", ids);
            g_free(ids);
        }
        value_row(grid, row++,
                  "Type:", key->secret ? "Key pair (you have its secret key)" : "Public key");
        value_row(grid, row++, "Key ID:", key->key_id);
        char *fp = grouped(key->fingerprint);
        value_row(grid, row++, "Fingerprint:", fp);
        g_free(fp);
        value_row(grid, row++, "Algorithm:", key->algorithm != nullptr ? key->algorithm : "");
        char *created = xd_keys_day(key->created);
        value_row(grid, row++, "Created:", created);
        g_free(created);
        char *exp = expires_text(key);
        keys_grid_row(grid, row, "Expires:", nullptr);
        GtkWidget *exp_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(exp_row), keys_label(exp), TRUE, TRUE, 0);
        g_free(exp);
        props_dlg pd = {.p = page, .dialog = dialog_widget};
        GtkWidget *change = keys_button("C_hange...", G_CALLBACK(on_change_expiry), &pd);
        gtk_widget_set_sensitive(change, key->secret);
        if (!key->secret)
            gtk_widget_set_tooltip_text(change, "Only the secret key's owner can change it.");
        gtk_box_pack_start(GTK_BOX(exp_row), change, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), exp_row, 1, row++, 1, 1);
        value_row(grid, row++, "Trust:", trust_text(key));
        if (key->expired || key->revoked || key->disabled || key->invalid)
            value_row(grid, row++, "Status:",
                      key->revoked    ? "This key has been revoked by its owner."
                      : key->expired  ? "This key has expired."
                      : key->disabled ? "This key is disabled."
                                      : "This key is not valid.");
        gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
        gtk_widget_show_all(dialog_widget);
        gtk_widget_grab_focus(ok);
        gtk_dialog_run(GTK_DIALOG(dialog_widget));
        gtk_widget_destroy(dialog_widget);
        if (!pd.changed)
            return;
        /* The new date: read again, and show it. */
        refresh(page, nullptr);
    }
}

/* ---- the page ----------------------------------------------------------------------------- */

static void on_row_activated([[maybe_unused]] GtkTreeView *tree_view,
                             [[maybe_unused]] GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    on_properties(data);
}

static void on_selection([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    sync_buttons(data);
}

static gboolean on_view_key([[maybe_unused]] GtkWidget *widget, GdkEventKey *ev, gpointer data) {
    if (ev->keyval == GDK_KEY_Delete) {
        on_delete(data);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_F5) {
        refresh(data, nullptr);
        return TRUE;
    }
    return FALSE;
}

static void on_map([[maybe_unused]] GtkWidget *widget, gpointer data) {
    gpg_page *page = data;
    if (!page->tried) {
        page->tried = true;
        GError *error = nullptr;
        page->gpg = xd_gpg_new(nullptr, ask_passphrase, page, &error);
        if (page->gpg == nullptr) {
            char *text = g_strdup_printf("GnuPG isn't available, so PGP keys can't be shown.\n\n%s",
                                         error->message);
            gtk_label_set_text(GTK_LABEL(page->note), text);
            g_free(text);
            g_error_free(error);
        }
    }
    refresh(page, nullptr);
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    gpg_page *page = data;
    if (page->keys != nullptr)
        g_ptr_array_unref(page->keys);
    xd_gpg_free(page->gpg);
    g_object_unref(page->store);
    g_free(page);
}

GtkWidget *gpg_page_new(keys_app *app) {
    gpg_page *page = g_new0(gpg_page, 1);
    page->app = app;
    page->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page->page), 10);

    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("application-pgp-keys", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(intro), icon, FALSE, FALSE, 0);
    page->note = keys_label("PGP keys sign and encrypt e-mail and files. These keys are in your "
                            "GnuPG keyring:");
    gtk_box_pack_start(GTK_BOX(intro), page->note, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page->page), intro, FALSE, FALSE, 0);

    page->store = gtk_list_store_new(GK_NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    page->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(page->store));
    GtkTreeViewColumn *column = keys_text_column(page->view, "Name", GK_NAME, 110);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(column, pix, FALSE);
    gtk_tree_view_column_add_attribute(column, pix, "icon-name", GK_ICON);
    gtk_cell_layout_reorder(GTK_CELL_LAYOUT(column), pix, 0);
    keys_text_column(page->view, "E-mail", GK_EMAIL, 105);
    keys_text_column(page->view, "Key ID", GK_KEYID, 115);
    keys_text_column(page->view, "Expires", GK_EXPIRES, 75);
    keys_text_column(page->view, "Trust", GK_TRUST, 60);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)), "changed",
                     G_CALLBACK(on_selection), page);
    g_signal_connect(page->view, "row-activated", G_CALLBACK(on_row_activated), page);
    g_signal_connect(page->view, "key-press-event", G_CALLBACK(on_view_key), page);
    gtk_box_pack_start(GTK_BOX(page->page), keys_scrolled(page->view, 480, 220), TRUE, TRUE, 0);

    page->new_key = keys_button("_New...", G_CALLBACK(on_new), page);
    page->import = keys_button("_Import...", G_CALLBACK(on_import), page);
    page->export = keys_button("_Export...", G_CALLBACK(on_export), page);
    page->del = keys_button("_Delete", G_CALLBACK(on_delete), page);
    page->props = keys_button("Pr_operties", G_CALLBACK(on_properties), page);
    gtk_box_pack_start(
        GTK_BOX(page->page),
        keys_button_row(page->new_key, page->import, page->export, page->del, page->props, nullptr),
        FALSE, FALSE, 0);
    g_signal_connect(page->page, "map", G_CALLBACK(on_map), page);
    g_signal_connect(page->page, "destroy", G_CALLBACK(on_destroy), page);
    sync_buttons(page);
    return page->page;
}
