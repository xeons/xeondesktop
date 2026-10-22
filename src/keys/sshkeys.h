/* SPDX-License-Identifier: MIT
 * SSH key pairs in a folder (~/.ssh), through OpenSSH's own tools:
 * ssh-keygen lists, makes and re-protects them, ssh-add hands them to the
 * agent. Passphrases go to those tools through askpass (keyslib.h), never
 * on a command line, where any user could read them.
 */
#ifndef XD_KEYS_SSHKEYS_H
#define XD_KEYS_SSHKEYS_H

#include <gio/gio.h>

typedef struct xd_ssh_key {
    char *name;         /* the private key's file name (id_ed25519) */
    char *path;         /* the private key */
    char *pub_path;     /* the public key, path + ".pub" */
    char *type;         /* ED25519, RSA, ECDSA... */
    int bits;
    char *fingerprint;  /* SHA256:... */
    char *comment;
    bool has_private;   /* the private half is there */
    bool encrypted;     /* the private key has a passphrase */
    bool in_agent;      /* ssh-agent holds it */
} xd_ssh_key;

void xd_ssh_key_free(xd_ssh_key *key);

/* The key pairs in `dir`: every *.pub that ssh-keygen reads, with its
 * private half beside it if there is one; sorted by name. Never nullptr. */
GPtrArray *xd_ssh_list_keys(const char *dir);

/* The fingerprints (SHA256:...) of the keys ssh-agent holds; nullptr with
 * `error` set if no agent could be reached (no SSH_AUTH_SOCK, or nothing
 * listening there). */
char **xd_ssh_agent_fingerprints(GError **error);
/* Marks the keys the agent holds (the fingerprints from above). */
void xd_ssh_mark_agent_keys(GPtrArray *keys, char *const *fingerprints);

/* Starts an OpenSSH tool with `askpass` (xd-keys itself) as its askpass
 * program. Its standard input, output and error are pipes, for
 * g_subprocess_communicate_utf8(): the input is the answers to its
 * passphrase questions (xd_ssh_answers), which the askpass it runs reads
 * a line at a time. */
GSubprocess *xd_ssh_spawn(const char *const *argv, const char *askpass, GError **error);
/* The standard input for xd_ssh_spawn's tool: the answers, a line each.
 * g_free it (after clearing it: it holds passphrases). */
char *xd_ssh_answers(const char *const *answers);

/* Runs an OpenSSH tool to the end (xd_ssh_spawn), feeding it `answers`;
 * false with `error` set (the tool's own message) if it failed. */
bool xd_ssh_run(const char *const *argv, const char *askpass, const char *const *answers,
                GError **error);

/* The argument lists for ssh-keygen and ssh-add (g_strfreev them). `type`
 * is "ed25519", "rsa" or "ecdsa"; `bits` 0 for its default. With
 * `empty_passphrase`, ssh-keygen asks for none (-N ""); otherwise it asks
 * twice (new, again), and -p asks for the old one first if `encrypted`. */
char **xd_ssh_keygen_argv(const char *type, int bits, const char *comment, const char *path,
                          bool empty_passphrase);
char **xd_ssh_passwd_argv(const char *path, bool empty_passphrase);
char **xd_ssh_add_argv(const char *path, bool remove);

/* Whether the private key at `path` has a passphrase (ssh-keygen can't
 * read it with an empty one). */
bool xd_ssh_key_encrypted(const char *path);

/* ssh-keygen's or ssh-add's last line of complaint, tidied for a message
 * box ("incorrect passphrase supplied to decrypt private key"). g_free
 * it. */
char *xd_ssh_error_text(const char *stderr_text);

#endif
