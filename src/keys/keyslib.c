/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "keyslib.h"

static char *format_time(gint64 seconds, const char *format) {
    if (seconds <= 0)
        return g_strdup("");
    GDateTime *date_time = g_date_time_new_from_unix_local(seconds);
    if (date_time == nullptr)
        return g_strdup("");
    char *text = g_date_time_format(date_time, format);
    g_date_time_unref(date_time);
    return text != nullptr ? text : g_strdup("");
}

/* Without seconds, as Explorer's lists. */
char *xd_keys_date(gint64 seconds) { return format_time(seconds, "%x %H:%M"); }

char *xd_keys_day(gint64 seconds) { return format_time(seconds, "%x"); }

/* The kinds Seahorse names, and the ones our own programs store. */
static const struct {
    const char *schema, *kind;
} kinds[] = {
    {"org.gnome.keyring.NetworkPassword", "Network password"},
    {"org.gnome.keyring.Note", "Stored note"},
    {"org.gnome.keyring.ChainedKeyring", "Keyring password"},
    {"org.gnome.keyring.EncryptionKey", "Encryption key password"},
    {"org.gnome.keyring.PkStorage", "Key storage password"},
    {"org.gnome.GVfs.Luks.Password", "Encrypted volume password"},
    {"org.freedesktop.NetworkManager.Connection", "Network connection secret"},
    {"org.gnome.Evolution.Data.Source", "E-mail account password"},
    {"org.freedesktop.Secret.Generic", "Password"},
};

const char *xd_secret_kind(const char *schema) {
    if (schema != nullptr)
        for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++)
            if (strcmp(schema, kinds[i].schema) == 0)
                return kinds[i].kind;
    return "Password";
}

const char *xd_secret_display_name(const char *label, const char *schema) {
    return label != nullptr && label[0] != '\0' ? label : xd_secret_kind(schema);
}

bool xd_ssh_parse_fingerprint(const char *line, int *bits, char **fingerprint, char **comment,
                              char **type) {
    /* BITS FINGERPRINT [COMMENT...] (TYPE) */
    char *text = g_strstrip(g_strdup(line));
    bool ok = false;
    char *sp1 = strchr(text, ' ');
    char *open = strrchr(text, '(');
    size_t len = strlen(text);
    if (sp1 != nullptr && open != nullptr && open > sp1 && len > 0 && text[len - 1] == ')') {
        *sp1 = '\0';
        char *end = nullptr;
        errno = 0;
        long bit_count = strtol(text, &end, 10);
        char *fp = sp1 + 1;
        char *sp2 = strchr(fp, ' ');
        if (end != text && *end == '\0' && errno == 0 && bit_count > 0 && bit_count < 65536 &&
            sp2 != nullptr && sp2 < open) {
            *sp2 = '\0';
            text[len - 1] = '\0';
            open[0] = '\0';
            if (bits != nullptr)
                *bits = (int)bit_count;
            if (fingerprint != nullptr)
                *fingerprint = g_strdup(fp);
            if (type != nullptr)
                *type = g_strdup(open + 1);
            if (comment != nullptr)
                *comment = g_strstrip(g_strdup(sp2 + 1));
            ok = true;
        }
    }
    g_free(text);
    return ok;
}

static bool key_exists(const char *dir, const char *name) {
    char *path = g_build_filename(dir, name, nullptr);
    char *pub = g_strconcat(path, ".pub", nullptr);
    bool exists = g_file_test(path, G_FILE_TEST_EXISTS) || g_file_test(pub, G_FILE_TEST_EXISTS);
    g_free(pub);
    g_free(path);
    return exists;
}

char *xd_ssh_new_key_name(const char *dir, const char *type) {
    char *name = g_strdup_printf("id_%s", type);
    for (int i = 2; key_exists(dir, name) && i < 1000; i++) {
        g_free(name);
        name = g_strdup_printf("id_%s_%d", type, i);
    }
    return name;
}

bool xd_ssh_valid_key_name(const char *name) {
    static const char *const reserved[] = {"config", "known_hosts", "known_hosts.old",
                                           "authorized_keys", "authorized_keys2", "environment",
                                           "rc"};
    if (name == nullptr || name[0] == '\0' || name[0] == '.' || strchr(name, '/') != nullptr ||
        g_str_has_suffix(name, ".pub") || strlen(name) > 200)
        return false;
    for (size_t i = 0; i < G_N_ELEMENTS(reserved); i++)
        if (strcmp(name, reserved[i]) == 0)
            return false;
    return true;
}

int xd_keys_askpass_main(void) {
    /* A byte at a time: what's left in the pipe is the next askpass's. */
    GString *line = g_string_new(nullptr);
    bool got = false;
    char character;
    for (;;) {
        ssize_t bytes_read = read(STDIN_FILENO, &character, 1);
        if (bytes_read < 0 && errno == EINTR)
            continue;
        if (bytes_read <= 0)
            break;
        got = true;
        if (character == '\n')
            break;
        g_string_append_c(line, character);
    }
    int status = 1;
    if (got) {
        g_string_append_c(line, '\n');
        status = fwrite(line->str, 1, line->len, stdout) == line->len && fflush(stdout) == 0
                     ? 0
                     : 1;
    }
    /* It held a passphrase. */
    memset(line->str, 0, line->len);
    g_string_free(line, TRUE);
    return status;
}

char *xd_gpg_user_id(const char *name, const char *email, const char *comment) {
    GString *string = g_string_new(nullptr);
    if (name != nullptr && name[0] != '\0')
        g_string_append(string, name);
    if (comment != nullptr && comment[0] != '\0')
        g_string_append_printf(string, "%s(%s)", string->len > 0 ? " " : "", comment);
    if (email != nullptr && email[0] != '\0')
        g_string_append_printf(string, "%s<%s>", string->len > 0 ? " " : "", email);
    return g_string_free(string, FALSE);
}

const char *xd_gpg_validity_text(int validity) {
    /* GPGME_VALIDITY_UNKNOWN, UNDEFINED, NEVER, MARGINAL, FULL, ULTIMATE */
    switch (validity) {
    case 2:
        return "Never";
    case 3:
        return "Marginal";
    case 4:
        return "Full";
    case 5:
        return "Ultimate";
    default:
        return "Unknown";
    }
}
