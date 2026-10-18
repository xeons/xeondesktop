/* SPDX-License-Identifier: MIT */
#include <json-glib/json-glib.h>
#include <math.h>

#include "qtpalette.h"

/* One palette role's colours in Qt's terms: the active and inactive
 * colour, and the disabled one. */
typedef struct qt_role {
    const char *role;
    xd_rgba normal, disabled;
} qt_role;

static unsigned channel(double v) {
    double c = round(v * 255.0);
    return c <= 0 ? 0u : c >= 255 ? 255u : (unsigned)c;
}

/* A colour as Qt saves a fixed brush. */
static void add_entry(JsonBuilder *b, const char *group, xd_rgba c) {
    char hex[32];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", channel(c.r), channel(c.g), channel(c.b));
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "ColorGroup");
    json_builder_add_string_value(b, group);
    json_builder_set_member_name(b, "ColorScheme");
    json_builder_add_string_value(b, "Unknown"); /* light and dark alike */
    json_builder_set_member_name(b, "SourceType");
    json_builder_add_string_value(b, "Fixed");
    json_builder_set_member_name(b, "SourceData");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "FixedBrush");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "Color");
    json_builder_add_string_value(b, hex);
    json_builder_set_member_name(b, "Height");
    json_builder_add_int_value(b, 0);
    json_builder_set_member_name(b, "Width");
    json_builder_add_int_value(b, 0);
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
}

static void add_palette(JsonBuilder *b, const char *name, const qt_role *roles, size_t n) {
    json_builder_set_member_name(b, name);
    json_builder_begin_object(b);
    for (size_t i = 0; i < n; i++) {
        json_builder_set_member_name(b, roles[i].role);
        json_builder_begin_array(b);
        add_entry(b, "Active", roles[i].normal);
        add_entry(b, "Inactive", roles[i].normal);
        add_entry(b, "Disabled", roles[i].disabled);
        json_builder_end_array(b);
    }
    json_builder_end_object(b);
}

void xd_qt_palette_json(const xd_theme *t, xd_strbuf *out) {
    xd_toolkit_colors c;
    xd_theme_toolkit_colors(t, &c);
    xd_rgba face = c.face.a > 0 ? c.face : c.bg;
    /* The 3D edges from the scheme, as Windows draws classic controls in
     * every style: Qt's Windows style uses them for its bevels, Fusion
     * for its outlines and shadows. */
    xd_rgba hilight = xd_sys(t, XD_COLOR_BUTTON_HILIGHT);
    xd_rgba light = xd_sys(t, XD_COLOR_BUTTON_LIGHT);
    xd_rgba shadow = xd_sys(t, XD_COLOR_BUTTON_SHADOW);
    xd_rgba dkshadow = xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW);
    const qt_role system[] = {
        {"Window", c.bg, c.bg},
        {"WindowText", c.fg, c.disabled},
        {"Base", c.base, c.bg}, /* a disabled field is grey, as in Windows */
        {"AlternateBase", c.base, c.bg},
        {"Text", c.text, c.disabled},
        {"PlaceholderText", c.disabled, c.disabled},
        {"Button", face, face},
        {"ButtonText", c.fg, c.disabled},
        {"BrightText", xd_rgba_hex(0xFFFFFF), xd_rgba_hex(0xFFFFFF)},
        {"Highlight", c.selected_bg, face},
        {"HighlightedText", c.selected_fg, c.disabled},
        {"Light", hilight, hilight},
        {"Midlight", light, light},
        {"Mid", shadow, shadow},
        {"Dark", shadow, shadow},
        {"Shadow", dkshadow, dkshadow},
        {"ToolTipBase", c.tooltip_bg, c.tooltip_bg},
        {"ToolTipText", c.tooltip_fg, c.tooltip_fg},
        {"Link", xd_rgba_hex(0x0000FF), c.disabled},
    };
    /* XP's menus are white under Luna; the bar keeps the dialog colour. */
    const qt_role menu[] = {
        {"Window", c.menu_bg, c.menu_bg},       {"Base", c.menu_bg, c.menu_bg},
        {"Button", c.menu_bg, c.menu_bg},       {"WindowText", c.menu_fg, c.disabled},
        {"Text", c.menu_fg, c.disabled},        {"ButtonText", c.menu_fg, c.disabled},
        {"Highlight", c.menu_hot, c.menu_bg},   {"HighlightedText", c.menu_hot_fg, c.disabled},
    };
    const qt_role menubar[] = {
        {"Window", c.menubar_bg, c.menubar_bg},
        {"Button", c.menubar_bg, c.menubar_bg},
        {"Highlight", c.menu_hot, c.menubar_bg},
        {"HighlightedText", c.menu_hot_fg, c.disabled},
    };

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "QtGtk3Palettes");
    json_builder_begin_object(b);
    add_palette(b, "SystemPalette", system, G_N_ELEMENTS(system));
    add_palette(b, "MenuPalette", menu, G_N_ELEMENTS(menu));
    add_palette(b, "MenuBarPalette", menubar, G_N_ELEMENTS(menubar));
    json_builder_end_object(b);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, true);
    gsize len = 0;
    char *text = json_generator_to_data(gen, &len);
    xd_strbuf_clear(out);
    xd_strbuf_append(out, (xd_str){text, len});
    g_free(text);
    json_node_unref(root);
    g_object_unref(gen);
    g_object_unref(b);
}
