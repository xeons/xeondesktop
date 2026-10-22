/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "admin.h"

/* ---- Services ---------------------------------------------------------------------- */

char *xd_unit_startup_label(const char *unit_file_state) {
    if (unit_file_state == nullptr || unit_file_state[0] == '\0')
        return g_strdup("");
    /* "enabled-runtime" -> "Enabled (runtime)" */
    const char *dash = g_str_has_suffix(unit_file_state, "-runtime")
                           ? unit_file_state + strlen(unit_file_state) - strlen("-runtime")
                           : nullptr;
    char *base = dash != nullptr ? g_strndup(unit_file_state, (gsize)(dash - unit_file_state))
                                 : g_strdup(unit_file_state);
    base[0] = g_ascii_toupper(base[0]);
    char *out = dash != nullptr ? g_strdup_printf("%s (runtime)", base) : g_strdup(base);
    g_free(base);
    return out;
}

xd_unit_startup xd_unit_startup_of(const char *unit_file_state) {
    if (g_strcmp0(unit_file_state, "enabled") == 0)
        return XD_STARTUP_ENABLED;
    if (g_strcmp0(unit_file_state, "disabled") == 0)
        return XD_STARTUP_DISABLED;
    if (g_strcmp0(unit_file_state, "masked") == 0)
        return XD_STARTUP_MASKED;
    return XD_STARTUP_OTHER;
}

const char *xd_unit_status_label(const char *active_state) {
    static const struct {
        const char *state, *label;
    } labels[] = {
        {"active", "Started"},      {"reloading", "Started"}, {"refreshing", "Started"},
        {"activating", "Starting"}, {"deactivating", "Stopping"}, {"failed", "Failed"},
        {"maintenance", "Maintenance"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(labels); i++)
        if (g_strcmp0(active_state, labels[i].state) == 0)
            return labels[i].label;
    return "";
}

bool xd_unit_is_started(const char *active_state) {
    return g_strcmp0(active_state, "active") == 0 || g_strcmp0(active_state, "reloading") == 0 ||
           g_strcmp0(active_state, "refreshing") == 0 ||
           g_strcmp0(active_state, "activating") == 0;
}

char *xd_unit_display_name(const char *id) {
    if (id == nullptr)
        return g_strdup("");
    const char *dot = strrchr(id, '.');
    /* Only a unit type's suffix: "user@1000.service" keeps its "@1000". */
    return dot != nullptr && dot != id ? g_strndup(id, (gsize)(dot - id)) : g_strdup(id);
}

const char *xd_unit_logon_label(const char *user, bool user_manager, const char *me) {
    if (user != nullptr && user[0] != '\0')
        return user;
    return user_manager ? (me != nullptr ? me : "") : "Local System";
}

char *xd_unit_command_line(const char *const *argv) {
    GString *command_line = g_string_new(nullptr);
    for (size_t i = 0; argv != nullptr && argv[i] != nullptr; i++) {
        if (i > 0)
            g_string_append_c(command_line, ' ');
        const char *argument = argv[i];
        bool plain = argument[0] != '\0' && strpbrk(argument, " \t\n'\"\\$`") == nullptr;
        if (plain) {
            g_string_append(command_line, argument);
        } else {
            char *quoted = g_shell_quote(argument);
            g_string_append(command_line, quoted);
            g_free(quoted);
        }
    }
    return g_string_free(command_line, FALSE);
}

/* ---- Event Viewer -------------------------------------------------------------------- */

const char *const xd_security_identifiers[] = {
    "sudo",     "su",       "polkitd",  "polkit-agent-helper-1", "pkexec", "sshd",
    "sshd-session", "login", "unix_chkpwd", "passwd",   "chpasswd", "useradd",
    "usermod",  "userdel",  "groupadd", "groupdel", "gpasswd", "chage",  "systemd-homed",
    "audit",    "doas",     "run0",     nullptr,
};

xd_event_log xd_event_log_of(int facility, const char *identifier, const char *transport,
                             bool user_unit, long uid) {
    /* LOG_AUTH (4), LOG_AUTHPRIV (10) and LOG_AUDIT (13) */
    if (facility == 4 || facility == 10 || facility == 13 || g_strcmp0(transport, "audit") == 0)
        return XD_LOG_SECURITY;
    if (identifier != nullptr && g_strv_contains(xd_security_identifiers, identifier))
        return XD_LOG_SECURITY;
    if (g_strcmp0(transport, "kernel") == 0)
        return XD_LOG_SYSTEM;
    /* Regular users' uids start at 1000 (login.defs' and systemd's
     * default); 65534 is nobody. */
    if (user_unit || (uid >= 1000 && uid != 65534))
        return XD_LOG_APPLICATION;
    return XD_LOG_SYSTEM;
}

xd_event_type xd_event_type_of(int priority, xd_event_log log) {
    if (priority < 0 || priority > 7)
        priority = 6; /* LOG_INFO */
    if (log == XD_LOG_SECURITY)
        return priority <= 4 ? XD_EVENT_AUDIT_FAILURE : XD_EVENT_AUDIT_SUCCESS;
    if (priority <= 3)
        return XD_EVENT_ERROR;
    return priority == 4 ? XD_EVENT_WARNING : XD_EVENT_INFORMATION;
}

const char *xd_event_type_label(xd_event_type event_type) {
    static const char *const labels[XD_EVENT_TYPE_COUNT] = {
        [XD_EVENT_ERROR] = "Error",
        [XD_EVENT_WARNING] = "Warning",
        [XD_EVENT_INFORMATION] = "Information",
        [XD_EVENT_AUDIT_SUCCESS] = "Success Audit",
        [XD_EVENT_AUDIT_FAILURE] = "Failure Audit",
    };
    return event_type < XD_EVENT_TYPE_COUNT ? labels[event_type] : "";
}

const char *xd_event_type_icon(xd_event_type event_type) {
    /* XP's key for a success audit, padlock for a failure. */
    static const char *const icons[XD_EVENT_TYPE_COUNT] = {
        [XD_EVENT_ERROR] = "dialog-error",
        [XD_EVENT_WARNING] = "dialog-warning",
        [XD_EVENT_INFORMATION] = "dialog-information",
        [XD_EVENT_AUDIT_SUCCESS] = "dialog-password",
        [XD_EVENT_AUDIT_FAILURE] = "system-lock-screen",
    };
    return event_type < XD_EVENT_TYPE_COUNT ? icons[event_type] : "dialog-information";
}

/* ---- Disk Management ------------------------------------------------------------------ */

char *xd_disk_size_text(guint64 bytes) {
    static constexpr guint64 KB = 1024;
    static constexpr guint64 MB = KB * 1024;
    static constexpr guint64 GB = MB * 1024;
    static constexpr guint64 TB = GB * 1024;
    /* Two decimals, cut rather than rounded, as Windows shows them. */
    if (bytes >= TB)
        return g_strdup_printf("%" G_GUINT64_FORMAT ".%02" G_GUINT64_FORMAT " TB", bytes / TB,
                               bytes % TB / (TB / 100));
    if (bytes >= GB)
        return g_strdup_printf("%" G_GUINT64_FORMAT ".%02" G_GUINT64_FORMAT " GB", bytes / GB,
                               bytes % GB * 100 / GB);
    if (bytes >= MB)
        return g_strdup_printf("%" G_GUINT64_FORMAT " MB", (bytes + MB / 2) / MB);
    return g_strdup_printf("%" G_GUINT64_FORMAT " KB", (bytes + KB / 2) / KB);
}

char *xd_disk_fs_label(const char *id_type, const char *id_version) {
    if (id_type == nullptr || id_type[0] == '\0')
        return g_strdup("");
    static const struct {
        const char *type, *label;
    } names[] = {
        {"ntfs", "NTFS"},  {"exfat", "exFAT"}, {"iso9660", "CDFS"},  {"udf", "UDF"},
        {"swap", "Swap"},  {"crypto_LUKS", "LUKS"}, {"LVM2_member", "LVM"},
        {"linux_raid_member", "RAID"}, {"bitlocker", "BitLocker"}, {"zfs_member", "ZFS"},
    };
    if (strcmp(id_type, "vfat") == 0)
        return g_strdup(g_strcmp0(id_version, "FAT32") == 0 ? "FAT32" : "FAT");
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
        if (strcmp(id_type, names[i].type) == 0)
            return g_strdup(names[i].label);
    return g_strdup(id_type);
}

guint xd_disk_percent_free(guint64 free, guint64 size) {
    if (size == 0)
        return 0;
    if (free >= size)
        return 100;
    /* free * 100 could overflow for huge sizes; scale down first. */
    long double pct = (long double)free * 100.0L / (long double)size;
    return (guint)(pct + 0.5L);
}

static int compare_parts(gconstpointer left, gconstpointer right) {
    const xd_disk_part *left_part = left, *right_part = right;
    if (left_part->offset != right_part->offset)
        return left_part->offset < right_part->offset ? -1 : 1;
    /* A container before what it holds. */
    return (int)right_part->container - (int)left_part->container;
}

static void add_segment(GArray *out, guint64 offset, guint64 size, int index,
                        xd_segment_kind kind, bool logical) {
    xd_disk_segment disk_segment = {
        .offset = offset, .size = size, .index = index, .kind = kind, .logical = logical};
    g_array_append_val(out, disk_segment);
}

/* The gap [from, to) as unallocated or (inside a container) free space,
 * split where it crosses a container's edge. */
static void add_gap(GArray *out, guint64 from, guint64 to, const xd_disk_part *parts,
                    size_t count) {
    while (from < to) {
        guint64 end = to;
        bool inside = false;
        for (size_t i = 0; i < count; i++) {
            if (!parts[i].container)
                continue;
            guint64 cs = parts[i].offset, ce = parts[i].offset + parts[i].size;
            if (from >= cs && from < ce) {
                inside = true;
                end = (ce < end ? ce : end);
            } else if (cs > from && cs < end) {
                end = cs;
            }
        }
        if (end - from >= XD_DISK_MIN_GAP)
            add_segment(out, from, end - from, -1,
                        inside ? XD_SEGMENT_FREE : XD_SEGMENT_UNALLOCATED, inside);
        from = end;
    }
}

GArray *xd_disk_segments(guint64 disk_size, const xd_disk_part *parts, size_t count) {
    GArray *out = g_array_new(FALSE, TRUE, sizeof(xd_disk_segment));
    xd_disk_part *sorted = count > 0 ? g_memdup2(parts, count * sizeof(*parts)) : nullptr;
    if (count > 0)
        qsort(sorted, count, sizeof(*sorted), compare_parts);
    guint64 cursor = 0;
    for (size_t i = 0; i < count; i++) {
        const xd_disk_part *part = &sorted[i];
        if (part->container)
            continue;
        guint64 start = (part->offset > cursor ? part->offset : cursor);
        guint64 end = part->offset + part->size;
        if (end > disk_size)
            end = disk_size;
        if (start >= end)
            continue; /* overlapped, or past the end */
        add_gap(out, cursor, start, sorted, count);
        bool logical = false;
        for (size_t k = 0; k < count; k++)
            if (sorted[k].container && start >= sorted[k].offset &&
                start < sorted[k].offset + sorted[k].size)
                logical = true;
        add_segment(out, start, end - start, part->index, XD_SEGMENT_PARTITION, logical);
        cursor = end;
    }
    add_gap(out, cursor, disk_size, sorted, count);
    g_free(sorted);
    return out;
}

void xd_disk_block_widths(const guint64 *sizes, size_t count, int total, int min, int *out) {
    if (count == 0)
        return;
    int spare = total - (int)count * min;
    if (spare <= 0) {
        for (size_t i = 0; i < count; i++)
            out[i] = min;
        return;
    }
    long double sum = 0;
    for (size_t i = 0; i < count; i++)
        sum += (long double)sizes[i];
    int given = 0;
    for (size_t i = 0; i < count; i++) {
        int extra =
            sum > 0 ? (int)((long double)spare * (long double)sizes[i] / sum) : spare / (int)count;
        out[i] = min + extra;
        given += out[i];
    }
    /* Rounding's leftovers go to the last block. */
    out[count - 1] += total - given;
}

/* ---- partitioning ---------------------------------------------------------------------- */

guint64 xd_disk_align_up(guint64 bytes) {
    guint64 remainder = bytes % XD_MIB;
    return remainder == 0 ? bytes : bytes + (XD_MIB - remainder);
}

guint64 xd_disk_align_down(guint64 bytes) { return bytes - bytes % XD_MIB; }

xd_new_part_kinds xd_disk_new_part_kinds(const char *table_type, xd_segment_kind kind,
                                         guint n_primary, bool has_extended) {
    xd_new_part_kinds new_part_kinds = {0};
    if (kind == XD_SEGMENT_PARTITION)
        return new_part_kinds;
    if (g_strcmp0(table_type, "dos") != 0) {
        /* GPT: 128 entries; more is never the limit in practice. */
        new_part_kinds.primary = kind == XD_SEGMENT_UNALLOCATED && n_primary < 128;
        return new_part_kinds;
    }
    if (kind == XD_SEGMENT_FREE) {
        new_part_kinds.logical = true;
        return new_part_kinds;
    }
    new_part_kinds.primary = n_primary < 4;
    new_part_kinds.extended = n_primary < 4 && !has_extended;
    return new_part_kinds;
}

bool xd_disk_new_part_range(guint64 seg_offset, guint64 seg_size, guint64 disk_size,
                            bool logical, guint64 *offset, guint64 *max_size) {
    /* A logical drive's extended boot record sits in front of it. */
    guint64 start = xd_disk_align_up(logical ? seg_offset + 1 : seg_offset);
    if (start < XD_MIB)
        start = XD_MIB;
    guint64 end = seg_offset + seg_size;
    guint64 last = disk_size > XD_MIB ? disk_size - XD_MIB : 0;
    end = xd_disk_align_down(end < last ? end : last);
    if (end <= start || end - start < XD_MIB)
        return false;
    *offset = start;
    *max_size = end - start;
    return true;
}

guint64 xd_disk_grow_max(const GArray *segments, guint i, guint64 disk_size) {
    if (segments == nullptr || i >= segments->len)
        return 0;
    const xd_disk_segment *disk_segment = &g_array_index(segments, xd_disk_segment, i);
    guint64 end = disk_segment->offset + disk_segment->size;
    if (i + 1 < segments->len) {
        const xd_disk_segment *next = &g_array_index(segments, xd_disk_segment, i + 1);
        bool free_for_it = disk_segment->logical ? next->kind == XD_SEGMENT_FREE
                                                 : next->kind == XD_SEGMENT_UNALLOCATED;
        if (free_for_it && next->offset == end)
            end = next->offset + next->size;
    }
    guint64 last = disk_size > XD_MIB ? disk_size - XD_MIB : 0;
    guint64 limit = xd_disk_align_down(end < last ? end : last);
    /* Never less than it is: alignment mustn't shrink it. */
    return limit > disk_segment->offset + disk_segment->size ? limit - disk_segment->offset
                                                             : disk_segment->size;
}

guint64 xd_disk_shrink_min(guint64 used, guint64 size) {
    guint64 min;
    if (used == 0) {
        min = 32 * XD_MIB;
    } else {
        guint64 margin = used / 10;
        if (margin < 64 * XD_MIB)
            margin = 64 * XD_MIB;
        min = xd_disk_align_up(used + margin);
    }
    return min < size ? min : size;
}

xd_resize_how xd_disk_resize_how(guint64 modes, bool shrink, bool mounted) {
    guint64 online = shrink ? XD_RESIZE_ONLINE_SHRINK : XD_RESIZE_ONLINE_GROW;
    guint64 offline = shrink ? XD_RESIZE_OFFLINE_SHRINK : XD_RESIZE_OFFLINE_GROW;
    if ((modes & (mounted ? online : offline)) != 0)
        return XD_RESIZE_AS_IS;
    if (mounted && (modes & offline) != 0)
        return XD_RESIZE_NEED_UNMOUNT;
    if (!mounted && (modes & online) != 0)
        return XD_RESIZE_NEED_MOUNT;
    return XD_RESIZE_NO;
}

bool xd_mount_is_system(const char *mount_point) {
    static const char *const system[] = {"/",     "/boot", "/boot/efi", "/efi",
                                         "/usr",  "/var",  "/home",     "/opt",
                                         "/srv",  "/root", nullptr};
    return mount_point != nullptr && g_strv_contains(system, mount_point);
}

static bool any_mount(const char *const *mounts) { return mounts != nullptr && mounts[0] != nullptr; }

static const char *system_mount(const char *const *mounts) {
    for (size_t i = 0; mounts != nullptr && mounts[i] != nullptr; i++)
        if (xd_mount_is_system(mounts[i]))
            return mounts[i];
    return nullptr;
}

const char *xd_volume_lock_reason(const xd_volume_use *volume_use, xd_disk_action action,
                                  bool *need_unmount) {
    *need_unmount = false;
    bool destroys = action == XD_ACTION_DELETE || action == XD_ACTION_FORMAT;
    bool resizes = action == XD_ACTION_SHRINK || action == XD_ACTION_EXTEND;
    const char *type = volume_use->id_type != nullptr ? volume_use->id_type : "";
    if (volume_use->container) {
        if (action == XD_ACTION_DELETE)
            return volume_use->has_logical
                       ? "Delete the logical drives in the extended partition first."
                       : nullptr;
        return "An extended partition only holds logical drives.";
    }
    if ((destroys || resizes) && strcmp(type, "LVM2_member") == 0)
        return "This partition is part of an LVM volume group. Use the LVM tools for it.";
    if ((destroys || resizes) && strcmp(type, "linux_raid_member") == 0)
        return "This partition is part of a RAID array. Use mdadm for it.";
    if ((destroys || resizes) && volume_use->unlocked)
        return "This encrypted volume is unlocked. Lock it first.";
    if (resizes && strcmp(type, "crypto_LUKS") == 0)
        return "Encrypted volumes can't be resized here.";
    if ((destroys || resizes) && volume_use->swap_active)
        return "This is a page file (swap) in use. Turn it off first.";
    if (action == XD_ACTION_LABEL &&
        (type[0] == '\0' || strcmp(type, "swap") == 0 || strcmp(type, "crypto_LUKS") == 0))
        return "This volume has no file system with a label.";
    const char *sys = system_mount(volume_use->mounts);
    if (sys != nullptr && (destroys || action == XD_ACTION_SHRINK))
        return "This is a system volume, in use by the running system. It can't be deleted, "
               "formatted or shrunk.";
    if (destroys && any_mount(volume_use->mounts))
        *need_unmount = true;
    return nullptr;
}

char *xd_volume_status(const char *const *mounts, bool swap_active, bool esp, bool active) {
    const char *what[8];
    size_t count = 0;
    for (size_t i = 0; mounts != nullptr && mounts[i] != nullptr && count < 2; i++) {
        if (strcmp(mounts[i], "/") == 0)
            what[count++] = "System";
        else if (strcmp(mounts[i], "/boot") == 0)
            what[count++] = "Boot";
    }
    if (esp)
        what[count++] = "EFI System Partition";
    if (swap_active)
        what[count++] = "Page File";
    if (active)
        what[count++] = "Active";
    GString *status = g_string_new("Healthy");
    for (size_t i = 0; i < count; i++)
        g_string_append_printf(status, "%s%s", i == 0 ? " (" : ", ", what[i]);
    if (count > 0)
        g_string_append_c(status, ')');
    return g_string_free(status, FALSE);
}

const xd_part_type xd_part_types[] = {
    {"gpt", "0fc63daf-8483-4772-8e79-3d69d8477de4", "Linux filesystem"},
    {"gpt", "933ac7e1-2eb4-4f13-b844-0e14e2aef915", "Linux home"},
    {"gpt", "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "Linux swap"},
    {"gpt", "e6d6d379-f507-44c2-a23c-238f2a3df928", "Linux LVM"},
    {"gpt", "a19d880f-05fc-4d3b-a006-743f0f84911e", "Linux RAID"},
    {"gpt", "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "EFI System Partition"},
    {"gpt", "21686148-6449-6e6f-744e-656564454649", "BIOS boot partition"},
    {"gpt", "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "Microsoft basic data"},
    {"gpt", "e3c9e316-0b5c-4db8-817d-f92df00215ae", "Microsoft reserved"},
    {"gpt", "de94bba4-06d1-4d40-a16a-bfd50179d6ac", "Windows recovery environment"},
    {"dos", "0x83", "Linux"},
    {"dos", "0x82", "Linux swap"},
    {"dos", "0x8e", "Linux LVM"},
    {"dos", "0xfd", "Linux RAID autodetect"},
    {"dos", "0x07", "NTFS / exFAT"},
    {"dos", "0x0c", "FAT32 (LBA)"},
    {"dos", "0x0b", "FAT32"},
    {"dos", "0x06", "FAT16"},
    {"dos", "0xef", "EFI System Partition"},
    {"dos", "0x05", "Extended"},
    {"dos", "0x0f", "Extended (LBA)"},
};
const size_t xd_part_type_count = G_N_ELEMENTS(xd_part_types);

const char *xd_part_type_name(const char *table_type, const char *id) {
    if (id == nullptr)
        return nullptr;
    for (size_t i = 0; i < xd_part_type_count; i++)
        if (g_strcmp0(table_type, xd_part_types[i].table) == 0 &&
            g_ascii_strcasecmp(id, xd_part_types[i].id) == 0)
            return xd_part_types[i].name;
    return nullptr;
}

bool xd_part_type_is_esp(const char *id) {
    return id != nullptr && (g_ascii_strcasecmp(id, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") == 0 ||
                             g_ascii_strcasecmp(id, "0xef") == 0);
}

/* ---- Disk Management: startup mount points, format options ------------------------------ */

bool xd_udisks_version_at_least(const char *version, int major, int minor) {
    if (version == nullptr)
        return false;
    char *end = nullptr;
    long ma = strtol(version, &end, 10);
    if (end == version)
        return false;
    long mi = *end == '.' ? strtol(end + 1, nullptr, 10) : 0;
    return ma > major || (ma == major && mi >= minor);
}

char *xd_fstab_dir_clean(const char *typed) {
    char *dir = g_strstrip(g_strdup(typed != nullptr ? typed : ""));
    size_t len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/')
        dir[--len] = '\0';
    return dir;
}

const char *xd_fstab_dir_problem(const char *dir) {
    if (dir == nullptr || dir[0] == '\0')
        return "Type the folder to mount the volume in.";
    if (dir[0] != '/')
        return "The folder must be a full path, starting with /.";
    for (const char *character = dir; *character != '\0'; character++)
        if (g_ascii_iscntrl(*character))
            return "The folder's name can't have tabs or line breaks in it.";
    char **parts = g_strsplit(dir, "/", -1);
    bool dots = false;
    for (char **part = parts; *part != nullptr; part++)
        dots |= strcmp(*part, ".") == 0 || strcmp(*part, "..") == 0;
    g_strfreev(parts);
    if (dots)
        return "The folder must be a full path without . or .. in it.";
    /* Mounting over these would hide what the running system needs. */
    static const char *const kept[] = {"/bin",  "/etc",   "/lib",       "/lib64", "/sbin", "/run",
                                       "/tmp",  "/mnt",   "/media",     "/var/tmp",
                                       "/run/media", nullptr};
    if (xd_mount_is_system(dir) || g_strv_contains(kept, dir))
        return "The running system uses that folder. Choose an empty folder of your own, such "
               "as one in /mnt.";
    static const char *const below[] = {"/proc", "/sys", "/dev", "/etc", "/usr", "/boot", nullptr};
    for (size_t i = 0; below[i] != nullptr; i++) {
        size_t count = strlen(below[i]);
        if (strncmp(dir, below[i], count) == 0 && (dir[count] == '\0' || dir[count] == '/'))
            return "The running system keeps that folder for itself. Choose an empty folder of "
                   "your own, such as one in /mnt.";
    }
    return nullptr;
}

char *xd_fstab_suggest_dir(const char *label, const char *device) {
    const char *name = label;
    if (name == nullptr || name[0] == '\0') {
        const char *slash = device != nullptr ? strrchr(device, '/') : nullptr;
        name = slash != nullptr ? slash + 1 : device;
    }
    if (name == nullptr || name[0] == '\0')
        name = "volume";
    GString *path = g_string_new("/mnt/");
    for (const char *character = name; *character != '\0'; character++)
        g_string_append_c(path, g_ascii_isalnum(*character) || *character == '.' ||
                                        *character == '_' || *character == '-'
                                    ? *character
                                    : '_');
    /* "." or ".." alone isn't a folder's name. */
    if (strcmp(path->str, "/mnt/.") == 0 || strcmp(path->str, "/mnt/..") == 0)
        g_string_assign(path, "/mnt/volume");
    return g_string_free(path, FALSE);
}

char *xd_fstab_default_options(const char *id_type, guint uid, guint gid) {
    static const char *const ownerless[] = {"vfat", "exfat", "ntfs", "ntfs3", nullptr};
    if (id_type != nullptr && g_strv_contains(ownerless, id_type))
        return g_strdup_printf("nofail,x-mount.mkdir,uid=%u,gid=%u", uid, gid);
    return g_strdup("nofail,x-mount.mkdir");
}

/* XP's lists: NTFS 512 bytes to 64K, FAT32 to 32K (FAT's own limit),
 * exFAT to 32 MB; ext4 and XFS up to the page size Linux mounts. */
static const guint units_ext[] = {1024, 2048, 4096, 0};
static const guint units_fat[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 0};
static const guint units_ntfs[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 0};
static const guint units_exfat[] = {512,     1024,    2048,     4096,     8192,     16384,
                                    32768,   65536,   131072,   262144,   524288,   1048576,
                                    2097152, 4194304, 8388608,  16777216, 33554432, 0};

const guint *xd_format_unit_sizes(const char *type) {
    if (g_strcmp0(type, "ext4") == 0 || g_strcmp0(type, "xfs") == 0)
        return units_ext;
    if (g_strcmp0(type, "vfat") == 0)
        return units_fat;
    if (g_strcmp0(type, "ntfs") == 0)
        return units_ntfs;
    if (g_strcmp0(type, "exfat") == 0)
        return units_exfat;
    /* Btrfs's sector size has to be the page size; swap has no clusters. */
    return nullptr;
}

char *xd_format_unit_label(guint bytes) {
    return bytes < 16384 ? g_strdup_printf("%u bytes", bytes)
                         : g_strdup_printf("%uK", bytes / 1024);
}

char **xd_format_unit_args(const char *type, guint bytes, guint sector_size) {
    const guint *sizes = xd_format_unit_sizes(type);
    bool listed = false;
    for (size_t i = 0; sizes != nullptr && sizes[i] != 0; i++)
        listed |= sizes[i] == bytes;
    if (!listed || sector_size == 0 || bytes < sector_size)
        return nullptr;
    char *flag, *value;
    if (strcmp(type, "vfat") == 0) {
        /* mkfs.fat counts sectors per cluster, at most 128. */
        if (bytes / sector_size > 128)
            return nullptr;
        flag = g_strdup("-s");
        value = g_strdup_printf("%u", bytes / sector_size);
    } else if (strcmp(type, "xfs") == 0) {
        flag = g_strdup("-b");
        value = g_strdup_printf("size=%u", bytes);
    } else {
        /* mke2fs -b, mkntfs -c and exfatprogs' mkfs.exfat -c take bytes. */
        flag = g_strdup(strcmp(type, "ext4") == 0 ? "-b" : "-c");
        value = g_strdup_printf("%u", bytes);
    }
    char **args = g_new0(char *, 3);
    args[0] = flag;
    args[1] = value;
    return args;
}
