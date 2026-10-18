/* SPDX-License-Identifier: MIT
 * GnuPG keys through gpgme: the keyring's public and secret keys,
 * importing, exporting, making, deleting and changing expiry dates.
 *
 * Passphrases are asked for by the caller (loopback pinentry): gpg-agent
 * hands the question to xd_gpg's callback instead of starting a pinentry
 * program of its own, so they're asked in our own dialogs.
 */
#ifndef XD_KEYS_GPGKEYS_H
#define XD_KEYS_GPGKEYS_H

#include <glib.h>

typedef struct xd_gpg_key {
    char *fingerprint;
    char *key_id;          /* the last 16 hex digits */
    char *name, *email, *comment; /* the primary user ID's */
    char **user_ids;       /* all of them, as "Name (comment) <email>" */
    char *algorithm;       /* "ed25519", "rsa4096"... */
    gint64 created, expires; /* 0 = never expires */
    int validity;          /* the primary user ID's (gpgme_validity_t) */
    int owner_trust;
    bool secret;           /* we have the secret key */
    bool expired, revoked, disabled, invalid;
    int subkeys;
} xd_gpg_key;

void xd_gpg_key_free(xd_gpg_key *k);

/* Asked for a passphrase: `hint` is the key's ("ID Name <email>"), `again`
 * after a wrong one. Returns a g_malloc'd passphrase (cleared and freed
 * by the caller), or nullptr to cancel. */
typedef char *(*xd_gpg_passphrase_fn)(void *data, const char *hint, bool again);

typedef struct xd_gpg xd_gpg;

/* A gpgme context for OpenPGP keys in `homedir` (nullptr: GNUPGHOME or
 * ~/.gnupg); nullptr with `error` set if gpg can't be used. */
xd_gpg *xd_gpg_new(const char *homedir, xd_gpg_passphrase_fn ask, void *data, GError **error);
void xd_gpg_free(xd_gpg *g);

/* The keyring's public keys (secret ones marked), sorted by name; nullptr
 * with `error` set if they couldn't be listed. */
GPtrArray *xd_gpg_list(xd_gpg *g, GError **error);

/* A new key pair for `user_id` (xd_gpg_user_id) with gpg's default
 * algorithms (Ed25519 for signing, Curve25519 for encryption), protected
 * by `passphrase` ("" for none), expiring `expires` seconds from now (0:
 * never). Its fingerprint in `fingerprint` (g_free it), if wanted. */
bool xd_gpg_create(xd_gpg *g, const char *user_id, const char *passphrase, guint64 expires,
                   char **fingerprint, GError **error);

/* Imports the keys in a file (armoured or binary); how many were new or
 * changed and how many were there already. */
bool xd_gpg_import_file(xd_gpg *g, const char *path, int *imported, int *unchanged,
                        GError **error);
/* The public key, armoured. g_free it. */
char *xd_gpg_export(xd_gpg *g, const char *fingerprint, GError **error);
/* Deletes the key, with its secret key if there is one (no questions:
 * the caller has asked). */
bool xd_gpg_delete(xd_gpg *g, const char *fingerprint, GError **error);
/* Sets when the key and its subkeys expire: `expires` seconds from now,
 * 0 = never. Needs the secret key (and its passphrase). */
bool xd_gpg_set_expiry(xd_gpg *g, const char *fingerprint, guint64 expires, GError **error);

/* Whether the operation was cancelled at the passphrase question. */
bool xd_gpg_cancelled(const GError *error);

#endif
