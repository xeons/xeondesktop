/* SPDX-License-Identifier: MIT
 * Flatpak permissions without a window: an app's [Context], bus policies
 * and environment merged from its layers, and edits to a per-user
 * override file, in the format flatpak-metadata(5) documents and
 * `flatpak override --user` writes:
 *
 *   [Context]
 *   shared=ipc;!network;
 *   sockets=!wayland;x11;if:x11:!has-wayland;
 *   filesystems=home:ro;!xdg-download;/srv/data:create;
 *   persistent=.foo;
 *   unset-environment=HELLO_MODE;
 *   [Session Bus Policy]
 *   org.example.Name=talk
 *   [Environment]
 *   A=1
 *
 * The layers, lowest first, as flatpak merges them for `flatpak run` (and
 * `flatpak info --show-permissions` shows): the runtime's metadata, the
 * app's, then for an app of a system installation the system's global
 * overrides and the app's system overrides, then the user's global
 * overrides and last the user's override for the app, the one this
 * applet edits. Within a list, each layer changes only the names it
 * mentions: "x" grants, "!x" takes away, and "if:x:condition" (Flatpak
 * 1.17) grants only when the condition holds; a layer with conditions for
 * a name makes it conditional even beside a plain "x" (written for older
 * Flatpaks).
 */
#ifndef XD_FLATPAK_PERMS_H
#define XD_FLATPAK_PERMS_H

#include <glib.h>

/* The [Context] lists of names that are granted or not. */
typedef enum xd_fp_list : uint8_t {
    XD_FP_SHARED,
    XD_FP_SOCKETS,
    XD_FP_DEVICES,
    XD_FP_FEATURES,
    XD_FP_LIST_COUNT,
} xd_fp_list;

typedef enum xd_fp_bus : uint8_t {
    XD_FP_SESSION_BUS,
    XD_FP_SYSTEM_BUS,
    XD_FP_BUS_COUNT,
} xd_fp_bus;

/* A name's state in a list. */
typedef enum xd_fp_state : uint8_t {
    XD_FP_UNSET,       /* not mentioned: not granted */
    XD_FP_ON,
    XD_FP_OFF,         /* "!name" */
    XD_FP_CONDITIONAL, /* granted only "if:name:condition" */
} xd_fp_state;

/* A filesystem's access. */
typedef enum xd_fp_fs_mode : uint8_t {
    XD_FP_FS_NONE, /* "!fs": taken away */
    XD_FP_FS_RO,   /* "fs:ro" */
    XD_FP_FS_RW,   /* "fs" or "fs:rw" */
    XD_FP_FS_CREATE, /* "fs:create" */
} xd_fp_fs_mode;

typedef struct xd_fp_perms xd_fp_perms;

/* The [Context] key of a list ("shared", ...). */
const char *xd_fp_list_key(xd_fp_list list);
/* "Session Bus Policy" or "System Bus Policy". */
const char *xd_fp_bus_group(xd_fp_bus bus);
/* The names flatpak(1) 1.18 knows in a list, nullptr-terminated. */
const char *const *xd_fp_list_names(xd_fp_list list);

/* Nothing granted. */
xd_fp_perms *xd_fp_perms_new(void);
void xd_fp_perms_free(xd_fp_perms *permissions);
xd_fp_perms *xd_fp_perms_copy(const xd_fp_perms *original);
/* Puts a layer (metadata or an override file) over what's there. */
void xd_fp_perms_merge(xd_fp_perms *permissions, GKeyFile *layer);

xd_fp_state xd_fp_perms_state(const xd_fp_perms *permissions, xd_fp_list list, const char *name);
/* A conditional name's conditions ("!has-wayland", ...), or nullptr. */
const GPtrArray *xd_fp_perms_conditions(const xd_fp_perms *permissions, xd_fp_list list,
                                        const char *name);
/* Names mentioned in the list (granted, taken away or conditional) that
 * xd_fp_list_names doesn't know, sorted: a newer Flatpak's. Free the
 * array only (g_ptr_array_unref). */
GPtrArray *xd_fp_perms_other_names(const xd_fp_perms *permissions, xd_fp_list list);

/* The filesystems mentioned, sorted (canonical form, without the mode),
 * including the ones taken away (NONE). Free the array only. */
GPtrArray *xd_fp_perms_filesystems(const xd_fp_perms *permissions);
/* A filesystem's access; *found false when it's not mentioned. */
xd_fp_fs_mode xd_fp_perms_filesystem(const xd_fp_perms *permissions, const char *fs, bool *found);
/* Homedir-relative persistent directories, sorted. Free the array only. */
GPtrArray *xd_fp_perms_persistent(const xd_fp_perms *permissions);
/* Variable names set or unset, sorted. Free the array only. */
GPtrArray *xd_fp_perms_env_names(const xd_fp_perms *permissions);
/* A variable's value: nullptr if unset (*found true) or not mentioned
 * (*found false). */
const char *xd_fp_perms_env(const xd_fp_perms *permissions, const char *name, bool *found);
/* Bus names with a policy, sorted. Free the array only. */
GPtrArray *xd_fp_perms_bus_names(const xd_fp_perms *permissions, xd_fp_bus bus);
/* "none", "see", "talk" or "own"; nullptr when not mentioned. */
const char *xd_fp_perms_bus(const xd_fp_perms *permissions, xd_fp_bus bus, const char *name);

/* "[!]fs[:mode]" as flatpak reads it: the canonical filesystem ("~" is
 * "home", "home/x" is "~/x", doubled and trailing slashes go) and its
 * mode, NONE for a leading "!". nullptr if empty. g_free it. */
char *xd_fp_filesystem_parse(const char *entry, xd_fp_fs_mode *mode);
/* Whether flatpak accepts `fs` (no mode) as a filesystem: a keyword
 * (home, host, xdg-download...), one with a /subpath where flatpak allows
 * it, an absolute path or ~/path. */
bool xd_fp_filesystem_valid(const char *fs);
/* A D-Bus name or a prefix ending in ".*". */
bool xd_fp_bus_name_valid(const char *name);
/* An environment variable's name: not empty, no '='. */
bool xd_fp_env_name_valid(const char *name);

/* ---- the user's override file ---------------------------------------------- */

/* `dir`/overrides/`app_id`: dir is the user installation's base
 * directory (flatpak_installation_get_path: $FLATPAK_USER_DIR, or
 * $XDG_DATA_HOME/flatpak). g_free it. */
char *xd_fp_override_path(const char *dir, const char *app_id);
/* The file; an empty key file when there's none. nullptr (and `error`)
 * when it can't be read or parsed. */
GKeyFile *xd_fp_override_load(const char *path, GError **error);
/* Writes the file, creating its folder, or removes it when nothing is
 * left in it (as `flatpak override --reset` does). */
bool xd_fp_override_save(GKeyFile *override, const char *path, GError **error);

/* Sets a name's state in the override: ON writes "name", OFF "!name",
 * UNSET drops it (and any "if:name:" conditions) so the layers below
 * decide. Other entries stay as they were. */
void xd_fp_override_set_state(GKeyFile *override, xd_fp_list list, const char *name,
                              xd_fp_state state);
/* Sets a filesystem's entry: `remove` drops it, otherwise "fs", "fs:ro",
 * "fs:create" or "!fs" for NONE. */
void xd_fp_override_set_filesystem(GKeyFile *override, const char *fs, xd_fp_fs_mode mode,
                                   bool remove);
void xd_fp_override_set_persistent(GKeyFile *override, const char *path, bool present);
/* Sets a variable (value), unsets it (value nullptr: unset-environment and
 * an empty [Environment] entry, as flatpak writes it), or with `remove`
 * drops it from the override. */
void xd_fp_override_set_env(GKeyFile *override, const char *name, const char *value,
                            bool remove);
/* Sets a bus name's policy; nullptr drops it. */
void xd_fp_override_set_bus(GKeyFile *override, xd_fp_bus bus, const char *name,
                            const char *policy);

#endif
