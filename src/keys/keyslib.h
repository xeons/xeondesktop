/* SPDX-License-Identifier: MIT
 * The parts of Passwords and Keys (xd-keys) that need no window and no
 * service: texts for what the services report, ssh-keygen's and
 * ssh-add's fingerprint lines, and the askpass helper.
 */
#ifndef XD_KEYS_KEYSLIB_H
#define XD_KEYS_KEYSLIB_H

#include <glib.h>

/* A date and time as the lists show them (the locale's date and time),
 * from seconds since 1970; "" for 0 (unknown). g_free it. */
char *xd_keys_date(gint64 seconds);
/* A date alone ("" for 0). g_free it. */
char *xd_keys_day(gint64 seconds);

/* What kind of password a Secret Service item is, from its xdg:schema
 * attribute (nullptr or "" without one): "Network password", ... */
const char *xd_secret_kind(const char *schema);
/* An item's name in the list: its label, else its schema's kind. */
const char *xd_secret_display_name(const char *label, const char *schema);

/* One line of `ssh-keygen -l` or `ssh-add -l`:
 *   256 SHA256:pnWl...FI0 user@host (ED25519)
 * Its parts (each nullable; g_free the strings). The comment may hold
 * spaces, or be "no comment". */
bool xd_ssh_parse_fingerprint(const char *line, int *bits, char **fingerprint, char **comment,
                              char **type);

/* A key file name in `dir` for a new key of `type` ("ed25519", "rsa",
 * "ecdsa"): id_TYPE, else id_TYPE_2, id_TYPE_3... whichever doesn't exist
 * yet (neither it nor its .pub). g_free it. */
char *xd_ssh_new_key_name(const char *dir, const char *type);

/* Whether `name` is a usable key file name in ~/.ssh: no slashes, not
 * hidden, not ".pub", not a file ssh itself keeps there (config,
 * known_hosts, authorized_keys...). */
bool xd_ssh_valid_key_name(const char *name);

/* The environment variable that makes xd-keys (or xd-tests) the askpass
 * program ssh-keygen and ssh-add run: */
#define XD_KEYS_ASKPASS_ENV "XD_KEYS_ASKPASS"
/* As askpass: answers with the next line of the standard input it shares
 * with the ssh-keygen or ssh-add that ran it, read a byte at a time so the
 * next askpass gets the line after; exits 1 at the end (no answer: ssh
 * gives up rather than asking again). The caller writes one line per
 * question it expects. Returns the exit status. */
int xd_keys_askpass_main(void);

/* A GnuPG user ID: "Name (comment) <email>", leaving out what's empty.
 * g_free it. */
char *xd_gpg_user_id(const char *name, const char *email, const char *comment);
/* gpgme's validity (gpgme_validity_t) as Seahorse shows trust: "Unknown",
 * "Never", "Marginal", "Full", "Ultimate". */
const char *xd_gpg_validity_text(int validity);

#endif
