/* SPDX-License-Identifier: MIT
 * Administrative Tools (xd-services, xd-eventvwr, xd-diskmgmt): what
 * they show, worked out from systemd's, journald's and UDisks2's data
 * without any GTK, so it can be tested on its own.
 */
#ifndef XD_ADMIN_ADMIN_H
#define XD_ADMIN_ADMIN_H

#include <glib.h>

/* ---- Services (systemd units) ---- */

/* The Startup Type column from a unit file's state (UnitFileState or
 * ListUnitFiles'): "Enabled", "Disabled", "Static", "Masked", "Enabled
 * (runtime)", ...; "" when unknown. Free with g_free. */
char *xd_unit_startup_label(const char *unit_file_state);

/* What the Properties dialog's Startup type list can set. */
typedef enum xd_unit_startup : int8_t {
    XD_STARTUP_OTHER = -1, /* static, generated, ...: the unit file decides */
    XD_STARTUP_ENABLED,
    XD_STARTUP_DISABLED,
    XD_STARTUP_MASKED,
} xd_unit_startup;
xd_unit_startup xd_unit_startup_of(const char *unit_file_state);

/* The Status column from ActiveState: "Started", "Starting", "Stopping",
 * "Failed", or "" when stopped (as Windows leaves it blank). */
const char *xd_unit_status_label(const char *active_state);
/* Whether the unit counts as started (Stop rather than Start). */
bool xd_unit_is_started(const char *active_state);

/* "sshd.service" -> "sshd" (any unit type's suffix). Free with g_free. */
char *xd_unit_display_name(const char *id);

/* Log On As: the service's User=, else "Local System" for the system
 * manager, else `me` (the user manager runs as you). */
const char *xd_unit_logon_label(const char *user, bool user_manager, const char *me);

/* A command line from an ExecStart argv, quoting the arguments that need
 * it. Free with g_free. */
char *xd_unit_command_line(const char *const *argv);

/* ---- Event Viewer (journald) ---- */

typedef enum xd_event_log : uint8_t {
    XD_LOG_APPLICATION,
    XD_LOG_SECURITY,
    XD_LOG_SYSTEM,
    XD_LOG_COUNT,
} xd_event_log;

/* Which of XP's logs a journal entry goes in. Security: the auth and
 * authpriv syslog facilities, the audit transport and the programs that
 * authenticate (sudo, su, polkit, sshd, login, ...). System: the kernel
 * and system services. Application: your own programs and user units.
 * `facility` and `uid` are -1 when the entry has none. */
xd_event_log xd_event_log_of(int facility, const char *identifier, const char *transport,
                             bool user_unit, long uid);

/* The identifiers that make an entry a Security event. */
extern const char *const xd_security_identifiers[];

typedef enum xd_event_type : uint8_t {
    XD_EVENT_ERROR,
    XD_EVENT_WARNING,
    XD_EVENT_INFORMATION,
    XD_EVENT_AUDIT_SUCCESS,
    XD_EVENT_AUDIT_FAILURE,
    XD_EVENT_TYPE_COUNT,
} xd_event_type;

/* From the syslog priority (0 emerg ... 7 debug; -1 when missing, which
 * journald treats as info): errors and worse are Errors, warnings
 * Warnings, the rest Information. The Security log has audits instead:
 * a warning or worse is a Failure Audit. */
xd_event_type xd_event_type_of(int priority, xd_event_log log);
const char *xd_event_type_label(xd_event_type event_type);
const char *xd_event_type_icon(xd_event_type event_type);

/* ---- Disk Management (UDisks2) ---- */

/* Windows' sizes: "232.88 GB", "512 MB", "4 KB" (binary units). Free
 * with g_free. */
char *xd_disk_size_text(guint64 bytes);

/* The File System column from IdType and IdVersion: "NTFS", "FAT32",
 * "exFAT", "CDFS", "ext4", "Swap", "LUKS", ...; "" when there's none. */
char *xd_disk_fs_label(const char *id_type, const char *id_version);

/* Free space as a whole percentage of `size`, rounded. */
guint xd_disk_percent_free(guint64 free, guint64 size);

/* A partition (or a container, such as an MBR extended partition) on a
 * disk, as the disk map lays them out. `index` is the caller's. */
typedef struct xd_disk_part {
    guint64 offset, size;
    int index;
    bool container;
} xd_disk_part;

typedef enum xd_segment_kind : uint8_t {
    XD_SEGMENT_PARTITION,
    XD_SEGMENT_UNALLOCATED, /* outside any partition */
    XD_SEGMENT_FREE,        /* unused space in an extended partition */
} xd_segment_kind;

typedef struct xd_disk_segment {
    guint64 offset, size;
    int index; /* the partition's, or -1 */
    xd_segment_kind kind;
    bool logical; /* inside a container */
} xd_disk_segment;

/* Gaps smaller than this (alignment, GPT's backup table) aren't shown. */
#define XD_DISK_MIN_GAP ((guint64)16 * 1024 * 1024)

/* The disk map's blocks left to right: the partitions by offset (not the
 * containers themselves), with the gaps between them as unallocated or
 * free space. Overlaps and space past the disk's end are clipped. Free
 * with g_array_unref. */
GArray *xd_disk_segments(guint64 disk_size, const xd_disk_part *parts, size_t count);

/* The disk map's block widths: proportional to `sizes`, each at least
 * `min` pixels, `total` in all (when there's room). */
void xd_disk_block_widths(const guint64 *sizes, size_t count, int total, int min, int *out);

/* ---- Disk Management: partitioning ---- */

/* Partitions start and end on 1 MiB boundaries, as Windows Vista on and
 * parted do; the first starts 1 MiB in, and the disk's last MiB is left
 * for GPT's backup table. */
#define XD_MIB ((guint64)1024 * 1024)
guint64 xd_disk_align_up(guint64 bytes);
guint64 xd_disk_align_down(guint64 bytes);

/* What New Partition can make in a block of free space: GPT has only
 * (primary) partitions; MBR has four primary ones, one of which may be
 * the extended partition holding the logical drives. `n_primary`
 * counts the extended one too. */
typedef struct xd_new_part_kinds {
    bool primary, extended, logical;
} xd_new_part_kinds;
xd_new_part_kinds xd_disk_new_part_kinds(const char *table_type, xd_segment_kind kind,
                                         guint n_primary, bool has_extended);

/* Where a new partition goes in the free block [seg_offset, +seg_size)
 * and how big it can be, aligned; a logical drive leaves room for its
 * boot record. False when less than 1 MiB fits. */
bool xd_disk_new_part_range(guint64 seg_offset, guint64 seg_size, guint64 disk_size,
                            bool logical, guint64 *offset, guint64 *max_size);

/* The size the partition at segments[i] can grow to: into the free
 * block right after it (unallocated space for a primary partition, the
 * extended partition's free space for a logical drive), else its own
 * size. */
guint64 xd_disk_grow_max(const GArray *segments, guint i, guint64 disk_size);

/* The smallest size a filesystem can be shrunk to: what it holds and a
 * margin, aligned; 32 MiB when its use is unknown (the filesystem's own
 * resize tool then refuses too small a size). Never more than `size`. */
guint64 xd_disk_shrink_min(guint64 used, guint64 size);

/* UDisks2's CanResize modes (libblockdev's BDFSResizeFlags). */
enum {
    XD_RESIZE_OFFLINE_SHRINK = 2,
    XD_RESIZE_OFFLINE_GROW = 4,
    XD_RESIZE_ONLINE_SHRINK = 8,
    XD_RESIZE_ONLINE_GROW = 16,
};
typedef enum xd_resize_how : uint8_t {
    XD_RESIZE_NO,           /* the filesystem can't be resized this way */
    XD_RESIZE_AS_IS,        /* mounted or not, as it is now */
    XD_RESIZE_NEED_UNMOUNT, /* only unmounted */
    XD_RESIZE_NEED_MOUNT,   /* only mounted (btrfs) */
} xd_resize_how;
xd_resize_how xd_disk_resize_how(guint64 modes, bool shrink, bool mounted);

/* What the running system needs mounted: "/", "/boot", "/usr", ... */
bool xd_mount_is_system(const char *mount_point);

/* What Disk Management can do to a volume. */
typedef enum xd_disk_action : uint8_t {
    XD_ACTION_DELETE,
    XD_ACTION_FORMAT,
    XD_ACTION_SHRINK,
    XD_ACTION_EXTEND,
    XD_ACTION_LABEL,
    XD_ACTION_SET_TYPE,
    XD_ACTION_SET_ACTIVE,
} xd_disk_action;

/* How a volume is in use. */
typedef struct xd_volume_use {
    const char *const *mounts; /* its mount points (nullptr-terminated), or nullptr */
    bool swap_active;
    bool unlocked;   /* an encrypted volume that's unlocked */
    bool container;  /* an extended partition */
    bool has_logical; /* ... with logical drives in it */
    const char *id_type;
} xd_volume_use;

/* Why `action` can't be done to the volume, as Windows greys it for the
 * system and boot volumes, or nullptr if it can. When it can but only
 * unmounted, *need_unmount is set (Disk Management unmounts it first). */
const char *xd_volume_lock_reason(const xd_volume_use *volume_use, xd_disk_action action,
                                  bool *need_unmount);

/* The Status column: "Healthy (System)", "Healthy (Boot)", "Healthy
 * (EFI System Partition)", "Healthy (Page File)", "Healthy (Active)",
 * "Healthy". Free with g_free. */
char *xd_volume_status(const char *const *mounts, bool swap_active, bool esp, bool active);

/* Partition types by table ("gpt" GUIDs, "dos" hex bytes). */
typedef struct xd_part_type {
    const char *table, *id, *name;
} xd_part_type;
extern const xd_part_type xd_part_types[];
extern const size_t xd_part_type_count;
/* The type's name, or nullptr when it isn't a known one. */
const char *xd_part_type_name(const char *table_type, const char *id);
/* Whether it's the EFI System Partition's type. */
bool xd_part_type_is_esp(const char *id);

/* ---- Disk Management: startup mount points and format options ---- */

/* Whether UDisks2's Manager.Version ("2.11.0") is at least major.minor. */
bool xd_udisks_version_at_least(const char *version, int major, int minor);

/* A typed mount point tidied: surrounding blanks and trailing slashes
 * dropped ("/" stays). Free with g_free. */
char *xd_fstab_dir_clean(const char *typed);

/* Why `dir` (cleaned) can't be a startup mount point, or nullptr: it
 * must be a full path without "." or ".." in it, not one the running
 * system uses or keeps for itself (/, /usr, /etc, /proc, ...). */
const char *xd_fstab_dir_problem(const char *dir);

/* The mount point Add suggests: /mnt/ and the label (or the device's
 * name, "sdb1"), with anything but letters, digits, ".", "_" and "-"
 * made "_". Free with g_free. */
char *xd_fstab_suggest_dir(const char *label, const char *device);

/* The options a new startup mount point gets: "nofail" (a missing disk
 * doesn't stop the computer starting), "x-mount.mkdir" (the folder is
 * made if it isn't there), and for file systems without Unix owners
 * (FAT, exFAT, NTFS) uid= and gid= so the files are the user's. Free
 * with g_free. */
char *xd_fstab_default_options(const char *id_type, guint uid, guint gid);

/* The allocation unit sizes (bytes, 0-terminated) Format can ask mkfs
 * for on `type`, or nullptr when only Default is possible. */
const guint *xd_format_unit_sizes(const char *type);
/* XP's names for them: "512 bytes" ... "8192 bytes", then "16K", "64K".
 * Free with g_free. */
char *xd_format_unit_label(guint bytes);
/* The mkfs arguments (UDisks2 Format's "mkfs-args") for allocation unit
 * `bytes` on `type`, for a disk of `sector_size` byte logical sectors;
 * nullptr when that size can't be made. Free with g_strfreev. */
char **xd_format_unit_args(const char *type, guint bytes, guint sector_size);

#endif
