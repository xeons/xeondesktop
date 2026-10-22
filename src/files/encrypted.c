/* SPDX-License-Identifier: MIT
 * My Computer's encrypted drives. See encrypted.h.
 *
 * Remembered passphrases are kept with libsecret in the Secret Service
 * (GNOME Keyring, KWallet's ksecretd, KeePassXC...), under gvfs's schema
 * so they're shared with GNOME's Files and Disks. Without libsecret,
 * "Remember my passphrase" isn't offered.
 */
#include "encrypted.h"

#include <string.h>

#if XD_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#include "../gtkapp/gtkapp.h"
#include "luks.h"
#include "luksdlg.h"
#include "places.h"
#include "udisks.h"

#define UD_ERROR "org.freedesktop.UDisks2.Error."

/* The drive's name, as My Computer shows it. */
static char *crypt_name(const XdUdisksCrypt *udisks_crypt) {
    char *size = xd_format_drive_size(udisks_crypt->size);
    char *name = xd_luks_volume_name(udisks_crypt->inner_label, udisks_crypt->label, size);
    g_free(size);
    return name;
}

/* Whether polkit's box was cancelled: nothing more to say. */
static gboolean dismissed(const char *dbus_error) {
    return g_strcmp0(dbus_error, UD_ERROR "NotAuthorizedDismissed") == 0;
}

/* ---- the keyring ---------------------------------------------------------------- */

#if XD_HAVE_LIBSECRET
static const SecretSchema *luks_schema(void) {
    static const SecretSchema schema = {
        .name = XD_LUKS_SECRET_SCHEMA,
        .flags = SECRET_SCHEMA_DONT_MATCH_NAME,
        .attributes = {{XD_LUKS_SECRET_ATTRIBUTE, SECRET_SCHEMA_ATTRIBUTE_STRING},
                       {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    };
    return &schema;
}
#endif

static gboolean keyring_available(void) {
#if XD_HAVE_LIBSECRET
    return TRUE;
#else
    return FALSE;
#endif
}

static void keyring_lookup(const char *uuid, GCancellable *cancel, GAsyncReadyCallback done,
                           gpointer data) {
#if XD_HAVE_LIBSECRET
    secret_password_lookup(luks_schema(), cancel, done, data, XD_LUKS_SECRET_ATTRIBUTE, uuid,
                           nullptr);
#else
    (void)uuid;
    GTask *task = g_task_new(nullptr, cancel, done, data);
    g_task_return_pointer(task, nullptr, nullptr);
    g_object_unref(task);
#endif
}

/* The passphrase remembered, or nullptr (free with
 * xd_luks_passphrase_free). A keyring that isn't there, or is locked
 * and stays so, remembers nothing. */
static char *keyring_lookup_finish(GAsyncResult *res, GError **err) {
#if XD_HAVE_LIBSECRET
    char *secret = secret_password_lookup_finish(res, err);
    char *copy = g_strdup(secret);
    secret_password_free(secret);
    return copy;
#else
    return g_task_propagate_pointer(G_TASK(res), err);
#endif
}

/* Stores the passphrase (in the default collection, kept across logins
 * as gvfs's "Remember forever"); done may be nullptr. */
static void keyring_store(const char *uuid, const char *name, const char *passphrase,
                          GAsyncReadyCallback done, gpointer data) {
#if XD_HAVE_LIBSECRET
    char *label = xd_luks_secret_label(name);
    secret_password_store(luks_schema(), SECRET_COLLECTION_DEFAULT, label, passphrase, nullptr,
                          done, data, XD_LUKS_SECRET_ATTRIBUTE, uuid, nullptr);
    g_free(label);
#else
    (void)uuid;
    (void)name;
    (void)passphrase;
    GTask *task = g_task_new(nullptr, nullptr, done, data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "There's no keyring support.");
    g_object_unref(task);
#endif
}

static gboolean keyring_store_finish(GAsyncResult *res, GError **err) {
#if XD_HAVE_LIBSECRET
    return secret_password_store_finish(res, err);
#else
    return g_task_propagate_boolean(G_TASK(res), err);
#endif
}

static void keyring_clear(const char *uuid, GAsyncReadyCallback done, gpointer data) {
#if XD_HAVE_LIBSECRET
    secret_password_clear(luks_schema(), nullptr, done, data, XD_LUKS_SECRET_ATTRIBUTE, uuid,
                          nullptr);
#else
    (void)uuid;
    GTask *task = g_task_new(nullptr, nullptr, done, data);
    g_task_return_boolean(task, FALSE);
    g_object_unref(task);
#endif
}

static void keyring_clear_finish(GAsyncResult *res) {
#if XD_HAVE_LIBSECRET
    secret_password_clear_finish(res, nullptr);
#else
    g_task_propagate_boolean(G_TASK(res), nullptr);
#endif
}

static void cleared_quietly([[maybe_unused]] GObject *src, GAsyncResult *res,
                            [[maybe_unused]] gpointer data) {
    keyring_clear_finish(res);
}

/* ---- unlocking and opening -------------------------------------------------------- */

gboolean xd_encrypted_needs_unlock(GFile *item) {
    g_autofree char *uri = item != nullptr ? g_file_get_uri(item) : nullptr;
    g_autofree char *uuid = xd_luks_item_uuid(uri);
    return uuid != nullptr;
}

typedef struct open_job {
    GtkWidget *parent; /* weak */
    char *object, *uuid, *name;
    char *passphrase; /* the one being tried */
    gboolean from_keyring, remember, retry;
    void (*opened)(GFile *root, gpointer data);
    gpointer data;
} open_job;

static void job_free(open_job *unlock_request) {
    if (unlock_request->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(unlock_request->parent),
                                     (gpointer *)&unlock_request->parent);
    g_free(unlock_request->object);
    g_free(unlock_request->uuid);
    g_free(unlock_request->name);
    xd_luks_passphrase_free(unlock_request->passphrase);
    g_free(unlock_request);
}

static void job_error(open_job *unlock_request, const char *text) {
    if (unlock_request->parent != nullptr) {
        char *msg = g_strdup_printf("%s is not accessible.\n\n%s", unlock_request->name, text);
        xd_message_box(GTK_WINDOW(unlock_request->parent), "Files", XD_MB_ERROR, XD_MB_OK, FALSE,
                       msg);
        g_free(msg);
    }
    job_free(unlock_request);
}

static void ask(open_job *unlock_request);

static void mounted(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    open_job *unlock_request = data;
    char *dbus_error = nullptr;
    GError *err = nullptr;
    GVariant *variant = xd_udisks_call_finish(res, &dbus_error, &err);
    char *point = nullptr;
    if (variant != nullptr) {
        g_variant_get(variant, "(s)", &point);
        g_variant_unref(variant);
    } else if (g_strcmp0(dbus_error, UD_ERROR "AlreadyMounted") == 0) {
        g_autoptr(XdUdisksCrypt) crypt = nullptr;
        g_autofree char *uri = xd_luks_item_uri(unlock_request->uuid);
        GFile *item = g_file_new_for_uri(uri);
        crypt = xd_udisks_crypt_for(item);
        g_object_unref(item);
        point = crypt != nullptr ? g_strdup(crypt->mount_point) : nullptr;
    }
    if (point != nullptr) {
        if (unlock_request->parent != nullptr) {
            GFile *root = g_file_new_for_path(point);
            unlock_request->opened(root, unlock_request->data);
            g_object_unref(root);
        }
        g_free(point);
        job_free(unlock_request);
    } else if (dismissed(dbus_error)) {
        job_free(unlock_request);
    } else {
        job_error(unlock_request, err != nullptr ? err->message
                                                 : "It holds no file system that can be "
                                                   "opened.");
    }
    g_clear_error(&err);
    g_free(dbus_error);
}

static void mount_cleartext(open_job *unlock_request, const char *cleartext) {
    xd_udisks_call(cleartext, XD_UDISKS_FILESYSTEM, "Mount", g_variant_new("(a{sv})", nullptr),
                   nullptr, mounted, unlock_request);
}

static void remembered(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    GtkWidget **parent = data;
    GError *err = nullptr;
    if (!keyring_store_finish(res, &err)) {
        if (*parent != nullptr) {
            char *msg = g_strdup_printf("The passphrase could not be remembered: %s",
                                        err->message);
            xd_message_box(GTK_WINDOW(*parent), "Files", XD_MB_WARNING, XD_MB_OK, FALSE, msg);
            g_free(msg);
        }
        g_error_free(err);
    }
    if (*parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(*parent), (gpointer *)parent);
    g_free(parent);
}

/* Remembers the passphrase that just worked, telling the user (over
 * `parent`, if still open) if the keyring can't. */
static void remember(GtkWidget *parent, const char *uuid, const char *name,
                     const char *passphrase) {
    GtkWidget **weak = g_new0(GtkWidget *, 1);
    *weak = parent;
    if (parent != nullptr)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)weak);
    keyring_store(uuid, name, passphrase, remembered, weak);
}

static void unlocked(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    open_job *unlock_request = data;
    char *dbus_error = nullptr;
    GError *err = nullptr;
    GVariant *variant = xd_udisks_call_finish(res, &dbus_error, &err);
    if (variant != nullptr) {
        char *cleartext = nullptr;
        g_variant_get(variant, "(o)", &cleartext);
        g_variant_unref(variant);
        if (unlock_request->remember && !unlock_request->from_keyring)
            remember(unlock_request->parent, unlock_request->uuid, unlock_request->name,
                     unlock_request->passphrase);
        g_clear_pointer(&unlock_request->passphrase, xd_luks_passphrase_free);
        if (unlock_request->parent != nullptr)
            mount_cleartext(unlock_request, cleartext);
        else
            job_free(unlock_request);
        g_free(cleartext);
    } else if (xd_luks_wrong_passphrase(dbus_error, err->message)) {
        g_clear_pointer(&unlock_request->passphrase, xd_luks_passphrase_free);
        if (unlock_request->from_keyring) {
            /* A passphrase changed elsewhere: forgotten, and asked for
             * as if never remembered (as gvfs does). */
            keyring_clear(unlock_request->uuid, cleared_quietly, nullptr);
            unlock_request->from_keyring = FALSE;
            unlock_request->retry = FALSE;
        } else {
            unlock_request->retry = TRUE;
        }
        ask(unlock_request);
    } else if (g_strcmp0(dbus_error, UD_ERROR "AlreadyUnlocked") == 0) {
        /* Unlocked meanwhile, elsewhere. */
        g_autofree char *uri = xd_luks_item_uri(unlock_request->uuid);
        GFile *item = g_file_new_for_uri(uri);
        g_autoptr(XdUdisksCrypt) crypt = xd_udisks_crypt_for(item);
        g_object_unref(item);
        if (crypt != nullptr && crypt->cleartext != nullptr && unlock_request->parent != nullptr)
            mount_cleartext(unlock_request, crypt->cleartext);
        else
            job_free(unlock_request);
    } else if (dismissed(dbus_error)) {
        job_free(unlock_request);
    } else {
        job_error(unlock_request, err->message);
    }
    g_clear_error(&err);
    g_free(dbus_error);
}

static void try_unlock(open_job *unlock_request) {
    xd_udisks_call(unlock_request->object, XD_UDISKS_ENCRYPTED, "Unlock",
                   g_variant_new("(sa{sv})", unlock_request->passphrase, nullptr), nullptr,
                   unlocked, unlock_request);
}

static void ask(open_job *unlock_request) {
    if (unlock_request->parent == nullptr) {
        job_free(unlock_request);
        return;
    }
    gboolean *keep = keyring_available() ? &unlock_request->remember : nullptr;
    unlock_request->passphrase = xd_luks_ask_passphrase(
        GTK_WINDOW(unlock_request->parent), unlock_request->name, unlock_request->retry, keep);
    if (unlock_request->passphrase == nullptr) {
        job_free(unlock_request);
        return;
    }
    try_unlock(unlock_request);
}

static void looked_up(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    open_job *unlock_request = data;
    char *secret = keyring_lookup_finish(res, nullptr);
    if (secret != nullptr) {
        unlock_request->passphrase = secret;
        unlock_request->from_keyring = TRUE;
        try_unlock(unlock_request);
    } else {
        ask(unlock_request);
    }
}

void xd_encrypted_open(GtkWindow *parent, GFile *item,
                       void (*opened)(GFile *root, gpointer data), gpointer data) {
    g_autoptr(XdUdisksCrypt) crypt = xd_udisks_crypt_for(item);
    open_job *unlock_request = g_new0(open_job, 1);
    unlock_request->parent = GTK_WIDGET(parent);
    g_object_add_weak_pointer(G_OBJECT(unlock_request->parent),
                              (gpointer *)&unlock_request->parent);
    unlock_request->opened = opened;
    unlock_request->data = data;
    if (crypt == nullptr) {
        unlock_request->name = g_file_get_parse_name(item);
        job_error(unlock_request, "The drive is no longer there.");
        return;
    }
    unlock_request->object = g_strdup(crypt->object);
    unlock_request->uuid = g_strdup(crypt->uuid);
    unlock_request->name = crypt_name(crypt);
    if (crypt->mount_point != nullptr) {
        GFile *root = g_file_new_for_path(crypt->mount_point);
        opened(root, data);
        g_object_unref(root);
        job_free(unlock_request);
    } else if (crypt->cleartext != nullptr) {
        mount_cleartext(unlock_request, crypt->cleartext);
    } else {
        /* Remembered only when asked to, as XP's Connect to box. */
        unlock_request->remember = FALSE;
        keyring_lookup(unlock_request->uuid, nullptr, looked_up, unlock_request);
    }
}

/* ---- Lock ---------------------------------------------------------------------------- */

gboolean xd_encrypted_can_lock(GFile *item) {
    g_autoptr(XdUdisksCrypt) crypt = xd_udisks_crypt_for(item);
    return crypt != nullptr && crypt->cleartext != nullptr;
}

typedef struct lock_job {
    GtkWidget *parent; /* weak */
    char *object, *cleartext, *name;
    void (*done)(gpointer data);
    gpointer data;
} lock_job;

static void lock_job_free(lock_job *lock_request) {
    if (lock_request->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(lock_request->parent),
                                     (gpointer *)&lock_request->parent);
    g_free(lock_request->object);
    g_free(lock_request->cleartext);
    g_free(lock_request->name);
    g_free(lock_request);
}

static void lock_step(GObject *src, GAsyncResult *res, gpointer data);

static void lock_now(lock_job *lock_request) {
    g_clear_pointer(&lock_request->cleartext, g_free);
    xd_udisks_call(lock_request->object, XD_UDISKS_ENCRYPTED, "Lock",
                   g_variant_new("(a{sv})", nullptr), nullptr, lock_step, lock_request);
}

/* After Unmount (cleartext still set), then after Lock. */
static void lock_step(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    lock_job *lock_request = data;
    char *dbus_error = nullptr;
    GError *err = nullptr;
    GVariant *variant = xd_udisks_call_finish(res, &dbus_error, &err);
    g_clear_pointer(&variant, g_variant_unref);
    if (err == nullptr && lock_request->cleartext != nullptr) {
        lock_now(lock_request);
        return;
    }
    if (err != nullptr && !dismissed(dbus_error) && lock_request->parent != nullptr) {
        char *msg = g_strdup_printf("Cannot lock %s: %s", lock_request->name, err->message);
        xd_message_box(GTK_WINDOW(lock_request->parent), "Lock", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
    }
    if (err == nullptr && lock_request->done != nullptr && lock_request->parent != nullptr)
        lock_request->done(lock_request->data);
    g_clear_error(&err);
    g_free(dbus_error);
    lock_job_free(lock_request);
}

static void lock_with(GtkWindow *parent, GFile *item, void (*done)(gpointer), gpointer data) {
    g_autoptr(XdUdisksCrypt) crypt = xd_udisks_crypt_for(item);
    if (crypt == nullptr || crypt->cleartext == nullptr)
        return;
    lock_job *lock_request = g_new0(lock_job, 1);
    lock_request->parent = GTK_WIDGET(parent);
    g_object_add_weak_pointer(G_OBJECT(lock_request->parent), (gpointer *)&lock_request->parent);
    lock_request->object = g_strdup(crypt->object);
    lock_request->name = crypt_name(crypt);
    lock_request->done = done;
    lock_request->data = data;
    if (crypt->mount_point != nullptr) {
        lock_request->cleartext = g_strdup(crypt->cleartext);
        xd_udisks_call(crypt->cleartext, XD_UDISKS_FILESYSTEM, "Unmount",
                       g_variant_new("(a{sv})", nullptr), nullptr, lock_step, lock_request);
    } else {
        lock_now(lock_request);
    }
}

void xd_encrypted_lock(GtkWindow *parent, GFile *item) {
    lock_with(parent, item, nullptr, nullptr);
}

/* ---- the Encryption tab ------------------------------------------------------------- */

typedef struct crypt_page {
    GtkWidget *dialog, *status, *lock, *remembered, *forget;
    GFile *item;
    char *object, *uuid, *name;
    GCancellable *cancel; /* the keyring lookup, cancelled with the page */
} crypt_page;

static void crypt_page_free(gpointer user_data) {
    crypt_page *cp = user_data;
    g_cancellable_cancel(cp->cancel);
    g_object_unref(cp->cancel);
    g_object_unref(cp->item);
    g_free(cp->object);
    g_free(cp->uuid);
    g_free(cp->name);
    g_free(cp);
}

static void show_remembered(crypt_page *cp, gboolean is) {
    gtk_label_set_text(GTK_LABEL(cp->remembered),
                       !keyring_available() ? "Passphrases can't be remembered: XeonDesktop was "
                                              "built without libsecret."
                       : is ? "Your keyring remembers the passphrase: the drive is unlocked "
                              "with it when you open it."
                            : "Your keyring doesn't remember the passphrase: it's asked for "
                              "when you open the drive.");
    gtk_widget_set_sensitive(cp->forget, is);
}

static void page_looked_up(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    GError *err = nullptr;
    char *secret = keyring_lookup_finish(res, &err);
    /* The page is gone when cancelled. */
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        show_remembered(data, secret != nullptr);
    g_clear_error(&err);
    xd_luks_passphrase_free(secret);
}

static void check_remembered(crypt_page *cp) {
    keyring_lookup(cp->uuid, cp->cancel, page_looked_up, cp);
}

static void page_cleared(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    keyring_clear_finish(res);
    GtkWidget **weak = data;
    if (*weak != nullptr) {
        check_remembered(g_object_get_data(G_OBJECT(*weak), "xd-crypt-page"));
        g_object_remove_weak_pointer(G_OBJECT(*weak), (gpointer *)weak);
    }
    g_free(weak);
}

static void on_forget(GtkButton *button, gpointer data) {
    crypt_page *cp = data;
    GtkWidget **weak = g_new0(GtkWidget *, 1);
    *weak = GTK_WIDGET(button);
    g_object_add_weak_pointer(G_OBJECT(button), (gpointer *)weak);
    keyring_clear(cp->uuid, page_cleared, weak);
}

static void page_locked(gpointer data) {
    GtkWidget **weak = data;
    if (*weak != nullptr) {
        crypt_page *cp = g_object_get_data(G_OBJECT(*weak), "xd-crypt-page");
        gtk_label_set_text(GTK_LABEL(cp->status), "Locked");
        gtk_widget_set_sensitive(cp->lock, FALSE);
        g_object_remove_weak_pointer(G_OBJECT(*weak), (gpointer *)weak);
    }
    g_free(weak);
}

static void on_lock(GtkButton *button, gpointer data) {
    crypt_page *cp = data;
    GtkWidget **weak = g_new0(GtkWidget *, 1);
    *weak = GTK_WIDGET(button);
    g_object_add_weak_pointer(G_OBJECT(button), (gpointer *)weak);
    lock_with(GTK_WINDOW(cp->dialog), cp->item, page_locked, weak);
}

typedef struct change_job {
    GtkWidget *dialog; /* weak */
    char *uuid, *name, *passphrase;
    gboolean update_keyring;
} change_job;

static void change_job_free(change_job *change_request) {
    if (change_request->dialog != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(change_request->dialog),
                                     (gpointer *)&change_request->dialog);
    g_free(change_request->uuid);
    g_free(change_request->name);
    xd_luks_passphrase_free(change_request->passphrase);
    g_free(change_request);
}

static void changed(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    change_job *change_request = data;
    char *dbus_error = nullptr;
    GError *err = nullptr;
    GVariant *variant = xd_udisks_call_finish(res, &dbus_error, &err);
    g_clear_pointer(&variant, g_variant_unref);
    const char *title = "Change Passphrase";
    if (err == nullptr) {
        /* A remembered passphrase follows, or it would stop working. */
        if (change_request->update_keyring)
            remember(change_request->dialog, change_request->uuid, change_request->name,
                     change_request->passphrase);
        if (change_request->dialog != nullptr)
            xd_message_box(GTK_WINDOW(change_request->dialog), title, XD_MB_INFO, XD_MB_OK, FALSE,
                           "The passphrase has been changed.");
    } else if (change_request->dialog != nullptr && !dismissed(dbus_error)) {
        char *msg = xd_luks_wrong_passphrase(dbus_error, err->message)
                        ? g_strdup("The old passphrase you typed is incorrect. The passphrase "
                                   "has not been changed.")
                        : g_strdup_printf("The passphrase could not be changed: %s",
                                          err->message);
        xd_message_box(GTK_WINDOW(change_request->dialog), title, XD_MB_ERROR, XD_MB_OK, FALSE,
                       msg);
        g_free(msg);
    }
    g_clear_error(&err);
    g_free(dbus_error);
    change_job_free(change_request);
}

static void on_change(GtkButton *button, gpointer data) {
    (void)button;
    crypt_page *cp = data;
    char *old = nullptr, *new_passphrase = nullptr;
    if (!xd_luks_ask_change(GTK_WINDOW(cp->dialog), cp->name, &old, &new_passphrase))
        return;
    change_job *change_request = g_new0(change_job, 1);
    change_request->dialog = cp->dialog;
    g_object_add_weak_pointer(G_OBJECT(change_request->dialog),
                              (gpointer *)&change_request->dialog);
    change_request->uuid = g_strdup(cp->uuid);
    change_request->name = g_strdup(cp->name);
    change_request->passphrase = new_passphrase;
    /* The forget button is live exactly when one is remembered. */
    change_request->update_keyring = gtk_widget_get_sensitive(cp->forget);
    xd_udisks_call(cp->object, XD_UDISKS_ENCRYPTED, "ChangePassphrase",
                   g_variant_new("(ssa{sv})", old, new_passphrase, nullptr), nullptr, changed,
                   change_request);
    xd_luks_passphrase_free(old);
}

GtkWidget *xd_encrypted_page(GtkWidget *dialog, GFile *item, const char *name) {
    g_autoptr(XdUdisksCrypt) crypt = xd_udisks_crypt_for(item);
    if (crypt == nullptr)
        return nullptr;
    crypt_page *cp = g_new0(crypt_page, 1);
    cp->dialog = dialog;
    cp->item = g_object_ref(item);
    cp->object = g_strdup(crypt->object);
    cp->uuid = g_strdup(crypt->uuid);
    cp->name = g_strdup(name);
    cp->cancel = g_cancellable_new();

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    g_object_set_data_full(G_OBJECT(page), "xd-crypt-page", cp, crypt_page_free);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-password", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    char *version = xd_luks_version_label(crypt->version);
    char *about = g_strdup_printf("This drive is encrypted (%s). What's on it can only be read "
                                  "once it's unlocked with its passphrase.",
                                  version);
    GtkWidget *text = gtk_label_new(about);
    g_free(about);
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 44);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_box_pack_start(GTK_BOX(top), text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    const char *keys[] = {"Encryption:", "Device:", "Status:"};
    const char *values[] = {version, crypt->device,
                            crypt->cleartext != nullptr ? "Unlocked" : "Locked"};
    for (int i = 0; i < 3; i++) {
        GtkWidget *key_label = gtk_label_new(keys[i]);
        gtk_label_set_xalign(GTK_LABEL(key_label), 0);
        gtk_grid_attach(GTK_GRID(grid), key_label, 0, i, 1, 1);
        GtkWidget *value_label = gtk_label_new(values[i]);
        gtk_label_set_xalign(GTK_LABEL(value_label), 0);
        gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
        gtk_widget_set_can_focus(value_label, FALSE);
        gtk_grid_attach(GTK_GRID(grid), value_label, 1, i, 1, 1);
        if (i == 2)
            cp->status = value_label;
    }
    g_free(version);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    /* XP's group boxes: what can be done, each with its button. */
    GtkWidget *frame = gtk_frame_new("Passphrase");
    GtkWidget *fbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(fbox), 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *what = gtk_label_new("Changes the passphrase the drive is unlocked with.");
    gtk_label_set_line_wrap(GTK_LABEL(what), TRUE);
    gtk_label_set_xalign(GTK_LABEL(what), 0);
    gtk_box_pack_start(GTK_BOX(row), what, TRUE, TRUE, 0);
    GtkWidget *change = gtk_button_new_with_mnemonic("C_hange Passphrase...");
    gtk_widget_set_valign(change, GTK_ALIGN_CENTER);
    g_signal_connect(change, "clicked", G_CALLBACK(on_change), cp);
    gtk_box_pack_start(GTK_BOX(row), change, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbox), row, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    cp->remembered = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(cp->remembered), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(cp->remembered), 32);
    gtk_label_set_xalign(GTK_LABEL(cp->remembered), 0);
    gtk_box_pack_start(GTK_BOX(row), cp->remembered, TRUE, TRUE, 0);
    cp->forget = gtk_button_new_with_mnemonic("_Forget Passphrase");
    gtk_widget_set_valign(cp->forget, GTK_ALIGN_CENTER);
    g_signal_connect(cp->forget, "clicked", G_CALLBACK(on_forget), cp);
    gtk_box_pack_start(GTK_BOX(row), cp->forget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbox), row, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), fbox);
    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 0);
    show_remembered(cp, FALSE);
    check_remembered(cp);

    frame = gtk_frame_new("Lock");
    fbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(fbox), 8);
    what = gtk_label_new("Unmounts the drive and locks it: its passphrase is needed to open it "
                         "again.");
    gtk_label_set_line_wrap(GTK_LABEL(what), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(what), 32);
    gtk_label_set_xalign(GTK_LABEL(what), 0);
    gtk_box_pack_start(GTK_BOX(fbox), what, TRUE, TRUE, 0);
    cp->lock = gtk_button_new_with_mnemonic("_Lock Now");
    gtk_widget_set_valign(cp->lock, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(cp->lock, crypt->cleartext != nullptr);
    g_signal_connect(cp->lock, "clicked", G_CALLBACK(on_lock), cp);
    gtk_box_pack_start(GTK_BOX(fbox), cp->lock, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), fbox);
    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 0);
    /* Their callbacks find the page through these. */
    g_object_set_data(G_OBJECT(cp->forget), "xd-crypt-page", cp);
    g_object_set_data(G_OBJECT(cp->lock), "xd-crypt-page", cp);
    return page;
}
