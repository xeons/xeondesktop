/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "keyslib.h"
#include "sshkeys.h"

void xd_ssh_key_free(xd_ssh_key *key) {
    if (key == nullptr)
        return;
    g_free(key->name);
    g_free(key->path);
    g_free(key->pub_path);
    g_free(key->type);
    g_free(key->fingerprint);
    g_free(key->comment);
    g_free(key);
}

/* Runs a tool that needs no answers and returns its output, or nullptr if
 * it failed. */
static char *run_quiet(const char *const *argv, int *status) {
    char *out = nullptr;
    int st = -1;
    bool ok = g_spawn_sync(nullptr, (char **)(uintptr_t)argv, nullptr,
                           G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL |
                               G_SPAWN_STDIN_FROM_DEV_NULL,
                           nullptr, nullptr, &out, nullptr, &st, nullptr);
    if (status != nullptr)
        *status = ok ? st : -1;
    if (!ok || !g_spawn_check_wait_status(st, nullptr)) {
        g_free(out);
        return nullptr;
    }
    return out;
}

bool xd_ssh_key_encrypted(const char *path) {
    /* An unprotected key reads with an empty passphrase; a protected one
     * is refused before any key derivation is tried. */
    const char *argv[] = {"ssh-keygen", "-y", "-P", "", "-f", path, nullptr};
    char *out = run_quiet(argv, nullptr);
    bool encrypted = out == nullptr;
    g_free(out);
    return encrypted;
}

static gint by_name(gconstpointer left, gconstpointer right) {
    const xd_ssh_key *left_key = *(xd_ssh_key *const *)left;
    const xd_ssh_key *right_key = *(xd_ssh_key *const *)right;
    return g_utf8_collate(left_key->name, right_key->name);
}

static xd_ssh_key *read_key(const char *dir, const char *pub_name) {
    char *pub = g_build_filename(dir, pub_name, nullptr);
    const char *argv[] = {"ssh-keygen", "-l", "-f", pub, nullptr};
    char *out = run_quiet(argv, nullptr);
    xd_ssh_key *key = nullptr;
    if (out != nullptr) {
        key = g_new0(xd_ssh_key, 1);
        if (!xd_ssh_parse_fingerprint(out, &key->bits, &key->fingerprint, &key->comment,
                                      &key->type)) {
            xd_ssh_key_free(key);
            key = nullptr;
        }
    }
    if (key != nullptr) {
        key->name = g_strndup(pub_name, strlen(pub_name) - strlen(".pub"));
        key->pub_path = pub;
        pub = nullptr;
        key->path = g_build_filename(dir, key->name, nullptr);
        key->has_private = g_file_test(key->path, G_FILE_TEST_IS_REGULAR);
        key->encrypted = key->has_private && xd_ssh_key_encrypted(key->path);
    }
    g_free(out);
    g_free(pub);
    return key;
}

GPtrArray *xd_ssh_list_keys(const char *dir) {
    GPtrArray *keys = g_ptr_array_new_with_free_func((GDestroyNotify)xd_ssh_key_free);
    GDir *key_dir = g_dir_open(dir, 0, nullptr);
    if (key_dir == nullptr)
        return keys;
    for (const char *name; (name = g_dir_read_name(key_dir)) != nullptr;) {
        if (!g_str_has_suffix(name, ".pub") || strlen(name) <= strlen(".pub"))
            continue;
        xd_ssh_key *key = read_key(dir, name);
        if (key != nullptr)
            g_ptr_array_add(keys, key);
    }
    g_dir_close(key_dir);
    g_ptr_array_sort(keys, by_name);
    return keys;
}

char **xd_ssh_agent_fingerprints(GError **error) {
    const char *sock = g_getenv("SSH_AUTH_SOCK");
    if (sock == nullptr || sock[0] == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No SSH agent is running.");
        return nullptr;
    }
    /* ssh-add -l: 0 with keys, 1 with none, 2 without an agent. */
    const char *argv[] = {"ssh-add", "-l", nullptr};
    char *out = nullptr;
    int st = -1;
    if (!g_spawn_sync(nullptr, (char **)(uintptr_t)argv, nullptr,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL |
                          G_SPAWN_STDIN_FROM_DEV_NULL,
                      nullptr, nullptr, &out, nullptr, &st, error))
        return nullptr;
    GError *wait_error = nullptr;
    bool ok = g_spawn_check_wait_status(st, &wait_error);
    if (!ok && !g_error_matches(wait_error, G_SPAWN_EXIT_ERROR, 1)) {
        g_clear_error(&wait_error);
        g_free(out);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                            "The SSH agent can't be reached.");
        return nullptr;
    }
    g_clear_error(&wait_error);
    GPtrArray *fps = g_ptr_array_new();
    char **lines = g_strsplit(out != nullptr ? out : "", "\n", -1);
    for (char **line = lines; ok && *line != nullptr; line++) {
        char *fp = nullptr;
        if (xd_ssh_parse_fingerprint(*line, nullptr, &fp, nullptr, nullptr))
            g_ptr_array_add(fps, fp);
    }
    g_strfreev(lines);
    g_free(out);
    g_ptr_array_add(fps, nullptr);
    return (char **)g_ptr_array_free(fps, FALSE);
}

void xd_ssh_mark_agent_keys(GPtrArray *keys, char *const *fingerprints) {
    for (guint i = 0; i < keys->len; i++) {
        xd_ssh_key *key = keys->pdata[i];
        key->in_agent = fingerprints != nullptr &&
                        g_strv_contains((const char *const *)fingerprints, key->fingerprint);
    }
}

GSubprocess *xd_ssh_spawn(const char *const *argv, const char *askpass, GError **error) {
    GSubprocessLauncher *launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                  G_SUBPROCESS_FLAGS_STDERR_PIPE);
    /* Always askpass, even started from a terminal; OpenSSH isn't
     * translated, so its messages are the same everywhere. */
    g_subprocess_launcher_setenv(launcher, "SSH_ASKPASS", askpass, TRUE);
    g_subprocess_launcher_setenv(launcher, "SSH_ASKPASS_REQUIRE", "force", TRUE);
    g_subprocess_launcher_setenv(launcher, XD_KEYS_ASKPASS_ENV, "1", TRUE);
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, error);
    g_object_unref(launcher);
    return process;
}

char *xd_ssh_answers(const char *const *answers) {
    GString *string = g_string_new(nullptr);
    for (const char *const *each_answer = answers;
         each_answer != nullptr && *each_answer != nullptr; each_answer++) {
        g_string_append(string, *each_answer);
        g_string_append_c(string, '\n');
    }
    return g_string_free(string, FALSE);
}

char *xd_ssh_error_text(const char *stderr_text) {
    char *last = nullptr;
    char **lines = g_strsplit(stderr_text != nullptr ? stderr_text : "", "\n", -1);
    for (char **line = lines; *line != nullptr; line++) {
        char *text = g_strstrip(*line);
        if (text[0] != '\0') {
            g_free(last);
            last = g_strdup(text);
        }
    }
    g_strfreev(lines);
    if (last == nullptr)
        return g_strdup("The program failed without saying why.");
    /* "Load key "/home/u/.ssh/id": incorrect passphrase supplied..." and
     * "Failed to load key ...: incorrect passphrase supplied...": the
     * path is in the dialog already. */
    char *colon = strstr(last, "\": ");
    if (colon != nullptr) {
        char *rest = g_strdup(colon + 3);
        g_free(last);
        last = rest;
    }
    if (last[0] != '\0')
        last[0] = g_ascii_toupper(last[0]);
    if (!g_str_has_suffix(last, ".")) {
        char *dot = g_strconcat(last, ".", nullptr);
        g_free(last);
        last = dot;
    }
    return last;
}

bool xd_ssh_run(const char *const *argv, const char *askpass, const char *const *answers,
                GError **error) {
    GSubprocess *process = xd_ssh_spawn(argv, askpass, error);
    if (process == nullptr)
        return false;
    char *in = xd_ssh_answers(answers);
    char *err = nullptr;
    bool ok = g_subprocess_communicate_utf8(process, in, nullptr, nullptr, &err, error);
    memset(in, 0, strlen(in));
    g_free(in);
    if (ok && !g_subprocess_get_successful(process)) {
        char *why = xd_ssh_error_text(err);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, why);
        g_free(why);
        ok = false;
    }
    g_free(err);
    g_object_unref(process);
    return ok;
}

char **xd_ssh_keygen_argv(const char *type, int bits, const char *comment, const char *path,
                          bool empty_passphrase) {
    GPtrArray *array = g_ptr_array_new();
    g_ptr_array_add(array, g_strdup("ssh-keygen"));
    g_ptr_array_add(array, g_strdup("-q"));
    g_ptr_array_add(array, g_strdup("-t"));
    g_ptr_array_add(array, g_strdup(type));
    if (bits > 0) {
        g_ptr_array_add(array, g_strdup("-b"));
        g_ptr_array_add(array, g_strdup_printf("%d", bits));
    }
    g_ptr_array_add(array, g_strdup("-C"));
    g_ptr_array_add(array, g_strdup(comment != nullptr ? comment : ""));
    g_ptr_array_add(array, g_strdup("-f"));
    g_ptr_array_add(array, g_strdup(path));
    if (empty_passphrase) {
        g_ptr_array_add(array, g_strdup("-N"));
        g_ptr_array_add(array, g_strdup(""));
    }
    g_ptr_array_add(array, nullptr);
    return (char **)g_ptr_array_free(array, FALSE);
}

char **xd_ssh_passwd_argv(const char *path, bool empty_passphrase) {
    GPtrArray *array = g_ptr_array_new();
    g_ptr_array_add(array, g_strdup("ssh-keygen"));
    g_ptr_array_add(array, g_strdup("-q"));
    g_ptr_array_add(array, g_strdup("-p"));
    g_ptr_array_add(array, g_strdup("-f"));
    g_ptr_array_add(array, g_strdup(path));
    if (empty_passphrase) {
        g_ptr_array_add(array, g_strdup("-N"));
        g_ptr_array_add(array, g_strdup(""));
    }
    g_ptr_array_add(array, nullptr);
    return (char **)g_ptr_array_free(array, FALSE);
}

char **xd_ssh_add_argv(const char *path, bool remove) {
    GPtrArray *array = g_ptr_array_new();
    g_ptr_array_add(array, g_strdup("ssh-add"));
    if (remove)
        g_ptr_array_add(array, g_strdup("-d"));
    g_ptr_array_add(array, g_strdup(path));
    g_ptr_array_add(array, nullptr);
    return (char **)g_ptr_array_free(array, FALSE);
}
