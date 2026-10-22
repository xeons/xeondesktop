/* SPDX-License-Identifier: MIT */
#include <json-glib/json-glib.h>
#include <math.h>

#include "qtpalette.h"

#include "../theme/gtkpkg.h"

/* One palette role's colours in Qt's terms: the active and inactive
 * colour, and the disabled one. */
typedef struct qt_role {
    const char *role;
    xd_rgba normal, disabled;
} qt_role;

static unsigned channel(double value) {
    double scaled = round(value * 255.0);
    return scaled <= 0 ? 0u : scaled >= 255 ? 255u : (unsigned)scaled;
}

/* A colour as Qt saves a fixed brush. */
static void add_entry(JsonBuilder *builder, const char *group, xd_rgba color) {
    char hex[32];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", channel(color.r), channel(color.g),
             channel(color.b));
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "ColorGroup");
    json_builder_add_string_value(builder, group);
    json_builder_set_member_name(builder, "ColorScheme");
    json_builder_add_string_value(builder, "Unknown"); /* light and dark alike */
    json_builder_set_member_name(builder, "SourceType");
    json_builder_add_string_value(builder, "Fixed");
    json_builder_set_member_name(builder, "SourceData");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "FixedBrush");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "Color");
    json_builder_add_string_value(builder, hex);
    json_builder_set_member_name(builder, "Height");
    json_builder_add_int_value(builder, 0);
    json_builder_set_member_name(builder, "Width");
    json_builder_add_int_value(builder, 0);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
}

static void add_palette(JsonBuilder *builder, const char *name, const qt_role *roles,
                        size_t count) {
    json_builder_set_member_name(builder, name);
    json_builder_begin_object(builder);
    for (size_t i = 0; i < count; i++) {
        json_builder_set_member_name(builder, roles[i].role);
        json_builder_begin_array(builder);
        add_entry(builder, "Active", roles[i].normal);
        add_entry(builder, "Inactive", roles[i].normal);
        add_entry(builder, "Disabled", roles[i].disabled);
        json_builder_end_array(builder);
    }
    json_builder_end_object(builder);
}

void xd_qt_palette_json(const xd_theme *theme, xd_strbuf *out) {
    xd_toolkit_colors toolkit_colors;
    xd_theme_toolkit_colors(theme, &toolkit_colors);
    xd_gtk_pkg_toolkit_colors(theme, &toolkit_colors); /* a theme package's colours, as GTK's */
    xd_rgba face = toolkit_colors.face.a > 0 ? toolkit_colors.face : toolkit_colors.bg;
    /* The 3D edges from the scheme, as Windows draws classic controls in
     * every style: Qt's Windows style uses them for its bevels, Fusion
     * for its outlines and shadows. */
    xd_rgba hilight = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT);
    xd_rgba light = xd_sys(theme, XD_COLOR_BUTTON_LIGHT);
    xd_rgba shadow = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
    xd_rgba dkshadow = xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW);
    const qt_role system[] = {
        {"Window", toolkit_colors.bg, toolkit_colors.bg},
        {"WindowText", toolkit_colors.fg, toolkit_colors.disabled},
        {"Base", toolkit_colors.base,
         toolkit_colors.bg}, /* a disabled field is grey, as in Windows */
        {"AlternateBase", toolkit_colors.base, toolkit_colors.bg},
        {"Text", toolkit_colors.text, toolkit_colors.disabled},
        {"PlaceholderText", toolkit_colors.disabled, toolkit_colors.disabled},
        {"Button", face, face},
        {"ButtonText", toolkit_colors.fg, toolkit_colors.disabled},
        {"BrightText", xd_rgba_hex(0xFFFFFF), xd_rgba_hex(0xFFFFFF)},
        {"Highlight", toolkit_colors.selected_bg, face},
        {"HighlightedText", toolkit_colors.selected_fg, toolkit_colors.disabled},
        {"Light", hilight, hilight},
        {"Midlight", light, light},
        {"Mid", shadow, shadow},
        {"Dark", shadow, shadow},
        {"Shadow", dkshadow, dkshadow},
        {"ToolTipBase", toolkit_colors.tooltip_bg, toolkit_colors.tooltip_bg},
        {"ToolTipText", toolkit_colors.tooltip_fg, toolkit_colors.tooltip_fg},
        {"Link", xd_rgba_hex(0x0000FF), toolkit_colors.disabled},
    };
    /* XP's menus are white under Luna; the bar keeps the dialog colour. */
    const qt_role menu[] = {
        {"Window", toolkit_colors.menu_bg, toolkit_colors.menu_bg},
        {"Base", toolkit_colors.menu_bg, toolkit_colors.menu_bg},
        {"Button", toolkit_colors.menu_bg, toolkit_colors.menu_bg},
        {"WindowText", toolkit_colors.menu_fg, toolkit_colors.disabled},
        {"Text", toolkit_colors.menu_fg, toolkit_colors.disabled},
        {"ButtonText", toolkit_colors.menu_fg, toolkit_colors.disabled},
        {"Highlight", toolkit_colors.menu_hot, toolkit_colors.menu_bg},
        {"HighlightedText", toolkit_colors.menu_hot_fg, toolkit_colors.disabled},
    };
    const qt_role menubar[] = {
        {"Window", toolkit_colors.menubar_bg, toolkit_colors.menubar_bg},
        {"Button", toolkit_colors.menubar_bg, toolkit_colors.menubar_bg},
        {"Highlight", toolkit_colors.menu_hot, toolkit_colors.menubar_bg},
        {"HighlightedText", toolkit_colors.menu_hot_fg, toolkit_colors.disabled},
    };

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "QtGtk3Palettes");
    json_builder_begin_object(builder);
    add_palette(builder, "SystemPalette", system, G_N_ELEMENTS(system));
    add_palette(builder, "MenuPalette", menu, G_N_ELEMENTS(menu));
    add_palette(builder, "MenuBarPalette", menubar, G_N_ELEMENTS(menubar));
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, true);
    gsize len = 0;
    char *text = json_generator_to_data(gen, &len);
    xd_strbuf_clear(out);
    xd_strbuf_append(out, (xd_str){text, len});
    g_free(text);
    json_node_unref(root);
    g_object_unref(gen);
    g_object_unref(builder);
}
