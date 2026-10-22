/* SPDX-License-Identifier: MIT
 * The language indicator: Windows XP's Language bar minimized into the
 * taskbar, left of the notification area (Windows 2000 had the same "En"
 * square in the tray). It shows the active keyboard layout's language in
 * two letters on a blue square, only when [Keyboard] Layout lists more
 * than one layout, as Windows shows it only with several input languages.
 *
 * A click opens the list of layouts with a check by the active one;
 * choosing one switches every keyboard at once, for this session. The
 * compositor owns the keyboards, so both ways go through it
 * (xd-window-arranger-v1): it tells the active layout (also after the
 * switching keys, [Keyboard] Options' grp:, are pressed), and the menu
 * asks it to switch. Right-click: Settings... opens Keyboard Properties.
 *
 * Names and languages come from xkeyboard-config through libxkbregistry;
 * the two-letter code from the ISO 639 table of iso-codes, when installed.
 */
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <xkbcommon/xkbregistry.h>

#include "shell.h"

typedef struct language {
    char code[8]; /* "EN" */
    char *name;   /* "English (US)" */
} language;

struct xd_langbar {
    char key[sizeof(((xd_keyboard_settings *)nullptr)->layout) +
             sizeof(((xd_keyboard_settings *)nullptr)->variant) + 2];
    language *langs;
    size_t count;
    uint32_t active;
    bool told; /* the compositor has said which is active */
};

static xd_langbar *state(xd_shell *shell) {
    if (shell->langbar == nullptr)
        shell->langbar = XD_NEW(xd_langbar);
    return shell->langbar;
}

static void clear(xd_langbar *lb) {
    for (size_t i = 0; i < lb->count; i++)
        g_free(lb->langs[i].name);
    xd_free(lb->langs);
    lb->langs = nullptr;
    lb->count = 0;
}

/* ISO 639-2/T (three letters, as xkeyboard-config gives) to the
 * two-letter code Windows shows, from iso-codes' table. */
static JsonArray *iso639_table(JsonParser **owner) {
    *owner = nullptr;
    const char *const *dirs = g_get_system_data_dirs();
    for (size_t i = 0; dirs[i] != nullptr; i++) {
        char *path = g_build_filename(dirs[i], "iso-codes", "json", "iso_639-2.json", nullptr);
        JsonParser *parser = json_parser_new();
        bool ok = json_parser_load_from_file(parser, path, nullptr);
        g_free(path);
        JsonNode *root = ok ? json_parser_get_root(parser) : nullptr;
        if (root != nullptr && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *object = json_node_get_object(root);
            JsonNode *list = json_object_get_member(object, "639-2");
            if (list != nullptr && JSON_NODE_HOLDS_ARRAY(list)) {
                *owner = parser;
                return json_node_get_array(list);
            }
        }
        g_object_unref(parser);
    }
    return nullptr;
}

static const char *alpha2_of(JsonArray *table, const char *alpha3) {
    if (table == nullptr || alpha3 == nullptr)
        return nullptr;
    for (guint i = 0; i < json_array_get_length(table); i++) {
        JsonObject *object = json_array_get_object_element(table, i);
        if (object == nullptr)
            continue;
        const char *a3 = json_object_get_string_member_with_default(object, "alpha_3", "");
        const char *bib = json_object_get_string_member_with_default(object, "bibliographic", "");
        if (strcmp(a3, alpha3) == 0 || strcmp(bib, alpha3) == 0)
            return json_object_get_string_member_with_default(object, "alpha_2", nullptr);
    }
    return nullptr;
}

static struct rxkb_layout *find_layout(struct rxkb_context *ctx, const char *name,
                                       const char *variant) {
    for (struct rxkb_layout *layout = ctx != nullptr ? rxkb_layout_first(ctx) : nullptr;
         layout != nullptr; layout = rxkb_layout_next(layout)) {
        const char *layout_variant = rxkb_layout_get_variant(layout);
        if (strcmp(rxkb_layout_get_name(layout), name) == 0 &&
            strcmp(layout_variant != nullptr ? layout_variant : "", variant) == 0)
            return layout;
    }
    return nullptr;
}

static const char *iso639_of(struct rxkb_layout *layout) {
    struct rxkb_iso639_code *iso_code =
        layout != nullptr ? rxkb_layout_get_iso639_first(layout) : nullptr;
    return iso_code != nullptr ? rxkb_iso639_code_get_code(iso_code) : nullptr;
}

static void set_code(language *lang, const char *from) {
    size_t count = 0;
    for (; from != nullptr && from[count] != '\0' && count < 2; count++)
        lang->code[count] = g_ascii_toupper(from[count]);
    lang->code[count] = '\0';
}

/* Re-reads [Keyboard] Layout and Variant when they changed. */
static void refresh(xd_shell *shell) {
    xd_langbar *lb = state(shell);
    const xd_keyboard_settings *keyboard = &shell->settings.keyboard;
    char key[sizeof(lb->key)];
    snprintf(key, sizeof(key), "%s;%s", keyboard->layout, keyboard->variant);
    if (lb->langs != nullptr && strcmp(key, lb->key) == 0)
        return;
    clear(lb);
    g_strlcpy(lb->key, key, sizeof(lb->key));
    char **layouts = g_strsplit(keyboard->layout, ",", -1);
    char **variants = g_strsplit(keyboard->variant, ",", -1);
    size_t count = g_strv_length(layouts);
    lb->langs = xd_alloc_array(XD_MAX(count, 1), sizeof(*lb->langs));
    if (count <= 1) {
        lb->langs[0] = (language){0};
        g_strfreev(layouts);
        g_strfreev(variants);
        return; /* one layout (or the default): nothing to show */
    }
    struct rxkb_context *ctx = rxkb_context_new(RXKB_CONTEXT_NO_FLAGS);
    if (ctx != nullptr && !rxkb_context_parse_default_ruleset(ctx)) {
        rxkb_context_unref(ctx);
        ctx = nullptr;
    }
    JsonParser *owner;
    JsonArray *table = iso639_table(&owner);
    size_t nv = g_strv_length(variants);
    for (size_t i = 0; i < count; i++) {
        char *name = g_strstrip(layouts[i]);
        const char *variant = i < nv ? g_strstrip(variants[i]) : "";
        struct rxkb_layout *layout = find_layout(ctx, name, variant);
        struct rxkb_layout *base = variant[0] != '\0' ? find_layout(ctx, name, "") : layout;
        const char *iso = iso639_of(layout);
        if (iso == nullptr)
            iso = iso639_of(base);
        language *lang = &lb->langs[lb->count++];
        const char *two = alpha2_of(table, iso);
        set_code(lang, two != nullptr ? two : iso != nullptr ? iso : name);
        const char *desc = layout != nullptr ? rxkb_layout_get_description(layout) : nullptr;
        lang->name = g_strdup(desc != nullptr ? desc : name);
    }
    if (owner != nullptr)
        g_object_unref(owner);
    if (ctx != nullptr)
        rxkb_context_unref(ctx);
    g_strfreev(layouts);
    g_strfreev(variants);
}

static bool shown(xd_shell *shell) {
    if (shell->arranger == nullptr ||
        xd_window_arranger_v1_get_version(shell->arranger) <
            XD_WINDOW_ARRANGER_V1_SET_KEYBOARD_LAYOUT_SINCE_VERSION)
        return false;
    refresh(shell);
    return shell->langbar->count > 1 && shell->langbar->told;
}

void xd_langbar_set_active(xd_shell *shell, uint32_t index) {
    xd_langbar *lb = state(shell);
    lb->active = index;
    lb->told = true;
    xd_taskbars_mark_dirty(shell);
}

xd_rect xd_langbar_layout(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, xd_rect room,
                          bool side, int row_h) {
    xd_shell *shell = bar->shell;
    bar->lang_rect = (xd_rect){0};
    if (!shown(shell))
        return room;
    xd_langbar *lb = shell->langbar;
    const char *code = lb->active < lb->count ? lb->langs[lb->active].code : "??";
    xd_font_spec bold = *xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_CENTER).font;
    bold.bold = true;
    xd_text_style st = xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_CENTER);
    st.font = &bold;
    int badge_w = XD_MAX(xd_tpx(theme, 18), xd_text_width(cairo, code, &st) + xd_tpx(theme, 4));
    int badge_h = xd_tpx(theme, 16);
    int gap = xd_tpx(theme, 2);
    xd_rect slot;
    if (side) {
        int height = XD_MIN(row_h, room.h);
        slot = (xd_rect){room.x, room.y + room.h - height, room.w, height};
        room.h -= height + gap;
    } else {
        int width = XD_MIN(badge_w + xd_tpx(theme, 8), room.w);
        slot = (xd_rect){room.x + room.w - width, room.y, width, XD_MIN(row_h, room.h)};
        room.w -= width + gap;
    }
    if (slot.w <= 0 || slot.h <= 0)
        return room;
    bar->lang_rect = slot;
    /* Hovered or held: a light or dark wash, as the Language bar's
     * buttons light up. */
    if (bar->hot == XD_HIT_LANG || bar->pressed == XD_HIT_LANG)
        xd_fill_rect(
            cairo, slot,
            xd_rgba_alpha(xd_rgba_hex(bar->pressed == XD_HIT_LANG ? 0x000000 : 0xFFFFFF), 0.2));
    /* The blue square: Windows 2000's navy; on the XP styles' own blue
     * taskbar a darker blue with a light rim, so it stands out. */
    xd_rect badge = {slot.x + (slot.w - badge_w) / 2, slot.y + (slot.h - badge_h) / 2, badge_w,
                     badge_h};
    if (theme->style == XD_STYLE_CLASSIC) {
        xd_fill_rect(cairo, badge, xd_rgba_hex(0x000080));
    } else {
        xd_fill_rect(cairo, badge, xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.55));
        xd_fill_rect(cairo, xd_rect_inset(badge, xd_tpx(theme, 1), xd_tpx(theme, 1)),
                     xd_rgba_hex(0x0F3A9E));
    }
    xd_draw_text(cairo, badge, code, &st);
    return room;
}

const char *xd_langbar_tooltip(xd_shell *shell) {
    if (!shown(shell))
        return nullptr;
    xd_langbar *lb = shell->langbar;
    return lb->active < lb->count ? lb->langs[lb->active].name : nullptr;
}

static void act_choose(xd_shell *shell, void *data) {
    uint32_t index = (uint32_t)(uintptr_t)data;
    if (shell->arranger != nullptr)
        xd_window_arranger_v1_set_keyboard_layout(shell->arranger, index);
}

static void act_settings(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-keyboard", nullptr));
}

void xd_langbar_click(xd_taskbar *bar, uint32_t button, uint32_t serial) {
    xd_shell *shell = bar->shell;
    if (!shown(shell) || (button != BTN_LEFT && button != BTN_RIGHT))
        return;
    xd_langbar *lb = shell->langbar;
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *menu_model = xd_menu_model_new(&shell->context_tree);
    if (button == BTN_LEFT) {
        /* "EN English (US)", the active one checked. */
        for (size_t i = 0; i < lb->count; i++) {
            char *label = g_strdup_printf("%s %s", lb->langs[i].code, lb->langs[i].name);
            xd_menu_item *it =
                xd_menu_add(&shell->context_tree, menu_model, XD_MENU_ACTION, label, nullptr);
            g_free(label);
            it->action = act_choose;
            it->action_data = (void *)(uintptr_t)i;
            it->checked = i == lb->active;
        }
    } else {
        xd_menu_item *it =
            xd_menu_add(&shell->context_tree, menu_model, XD_MENU_ACTION, "Settings...", nullptr);
        it->action = act_settings;
    }
    xd_rect rect = bar->lang_rect;
    xd_rect anchor = {(int)(rect.x / bar->scale), (int)(rect.y / bar->scale),
                      (int)(rect.w / bar->scale), (int)(rect.h / bar->scale)};
    xd_taskbar_take_keyboard(bar);
    shell->context_menu = true;
    xd_menu_open_root_bar(shell, menu_model, bar, anchor, serial);
    shell->start_bar = bar;
    bar->dirty = true;
}

void xd_langbar_finish(xd_shell *shell) {
    if (shell->langbar == nullptr)
        return;
    clear(shell->langbar);
    xd_free(shell->langbar);
    shell->langbar = nullptr;
}
