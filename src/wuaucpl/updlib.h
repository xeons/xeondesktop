/* SPDX-License-Identifier: MIT
 * The parts of Automatic Updates (xd-wuaucpl) that need no window and no
 * service: how important an update is, PackageKit's restart hints, and
 * reading what fwupd says (release notes, checksums, the Host Security ID
 * and its attributes).
 */
#ifndef XD_WUAUCPL_UPDLIB_H
#define XD_WUAUCPL_UPDLIB_H

#include <glib.h>
#include <packagekit-glib2/packagekit.h>

/* How important an update is, Windows Update's way: the higher the more. */
typedef enum xd_update_rank {
    XD_RANK_UNKNOWN,
    XD_RANK_OPTIONAL,    /* PackageKit's low; fwupd's low urgency */
    XD_RANK_ENHANCEMENT, /* a new feature */
    XD_RANK_RECOMMENDED, /* normal; fwupd's medium */
    XD_RANK_BUGFIX,      /* fixes problems */
    XD_RANK_IMPORTANT,   /* important; fwupd's high */
    XD_RANK_SECURITY,    /* a security fix */
    XD_RANK_CRITICAL,    /* critical; fwupd's critical */
} xd_update_rank;

/* From a package's update severity (PackageKit 1.2.4 on) or else its info,
 * as GetUpdates gives them. */
xd_update_rank xd_update_rank_of_package(PkInfoEnum info, PkInfoEnum severity);
/* From a firmware release's Urgency (fwupd's FwupdReleaseUrgency). */
xd_update_rank xd_update_rank_of_urgency(unsigned urgency);
/* "Critical", "Security", ...; "" for UNKNOWN. */
const char *xd_update_rank_name(xd_update_rank rank);
/* Counted as important in the notification area's tooltip. */
bool xd_update_rank_important(xd_update_rank rank);

/* What an update's restart hint means for the user, or nullptr for none. */
const char *xd_update_restart_text(PkRestartEnum restart);
/* Whether the computer must restart (not only a program or the session). */
bool xd_update_restart_system(PkRestartEnum restart);

/* A firmware release's description (AppStream markup: <p>, <ul>/<ol>
 * with <li>, <em>, <code>) as plain text: paragraphs apart, items as
 * bullets. Invalid markup gives its text with the tags stripped by
 * GMarkup's reading, or nullptr if it can't be read at all. */
char *xd_fwupd_markup_to_text(const char *markup);

/* Whether `data` matches one of a release's checksums (fwupd's
 * comma-separated "Checksum": SHA-256 when given, else SHA-1; MD5 isn't
 * trusted). False when there's no usable checksum. */
bool xd_fwupd_checksum_ok(const char *checksums, GBytes *data);

/* The first location to download a release from: https, or file (a
 * local remote, and the tests); nullptr if none can be used. */
const char *xd_fwupd_pick_location(const char *const *locations, const char *uri);

/* The level in fwupd's HostSecurityId ("HSI:2! (v1.9.6)" is 2, with
 * runtime issues); -1 if it isn't one ("HSI:INVALID:..."). */
int xd_fwupd_hsi_level(const char *hsi, bool *runtime_issue);
/* What a level means, a sentence. */
const char *xd_fwupd_hsi_level_text(int level);
/* A security attribute's result (fwupd's FwupdSecurityAttrResult):
 * "Enabled", "Not locked", ...; "" when unknown. */
const char *xd_fwupd_hsi_result_text(unsigned result);
/* A readable name for an attribute: fwupd's Title (1.8 on), else a
 * name we know for its AppstreamId, else its Name, else the id. */
const char *xd_fwupd_attr_title(const char *appstream_id, const char *title, const char *name);

#endif
