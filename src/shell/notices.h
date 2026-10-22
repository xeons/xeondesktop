/* SPDX-License-Identifier: MIT
 * The notifications programs send (org.freedesktop.Notifications'
 * Notify), as kept: the ones showing or waiting to, and the ones missed,
 * for the history. GLib only (and cairo for pictures): the D-Bus server
 * and the balloons are notifyd.c's.
 */
#ifndef XD_SHELL_NOTICES_H
#define XD_SHELL_NOTICES_H

#include <cairo.h>
#include <glib.h>
#include <stdint.h>

typedef enum xd_notice_urgency : uint8_t {
    XD_NOTICE_LOW,
    XD_NOTICE_NORMAL,
    XD_NOTICE_CRITICAL,
} xd_notice_urgency;

typedef struct xd_notice {
    uint32_t id;
    /* The unique bus name that sent it, to tell of its actions; nullptr
     * once it has gone (or for one kept from an earlier session). */
    char *sender;
    char *app_name;
    char *desktop_entry; /* its program's desktop file id, ".desktop" added; or nullptr */
    char *icon;          /* app_icon or image-path: a theme name or an absolute path; or nullptr */
    cairo_surface_t *image; /* image-data, or nullptr */
    char *summary;
    char *body;     /* plain text: markup taken out */
    char **actions; /* key, label, key, label... (nullptr-terminated); never nullptr */
    xd_notice_urgency urgency;
    int32_t expire_ms; /* -1: ours to choose; 0: until closed */
    bool transient;    /* not kept once it has gone */
    bool resident;     /* not closed by its actions */
    bool suppress_sound;
    int64_t time;     /* when it came, Unix seconds */
    bool unread;      /* not seen: it came while it couldn't be shown, or went unclicked */
    bool closed;      /* its program was told it closed: kept for the history only */
} xd_notice;

/* A Notify call's parameters, "(susssasa{sv}i)", as a notice (id 0), and
 * its replaces_id. nullptr (with `error`) for nonsense. Limits what's
 * kept: long texts are cut, many actions dropped, big pictures refused. */
[[nodiscard]] xd_notice *xd_notice_parse(GVariant *parameters, const char *sender,
                                         uint32_t *replaces_id, GError **error);
void xd_notice_free(xd_notice *notice);
/* The label of `notice`'s action `key`, or nullptr. */
const char *xd_notice_action_label(const xd_notice *notice, const char *key);
/* Its actions other than "default" (what a click on it does): how many,
 * and the i-th one's key. */
size_t xd_notice_button_count(const xd_notice *notice);
const char *xd_notice_button_key(const xd_notice *notice, size_t index);

/* Body markup (<b>, <i>, <a href>, <img alt>, entities) as plain text;
 * text that isn't markup as it is. g_free. */
char *xd_notice_plain_text(const char *body);

/* The ones kept, oldest first, by id. */
typedef struct xd_notices {
    GPtrArray *list; /* xd_notice * */
    uint32_t last_id;
} xd_notices;

void xd_notices_init(xd_notices *notices);
void xd_notices_finish(xd_notices *notices);
/* Keeps `notice` (taken). With `replaces_id` one kept from the same
 * sender, it takes that one's place and id (*replaced true); else it's
 * newest, with a new id. Returns it. */
xd_notice *xd_notices_add(xd_notices *notices, xd_notice *notice, uint32_t replaces_id,
                          bool *replaced);
xd_notice *xd_notices_find(const xd_notices *notices, uint32_t id);
/* Forgets it; false if it wasn't kept. */
bool xd_notices_remove(xd_notices *notices, uint32_t id);
size_t xd_notices_unread(const xd_notices *notices);
/* Forgets the oldest past `max` (never `keep`), returning their ids
 * (g_array_unref) for the ones not yet closed, to tell their programs. */
GArray *xd_notices_trim(xd_notices *notices, size_t max, uint32_t keep);
/* The kept ones (not transient), as a key file for the next session:
 * no pictures, actions or senders. g_free. */
char *xd_notices_save_data(const xd_notices *notices);
/* Adds what xd_notices_save_data wrote, as closed (with new ids). */
void xd_notices_load_data(xd_notices *notices, const char *data);

#endif
