/* SPDX-License-Identifier: MIT
 * Desktop Effects: the table of effects and their settings.
 *
 * Every effect is described once, in a file of its own under
 * src/settings/effects/ and a line in effects_list.h: its id, name,
 * category, description, icon and options. The compositor reads the
 * settings from it, and Desktop Effects (xd-effects) builds every page
 * from it, as CompizConfig Settings Manager did from plugin metadata.
 *
 * The settings are in ~/.config/xeondesktop/effects.ini, a GKeyFile with
 * one group per effect (its id): Enabled= and the options that differ
 * from their defaults. Saving keeps keys and groups this build doesn't
 * know, so an older Desktop Effects can't drop a newer one's settings.
 * Every effect is off by default: out of the box the desktop behaves as
 * Windows does.
 *
 * The compositor tells Desktop Effects which effects it can draw in
 * $XDG_RUNTIME_DIR/xeondesktop/effects-$WAYLAND_DISPLAY.ini (the
 * "support file"): the OpenGL effects need xd-compositor-fx drawing with
 * OpenGL ES.
 */
#ifndef XD_SETTINGS_EFFECTS_H
#define XD_SETTINGS_EFFECTS_H

#include <stddef.h>
#include <stdint.h>

/* The side bar's categories, as CCSM had them. */
typedef enum xd_effect_category : uint8_t {
    XD_EFFECT_CATEGORY_EFFECTS,
    XD_EFFECT_CATEGORY_WINDOW_MANAGEMENT,
    XD_EFFECT_CATEGORY_DESKTOP,
    XD_EFFECT_CATEGORY_ACCESSIBILITY,
    XD_EFFECT_CATEGORY_UTILITY,
    XD_EFFECT_CATEGORY_COUNT,
} xd_effect_category;

typedef enum xd_effect_option_type : uint8_t {
    XD_EFFECT_OPTION_BOOL,
    XD_EFFECT_OPTION_INT,    /* min..max, in steps of step */
    XD_EFFECT_OPTION_DOUBLE, /* min..max, shown with `digits` decimals */
    XD_EFFECT_OPTION_COLOR,  /* "#RRGGBBAA" */
    XD_EFFECT_OPTION_CHOICE, /* one of choice_names (stored), shown as choice_labels */
    XD_EFFECT_OPTION_KEY,    /* a key binding, GTK's syntax: "<Super>Tab"; "" is none */
    XD_EFFECT_OPTION_BUTTON, /* a mouse button binding: "<Super>Button1"; "" is none */
    XD_EFFECT_OPTION_EDGE,   /* screen edges and corners: "TopLeft;Bottom"; "" is none */
    XD_EFFECT_OPTION_MATCH,  /* which windows (xd_effects_match_window); "" is every one */
} xd_effect_option_type;

/* XD_EFFECT_OPTION_EDGE's values: a set of these bits. */
typedef enum xd_screen_edge : uint8_t {
    XD_SCREEN_EDGE_LEFT = 1 << 0,
    XD_SCREEN_EDGE_RIGHT = 1 << 1,
    XD_SCREEN_EDGE_TOP = 1 << 2,
    XD_SCREEN_EDGE_BOTTOM = 1 << 3,
    XD_SCREEN_EDGE_TOP_LEFT = 1 << 4,
    XD_SCREEN_EDGE_TOP_RIGHT = 1 << 5,
    XD_SCREEN_EDGE_BOTTOM_LEFT = 1 << 6,
    XD_SCREEN_EDGE_BOTTOM_RIGHT = 1 << 7,
} xd_screen_edge;

enum { XD_SCREEN_EDGES = 8 };

typedef struct xd_effect_option {
    const char *key;         /* in effects.ini: "SpringConstant" */
    const char *label;       /* as the page shows it: "Spring constant" */
    const char *description; /* its tooltip, or nullptr */
    const char *group;       /* the tab it's on; nullptr is "General" */
    xd_effect_option_type type;
    double min, max, step; /* INT and DOUBLE */
    int digits;            /* DOUBLE */
    /* CHOICE: the names stored and the labels shown, nullptr-terminated,
     * of the same length. */
    const char *const *choice_names;
    const char *const *choice_labels;
    /* CHOICE, optional: for each choice, the id of the effect it needs
     * turned on (and drawable), or nullptr for none; as long as
     * choice_names. Until it's on the compositor uses another choice
     * (Animations' Add-On effects, as CCSM listed them once the
     * animationaddon plugin was on), and Desktop Effects greys it. */
    const char *const *choice_requires;
    const char *choice_fallback; /* that other choice's name: "zoom" */
    const char *default_value; /* in the file's syntax */
} xd_effect_option;

typedef struct xd_effect_info {
    const char *id;          /* its group in effects.ini: "WobblyWindows" */
    const char *name;        /* "Wobbly Windows" */
    const char *description; /* one line, under its name on its page */
    const char *icon;        /* from the icon theme */
    xd_effect_category category;
    /* Drawn with OpenGL ES by xd-compositor-fx (src/compositor/effects);
     * unavailable in the plain build and in software rendering. */
    bool needs_opengl;
    /* Settings rather than an effect, as CCSM's General Options: always
     * in use, with no Enabled= of its own (xd_effects_is_enabled says
     * true) and no check box in Desktop Effects. */
    bool always_on;
    const xd_effect_option *options;
    size_t option_count;
} xd_effect_info;

/* An option's value; which member by its type. */
typedef union xd_effect_value {
    bool boolean;   /* BOOL */
    int integer;    /* INT */
    double number;  /* DOUBLE */
    uint32_t color; /* COLOR: 0xRRGGBBAA */
    int choice;     /* CHOICE: the index in choice_names */
    uint32_t edges; /* EDGE: xd_screen_edge bits */
    char *text;     /* KEY, BUTTON, MATCH: owned by the settings, never nullptr */
} xd_effect_value;

/* The built-in table: each effect's info, xd_effect_<name>. */
#define XD_EFFECT(name) extern const xd_effect_info xd_effect_##name;
#include "effects_list.h"
#undef XD_EFFECT

size_t xd_effects_get_builtin_count(void);
const xd_effect_info *const *xd_effects_get_builtin_table(void);
const char *xd_effects_get_category_name(xd_effect_category category);
/* Its group, or "General". */
const char *xd_effects_get_option_group(const xd_effect_option *option);

/* Settings for every effect of a table, at their defaults. The table
 * outlives them. */
typedef struct xd_effects_settings xd_effects_settings;
xd_effects_settings *xd_effects_create_settings(void); /* the built-in table */
xd_effects_settings *xd_effects_create_settings_for(const xd_effect_info *const *table,
                                                    size_t count);
void xd_effects_destroy_settings(xd_effects_settings *settings);

size_t xd_effects_get_count(const xd_effects_settings *settings);
const xd_effect_info *xd_effects_get_info(const xd_effects_settings *settings, size_t effect);
/* The effect with that id, and its option with that key. */
bool xd_effects_find(const xd_effects_settings *settings, const char *id, size_t *effect);
bool xd_effects_find_option(const xd_effect_info *info, const char *key, size_t *option);

/* ~/.config/xeondesktop/effects.ini (g_free it). */
char *xd_effects_get_default_path(void);
/* Reads a file over the defaults: values out of range are clamped, those
 * that don't parse stay at their defaults. A missing file leaves every
 * default. */
void xd_effects_load_file(xd_effects_settings *settings, const char *path);
void xd_effects_load(xd_effects_settings *settings);
/* Writes atomically, keeping what's in the file now that the table
 * doesn't know. */
[[nodiscard]] bool xd_effects_save_file(const xd_effects_settings *settings, const char *path);
[[nodiscard]] bool xd_effects_save(const xd_effects_settings *settings);

bool xd_effects_is_enabled(const xd_effects_settings *settings, size_t effect);
void xd_effects_set_enabled(xd_effects_settings *settings, size_t effect, bool enabled);
const xd_effect_value *xd_effects_get_value(const xd_effects_settings *settings, size_t effect,
                                            size_t option);
/* Clamped to the option's range; true if the value changed. */
bool xd_effects_set_value(xd_effects_settings *settings, size_t effect, size_t option,
                          const xd_effect_value *value);
bool xd_effects_is_default(const xd_effects_settings *settings, size_t effect, size_t option);
void xd_effects_reset_value(xd_effects_settings *settings, size_t effect, size_t option);
/* Every effect off and every option at its default. */
void xd_effects_reset_all(xd_effects_settings *settings);

/* By key, for the compositor's effects: the default's type when the key
 * is unknown (a programming error, which XD_ASSERT catches). */
bool xd_effects_get_bool(const xd_effects_settings *settings, size_t effect, const char *key);
int xd_effects_get_int(const xd_effects_settings *settings, size_t effect, const char *key);
double xd_effects_get_double(const xd_effects_settings *settings, size_t effect,
                             const char *key);
uint32_t xd_effects_get_color(const xd_effects_settings *settings, size_t effect,
                              const char *key);
int xd_effects_get_choice(const xd_effects_settings *settings, size_t effect, const char *key);
uint32_t xd_effects_get_edges(const xd_effects_settings *settings, size_t effect,
                              const char *key);
const char *xd_effects_get_text(const xd_effects_settings *settings, size_t effect,
                                const char *key);
/* A CHOICE option's value as stored: its choice_names entry. */
const char *xd_effects_get_choice_name(const xd_effects_settings *settings, size_t effect,
                                       const char *key);
/* The id of the effect a choice needs (choice_requires), or nullptr. */
const char *xd_effects_get_choice_requirement(const xd_effect_option *option, int choice);
/* Whether a choice can be used now: it needs no other effect, or the one
 * it needs is on and can be drawn (`opengl`: OpenGL effects can be). */
bool xd_effects_is_choice_available(const xd_effects_settings *settings, size_t effect,
                                    size_t option, int choice, bool opengl);

/* A value from the file's syntax, clamped; false if it doesn't parse.
 * A text value is newly allocated (g_free). */
bool xd_effects_parse_value(const xd_effect_option *option, const char *text,
                            xd_effect_value *out);
/* In the file's syntax (g_free it). */
char *xd_effects_format_value(const xd_effect_option *option, const xd_effect_value *value);

/* Desktop Effects' Preferences: warn when an effect turned on, or a
 * binding set, clashes with another effect's ([Preferences]
 * WarnConflicts, on by default). */
bool xd_effects_get_warn_conflicts(const xd_effects_settings *settings);
void xd_effects_set_warn_conflicts(xd_effects_settings *settings, bool warn);

/* ---- conflicts ------------------------------------------------------------------ */

/* Key bindings, mouse bindings and screen edges: modifiers as these bits
 * and the key ("Tab", "Button1"), from GTK's accelerator syntax, with
 * Mod4 as Super, Mod1 as Alt and Primary as Control. False for "" or
 * anything else that isn't a binding. */
typedef enum xd_binding_modifier : uint8_t {
    XD_BINDING_SHIFT = 1 << 0,
    XD_BINDING_CONTROL = 1 << 1,
    XD_BINDING_ALT = 1 << 2,
    XD_BINDING_SUPER = 1 << 3,
} xd_binding_modifier;

bool xd_effects_parse_binding(const char *text, uint32_t *modifiers, char *key, size_t cap);
/* Whether two values of options of this type would trigger together. */
bool xd_effects_bindings_overlap(xd_effect_option_type type, const xd_effect_value *left,
                                 const xd_effect_value *right);

typedef struct xd_effect_conflict {
    size_t option;       /* of the effect asked about */
    size_t other_effect; /* an effect turned on */
    size_t other_option;
} xd_effect_conflict;

/* Where `effect`'s bindings and edges clash with those of the other
 * effects turned on; returns how many (up to cap are stored). */
size_t xd_effects_find_conflicts(const xd_effects_settings *settings, size_t effect,
                                 xd_effect_conflict *out, size_t cap);
/* A binding or edge value as the user reads it: "Super+Tab", "Top left
 * corner, Bottom edge", "None" (g_free it). */
char *xd_effects_describe_value(const xd_effect_option *option, const xd_effect_value *value);

/* ---- window matches --------------------------------------------------------------- */

/* Whether a window matches XD_EFFECT_OPTION_MATCH's syntax: conditions
 * joined by "&" (and) into terms joined by "|" (or), each condition
 * key=pattern with the glob patterns of g_pattern_match_simple, "!" in
 * front to negate it. Keys: app_id, title, type (normal or dialog). ""
 * or "any" match every window. Spaces around the operators don't
 * count; the keys and the type are case-insensitive. */
bool xd_effects_match_window(const char *match, const char *app_id, const char *title,
                             bool is_dialog);

/* ---- watching and the support file ---------------------------------------------- */

/* Watches the folder of effects.ini; poll the fd and call
 * xd_effects_has_changed when it's readable (it drains it). -1 on error. */
int xd_effects_watch(void);
bool xd_effects_has_changed(int fd);

typedef struct xd_effects_support {
    bool opengl;       /* OpenGL effects can be drawn */
    char reason[256];  /* why not, for Desktop Effects to show */
    char renderer[128]; /* the GL renderer, for the log and the panel */
} xd_effects_support;

/* For the compositor on `display` (its WAYLAND_DISPLAY); Desktop Effects
 * reads its own WAYLAND_DISPLAY's. False when there's none (no
 * compositor of ours running). */
[[nodiscard]] bool xd_effects_write_support(const char *display,
                                            const xd_effects_support *support);
bool xd_effects_read_support(const char *display, xd_effects_support *support);
void xd_effects_remove_support(const char *display);

#endif
