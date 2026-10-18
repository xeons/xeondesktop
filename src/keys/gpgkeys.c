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

void xd_gpg_key_free(xd_gpg_key *k) {
    if (k == nullptr)
        return;
    g_free(k->fingerprint);
    g_free(k->key_id);
    g_free(k->name);
    g_free(k->email);
    g_free(k->comment);
    g_strfreev(k->user_ids);
    g_free(k->algorithm);
    g_free(k);
}

static gpgme_error_t on_passphrase(void *hook, const char *uid_hint,
                                   [[maybe_unused]] const char *info, int prev_was_bad, int fd) {
    xd_gpg *g = hook;
    char *pass = nullptr;
    if (g->pending != nullptr && !prev_was_bad)
        pass = g_strdup(g->pending);
    else if (g->ask != nullptr)
        pass = g->ask(g->data, uid_hint != nullptr ? uid_hint : "", prev_was_bad != 0);
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
    xd_gpg *g = g_new0(xd_gpg, 1);
    g->ask = ask;
    g->data = data;
    err = gpgme_new(&g->ctx);
    if (err == 0)
        err = gpgme_set_protocol(g->ctx, GPGME_PROTOCOL_OpenPGP);
    if (err == 0 && homedir != nullptr)
        err = gpgme_ctx_set_engine_info(g->ctx, GPGME_PROTOCOL_OpenPGP, nullptr, homedir);
    if (err == 0)
        err = gpgme_set_pinentry_mode(g->ctx, GPGME_PINENTRY_MODE_LOOPBACK);
    if (err != 0) {
        fail(error, err, "GnuPG can't be used");
        xd_gpg_free(g);
        return nullptr;
    }
    gpgme_set_passphrase_cb(g->ctx, on_passphrase, g);
    gpgme_set_armor(g->ctx, 1);
    return g;
}

void xd_gpg_free(xd_gpg *g) {
    if (g == nullptr)
        return;
    if (g->ctx != nullptr)
        gpgme_release(g->ctx);
    g_free(g);
}

static xd_gpg_key *make_key(gpgme_key_t key) {
    xd_gpg_key *k = g_new0(xd_gpg_key, 1);
    gpgme_subkey_t primary = key->subkeys;
    if (primary != nullptr) {
        k->fingerprint = g_strdup(primary->fpr);
        k->key_id = g_strdup(primary->keyid);
        k->algorithm = gpgme_pubkey_algo_string(primary);
        if (k->algorithm != nullptr) {
            /* gpgme's malloc, our g_free. */
            char *a = g_strdup(k->algorithm);
            gpgme_free(k->algorithm);
            k->algorithm = a;
        }
        k->created = primary->timestamp > 0 ? (gint64)primary->timestamp : 0;
        k->expires = primary->expires > 0 ? (gint64)primary->expires : 0;
    }
    for (gpgme_subkey_t s = key->subkeys; s != nullptr; s = s->next)
        k->subkeys++;
    GPtrArray *ids = g_ptr_array_new();
    for (gpgme_user_id_t u = key->uids; u != nullptr; u = u->next) {
        if (k->name == nullptr) {
            k->name = g_strdup(u->name != nullptr ? u->name : "");
            k->email = g_strdup(u->email != nullptr ? u->email : "");
            k->comment = g_strdup(u->comment != nullptr ? u->comment : "");
            k->validity = (int)u->validity;
        }
        g_ptr_array_add(ids, g_strdup(u->uid != nullptr ? u->uid : ""));
    }
    g_ptr_array_add(ids, nullptr);
    k->user_ids = (char **)g_ptr_array_free(ids, FALSE);
    if (k->name == nullptr) {
        k->name = g_strdup("");
        k->email = g_strdup("");
        k->comment = g_strdup("");
    }
    k->owner_trust = (int)key->owner_trust;
    k->expired = key->expired;
    k->revoked = key->revoked;
    k->disabled = key->disabled;
    k->invalid = key->invalid;
    return k;
}

static gint by_name(gconstpointer a, gconstpointer b) {
    const xd_gpg_key *x = *(xd_gpg_key *const *)a;
    const xd_gpg_key *y = *(xd_gpg_key *const *)b;
    int c = g_utf8_collate(x->name, y->name);
    return c != 0 ? c : g_strcmp0(x->fingerprint, y->fingerprint);
}

/* Each key in the keyring (or its secret keys): `fn` with it. */
static bool each_key(xd_gpg *g, bool secret, void (*fn)(gpgme_key_t, void *), void *data,
                     GError **error) {
    gpgme_set_keylist_mode(g->ctx, GPGME_KEYLIST_MODE_LOCAL);
    gpgme_error_t err = gpgme_op_keylist_start(g->ctx, nullptr, secret ? 1 : 0);
    gpgme_key_t key = nullptr;
    while (err == 0 && (err = gpgme_op_keylist_next(g->ctx, &key)) == 0) {
        fn(key, data);
        gpgme_key_unref(key);
    }
    gpgme_op_keylist_end(g->ctx);
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
        xd_gpg_key *k = keys->pdata[i];
        if (g_strcmp0(k->fingerprint, key->subkeys->fpr) == 0)
            k->secret = true;
    }
}

GPtrArray *xd_gpg_list(xd_gpg *g, GError **error) {
    GPtrArray *keys = g_ptr_array_new_with_free_func((GDestroyNotify)xd_gpg_key_free);
    if (!each_key(g, false, add_public, keys, error) ||
        !each_key(g, true, mark_secret, keys, error)) {
        g_ptr_array_unref(keys);
        return nullptr;
    }
    g_ptr_array_sort(keys, by_name);
    return keys;
}

bool xd_gpg_create(xd_gpg *g, const char *user_id, const char *passphrase, guint64 expires,
                   char **fingerprint, GError **error) {
    unsigned int flags = 0;
    if (passphrase == nullptr || passphrase[0] == '\0')
        flags |= GPGME_CREATE_NOPASSWD;
    if (expires == 0)
        flags |= GPGME_CREATE_NOEXPIRE;
    g->pending = passphrase;
    gpgme_error_t err = gpgme_op_createkey(g->ctx, user_id, "default", 0, (unsigned long)expires,
                                           nullptr, flags);
    g->pending = nullptr;
    if (err != 0)
        return fail(error, err, "The key could not be made");
    gpgme_genkey_result_t r = gpgme_op_genkey_result(g->ctx);
    if (fingerprint != nullptr)
        *fingerprint = g_strdup(r != nullptr ? r->fpr : nullptr);
    return true;
}

bool xd_gpg_import_file(xd_gpg *g, const char *path, int *imported, int *unchanged,
                        GError **error) {
    gpgme_data_t in = nullptr;
    gpgme_error_t err = gpgme_data_new_from_file(&in, path, 1);
    if (err != 0)
        return fail(error, err, "The file could not be read");
    err = gpgme_op_import(g->ctx, in);
    gpgme_data_release(in);
    if (err != 0)
        return fail(error, err, "The keys could not be imported");
    gpgme_import_result_t r = gpgme_op_import_result(g->ctx);
    if (r == nullptr || r->considered == 0) {
        g_set_error_literal(error, XD_GPG_ERROR, GPG_ERR_NO_DATA,
                            "The file holds no OpenPGP keys.");
        return false;
    }
    if (imported != nullptr)
        *imported = r->imported + r->secret_imported;
    if (unchanged != nullptr)
        *unchanged = r->unchanged;
    return true;
}

char *xd_gpg_export(xd_gpg *g, const char *fingerprint, GError **error) {
    gpgme_data_t out = nullptr;
    gpgme_error_t err = gpgme_data_new(&out);
    if (err == 0)
        err = gpgme_op_export(g->ctx, fingerprint, 0, out);
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

static gpgme_key_t get_key(xd_gpg *g, const char *fingerprint, GError **error) {
    gpgme_key_t key = nullptr;
    gpgme_error_t err = gpgme_get_key(g->ctx, fingerprint, &key, 0);
    if (err != 0) {
        fail(error, err, "The key isn't in the keyring");
        return nullptr;
    }
    return key;
}

bool xd_gpg_delete(xd_gpg *g, const char *fingerprint, GError **error) {
    gpgme_key_t key = get_key(g, fingerprint, error);
    if (key == nullptr)
        return false;
    /* FORCE: gpg would otherwise ask again through a pinentry. */
    gpgme_error_t err =
        gpgme_op_delete_ext(g->ctx, key, GPGME_DELETE_ALLOW_SECRET | GPGME_DELETE_FORCE);
    gpgme_key_unref(key);
    if (err != 0)
        return fail(error, err, "The key could not be deleted");
    return true;
}

bool xd_gpg_set_expiry(xd_gpg *g, const char *fingerprint, guint64 expires, GError **error) {
    gpgme_key_t key = get_key(g, fingerprint, error);
    if (key == nullptr)
        return false;
    /* The primary key, then all subkeys ("*"), as Seahorse's whole-key
     * expiry does: an encryption subkey left to expire first would make
     * the key useless for encryption before the date shown. */
    gpgme_error_t err = gpgme_op_setexpire(g->ctx, key, (unsigned long)expires, nullptr, 0);
    if (err == 0 && key->subkeys != nullptr && key->subkeys->next != nullptr)
        err = gpgme_op_setexpire(g->ctx, key, (unsigned long)expires, "*", 0);
    gpgme_key_unref(key);
    if (err != 0)
        return fail(error, err, "The expiry date could not be changed");
    return true;
}
