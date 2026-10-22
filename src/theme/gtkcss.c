/* SPDX-License-Identifier: MIT
 * GTK3 theme generated from the active visual style and colour scheme, so
 * GTK apps (xd-files, Control Panel applets, third-party apps) match the
 * shell: Windows 2000 3D bevels for Classic, XP's gradients and flat
 * borders for Luna and Whistler.
 */
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "gtkpkg.h"
#include "style.h"

/* "#rrggbb" for opaque colours, rgba() otherwise. */
typedef struct hexbuf {
    char s[64];
} hexbuf;

static unsigned channel(double value) {
    double scaled = round(value * 255.0);
    return scaled <= 0 ? 0u : scaled >= 255 ? 255u : (unsigned)scaled;
}

static hexbuf hex(xd_rgba color) {
    hexbuf out;
    unsigned red = channel(color.r);
    unsigned green = channel(color.g);
    unsigned blue = channel(color.b);
    double av = round(color.a * 1000.0);
    unsigned alpha = av <= 0 ? 0u : av >= 1000 ? 1000u : (unsigned)av;
    if (alpha >= 1000)
        snprintf(out.s, sizeof(out.s), "#%02x%02x%02x", red, green, blue);
    else
        snprintf(out.s, sizeof(out.s), "rgba(%u,%u,%u,0.%03u)", red, green, blue, alpha);
    return out;
}

#define H(c) (hex(c).s)
/* One of XP's own control colours (0xRRGGBB) as the theme's Luna variant
 * draws it: Dark's dark, the others' as they are. */
#define XP(rgb) (hex(xd_rgba_hex(xd_luna_control_rgb(theme, rgb))).s)

/* The scheme's message font as a CSS font-family list. */
static void font_rules(const xd_theme *theme, xd_strbuf *out) {
    const xd_font_spec *font = &theme->scheme.fonts[XD_FONT_MESSAGE];
    xd_strbuf_append(out, XD_STR("* {\n  font-family: "));
    xd_str rest = xd_str_from_cstr(font->family);
    bool first = true;
    while (rest.len > 0) {
        size_t comma = rest.len;
        bool more = xd_str_find_char(rest, ',', &comma);
        xd_str name = xd_str_trim(xd_str_sub(rest, 0, comma));
        if (name.len > 0) {
            if (!first)
                xd_strbuf_append(out, XD_STR(", "));
            first = false;
            bool generic = xd_str_eq(name, XD_STR("sans-serif")) ||
                           xd_str_eq(name, XD_STR("serif")) ||
                           xd_str_eq(name, XD_STR("monospace"));
            /* Quotes are dropped from names so they can't break out of the
             * string; scheme files are trusted, but stay safe anyway. */
            if (!generic)
                xd_strbuf_append_char(out, '"');
            for (size_t i = 0; i < name.len; i++) {
                if (name.ptr[i] != '"' && name.ptr[i] != '\\')
                    xd_strbuf_append_char(out, name.ptr[i]);
            }
            if (!generic)
                xd_strbuf_append_char(out, '"');
        }
        rest = more ? xd_str_sub(rest, comma + 1, rest.len) : XD_STR("");
    }
    /* Whole pixels, as Windows rounds them: 8pt is 11px (GTK scales px
     * itself on HiDPI). */
    xd_strbuf_appendf(out, ";\n  font-size: %dpx;\n}\n\n",
                      (int)lround(font->size_pt * 96.0 / 72.0));
}

static void defines(const xd_toolkit_colors *toolkit_colors, xd_strbuf *buffer) {
    /* Named colours many GTK apps (and our folder view) look up. */
    xd_strbuf_appendf(
        buffer,
        "@define-color theme_bg_color %s;\n@define-color theme_fg_color %s;\n"
        "@define-color theme_base_color %s;\n@define-color theme_text_color %s;\n"
        "@define-color theme_selected_bg_color %s;\n"
        "@define-color theme_selected_fg_color %s;\n"
        "@define-color insensitive_fg_color %s;\n"
        "@define-color theme_unfocused_bg_color %s;\n"
        "@define-color theme_unfocused_fg_color %s;\n"
        "@define-color theme_unfocused_base_color %s;\n"
        "@define-color theme_unfocused_text_color %s;\n"
        "@define-color theme_unfocused_selected_bg_color %s;\n"
        "@define-color theme_unfocused_selected_fg_color %s;\n"
        "@define-color borders %s;\n@define-color unfocused_borders %s;\n"
        "@define-color tooltip_bg_color %s;\n@define-color tooltip_fg_color %s;\n\n",
        H(toolkit_colors->bg), H(toolkit_colors->fg), H(toolkit_colors->base),
        H(toolkit_colors->text), H(toolkit_colors->selected_bg), H(toolkit_colors->selected_fg),
        H(toolkit_colors->disabled), H(toolkit_colors->bg), H(toolkit_colors->fg),
        H(toolkit_colors->base), H(toolkit_colors->text), H(toolkit_colors->selected_bg),
        H(toolkit_colors->selected_fg),
        H(toolkit_colors->classic ? toolkit_colors->shadow : toolkit_colors->field_border),
        H(toolkit_colors->classic ? toolkit_colors->shadow : toolkit_colors->field_border),
        H(toolkit_colors->tooltip_bg), H(toolkit_colors->tooltip_fg));
}

/* Rules shared by both looks. */
static void common(const xd_toolkit_colors *toolkit_colors, xd_strbuf *buffer) {
    xd_strbuf_appendf(
        buffer,
        "window, .background, dialog, messagedialog, assistant {\n"
        "  background-color: %s;\n  color: %s;\n}\n"
        "label:disabled, *:disabled { color: %s; }\n"
        "selection, *:selected, *:selected:focus, row:selected, treeview.view:selected,\n"
        "entry selection, textview text selection, label selection {\n"
        "  background-color: %s;\n  color: %s;\n}\n"
        "textview text, iconview, treeview.view, list, list row, flowbox, viewport.view {\n"
        "  background-color: %s;\n  color: %s;\n}\n"
        "row:hover:not(:selected) { background-color: transparent; }\n"
        "tooltip, tooltip.background {\n  background-color: %s;\n  color: %s;\n"
        "  border: 1px solid #000000;\n  border-radius: 0;\n  padding: 1px 3px;\n"
        "  box-shadow: none;\n}\n"
        "tooltip * { background-color: transparent; color: %s; }\n"
        "decoration, window.csd decoration, window.solid-csd decoration {\n"
        "  box-shadow: none;\n  border-radius: 0;\n  margin: 0;\n}\n"
        "headerbar, .titlebar {\n  background-image: linear-gradient(to right, %s, %s);\n"
        "  color: %s;\n  border-radius: 0;\n  box-shadow: none;\n  min-height: 22px;\n"
        "  padding: 0 4px;\n}\n"
        "headerbar .title, .titlebar .title { font-weight: bold; color: %s; }\n"
        "progressbar progress {\n  background-image: none;\n  background-color: %s;\n"
        "  border-radius: 0;\n  border: none;\n}\n"
        "progressbar trough { min-height: 14px; }\n"
        "*:focus { outline-style: dotted; outline-width: 1px; outline-offset: -3px;\n"
        "  outline-color: %s; -gtk-outline-radius: 0; }\n"
        "link, *:link { color: #0000ff; }\n\n",
        H(toolkit_colors->bg), H(toolkit_colors->fg), H(toolkit_colors->disabled),
        H(toolkit_colors->selected_bg), H(toolkit_colors->selected_fg), H(toolkit_colors->base),
        H(toolkit_colors->text), H(toolkit_colors->tooltip_bg), H(toolkit_colors->tooltip_fg),
        H(toolkit_colors->tooltip_fg), H(toolkit_colors->title_bg), H(toolkit_colors->title_bg2),
        H(toolkit_colors->title_fg), H(toolkit_colors->title_fg), H(toolkit_colors->selected_bg),
        H(toolkit_colors->fg));
}

/* ---- Classic (Windows 2000) --------------------------------------------------------- */

static void classic(const xd_toolkit_colors *toolkit_colors, xd_strbuf *buffer) {
    hexbuf fc = hex(toolkit_colors->face);
    const char *face = fc.s; /* fc must outlive every use of face */
    /* Raised: light/dkshadow outside, hilight/shadow inside. Sunken is the
     * reverse. */
    hexbuf hi = hex(toolkit_colors->hilight);
    hexbuf li = hex(toolkit_colors->light);
    hexbuf sh = hex(toolkit_colors->shadow);
    hexbuf dk = hex(toolkit_colors->dkshadow);
    xd_rgba dither = xd_rgba_mix(toolkit_colors->scrollbar, toolkit_colors->hilight, 0.5);

    xd_strbuf_appendf(
        buffer,
        /* Push buttons */
        "button {\n  background-image: none;\n  background-color: %s;\n  color: %s;\n"
        "  border-style: solid;\n  border-width: 1px;\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset -1px -1px %s, inset 1px 1px %s;\n  border-radius: 0;\n"
        "  padding: 3px 8px;\n  min-height: 0;\n  text-shadow: none;\n"
        "  -gtk-icon-shadow: none;\n}\n"
        "button:hover { background-color: %s; }\n"
        "button:active, button:checked {\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n"
        "  padding: 4px 7px 2px 9px;\n}\n"
        "button:checked { background-color: %s; }\n"
        "button:disabled label { color: %s; text-shadow: 1px 1px %s; }\n"
        /* The default button's black frame is its outermost pixel, inside
         * Windows' 75x23, with the usual bevel within it. (GTK paints the
         * later shadows over the earlier ones: the inner pair first.) */
        "button.default { border-color: %s;\n"
        "  box-shadow: inset 2px 2px %s, inset -2px -2px %s, inset 1px 1px %s,\n"
        "    inset -1px -1px %s; }\n"
        /* Flat buttons: toolbars, combo arrows inside entries */
        "toolbar button, button.flat, button.image-button.flat {\n"
        "  background-color: transparent;\n  border-color: transparent;\n"
        "  box-shadow: none;\n  padding: 2px 4px;\n}\n"
        "toolbar button:hover, button.flat:hover { border-color: %s %s %s %s; }\n"
        "toolbar button:active, toolbar button:checked, button.flat:active,\n"
        "button.flat:checked { border-color: %s %s %s %s; box-shadow: none; }\n"
        "toolbar button:checked, button.flat:checked { background-color: %s; }\n"
        "toolbar { background-color: %s; padding: 2px; border-bottom: 1px solid %s;\n"
        "  box-shadow: 0 1px %s; }\n"
        /* Text fields */
        "entry, spinbutton, textview.view, .view text {\n  border-radius: 0;\n}\n"
        "entry, spinbutton {\n  background-color: %s;\n  color: %s;\n"
        "  border-style: solid;\n  border-width: 1px;\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n  padding: 3px 4px;\n"
        "  min-height: 0;\n}\n"
        "entry:disabled, spinbutton:disabled { background-color: %s; }\n"
        "scrolledwindow.frame, frame > border, .frame, progressbar trough {\n"
        "  border-style: solid;\n"
        "  border-width: 1px;\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n  border-radius: 0;\n}\n"
        /* Check boxes and radio buttons */
        "check, radio {\n  min-width: 13px;\n  min-height: 13px;\n"
        "  background-image: none;\n  background-color: %s;\n  color: %s;\n"
        "  border-style: solid;\n  border-width: 1px;\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n  border-radius: 0;\n"
        "  -gtk-icon-source: none;\n  margin: 0 4px 0 0;\n  padding: 0;\n}\n"
        "radio { border-radius: 50%%; }\n"
        "check:disabled, radio:disabled { background-color: %s; color: %s; }\n",
        face, H(toolkit_colors->fg), hi.s, dk.s, dk.s, hi.s, sh.s, li.s, face, dk.s, hi.s, hi.s,
        dk.s, sh.s, li.s, H(dither), H(toolkit_colors->disabled), hi.s, H(toolkit_colors->frame),
        li.s, sh.s, hi.s, dk.s, hi.s, sh.s, sh.s, hi.s, sh.s, hi.s, hi.s, sh.s, H(dither), face,
        sh.s, hi.s, H(toolkit_colors->base), H(toolkit_colors->text), sh.s, hi.s, hi.s, sh.s, dk.s,
        li.s, face, sh.s, hi.s, hi.s, sh.s, dk.s, li.s, H(toolkit_colors->base),
        H(toolkit_colors->text), sh.s, hi.s, hi.s, sh.s, dk.s, li.s, face,
        H(toolkit_colors->disabled));

    xd_strbuf_appendf(
        buffer,
        /* Menus */
        "menubar { background-color: %s;\n  padding: 0;\n  box-shadow: none;\n"
        "  border: none;\n}\n"
        "menubar > menuitem { padding: 2px 7px; border: 1px solid transparent; }\n"
        "menubar > menuitem:hover { border-color: %s %s %s %s; background-color: transparent;\n"
        "  color: %s; }\n"
        "menu, .menu, .context-menu {\n  background-color: %s;\n  color: %s;\n"
        "  border-style: solid;\n  border-width: 1px;\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n  padding: 2px;\n"
        "  border-radius: 0;\n}\n"
        "menu menuitem { padding: 3px 20px 3px 4px;\n  min-height: 16px; }\n"
        "menu menuitem:hover { background-color: %s;\n  color: %s; }\n"
        "menu menuitem:disabled { color: %s; }\n"
        "menu separator { margin: 3px 1px;\n  min-height: 0;\n"
        "  border-top: 1px solid %s;\n  border-bottom: 1px solid %s;\n"
        "  background: none;\n}\n"
        /* Scroll bars: Windows 2000 always shows arrows and a dithered track */
        "scrollbar {\n  -GtkScrollbar-has-backward-stepper: true;\n"
        "  -GtkScrollbar-has-forward-stepper: true;\n  background-color: %s;\n"
        "  border: none;\n}\n"
        "scrollbar trough { background-color: %s; border: none; }\n"
        "scrollbar slider, scrollbar button {\n  min-width: 14px;\n  min-height: 14px;\n"
        "  background-color: %s;\n  border-style: solid;\n  border-width: 1px;\n"
        "  border-color: %s %s %s %s;\n  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n"
        "  border-radius: 0;\n  margin: 0;\n  padding: 0;\n  color: %s;\n}\n"
        "scrollbar button:active { border-color: %s; box-shadow: none; }\n"
        "scrollbar.vertical button.up { -gtk-icon-source: -gtk-icontheme(\"pan-up-symbolic\"); }\n"
        "scrollbar.vertical button.down { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-down-symbolic\"); }\n"
        "scrollbar.horizontal button.up { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-start-symbolic\"); }\n"
        "scrollbar.horizontal button.down { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-end-symbolic\"); }\n"
        /* Tabs */
        "notebook > header { background-color: %s; border: none; }\n"
        "notebook > header > tabs > tab {\n  padding: 2px 8px;\n  background-color: %s;\n"
        "  border-style: solid;\n  border-width: 1px 1px 0 1px;\n"
        "  border-color: %s %s %s %s;\n  box-shadow: inset 1px 1px %s, inset -1px 0 %s;\n"
        "  margin-top: 2px;\n  border-radius: 0;\n}\n"
        "notebook > header > tabs > tab:checked { margin-top: 0;\n  padding-bottom: 4px; }\n"
        "notebook > stack:not(:only-child) {\n  background-color: %s;\n"
        "  border-style: solid;\n  border-width: 1px;\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset -1px -1px %s, inset 1px 1px %s;\n}\n"
        /* Tree/list column headers, panes, separators, sliders */
        "treeview.view header button, columnview header button {\n"
        "  padding: 1px 6px;\n}\n"
        "paned > separator { background-color: %s; min-width: 4px; min-height: 4px;\n"
        "  background-image: none; }\n"
        "separator { background-color: %s; min-width: 1px; min-height: 1px; }\n"
        "scale trough { background-color: %s; min-height: 4px; min-width: 4px;\n"
        "  border-style: solid; border-width: 1px; border-color: %s %s %s %s;\n"
        "  border-radius: 0; }\n"
        "scale slider { min-width: 11px; min-height: 21px; background-color: %s;\n"
        "  border-style: solid; border-width: 1px; border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s; border-radius: 0;\n"
        "  margin: -9px 0; }\n"
        "scale highlight { background: none; }\n",
        H(toolkit_colors->menubar_bg), hi.s, sh.s, sh.s, hi.s, H(toolkit_colors->menu_fg),
        H(toolkit_colors->menu_bg), H(toolkit_colors->menu_fg), li.s, dk.s, dk.s, li.s, hi.s, sh.s,
        H(toolkit_colors->menu_hot), H(toolkit_colors->menu_hot_fg), H(toolkit_colors->disabled),
        sh.s, hi.s, H(dither), H(dither), face, li.s, dk.s, dk.s, li.s, hi.s, sh.s,
        H(toolkit_colors->fg), sh.s, face, face, hi.s, dk.s, dk.s, hi.s, li.s, sh.s, face, hi.s,
        dk.s, dk.s, hi.s, sh.s, li.s, face, sh.s, face, sh.s, hi.s, hi.s, sh.s, face, hi.s, dk.s,
        dk.s, hi.s, li.s, sh.s);

    /* Tabs: raised, slightly rounded, with a two-tone right edge; the page's
     * top edge belongs to the header so the selected tab can cover it and
     * join the page. Group boxes: a thin etched line with the caption set
     * in from the corner. Combo boxes: a sunken field with an arrow button. */
    xd_strbuf_appendf(
        buffer,
        "notebook > header { background-color: %s; border: none; border-bottom: 1px solid %s;\n"
        "  box-shadow: none; padding: 0 2px; }\n"
        "notebook > header > tabs > tab {\n  background-color: %s; background-image: none;\n"
        "  border-style: solid; border-width: 1px 1px 0 1px;\n"
        "  border-color: %s %s %s %s;\n  border-radius: 2px 2px 0 0;\n"
        "  box-shadow: inset -1px 0 %s%.0s;\n"
        "  margin: 2px 0 0 0; padding: 1px 6px 1px 6px; min-height: 0; outline-offset: -3px;\n}\n"
        /* The selected tab is 2px taller and wider on each side, over its
         * neighbours, and covers the page's top edge. GTK ignores negative
         * margins on tabs but draws the selected one last, so its edges
         * are drawn outside its box as shadows (GTK 3 paints them in
         * order, the last on top): Windows 2000's raised edge with its
         * rounded corner, 2px out from the box. */
        "notebook > header > tabs > tab:checked { margin: 2px 0 -1px 0;\n"
        "  padding: 0 6px 3px 6px; border-color: %s;\n"
        "  box-shadow: 1px -1px %s, -1px -1px %s, 0 -2px %s, 2px 0 %s, -2px 0 %s,\n"
        "    0 -1px %s, 1px 0 %s, -1px 0 %s; }\n"
        "notebook > stack:not(:only-child) {\n  border-style: solid; border-width: 0 1px 1px 1px;\n"
        "  border-color: %s %s %s %s;\n  box-shadow: inset -1px -1px %s, inset 1px 0 %s;\n}\n"
        "frame > border {\n  border-style: solid; border-width: 1px;\n"
        "  border-color: %s %s %s %s;\n  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n"
        "  border-radius: 0;\n}\n"
        /* GTK cuts the caption's margin out of the border, so the caption is
         * moved in by padding the frame and pulling the border back out. */
        "frame { padding-left: 6px; }\n"
        "frame > border { margin-left: -6px; }\n"
        "frame > border + label { margin: 0; padding: 0 2px; }\n"
        /* Status bar panes: a thin sunken edge. */
        "frame.xd-sunken { padding-left: 0; }\n"
        "frame.xd-sunken > border { margin-left: 0; border-color: %s %s %s %s; box-shadow: none; }\n"
        "combobox button.combo {\n  background-color: %s; color: %s;\n"
        "  border-style: solid; border-width: 1px; border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n  padding: 0 0 0 3px;\n}\n"
        "combobox button.combo:hover { background-color: %s; }\n"
        "combobox button.combo:active, combobox button.combo:checked {\n"
        "  border-color: %s %s %s %s; box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n"
        "  padding: 0 0 0 3px; background-color: %s;\n}\n"
        "combobox button.combo arrow {\n  -gtk-icon-source: -gtk-icontheme(\"pan-down-symbolic\");\n"
        "  min-width: 12px; min-height: 12px; margin: 2px; padding: 1px 1px;\n"
        "  background-color: %s; color: %s;\n  border-style: solid; border-width: 1px;\n"
        "  border-color: %s %s %s %s; box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n}\n"
        "combobox button.combo:checked arrow { border-color: %s; box-shadow: none; }\n",
        face, hi.s, face, hi.s, dk.s, dk.s, hi.s, sh.s, li.s, face, sh.s, hi.s, hi.s, dk.s, hi.s,
        face, sh.s, face, hi.s, dk.s, dk.s, hi.s, sh.s, li.s, sh.s, hi.s, hi.s, sh.s, hi.s, sh.s,
        sh.s, hi.s, hi.s, sh.s, H(toolkit_colors->base), H(toolkit_colors->text), sh.s, hi.s, hi.s,
        sh.s, dk.s, li.s, H(toolkit_colors->base), sh.s, hi.s, hi.s, sh.s, dk.s, li.s,
        H(toolkit_colors->base), face, H(toolkit_colors->fg), li.s, dk.s, dk.s, li.s, hi.s, sh.s,
        sh.s);
}

/* ---- Luna and Whistler (Windows XP) ----------------------------------------------- */

/* The selected tab, as libxdui draws it: 2px wider on each side, over its
 * neighbours (GTK ignores negative margins on tabs but draws the selected
 * one last, so the extra edges are shadows outside its box; GTK 3 paints
 * shadows in order, the last on top), 2px taller, its label 2px higher.
 * `edge` is the outline, `fill` the inside; `pad_bottom` takes the box a
 * pixel below the unselected tabs, over the page's top edge. */
static void flat_selected_tab(xd_strbuf *buffer, xd_rgba edge, xd_rgba fill, int pad_bottom) {
    hexbuf edge_hex = hex(edge);
    hexbuf fill_hex = hex(fill);
    xd_strbuf_appendf(buffer,
                      "notebook > header > tabs > tab:checked {\n"
                      "  margin: 2px 0 -1px 0; padding: 0 8px %dpx 8px; border: 1px solid %s;\n"
                      "  border-bottom: none; border-radius: 0;\n"
                      "  box-shadow: -2px -2px %s, 2px -2px %s, -2px 0 %s, 2px 0 %s,\n"
                      "    -1px -1px %s, 1px -1px %s, -1px 0 %s, 1px 0 %s;\n}\n",
                      pad_bottom, fill_hex.s, edge_hex.s, edge_hex.s, edge_hex.s, edge_hex.s,
                      fill_hex.s, fill_hex.s, fill_hex.s, fill_hex.s);
}

/* Luna's selected tab: as above, with XP's orange band (two light rows and
 * a darker one) across the top. */
static void luna_selected_tab(const xd_theme *theme, xd_strbuf *buffer, xd_rgba border,
                              xd_rgba hot) {
    hexbuf border_hex = hex(border);
    hexbuf hot_hex = hex(hot);
    xd_strbuf_appendf(buffer,
                      "notebook > header > tabs > tab:checked {\n"
                      "  margin: 2px 0 -1px 0; padding: 0 8px 5px 8px;\n"
                      "  border: 1px solid %s; border-top-color: %s;\n"
                      "  border-bottom: none; border-radius: 0;\n"
                      "  background-image: linear-gradient(%s, %s);\n"
                      "  box-shadow: -2px -2px %s, 2px -2px %s, -2px 0 %s, 2px 0 %s,\n"
                      "    -2px 1px %s, 2px 1px %s, -1px 1px %s, 1px 1px %s;\n}\n",
                      XP(0xFDFDFE), XP(0xE6A000), XP(0xFFFFFF), XP(0xFCFCFE), hot_hex.s, hot_hex.s,
                      XP(0xE6A000), XP(0xE6A000), border_hex.s, border_hex.s, XP(0xFDFDFE),
                      XP(0xFDFDFE));
}

static void modern(const xd_theme *theme, const xd_toolkit_colors *toolkit_colors, bool aero,
                   xd_strbuf *buffer) {
    hexbuf fb = hex(toolkit_colors->field_border);
    xd_strbuf_appendf(
        buffer,
        "button {\n  background-image: linear-gradient(%s, %s);\n  background-color: %s;\n"
        "  color: %s;\n  border: 1px solid %s;\n  border-radius: 3px;\n"
        "  padding: 3px 10px;\n  min-height: 0;\n  box-shadow: none;\n  text-shadow: none;\n"
        "  -gtk-icon-shadow: none;\n}\n"
        "button:hover { box-shadow: inset 0 0 0 2px %s; }\n"
        "button:active, button:checked {\n  background-image: linear-gradient(%s, %s);\n"
        "  box-shadow: none;\n}\n"
        "button:focus, button.default { box-shadow: inset 0 0 0 2px %s; }\n"
        "button:disabled { border-color: %s; background-image: none;\n"
        "  background-color: %s; }\n"
        "button:disabled label { color: %s; }\n"
        "toolbar button, button.flat {\n  background-image: none;\n"
        "  background-color: transparent;\n  border-color: transparent;\n"
        "  box-shadow: none;\n  padding: 2px 4px;\n}\n"
        "toolbar button:hover, button.flat:hover {\n  border-color: %s;\n"
        "  background-image: linear-gradient(%s, %s);\n  box-shadow: none;\n}\n"
        "toolbar button:active, toolbar button:checked, button.flat:active,\n"
        "button.flat:checked { border-color: %s;\n"
        "  background-image: linear-gradient(%s, %s); }\n"
        "toolbar { background-color: %s; padding: 2px;\n  border-bottom: 1px solid %s; }\n"
        "entry, spinbutton {\n  background-color: %s;\n  color: %s;\n"
        "  border: 1px solid %s;\n  border-radius: 0;\n  box-shadow: none;\n"
        "  padding: 3px 4px;\n  min-height: 0;\n}\n"
        "entry:disabled { background-color: %s; }\n"
        "scrolledwindow.frame, frame > border, .frame {\n  border: 1px solid %s;\n"
        "  border-radius: 0;\n  box-shadow: none;\n}\n"
        "progressbar trough {\n  background-color: %s;\n  border: 1px solid %s;\n"
        "  border-radius: 3px;\n  padding: 2px;\n}\n"
        "progressbar progress {\n  background-color: transparent;\n"
        "  background-image: repeating-linear-gradient(to right, #31c331 0px, #31c331 6px,\n"
        "    transparent 6px, transparent 8px);\n}\n"
        "check, radio {\n  min-width: 13px;\n  min-height: 13px;\n"
        "  background-image: linear-gradient(135deg, %s, %s);\n"
        "  border: 1px solid %s;\n  border-radius: 0;\n  box-shadow: none;\n"
        "  color: %s;\n  -gtk-icon-source: none;\n  margin: 0 4px 0 0;\n  padding: 0;\n}\n"
        "radio { border-radius: 50%%; }\n"
        "check:hover, radio:hover { box-shadow: inset 0 0 0 2px %s; }\n"
        "check:disabled, radio:disabled { border-color: %s; color: %s; }\n",
        H(toolkit_colors->button_top),
        H(toolkit_colors->button_bottom),
        H(toolkit_colors->button_bottom),
        H(toolkit_colors->fg),
        H(toolkit_colors->button_border),
        H(toolkit_colors->button_hot),
        H(toolkit_colors->button_bottom),
        H(toolkit_colors->button_top),
        H(toolkit_colors->button_focus),
        XP(0xC9C7BA),
        XP(0xF5F4EA),
        H(toolkit_colors->disabled),
        XP(0xCECEC3),
        XP(0xFFFFFF),
        XP(0xE5E5DE),
        XP(0x9D9D92),
        XP(0xE5E5DE),
        XP(0xF7F7F4),
        H(toolkit_colors->bg),
        XP(0xC2BFA5),
        H(toolkit_colors->base),
        H(toolkit_colors->text),
        fb.s,
        XP(0xEBEBE4),
        fb.s,
        XP(0xFFFFFF),
        XP(0x686868),
        XP(0xDCDCD7),
        XP(0xFFFFFF),
        H(toolkit_colors->check_border),
        H(toolkit_colors->check_mark),
        H(toolkit_colors->button_hot),
        XP(0xCAC8BB),
        XP(0xCAC8BB));

    xd_strbuf_appendf(
        buffer,
        "menubar { background-color: %s; padding: 0; box-shadow: none; border: none; }\n"
        "menubar > menuitem { padding: 2px 7px; }\n"
        "menubar > menuitem:hover { background-color: %s; color: %s; }\n"
        "menu, .menu, .context-menu {\n  background-color: %s;\n  color: %s;\n"
        "  border: 1px solid %s;\n  border-radius: 0;\n  padding: 2px;\n"
        "  box-shadow: 2px 2px 3px rgba(0,0,0,0.25);\n}\n"
        "menu menuitem { padding: 3px 20px 3px 4px; min-height: 16px; }\n"
        "menu menuitem:hover { background-color: %s; color: %s; }\n"
        "menu menuitem:disabled { color: %s; }\n"
        "menu separator { margin: 3px 2px; min-height: 1px; background-color: %s; }\n"
        "scrollbar {\n  -GtkScrollbar-has-backward-stepper: true;\n"
        "  -GtkScrollbar-has-forward-stepper: true;\n  background-color: %s;\n"
        "  border: none;\n}\n"
        "scrollbar trough { background-color: %s; border: none; }\n"
        "scrollbar slider, scrollbar button {\n  min-width: 15px;\n  min-height: 15px;\n"
        "  background-image: linear-gradient(to right, %s, %s);\n"
        "  border: 1px solid %s;\n  border-radius: 3px;\n  margin: 0;\n"
        "  padding: 0;\n  color: %s;\n  box-shadow: 0 0 0 1px %s;\n}\n"
        "scrollbar.vertical button.up { -gtk-icon-source: -gtk-icontheme(\"pan-up-symbolic\"); }\n"
        "scrollbar.vertical button.down { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-down-symbolic\"); }\n"
        "scrollbar.horizontal button.up { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-start-symbolic\"); }\n"
        "scrollbar.horizontal button.down { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-end-symbolic\"); }\n"
        "notebook > header { background-color: %s; border: none; }\n"
        "notebook > header > tabs > tab {\n  padding: 2px 8px;\n"
        "  background-image: linear-gradient(%s, %s);\n"
        "  border: 1px solid %s;\n  border-bottom: none;\n  border-radius: 3px 3px 0 0;\n"
        "  margin-top: 2px;\n}\n"
        "notebook > header > tabs > tab:hover { box-shadow: inset 0 2px %s; }\n"
        "notebook > stack:not(:only-child) { background-color: %s;\n"
        "  border: 1px solid %s; }\n"
        "treeview.view header button {\n  background-image: linear-gradient(%s, %s);\n"
        "  border: none;\n  border-right: 1px solid %s;\n"
        "  border-bottom: 3px solid %s;\n  border-radius: 0;\n  padding: 1px 6px;\n}\n"
        "paned > separator { background-color: %s; min-width: 4px; min-height: 4px;\n"
        "  background-image: none; }\n"
        "separator { background-color: %s; min-width: 1px; min-height: 1px; }\n"
        "scale trough { background-color: %s; border: 1px solid %s;\n"
        "  border-radius: 2px; min-height: 4px; min-width: 4px; }\n"
        "scale slider { min-width: 11px; min-height: 21px;\n"
        "  background-image: linear-gradient(%s, %s);\n"
        "  border: 1px solid %s; border-radius: 2px; margin: -9px 0; }\n"
        "scale highlight { background: none; }\n",
        H(toolkit_colors->menubar_bg),
        H(toolkit_colors->menu_hot),
        H(toolkit_colors->menu_hot_fg),
        H(toolkit_colors->menu_bg),
        H(toolkit_colors->menu_fg),
        H(toolkit_colors->menu_border),
        H(toolkit_colors->menu_hot),
        H(toolkit_colors->menu_hot_fg),
        H(toolkit_colors->disabled),
        H(toolkit_colors->menu_border),
        H(toolkit_colors->track),
        H(toolkit_colors->track),
        H(toolkit_colors->thumb_top),
        H(toolkit_colors->thumb_bottom),
        XP(0xFFFFFF),
        XP(0x4D6185),
        H(toolkit_colors->track),
        H(toolkit_colors->bg),
        XP(0xFFFFFF),
        XP(0xECEBE6),
        H(toolkit_colors->tab_border),
        H(toolkit_colors->tab_hot),
        XP(0xFCFCFE),
        H(toolkit_colors->tab_border),
        XP(0xFFFFFF),
        XP(0xEBEADB),
        XP(0xC7C5B2),
        XP(0xE2DECD),
        H(toolkit_colors->bg),
        XP(0xACA899),
        XP(0xFFFFFF),
        XP(0x9D9C99),
        XP(0xFFFFFF),
        XP(0xE5E5DE),
        H(toolkit_colors->button_border));

    xd_strbuf_appendf(
        buffer,
        "notebook > header { border: none; border-bottom: 1px solid %s; padding: 0 2px; }\n"
        "notebook > header > tabs > tab { margin: 2px 0 0 0; min-height: 0; }\n"
        "notebook > stack:not(:only-child) { border: 1px solid %s; border-top: none; }\n"
        "frame > border { border: 1px solid %s; border-radius: 3px; box-shadow: none; }\n"
        "frame { padding-left: 6px; }\n"
        "frame > border { margin-left: -6px; }\n"
        "frame > border + label { margin: 0; padding: 0 2px; color: %s; }\n"
        "frame.xd-sunken { padding-left: 0; }\n"
        "frame.xd-sunken > border { margin-left: 0; border: 1px solid; border-radius: 0;\n"
        "  border-color: %s %s %s %s; }\n"
        "combobox button.combo {\n  background-image: none; background-color: %s; color: %s;\n"
        "  border: 1px solid %s; border-radius: 0; box-shadow: none; padding: 0 0 0 3px;\n}\n"
        "combobox button.combo arrow {\n  -gtk-icon-source: -gtk-icontheme(\"pan-down-symbolic\");\n"
        "  min-width: 13px; min-height: 13px; margin: 1px; padding: 1px 1px;\n"
        "  background-image: linear-gradient(%s, %s); color: %s;\n"
        "  border: 1px solid %s; border-radius: 2px;\n}\n",
        H(toolkit_colors->tab_border),
        H(toolkit_colors->tab_border),
        XP(0xD0D0BF),
        H(toolkit_colors->selected_bg),
        XP(0xACA899),
        XP(0xFFFFFF),
        XP(0xFFFFFF),
        XP(0xACA899),
        H(toolkit_colors->base),
        H(toolkit_colors->text),
        H(toolkit_colors->field_border),
        H(toolkit_colors->thumb_top),
        H(toolkit_colors->thumb_bottom),
        XP(0x4D6185),
        XP(0xFFFFFF));

    /* Windows 7's selected tab is plain white; XP's has the orange band. */
    if (aero)
        flat_selected_tab(buffer, toolkit_colors->tab_border, xd_rgba_hex(0xFCFCFE), 5);
    else
        luna_selected_tab(theme, buffer, toolkit_colors->tab_border, toolkit_colors->tab_hot);
}

const char *const xd_gtk_assets[XD_GTK_ASSET_COUNT] = {
    [XD_GTK_CHECK] = "assets/check.svg",
    [XD_GTK_CHECK_DISABLED] = "assets/check-disabled.svg",
    [XD_GTK_CHECK_HOT] = "assets/check-hot.svg",
    [XD_GTK_RADIO] = "assets/radio.svg",
    [XD_GTK_RADIO_DISABLED] = "assets/radio-disabled.svg",
    [XD_GTK_RADIO_HOT] = "assets/radio-hot.svg",
    [XD_GTK_MENU_ARROW] = "assets/menu-arrow.svg",
    [XD_GTK_MENU_ARROW_HOT] = "assets/menu-arrow-hot.svg",
    [XD_GTK_MENU_ARROW_DISABLED] = "assets/menu-arrow-disabled.svg",
    [XD_GTK_SPIN_UP] = "assets/spin-up.svg",
    [XD_GTK_SPIN_UP_DISABLED] = "assets/spin-up-disabled.svg",
    [XD_GTK_SPIN_DOWN] = "assets/spin-down.svg",
    [XD_GTK_SPIN_DOWN_DISABLED] = "assets/spin-down-disabled.svg",
};

/* Windows 2000's 7x7 check mark, placed in the 11x11 inside of a check box. */
static const char *const check_rows[7] = {
    "......#", ".....##", "#...###", "##.###.", "#####..", ".###...", "..#....",
};
/* The 4x4 radio dot. */
static const char *const dot_rows[4] = {".##.", "####", "####", ".##."};
/* A submenu's 4x7 arrow and the spin buttons' 5x3 ones. */
static const char *const menu_arrow_rows[7] = {"#...", "##..", "###.", "####",
                                               "###.", "##..", "#..."};
static const char *const up_rows[3] = {"..#..", ".###.", "#####"};
static const char *const down_rows[3] = {"#####", ".###.", "..#.."};

static void pixel_svg_at(xd_strbuf *out, const char *const *rows, int row_count, int x0, int y0,
                         xd_rgba color);

static void pixel_svg(xd_strbuf *out, const char *const *rows, int row_count, int off,
                      xd_rgba color) {
    pixel_svg_at(out, rows, row_count, off, off, color);
}

static void pixel_svg_at(xd_strbuf *out, const char *const *rows, int row_count, int x0, int y0,
                         xd_rgba color) {
    xd_strbuf_appendf(out,
                      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"11\" height=\"11\" "
                      "viewBox=\"0 0 11 11\" shape-rendering=\"crispEdges\" fill=\"%s\">\n",
                      H(color));
    for (int y = 0; y < row_count; y++) {
        int len = (int)strlen(rows[y]);
        for (int x = 0; x < len;) {
            if (rows[y][x] != '#') {
                x++;
                continue;
            }
            int start = x;
            while (x < len && rows[y][x] == '#')
                x++;
            xd_strbuf_appendf(out, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"1\"/>\n",
                              x0 + start, y0 + y, x - start);
        }
    }
    xd_strbuf_append(out, XD_STR("</svg>\n"));
}

void xd_theme_gtk_asset(const xd_theme *theme, xd_gtk_asset which, xd_strbuf *out) {
    xd_toolkit_colors toolkit_colors;
    xd_theme_toolkit_colors(theme, &toolkit_colors);
    xd_rgba mark = toolkit_colors.classic ? toolkit_colors.text : toolkit_colors.check_mark;
    switch (which) {
    case XD_GTK_CHECK:
        pixel_svg(out, check_rows, 7, 2, mark);
        break;
    case XD_GTK_CHECK_DISABLED:
        pixel_svg(out, check_rows, 7, 2, toolkit_colors.disabled);
        break;
    case XD_GTK_CHECK_HOT:
        pixel_svg(out, check_rows, 7, 2, toolkit_colors.menu_hot_fg);
        break;
    case XD_GTK_RADIO:
        pixel_svg(out, dot_rows, 4, 4, mark);
        break;
    case XD_GTK_RADIO_DISABLED:
        pixel_svg(out, dot_rows, 4, 4, toolkit_colors.disabled);
        break;
    case XD_GTK_RADIO_HOT:
        pixel_svg(out, dot_rows, 4, 4, toolkit_colors.menu_hot_fg);
        break;
    case XD_GTK_MENU_ARROW:
        pixel_svg_at(out, menu_arrow_rows, 7, 4, 2, toolkit_colors.menu_fg);
        break;
    case XD_GTK_MENU_ARROW_HOT:
        pixel_svg_at(out, menu_arrow_rows, 7, 4, 2, toolkit_colors.menu_hot_fg);
        break;
    case XD_GTK_MENU_ARROW_DISABLED:
        pixel_svg_at(out, menu_arrow_rows, 7, 4, 2, toolkit_colors.disabled);
        break;
    case XD_GTK_SPIN_UP:
        pixel_svg_at(out, up_rows, 3, 3, 4, toolkit_colors.fg);
        break;
    case XD_GTK_SPIN_UP_DISABLED:
        pixel_svg_at(out, up_rows, 3, 3, 4, toolkit_colors.disabled);
        break;
    case XD_GTK_SPIN_DOWN:
        pixel_svg_at(out, down_rows, 3, 3, 4, toolkit_colors.fg);
        break;
    case XD_GTK_SPIN_DOWN_DISABLED:
        pixel_svg_at(out, down_rows, 3, 3, 4, toolkit_colors.disabled);
        break;
    case XD_GTK_ASSET_COUNT:
        break;
    }
}

/* ---- Whistler (Watercolor) ------------------------------------------------------------ */

/* Flat, square controls: grey-bordered buttons with a white inner edge and
 * a light blue ring when default, hovered or focused; #646464 field
 * borders; flat tabs; button-less scroll arrows and a white thumb. */
static void whistler(xd_strbuf *buffer) {
    xd_strbuf_append(
        buffer,
        XD_STR(
            "button {\n  background-image: none; background-color: #e5e7e1; color: #000000;\n"
            "  border: 1px solid #808080; border-radius: 0;\n"
            "  box-shadow: inset 1px 1px #ffffff, inset -1px -1px #bcbeb8;\n"
            "  padding: 3px 10px; min-height: 0; text-shadow: none; -gtk-icon-shadow: none;\n}\n"
            "button:hover, button:focus, button.default {\n  border-color: #6f89b0;\n"
            "  box-shadow: inset 1px 1px #c7dcf6, inset -1px -1px #9bbce5, inset 0 0 0 3px #a8c8eb;\n}\n"
            "button:active, button:checked {\n  background-color: #d8dad4; border-color: #808080;\n"
            "  box-shadow: none; padding: 4px 9px 2px 11px;\n}\n"
            "button:disabled { border-color: #c1c1bb; box-shadow: none; background-color: #ebebe4; }\n"
            "button:disabled label { color: #a0a0a0; }\n"
            "toolbar button, button.flat {\n  background-color: transparent; border-color: transparent;\n"
            "  box-shadow: none; padding: 2px 4px;\n}\n"
            "toolbar button:hover, button.flat:hover {\n  background-color: #e5e7e1; border-color: #808080;\n"
            "  box-shadow: inset 1px 1px #ffffff;\n}\n"
            "toolbar button:active, toolbar button:checked, button.flat:active, button.flat:checked {\n"
            "  background-color: #d8dad4; border-color: #808080; box-shadow: none;\n}\n"
            "toolbar { background-color: #ebebe4; padding: 2px; border-bottom: 1px solid #c1c1bb; }\n"
            "entry, spinbutton {\n  background-color: #ffffff; color: #000000; border: 1px solid #646464;\n"
            "  border-radius: 0; box-shadow: none; padding: 3px 4px; min-height: 0;\n}\n"
            "entry:disabled, spinbutton:disabled { background-color: #ebebe4; }\n"
            "scrolledwindow.frame, frame > border, .frame {\n  border: 1px solid #646464;\n"
            "  border-radius: 0; box-shadow: none;\n}\n"
            "progressbar trough { background-color: #ffffff; border: 1px solid #646464; }\n"
            "check, radio {\n  min-width: 13px; min-height: 13px; background-image: none;\n"
            "  background-color: #ffffff; border: 1px solid #646464; border-radius: 0;\n"
            "  box-shadow: none; color: #000000; -gtk-icon-source: none; margin: 0 4px 0 0;\n}\n"
            "radio { border-radius: 50%; }\n"
            "check:hover, radio:hover { box-shadow: inset 0 0 0 1px #a8c8eb; }\n"
            "check:disabled, radio:disabled { background-color: #ebebe4; border-color: #c1c1bb; }\n"
            "menubar { background-color: #ebebe4; padding: 0; box-shadow: none; border: none; }\n"
            "menubar > menuitem { padding: 2px 7px; }\n"
            "menubar > menuitem:hover { background-color: #316ac5; color: #ffffff; }\n"
            "menu, .menu, .context-menu {\n  background-color: #ffffff; color: #000000;\n"
            "  border: 1px solid #808080; border-radius: 0; padding: 2px; box-shadow: none;\n}\n"
            "menu menuitem { padding: 3px 20px 3px 4px; min-height: 16px; }\n"
            "menu menuitem:hover { background-color: #316ac5; color: #ffffff; }\n"
            "menu menuitem:disabled { color: #a0a0a0; }\n"
            "menu separator { margin: 3px 2px; min-height: 1px; background-color: #c1c1bb; }\n"
            "scrollbar {\n  -GtkScrollbar-has-backward-stepper: true;\n"
            "  -GtkScrollbar-has-forward-stepper: true;\n  background-color: #ebebe4; border: none;\n}\n"
            "scrollbar trough { background-color: #ebebe4; border: none; }\n"
            "scrollbar slider {\n  min-width: 11px; min-height: 11px; background-color: #ffffff;\n"
            "  border: 2px solid #c1c1bb; border-radius: 0; margin: 0 1px;\n}\n"
            "scrollbar.horizontal slider { margin: 1px 0; }\n"
            "scrollbar button {\n  min-width: 15px; min-height: 15px; background: none; border: none;\n"
            "  box-shadow: none; color: #000000; padding: 0; margin: 0;\n}\n"
            "scrollbar button:disabled { color: #a0a0a0; }\n"
            "scrollbar.vertical button.up { -gtk-icon-source: -gtk-icontheme(\"pan-up-symbolic\"); }\n"
            "scrollbar.vertical button.down { -gtk-icon-source: "
            "-gtk-icontheme(\"pan-down-symbolic\"); }\n"
            "scrollbar.horizontal button.up { -gtk-icon-source: "
            "-gtk-icontheme(\"pan-start-symbolic\"); }\n"
            "scrollbar.horizontal button.down { -gtk-icon-source: "
            "-gtk-icontheme(\"pan-end-symbolic\"); }\n"));
    /* Split in two: C limits the length of one string literal. */
    xd_strbuf_append(
        buffer,
        XD_STR(
            "notebook > header {\n  background-color: #ebebe4; border: none;\n"
            "  border-bottom: 1px solid #ffffff; padding: 0 2px;\n}\n"
            "notebook > header > tabs > tab {\n  background-color: #dbdbce; background-image: none;\n"
            "  border: none; border-radius: 0; box-shadow: inset -1px 0 #ffffff;\n"
            "  margin: 2px 0 0 0; padding: 2px 8px; min-height: 0; outline-offset: -3px;\n}\n"
            "notebook > header > tabs > tab:checked { background-color: #f3f3ef; }\n"
            "notebook > stack:not(:only-child) {\n  background-color: #f3f3ef; border: 1px solid;\n"
            "  border-color: #ffffff #c1c1bb #c1c1bb #ffffff; border-top: none;\n}\n"
            "frame { padding-left: 6px; }\n"
            "frame > border { margin-left: -6px; border: 1px solid #c1c1bb; }\n"
            "frame > border + label { margin: 0; padding: 0 2px; color: #000000; }\n"
            "frame.xd-sunken { padding-left: 0; }\n"
            "frame.xd-sunken > border { margin-left: 0; border-color: #c1c1bb #ffffff #ffffff #c1c1bb; }\n"
            "combobox button.combo {\n  background-image: none; background-color: #ffffff; color: #000000;\n"
            "  border: 1px solid #646464; border-radius: 0; box-shadow: none; padding: 0 0 0 3px;\n}\n"
            "combobox button.combo arrow {\n  -gtk-icon-source: -gtk-icontheme(\"pan-down-symbolic\");\n"
            "  min-width: 13px; min-height: 13px; margin: 1px; padding: 1px;\n"
            "  background-color: #ebebe4; color: #000000; border: none;\n}\n"
            "treeview.view header button, columnview header button {\n"
            "  background-color: #e5e7e1; border: none; border-right: 1px solid #a0a0a0;\n"
            "  border-bottom: 1px solid #808080; border-radius: 0; box-shadow: inset 1px 1px #ffffff;\n"
            "  padding: 1px 6px;\n}\n"
            "paned > separator { background-color: #ebebe4; min-width: 4px; min-height: 4px;\n"
            "  background-image: none; }\n"
            "separator { background-color: #c1c1bb; min-width: 1px; min-height: 1px; }\n"
            "scale trough { background-color: #ffffff; border: 1px solid #646464; border-radius: 0;\n"
            "  min-height: 4px; min-width: 4px; }\n"
            "scale slider { min-width: 11px; min-height: 21px; background-color: #e5e7e1;\n"
            "  border: 1px solid #808080; border-radius: 0; box-shadow: inset 1px 1px #ffffff;\n"
            "  margin: -9px 0; }\n"
            "scale highlight { background: none; }\n"));
    flat_selected_tab(buffer, xd_rgba_hex(0xFFFFFF), xd_rgba_hex(0xF3F3EF), 4);
}

/* XP push buttons: the theme engine's own button (saved as PNGs by
 * gtktheme.c) as a nine-slice border image, 75x23 like Windows' dialog
 * buttons, so GTK's match ours pixel for pixel. Toolbars keep flat
 * buttons. */
#define XP_BUTTON "button.text-button:not(.flat):not(.combo)"
static void xp_buttons(const xd_toolkit_colors *toolkit_colors, xd_strbuf *buffer) {
    static const char *const states[][2] = {
        {"", "button"},
        {":focus, " XP_BUTTON ".default", "button-default"},
        {":hover", "button-hot"},
        {":active, " XP_BUTTON ":checked", "button-pressed"},
        {":disabled", "button-disabled"},
    };
    xd_strbuf_append(buffer,
                     XD_STR(XP_BUTTON
                            " {\n"
                            "  border-style: solid;\n  border-width: 4px 5px;\n"
                            "  border-radius: 0;\n  background-image: none;\n"
                            "  background-color: transparent;\n  box-shadow: none;\n"
                            "  padding: 0 6px;\n  min-height: 15px;\n  min-width: 53px;\n}\n"));
    /* The border image paints the edges; the same image stretched as the
     * background fills the middle (border images don't). */
    for (size_t i = 0; i < XD_ARRAY_LEN(states); i++)
        xd_strbuf_appendf(buffer,
                          XP_BUTTON
                          "%s {\n  border-image: -gtk-scaled(url(\"assets/%s.png\"), "
                          "url(\"assets/%s@2.png\")) 4 5 4 5 stretch;\n"
                          "  background-image: -gtk-scaled(url(\"assets/%s.png\"), "
                          "url(\"assets/%s@2.png\"));\n"
                          "  background-size: 100%% 100%%;\n  background-origin: border-box;\n}\n",
                          states[i][0], states[i][1], states[i][1], states[i][1], states[i][1]);
    /* Pressed: the text moves one pixel down and right, as in Windows. */
    xd_strbuf_appendf(buffer,
                      XP_BUTTON ":active { padding: 1px 5px 0 7px; min-height: 14px; }\n" XP_BUTTON
                                ":disabled label { color: %s; }\n"
                                "toolbar " XP_BUTTON
                                " {\n  border-image: none;\n  border-width: 1px;\n"
                                "  padding: 2px 4px;\n  min-height: 0;\n  min-width: 0;\n}\n",
                      H(toolkit_colors->disabled));
}
#undef XP_BUTTON

/* Combo boxes drop down a list box, as in Windows, instead of a menu over
 * the field: white, a 1px border (libxdui's), the highlight following the
 * pointer. GTK names the pop-up window, not its combo box. */
static void combo_lists(const xd_toolkit_colors *toolkit_colors, xd_strbuf *buffer) {
    xd_rgba border = toolkit_colors->classic ? toolkit_colors->frame : xd_rgba_hex(0x7F9DB9);
    xd_strbuf_appendf(buffer,
                      "combobox { -GtkComboBox-appears-as-list: 1; }\n"
                      "#gtk-combobox-popup-window scrolledwindow.frame {\n"
                      "  border: 1px solid %s; border-radius: 0; box-shadow: none; padding: 0;\n}\n"
                      "#gtk-combobox-popup-window treeview.view {\n"
                      "  -GtkTreeView-vertical-separator: 0; background-color: %s; color: %s;\n}\n"
                      "#gtk-combobox-popup-window treeview.view:selected {\n"
                      "  background-color: %s; color: %s;\n}\n",
                      H(border), H(toolkit_colors->base), H(toolkit_colors->text),
                      H(toolkit_colors->selected_bg), H(toolkit_colors->selected_fg));
}

/* The file chooser as far as CSS reaches XP's Open dialog: the places
 * sidebar is a sunken list with full-colour icons, the path bar flat
 * buttons that rise under the pointer (the current folder in bold). XP
 * push buttons are border images, which the path bar must drop. */
#define PATH_BUTTON ".path-bar button"
#define PATH_XP_BUTTON ".path-bar button.text-button:not(.flat):not(.combo)"
static void file_chooser(const xd_toolkit_colors *toolkit_colors, xd_strbuf *buffer) {
    if (toolkit_colors->classic)
        xd_strbuf_appendf(buffer,
                          "placessidebar {\n  border-style: solid; border-width: 1px;\n"
                          "  border-color: %s %s %s %s;\n"
                          "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n}\n",
                          H(toolkit_colors->shadow), H(toolkit_colors->hilight),
                          H(toolkit_colors->hilight), H(toolkit_colors->shadow),
                          H(toolkit_colors->dkshadow), H(toolkit_colors->light));
    else
        xd_strbuf_appendf(buffer, "placessidebar { border: 1px solid %s; }\n",
                          H(toolkit_colors->field_border));
    xd_strbuf_appendf(
        buffer,
        "placessidebar { -gtk-icon-style: regular; background-color: %s; padding: 1px; }\n"
        "placessidebar list { background-color: %s; }\n"
        "placessidebar row.sidebar-row { min-height: 0; padding: 1px 4px; }\n"
        "placessidebar row.sidebar-row:selected label,\n"
        "placessidebar row.sidebar-row:selected image { color: %s; }\n"
        "placessidebar separator { margin: 3px 0; }\n" PATH_BUTTON ", " PATH_XP_BUTTON " {\n"
        "  background-color: transparent; background-image: none; border-image: none;\n"
        "  border: 1px solid transparent; border-radius: 0; box-shadow: none;\n"
        "  padding: 1px 4px; min-height: 0; min-width: 0; -gtk-icon-style: regular;\n}\n" PATH_BUTTON
        ":hover, " PATH_XP_BUTTON ":hover {\n"
        "  border-image: none; background-image: none; border-color: %s %s %s %s;\n}\n" PATH_BUTTON
        ":active, " PATH_XP_BUTTON ":active {\n"
        "  border-image: none; background-image: none; border-color: %s %s %s %s;\n"
        "  padding: 1px 4px;\n}\n" PATH_BUTTON ":checked, " PATH_XP_BUTTON
        ":checked { font-weight: bold; }\n",
        H(toolkit_colors->base), H(toolkit_colors->base), H(toolkit_colors->selected_fg),
        H(toolkit_colors->classic ? toolkit_colors->hilight : toolkit_colors->field_border),
        H(toolkit_colors->classic ? toolkit_colors->shadow : toolkit_colors->field_border),
        H(toolkit_colors->classic ? toolkit_colors->shadow : toolkit_colors->field_border),
        H(toolkit_colors->classic ? toolkit_colors->hilight : toolkit_colors->field_border),
        H(toolkit_colors->classic ? toolkit_colors->shadow : toolkit_colors->field_border),
        H(toolkit_colors->classic ? toolkit_colors->hilight : toolkit_colors->field_border),
        H(toolkit_colors->classic ? toolkit_colors->hilight : toolkit_colors->field_border),
        H(toolkit_colors->classic ? toolkit_colors->shadow : toolkit_colors->field_border));
}
#undef PATH_BUTTON
#undef PATH_XP_BUTTON

/* Windows that draw their own title bar (other programs' message boxes,
 * which GTK always gives one, and header-bar dialogs) get no frame from
 * the compositor, so they draw Windows' dialog frame themselves. Message
 * boxes: Yes before No comes from the theme's settings.ini (GTK's
 * alternative button order); GTK stretches the buttons across the box in
 * code, so CSS can only space them apart as Windows does. */
static void client_frames(const xd_toolkit_colors *toolkit_colors, xd_strbuf *buffer) {
    /* Menus and tooltips are client-decorated windows too (Wayland popups):
     * they have their own border. */
    const char *frame = "window.csd:not(.popup):not(.tooltip):not(.maximized):not(.fullscreen)"
                        ":not(.tiled) > decoration";
    if (toolkit_colors->classic)
        /* Windows 2000's 3px dialog frame. */
        xd_strbuf_appendf(buffer,
                          "%s {\n  border-style: solid; border-width: 1px;\n"
                          "  border-color: %s %s %s %s;\n"
                          "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n"
                          "  padding: 1px; background-color: %s;\n}\n"
                          /* GTK pads the title by 6px in code: Windows'
                           * caption is 18px. */
                          "messagedialog > box.titlebar > label.title {\n"
                          "  margin-top: -4px; margin-bottom: -4px;\n}\n",
                          frame, H(toolkit_colors->light), H(toolkit_colors->dkshadow),
                          H(toolkit_colors->dkshadow), H(toolkit_colors->light),
                          H(toolkit_colors->hilight), H(toolkit_colors->shadow),
                          H(toolkit_colors->face));
    else
        xd_strbuf_appendf(buffer, "%s {\n  border: 3px solid %s; border-top-width: 0;\n}\n", frame,
                          H(toolkit_colors->title_bg));
    xd_strbuf_append(buffer,
                     XD_STR("messagedialog .dialog-action-area > button { margin: 0 3px; }\n"
                            "messagedialog .dialog-action-area > button:first-child {\n"
                            "  margin-left: 0;\n}\n"
                            "messagedialog .dialog-action-area > button:last-child {\n"
                            "  margin-right: 0;\n}\n"));
}

void xd_theme_gtk_css(const xd_theme *theme, xd_strbuf *out) {
    xd_toolkit_colors toolkit_colors;
    xd_theme_toolkit_colors(theme, &toolkit_colors);
    xd_gtk_pkg_toolkit_colors(theme, &toolkit_colors);
    xd_strbuf_appendf(out,
                      "/* Generated by XeonDesktop from the %s style and the \"%s\" colour\n"
                      " * scheme. Edits are overwritten; change Display Properties instead. */\n\n",
                      xd_visual_style_name(theme->style), theme->scheme.name);
    defines(&toolkit_colors, out);
    font_rules(theme, out);
    common(&toolkit_colors, out);
    /* Dialog button rows in every style: Windows' 75x23 (50x14 dialog
     * units); GTK's own 85x27 minimum would win otherwise. */
    xd_strbuf_append(out, XD_STR("buttonbox {\n  -GtkButtonBox-child-min-width: 75;\n"
                                 "  -GtkButtonBox-child-min-height: 23;\n"
                                 "  -GtkButtonBox-child-internal-pad-x: 0;\n"
                                 "  -GtkButtonBox-child-internal-pad-y: 0;\n}\n"));
    if (toolkit_colors.classic) {
        classic(&toolkit_colors, out);
        /* Windows 2000's progress bar: blocks of the highlight colour. */
        xd_strbuf_appendf(
            out,
            "progressbar trough { padding: 1px; }\n"
            "progressbar progress {\n  background-color: transparent;\n"
            "  background-image: repeating-linear-gradient(to right, %s 0px, %s 8px,\n"
            "    transparent 8px, transparent 10px);\n}\n",
            H(toolkit_colors.selected_bg), H(toolkit_colors.selected_bg));
    } else if (theme->style == XD_STYLE_WHISTLER)
        whistler(out);
    else
        modern(theme, &toolkit_colors, theme->style == XD_STYLE_AERO, out);
    if (!toolkit_colors.classic)
        xp_buttons(&toolkit_colors, out);
    /* A slider's tick marks: Windows' 1 px lines, 4 px long, in the
     * trackbar's tick colour (without a size GTK draws them invisible). */
    xd_rgba tics = xd_theme_trackbar_tics_color(theme, false);
    xd_strbuf_appendf(out,
                      "scale marks mark indicator { background-color: %s; color: %s;\n"
                      "  min-height: 4px; min-width: 1px; }\n"
                      "scale.vertical marks mark indicator { min-height: 1px; min-width: 4px; }\n",
                      H(tics), H(tics));
    /* Windows' pixel check mark and radio dot, drawn by xd_theme_gtk_asset. */
    xd_strbuf_appendf(
        out,
        "check, radio { padding: 0; }\n"
        "check:checked { -gtk-icon-source: url(\"%s\"); }\n"
        "check:indeterminate { -gtk-icon-source: url(\"%s\"); background-color: %s; }\n"
        "check:checked:disabled { -gtk-icon-source: url(\"%s\"); }\n"
        "radio:checked { -gtk-icon-source: url(\"%s\"); }\n"
        "radio:indeterminate { -gtk-icon-source: none; background-color: %s; }\n"
        "radio:checked:disabled { -gtk-icon-source: url(\"%s\"); }\n"
        "menu menuitem:hover > check:checked { -gtk-icon-source: url(\"%s\"); }\n"
        "menu menuitem:hover > radio:checked { -gtk-icon-source: url(\"%s\"); }\n",
        xd_gtk_assets[XD_GTK_CHECK], xd_gtk_assets[XD_GTK_CHECK_DISABLED], H(toolkit_colors.bg),
        xd_gtk_assets[XD_GTK_CHECK_DISABLED], xd_gtk_assets[XD_GTK_RADIO], H(toolkit_colors.bg),
        xd_gtk_assets[XD_GTK_RADIO_DISABLED], xd_gtk_assets[XD_GTK_CHECK_HOT],
        xd_gtk_assets[XD_GTK_RADIO_HOT]);
    /* GTK marks the whole notebook as a .frame; Windows draws no outline
     * around the tab control, only the page and tabs themselves. */
    xd_strbuf_append(out, XD_STR("notebook, notebook.frame { border: none; box-shadow: none; }\n"));
    /* Disclosure triangles (expanders, "Details"). */
    xd_strbuf_append(out, XD_STR("expander > title > arrow {\n  min-width: 12px;\n"
                                 "  min-height: 12px;\n  margin-right: 4px;\n"
                                 "  -gtk-icon-source: -gtk-icontheme(\"pan-end-symbolic\");\n}\n"
                                 "expander > title > arrow:checked { -gtk-icon-source: "
                                 "-gtk-icontheme(\"pan-down-symbolic\"); }\n"));
    /* A submenu's arrow, and spin buttons' (narrow, with Windows' small
     * arrows in place of GTK's + and -; GTK keeps them side by side). */
    xd_strbuf_appendf(
        out,
        "menu menuitem > arrow { min-width: 11px; min-height: 11px;\n"
        "  -gtk-icon-source: url(\"%s\"); }\n"
        "menu menuitem:hover > arrow { -gtk-icon-source: url(\"%s\"); }\n"
        "menu menuitem:disabled > arrow { -gtk-icon-source: url(\"%s\"); }\n"
        /* GTK names the + and - icons in code: they're hidden, and the
         * arrows drawn as the buttons' background images. */
        "spinbutton button { min-width: 11px; padding: 0 2px;\n"
        "  -gtk-icon-transform: scale(0); background-repeat: no-repeat;\n"
        "  background-position: center; }\n"
        "spinbutton button.up { background-image: url(\"%s\"); }\n"
        "spinbutton button.up:disabled { background-image: url(\"%s\"); }\n"
        "spinbutton button.down { background-image: url(\"%s\"); }\n"
        "spinbutton button.down:disabled { background-image: url(\"%s\"); }\n",
        xd_gtk_assets[XD_GTK_MENU_ARROW], xd_gtk_assets[XD_GTK_MENU_ARROW_HOT],
        xd_gtk_assets[XD_GTK_MENU_ARROW_DISABLED], xd_gtk_assets[XD_GTK_SPIN_UP],
        xd_gtk_assets[XD_GTK_SPIN_UP_DISABLED], xd_gtk_assets[XD_GTK_SPIN_DOWN],
        xd_gtk_assets[XD_GTK_SPIN_DOWN_DISABLED]);
    /* Info bars: Windows' information bar is the tooltip colour with a
     * thin border. Switches (Windows has none) as a sunken track with a
     * button-like thumb; their I/O glyphs aren't Windows. */
    xd_strbuf_appendf(
        out,
        "infobar { background-color: %s; color: %s;\n"
        "  border: 1px solid %s; padding: 2px; }\n"
        "infobar label { color: %s; }\n"
        "switch { background-color: %s; border: 1px solid %s; border-radius: 0;\n"
        "  min-width: 40px; min-height: 16px; padding: 1px; font-size: 0; }\n"
        "switch:checked { background-color: %s; }\n"
        "switch image { -gtk-icon-source: none; color: transparent; }\n"
        "switch slider { min-width: 18px; min-height: 14px; border-radius: 0;\n"
        "  background-color: %s; border: 1px solid %s; }\n",
        H(toolkit_colors.tooltip_bg), H(toolkit_colors.tooltip_fg),
        H(toolkit_colors.classic ? toolkit_colors.dkshadow : toolkit_colors.field_border),
        H(toolkit_colors.tooltip_fg), H(toolkit_colors.base),
        H(toolkit_colors.classic ? toolkit_colors.shadow : toolkit_colors.field_border),
        H(toolkit_colors.selected_bg),
        H(toolkit_colors.classic ? toolkit_colors.face : toolkit_colors.bg),
        H(toolkit_colors.classic ? toolkit_colors.dkshadow : toolkit_colors.field_border));
    /* Popovers (other programs' menus) look like Windows menus: square,
     * the menu colour and border, highlighted rows, the submenu arrow. */
    xd_strbuf_appendf(
        out,
        "popover.background { background-color: %s; color: %s; border: 1px solid %s;\n"
        "  border-radius: 0; box-shadow: %s; padding: 2px; }\n"
        "popover.background modelbutton.flat { border-radius: 0; min-height: 18px;\n"
        "  padding: 1px 8px; color: %s; }\n"
        "popover.background modelbutton.flat:hover { background-color: %s; color: %s; }\n"
        "popover.background modelbutton.flat:disabled { color: %s; }\n"
        "modelbutton.flat arrow.right { -gtk-icon-source: url(\"%s\"); }\n"
        "modelbutton.flat:hover arrow.right { -gtk-icon-source: url(\"%s\"); }\n"
        "popover.background separator { margin: 3px 0; }\n",
        H(toolkit_colors.menu_bg), H(toolkit_colors.menu_fg), H(toolkit_colors.menu_border),
        toolkit_colors.classic ? "inset 1px 1px #ffffff, inset -1px -1px #808080" : "none",
        H(toolkit_colors.menu_fg), H(toolkit_colors.menu_hot), H(toolkit_colors.menu_hot_fg),
        H(toolkit_colors.disabled), xd_gtk_assets[XD_GTK_MENU_ARROW],
        xd_gtk_assets[XD_GTK_MENU_ARROW_HOT]);
    /* Windows menus show only the mark of a check or radio item, never a box. */
    xd_strbuf_append(out, XD_STR("menu menuitem check, menu menuitem radio, menu menuitem check:hover,\n"
                                 "menu menuitem radio:hover {\n"
                                 "  background-image: none;\n  background-color: transparent;\n"
                                 "  border-color: transparent;\n  box-shadow: none;\n"
                                 "  color: inherit;\n}\n"));
    combo_lists(&toolkit_colors, out);
    file_chooser(&toolkit_colors, out);
    client_frames(&toolkit_colors, out);
    /* A theme package's images over all that, where it has them. */
    xd_gtk_pkg_css(theme, out);
}
