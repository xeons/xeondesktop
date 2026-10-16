/* SPDX-License-Identifier: MIT
 * GTK3 theme generated from the active visual style and colour scheme, so
 * GTK apps (xd-files, Control Panel applets, third-party apps) match the
 * shell: Windows 2000 3D bevels for Classic, XP's gradients and flat
 * borders for Luna and Whistler.
 */
#include <math.h>
#include <stdarg.h>

#include "style.h"

/* "#rrggbb" for opaque colours, rgba() otherwise. */
typedef struct hexbuf {
    char s[64];
} hexbuf;

static unsigned channel(double v) {
    double c = round(v * 255.0);
    return c <= 0 ? 0u : c >= 255 ? 255u : (unsigned)c;
}

static hexbuf hex(xd_rgba c) {
    hexbuf h;
    unsigned r = channel(c.r);
    unsigned g = channel(c.g);
    unsigned b = channel(c.b);
    double av = round(c.a * 1000.0);
    unsigned a = av <= 0 ? 0u : av >= 1000 ? 1000u : (unsigned)av;
    if (a >= 1000)
        snprintf(h.s, sizeof(h.s), "#%02x%02x%02x", r, g, b);
    else
        snprintf(h.s, sizeof(h.s), "rgba(%u,%u,%u,0.%03u)", r, g, b, a);
    return h;
}

#define H(c) (hex(c).s)

/* The scheme's message font as a CSS font-family list. */
static void font_rules(const xd_theme *t, xd_strbuf *out) {
    const xd_font_spec *f = &t->scheme.fonts[XD_FONT_MESSAGE];
    xd_strbuf_append(out, XD_STR("* {\n  font-family: "));
    xd_str rest = xd_str_from_cstr(f->family);
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
    xd_strbuf_appendf(out, ";\n  font-size: %.1fpt;\n}\n\n", f->size_pt);
}

static void defines(const xd_toolkit_colors *c, xd_strbuf *o) {
    /* Named colours many GTK apps (and our folder view) look up. */
    xd_strbuf_appendf(o,
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
                      H(c->bg), H(c->fg), H(c->base), H(c->text), H(c->selected_bg),
                      H(c->selected_fg), H(c->disabled), H(c->bg), H(c->fg), H(c->base),
                      H(c->text), H(c->selected_bg), H(c->selected_fg),
                      H(c->classic ? c->shadow : c->field_border),
                      H(c->classic ? c->shadow : c->field_border), H(c->tooltip_bg),
                      H(c->tooltip_fg));
}

/* Rules shared by both looks. */
static void common(const xd_toolkit_colors *c, xd_strbuf *o) {
    xd_strbuf_appendf(
        o,
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
        H(c->bg), H(c->fg), H(c->disabled), H(c->selected_bg), H(c->selected_fg), H(c->base),
        H(c->text), H(c->tooltip_bg), H(c->tooltip_fg), H(c->tooltip_fg), H(c->title_bg),
        H(c->title_bg2), H(c->title_fg), H(c->title_fg), H(c->selected_bg), H(c->fg));
}

/* ---- Classic (Windows 2000) --------------------------------------------------------- */

static void classic(const xd_toolkit_colors *c, xd_strbuf *o) {
    hexbuf fc = hex(c->face);
    const char *face = fc.s; /* fc must outlive every use of face */
    /* Raised: light/dkshadow outside, hilight/shadow inside. Sunken is the
     * reverse. */
    hexbuf hi = hex(c->hilight);
    hexbuf li = hex(c->light);
    hexbuf sh = hex(c->shadow);
    hexbuf dk = hex(c->dkshadow);
    xd_rgba dither = xd_rgba_mix(c->scrollbar, c->hilight, 0.5);

    xd_strbuf_appendf(
        o,
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
        "button.default { box-shadow: 0 0 0 1px %s, inset -1px -1px %s, inset 1px 1px %s; }\n"
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
        "scrolledwindow.frame, frame > border, .frame {\n  border-style: solid;\n"
        "  border-width: 1px;\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n  border-radius: 0;\n}\n"
        /* Check boxes and radio buttons */
        "check, radio {\n  min-width: 13px;\n  min-height: 13px;\n"
        "  background-image: none;\n  background-color: %s;\n  color: %s;\n"
        "  border-style: solid;\n  border-width: 1px;\n  border-color: %s %s %s %s;\n"
        "  box-shadow: inset 1px 1px %s, inset -1px -1px %s;\n  border-radius: 0;\n"
        "  -gtk-icon-source: none;\n  margin: 0 4px 0 0;\n  padding: 0;\n}\n"
        "radio { border-radius: 50%%; }\n"
        "check:checked { -gtk-icon-source: -gtk-icontheme(\"object-select-symbolic\"); }\n"
        "radio:checked { -gtk-icon-source: -gtk-icontheme(\"media-record-symbolic\"); }\n"
        "check:disabled, radio:disabled { background-color: %s; color: %s; }\n",
        face, H(c->fg), hi.s, dk.s, dk.s, hi.s, sh.s, li.s, face, dk.s, hi.s, hi.s, dk.s, sh.s,
        li.s, H(dither), H(c->disabled), hi.s, H(c->frame), sh.s, li.s, hi.s, sh.s, sh.s, hi.s,
        sh.s, hi.s, hi.s, sh.s, H(dither), face, sh.s, hi.s, H(c->base), H(c->text), sh.s, hi.s,
        hi.s, sh.s, dk.s, li.s, face, sh.s, hi.s, hi.s, sh.s, dk.s, li.s, H(c->base), H(c->text),
        sh.s, hi.s, hi.s, sh.s, dk.s, li.s, face, H(c->disabled));

    xd_strbuf_appendf(
        o,
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
        H(c->menubar_bg), hi.s, sh.s, sh.s, hi.s, H(c->menu_fg), H(c->menu_bg), H(c->menu_fg),
        li.s, dk.s, dk.s, li.s, hi.s, sh.s, H(c->menu_hot), H(c->menu_hot_fg), H(c->disabled),
        sh.s, hi.s, H(dither), H(dither), face, li.s, dk.s, dk.s, li.s, hi.s, sh.s, H(c->fg),
        sh.s, face, face, hi.s, dk.s, dk.s, hi.s, li.s, sh.s, face, hi.s, dk.s, dk.s, hi.s, sh.s,
        li.s, face, sh.s, face, sh.s, hi.s, hi.s, sh.s, face, hi.s, dk.s, dk.s, hi.s, li.s, sh.s);
}

/* ---- Luna and Whistler (Windows XP) ----------------------------------------------- */

static void modern(const xd_toolkit_colors *c, xd_strbuf *o) {
    hexbuf fb = hex(c->field_border);
    xd_strbuf_appendf(
        o,
        "button {\n  background-image: linear-gradient(%s, %s);\n  background-color: %s;\n"
        "  color: %s;\n  border: 1px solid %s;\n  border-radius: 3px;\n"
        "  padding: 3px 10px;\n  min-height: 0;\n  box-shadow: none;\n  text-shadow: none;\n"
        "  -gtk-icon-shadow: none;\n}\n"
        "button:hover { box-shadow: inset 0 0 0 2px %s; }\n"
        "button:active, button:checked {\n  background-image: linear-gradient(%s, %s);\n"
        "  box-shadow: none;\n}\n"
        "button:focus, button.default { box-shadow: inset 0 0 0 2px %s; }\n"
        "button:disabled { border-color: #c9c7ba; background-image: none;\n"
        "  background-color: #f5f4ea; }\n"
        "button:disabled label { color: %s; }\n"
        "toolbar button, button.flat {\n  background-image: none;\n"
        "  background-color: transparent;\n  border-color: transparent;\n"
        "  box-shadow: none;\n  padding: 2px 4px;\n}\n"
        "toolbar button:hover, button.flat:hover {\n  border-color: #cecec3;\n"
        "  background-image: linear-gradient(#ffffff, #e5e5de);\n  box-shadow: none;\n}\n"
        "toolbar button:active, toolbar button:checked, button.flat:active,\n"
        "button.flat:checked { border-color: #9d9d92;\n"
        "  background-image: linear-gradient(#e5e5de, #f7f7f4); }\n"
        "toolbar { background-color: %s; padding: 2px;\n  border-bottom: 1px solid #c2bfa5; }\n"
        "entry, spinbutton {\n  background-color: %s;\n  color: %s;\n"
        "  border: 1px solid %s;\n  border-radius: 0;\n  box-shadow: none;\n"
        "  padding: 3px 4px;\n  min-height: 0;\n}\n"
        "entry:disabled { background-color: #ebebe4; }\n"
        "scrolledwindow.frame, frame > border, .frame {\n  border: 1px solid %s;\n"
        "  border-radius: 0;\n  box-shadow: none;\n}\n"
        "check, radio {\n  min-width: 13px;\n  min-height: 13px;\n"
        "  background-image: linear-gradient(135deg, #dcdcd7, #ffffff);\n"
        "  border: 1px solid %s;\n  border-radius: 0;\n  box-shadow: none;\n"
        "  color: %s;\n  -gtk-icon-source: none;\n  margin: 0 4px 0 0;\n  padding: 0;\n}\n"
        "radio { border-radius: 50%%; }\n"
        "check:hover, radio:hover { box-shadow: inset 0 0 0 2px %s; }\n"
        "check:checked { -gtk-icon-source: -gtk-icontheme(\"object-select-symbolic\"); }\n"
        "radio:checked { -gtk-icon-source: -gtk-icontheme(\"media-record-symbolic\"); }\n"
        "check:disabled, radio:disabled { border-color: #cac8bb; color: #cac8bb; }\n",
        H(c->button_top), H(c->button_bottom), H(c->button_bottom), H(c->fg),
        H(c->button_border), H(c->button_hot), H(c->button_bottom), H(c->button_top),
        H(c->button_focus), H(c->disabled), H(c->bg), H(c->base), H(c->text), fb.s, fb.s,
        H(c->check_border), H(c->check_mark), H(c->button_hot));

    xd_strbuf_appendf(
        o,
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
        "  border: 1px solid #ffffff;\n  border-radius: 3px;\n  margin: 0;\n"
        "  padding: 0;\n  color: #4d6185;\n  box-shadow: 0 0 0 1px %s;\n}\n"
        "scrollbar.vertical button.up { -gtk-icon-source: -gtk-icontheme(\"pan-up-symbolic\"); }\n"
        "scrollbar.vertical button.down { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-down-symbolic\"); }\n"
        "scrollbar.horizontal button.up { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-start-symbolic\"); }\n"
        "scrollbar.horizontal button.down { -gtk-icon-source: "
        "-gtk-icontheme(\"pan-end-symbolic\"); }\n"
        "notebook > header { background-color: %s; border: none; }\n"
        "notebook > header > tabs > tab {\n  padding: 2px 8px;\n"
        "  background-image: linear-gradient(#ffffff, #ecebe6);\n"
        "  border: 1px solid %s;\n  border-bottom: none;\n  border-radius: 3px 3px 0 0;\n"
        "  margin-top: 2px;\n}\n"
        "notebook > header > tabs > tab:hover, notebook > header > tabs > tab:checked {\n"
        "  box-shadow: inset 0 2px %s;\n}\n"
        "notebook > header > tabs > tab:checked { margin-top: 0; padding-bottom: 4px;\n"
        "  background-image: linear-gradient(#ffffff, #fcfcfe); }\n"
        "notebook > stack:not(:only-child) { background-color: #fcfcfe;\n"
        "  border: 1px solid %s; }\n"
        "treeview.view header button {\n  background-image: linear-gradient(#ffffff, #ebeadb);\n"
        "  border: none;\n  border-right: 1px solid #c7c5b2;\n"
        "  border-bottom: 3px solid #e2decd;\n  border-radius: 0;\n  padding: 1px 6px;\n}\n"
        "paned > separator { background-color: %s; min-width: 4px; min-height: 4px;\n"
        "  background-image: none; }\n"
        "separator { background-color: #aca899; min-width: 1px; min-height: 1px; }\n"
        "scale trough { background-color: #ffffff; border: 1px solid #9d9c99;\n"
        "  border-radius: 2px; min-height: 4px; min-width: 4px; }\n"
        "scale slider { min-width: 11px; min-height: 21px;\n"
        "  background-image: linear-gradient(#ffffff, #e5e5de);\n"
        "  border: 1px solid %s; border-radius: 2px; margin: -9px 0; }\n"
        "scale highlight { background: none; }\n",
        H(c->menubar_bg), H(c->menu_hot), H(c->menu_hot_fg), H(c->menu_bg), H(c->menu_fg),
        H(c->menu_border), H(c->menu_hot), H(c->menu_hot_fg), H(c->disabled),
        H(c->menu_border), H(c->track), H(c->track), H(c->thumb_top), H(c->thumb_bottom),
        H(c->track), H(c->bg), H(c->tab_border), H(c->tab_hot), H(c->tab_border), H(c->bg),
        H(c->button_border));
}

void xd_theme_gtk_css(const xd_theme *t, xd_strbuf *out) {
    xd_toolkit_colors c;
    xd_theme_toolkit_colors(t, &c);
    xd_strbuf_appendf(out,
                      "/* Generated by XeonDesktop from the %s style and the \"%s\" colour\n"
                      " * scheme. Edits are overwritten; change Display Properties instead. */\n\n",
                      xd_visual_style_name(t->style), t->scheme.name);
    defines(&c, out);
    font_rules(t, out);
    common(&c, out);
    if (c.classic)
        classic(&c, out);
    else
        modern(&c, out);
    /* Windows menus show only the mark of a check or radio item, never a box. */
    xd_strbuf_append(out, XD_STR("menu menuitem check, menu menuitem radio, menu menuitem check:hover,\n"
                                 "menu menuitem radio:hover {\n"
                                 "  background-image: none;\n  background-color: transparent;\n"
                                 "  border-color: transparent;\n  box-shadow: none;\n"
                                 "  color: inherit;\n}\n"));
}
