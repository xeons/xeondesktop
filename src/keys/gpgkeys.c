/* SPDX-License-Identifier: MIT */
#include <locale.h>
#include <string.h>

#include <gpgme.h>

#include "gpgkeys.h"

struct xd_gpg {
    gpgme_ctx_t ctx;
    xd_gpg_passphrase_fn ask;
    void *data;
    /* The new key's passphrase while xd_gpg_create runs. */
    const char *pending;
};

#define XD_GPG_ERROR (xd_gpg_error_quark())
static GQuark xd_gpg_error_quark(void) { return g_quark_from_static_string("xd-gpg-error"); }

static bool fail(GError **error, gpgme_error_t err, const char *what) {
    gpgme_err_code_t code = gpgme_err_code(err);
    g_set_error(error, XD_GPG_ERROR, (int)code, "%s: %s", what, gpgme_strerror(err));
    return false;
}

bool xd_gpg_cancelled(const GError *error) {
    return error != nullptr && error->domain == XD_GPG_ERROR &&
           (error->code == GPG_ERR_CANCELED || error->code == GPG_ERR_FULLY_CANCELED);
}

void xd_gpg_key_free(xd_gpg_key *key) {
    if (key == nullptr)
        return;
    g_free(key->fingerprint);
    g_free(key->key_id);
    g_free(key->name);
    g_free(key->email);
    g_free(key->comment);
    g_strfreev(key->user_ids);
    g_free(key->algorithm);
    g_free(key);
}

static gpgme_error_t on_passphrase(void *hook, const char *uid_hint,
                                   [[maybe_unused]] const char *info, int prev_was_bad, int fd) {
    xd_gpg *gpg = hook;
    char *pass = nullptr;
    if (gpg->pending != nullptr && !prev_was_bad)
        pass = g_strdup(gpg->pending);
    else if (gpg->ask != nullptr)
        pass = gpg->ask(gpg->data, uid_hint != nullptr ? uid_hint : "", prev_was_bad != 0);
    if (pass == nullptr)
        return gpgme_error(GPG_ERR_CANCELED);
    size_t len = strlen(pass);
    bool ok = gpgme_io_writen(fd, pass, len) == 0 && gpgme_io_writen(fd, "\n", 1) == 0;
    memset(pass, 0, len);
    g_free(pass);
    return ok ? 0 : gpgme_error(GPG_ERR_GENERAL);
}

xd_gpg *xd_gpg_new(const char *homedir, xd_gpg_passphrase_fn ask, void *data, GError **error) {
    /* Once per process, before any context. */
    static bool initialised;
    if (!initialised) {
        gpgme_check_version(nullptr);
        gpgme_set_locale(nullptr, LC_CTYPE, setlocale(LC_CTYPE, nullptr));
        initialised = true;
    }
    gpgme_error_t err = gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP);
    if (err != 0) {
        fail(error, err, "GnuPG can't be used");
        return nullptr;
    }
    xd_gpg *gpg = g_new0(xd_gpg, 1);
    gpg->ask = ask;
    gpg->data = data;
    err = gpgme_new(&gpg->ctx);
    if (err == 0)
        err = gpgme_set_protocol(gpg->ctx, GPGME_PROTOCOL_OpenPGP);
    if (err == 0 && homedir != nullptr)
        err = gpgme_ctx_set_engine_info(gpg->ctx, GPGME_PROTOCOL_OpenPGP, nullptr, homedir);
    if (err == 0)
        err = gpgme_set_pinentry_mode(gpg->ctx, GPGME_PINENTRY_MODE_LOOPBACK);
    if (err != 0) {
        fail(error, err, "GnuPG can't be used");
        xd_gpg_free(gpg);
        return nullptr;
    }
    gpgme_set_passphrase_cb(gpg->ctx, on_passphrase, gpg);
    gpgme_set_armor(gpg->ctx, 1);
    return gpg;
}

void xd_gpg_free(xd_gpg *gpg) {
    if (gpg == nullptr)
        return;
    if (gpg->ctx != nullptr)
        gpgme_release(gpg->ctx);
    g_free(gpg);
}

static xd_gpg_key *make_key(gpgme_key_t key) {
    xd_gpg_key *gpg_key = g_new0(xd_gpg_key, 1);
    gpgme_subkey_t primary = key->subkeys;
    if (primary != nullptr) {
        gpg_key->fingerprint = g_strdup(primary->fpr);
        gpg_key->key_id = g_strdup(primary->keyid);
        gpg_key->algorithm = gpgme_pubkey_algo_string(primary);
        if (gpg_key->algorithm != nullptr) {
            /* gpgme's malloc, our g_free. */
            char *algorithm = g_strdup(gpg_key->algorithm);
            gpgme_free(gpg_key->algorithm);
            gpg_key->algorithm = algorithm;
        }
        gpg_key->created = primary->timestamp > 0 ? (gint64)primary->timestamp : 0;
        gpg_key->expires = primary->expires > 0 ? (gint64)primary->expires : 0;
    }
    for (gpgme_subkey_t subkey = key->subkeys; subkey != nullptr; subkey = subkey->next)
        gpg_key->subkeys++;
    GPtrArray *ids = g_ptr_array_new();
    for (gpgme_user_id_t user_id = key->uids; user_id != nullptr; user_id = user_id->next) {
        if (gpg_key->name == nullptr) {
            gpg_key->name = g_strdup(user_id->name != nullptr ? user_id->name : "");
            gpg_key->email = g_strdup(user_id->email != nullptr ? user_id->email : "");
            gpg_key->comment = g_strdup(user_id->comment != nullptr ? user_id->comment : "");
            gpg_key->validity = (int)user_id->validity;
        }
        g_ptr_array_add(ids, g_strdup(user_id->uid != nullptr ? user_id->uid : ""));
    }
    g_ptr_array_add(ids, nullptr);
    gpg_key->user_ids = (char **)g_ptr_array_free(ids, FALSE);
    if (gpg_key->name == nullptr) {
        gpg_key->name = g_strdup("");
        gpg_key->email = g_strdup("");
        gpg_key->comment = g_strdup("");
    }
    gpg_key->owner_trust = (int)key->owner_trust;
    gpg_key->expired = key->expired;
    gpg_key->revoked = key->revoked;
    gpg_key->disabled = key->disabled;
    gpg_key->invalid = key->invalid;
    return gpg_key;
}

static gint by_name(gconstpointer left, gconstpointer right) {
    const xd_gpg_key *left_key = *(xd_gpg_key *const *)left;
    const xd_gpg_key *right_key = *(xd_gpg_key *const *)right;
    int order = g_utf8_collate(left_key->name, right_key->name);
    return order != 0 ? order : g_strcmp0(left_key->fingerprint, right_key->fingerprint);
}

/* Each key in the keyring (or its secret keys): `fn` with it. */
static bool each_key(xd_gpg *gpg, bool secret, void (*fn)(gpgme_key_t, void *), void *data,
                     GError **error) {
    gpgme_set_keylist_mode(gpg->ctx, GPGME_KEYLIST_MODE_LOCAL);
    gpgme_error_t err = gpgme_op_keylist_start(gpg->ctx, nullptr, secret ? 1 : 0);
    gpgme_key_t key = nullptr;
    while (err == 0 && (err = gpgme_op_keylist_next(gpg->ctx, &key)) == 0) {
        fn(key, data);
        gpgme_key_unref(key);
    }
    gpgme_op_keylist_end(gpg->ctx);
    if (gpgme_err_code(err) != GPG_ERR_EOF)
        return fail(error, err, "The keys could not be listed");
    return true;
}

static void add_public(gpgme_key_t key, void *data) {
    g_ptr_array_add(data, make_key(key));
}

static void mark_secret(gpgme_key_t key, void *data) {
    GPtrArray *keys = data;
    if (key->subkeys == nullptr)
        return;
    for (guint i = 0; i < keys->len; i++) {
        xd_gpg_key *gpg_key = keys->pdata[i];
        if (g_strcmp0(gpg_key->fingerprint, key->subkeys->fpr) == 0)
            gpg_key->secret = true;
    }
}

GPtrArray *xd_gpg_list(xd_gpg *gpg, GError **error) {
    GPtrArray *keys = g_ptr_array_new_with_free_func((GDestroyNotify)xd_gpg_key_free);
    if (!each_key(gpg, false, add_public, keys, error) ||
        !each_key(gpg, true, mark_secret, keys, error)) {
        g_ptr_array_unref(keys);
        return nullptr;
    }
    g_ptr_array_sort(keys, by_name);
    return keys;
}

bool xd_gpg_create(xd_gpg *gpg, const char *user_id, const char *passphrase, guint64 expires,
                   char **fingerprint, GError **error) {
    unsigned int flags = 0;
    if (passphrase == nullptr || passphrase[0] == '\0')
        flags |= GPGME_CREATE_NOPASSWD;
    if (expires == 0)
        flags |= GPGME_CREATE_NOEXPIRE;
    gpg->pending = passphrase;
    gpgme_error_t err =
        gpgme_op_createkey(gpg->ctx, user_id, "default", 0, (unsigned long)expires, nullptr, flags);
    gpg->pending = nullptr;
    if (err != 0)
        return fail(error, err, "The key could not be made");
    gpgme_genkey_result_t genkey_result = gpgme_op_genkey_result(gpg->ctx);
    if (fingerprint != nullptr)
        *fingerprint = g_strdup(genkey_result != nullptr ? genkey_result->fpr : nullptr);
    return true;
}

bool xd_gpg_import_file(xd_gpg *gpg, const char *path, int *imported, int *unchanged,
                        GError **error) {
    gpgme_data_t in = nullptr;
    gpgme_error_t err = gpgme_data_new_from_file(&in, path, 1);
    if (err != 0)
        return fail(error, err, "The file could not be read");
    err = gpgme_op_import(gpg->ctx, in);
    gpgme_data_release(in);
    if (err != 0)
        return fail(error, err, "The keys could not be imported");
    gpgme_import_result_t import_result = gpgme_op_import_result(gpg->ctx);
    if (import_result == nullptr || import_result->considered == 0) {
        g_set_error_literal(error, XD_GPG_ERROR, GPG_ERR_NO_DATA,
                            "The file holds no OpenPGP keys.");
        return false;
    }
    if (imported != nullptr)
        *imported = import_result->imported + import_result->secret_imported;
    if (unchanged != nullptr)
        *unchanged = import_result->unchanged;
    return true;
}

char *xd_gpg_export(xd_gpg *gpg, const char *fingerprint, GError **error) {
    gpgme_data_t out = nullptr;
    gpgme_error_t err = gpgme_data_new(&out);
    if (err == 0)
        err = gpgme_op_export(gpg->ctx, fingerprint, 0, out);
    if (err != 0) {
        if (out != nullptr)
            gpgme_data_release(out);
        fail(error, err, "The key could not be exported");
        return nullptr;
    }
    size_t len = 0;
    char *buf = gpgme_data_release_and_get_mem(out, &len);
    if (buf == nullptr || len == 0) {
        gpgme_free(buf);
        g_set_error_literal(error, XD_GPG_ERROR, GPG_ERR_NO_PUBKEY,
                            "The key could not be exported: it isn't in the keyring.");
        return nullptr;
    }
    char *text = g_strndup(buf, len);
    gpgme_free(buf);
    return text;
}

static gpgme_key_t get_key(xd_gpg *gpg, const char *fingerprint, GError **error) {
    gpgme_key_t key = nullptr;
    gpgme_error_t err = gpgme_get_key(gpg->ctx, fingerprint, &key, 0);
    if (err != 0) {
        fail(error, err, "The key isn't in the keyring");
        return nullptr;
    }
    return key;
}

bool xd_gpg_delete(xd_gpg *gpg, const char *fingerprint, GError **error) {
    gpgme_key_t key = get_key(gpg, fingerprint, error);
    if (key == nullptr)
        return false;
    /* FORCE: gpg would otherwise ask again through a pinentry. */
    gpgme_error_t err =
        gpgme_op_delete_ext(gpg->ctx, key, GPGME_DELETE_ALLOW_SECRET | GPGME_DELETE_FORCE);
    gpgme_key_unref(key);
    if (err != 0)
        return fail(error, err, "The key could not be deleted");
    return true;
}

bool xd_gpg_set_expiry(xd_gpg *gpg, const char *fingerprint, guint64 expires, GError **error) {
    gpgme_key_t key = get_key(gpg, fingerprint, error);
    if (key == nullptr)
        return false;
    /* The primary key, then all subkeys ("*"), as Seahorse's whole-key
     * expiry does: an encryption subkey left to expire first would make
     * the key useless for encryption before the date shown. */
    gpgme_error_t err = gpgme_op_setexpire(gpg->ctx, key, (unsigned long)expires, nullptr, 0);
    if (err == 0 && key->subkeys != nullptr && key->subkeys->next != nullptr)
        err = gpgme_op_setexpire(gpg->ctx, key, (unsigned long)expires, "*", 0);
    gpgme_key_unref(key);
    if (err != 0)
        return fail(error, err, "The expiry date could not be changed");
    return true;
}
