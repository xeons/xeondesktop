/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "../core/common.h"
#include "updlib.h"

/* ---- importance --------------------------------------------------------------- */

static xd_update_rank rank_of_info(PkInfoEnum info) {
    static const xd_update_rank ranks[PK_INFO_ENUM_LAST] = {
        [PK_INFO_ENUM_LOW] = XD_RANK_OPTIONAL,
        [PK_INFO_ENUM_ENHANCEMENT] = XD_RANK_ENHANCEMENT,
        [PK_INFO_ENUM_NORMAL] = XD_RANK_RECOMMENDED,
        [PK_INFO_ENUM_BUGFIX] = XD_RANK_BUGFIX,
        [PK_INFO_ENUM_IMPORTANT] = XD_RANK_IMPORTANT,
        [PK_INFO_ENUM_SECURITY] = XD_RANK_SECURITY,
        [PK_INFO_ENUM_CRITICAL] = XD_RANK_CRITICAL,
    };
    return (unsigned)info < XD_ARRAY_LEN(ranks) ? ranks[info] : XD_RANK_UNKNOWN;
}

xd_update_rank xd_update_rank_of_package(PkInfoEnum info, PkInfoEnum severity) {
    /* The severity, where the backend gives one, says how urgent; the
     * info says what kind (a security fix stays one even if low). */
    xd_update_rank info_rank = rank_of_info(info), severity_rank = rank_of_info(severity);
    return info_rank > severity_rank ? info_rank : severity_rank;
}

xd_update_rank xd_update_rank_of_urgency(unsigned urgency) {
    static const xd_update_rank ranks[] = {XD_RANK_UNKNOWN, XD_RANK_OPTIONAL,
                                           XD_RANK_RECOMMENDED, XD_RANK_IMPORTANT,
                                           XD_RANK_CRITICAL};
    return urgency < XD_ARRAY_LEN(ranks) ? ranks[urgency] : XD_RANK_UNKNOWN;
}

const char *xd_update_rank_name(xd_update_rank rank) {
    static const char *const names[] = {
        [XD_RANK_UNKNOWN] = "",
        [XD_RANK_OPTIONAL] = "Optional",
        [XD_RANK_ENHANCEMENT] = "Enhancement",
        [XD_RANK_RECOMMENDED] = "Recommended",
        [XD_RANK_BUGFIX] = "Bug fix",
        [XD_RANK_IMPORTANT] = "Important",
        [XD_RANK_SECURITY] = "Security",
        [XD_RANK_CRITICAL] = "Critical",
    };
    return (unsigned)rank < XD_ARRAY_LEN(names) ? names[rank] : "";
}

bool xd_update_rank_important(xd_update_rank rank) { return rank >= XD_RANK_IMPORTANT; }

const char *xd_update_restart_text(PkRestartEnum restart) {
    if (restart == PK_RESTART_ENUM_SYSTEM || restart == PK_RESTART_ENUM_SECURITY_SYSTEM)
        return "You must restart your computer after installing this update.";
    if (restart == PK_RESTART_ENUM_SESSION || restart == PK_RESTART_ENUM_SECURITY_SESSION)
        return "You must log off and log on again after installing this update.";
    if (restart == PK_RESTART_ENUM_APPLICATION)
        return "Programs that use it must be closed and opened again after this update.";
    return nullptr;
}

bool xd_update_restart_system(PkRestartEnum restart) {
    return restart == PK_RESTART_ENUM_SYSTEM || restart == PK_RESTART_ENUM_SECURITY_SYSTEM;
}

/* ---- fwupd: release notes ------------------------------------------------------- */

typedef struct markup {
    GString *out;
    int sep;    /* before the next text: 1 a line break, 2 a blank line */
    bool fresh; /* a block (paragraph, list item) began */
    bool item;  /* ...and it's a list item: a bullet first */
    bool space; /* white space seen since the last character */
} markup;

static void md_start(GMarkupParseContext *context, const char *name, const char **attr_names,
                     const char **attr_values, gpointer data, GError **error) {
    (void)context;
    (void)attr_names;
    (void)attr_values;
    (void)error;
    markup *converter = data;
    if (strcmp(name, "p") == 0 || strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
        converter->fresh = true;
        converter->sep = 2;
    } else if (strcmp(name, "li") == 0) {
        converter->fresh = true;
        converter->item = true;
        converter->sep = XD_MAX(converter->sep, 1);
    }
}

static void md_end(GMarkupParseContext *context, const char *name, gpointer data, GError **error) {
    (void)context;
    (void)error;
    markup *converter = data;
    if (strcmp(name, "p") == 0 || strcmp(name, "li") == 0 || strcmp(name, "ul") == 0 ||
        strcmp(name, "ol") == 0)
        converter->fresh = true;
}

static void md_text(GMarkupParseContext *context, const char *text, gsize len, gpointer data,
                    GError **error) {
    (void)context;
    (void)error;
    markup *converter = data;
    for (gsize i = 0; i < len; i++) {
        char ch = text[i];
        if (g_ascii_isspace(ch)) {
            converter->space = true;
            continue;
        }
        if (converter->fresh) {
            if (converter->out->len > 0)
                g_string_append(converter->out, converter->sep >= 2 ? "\n\n" : "\n");
            if (converter->item)
                g_string_append(converter->out, "• ");
            converter->fresh = converter->item = converter->space = false;
            converter->sep = 0;
        } else if (converter->space) {
            g_string_append_c(converter->out, ' ');
            converter->space = false;
        }
        g_string_append_c(converter->out, ch);
    }
}

char *xd_fwupd_markup_to_text(const char *text) {
    if (text == nullptr)
        return nullptr;
    static const GMarkupParser parser = {
        .start_element = md_start,
        .end_element = md_end,
        .text = md_text,
    };
    markup converter = {.out = g_string_new(nullptr), .fresh = true};
    /* Release notes are a run of blocks; one root makes them a document. */
    char *doc = g_strconcat("<description>", text, "</description>", nullptr);
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, 0, &converter, nullptr);
    bool ok = g_markup_parse_context_parse(ctx, doc, -1, nullptr) &&
              g_markup_parse_context_end_parse(ctx, nullptr);
    g_markup_parse_context_free(ctx);
    g_free(doc);
    if (!ok && converter.out->len == 0) {
        g_string_free(converter.out, TRUE);
        return nullptr;
    }
    return g_string_free(converter.out, FALSE);
}

/* ---- fwupd: downloads ------------------------------------------------------------- */

static bool all_hex(const char *text, size_t length) {
    return strlen(text) == length && strspn(text, "0123456789abcdefABCDEF") == length;
}

bool xd_fwupd_checksum_ok(const char *checksums, GBytes *data) {
    if (checksums == nullptr || data == nullptr)
        return false;
    char **sums = g_strsplit(checksums, ",", -1);
    GChecksumType type = G_CHECKSUM_SHA1;
    size_t hex = 40;
    bool any = false;
    for (char **sum = sums; *sum != nullptr; sum++) {
        g_strstrip(*sum);
        if (all_hex(*sum, 64)) {
            type = G_CHECKSUM_SHA256;
            hex = 64;
        }
        any = any || all_hex(*sum, 40) || all_hex(*sum, 64);
    }
    bool ok = false;
    if (any) {
        char *got = g_compute_checksum_for_bytes(type, data);
        for (char **sum = sums; *sum != nullptr && !ok; sum++)
            ok = all_hex(*sum, hex) && g_ascii_strcasecmp(*sum, got) == 0;
        g_free(got);
    }
    g_strfreev(sums);
    return ok;
}

static bool usable(const char *uri) {
    return uri != nullptr &&
           (g_str_has_prefix(uri, "https://") || g_str_has_prefix(uri, "file:///"));
}

const char *xd_fwupd_pick_location(const char *const *locations, const char *uri) {
    for (const char *const *location = locations; location != nullptr && *location != nullptr;
         location++) {
        if (usable(*location))
            return *location;
    }
    return usable(uri) ? uri : nullptr;
}

/* ---- fwupd: Host Security ID ------------------------------------------------------- */

int xd_fwupd_hsi_level(const char *hsi, bool *runtime_issue) {
    *runtime_issue = false;
    if (hsi == nullptr || !g_str_has_prefix(hsi, "HSI:"))
        return -1;
    const char *cursor = hsi + strlen("HSI:");
    if (!g_ascii_isdigit(*cursor))
        return -1;
    int level = 0;
    while (g_ascii_isdigit(*cursor) && level < 100)
        level = level * 10 + (*cursor++ - '0');
    *runtime_issue = *cursor == '!';
    return level;
}

const char *xd_fwupd_hsi_level_text(int level) {
    switch (level) {
    case 0:
        return "This computer lacks protections that its firmware should give it: even the "
               "critical ones aren't all in place.";
    case 1:
        return "The critical protections are in place: the firmware can't simply be changed "
               "by a program.";
    case 2:
        return "The critical and the important protections are in place.";
    case 3:
        return "The critical, important and theoretical protections are in place: even attacks "
               "that are hard to carry out are guarded against.";
    case 4:
    case 5:
        return "Every protection checked for is in place.";
    default:
        return "The firmware's security level couldn't be found out.";
    }
}

const char *xd_fwupd_hsi_result_text(unsigned result) {
    static const char *const names[] = {
        "",            "Enabled",   "Not enabled", "Valid",     "Not valid",
        "Locked",      "Not locked", "Encrypted",  "Not encrypted", "Tainted",
        "Not tainted", "Found",     "Not found",   "Supported", "Not supported",
    };
    return result < XD_ARRAY_LEN(names) ? names[result] : "";
}

const char *xd_fwupd_attr_title(const char *appstream_id, const char *title, const char *name) {
    if (title != nullptr && title[0] != '\0')
        return title;
    static const char *const known[][2] = {
        {"org.fwupd.hsi.Uefi.SecureBoot", "UEFI Secure Boot"},
        {"org.fwupd.hsi.Uefi.Pk", "UEFI platform key"},
        {"org.fwupd.hsi.Tpm.Version20", "TPM v2.0"},
        {"org.fwupd.hsi.Tpm.ReconstructionPcr0", "TPM PCR0 reconstruction"},
        {"org.fwupd.hsi.Tpm.EmptyPcr", "TPM empty PCRs"},
        {"org.fwupd.hsi.Iommu", "IOMMU protection"},
        {"org.fwupd.hsi.Spi.Bioswe", "BIOS write enable"},
        {"org.fwupd.hsi.Spi.Ble", "BIOS lock enable"},
        {"org.fwupd.hsi.Spi.SmmBwp", "SPI write protection"},
        {"org.fwupd.hsi.Spi.Descriptor", "SPI descriptor"},
        {"org.fwupd.hsi.IntelBootguard.Enabled", "Intel BootGuard"},
        {"org.fwupd.hsi.IntelCet.Enabled", "Intel CET"},
        {"org.fwupd.hsi.EncryptedRam", "Encrypted RAM"},
        {"org.fwupd.hsi.Kernel.Tainted", "Linux kernel tainted"},
        {"org.fwupd.hsi.Kernel.Lockdown", "Linux kernel lockdown"},
        {"org.fwupd.hsi.Kernel.Swap", "Linux swap"},
        {"org.fwupd.hsi.SuspendToRam", "Suspend to RAM"},
        {"org.fwupd.hsi.SuspendToIdle", "Suspend to idle"},
        {"org.fwupd.hsi.Mei.ManufacturingMode", "Intel Management Engine manufacturing mode"},
        {"org.fwupd.hsi.Mei.OverrideStrap", "Intel Management Engine override"},
        {"org.fwupd.hsi.Mei.Version", "Intel Management Engine version"},
        {"org.fwupd.hsi.PlatformDebugLocked", "Platform debugging"},
        {"org.fwupd.hsi.Fwupd.Plugins", "fwupd plugins"},
    };
    for (size_t i = 0; appstream_id != nullptr && i < XD_ARRAY_LEN(known); i++) {
        if (strcmp(appstream_id, known[i][0]) == 0)
            return known[i][1];
    }
    if (name != nullptr && name[0] != '\0')
        return name;
    return appstream_id != nullptr ? appstream_id : "";
}

/* ---- why an install failed ------------------------------------------------------ */

bool xd_update_get_failure(const GError *error, PkResults *results, PkErrorEnum *code,
                           char **details) {
    PkError *result_error = results != nullptr ? pk_results_get_error_code(results) : nullptr;
    if (error == nullptr && result_error == nullptr)
        return false;
    if (error != nullptr && error->domain == PK_CLIENT_ERROR && error->code >= 0xff &&
        error->code - 0xff < PK_ERROR_ENUM_LAST) {
        *code = (PkErrorEnum)(error->code - 0xff);
        *details = g_strdup(error->message);
    } else if (result_error != nullptr) {
        *code = pk_error_get_code(result_error);
        *details = g_strdup(pk_error_get_details(result_error));
    } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        *code = PK_ERROR_ENUM_TRANSACTION_CANCELLED;
        *details = g_strdup(error->message);
    } else {
        /* PackageKit didn't answer, or the D-Bus call itself failed. */
        *code = PK_ERROR_ENUM_INTERNAL_ERROR;
        *details = g_strdup(error->message);
    }
    if (*details == nullptr)
        *details = g_strdup("");
    g_clear_object(&result_error);
    return true;
}

static const char *const error_summaries[PK_ERROR_ENUM_LAST] = {
    [PK_ERROR_ENUM_FILE_CONFLICTS] = "Some files in the updates already belong to other "
                                     "packages, or are already on the disk, so nothing was "
                                     "installed.",
    [PK_ERROR_ENUM_DEP_RESOLUTION_FAILED] = "The updates need packages that conflict with each "
                                            "other or can't be found, so nothing was installed.",
    [PK_ERROR_ENUM_PACKAGE_CONFLICTS] = "The updates need packages that conflict with each other "
                                        "or can't be found, so nothing was installed.",
    [PK_ERROR_ENUM_CANNOT_GET_LOCK] = "Another program is installing or removing software. "
                                      "Close it and try again.",
    [PK_ERROR_ENUM_LOCK_REQUIRED] = "Another program is installing or removing software. "
                                    "Close it and try again.",
    [PK_ERROR_ENUM_GPG_FAILURE] = "Some updates couldn't be verified: their signatures are "
                                  "missing, damaged or made with keys this computer doesn't "
                                  "know. Install the update to your distribution's keyring "
                                  "package on its own first (on Arch Linux, archlinux-keyring), "
                                  "then try again.",
    [PK_ERROR_ENUM_PACKAGE_DOWNLOAD_FAILED] = "Some updates could not be downloaded. Check your "
                                              "Internet connection, then try again.",
    [PK_ERROR_ENUM_CANNOT_FETCH_SOURCES] = "Some updates could not be downloaded. Check your "
                                           "Internet connection, then try again.",
    [PK_ERROR_ENUM_NO_NETWORK] = "The computer isn't connected to the Internet, so the updates "
                                 "could not be downloaded. Connect it, then try again.",
    [PK_ERROR_ENUM_REPO_NOT_AVAILABLE] = "The servers the updates come from couldn't be "
                                         "reached. Try again later.",
    [PK_ERROR_ENUM_NO_MORE_MIRRORS_TO_TRY] = "The servers the updates come from couldn't be "
                                             "reached. Try again later.",
    [PK_ERROR_ENUM_NO_SPACE_ON_DEVICE] = "There isn't enough free disk space for the updates. "
                                         "Free some space (Disk Cleanup can help), then try "
                                         "again.",
    [PK_ERROR_ENUM_TRANSACTION_ERROR] = "An error occurred while the updates were being "
                                        "installed, and some of them may not have been. Click "
                                        "Details to see what the package manager reported.",
    [PK_ERROR_ENUM_NOT_AUTHORIZED] = "You don't have permission to install updates on this "
                                     "computer.",
    [PK_ERROR_ENUM_PACKAGE_CORRUPT] = "Some of the downloaded updates are damaged. Try again: "
                                      "they'll be downloaded again.",
    [PK_ERROR_ENUM_INVALID_PACKAGE_FILE] = "Some of the downloaded updates are damaged. Try "
                                           "again: they'll be downloaded again.",
    [PK_ERROR_ENUM_UPDATE_FAILED_DUE_TO_RUNNING_PROCESS] = "A program that's running must be "
                                                           "closed before the updates can be "
                                                           "installed. Close your programs, then "
                                                           "try again.",
    [PK_ERROR_ENUM_INTERNAL_ERROR] = "The service that installs software (PackageKit) reported "
                                     "an error or didn't answer.",
};

char *xd_update_get_error_summary(PkErrorEnum code, bool only_some) {
    if (code == PK_ERROR_ENUM_TRANSACTION_CANCELLED)
        return nullptr;
    /* Signatures and keys share one answer. */
    PkErrorEnum known = code;
    if (code == PK_ERROR_ENUM_BAD_GPG_SIGNATURE || code == PK_ERROR_ENUM_MISSING_GPG_SIGNATURE ||
        code == PK_ERROR_ENUM_CANNOT_INSTALL_REPO_UNSIGNED ||
        code == PK_ERROR_ENUM_CANNOT_UPDATE_REPO_UNSIGNED)
        known = PK_ERROR_ENUM_GPG_FAILURE;
    const char *text = (unsigned)known < XD_ARRAY_LEN(error_summaries) ? error_summaries[known]
                                                                        : nullptr;
    if (text == nullptr)
        return g_strdup_printf("The package manager reported an error (%s).",
                               pk_error_enum_to_string(code));
    /* Arch and its kind upgrade the whole system or nothing: a partial
     * selection is the likely cause. */
    bool dependencies =
        known == PK_ERROR_ENUM_DEP_RESOLUTION_FAILED || known == PK_ERROR_ENUM_PACKAGE_CONFLICTS;
    if (dependencies && only_some)
        return g_strdup_printf("%s Not all of the updates were selected; installing all of them "
                               "may help.",
                               text);
    return g_strdup(text);
}

/* An item as backends list them: a name or path, maybe "<-" or "<->" and
 * another (a dependency, a conflict), maybe a reason or file in
 * parentheses. */
static GRegex *new_item_regex(void) {
    return g_regex_new("^([^\\s,()]+)(?: (<->?) ([^\\s,()]+))?(?: \\(([^()]*)\\))?$", 0, 0,
                       nullptr);
}

/* A line as alpm writes its lists ("a (x), b (y): conflicting files"): its
 * items, and what went wrong after them (or nullptr). False when the line
 * isn't such a list. */
static bool split_list_line(GRegex *regex, const char *line, char ***items, char **message) {
    const char *colon = g_strrstr(line, ": ");
    char *list = nullptr;
    *message = nullptr;
    if (colon != nullptr && strstr(colon, ", ") == nullptr) {
        list = g_strndup(line, (gsize)(colon - line));
        *message = g_strdup(colon + 2);
    } else {
        list = g_strdup(line);
    }
    char **parts = g_strsplit(list, ", ", -1);
    unsigned count = g_strv_length(parts);
    bool ok = count >= 2 || (count == 1 && (strstr(parts[0], " (") != nullptr ||
                                            strstr(parts[0], " <-") != nullptr));
    for (unsigned i = 0; ok && i < count; i++)
        ok = g_regex_match(regex, parts[i], 0, nullptr);
    g_free(list);
    if (!ok) {
        g_strfreev(parts);
        g_clear_pointer(message, g_free);
        return false;
    }
    *items = parts;
    return true;
}

static char **split_lines(const char *details) {
    char **lines = g_strsplit(details != nullptr ? details : "", "\n", -1);
    for (char **line = lines; *line != nullptr; line++)
        g_strstrip(*line);
    return lines;
}

static void append_line(GString *out, const char *line) {
    if (out->len > 0)
        g_string_append_c(out, '\n');
    g_string_append(out, line);
}

char *xd_update_format_error_details(const char *details) {
    GRegex *regex = new_item_regex();
    char **lines = split_lines(details);
    GString *out = g_string_new(nullptr);
    for (char **line = lines; *line != nullptr; line++) {
        if ((*line)[0] == '\0')
            continue;
        char **items = nullptr, *message = nullptr;
        if (!split_list_line(regex, *line, &items, &message)) {
            append_line(out, *line);
            continue;
        }
        if (message != nullptr) {
            append_line(out, message);
            g_string_append_c(out, ':');
        }
        for (char **item = items; *item != nullptr; item++)
            append_line(out, *item);
        g_strfreev(items);
        g_free(message);
    }
    g_strfreev(lines);
    g_regex_unref(regex);
    return g_string_free(out, FALSE);
}

static char *describe_item(GRegex *regex, const char *item) {
    GMatchInfo *match = nullptr;
    if (!g_regex_match(regex, item, 0, &match)) {
        g_match_info_free(match);
        return g_strdup(item);
    }
    char *first = g_match_info_fetch(match, 1), *arrow = g_match_info_fetch(match, 2);
    char *second = g_match_info_fetch(match, 3), *aside = g_match_info_fetch(match, 4);
    g_match_info_free(match);
    bool path = aside != nullptr && aside[0] == '/';
    char *text;
    if (g_strcmp0(arrow, "<-") == 0) /* "dependency <- package" */
        text = g_strdup_printf("%s needs %s", second, first);
    else if (g_strcmp0(arrow, "<->") == 0 && path) /* a file both packages have */
        text = g_strdup_printf("%s (%s, %s)", aside, first, second);
    else if (g_strcmp0(arrow, "<->") == 0)
        text = aside != nullptr && aside[0] != '\0'
                   ? g_strdup_printf("%s conflicts with %s (%s)", first, second, aside)
                   : g_strdup_printf("%s conflicts with %s", first, second);
    else if (path) /* a package's file already on the disk */
        text = g_strdup_printf("%s (%s)", aside, first);
    else
        text = g_strdup(item);
    g_free(first);
    g_free(arrow);
    g_free(second);
    g_free(aside);
    return text;
}

GPtrArray *xd_update_get_error_items(const char *details) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GRegex *regex = new_item_regex();
    char **lines = split_lines(details);
    for (char **line = lines; *line != nullptr; line++) {
        char **items = nullptr, *message = nullptr;
        if ((*line)[0] == '\0' || !split_list_line(regex, *line, &items, &message))
            continue;
        for (char **item = items; *item != nullptr; item++)
            g_ptr_array_add(out, describe_item(regex, *item));
        g_strfreev(items);
        g_free(message);
    }
    g_strfreev(lines);
    g_regex_unref(regex);
    return out;
}

char *xd_update_list_error_items(const GPtrArray *items, unsigned max) {
    GString *out = g_string_new(nullptr);
    for (unsigned i = 0; i < items->len && i < max; i++) {
        if (i > 0)
            g_string_append_c(out, '\n');
        g_string_append_printf(out, "• %s", (const char *)g_ptr_array_index(items, i));
    }
    if (items->len > max)
        g_string_append_printf(out, "\nand %u more", items->len - max);
    return g_string_free(out, FALSE);
}
