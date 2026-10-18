/* SPDX-License-Identifier: MIT
 * Encrypted (LUKS) volumes' names, items and settings. See luks.h.
 */
/* explicit_bzero, in Disk Management too (built without _GNU_SOURCE). */
#ifndef _GNU_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "luks.h"

#include <string.h>

char *xd_luks_secret_label(const char *volume_name) {
    /* gvfs's wording, so the keyring shows one kind of entry. */
    return g_strdup_printf("Encryption passphrase for %s",
                           volume_name != nullptr ? volume_name : "");
}

char *xd_luks_item_uri(const char *uuid) {
    char *escaped = g_uri_escape_string(uuid != nullptr ? uuid : "", nullptr, FALSE);
    char *uri = g_strconcat(XD_LUKS_ITEM_PREFIX, escaped, nullptr);
    g_free(escaped);
    return uri;
}

char *xd_luks_item_uuid(const char *uri) {
    if (uri == nullptr || !g_str_has_prefix(uri, XD_LUKS_ITEM_PREFIX))
        return nullptr;
    const char *rest = uri + strlen(XD_LUKS_ITEM_PREFIX);
    char *uuid = g_uri_unescape_string(rest, "/");
    if (uuid != nullptr && uuid[0] == '\0')
        g_clear_pointer(&uuid, g_free);
    return uuid;
}

char *xd_luks_volume_name(const char *inner_label, const char *luks_label, const char *size) {
    if (inner_label != nullptr && inner_label[0] != '\0')
        return g_strdup(inner_label);
    if (luks_label != nullptr && luks_label[0] != '\0')
        return g_strdup(luks_label);
    if (size != nullptr && size[0] != '\0')
        return g_strdup_printf("%s Encrypted Volume", size);
    return g_strdup("Encrypted Volume");
}

char *xd_luks_version_label(const char *id_version) {
    if (id_version != nullptr && (strcmp(id_version, "1") == 0 || strcmp(id_version, "2") == 0))
        return g_strdup_printf("LUKS%s", id_version);
    return g_strdup("LUKS");
}

const char *xd_luks_passphrase_problem(const char *passphrase, const char *confirm) {
    if (passphrase == nullptr || passphrase[0] == '\0')
        return "Type a passphrase. An encrypted volume can't be left without one.";
    if (confirm == nullptr || strcmp(passphrase, confirm) != 0)
        return "The passphrases you typed do not match. Type the new passphrase in both text "
               "boxes.";
    return nullptr;
}

gboolean xd_luks_wrong_passphrase(const char *dbus_error, const char *message) {
    if (g_strcmp0(dbus_error, "org.freedesktop.UDisks2.Error.Failed") != 0 || message == nullptr)
        return FALSE;
    /* libblockdev passes on cryptsetup's errno or words: EPERM for a key
     * that opens no key slot, "No key available with this passphrase"
     * from cryptsetup itself, "Incorrect passphrase" from libblockdev 3. */
    static const char *const signs[] = {"Operation not permitted", "No key available",
                                        "ncorrect passphrase", "wrong passphrase",
                                        "Invalid passphrase"};
    for (size_t i = 0; i < G_N_ELEMENTS(signs); i++)
        if (strstr(message, signs[i]) != nullptr)
            return TRUE;
    return FALSE;
}

void xd_luks_passphrase_free(char *passphrase) {
    if (passphrase == nullptr)
        return;
    explicit_bzero(passphrase, strlen(passphrase));
    g_free(passphrase);
}

char *xd_crypttab_name(const char *luks_uuid) {
    return g_strdup_printf("luks-%s", luks_uuid != nullptr ? luks_uuid : "");
}

char *xd_crypttab_key_path(const char *name) {
    return g_strdup_printf("/etc/luks-keys/%s", name != nullptr ? name : "");
}
