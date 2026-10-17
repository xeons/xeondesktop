/* SPDX-License-Identifier: MIT
 * [Keyboard]'s XKB names as Keyboard Properties edits them: the layout
 * and variant lists ("us,de" with ",nodeadkeys") as pairs, and the
 * options list ("grp:alt_shift_toggle,caps:ctrl_modifier").
 */
#ifndef XD_KEYBOARD_LAYOUTS_H
#define XD_KEYBOARD_LAYOUTS_H

#include <stddef.h>

/* XKB has four groups, so at most four layouts at once. */
enum { XD_KBD_LAYOUTS_MAX = 4 };

typedef struct xd_kbd_layout {
    char name[32];    /* e.g. "de" */
    char variant[64]; /* e.g. "nodeadkeys"; "" = the layout's default */
} xd_kbd_layout;

/* Splits the comma-separated lists into pairs (variants may be fewer or
 * empty). Returns how many, at most XD_KBD_LAYOUTS_MAX; empty names are
 * skipped. */
size_t xd_kbd_layouts_parse(const char *layouts, const char *variants,
                            xd_kbd_layout out[XD_KBD_LAYOUTS_MAX]);
/* Joins pairs back into lists; variants is "" when every one is the
 * default. False (and empty strings) if they don't fit. */
bool xd_kbd_layouts_format(const xd_kbd_layout *l, size_t n, char *layouts, size_t lcap,
                           char *variants, size_t vcap);

/* The first option in `options` from the group `group` ("grp" matches
 * "grp:alt_shift_toggle"), newly allocated (g_free); nullptr if there is none. */
char *xd_kbd_option_in_group(const char *options, const char *group);
/* `options` with `option` added (on) or removed, newly allocated (g_free). */
char *xd_kbd_option_set(const char *options, const char *option, bool on);
/* `options` without any option of `group`, newly allocated (g_free). */
char *xd_kbd_options_clear_group(const char *options, const char *group);

#endif
