/* SPDX-License-Identifier: MIT
 * Control Panel applets: the installed programs whose desktop entry has
 * `Categories=...;X-XeonDesktop-ControlPanel;`, found through GIO
 * (GDesktopAppInfo), and XP's categories for them. An applet names its
 * categories with `X-XeonDesktop-ControlPanel-Category=appearance;` (keys
 * below); one without goes under "Other Control Panel Options". Applets
 * are usually NoDisplay=true: as in XP, they show in Control Panel, not
 * in All Programs.
 */
#ifndef XD_FILES_CONTROL_H
#define XD_FILES_CONTROL_H

#include <gio/gdesktopappinfo.h>

G_BEGIN_DECLS

#define XD_CONTROL_CATEGORY "X-XeonDesktop-ControlPanel"
#define XD_CONTROL_CATEGORY_KEY "X-XeonDesktop-ControlPanel-Category"

typedef struct XdControlCategory {
    const char *key;   /* in X-XeonDesktop-ControlPanel-Category */
    const char *title; /* XP's name */
    const char *icon;  /* theme icon name */
    const char *fallback;
} XdControlCategory;

/* XP's nine categories in XP's order, then "other". */
extern const XdControlCategory xd_control_categories[];
extern const size_t xd_control_category_count;
#define XD_CONTROL_OTHER "other"

const XdControlCategory *xd_control_category(const char *key);
/* The category's icon, with its fallbacks (a new reference). */
GIcon *xd_control_category_icon(const XdControlCategory *c);

/* Is this a Control Panel applet (for this desktop)? */
gboolean xd_control_is_applet(GDesktopAppInfo *info);
/* All applets, by name. Free with g_list_free_full(list, g_object_unref). */
GList *xd_control_applets(void);
/* Whether `info` belongs in category `key` (an applet without a known
 * category is in "other"). */
gboolean xd_control_applet_in(GDesktopAppInfo *info, const char *key);

/* An applet by a short name, as Windows' `control desk.cpl`: its
 * program's name without "xd-" ("desk", "sysdm", "timedate"), with or
 * without ".cpl", its desktop file id, or its Name ("Display"); case
 * doesn't matter. A new reference, or nullptr. */
GDesktopAppInfo *xd_control_find(const char *name);

G_END_DECLS

#endif
