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
