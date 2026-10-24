/* SPDX-License-Identifier: MIT
 * The two-column Start menus: Windows 7 and Windows XP.
 *
 * Windows XP (m->xp): a header with the user's picture and name, pinned
 * programs (Internet / E-mail as two-line entries) and the most used ones
 * on the left over "All Programs", which cascades into the Programs menus;
 * the user's folders, Control Panel and Run on the right; Log Off and Turn
 * Off Computer along the bottom. Luna, Watercolor (Whistler) and Classic
 * colours.
 *
 * Windows 7:
 * One popup with two columns. Left (white): pinned programs, frequently
 * used programs, "All Programs" (which swaps the list for a folder tree in
 * place) and a search box that takes any typing. Right (dark): the user's
 * picture overhanging the top edge, user folders, Recent Items, Control
 * Panel, Run and a Shut Down split button. ▸ items open ordinary popup
 * menus attached to this surface.
 */
#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <gio/gdesktopappinfo.h>
#include <pango/pango.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../core/alloc.h"
#include "../settings/state.h"
#include "../ui/image.h"
#include "../ui/textedit.h"
#include "recent.h"
#include "shell.h"

/* Layout, logical px. TOP is the transparent band the user picture sticks
 * up into, as on Windows 7. */
enum {
    S7_W = 440,
    S7_H = 520,
    S7_TOP = 22,
    S7_LEFT_W = 272,
    S7_PAD = 6,
    S7_ROW_LARGE = 40,
    S7_ROW_SMALL = 24,
    S7_ROW_SEP = 9,
    S7_ROW_HEADER = 22,
    S7_RIGHT_ROW = 30,
    S7_SEARCH_H = 24,
    S7_PICTURE = 48,
    S7_FREQUENT = 7,
    S7_DELAY_MS = 400,
};

typedef enum row_kind : uint8_t { ROW_APP, ROW_FOLDER, ROW_SEP, ROW_HEADER } row_kind;
typedef enum left_mode : uint8_t { MODE_HOME, MODE_ALL, MODE_SEARCH } left_mode;

typedef struct row {
    row_kind kind;
    const xd_app *app;  /* ROW_APP */
    const char *label;  /* ROW_FOLDER, ROW_HEADER */
    const char *icon;
    int folder;         /* ROW_FOLDER: index into folders */
    int indent;
    bool pinned;        /* XP: pinned programs are bold, Internet/E-mail two-line */
    bool jump;          /* Windows 7: has a jump list (the ▸ at its right) */
} row;

typedef enum right_kind : uint8_t {
    R_USER,
    R_DOCUMENTS,
    R_PICTURES,
    R_MUSIC,
    R_RECENT,
    R_COMPUTER,
    R_SEP,
    R_CONTROL,
    R_DEVICES,
    R_DEFAULTS,
    R_HELP,
    R_RUN,
    R_COUNT,
} right_kind;

static const struct {
    const char *label;
    bool submenu;
    bool disabled;
} right_items[R_COUNT] = {
    [R_USER] = {"", false, false},
    [R_DOCUMENTS] = {"Documents", false, false},
    [R_PICTURES] = {"Pictures", false, false},
    [R_MUSIC] = {"Music", false, false},
    [R_RECENT] = {"Recent Items", true, false},
    [R_COMPUTER] = {"Computer", false, false},
    [R_SEP] = {"", false, true},
    [R_CONTROL] = {"Control Panel", true, false},
    [R_DEVICES] = {"Devices and Printers", false, false},
    [R_DEFAULTS] = {"Default Programs", false, false},
    [R_HELP] = {"Help and Support", false, false},
    [R_RUN] = {"Run...", false, false},
};

/* Hit targets on the right besides items. */
enum {
    HIT_SHUTDOWN = 100,
    HIT_SHUTDOWN_ARROW = 101,
    HIT_ALL = 102,
    HIT_SEARCH = 103,
    HIT_LOGOFF = 104,     /* XP */
    HIT_TURNOFF = 105,    /* XP */
    HIT_XP_PRINTERS = 106, /* XP */
    HIT_XP_SEARCH = 107,   /* XP */
};

/* Windows XP layout, logical px. */
enum {
    XP_W = 420,
    XP_LEFT_W = 190,
    XP_BODY_H = 372, /* the least; more programs make it taller */
    XP_PAD = 6,
    XP_PIN_ROW = 42,
    XP_ROW = 36,
    XP_ALL_H = 30,
    XP_SEP = 9,
    XP_BOLD_ROW = 30,
    XP_RIGHT_ROW = 26,
    XP_TALL_ROW = 36,
    XP_PICTURE = 48,
    /* Customize Start Menu's "Small icons": 16px icons in shorter rows. */
    XP_ROW_SMALL = 22,
    XP_PIN_ROW_SMALL = 32, /* Internet and E-mail keep their two lines */
};

typedef struct xp_item {
    int hit; /* right_kind or HIT_* */
    const char *label, *icon;
    bool bold, submenu, disabled, tall;
} xp_item;

static const xp_item xp_items[] = {
    {R_DOCUMENTS, "My Documents", "folder-documents", true, false, false, false},
    {R_RECENT, "My Recent Documents", "document-open-recent", true, true, false, false},
    {R_PICTURES, "My Pictures", "folder-pictures", true, false, false, false},
    {R_MUSIC, "My Music", "folder-music", true, false, false, false},
    {R_COMPUTER, "My Computer", "computer", true, false, false, false},
    {R_SEP, nullptr, nullptr, false, false, true, false},
    {R_CONTROL, "Control Panel", "preferences-system", false, true, false, false},
    {R_DEFAULTS, "Set Program Access and Defaults", "preferences-desktop-default-applications",
     false, false, false, true},
    {HIT_XP_PRINTERS, "Printers and Faxes", "printer", false, false, false, false},
    {R_SEP, nullptr, nullptr, false, false, true, false},
    {R_HELP, "Help and Support", "help-browser", false, false, false, false},
    {HIT_XP_SEARCH, "Search", "system-search", false, false, false, false},
    {R_RUN, "Run...", "system-run", false, false, false, false},
};

/* XP's underlined letters, for the items whose letter is known (as in
 * XP's Win, U, U); the others go by their first letter, not underlined. */
static uint32_t xp_key(int hit) {
    switch (hit) {
    case HIT_ALL:
        return 'p'; /* All &Programs */
    case R_RUN:
        return 'r'; /* &Run... */
    case HIT_LOGOFF:
        return 'l'; /* &Log Off */
    case HIT_TURNOFF:
        return 'u'; /* T&urn Off Computer */
    default:
        return 0;
    }
}

struct xd_start7 {
    xd_shell *shell;
    xd_taskbar *bar;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    double scale;
    bool configured, shown, dirty;
    bool xp;                /* Windows XP layout */
    xd_visual_style look;   /* style when opened (XP header and footer sizes) */
    int w, h;               /* logical size */
    bool large;             /* XP: the program list's large icons */
    int big_icon;           /* XP: their size; 48 with Effects' "Use large icons" */
    int body_h;             /* XP: between the header and the footer */
    double press_x, press_y; /* where a program was pressed, to drag it */

    left_mode mode;
    row *rows;
    size_t row_count, row_cap;
    int scroll;            /* first row shown in ALL/SEARCH */
    bool *expanded;        /* ALL: per folder */
    const char **folders;  /* ALL: folder names in order */
    const char **folder_icons;
    size_t folder_count;
    xd_textedit search;
    guint caret_timer;     /* the search box's caret blinking, or 0 */
    bool caret_hidden;     /* in its off phase */

    int hot_left;          /* row index or -1 */
    int hot_right;         /* right_kind, HIT_*, or -1 */
    int pressed;           /* hit code pressed, for click-on-release */

    xd_menu_tree tree;     /* submenu models */
    xd_menu *sub;
    int sub_owner;         /* right item / HIT_SHUTDOWN_ARROW / OWNER_CONTEXT that opened `sub` */
    int context_row;       /* the program row right-clicked (OWNER_CONTEXT) */
    double ptr_x, ptr_y;   /* the pointer, where the right-click menu opens */
    bool pending_hover;
    bool hot_arrow;        /* the pointer is on hot_left's jump list ▸ */
    int jump_row;          /* the row whose jump list is open (OWNER_JUMP) */

    cairo_surface_t *picture;
    char user[128];
    uint32_t serial;
};

/* sub_owner for a program's jump list. */
static constexpr int OWNER_JUMP = 5001;
/* How far a pressed program moves before it's dragged. */
static constexpr double S7_DRAG_PX = 5;
/* Width of a program's jump list ▸ at the right of its row. */
static constexpr int S7_JUMP_W = 24;

/* ---- palettes ------------------------------------------------------------------------ */

typedef struct s7_palette {
    xd_rgba left_bg, left_text, left_dim, left_hot, left_hot_text, left_hot_border;
    xd_rgba right_bg, right_text, right_dim, right_hot, right_sep;
    xd_rgba frame, separator;
    bool classic;
    bool square; /* Watercolor: square highlights, as its menus have */
} s7_palette;

static s7_palette palette(const xd_theme *theme) {
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    switch (theme->style) {
    case XD_STYLE_CLASSIC:
        return (s7_palette){
            .left_bg = xd_sys(theme, XD_COLOR_WINDOW),
            .left_text = xd_sys(theme, XD_COLOR_WINDOW_TEXT),
            .left_dim = xd_sys(theme, XD_COLOR_GRAY_TEXT),
            .left_hot = xd_sys(theme, XD_COLOR_HILIGHT),
            .left_hot_text = xd_sys(theme, XD_COLOR_HILIGHT_TEXT),
            .left_hot_border = none,
            .right_bg = xd_sys(theme, XD_COLOR_MENU),
            .right_text = xd_sys(theme, XD_COLOR_MENU_TEXT),
            .right_dim = xd_sys(theme, XD_COLOR_GRAY_TEXT),
            .right_hot = xd_sys(theme, XD_COLOR_HILIGHT),
            .right_sep = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
            .frame = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
            .separator = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
            .classic = true,
        };
    case XD_STYLE_WHISTLER:
        /* Watercolor's, as its XP menu (xp_colors) and taskbar have them:
         * the white list inset in the flat blue body, white text on blue
         * on the right, its menus' blue highlight. */
        return (s7_palette){
            .left_bg = xd_rgba_hex(0xFFFFFF),
            .left_text = xd_rgba_hex(0x000000),
            .left_dim = xd_rgba_hex(0x7A7A7A),
            .left_hot = xd_rgba_hex(0x316AC5),
            .left_hot_text = xd_rgba_hex(0xFFFFFF),
            .left_hot_border = xd_rgba_hex(0x316AC5),
            .right_bg = xd_rgba_hex(0x3573D6),
            .right_text = xd_rgba_hex(0xFFFFFF),
            .right_dim = xd_rgba_hex(0xA9C7F2),
            .right_hot = xd_rgba_hex(0x5297F9),
            .right_sep = xd_rgba_hex(0x6FA2EA),
            .frame = xd_rgba_hex(0x2B5FB5),
            .separator = xd_rgba_hex(0xD8D8D0),
            .square = true,
        };
    case XD_STYLE_LUNA:
    case XD_STYLE_AERO: /* Windows 7's own colours */
    case XD_STYLE_COUNT:
        break;
    }
    /* Luna: white list, dark navy panel (the reference look). The panels are
     * opaque: Windows 7 blurs what's behind its glass, and without blur in
     * the compositor a see-through panel just shows the wallpaper sharply.
     * Aero with the compositor's blur: the panel and frame are glass.
     * Aero's are in the glass colour (Window Color and Appearance). */
    if (theme->style == XD_STYLE_AERO) {
        xd_glass_colors glass;
        xd_theme_glass_colors(theme, &glass);
        bool see = glass.translucent;
        return (s7_palette){
            .left_bg = xd_rgba_hex(0xFFFFFF),
            .left_text = xd_rgba_hex(0x000000),
            .left_dim = xd_rgba_hex(0x6D6D6D),
            .left_hot = xd_rgba_hex(0xDCEBFC),
            .left_hot_text = xd_rgba_hex(0x000000),
            .left_hot_border = xd_rgba_hex(0x7DA2CE),
            .right_bg = glass.menu,
            .right_text = xd_rgba_hex(0xFFFFFF),
            .right_dim = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), see ? 0.55 : 0.45),
            .right_hot = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), see ? 0.22 : 0.18),
            .right_sep = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), see ? 0.3 : 0.25),
            .frame = glass.menu_frame,
            .separator = xd_rgba_hex(0xD6E5F5),
        };
    }
    return (s7_palette){
        .left_bg = xd_rgba_hex(0xFFFFFF),
        .left_text = xd_rgba_hex(0x000000),
        .left_dim = xd_rgba_hex(0x6D6D6D),
        .left_hot = xd_rgba_hex(0xDCEBFC),
        .left_hot_text = xd_rgba_hex(0x000000),
        .left_hot_border = xd_rgba_hex(0x7DA2CE),
        .right_bg = xd_rgba_hex(0x1B3A5C),
        .right_text = xd_rgba_hex(0xFFFFFF),
        .right_dim = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.45),
        .right_hot = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.18),
        .right_sep = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.25),
        .frame = xd_rgba_hex(0x0E2440),
        .separator = xd_rgba_hex(0xD6E5F5),
    };
}

/* A theme package's colours for the parts it styles. */
static void pkg_color(const xd_theme *theme, xd_start_part part, xd_start_color which,
                      xd_rgba *color) {
    (void)xd_theme_start_part_color(theme, part, 0, which, color);
}

static s7_palette palette_7(const xd_theme *theme) {
    s7_palette menu_colors = palette(theme);
    pkg_color(theme, XD_START_PROG_LIST, XD_START_TEXT, &menu_colors.left_text);
    pkg_color(theme, XD_START_PROG_LIST, XD_START_TEXT, &menu_colors.left_hot_text);
    pkg_color(theme, XD_START_PLACES_LIST, XD_START_TEXT, &menu_colors.right_text);
    return menu_colors;
}

/* ---- geometry (logical) ------------------------------------------------------------ */

static xd_rect frame_rect(void) { return (xd_rect){0, S7_TOP, S7_W, S7_H - S7_TOP}; }

/* ---- Windows XP geometry ---- */
static bool watercolor(const xd_start7 *menu) { return menu->look == XD_STYLE_WHISTLER; }
static int xp_head(const xd_start7 *menu) { return watercolor(menu) ? 24 : 56; }
static int xp_foot(const xd_start7 *menu) { return watercolor(menu) ? 34 : 40; }
/* The white program list; on Watercolor it's inset in the blue body. */
static xd_rect xp_left_panel(const xd_start7 *menu) {
    int top = xp_head(menu);
    if (watercolor(menu))
        return (xd_rect){4, top + 3, XP_LEFT_W - 4, menu->body_h - 6};
    return (xd_rect){0, top, XP_LEFT_W, menu->body_h};
}
static int xp_item_height(const xp_item *it) {
    return it->hit == R_SEP ? XP_SEP : it->tall ? XP_TALL_ROW : it->bold ? XP_BOLD_ROW : XP_RIGHT_ROW;
}
static xd_rect xp_item_rect(const xd_start7 *menu, size_t index) {
    int y = xp_head(menu) + XP_PAD;
    for (size_t i = 0; i < index; i++)
        y += xp_item_height(&xp_items[i]);
    return (xd_rect){XP_LEFT_W + 4, y, menu->w - XP_LEFT_W - 8, xp_item_height(&xp_items[index])};
}
static xd_rect xp_turnoff_rect(const xd_start7 *menu) {
    int foot = xp_foot(menu);
    return (xd_rect){menu->w - 8 - 140, menu->h - foot + (foot - 28) / 2, 140, 28};
}
static xd_rect xp_logoff_rect(const xd_start7 *menu) {
    xd_rect rect = xp_turnoff_rect(menu);
    return (xd_rect){rect.x - 6 - 82, rect.y, 82, rect.h};
}
/* Whether the theme package draws 7's program list (its image carries
 * its own frame and rounded corners, the glass showing round them). */
static bool pkg_list(const xd_start7 *menu, xd_insets *cm) {
    xd_theme theme;
    xd_theme_init(&theme, menu->shell->style, &menu->shell->scheme, 1.0);
    xd_insets insets;
    if (cm == nullptr)
        cm = &insets;
    return !menu->xp && xd_theme_start_part_margins(&theme, XD_START_PROG_LIST, cm);
}
/* The white list's panel, logical px: inset in the left column, or a
 * package's, the whole column, as Windows 7 draws ProgList. */
static xd_rect list_panel_rect(const xd_start7 *menu) {
    if (pkg_list(menu, nullptr))
        return (xd_rect){0, S7_TOP, S7_LEFT_W, S7_H - S7_TOP};
    return (xd_rect){S7_PAD - 2, S7_TOP + S7_PAD - 2, S7_LEFT_W - 2 * S7_PAD + 4,
                     S7_H - S7_TOP - 2 * S7_PAD + 4};
}
/* A package's OpenBox: the search box's own pane at the panel's bottom,
 * as tall as its ContentMargins around the box ({0} without one; the box
 * then sits in the list). `cm` (may be nullptr) gets the margins. */
static xd_rect open_box_rect(const xd_start7 *menu, xd_insets *cm) {
    xd_theme theme;
    xd_theme_init(&theme, menu->shell->style, &menu->shell->scheme, 1.0);
    xd_insets insets;
    if (menu->xp || !xd_theme_start_part_margins(&theme, XD_START_OPEN_BOX, &insets))
        return (xd_rect){0};
    if (cm != nullptr)
        *cm = insets;
    xd_rect rect = list_panel_rect(menu);
    int height = insets.top + S7_SEARCH_H + insets.bottom;
    return (xd_rect){rect.x, rect.y + rect.h - height, rect.w, height};
}
static xd_rect search_rect(const xd_start7 *menu) {
    xd_insets cm;
    xd_rect ob = open_box_rect(menu, &cm);
    if (ob.w > 0)
        return (xd_rect){ob.x + cm.left, ob.y + cm.top, ob.w - cm.left - cm.right, S7_SEARCH_H};
    return (xd_rect){S7_PAD + 4, S7_H - S7_PAD - S7_SEARCH_H - 4, S7_LEFT_W - 2 * S7_PAD - 8,
                     S7_SEARCH_H};
}
static xd_rect all_rect(const xd_start7 *menu) {
    if (menu->xp) {
        xd_rect panel = xp_left_panel(menu);
        return (xd_rect){panel.x + 2, panel.y + panel.h - XP_PAD - XP_ALL_H, panel.w - 4, XP_ALL_H};
    }
    xd_rect ob = open_box_rect(menu, nullptr);
    if (ob.w > 0)
        return (xd_rect){S7_PAD, ob.y - 2 - 28, S7_LEFT_W - 2 * S7_PAD, 28};
    xd_rect search_box = search_rect(menu);
    return (xd_rect){S7_PAD, search_box.y - 8 - 28, S7_LEFT_W - 2 * S7_PAD, 28};
}
static xd_rect list_rect(const xd_start7 *menu) {
    xd_rect all_programs = all_rect(menu);
    if (menu->xp) {
        xd_rect panel = xp_left_panel(menu);
        return (xd_rect){panel.x + 2, panel.y + XP_PAD, panel.w - 4,
                         all_programs.y - XP_SEP - (panel.y + XP_PAD)};
    }
    /* A package's list: within its ProgList's ContentMargins, clear of
     * the frame its image draws. */
    xd_insets cm;
    if (pkg_list(menu, &cm)) {
        xd_rect panel = list_panel_rect(menu);
        int x = XD_MAX(panel.x + cm.left, S7_PAD), y = XD_MAX(panel.y + cm.top, S7_TOP + S7_PAD);
        int right = XD_MIN(panel.x + panel.w - cm.right, S7_LEFT_W - S7_PAD);
        return (xd_rect){x, y, right - x, all_programs.y - 6 - y};
    }
    return (xd_rect){S7_PAD, S7_TOP + S7_PAD, S7_LEFT_W - 2 * S7_PAD,
                     all_programs.y - 6 - (S7_TOP + S7_PAD)};
}
static int right_x(void) { return S7_LEFT_W; }
static xd_rect right_row(int i) {
    return (xd_rect){right_x() + 6, S7_TOP + S7_PICTURE - 4 + i * S7_RIGHT_ROW,
                     S7_W - right_x() - 12, i == R_SEP ? 10 : S7_RIGHT_ROW};
}
static xd_rect right_item(int k) {
    /* Rows after the separator are shifted by its height difference. */
    xd_rect rect = right_row(k);
    if (k > R_SEP)
        rect.y -= S7_RIGHT_ROW - 10;
    return rect;
}
static xd_rect shutdown_rect(void) {
    return (xd_rect){right_x() + 12, S7_H - S7_PAD - 30, 96, 26};
}
static xd_rect shutdown_arrow_rect(void) {
    xd_rect rect = shutdown_rect();
    return (xd_rect){rect.x + rect.w, rect.y, 24, rect.h};
}
static xd_rect picture_rect(void) {
    int cx = right_x() + (S7_W - right_x()) / 2;
    return (xd_rect){cx - S7_PICTURE / 2, 2, S7_PICTURE, S7_PICTURE};
}

static int row_height(left_mode mode, const row *menu_row);
static const char *app_role(const xd_app *app);
static int row_h(const xd_start7 *menu, const row *menu_row) {
    if (menu->xp) {
        if (menu_row->kind == ROW_SEP)
            return XP_SEP;
        if (menu->large)
            return (menu_row->pinned ? XP_PIN_ROW : XP_ROW) + menu->big_icon - 32;
        return menu_row->pinned && menu_row->kind == ROW_APP && app_role(menu_row->app) != nullptr
                   ? XP_PIN_ROW_SMALL
                   : XP_ROW_SMALL;
    }
    return row_height(menu->mode, menu_row);
}

static int row_height(left_mode mode, const row *menu_row) {
    switch (menu_row->kind) {
    case ROW_SEP:
        return S7_ROW_SEP;
    case ROW_HEADER:
        return S7_ROW_HEADER;
    case ROW_APP:
    case ROW_FOLDER:
        break;
    }
    return mode == MODE_HOME ? S7_ROW_LARGE : S7_ROW_SMALL;
}

static bool contains(xd_rect rect, double x, double y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

/* ---- left column content -------------------------------------------------------- */

static void push_row(xd_start7 *menu, row menu_row) {
    if (menu->row_count == menu->row_cap) {
        menu->row_cap = menu->row_cap == 0 ? 32 : menu->row_cap * 2;
        menu->rows = xd_realloc_array(menu->rows, menu->row_cap, sizeof(*menu->rows));
    }
    menu->rows[menu->row_count++] = menu_row;
}

static const xd_app *find_in(const xd_app_list *list, const char *id) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].id, id) == 0)
            return &list->items[i];
    }
    return nullptr;
}

/* Programs, then Control Panel applets (found by the search, and so
 * pinned or counted from there too). */
static const xd_app *find_app(const xd_shell *shell, const char *id) {
    const xd_app *app = find_in(&shell->apps, id);
    return app != nullptr ? app : find_in(&shell->control_apps, id);
}

static void pinned_path(xd_strbuf *path) {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (cfg != nullptr && cfg[0] == '/')
        xd_strbuf_appendf(path, "%s/xeondesktop/start-pinned", cfg);
    else
        xd_strbuf_appendf(path, "%s/.config/xeondesktop/start-pinned", home != nullptr ? home : "");
}

/* Pinned ids: ~/.config/xeondesktop/start-pinned, one desktop id per line.
 * Without the file: the browser and the terminal, like Quick Launch. */
static void load_pinned(const xd_shell *shell, const xd_app **out, size_t *count, size_t max) {
    *count = 0;
    xd_strbuf path = {0};
    xd_strbuf text = {0};
    pinned_path(&path);
    if (xd_read_file(xd_strbuf_cstr(&path), 64 * 1024, &text)) {
        xd_str rest = xd_strbuf_view(&text);
        while (rest.len > 0 && *count < max) {
            size_t nl = rest.len;
            bool more = xd_str_find_char(rest, '\n', &nl);
            char *id = xd_str_dup_cstr(xd_str_trim(xd_str_sub(rest, 0, nl)));
            const xd_app *app = id[0] != '\0' ? find_app(shell, id) : nullptr;
            if (app != nullptr)
                out[(*count)++] = app;
            xd_free(id);
            rest = more ? xd_str_sub(rest, nl + 1, rest.len) : XD_STR("");
        }
    } else {
        static const char *const defaults[] = {
            "firefox.desktop",   "org.mozilla.firefox.desktop", "chromium.desktop",
            "google-chrome.desktop", "Alacritty.desktop",       "foot.desktop",
            "org.gnome.Console.desktop", "org.kde.konsole.desktop", "qterminal.desktop"};
        bool have_browser = false;
        bool have_terminal = false;
        for (size_t i = 0; i < XD_ARRAY_LEN(defaults) && *count < max; i++) {
            bool browser = i < 4;
            if ((browser && have_browser) || (!browser && have_terminal))
                continue;
            const xd_app *app = find_app(shell, defaults[i]);
            if (app != nullptr) {
                out[(*count)++] = app;
                if (browser)
                    have_browser = true;
                else
                    have_terminal = true;
            }
        }
    }
    xd_strbuf_free(&text);
    xd_strbuf_free(&path);
}

static constexpr size_t MAX_PINNED = 16;

/* Pin to / Unpin from Start menu: rewrites start-pinned (starting from the
 * defaults when there's no file yet). */
static void set_pinned(const xd_shell *shell, const char *id, bool pin) {
    const xd_app *apps[MAX_PINNED];
    size_t count = 0;
    load_pinned(shell, apps, &count, MAX_PINNED);
    GString *text = g_string_new(nullptr);
    bool present = false;
    for (size_t i = 0; i < count; i++) {
        bool same = strcmp(apps[i]->id, id) == 0;
        present = present || same;
        if (!same || pin)
            g_string_append_printf(text, "%s\n", apps[i]->id);
    }
    if (pin && !present && count < MAX_PINNED)
        g_string_append_printf(text, "%s\n", id);
    xd_strbuf path = {0};
    pinned_path(&path);
    char *dir = g_path_get_dirname(xd_strbuf_cstr(&path));
    GError *error = nullptr;
    if (g_mkdir_with_parents(dir, 0700) != 0 ||
        !g_file_set_contents(xd_strbuf_cstr(&path), text->str, (gssize)text->len, &error)) {
        fprintf(stderr, "xd-shell: cannot save %s: %s\n", xd_strbuf_cstr(&path),
                error != nullptr ? error->message : g_strerror(errno));
        g_clear_error(&error);
    }
    g_free(dir);
    xd_strbuf_free(&path);
    g_string_free(text, TRUE);
}

bool xd_start7_pinned(const xd_shell *shell, const char *id, bool *full) {
    const xd_app *apps[MAX_PINNED];
    size_t count = 0;
    load_pinned(shell, apps, &count, MAX_PINNED);
    bool pinned = false;
    for (size_t i = 0; i < count; i++)
        pinned = pinned || strcmp(apps[i]->id, id) == 0;
    if (full != nullptr)
        *full = count >= MAX_PINNED;
    return pinned;
}

void xd_start7_set_pinned(const xd_shell *shell, const char *id, bool pin) {
    set_pinned(shell, id, pin);
}

/* Remove from this list: forgets a program's launches. */
static void forget_launches(const char *id) {
    char **lines = nullptr;
    size_t count = 0;
    xd_state_read_lines("start-frequent", &lines, &count, 200);
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab != nullptr && strcmp(tab + 1, id) == 0) {
            xd_free(lines[i]);
            continue;
        }
        lines[kept++] = lines[i];
    }
    if (!xd_state_write_lines("start-frequent", lines, kept))
        fprintf(stderr, "xd-shell: cannot save launch counts\n");
    xd_free_lines(lines, kept);
}

/* When each program was last started: "seconds<TAB>id" lines, the latest
 * first, for Add or Remove Programs' "Last Used On" (the counts have no
 * dates). */
static void note_launch_time(const char *id) {
    char **lines = nullptr;
    size_t count = 0;
    xd_state_read_lines("start-last-used", &lines, &count, 200);
    char **out = xd_alloc_array(count + 1, sizeof(char *));
    size_t kept = 0;
    xd_strbuf entry = {0};
    xd_strbuf_appendf(&entry, "%lld\t%s", (long long)g_get_real_time() / G_USEC_PER_SEC, id);
    out[kept++] = xd_str_dup_cstr(xd_strbuf_view(&entry));
    for (size_t i = 0; i < count && kept < 200; i++) {
        xd_str line = xd_str_from_cstr(lines[i]);
        size_t tab;
        if (!xd_str_find_char(line, '\t', &tab) ||
            xd_str_eq(xd_str_sub(line, tab + 1, line.len), xd_str_from_cstr(id)))
            continue;
        out[kept++] = xd_str_dup_cstr(line);
    }
    if (!xd_state_write_lines("start-last-used", out, kept))
        fprintf(stderr, "xd-shell: cannot save launch times\n");
    xd_strbuf_free(&entry);
    xd_free_lines(out, kept);
    xd_free_lines(lines, count);
}

void xd_start7_count_launch(const char *id) {
    note_launch_time(id);
    xd_start_menu_program_opened(id); /* not "newly installed" any more */
    /* "count<TAB>id" lines; the most used come first. */
    char **lines = nullptr;
    size_t line_count = 0;
    xd_state_read_lines("start-frequent", &lines, &line_count, 200);
    int64_t count = 0;
    char **out = xd_alloc_array(line_count + 1, sizeof(char *));
    size_t kept = 0;
    for (size_t i = 0; i < line_count; i++) {
        xd_str line = xd_str_from_cstr(lines[i]);
        size_t tab;
        int64_t launches;
        if (!xd_str_find_char(line, '\t', &tab) ||
            !xd_str_parse_i64(xd_str_sub(line, 0, tab), 0, 1000000, &launches))
            continue;
        if (xd_str_eq(xd_str_sub(line, tab + 1, line.len), xd_str_from_cstr(id))) {
            count = launches;
            continue;
        }
        out[kept++] = xd_str_dup_cstr(line);
    }
    xd_strbuf entry = {0};
    xd_strbuf_appendf(&entry, "%lld\t%s", (long long)(count + 1), id);
    /* Insert in descending count order. */
    size_t pos = 0;
    while (pos < kept && strtoll(out[pos], nullptr, 10) > count + 1)
        pos++;
    memmove(out + pos + 1, out + pos, (kept - pos) * sizeof(char *));
    out[pos] = xd_str_dup_cstr(xd_strbuf_view(&entry));
    kept++;
    if (!xd_state_write_lines("start-frequent", out, kept))
        fprintf(stderr, "xd-shell: cannot save launch counts\n");
    xd_strbuf_free(&entry);
    xd_free_lines(out, kept);
    xd_free_lines(lines, line_count);
}

static void build_home(xd_start7 *menu) {
    xd_shell *shell = menu->shell;
    menu->row_count = 0;
    menu->scroll = 0;
    const xd_app *pinned[MAX_PINNED];
    size_t np = 0;
    load_pinned(shell, pinned, &np, XD_ARRAY_LEN(pinned));
    for (size_t i = 0; i < np; i++)
        push_row(menu, (row){.kind = ROW_APP,
                             .app = pinned[i],
                             .pinned = true,
                             .jump = !menu->xp && xd_jumplist_has_items(shell, pinned[i]->id)});

    char **lines = nullptr;
    size_t count = 0;
    xd_state_read_lines("start-frequent", &lines, &count, 64);
    bool sep = false;
    size_t shown = 0;
    size_t most = menu->xp ? (size_t)XD_MAX(shell->settings.start_programs, 0) : S7_FREQUENT;
    for (size_t i = 0; i < count && shown < most; i++) {
        xd_str line = xd_str_from_cstr(lines[i]);
        size_t tab;
        if (!xd_str_find_char(line, '\t', &tab))
            continue;
        char *id = xd_str_dup_cstr(xd_str_sub(line, tab + 1, line.len));
        const xd_app *app = find_app(shell, id);
        xd_free(id);
        bool is_pinned = false;
        for (size_t pin_index = 0; pin_index < np; pin_index++)
            is_pinned = is_pinned || pinned[pin_index] == app;
        if (app == nullptr || is_pinned)
            continue;
        if (!sep && np > 0) {
            push_row(menu, (row){.kind = ROW_SEP});
            sep = true;
        }
        push_row(menu, (row){.kind = ROW_APP,
                             .app = app,
                             .jump = !menu->xp && xd_jumplist_has_items(shell, app->id)});
        shown++;
    }
    xd_free_lines(lines, count);
}

static void build_all(xd_start7 *menu) {
    xd_shell *shell = menu->shell;
    menu->row_count = 0;
    /* Folder list (apps are sorted by folder). */
    if (menu->folders == nullptr) {
        menu->folders = xd_alloc_array(XD_MAX(shell->apps.count, 1), sizeof(char *));
        menu->folder_icons = xd_alloc_array(XD_MAX(shell->apps.count, 1), sizeof(char *));
        menu->expanded = xd_alloc_zeroed(XD_MAX(shell->apps.count, 1), sizeof(bool));
        for (size_t i = 0; i < shell->apps.count; i++) {
            const xd_app *folder_app = &shell->apps.items[i];
            if (menu->folder_count == 0 ||
                strcmp(menu->folders[menu->folder_count - 1], folder_app->folder) != 0) {
                menu->folders[menu->folder_count] = folder_app->folder;
                menu->folder_icons[menu->folder_count] = folder_app->folder_icon;
                menu->folder_count++;
            }
        }
    }
    size_t app_index = 0;
    for (size_t folder_index = 0; folder_index < menu->folder_count; folder_index++) {
        push_row(menu, (row){.kind = ROW_FOLDER,
                             .label = menu->folders[folder_index],
                             .icon = menu->folder_icons[folder_index],
                             .folder = (int)folder_index});
        for (; app_index < shell->apps.count &&
               strcmp(shell->apps.items[app_index].folder, menu->folders[folder_index]) == 0;
             app_index++) {
            if (menu->expanded[folder_index])
                push_row(menu,
                         (row){.kind = ROW_APP, .app = &shell->apps.items[app_index], .indent = 1});
        }
    }
}

static bool contains_nocase(const char *text, xd_str query) {
    xd_str text_view = xd_str_from_cstr(text);
    for (size_t i = 0; query.len <= text_view.len && i + query.len <= text_view.len; i++) {
        if (xd_str_eq_nocase(xd_str_sub(text_view, i, query.len), query))
            return true;
    }
    return false;
}

/* Name or Keywords (e.g. "browser" finds Firefox), case-insensitively. */
static bool matches(const xd_app *app, xd_str query) {
    if (query.len == 0)
        return true;
    return contains_nocase(app->name, query) ||
           (app->keywords != nullptr && contains_nocase(app->keywords, query));
}

/* One group of results under its heading, if anything matches: names
 * starting with the query first, then other matches. */
static void search_group(xd_start7 *menu, const char *heading, const xd_app_list *list,
                         xd_str query, const xd_app_list *skip) {
    size_t header = menu->row_count;
    push_row(menu, (row){.kind = ROW_HEADER, .label = heading});
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < list->count; i++) {
            const xd_app *candidate = &list->items[i];
            bool prefix = xd_str_starts_with(xd_str_from_cstr(candidate->name), query) ||
                          (query.len <= strlen(candidate->name) &&
                           xd_str_eq_nocase(
                               xd_str_sub(xd_str_from_cstr(candidate->name), 0, query.len), query));
            if (matches(candidate, query) && prefix == (pass == 0) &&
                (skip == nullptr || find_in(skip, candidate->id) == nullptr))
                push_row(menu, (row){.kind = ROW_APP, .app = candidate});
        }
    }
    if (menu->row_count == header + 1)
        menu->row_count = header;
}

/* Windows 7 groups the results: Programs, then Control Panel (the applets
 * are hidden from All Programs, so this is where they're found). */
static void build_search(xd_start7 *menu) {
    xd_shell *shell = menu->shell;
    menu->row_count = 0;
    menu->scroll = 0;
    xd_str query = xd_str_trim(xd_textedit_view(&menu->search));
    search_group(menu, "Programs", &shell->apps, query, nullptr);
    search_group(menu, "Control Panel", &shell->control_apps, query, &shell->apps);
    if (menu->row_count == 0)
        push_row(menu, (row){.kind = ROW_HEADER, .label = "No items match your search."});
}

static void rebuild(xd_start7 *menu) {
    switch (menu->mode) {
    case MODE_HOME:
        build_home(menu);
        break;
    case MODE_ALL:
        build_all(menu);
        break;
    case MODE_SEARCH:
        build_search(menu);
        break;
    }
    menu->hot_left = -1;
    menu->dirty = true;
}

/* Rows that fit, from `scroll`. */
static int visible_rows(const xd_start7 *menu, int *heights_total) {
    xd_rect lr = list_rect(menu);
    int y = 0;
    int count = 0;
    for (size_t i = (size_t)menu->scroll; i < menu->row_count; i++) {
        int height = row_h(menu, &menu->rows[i]);
        if (y + height > lr.h)
            break;
        y += height;
        count++;
    }
    if (heights_total != nullptr)
        *heights_total = y;
    return count;
}

static int row_at(const xd_start7 *menu, double x, double y) {
    xd_rect lr = list_rect(menu);
    if (!contains(lr, x, y))
        return -1;
    double top = lr.y;
    int count = visible_rows(menu, nullptr);
    for (int k = 0; k < count; k++) {
        size_t i = (size_t)(menu->scroll + k);
        int height = row_h(menu, &menu->rows[i]);
        if (y >= top && y < top + height)
            return (menu->rows[i].kind == ROW_APP || menu->rows[i].kind == ROW_FOLDER) ? (int)i
                                                                                       : -1;
        top += height;
    }
    return -1;
}

/* Where visible row `index` is; false if it isn't shown. */
static bool row_rect(const xd_start7 *menu, int index, xd_rect *out) {
    xd_rect lr = list_rect(menu);
    int top = lr.y;
    int count = visible_rows(menu, nullptr);
    for (int k = 0; k < count; k++) {
        int i = menu->scroll + k;
        int height = row_h(menu, &menu->rows[i]);
        if (i == index) {
            *out = (xd_rect){lr.x, top, lr.w, height};
            return true;
        }
        top += height;
    }
    return false;
}

static xd_rect jump_arrow_rect(xd_rect rr) {
    return (xd_rect){rr.x + rr.w - S7_JUMP_W, rr.y, S7_JUMP_W, rr.h};
}

/* On row `index`'s jump list ▸ (the Windows 7 menu's programs). */
static bool on_jump_arrow(const xd_start7 *menu, int index, double x, double y) {
    xd_rect rect;
    return index >= 0 && menu->mode == MODE_HOME && menu->rows[index].jump &&
           row_rect(menu, index, &rect) && contains(jump_arrow_rect(rect), x, y);
}

static int right_at(const xd_start7 *menu, double x, double y) {
    if (menu->xp) {
        if (contains(xp_logoff_rect(menu), x, y))
            return HIT_LOGOFF;
        if (contains(xp_turnoff_rect(menu), x, y))
            return HIT_TURNOFF;
        for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++) {
            if (xp_items[i].hit != R_SEP && contains(xp_item_rect(menu, i), x, y))
                return xp_items[i].hit;
        }
        return -1;
    }
    if (contains(shutdown_rect(), x, y))
        return HIT_SHUTDOWN;
    if (contains(shutdown_arrow_rect(), x, y))
        return HIT_SHUTDOWN_ARROW;
    for (int k = 0; k < R_COUNT; k++) {
        if (k != R_SEP && contains(right_item(k), x, y))
            return k;
    }
    return -1;
}

/* ---- drawing ------------------------------------------------------------------------ */

static int dev(double value, double scale) {
    double rounded = round(value * scale);
    XD_ASSERT(rounded > -65536 && rounded < 65536);
    return (int)rounded;
}

static xd_rect D(xd_rect rect, double scale) {
    int x0 = dev(rect.x, scale);
    int y0 = dev(rect.y, scale);
    return (xd_rect){x0, y0, dev(rect.x + rect.w, scale) - x0, dev(rect.y + rect.h, scale) - y0};
}

static void draw_icon(xd_shell *shell, cairo_t *cairo, const char *icon, const char *fallback,
                      xd_rect rect) {
    const char *names[] = {icon, fallback, "application-x-executable"};
    cairo_surface_t *img = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names), rect.w);
    if (img != nullptr) {
        cairo_set_source_surface(cairo, img, rect.x, rect.y);
        cairo_paint(cairo);
    }
}

static void draw_hot(cairo_t *cairo, const xd_theme *theme, xd_rect rect,
                     const s7_palette *menu_colors) {
    if (menu_colors->classic || menu_colors->square) {
        xd_fill_rect(cairo, rect, menu_colors->left_hot);
        return;
    }
    double rad = xd_tpx(theme, 3);
    xd_path_rounded(cairo, rect.x + 0.5, rect.y + 0.5, rect.w - 1, rect.h - 1, rad, rad, rad, rad);
    xd_set_color(cairo, menu_colors->left_hot);
    cairo_fill_preserve(cairo);
    xd_set_color(cairo, menu_colors->left_hot_border);
    cairo_set_line_width(cairo, xd_tpx(theme, 1));
    cairo_stroke(cairo);
}

/* A program's highlight: the package's, else ours. */
static void draw_hot_item(cairo_t *cairo, const xd_theme *theme, xd_rect rect,
                          const s7_palette *menu_colors) {
    if (!xd_theme_draw_start_part(theme, cairo, XD_START_PROG_ITEM, rect, XD_STATE_HOT))
        draw_hot(cairo, theme, rect, menu_colors);
}

/* A separator line in r: the package's image, else a line of `color`. */
static void draw_sep(cairo_t *cairo, const xd_theme *theme, xd_start_part part, xd_rect rect,
                     xd_rgba color) {
    int width, height;
    if (xd_theme_start_part_size(theme, part, 0, &width, &height) &&
        xd_theme_draw_start_part(theme, cairo, part,
                                 (xd_rect){rect.x, rect.y - height / 2, rect.w, height}, 0))
        return;
    xd_fill_rect(cairo, rect, color);
}

static void draw_triangle(cairo_t *cairo, const xd_theme *theme, xd_rect rect, xd_rgba color,
                          bool left) {
    double width = xd_tpx(theme, 4);
    double height = xd_tpx(theme, 7);
    double x = rect.x + (rect.w - width) / 2;
    double y = rect.y + (rect.h - height) / 2;
    xd_set_color(cairo, color);
    if (left) {
        cairo_move_to(cairo, x + width, y);
        cairo_line_to(cairo, x, y + height / 2);
        cairo_line_to(cairo, x + width, y + height);
    } else {
        cairo_move_to(cairo, x, y);
        cairo_line_to(cairo, x + width, y + height / 2);
        cairo_line_to(cairo, x, y + height);
    }
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

static void draw_left(xd_start7 *menu, cairo_t *cairo, const xd_theme *theme,
                      const s7_palette *menu_colors) {
    double scale = theme->scale;
    xd_shell *shell = menu->shell;
    xd_rect lr = list_rect(menu);
    int y = lr.y;
    int count = visible_rows(menu, nullptr);
    for (int k = 0; k < count; k++) {
        size_t i = (size_t)(menu->scroll + k);
        const row *menu_row = &menu->rows[i];
        int height = row_h(menu, menu_row);
        xd_rect rr = {lr.x, y, lr.w, height};
        y += height;
        if (menu_row->kind == ROW_SEP) {
            draw_sep(cairo, theme, XD_START_PROG_SEPARATOR,
                     D((xd_rect){rr.x + 8, rr.y + height / 2, rr.w - 16, 1}, scale),
                     menu_colors->separator);
            continue;
        }
        if (menu_row->kind == ROW_HEADER) {
            xd_rgba heading = menu_colors->left_dim;
            (void)xd_theme_start_heading_color(theme, &heading);
            xd_text_style st = xd_text(theme, XD_FONT_MENU, heading, XD_ALIGN_START);
            xd_draw_text(cairo, D(xd_rect_inset4(rr, 8, 0, 4, 0), scale), menu_row->label, &st);
            continue;
        }
        bool jump_open = menu->sub_owner == OWNER_JUMP && menu->jump_row == (int)i;
        bool hot = (int)i == menu->hot_left || jump_open;
        bool jump = menu_row->jump && menu->mode == MODE_HOME;
        xd_rect jr = jump_arrow_rect(rr);
        /* A split row's two halves, as the package draws them. */
        bool split = hot && jump &&
                     xd_theme_draw_start_part(theme, cairo, XD_START_PROG_ITEM_LEFT,
                                              D((xd_rect){rr.x, rr.y, jr.x - rr.x, rr.h}, scale),
                                              XD_STATE_HOT);
        if (hot && !split)
            draw_hot_item(cairo, theme, D(rr, scale), menu_colors);
        if (split)
            (void)xd_theme_draw_start_part(
                theme, cairo, XD_START_PROG_ITEM_SPLIT, D(jr, scale),
                (menu->hot_arrow && (int)i == menu->hot_left) || jump_open ? XD_STATE_PRESSED
                                                                           : XD_STATE_HOT);
        if (jump) {
            /* Windows 7's split row: the ▸ opens the jump list. */
            xd_rect ar = jr;
            if (hot && !split) {
                xd_fill_rect(cairo, D((xd_rect){ar.x, ar.y + 4, 1, ar.h - 8}, scale),
                             menu_colors->classic ? menu_colors->left_hot_text
                                                  : menu_colors->left_hot_border);
                if ((menu->hot_arrow && (int)i == menu->hot_left) || jump_open)
                    draw_hot(cairo, theme, D(ar, scale), menu_colors);
            }
            draw_triangle(cairo, theme, D(ar, scale),
                          hot ? menu_colors->left_hot_text : menu_colors->left_text, false);
        }
        bool large = menu->mode == MODE_HOME;
        int icon = large ? 32 : 16;
        int ix = rr.x + 4 + menu_row->indent * 18;
        const char *label = menu_row->kind == ROW_APP ? menu_row->app->name : menu_row->label;
        if (menu_row->kind == ROW_APP)
            draw_icon(shell, cairo, menu_row->app->icon, nullptr,
                      D((xd_rect){ix, rr.y + (height - icon) / 2, icon, icon}, scale));
        else
            draw_icon(shell, cairo, menu_row->icon, "folder",
                      D((xd_rect){ix, rr.y + (height - icon) / 2, icon, icon}, scale));
        xd_text_style st =
            xd_text(theme, XD_FONT_MENU, hot ? menu_colors->left_hot_text : menu_colors->left_text,
                    XD_ALIGN_START);
        int tx = ix + icon + 8;
        int text_end = rr.x + rr.w - (jump ? S7_JUMP_W : 0);
        xd_draw_text(cairo, D((xd_rect){tx, rr.y, text_end - tx - 4, height}, scale), label, &st);
    }

    /* Scroll bar when the list overflows (All Programs, search). */
    int total = (int)menu->row_count;
    if (menu->mode != MODE_HOME &&
        (menu->scroll > 0 || (size_t)(menu->scroll + count) < menu->row_count)) {
        xd_rect sb = {lr.x + lr.w - 12, lr.y, 12, lr.h};
        xd_theme_draw_vscrollbar(theme, cairo, D(sb, scale), menu->scroll, count, total);
    }

    /* Separator, then All Programs / Back. */
    xd_rect ar = all_rect(menu);
    draw_sep(cairo, theme, XD_START_PROG_SEPARATOR,
             D((xd_rect){ar.x + 8, ar.y - 4, ar.w - 16, 1}, scale), menu_colors->separator);
    if (menu->mode != MODE_SEARCH) {
        bool hot = menu->hot_right == HIT_ALL;
        if (hot)
            draw_hot_item(cairo, theme, D(ar, scale), menu_colors);
        xd_rgba color = hot ? menu_colors->left_hot_text : menu_colors->left_text;
        xd_rect tri = D((xd_rect){ar.x + 8, ar.y, 16, ar.h}, scale);
        if (!xd_theme_draw_start_part(theme, cairo,
                                      menu->mode == MODE_ALL ? XD_START_MORE_PROGRAMS_BACK
                                                             : XD_START_MORE_PROGRAMS_ARROW,
                                      tri, hot ? XD_STATE_HOT : 0))
            draw_triangle(cairo, theme, tri, color, menu->mode == MODE_ALL);
        xd_font_spec bold = theme->scheme.fonts[XD_FONT_MENU];
        bold.bold = true;
        xd_text_style st = xd_text(theme, XD_FONT_MENU, color, XD_ALIGN_START);
        st.font = &bold;
        xd_draw_text(cairo, D((xd_rect){ar.x + 36, ar.y, ar.w - 40, ar.h}, scale),
                     menu->mode == MODE_ALL ? "Back" : "All Programs", &st);
    }

    /* Search box: takes any typing, so it has the focus while the menu
     * is open. */
    xd_rect sr = D(search_rect(menu), scale);
    xd_rect inner = xd_theme_draw_field_state(theme, cairo, sr, XD_STATE_FOCUSED);
    bool caret = !menu->caret_hidden;
    xd_str query = xd_textedit_view(&menu->search);
    int pad = xd_tpx(theme, 4);
    xd_rect tr = xd_rect_inset4(inner, pad, 0, pad, 0);
    if (query.len == 0) {
        xd_font_spec it = theme->scheme.fonts[XD_FONT_MENU];
        it.italic = true;
        xd_text_style st = xd_text(theme, XD_FONT_MENU, menu_colors->left_dim, XD_ALIGN_START);
        st.font = &it;
        xd_draw_text(cairo, tr, "Search programs and files", &st);
        if (caret)
            xd_fill_rect(cairo,
                         (xd_rect){tr.x, tr.y + xd_tpx(theme, 4), XD_MAX(xd_tpx(theme, 1), 1),
                                   tr.h - xd_tpx(theme, 8)},
                         menu_colors->left_text);
    } else {
        xd_text_style st = xd_text(theme, XD_FONT_MENU, menu_colors->left_text, XD_ALIGN_START);
        char *text = xd_str_dup_cstr(query);
        xd_draw_text(cairo, tr, text, &st);
        int width = XD_MIN(xd_text_width(cairo, text, &st), tr.w - 1);
        if (caret)
            xd_fill_rect(cairo,
                         (xd_rect){tr.x + width + 1, tr.y + xd_tpx(theme, 4),
                                   XD_MAX(xd_tpx(theme, 1), 1), tr.h - xd_tpx(theme, 8)},
                         menu_colors->left_text);
        xd_free(text);
    }
    /* Magnifier glyph on the right of the box. */
    int glyph_size = xd_tpx(theme, 10);
    double gx = inner.x + inner.w - glyph_size - pad;
    double gy = inner.y + (inner.h - glyph_size) / 2.0;
    xd_set_color(cairo, menu_colors->classic ? menu_colors->left_text : xd_rgba_hex(0x2C628B));
    cairo_set_line_width(cairo, XD_MAX(xd_tpx(theme, 1), 1) * 1.5);
    cairo_new_path(cairo); /* the text leaves a current point: no line from it to the arc */
    cairo_arc(cairo, gx + glyph_size * 0.6, gy + glyph_size * 0.4, glyph_size * 0.35, 0, 2 * XD_PI);
    cairo_stroke(cairo);
    cairo_move_to(cairo, gx + glyph_size * 0.35, gy + glyph_size * 0.65);
    cairo_line_to(cairo, gx, gy + glyph_size);
    cairo_stroke(cairo);
}

static void draw_right(xd_start7 *menu, cairo_t *cairo, const xd_theme *theme,
                       const s7_palette *menu_colors) {
    double scale = theme->scale;
    for (int k = 0; k < R_COUNT; k++) {
        xd_rect item_rect = right_item(k);
        if (k == R_SEP) {
            draw_sep(
                cairo, theme, XD_START_PLACES_SEPARATOR,
                D((xd_rect){item_rect.x + 4, item_rect.y + item_rect.h / 2, item_rect.w - 8, 1},
                  scale),
                menu_colors->right_sep);
            continue;
        }
        bool hot = (menu->hot_right == k || menu->sub_owner == k) && !right_items[k].disabled;
        if (hot && xd_theme_draw_start_part(theme, cairo, XD_START_PLACES_ITEM, D(item_rect, scale),
                                            XD_STATE_HOT)) {
            /* The package's highlight. */
        } else if (hot) {
            if (menu_colors->classic || menu_colors->square)
                xd_fill_rect(cairo, D(item_rect, scale), menu_colors->right_hot);
            else {
                double rad = xd_tpx(theme, 3);
                xd_rect device_rect = D(item_rect, scale);
                xd_path_rounded(cairo, device_rect.x, device_rect.y, device_rect.w, device_rect.h,
                                rad, rad, rad, rad);
                xd_set_color(cairo, menu_colors->right_hot);
                cairo_fill(cairo);
            }
        }
        xd_rgba color = right_items[k].disabled         ? menu_colors->right_dim
                        : (hot && menu_colors->classic) ? xd_sys(theme, XD_COLOR_HILIGHT_TEXT)
                                                        : menu_colors->right_text;
        xd_font_spec font = theme->scheme.fonts[XD_FONT_MENU];
        font.bold = k == R_USER;
        xd_text_style st = xd_text(theme, XD_FONT_MENU, color, XD_ALIGN_START);
        st.font = &font;
        const char *label = k == R_USER ? menu->user : right_items[k].label;
        xd_draw_text(cairo, D(xd_rect_inset4(item_rect, 8, 0, 20, 0), scale), label, &st);
        if (right_items[k].submenu)
            draw_triangle(
                cairo, theme,
                D((xd_rect){item_rect.x + item_rect.w - 16, item_rect.y, 12, item_rect.h}, scale),
                color, false);
    }

    /* Shut Down split button. */
    xd_rect button = D(shutdown_rect(), scale);
    xd_rect arrow = D(shutdown_arrow_rect(), scale);
    uint8_t bs = menu->hot_right == HIT_SHUTDOWN ? XD_STATE_HOT : 0;
    uint8_t as = menu->hot_right == HIT_SHUTDOWN_ARROW || menu->sub_owner == HIT_SHUTDOWN_ARROW
                     ? XD_STATE_HOT
                     : 0;
    int sw, sh;
    if (xd_theme_start_part_size(theme, XD_START_SHUTDOWN, 0, &sw, &sh) &&
        xd_theme_start_part_size(theme, XD_START_SHUTDOWN_ARROW, 0, &sw, &sh)) {
        (void)xd_theme_draw_start_part(theme, cairo, XD_START_SHUTDOWN, button, bs);
        (void)xd_theme_draw_start_part(theme, cairo, XD_START_SHUTDOWN_ARROW, arrow, as);
        xd_text_style st = xd_text(theme, XD_FONT_MENU, menu_colors->right_text, XD_ALIGN_CENTER);
        xd_draw_text(cairo, button, "Shut Down", &st);
    } else if (menu_colors->classic) {
        xd_theme_draw_button(theme, cairo, button, bs, "Shut Down");
        xd_theme_draw_button(theme, cairo, arrow, as, "");
        draw_triangle(cairo, theme, arrow, xd_sys(theme, XD_COLOR_BUTTON_TEXT), false);
    } else {
        double rad = xd_tpx(theme, 2);
        xd_rect whole = {button.x, button.y, arrow.x + arrow.w - button.x, button.h};
        xd_path_rounded(cairo, whole.x + 0.5, whole.y + 0.5, whole.w - 1, whole.h - 1, rad, rad,
                        rad, rad);
        xd_set_color(cairo, xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), bs || as ? 0.22 : 0.1));
        cairo_fill_preserve(cairo);
        xd_set_color(cairo, xd_rgba_alpha(menu_colors->right_text, 0.6));
        cairo_set_line_width(cairo, xd_tpx(theme, 1));
        cairo_stroke(cairo);
        xd_fill_rect(cairo,
                     (xd_rect){arrow.x, arrow.y + xd_tpx(theme, 3), xd_tpx(theme, 1),
                               arrow.h - xd_tpx(theme, 6)},
                     xd_rgba_alpha(menu_colors->right_text, 0.6));
        xd_text_style st = xd_text(theme, XD_FONT_MENU, menu_colors->right_text, XD_ALIGN_CENTER);
        xd_draw_text(cairo, button, "Shut Down", &st);
        draw_triangle(cairo, theme, arrow, menu_colors->right_text, false);
    }
}

/* ---- Windows XP drawing ------------------------------------------------------------- */

typedef struct xp_palette {
    xd_rgba head_a, head_b, foot_a, foot_b; /* gradients */
    bool head_horizontal;                    /* Classic: title-bar style gradient */
    xd_rgba body, frame_light, frame_dark;
    xd_rgba left_bg, left_text, left_sub, left_sep, panel_border;
    xd_rgba right_bg, right_text, right_dim, right_sep, column_edge;
    xd_rgba hot, hot_text, right_hot, right_hot_text;
    xd_rgba name, foot_text, foot_hot;
    xd_rgba accent; /* line under the header; alpha 0 for none */
    bool classic, whistler;
} xp_palette;

static xp_palette xp_colors(const xd_theme *theme) {
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    xd_rgba white = xd_rgba_hex(0xFFFFFF);
    switch (theme->style) {
    case XD_STYLE_CLASSIC:
        return (xp_palette){
            .head_a = xd_sys(theme, XD_COLOR_ACTIVE_TITLE),
            .head_b = xd_sys(theme, XD_COLOR_GRADIENT_ACTIVE_TITLE),
            .foot_a = xd_sys(theme, XD_COLOR_ACTIVE_TITLE),
            .foot_b = xd_sys(theme, XD_COLOR_GRADIENT_ACTIVE_TITLE),
            .head_horizontal = true,
            .left_bg = xd_sys(theme, XD_COLOR_WINDOW),
            .left_text = xd_sys(theme, XD_COLOR_WINDOW_TEXT),
            .left_sub = xd_sys(theme, XD_COLOR_GRAY_TEXT),
            .left_sep = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
            .panel_border = none,
            .right_bg = xd_sys(theme, XD_COLOR_MENU),
            .right_text = xd_sys(theme, XD_COLOR_MENU_TEXT),
            .right_dim = xd_sys(theme, XD_COLOR_GRAY_TEXT),
            .right_sep = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
            .column_edge = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
            .hot = xd_sys(theme, XD_COLOR_HILIGHT),
            .hot_text = xd_sys(theme, XD_COLOR_HILIGHT_TEXT),
            .right_hot = xd_sys(theme, XD_COLOR_HILIGHT),
            .right_hot_text = xd_sys(theme, XD_COLOR_HILIGHT_TEXT),
            .name = xd_sys(theme, XD_COLOR_TITLE_TEXT),
            .foot_text = xd_sys(theme, XD_COLOR_TITLE_TEXT),
            .foot_hot = xd_rgba_alpha(white, 0.2),
            .accent = none,
            .classic = true,
        };
    case XD_STYLE_WHISTLER: {
        /* Watercolor: one flat blue body, the white list inset in it. */
        xd_rgba blue = xd_rgba_hex(0x3573D6);
        return (xp_palette){
            .head_a = xd_rgba_hex(0x5297F9),
            .head_b = xd_rgba_hex(0x5297F9),
            .foot_a = blue,
            .foot_b = blue,
            .body = blue,
            .frame_light = xd_rgba_hex(0x7DB0FA),
            .frame_dark = xd_rgba_hex(0x2B5FB5),
            .left_bg = white,
            .left_text = xd_rgba_hex(0x000000),
            .left_sub = xd_rgba_hex(0x7A7A7A),
            .left_sep = xd_rgba_hex(0xD8D8D0),
            .panel_border = xd_rgba_hex(0x2B5FB5),
            .right_bg = none,
            .right_text = white,
            .right_dim = xd_rgba_hex(0xA9C7F2),
            .right_sep = xd_rgba_hex(0x6FA2EA),
            .column_edge = none,
            .hot = xd_rgba_hex(0x316AC5),
            .hot_text = white,
            .right_hot = xd_rgba_hex(0x5297F9),
            .right_hot_text = white,
            .name = white,
            .foot_text = white,
            .foot_hot = xd_rgba_alpha(white, 0.18),
            .accent = none,
            .whistler = true,
        };
    }
    case XD_STYLE_LUNA:
    case XD_STYLE_AERO: /* Windows 7's own colours */
    case XD_STYLE_COUNT:
        break;
    }
    /* Luna: the colour variant's own (theme.h). */
    xd_luna_start_colors luna_start_colors;
    xd_theme_luna_start_colors(theme, &luna_start_colors);
    return (xp_palette){
        .head_a = xd_rgba_hex(luna_start_colors.head_a),
        .head_b = xd_rgba_hex(luna_start_colors.head_b),
        .foot_a = xd_rgba_hex(luna_start_colors.foot_a),
        .foot_b = xd_rgba_hex(luna_start_colors.foot_b),
        .frame_light = xd_rgba_hex(luna_start_colors.frame),
        .frame_dark = xd_rgba_hex(luna_start_colors.frame),
        .left_bg = xd_rgba_hex(luna_start_colors.left_bg),
        .left_text = xd_rgba_hex(luna_start_colors.left_text),
        .left_sub = xd_rgba_hex(luna_start_colors.left_sub),
        .left_sep = xd_rgba_hex(luna_start_colors.left_sep),
        .panel_border = none,
        .right_bg = xd_rgba_hex(luna_start_colors.right_bg),
        .right_text = xd_rgba_hex(luna_start_colors.right_text),
        .right_dim = xd_rgba_hex(luna_start_colors.right_dim),
        .right_sep = xd_rgba_hex(luna_start_colors.right_sep),
        .column_edge = xd_rgba_hex(luna_start_colors.column_edge),
        .hot = xd_rgba_hex(luna_start_colors.hot),
        .hot_text = white,
        .right_hot = xd_rgba_hex(luna_start_colors.hot),
        .right_hot_text = white,
        .name = white,
        .foot_text = white,
        .foot_hot = xd_rgba_alpha(white, 0.2),
        .accent = luna_start_colors.accent ? xd_rgba_hex(0xF09A3C) : none,
    };
}

static xp_palette xp_colors_pkg(const xd_theme *theme) {
    xp_palette menu_colors = xp_colors(theme);
    pkg_color(theme, XD_START_PROG_LIST, XD_START_TEXT, &menu_colors.left_text);
    pkg_color(theme, XD_START_PROG_LIST, XD_START_SUBTEXT, &menu_colors.left_sub);
    pkg_color(theme, XD_START_PLACES_LIST, XD_START_TEXT, &menu_colors.right_text);
    pkg_color(theme, XD_START_USER_PANE, XD_START_TEXT, &menu_colors.name);
    pkg_color(theme, XD_START_LOGOFF, XD_START_TEXT, &menu_colors.foot_text);
    return menu_colors;
}

static void fill_gradient2(cairo_t *cairo, xd_rect rect, xd_rgba from, xd_rgba to,
                           bool horizontal) {
    xd_fill_gradient(cairo, rect, from, to, horizontal);
}

/* "Internet" for the web browser and "E-mail" for the mail client, as XP
 * labels its two pinned programs; nullptr for anything else. */
static const char *app_role(const xd_app *app) {
    if (app->info == nullptr)
        return nullptr;
    const char *cats = g_desktop_app_info_get_categories(app->info);
    if (cats == nullptr)
        return nullptr;
    if (strstr(cats, "WebBrowser") != nullptr)
        return "Internet";
    if (strstr(cats, "Email") != nullptr)
        return "E-mail";
    return nullptr;
}

static void draw_xp_arrow(cairo_t *cairo, const xd_theme *theme, xd_rect rect,
                          const xp_palette *menu_colors) {
    /* XP's round green arrow; Watercolor uses a plain "»". */
    if (menu_colors->whistler) {
        xd_font_spec bold = theme->scheme.fonts[XD_FONT_MENU];
        bold.bold = true;
        xd_text_style st = xd_text(theme, XD_FONT_MENU, menu_colors->left_text, XD_ALIGN_CENTER);
        st.font = &bold;
        xd_draw_text(cairo, rect, "»", &st);
        return;
    }
    double diameter = XD_MIN(rect.w, rect.h);
    double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h / 2.0;
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, cy, diameter / 2, 0, 2 * XD_PI);
    xd_set_color(cairo, xd_rgba_hex(0x3C9D3C));
    cairo_fill_preserve(cairo);
    xd_set_color(cairo, xd_rgba_hex(0x24782A));
    cairo_set_line_width(cairo, XD_MAX(xd_tpx(theme, 1), 1));
    cairo_stroke(cairo);
    double arm = diameter * 0.22;
    cairo_move_to(cairo, cx - arm * 0.7, cy - arm);
    cairo_line_to(cairo, cx + arm, cy);
    cairo_line_to(cairo, cx - arm * 0.7, cy + arm);
    cairo_close_path(cairo);
    xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
    cairo_fill(cairo);
}

/* XP lights All Programs while a newly installed program is in it. */
static bool new_programs(const xd_start7 *menu) {
    const xd_shell *shell = menu->shell;
    return shell->settings.start_highlight_new && shell->new_apps != nullptr &&
           g_hash_table_size(shell->new_apps) > 0;
}

/* Where an item's letter is underlined in its label, while the menus show
 * their access keys (xd_menu_cues). */
static const char *xp_underline(const xd_start7 *menu, const char *label, int hit) {
    uint32_t key = xp_key(hit);
    if (key == 0 || !xd_menu_cues(menu->shell))
        return nullptr;
    for (const char *character = label; *character != '\0'; character++) {
        if ((uint32_t)g_ascii_tolower(*character) == key)
            return character;
    }
    return nullptr;
}

static void draw_left_xp(xd_start7 *menu, cairo_t *cairo, const xd_theme *theme,
                         const xp_palette *menu_colors) {
    double scale = theme->scale;
    xd_rect lr = list_rect(menu);
    int y = lr.y;
    for (size_t i = 0; i < menu->row_count; i++) {
        const row *menu_row = &menu->rows[i];
        int height = row_h(menu, menu_row);
        if (y + height > lr.y + lr.h)
            break;
        xd_rect rr = {lr.x, y, lr.w, height};
        y += height;
        if (menu_row->kind == ROW_SEP) {
            draw_sep(cairo, theme, XD_START_PROG_SEPARATOR,
                     D((xd_rect){rr.x + 6, rr.y + height / 2, rr.w - 12, 1}, scale),
                     menu_colors->left_sep);
            continue;
        }
        if (menu_row->kind != ROW_APP)
            continue;
        bool hot = (int)i == menu->hot_left;
        if (hot)
            xd_fill_rect(cairo, D(rr, scale), menu_colors->hot);
        int icon = menu->large ? menu->big_icon : 16;
        draw_icon(menu->shell, cairo, menu_row->app->icon, nullptr,
                  D((xd_rect){rr.x + 4, rr.y + (height - icon) / 2, icon, icon}, scale));
        int tx = rr.x + 4 + icon + 6;
        xd_rect text = {tx, rr.y, rr.x + rr.w - tx - 4, height};
        xd_rgba color = hot ? menu_colors->hot_text : menu_colors->left_text;
        xd_font_spec bold = theme->scheme.fonts[XD_FONT_MENU];
        bold.bold = true;
        const char *role = menu_row->pinned ? app_role(menu_row->app) : nullptr;
        if (role != nullptr) {
            /* Two lines: the role in bold, the program's name below. */
            xd_text_style st = xd_text(theme, XD_FONT_MENU, color, XD_ALIGN_START);
            st.font = &bold;
            xd_draw_text(cairo, D((xd_rect){text.x, text.y + 4, text.w, height / 2 - 3}, scale),
                         role, &st);
            xd_text_style sub =
                xd_text(theme, XD_FONT_MENU, hot ? menu_colors->hot_text : menu_colors->left_sub,
                        XD_ALIGN_START);
            xd_draw_text(
                cairo, D((xd_rect){text.x, text.y + height / 2 - 1, text.w, height / 2 - 3}, scale),
                menu_row->app->name, &sub);
        } else {
            xd_text_style st = xd_text(theme, XD_FONT_MENU, color, XD_ALIGN_START);
            if (menu_row->pinned)
                st.font = &bold;
            xd_draw_text(cairo, D(text, scale), menu_row->app->name, &st);
        }
    }

    /* Separator, then All Programs (cascades). */
    xd_rect ar = all_rect(menu);
    bool hot = menu->hot_right == HIT_ALL || menu->sub_owner == HIT_ALL;
    /* The package's All Programs pane: the list's foot, edge to edge. */
    xd_rect pl = xp_left_panel(menu);
    int pane_y = ar.y - XP_SEP / 2;
    (void)xd_theme_draw_start_part(theme, cairo, XD_START_MORE_PROGRAMS,
                                   D((xd_rect){pl.x, pane_y, pl.w, pl.y + pl.h - pane_y}, scale),
                                   0);
    draw_sep(cairo, theme, XD_START_PROG_SEPARATOR,
             D((xd_rect){ar.x + 6, ar.y - XP_SEP / 2 - 1, ar.w - 12, 1}, scale),
             menu_colors->left_sep);
    if (hot)
        xd_fill_rect(cairo, D(ar, scale), menu_colors->hot);
    else if (new_programs(menu) && !xd_theme_draw_new_program(theme, cairo, D(ar, scale)))
        xd_fill_rect(cairo, D(ar, scale), xd_rgba_hex(XD_NEW_PROGRAM_COLOR));
    xd_font_spec bold = theme->scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style st = xd_text(theme, XD_FONT_MENU,
                               hot ? menu_colors->hot_text : menu_colors->left_text, XD_ALIGN_END);
    st.font = &bold;
    int mid = ar.x + ar.w / 2 + 24;
    static const char all[] = "All Programs";
    st.underline = xp_underline(menu, all, HIT_ALL);
    xd_draw_text(cairo, D((xd_rect){ar.x, ar.y, mid - ar.x, ar.h}, scale), all, &st);
    xd_rect arrow = {mid + 8, ar.y + (ar.h - 16) / 2, 16, 16};
    xp_palette ap = *menu_colors;
    if (hot)
        ap.left_text = menu_colors->hot_text;
    if (!xd_theme_draw_start_part(theme, cairo, XD_START_MORE_PROGRAMS_ARROW,
                                  D((xd_rect){arrow.x - 4, ar.y, arrow.w + 8, ar.h}, scale),
                                  hot ? XD_STATE_HOT : 0))
        draw_xp_arrow(cairo, theme, D(arrow, scale), &ap);
}

static void draw_right_xp(xd_start7 *menu, cairo_t *cairo, const xd_theme *theme,
                          const xp_palette *menu_colors) {
    double scale = theme->scale;
    for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++) {
        const xp_item *it = &xp_items[i];
        xd_rect rect = xp_item_rect(menu, i);
        if (it->hit == R_SEP) {
            draw_sep(cairo, theme, XD_START_PLACES_SEPARATOR,
                     D((xd_rect){rect.x + 4, rect.y + rect.h / 2, rect.w - 8, 1}, scale),
                     menu_colors->right_sep);
            continue;
        }
        bool hot = !it->disabled && (menu->hot_right == it->hit || menu->sub_owner == it->hit);
        if (hot)
            xd_fill_rect(cairo, D(rect, scale), menu_colors->right_hot);
        int icon = 24;
        draw_icon(menu->shell, cairo, it->icon, "folder",
                  D((xd_rect){rect.x + 4, rect.y + (rect.h - icon) / 2, icon, icon}, scale));
        xd_rgba color = it->disabled ? menu_colors->right_dim
                        : hot        ? menu_colors->right_hot_text
                                     : menu_colors->right_text;
        xd_font_spec font = theme->scheme.fonts[XD_FONT_MENU];
        font.bold = it->bold;
        xd_text_style st = xd_text(theme, XD_FONT_MENU, color, XD_ALIGN_START);
        st.font = &font;
        if (it->tall) {
            st.wrap = true;
            st.ellipsize = false;
        }
        st.underline = xp_underline(menu, it->label, it->hit);
        xd_rect tr = D(xd_rect_inset4(rect, 4 + icon + 6, 0, it->submenu ? 16 : 4, 0), scale);
        if (it->tall) {
            /* Wrapped text is drawn from the top: its lines (one or two)
             * centred on the icon, as XP has them. */
            PangoLayout *layout = xd_text_layout(cairo, it->label, &st);
            pango_layout_set_width(layout, tr.w * PANGO_SCALE);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
            int lw = 0, lh = 0;
            pango_layout_get_pixel_size(layout, &lw, &lh);
            g_object_unref(layout);
            if (lh < tr.h) {
                tr.y += (tr.h - lh) / 2;
                tr.h = lh;
            }
        }
        xd_draw_text(cairo, tr, it->label, &st);
        if (it->submenu)
            draw_triangle(cairo, theme,
                          D((xd_rect){rect.x + rect.w - 14, rect.y, 12, rect.h}, scale), color,
                          false);
    }
}

/* XP's footer icons: an orange tile with a padlock (Log Off) and a red
 * tile with the power symbol (Turn Off Computer). */
static void draw_foot_icon(cairo_t *cairo, const xd_theme *theme, xd_rect rect, bool power) {
    double rad = xd_tpx(theme, 3);
    double lw = XD_MAX(xd_tpx(theme, 1), 1);
    cairo_new_path(cairo);
    xd_path_rounded(cairo, rect.x + lw / 2, rect.y + lw / 2, rect.w - lw, rect.h - lw, rad, rad,
                    rad, rad);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    xd_rgba top = xd_rgba_hex(power ? 0xF07B5E : 0xF9C15A);
    xd_rgba bottom = xd_rgba_hex(power ? 0xC2310F : 0xD9780A);
    cairo_pattern_add_color_stop_rgb(pattern, 0, top.r, top.g, top.b);
    cairo_pattern_add_color_stop_rgb(pattern, 1, bottom.r, bottom.g, bottom.b);
    cairo_set_source(cairo, pattern);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(pattern);
    xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
    cairo_set_line_width(cairo, lw);
    cairo_stroke(cairo);

    double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h / 2.0, unit = rect.w / 22.0;
    xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    if (power) {
        cairo_set_line_width(cairo, 2.2 * unit);
        cairo_new_path(cairo);
        cairo_arc(cairo, cx, cy + unit, 6 * unit, -XD_PI / 2 + 0.75, -XD_PI / 2 - 0.75 + 2 * XD_PI);
        cairo_stroke(cairo);
        cairo_move_to(cairo, cx, cy - 6 * unit);
        cairo_line_to(cairo, cx, cy + unit);
        cairo_stroke(cairo);
    } else {
        /* Padlock: shackle and body. */
        cairo_set_line_width(cairo, 2 * unit);
        cairo_new_path(cairo);
        cairo_arc(cairo, cx, cy - 2 * unit, 4 * unit, XD_PI, 2 * XD_PI);
        cairo_stroke(cairo);
        cairo_rectangle(cairo, cx - 6 * unit, cy - unit, 12 * unit, 9 * unit);
        cairo_fill(cairo);
        xd_set_color(cairo, bottom);
        cairo_rectangle(cairo, cx - unit, cy + 2 * unit, 2 * unit, 3 * unit);
        cairo_fill(cairo);
    }
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_BUTT);
}

static void draw_foot_button(cairo_t *cairo, const xd_theme *theme, const xp_palette *menu_colors,
                             xd_rect button, bool hot, bool power, const char *label,
                             const char *underline) {
    double scale = theme->scale;
    if (hot) {
        xd_rect device_rect = D(button, scale);
        if (menu_colors->classic) {
            xd_fill_rect(cairo, device_rect, menu_colors->foot_hot);
        } else {
            double rad = xd_tpx(theme, 3);
            xd_path_rounded(cairo, device_rect.x, device_rect.y, device_rect.w, device_rect.h, rad,
                            rad, rad, rad);
            xd_set_color(cairo, menu_colors->foot_hot);
            cairo_fill(cairo);
        }
    }
    int is = 22;
    xd_rect ir = D((xd_rect){button.x + 3, button.y + (button.h - is) / 2, is, is}, scale);
    if (!xd_theme_draw_start_logoff_icon(theme, cairo, ir, power, hot ? XD_STATE_HOT : 0))
        draw_foot_icon(cairo, theme, ir, power);
    xd_text_style st = xd_text(theme, XD_FONT_MENU, menu_colors->foot_text, XD_ALIGN_START);
    st.underline = underline;
    xd_draw_text(cairo,
                 D((xd_rect){button.x + 3 + is + 5, button.y, button.w - is - 10, button.h}, scale),
                 label, &st);
}

static void render_xp(xd_start7 *menu, cairo_t *cairo, const xd_theme *theme) {
    double scale = theme->scale;
    xp_palette menu_colors = xp_colors_pkg(theme);
    int head = xp_head(menu), foot = xp_foot(menu);
    xd_rect whole = D((xd_rect){0, 0, menu->w, menu->h}, scale);
    xd_rect head_r = D((xd_rect){0, 0, menu->w, head}, scale);
    xd_rect body_r = D((xd_rect){0, head, menu->w, menu->body_h}, scale);
    xd_rect foot_r = D((xd_rect){0, head + menu->body_h, menu->w, foot}, scale);
    double rad = menu_colors.classic || menu_colors.whistler ? 0 : xd_tpx(theme, 7);

    cairo_save(cairo);
    xd_path_rounded(cairo, whole.x, whole.y, whole.w, whole.h, rad, rad, 0, 0);
    cairo_clip(cairo);

    /* Header: the package's UserPane, else the style's. */
    bool pkg_head = xd_theme_draw_start_part(theme, cairo, XD_START_USER_PANE, head_r, 0);
    if (!pkg_head)
        fill_gradient2(cairo, head_r, menu_colors.head_a, menu_colors.head_b,
                       menu_colors.head_horizontal);
    if (menu_colors.whistler && !pkg_head) {
        int mx = dev(menu->w * 0.58, scale);
        xd_fill_rect(cairo, (xd_rect){mx, head_r.y, head_r.x + head_r.w - mx, head_r.h},
                     menu_colors.body);
        xd_theme_draw_whistler_mosaic(theme, cairo, head_r, mx);
        xd_fill_rect(cairo, (xd_rect){head_r.x, head_r.y, head_r.w, XD_MAX(xd_tpx(theme, 1), 1)},
                     xd_rgba_hex(0x7DB1FB));
    }

    /* Body: left list and right column */
    xd_rect left = D(xp_left_panel(menu), scale);
    xd_rect right_col = D((xd_rect){XP_LEFT_W, head, menu->w - XP_LEFT_W, menu->body_h}, scale);
    /* A package's ProgList and PlacesList draw the columns, edges and all. */
    bool pkg_body = xd_theme_start_part_size(theme, XD_START_PROG_LIST, 0, &(int){0}, &(int){0});
    if (pkg_body) {
        xd_fill_rect(cairo, body_r, menu_colors.left_bg);
        if (!xd_theme_draw_start_part(theme, cairo, XD_START_PLACES_LIST, right_col, 0))
            xd_fill_rect(cairo, right_col, menu_colors.right_bg);
        (void)xd_theme_draw_start_part(theme, cairo, XD_START_PROG_LIST, left, 0);
    } else if (menu_colors.whistler) {
        xd_fill_rect(cairo, body_r, menu_colors.body);
        xd_fill_rect(cairo, left, menu_colors.left_bg);
        xd_draw_bevel(cairo, left, XD_MAX(xd_tpx(theme, 1), 1), menu_colors.panel_border,
                      menu_colors.panel_border);
    } else {
        xd_rect right = D((xd_rect){XP_LEFT_W, head, menu->w - XP_LEFT_W, menu->body_h}, scale);
        xd_fill_rect(cairo, left, menu_colors.left_bg);
        xd_fill_rect(cairo, right, menu_colors.right_bg);
        xd_fill_rect(cairo, (xd_rect){right.x, right.y, XD_MAX(xd_tpx(theme, 1), 1), right.h},
                     menu_colors.column_edge);
    }

    /* Footer */
    if (!xd_theme_draw_start_part(theme, cairo, XD_START_LOGOFF, foot_r, 0))
        fill_gradient2(cairo, foot_r, menu_colors.foot_a, menu_colors.foot_b,
                       menu_colors.head_horizontal);
    cairo_restore(cairo);

    /* XP's orange line under the header, fading out at both ends (a
     * package's columns have their own). */
    if (menu_colors.accent.a > 0 && !pkg_body) {
        xd_rect line = D((xd_rect){0, head, menu->w, 2}, scale);
        cairo_pattern_t *pattern = cairo_pattern_create_linear(line.x, 0, line.x + line.w, 0);
        cairo_pattern_add_color_stop_rgba(pattern, 0, menu_colors.accent.r, menu_colors.accent.g,
                                          menu_colors.accent.b, 0);
        cairo_pattern_add_color_stop_rgba(pattern, 0.5, menu_colors.accent.r, menu_colors.accent.g,
                                          menu_colors.accent.b, 1);
        cairo_pattern_add_color_stop_rgba(pattern, 1, menu_colors.accent.r, menu_colors.accent.g,
                                          menu_colors.accent.b, 0);
        cairo_rectangle(cairo, line.x, line.y, line.w, line.h);
        cairo_set_source(cairo, pattern);
        cairo_fill(cairo);
        cairo_pattern_destroy(pattern);
    }

    /* Frame */
    if (pkg_head && pkg_body) {
        /* The package's images have their edges. */
    } else if (menu_colors.classic) {
        xd_draw_bevel(cairo, whole, XD_MAX(xd_tpx(theme, 1), 1),
                      xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
                      xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
    } else if (menu_colors.whistler) {
        xd_draw_bevel(cairo, whole, XD_MAX(xd_tpx(theme, 1), 1), menu_colors.frame_light,
                      menu_colors.frame_dark);
    } else {
        double lw = XD_MAX(xd_tpx(theme, 1), 1);
        xd_path_rounded(cairo, whole.x + lw / 2, whole.y + lw / 2, whole.w - lw, whole.h - lw, rad,
                        rad, 0, 0);
        xd_set_color(cairo, menu_colors.frame_dark);
        cairo_set_line_width(cairo, lw);
        cairo_stroke(cairo);
    }

    /* User picture and name */
    xd_font_spec name_font = {
        .family = XD_FONT_FAMILY_TREBUCHET, .size_pt = menu_colors.whistler ? 9 : 13, .bold = true};
    (void)xd_theme_start_part_font(theme, XD_START_USER_PANE, &name_font);
    xd_rgba name_shadow = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.45);
    bool pkg_shadow =
        xd_theme_start_part_color(theme, XD_START_USER_PANE, 0, XD_START_SHADOW, &name_shadow);
    xd_text_style ns = {
        .font = &name_font,
        .scale = scale,
        .color = menu_colors.name,
        .align = XD_ALIGN_START,
        .ellipsize = true,
        .shadow = pkg_shadow || (!menu_colors.classic && !menu_colors.whistler),
        .shadow_color = name_shadow,
        .smoothing = theme->scheme.font_smoothing,
    };
    if (menu_colors.whistler) {
        xd_draw_text(cairo, D((xd_rect){8, 0, menu->w / 2, head}, scale), menu->user, &ns);
    } else {
        xd_rect pic = D((xd_rect){8, (head - XP_PICTURE) / 2, XP_PICTURE, XP_PICTURE}, scale);
        int bw = XD_MAX(xd_tpx(theme, 2), 2);
        xd_rect outer = xd_rect_inset(pic, -bw, -bw);
        double prad = menu_colors.classic ? 0 : xd_tpx(theme, 3);
        int fw = xd_tpx(theme, 4);
        if (!xd_theme_draw_start_part(theme, cairo, XD_START_USER_PICTURE,
                                      xd_rect_inset(pic, -fw, -fw), 0)) {
            xd_path_rounded(cairo, outer.x, outer.y, outer.w, outer.h, prad, prad, prad, prad);
            xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
            cairo_fill(cairo);
        }
        if (menu->picture != nullptr) {
            cairo_save(cairo);
            cairo_rectangle(cairo, pic.x, pic.y, pic.w, pic.h);
            cairo_clip(cairo);
            xd_image_paint_wallpaper(cairo, menu->picture, pic, 3 /* fill */, 1.0);
            cairo_restore(cairo);
        } else {
            draw_icon(menu->shell, cairo, "avatar-default", "user-identity", pic);
        }
        xd_draw_text(cairo,
                     D((xd_rect){8 + XP_PICTURE + 10, 0, menu->w - XP_PICTURE - 30, head}, scale),
                     menu->user, &ns);
    }

    draw_left_xp(menu, cairo, theme, &menu_colors);
    draw_right_xp(menu, cairo, theme, &menu_colors);
    static const char logoff[] = "Log Off", turnoff[] = "Turn Off Computer";
    draw_foot_button(cairo, theme, &menu_colors, xp_logoff_rect(menu),
                     menu->hot_right == HIT_LOGOFF, false, logoff,
                     xp_underline(menu, logoff, HIT_LOGOFF));
    draw_foot_button(cairo, theme, &menu_colors, xp_turnoff_rect(menu),
                     menu->hot_right == HIT_TURNOFF, true, turnoff,
                     xp_underline(menu, turnoff, HIT_TURNOFF));
}

static void render_7(xd_start7 *menu, cairo_t *cairo, const xd_theme *tp) {
    xd_shell *shell = menu->shell;
    double scale = menu->scale;
    xd_theme theme = *tp;
    s7_palette menu_colors = palette_7(&theme);
    xd_rect fr = D(frame_rect(), scale);
    xd_rect left = D((xd_rect){0, S7_TOP, S7_LEFT_W, S7_H - S7_TOP}, scale);
    xd_rect right = D((xd_rect){S7_LEFT_W, S7_TOP, S7_W - S7_LEFT_W, S7_H - S7_TOP}, scale);
    if (menu_colors.classic) {
        xd_theme_draw_menu_frame(&theme, cairo, fr);
        xd_menu_metrics mm = xd_theme_menu_metrics(&theme);
        xd_fill_rect(
            cairo,
            (xd_rect){right.x, right.y + mm.border, right.w - mm.border, right.h - 2 * mm.border},
            menu_colors.right_bg);
        xd_fill_rect(cairo,
                     (xd_rect){left.x + mm.border, left.y + mm.border, left.w - mm.border,
                               left.h - 2 * mm.border},
                     menu_colors.left_bg);
    } else {
        /* The white list sits inset in the panel, as on Windows 7. */
        xd_rect inset = D(list_panel_rect(menu), scale);
        /* Windows 7's list stops above the search box's pane. */
        xd_rect ob = open_box_rect(menu, nullptr);
        xd_rect list = inset;
        if (ob.w > 0)
            list.h = D(ob, scale).y - list.y;
        /* A package's columns over the glass, their images shaping the
         * menu (its rounded corners, the band round the list); else ours,
         * the whole panel behind the list included. */
        xd_insets unused;
        if (!xd_theme_start_part_margins(&theme, XD_START_PLACES_LIST, &unused))
            xd_fill_rect(cairo, right, menu_colors.right_bg);
        if (!pkg_list(menu, nullptr))
            xd_fill_rect(cairo, left, menu_colors.right_bg);
        bool places = xd_theme_draw_start_part(&theme, cairo, XD_START_PLACES_LIST, right, 0);
        if (ob.w > 0)
            (void)xd_theme_draw_start_part(&theme, cairo, XD_START_OPEN_BOX, D(ob, scale), 0);
        if (!xd_theme_draw_start_part(&theme, cairo, XD_START_PROG_LIST, list, 0)) {
            xd_fill_rect(cairo, inset, menu_colors.left_bg);
            xd_draw_bevel(cairo, inset, XD_MAX(xd_tpx(&theme, 1), 1), menu_colors.frame,
                          menu_colors.frame);
        }
        if (!places)
            xd_draw_bevel(cairo, fr, XD_MAX(xd_tpx(&theme, 1), 1), menu_colors.frame,
                          menu_colors.frame);
    }
    draw_left(menu, cairo, &theme, &menu_colors);
    draw_right(menu, cairo, &theme, &menu_colors);

    /* User picture, overhanging the top edge. */
    xd_rect pic = D(picture_rect(), scale);
    int bw = XD_MAX(xd_tpx(&theme, 2), 2);
    int fw = xd_tpx(&theme, 8);
    if (!xd_theme_draw_start_part(&theme, cairo, XD_START_USER_PICTURE,
                                  xd_rect_inset(pic, -fw, -fw), 0)) {
        xd_fill_rect(cairo, xd_rect_inset(pic, -bw, -bw), xd_rgba_hex(0xFFFFFF));
        xd_draw_bevel(cairo, xd_rect_inset(pic, -bw, -bw), 1, xd_rgba_hex(0xA0A0A0),
                      xd_rgba_hex(0x606060));
    }
    if (menu->picture != nullptr) {
        cairo_save(cairo);
        cairo_rectangle(cairo, pic.x, pic.y, pic.w, pic.h);
        cairo_clip(cairo);
        xd_image_paint_wallpaper(cairo, menu->picture, pic, 3 /* fill */, 1.0);
        cairo_restore(cairo);
    } else {
        draw_icon(shell, cairo, "avatar-default", "user-identity", pic);
    }
}

static void render(xd_start7 *menu) {
    xd_shell *shell = menu->shell;
    double scale = menu->scale;
    int dw = dev(menu->w, scale);
    int dh = dev(menu->h, scale);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&menu->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme theme;
    xd_theme_init(&theme, shell->style, &shell->scheme, scale);
    cairo_t *cairo = cairo_create(buf->surface);
    /* Transparent outside the shape (Windows 7's top band, XP's corners). */
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    if (menu->xp)
        render_xp(menu, cairo, &theme);
    else
        render_7(menu, cairo, &theme);

    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(menu->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(menu->surface, 0, 0, dw, dh);
    if (menu->viewport != nullptr)
        wp_viewport_set_destination(menu->viewport, menu->w, menu->h);
    wl_surface_commit(menu->surface);
    buf->busy = true;
    menu->dirty = false;
    menu->shown = true;
}

/* ---- submenus ------------------------------------------------------------------------ */

static void close_sub(xd_start7 *menu) {
    if (menu->sub != nullptr) {
        xd_menu_destroy(menu->sub);
        menu->sub = nullptr;
        menu->dirty = true;
    }
    menu->sub_owner = -1;
}

static xd_menu_item *add_cmd(xd_menu_tree *tree, xd_menu_model *mm, const char *label,
                             const char *icon, const char *cmd) {
    xd_menu_item *it = xd_menu_add(tree, mm, XD_MENU_ACTION, label, icon);
    it->command = cmd;
    it->disabled = cmd == nullptr;
    return it;
}

/* As add_cmd, the label written as Windows writes it ("&Restart"). */
static xd_menu_item *add_keyed_cmd(xd_menu_tree *tree, xd_menu_model *mm, const char *label,
                                   const char *icon, const char *cmd) {
    xd_menu_item *it = xd_menu_add_keyed(tree, mm, XD_MENU_ACTION, label, icon);
    it->command = cmd;
    it->disabled = cmd == nullptr;
    return it;
}

static xd_menu_model *recent_model(xd_start7 *menu) {
    xd_menu_model *mm = xd_menu_model_new(&menu->tree);
    char **paths = nullptr;
    size_t count = 0;
    xd_recent_load(&paths, &count, 15);
    xd_strbuf cmd = {0};
    for (size_t i = 0; i < count; i++) {
        const char *base = strrchr(paths[i], '/');
        xd_strbuf_clear(&cmd);
        xd_strbuf_append(&cmd, XD_STR("xdg-open '"));
        for (const char *character = paths[i]; *character != '\0'; character++) {
            if (*character == '\'')
                xd_strbuf_append(&cmd, XD_STR("'\\''"));
            else
                xd_strbuf_append_char(&cmd, *character);
        }
        xd_strbuf_append_char(&cmd, '\'');
        add_cmd(&menu->tree, mm, base != nullptr ? base + 1 : paths[i], "text-x-generic",
                xd_str_arena_cstr(&menu->tree.arena, xd_strbuf_view(&cmd)));
    }
    xd_strbuf_free(&cmd);
    xd_free_lines(paths, count);
    if (mm->count == 0)
        xd_menu_add(&menu->tree, mm, XD_MENU_ACTION, "(Empty)", nullptr)->disabled = true;
    return mm;
}

/* Where a right-column item is, in either layout. */
static xd_rect xp_or_right_item(const xd_start7 *menu, int hit) {
    if (menu->xp) {
        for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++) {
            if (xp_items[i].hit == hit)
                return xp_item_rect(menu, i);
        }
    }
    return right_item(hit);
}

/* sub_owner for a program's right-click menu. */
static constexpr int OWNER_CONTEXT = 5000;

/* The actions run after the menus (and their trees) are freed; `data` is
 * the xd_app, which belongs to shell->apps. */
static void ctx_open([[maybe_unused]] xd_shell *shell, void *data) {
    const xd_app *app = data;
    if (xd_app_launch(app))
        xd_start7_count_launch(app->id);
}
static void ctx_pin(xd_shell *shell, void *data) {
    set_pinned(shell, ((const xd_app *)data)->id, true);
}
static void ctx_unpin(xd_shell *shell, void *data) {
    set_pinned(shell, ((const xd_app *)data)->id, false);
}
static void ctx_pin_bar(xd_shell *shell, void *data) {
    xd_superbar_set_pinned(shell, ((const xd_app *)data)->id, true);
}
static void ctx_unpin_bar(xd_shell *shell, void *data) {
    xd_superbar_set_pinned(shell, ((const xd_app *)data)->id, false);
}
static void ctx_forget([[maybe_unused]] xd_shell *shell, void *data) {
    forget_launches(((const xd_app *)data)->id);
}

/* Windows 7's menu for a program: Open, Pin to / Unpin from Start menu,
 * and Remove from this list for the frequently used ones. */
static xd_menu_model *context_model(xd_start7 *menu, const row *menu_row) {
    xd_menu_model *mm = xd_menu_model_new(&menu->tree);
    void *id = (void *)(uintptr_t)menu_row->app;
    const xd_app *pinned[MAX_PINNED];
    size_t np = 0;
    load_pinned(menu->shell, pinned, &np, MAX_PINNED);
    bool is_pinned = false;
    for (size_t i = 0; i < np; i++)
        is_pinned = is_pinned || pinned[i] == menu_row->app;
    xd_menu_item *it = xd_menu_add_keyed(&menu->tree, mm, XD_MENU_ACTION, "&Open", nullptr);
    it->bold = true;
    it->action = ctx_open;
    it->action_data = id;
    xd_menu_add_gpu_items(&menu->tree, mm, menu_row->app);
    xd_menu_add(&menu->tree, mm, XD_MENU_SEPARATOR, nullptr, nullptr);
    if (!menu->xp && menu->shell->style == XD_STYLE_AERO) {
        /* Windows 7's other place to pin to (Aero's taskbar shows it). */
        bool on_bar = xd_superbar_pinned(menu->shell, menu_row->app->id);
        it = xd_menu_add_keyed(&menu->tree, mm, XD_MENU_ACTION,
                               on_bar ? "Unpin from Tas&kbar" : "Pin to Tas&kbar", nullptr);
        it->action = on_bar ? ctx_unpin_bar : ctx_pin_bar;
        it->action_data = id;
    }
    it = xd_menu_add(&menu->tree, mm, XD_MENU_ACTION,
                     is_pinned ? "Unpin from Start menu" : "Pin to Start menu", nullptr);
    it->action = is_pinned ? ctx_unpin : ctx_pin;
    it->action_data = id;
    it->disabled = !is_pinned && np >= MAX_PINNED;
    if (menu->mode == MODE_HOME && !is_pinned) {
        it = xd_menu_add_keyed(&menu->tree, mm, XD_MENU_ACTION,
                               menu->xp ? "Remove from This &List" : "Remove from this &list",
                               nullptr);
        it->action = ctx_forget;
        it->action_data = id;
    }
    xd_menu_add_program_items(&menu->tree, mm, menu_row->app);
    return mm;
}

/* Items that open a submenu (on hover, or on press). */
static bool opens_sub(const xd_start7 *menu, int hit) {
    return hit == R_RECENT || hit == R_CONTROL || hit == HIT_SHUTDOWN_ARROW ||
           (menu->xp && hit == HIT_ALL);
}

/* ...on press. Control Panel's applets cascade on hover, but clicking it
 * opens the folder, as XP's and Windows 7's default link does. */
static bool opens_on_press(const xd_start7 *menu, int hit) {
    return opens_sub(menu, hit) && hit != R_CONTROL;
}

static void switch_user(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_account_switch_user(shell);
}

static void open_sub(xd_start7 *menu, int owner) {
    /* A right-click on another program replaces its menu. */
    if (menu->sub_owner == owner && menu->sub != nullptr && owner != OWNER_CONTEXT)
        return;
    close_sub(menu);
    if (!menu->shown)
        return;
    xd_shell *shell = menu->shell;
    xd_menu_model *mm = nullptr;
    xd_rect anchor;
    if (owner == OWNER_CONTEXT) {
        if (menu->context_row < 0 || (size_t)menu->context_row >= menu->row_count ||
            menu->rows[menu->context_row].kind != ROW_APP)
            return;
        mm = context_model(menu, &menu->rows[menu->context_row]);
        anchor = (xd_rect){(int)menu->ptr_x, (int)menu->ptr_y, 1, 1};
    } else if (menu->xp && owner == HIT_ALL) {
        mm = xd_start_programs_model(shell, &menu->tree, shell->settings.start_highlight_new);
        anchor = all_rect(menu);
    } else if (owner == R_RECENT) {
        mm = recent_model(menu);
        anchor = xp_or_right_item(menu, R_RECENT);
    } else if (owner == R_CONTROL) {
        mm = xd_start_control_model(shell, &menu->tree);
        anchor = xp_or_right_item(menu, R_CONTROL);
    } else if (owner == HIT_SHUTDOWN_ARROW) {
        mm = xd_menu_model_new(&menu->tree);
        xd_menu_item *sw = xd_menu_add_keyed(&menu->tree, mm, XD_MENU_ACTION, "S&witch user",
                                             "system-switch-user");
        sw->action = switch_user;
        sw->disabled = !xd_account_can_switch_user(shell);
        add_keyed_cmd(&menu->tree, mm, "&Log off", "system-log-out",
                      xd_shell_own_command(shell, "xd-shutdown", "--action logoff"));
        add_keyed_cmd(&menu->tree, mm, "L&ock", "system-lock-screen",
                      xd_shell_own_command(shell, "xd-lock", nullptr));
        xd_menu_add(&menu->tree, mm, XD_MENU_SEPARATOR, nullptr, nullptr);
        add_keyed_cmd(&menu->tree, mm, "&Restart", "system-reboot",
                      xd_shell_own_command(shell, "xd-shutdown", "--action restart"));
        add_keyed_cmd(&menu->tree, mm, "&Sleep", "system-suspend",
                      xd_shell_own_command(shell, "xd-shutdown", "--action standby"));
        add_keyed_cmd(&menu->tree, mm, "&Hibernate", "system-suspend-hibernate",
                      xd_shell_own_command(shell, "xd-shutdown", "--action hibernate"));
        anchor = shutdown_arrow_rect();
    } else {
        return;
    }
    /* The Start menu's own cascades, as its menus (not a right-click's). */
    if (owner == HIT_ALL || owner == R_RECENT || owner == R_CONTROL)
        xd_menu_model_set_look(mm, XD_MENU_LOOK_START);
    menu->sub = xd_menu_open_attached(shell, mm, menu->xdg_surface, anchor, menu->serial);
    menu->sub_owner = owner;
    menu->dirty = true;
}

/* A program's jump list (Windows 7), beside its row. Its items act after
 * the menu closes, so it's built in the shell's context tree, which
 * outlives this menu. */
static void open_jump(xd_start7 *menu, int index) {
    if (menu->sub_owner == OWNER_JUMP && menu->sub != nullptr && menu->jump_row == index)
        return;
    close_sub(menu);
    xd_rect anchor;
    if (!menu->shown || index < 0 || (size_t)index >= menu->row_count || !menu->rows[index].jump ||
        !row_rect(menu, index, &anchor))
        return;
    xd_shell *shell = menu->shell;
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *mm = xd_jumplist_model(shell, &shell->context_tree, menu->rows[index].app->id);
    menu->sub = xd_menu_open_attached(shell, mm, menu->xdg_surface, anchor, menu->serial);
    menu->sub_owner = OWNER_JUMP;
    menu->jump_row = index;
    menu->dirty = true;
}

xd_menu *xd_start7_submenu(xd_shell *shell) {
    return shell->start7 != nullptr ? shell->start7->sub : nullptr;
}

/* ---- actions ----------------------------------------------------------------------- */

static void launch_app(xd_start7 *menu, const xd_app *app) {
    xd_menu_close_all(menu->shell); /* frees m; `app` belongs to shell->apps */
    if (xd_app_launch(app))
        xd_start7_count_launch(app->id);
}

static void run_command(xd_start7 *menu, const char *command) {
    xd_shell *shell = menu->shell;
    char *cmd = xd_str_dup_cstr(xd_str_from_cstr(command));
    xd_menu_close_all(shell); /* frees m */
    xd_shell_launch(shell, cmd);
    xd_free(cmd);
}

/* Opens a user folder (XDG user dir, else ~/fallback) in xd-files. */
static const char *folder_command(xd_shell *shell, const char *xdg_name, const char *fallback) {
    char arg[160];
    snprintf(arg, sizeof(arg), "\"$(xdg-user-dir %s 2>/dev/null || echo \"$HOME/%s\")\"",
             xdg_name, fallback);
    return xd_shell_own_command(shell, "xd-files", arg);
}

/* Activates a left row or right target. May free `menu`. */
static void activate_left(xd_start7 *menu, int index) {
    if (index < 0 || (size_t)index >= menu->row_count)
        return;
    const row *menu_row = &menu->rows[index];
    if (menu_row->kind == ROW_APP) {
        launch_app(menu, menu_row->app);
    } else if (menu_row->kind == ROW_FOLDER) {
        menu->expanded[menu_row->folder] = !menu->expanded[menu_row->folder];
        int keep = menu->scroll;
        rebuild(menu);
        menu->scroll = keep;
        menu->hot_left = index;
    }
}

static void activate_right(xd_start7 *menu, int hit) {
    xd_shell *shell = menu->shell;
    switch (hit) {
    case R_USER:
        run_command(menu, xd_shell_own_command(shell, "xd-files", "\"$HOME\""));
        return;
    case R_DOCUMENTS:
        run_command(menu, folder_command(shell, "DOCUMENTS", "Documents"));
        return;
    case R_PICTURES:
        run_command(menu, folder_command(shell, "PICTURES", "Pictures"));
        return;
    case R_MUSIC:
        run_command(menu, folder_command(shell, "MUSIC", "Music"));
        return;
    case R_COMPUTER:
        run_command(menu, xd_shell_own_command(shell, "xd-files", "computer:///"));
        return;
    case R_DEVICES:
    case HIT_XP_PRINTERS:
        run_command(menu, xd_shell_own_command(shell, "xd-printers", nullptr));
        return;
    case R_DEFAULTS:
        run_command(menu, xd_shell_own_command(shell, "xd-defaults", nullptr));
        return;
    case R_HELP:
        run_command(menu, xd_shell_own_command(shell, "xd-help", nullptr));
        return;
    case HIT_XP_SEARCH:
        run_command(menu, xd_shell_own_command(shell, "xd-search", nullptr));
        return;
    case R_RUN:
        run_command(menu, xd_shell_own_command(shell, "xd-run", nullptr));
        return;
    case R_CONTROL:
        run_command(menu, xd_shell_own_command(shell, "xd-control", nullptr));
        return;
    case R_RECENT:
    case HIT_SHUTDOWN_ARROW:
        open_sub(menu, hit);
        return;
    case HIT_SHUTDOWN:
        run_command(menu, xd_shell_own_command(shell, "xd-shutdown", "--action shutdown"));
        return;
    case HIT_ALL:
        if (menu->xp) {
            open_sub(menu, HIT_ALL); /* All Programs cascades */
            return;
        }
        menu->mode = menu->mode == MODE_ALL ? MODE_HOME : MODE_ALL;
        rebuild(menu);
        return;
    case HIT_LOGOFF:
        run_command(menu, xd_shell_own_command(shell, "xd-shutdown", "--logoff"));
        return;
    case HIT_TURNOFF:
        run_command(menu, xd_shell_own_command(shell, "xd-shutdown", nullptr));
        return;
    default:
        return;
    }
}

/* ---- popup plumbing -------------------------------------------------------------------- */

static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    xd_start7 *menu = data;
    xdg_surface_ack_configure(xdg, serial);
    menu->configured = true;
    menu->dirty = true;
}
static const struct xdg_surface_listener xdg_listener = {.configure = xdg_configure};

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *popup,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}
static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *popup) {
    xd_start7 *menu = data;
    xd_menu_close_all(menu->shell);
}
static void popup_repositioned([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_popup *popup,
                               [[maybe_unused]] uint32_t token) {}
static const struct xdg_popup_listener popup_listener = {popup_configure, popup_done,
                                                         popup_repositioned};

static void preferred_scale(void *data,
                            [[maybe_unused]] struct wp_fractional_scale_v1 *fractional_scale,
                            uint32_t scale_120) {
    xd_start7 *menu = data;
    double scale = scale_120 / 120.0;
    if (scale >= 0.5 && scale <= 8.0 && scale != menu->scale) {
        menu->scale = scale;
        menu->dirty = true;
    }
}
static const struct wp_fractional_scale_v1_listener fractional_listener = {preferred_scale};

/* XP's header shows the account's full name; Windows 7's top right item
 * is the user's folder, labelled with the login name. */
static void load_user_name(xd_start7 *menu) {
    const char *user = getenv("USER");
    const char *name = menu->xp          ? xd_account_full_name(menu->shell)
                       : user != nullptr ? user
                                         : "User";
    g_strlcpy(menu->user, name, sizeof(menu->user));
}

void xd_start7_user_changed(xd_shell *shell) {
    xd_start7 *menu = shell->start7;
    if (menu == nullptr)
        return;
    load_user_name(menu);
    menu->dirty = true;
}

static void load_user(xd_start7 *menu) {
    load_user_name(menu);
    xd_account_refresh(menu->shell);
    const char *home = getenv("HOME");
    if (home == nullptr)
        return;
    xd_strbuf buffer = {0};
    static const char *const names[] = {".face", ".face.icon"};
    for (size_t i = 0; i < XD_ARRAY_LEN(names) && menu->picture == nullptr; i++) {
        xd_strbuf_clear(&buffer);
        xd_strbuf_appendf(&buffer, "%s/%s", home, names[i]);
        if (access(xd_strbuf_cstr(&buffer), R_OK) == 0)
            menu->picture = xd_image_load(xd_strbuf_cstr(&buffer), 256, 256);
    }
    xd_strbuf_free(&buffer);
}

/* XP's menu is as tall as its columns need: the right one's items, or the
 * program list (more most-used programs than fit make it taller), but no
 * taller than the screen. */
static int xp_body_height(const xd_start7 *menu) {
    int right = 2 * XP_PAD;
    for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++)
        right += xp_item_height(&xp_items[i]);
    int left = 2 * XP_PAD + XP_SEP + XP_ALL_H + (watercolor(menu) ? 6 : 0);
    for (size_t i = 0; i < menu->row_count; i++)
        left += row_h(menu, &menu->rows[i]);
    int most = xd_shell_screen_height(menu->shell) - xp_head(menu) - xp_foot(menu) -
               xd_taskbar_thickness(menu->shell);
    return XD_MAX(XD_MIN(XD_MAX(XD_MAX(right, left), XP_BODY_H), most), right);
}

static gboolean blink_caret(gpointer data) {
    xd_start7 *menu = data;
    menu->caret_hidden = !menu->caret_hidden;
    menu->dirty = true;
    return G_SOURCE_CONTINUE;
}

/* The caret shown and blinking afresh, as after each key in Windows; at
 * Keyboard Properties' rate, steady at 0. */
static void restart_caret(xd_start7 *menu) {
    menu->caret_hidden = false;
    menu->dirty = true;
    if (menu->caret_timer != 0)
        g_source_remove(menu->caret_timer);
    menu->caret_timer = 0;
    int blink_ms = menu->shell->settings.keyboard.cursor_blink_ms;
    if (blink_ms > 0)
        menu->caret_timer = g_timeout_add((guint)blink_ms, blink_caret, menu);
}

void xd_start7_open(xd_taskbar *bar, uint32_t serial, bool xp) {
    xd_shell *shell = bar->shell;
    xd_menu_close_all(shell);
    xd_start7 *menu = XD_NEW(xd_start7);
    menu->shell = shell;
    menu->bar = bar;
    menu->scale = bar->scale;
    menu->hot_left = -1;
    menu->hot_right = -1;
    menu->pressed = -1;
    menu->sub_owner = -1;
    menu->serial = serial;
    menu->search.max_len = 256;
    menu->xp = xp;
    menu->look = shell->style;
    menu->w = xp ? XP_W : S7_W;
    menu->large = shell->settings.start_large_icons;
    menu->big_icon = shell->settings.large_icons ? 48 : 32;
    xd_menu_tree_init(&menu->tree);
    load_user(menu);
    shell->start7 = menu;
    shell->menu_cues = false; /* the keyboard's openers say otherwise */
    xd_balloon_hide(shell);  /* "New programs installed": here they are */
    if (xp)
        xd_start_menu_note_programs(shell); /* what's new since it last opened */
    rebuild(menu);
    if (!xp)
        restart_caret(menu);
    menu->body_h = xp ? xp_body_height(menu) : 0;
    menu->h = xp ? xp_head(menu) + menu->body_h + xp_foot(menu) : S7_H;

    menu->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        menu->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            shell->fractional_manager, menu->surface);
        wp_fractional_scale_v1_add_listener(menu->fractional, &fractional_listener, menu);
        menu->viewport = wp_viewporter_get_viewport(shell->viewporter, menu->surface);
    }
    menu->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, menu->surface);
    xdg_surface_add_listener(menu->xdg_surface, &xdg_listener, menu);

    xd_taskbar_take_keyboard(bar); /* before the grab, see there */
    xd_rect rect = bar->start_rect;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, menu->w, menu->h);
    xd_taskbar_popup_position(bar,
                              (xd_rect){(int)(rect.x / bar->scale), (int)(rect.y / bar->scale),
                                        (int)(rect.w / bar->scale), (int)(rect.h / bar->scale)},
                              pos);
    menu->popup = xdg_surface_get_popup(menu->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(bar->layer, menu->popup);
    xdg_popup_add_listener(menu->popup, &popup_listener, menu);
    if (serial != 0 && shell->seat != nullptr)
        xdg_popup_grab(menu->popup, shell->seat, serial);
    wl_surface_commit(menu->surface);

    shell->start_bar = bar;
    bar->dirty = true;
}

void xd_start7_close(xd_shell *shell) {
    xd_start7 *menu = shell->start7;
    if (menu == nullptr)
        return;
    close_sub(menu);
    if (menu->caret_timer != 0)
        g_source_remove(menu->caret_timer);
    shell->start7 = nullptr;
    shell->pending_start7 = false;
    xdg_popup_destroy(menu->popup);
    xdg_surface_destroy(menu->xdg_surface);
    if (menu->viewport != nullptr)
        wp_viewport_destroy(menu->viewport);
    if (menu->fractional != nullptr)
        wp_fractional_scale_v1_destroy(menu->fractional);
    wl_surface_destroy(menu->surface);
    xd_shm_ring_finish(&menu->ring);
    if (menu->picture != nullptr)
        cairo_surface_destroy(menu->picture);
    xd_menu_tree_free(&menu->tree);
    xd_textedit_free(&menu->search);
    xd_free(menu->rows);
    xd_free(menu->folders);
    xd_free(menu->folder_icons);
    xd_free(menu->expanded);
    xd_free(menu);
    xd_shell_start_menu_closed(shell);
}

bool xd_start7_owns(xd_shell *shell, struct wl_surface *surface) {
    return shell->start7 != nullptr && shell->start7->surface == surface;
}

void xd_start7_redraw(xd_shell *shell) {
    if (shell->start7 != nullptr)
        shell->start7->dirty = true;
}

void xd_start7_render_dirty(xd_shell *shell) {
    xd_start7 *menu = shell->start7;
    if (menu != nullptr && menu->dirty && menu->configured)
        render(menu);
}

/* ---- input ------------------------------------------------------------------------- */

static void arm_hover(xd_start7 *menu) {
    menu->shell->pending_start7 = true;
    if (menu->shell->menu_timer_fd < 0)
        return;
    struct itimerspec spec = {.it_value = {.tv_nsec = S7_DELAY_MS * 1000000L}};
    timerfd_settime(menu->shell->menu_timer_fd, 0, &spec, nullptr);
}

void xd_start7_timer_fired(xd_shell *shell) {
    xd_start7 *menu = shell->start7;
    shell->pending_start7 = false;
    if (menu == nullptr)
        return;
    /* A program's ▸ held: its jump list; another program: it closes. */
    if (menu->hot_arrow) {
        open_jump(menu, menu->hot_left);
        return;
    }
    if (menu->sub_owner == OWNER_JUMP && menu->hot_left >= 0 && menu->hot_left != menu->jump_row) {
        close_sub(menu);
        return;
    }
    int height = menu->hot_right;
    if (opens_sub(menu, height))
        open_sub(menu, height);
    else if (height >= 0 && menu->sub != nullptr)
        close_sub(menu);
}

void xd_start7_pointer_motion(xd_shell *shell, double x, double y) {
    xd_start7 *menu = shell->start7;
    if (menu == nullptr)
        return;
    menu->ptr_x = x;
    menu->ptr_y = y;
    /* A program pressed and pulled: dragged (to Quick Launch, the desktop),
     * as from XP's and Windows 7's menus. */
    if (menu->pressed >= 0 && menu->pressed < 1000 && (size_t)menu->pressed < menu->row_count &&
        menu->rows[menu->pressed].kind == ROW_APP &&
        fabs(x - menu->press_x) + fabs(y - menu->press_y) > S7_DRAG_PX) {
        const xd_app *app = menu->rows[menu->pressed].app;
        menu->pressed = -1;
        if (xd_dnd_start_app_drag(shell, app, menu->surface, menu->scale))
            return;
    }
    int left = row_at(menu, x, y);
    int right = right_at(menu, x, y);
    if (contains(all_rect(menu), x, y) && menu->mode != MODE_SEARCH)
        right = HIT_ALL;
    bool arrow = on_jump_arrow(menu, left, x, y);
    if (left != menu->hot_left || right != menu->hot_right || arrow != menu->hot_arrow) {
        bool other_row = left != menu->hot_left;
        menu->hot_left = left;
        menu->hot_right = right;
        menu->hot_arrow = arrow;
        menu->dirty = true;
        if (right >= 0 && right != menu->sub_owner)
            arm_hover(menu);
        else if ((arrow && !(menu->sub_owner == OWNER_JUMP && menu->jump_row == left)) ||
                 (other_row && left >= 0 && menu->sub_owner == OWNER_JUMP))
            arm_hover(menu);
    }
}

void xd_start7_pointer_leave(xd_shell *shell) {
    xd_start7 *menu = shell->start7;
    if (menu != nullptr && (menu->hot_left >= 0 || menu->hot_right >= 0)) {
        menu->hot_left = -1;
        menu->hot_right = -1;
        menu->hot_arrow = false;
        menu->dirty = true;
    }
}

void xd_start7_pointer_button(xd_shell *shell, uint32_t button, bool pressed) {
    xd_start7 *menu = shell->start7;
    if (menu == nullptr)
        return;
    /* A program's right-click menu, on release as in Windows. */
    if (button == BTN_RIGHT && !pressed && menu->hot_left >= 0 &&
        menu->rows[menu->hot_left].kind == ROW_APP) {
        menu->context_row = menu->hot_left;
        open_sub(menu, OWNER_CONTEXT);
        return;
    }
    if (button != BTN_LEFT)
        return;
    /* A program's ▸: its jump list at once (not the program). */
    if (menu->hot_arrow && menu->hot_left >= 0) {
        if (pressed)
            open_jump(menu, menu->hot_left);
        menu->pressed = -1;
        return;
    }
    int target =
        menu->hot_left >= 0 ? menu->hot_left : (menu->hot_right >= 0 ? 1000 + menu->hot_right : -1);
    if (pressed) {
        menu->pressed = target;
        menu->press_x = menu->ptr_x;
        menu->press_y = menu->ptr_y;
        /* ▸ items open on press, like menus. */
        if (opens_on_press(menu, menu->hot_right))
            activate_right(menu, menu->hot_right);
        return;
    }
    int was = menu->pressed;
    menu->pressed = -1;
    if (target != was || target < 0)
        return;
    if (target >= 1000) {
        int hit = target - 1000;
        if (!opens_on_press(menu, hit) && !(hit < R_COUNT && right_items[hit].disabled))
            activate_right(menu, hit); /* may free m */
    } else {
        activate_left(menu, target); /* may free m */
    }
}

void xd_start7_pointer_axis(xd_shell *shell, double value) {
    xd_start7 *menu = shell->start7;
    if (menu == nullptr || menu->mode == MODE_HOME)
        return;
    int count = visible_rows(menu, nullptr);
    int max_scroll = XD_MAX((int)menu->row_count - count, 0);
    menu->scroll = XD_CLAMP(menu->scroll + (value > 0 ? 3 : -3), 0, max_scroll);
    menu->hot_left = -1;
    menu->dirty = true;
}

static int step_selectable(const xd_start7 *menu, int from, int dir) {
    int count = (int)menu->row_count;
    for (int i = from + dir; i >= 0 && i < count; i += dir) {
        if (menu->rows[i].kind == ROW_APP || menu->rows[i].kind == ROW_FOLDER)
            return i;
    }
    return from;
}

static void ensure_visible(xd_start7 *menu, int index) {
    if (index < menu->scroll)
        menu->scroll = index;
    while (index >= menu->scroll + visible_rows(menu, nullptr) && menu->scroll < index)
        menu->scroll++;
}

static void search_changed(xd_start7 *menu) {
    restart_caret(menu);
    bool has = xd_textedit_view(&menu->search).len > 0;
    menu->mode = has ? MODE_SEARCH : MODE_HOME;
    rebuild(menu);
    /* Enter launches the best match. */
    if (has)
        menu->hot_left = step_selectable(menu, -1, 1);
}

/* ---- keyboard ---------------------------------------------------------------------- */

/* The keyboard moves through columns: the program list with All Programs
 * under it, the right column (Windows 7: with the Shut Down button), and
 * XP's footer (Log Off, Turn Off Computer). Up and Down stay in a column
 * and wrap, Left and Right go across, Tab goes round them all. */
typedef enum kcol : uint8_t { KCOL_NONE, KCOL_LEFT, KCOL_RIGHT, KCOL_FOOT } kcol;

/* A stop in the left column: a row index, or STOP_ALL for All Programs. */
static constexpr int STOP_ALL = -2;

static size_t left_stops(const xd_start7 *menu, int *out, size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < menu->row_count && count < max; i++) {
        if (menu->rows[i].kind == ROW_APP || menu->rows[i].kind == ROW_FOLDER)
            out[count++] = (int)i;
    }
    if (menu->mode != MODE_SEARCH && count < max)
        out[count++] = STOP_ALL;
    return count;
}

/* Hit codes of the right column or the footer, in order. */
static size_t right_stops(const xd_start7 *menu, kcol col, int *out) {
    size_t count = 0;
    if (menu->xp) {
        if (col == KCOL_FOOT) {
            out[count++] = HIT_LOGOFF;
            out[count++] = HIT_TURNOFF;
            return count;
        }
        for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++) {
            if (xp_items[i].hit != R_SEP && !xp_items[i].disabled)
                out[count++] = xp_items[i].hit;
        }
        return count;
    }
    if (col == KCOL_FOOT)
        return 0;
    for (int k = 0; k < R_COUNT; k++) {
        if (k != R_SEP && !right_items[k].disabled)
            out[count++] = k;
    }
    out[count++] = HIT_SHUTDOWN;
    out[count++] = HIT_SHUTDOWN_ARROW;
    return count;
}

static constexpr size_t MAX_RIGHT_STOPS = 32;

static kcol key_column(const xd_start7 *menu) {
    if (menu->hot_left >= 0 || menu->hot_right == HIT_ALL)
        return KCOL_LEFT;
    if (menu->hot_right < 0)
        return KCOL_NONE;
    if (menu->xp && (menu->hot_right == HIT_LOGOFF || menu->hot_right == HIT_TURNOFF))
        return KCOL_FOOT;
    return KCOL_RIGHT;
}

static void set_hot(xd_start7 *menu, int left, int right) {
    menu->hot_left = left;
    menu->hot_right = right;
    if (left >= 0)
        ensure_visible(menu, left);
    menu->dirty = true;
}

static void set_stop(xd_start7 *menu, int stop) {
    if (stop == STOP_ALL)
        set_hot(menu, -1, HIT_ALL);
    else
        set_hot(menu, stop, -1);
}

/* Moves into column `col` at its first (dir > 0) or last item. */
static void enter_column(xd_start7 *menu, kcol col, int dir) {
    if (col == KCOL_LEFT) {
        size_t cap = menu->row_count + 1;
        int *stops = xd_alloc_array(cap, sizeof(int));
        size_t count = left_stops(menu, stops, cap);
        if (count > 0)
            set_stop(menu, stops[dir > 0 ? 0 : count - 1]);
        xd_free(stops);
        return;
    }
    int stops[MAX_RIGHT_STOPS];
    size_t count = right_stops(menu, col, stops);
    if (count > 0)
        set_hot(menu, -1, stops[dir > 0 ? 0 : count - 1]);
}

/* Up and Down: the next item in the same column, wrapping round. */
static void step_column(xd_start7 *menu, int dir) {
    kcol col = key_column(menu);
    if (col == KCOL_NONE) {
        enter_column(menu, KCOL_LEFT, dir);
        return;
    }
    if (col == KCOL_LEFT) {
        size_t cap = menu->row_count + 1;
        int *stops = xd_alloc_array(cap, sizeof(int));
        size_t count = left_stops(menu, stops, cap);
        int now = menu->hot_left >= 0 ? menu->hot_left : STOP_ALL;
        size_t at = 0;
        while (at < count && stops[at] != now)
            at++;
        if (count > 0)
            set_stop(
                menu,
                stops[at == count ? 0 : (at + count + (size_t)(dir > 0 ? 1 : count - 1)) % count]);
        xd_free(stops);
        return;
    }
    int stops[MAX_RIGHT_STOPS];
    size_t count = right_stops(menu, col, stops);
    size_t at = 0;
    while (at < count && stops[at] != menu->hot_right)
        at++;
    if (count > 0)
        set_hot(menu, -1,
                stops[at == count ? 0 : (at + (size_t)(dir > 0 ? 1 : count - 1)) % count]);
}

/* Opens a ▸ item's submenu from the keyboard, on its first item. */
static void open_sub_keyboard(xd_start7 *menu, int owner) {
    open_sub(menu, owner);
    xd_menu *sub = menu->sub;
    if (sub == nullptr || menu->sub_owner != owner)
        return;
    for (size_t i = 0; i < sub->model->count; i++) {
        if (sub->model->items[i].kind != XD_MENU_SEPARATOR) {
            sub->hot = (int)i;
            sub->dirty = true;
            break;
        }
    }
}

/* Windows 7's All Programs tree: Right opens a folder, Left closes it (or
 * goes up from a program to its folder). Returns true if it did. */
static bool tree_key(xd_start7 *menu, bool right) {
    if (menu->xp || menu->mode != MODE_ALL || menu->hot_left < 0)
        return false;
    const row *menu_row = &menu->rows[menu->hot_left];
    if (menu_row->kind == ROW_FOLDER && menu->expanded[menu_row->folder] != right) {
        activate_left(menu, menu->hot_left);
        return true;
    }
    if (!right && menu_row->kind == ROW_APP && menu_row->indent > 0) {
        int i = menu->hot_left;
        while (i > 0 && menu->rows[i].kind != ROW_FOLDER)
            i--;
        set_hot(menu, i, -1);
        return true;
    }
    return false;
}

static void key_right(xd_start7 *menu) {
    switch (key_column(menu)) {
    case KCOL_NONE:
        enter_column(menu, KCOL_RIGHT, 1);
        return;
    case KCOL_LEFT:
        if (menu->xp && menu->hot_right == HIT_ALL)
            open_sub_keyboard(menu, HIT_ALL); /* All Programs cascades */
        else if (!tree_key(menu, true))
            enter_column(menu, KCOL_RIGHT, 1);
        return;
    case KCOL_RIGHT:
        if (menu->hot_right == HIT_SHUTDOWN)
            set_hot(menu, -1, HIT_SHUTDOWN_ARROW);
        else if (opens_sub(menu, menu->hot_right))
            open_sub_keyboard(menu, menu->hot_right);
        return;
    case KCOL_FOOT:
        if (menu->hot_right == HIT_LOGOFF)
            set_hot(menu, -1, HIT_TURNOFF);
        return;
    }
}

static void key_left(xd_start7 *menu) {
    switch (key_column(menu)) {
    case KCOL_NONE:
        enter_column(menu, KCOL_LEFT, 1);
        return;
    case KCOL_LEFT:
        tree_key(menu, false);
        return;
    case KCOL_RIGHT:
        if (menu->hot_right == HIT_SHUTDOWN_ARROW)
            set_hot(menu, -1, HIT_SHUTDOWN);
        else
            enter_column(menu, KCOL_LEFT, 1);
        return;
    case KCOL_FOOT:
        if (menu->hot_right == HIT_TURNOFF)
            set_hot(menu, -1, HIT_LOGOFF);
        return;
    }
}

/* Tab and Shift+Tab: the next column round. */
static void key_tab(xd_start7 *menu, int dir) {
    static const kcol xp_order[] = {KCOL_LEFT, KCOL_RIGHT, KCOL_FOOT};
    static const kcol w7_order[] = {KCOL_LEFT, KCOL_RIGHT};
    const kcol *order = menu->xp ? xp_order : w7_order;
    size_t count = menu->xp ? XD_ARRAY_LEN(xp_order) : XD_ARRAY_LEN(w7_order);
    kcol col = key_column(menu);
    size_t at = 0;
    while (at < count && order[at] != col)
        at++;
    size_t next =
        at == count ? (dir > 0 ? 0 : count - 1) : (at + (dir > 0 ? 1 : count - 1)) % count;
    enter_column(menu, order[next], 1);
}

/* Enter. May free `menu`. */
static void key_activate(xd_start7 *menu) {
    if (menu->hot_left >= 0) {
        activate_left(menu, menu->hot_left);
        return;
    }
    int hit = menu->hot_right;
    if (hit < 0 || (hit < R_COUNT && right_items[hit].disabled))
        return;
    if (hit == R_RECENT || hit == HIT_SHUTDOWN_ARROW || (menu->xp && hit == HIT_ALL))
        open_sub_keyboard(menu, hit);
    else
        activate_right(menu, hit);
}

/* XP's letters: typing one picks the item it starts (or the underlined
 * letter, for the few whose letter is known: All Programs, Run..., Log Off,
 * Turn Off Computer, as in XP's Win, U, U). One item: it runs or opens;
 * several: they take turns being highlighted. Across the program list,
 * the right column and the footer, in that order. */
typedef struct letter_stop {
    int left, right; /* row index or -1; hit code or -1 */
    const char *label;
    uint32_t mnemonic;
} letter_stop;

static size_t xp_letter_stops(const xd_start7 *menu, letter_stop *out, size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < menu->row_count && count < max; i++) {
        const row *menu_row = &menu->rows[i];
        if (menu_row->kind != ROW_APP)
            continue;
        const char *role = menu_row->pinned ? app_role(menu_row->app) : nullptr;
        out[count++] = (letter_stop){(int)i, -1, role != nullptr ? role : menu_row->app->name, 0};
    }
    if (count < max)
        out[count++] = (letter_stop){-1, HIT_ALL, "All Programs", xp_key(HIT_ALL)};
    for (size_t i = 0; i < XD_ARRAY_LEN(xp_items) && count < max; i++) {
        const xp_item *it = &xp_items[i];
        if (it->hit != R_SEP && !it->disabled)
            out[count++] = (letter_stop){-1, it->hit, it->label, xp_key(it->hit)};
    }
    if (count + 2 <= max) {
        out[count++] = (letter_stop){-1, HIT_LOGOFF, "Log Off", xp_key(HIT_LOGOFF)};
        out[count++] = (letter_stop){-1, HIT_TURNOFF, "Turn Off Computer", xp_key(HIT_TURNOFF)};
    }
    return count;
}

static uint32_t first_letter(const char *label) {
    gunichar character = g_utf8_get_char_validated(label, -1);
    return character == (gunichar)-1 || character == (gunichar)-2 ? 0
                                                                  : g_unichar_tolower(character);
}

static bool xp_letter(xd_start7 *menu, const char *utf8) {
    gunichar character = g_utf8_get_char_validated(utf8, -1);
    if (character == (gunichar)-1 || character == (gunichar)-2 || character <= 0x20)
        return false;
    uint32_t ch = g_unichar_tolower(character);
    letter_stop stops[MAX_PINNED + XD_START_PROGRAMS_MAX + 32];
    size_t stop_count = xp_letter_stops(menu, stops, XD_ARRAY_LEN(stops));
    bool explicit = false;
    for (size_t i = 0; i < stop_count; i++)
        explicit = explicit || stops[i].mnemonic == ch;
    /* Where the highlight is now, in the same order. */
    size_t at = stop_count;
    for (size_t i = 0; i < stop_count; i++) {
        if ((stops[i].left >= 0 && stops[i].left == menu->hot_left) ||
            (stops[i].left < 0 && menu->hot_left < 0 && stops[i].right == menu->hot_right))
            at = i;
    }
    size_t count = 0, pick = stop_count;
    for (size_t k = 1; k <= stop_count; k++) {
        size_t i = at == stop_count ? k - 1 : (at + k) % stop_count;
        bool match = explicit ? stops[i].mnemonic == ch
                              : stops[i].mnemonic == 0 && first_letter(stops[i].label) == ch;
        if (!match)
            continue;
        if (count++ == 0)
            pick = i;
    }
    if (count == 0)
        return true;
    set_hot(menu, stops[pick].left, stops[pick].right);
    if (count == 1)
        key_activate(menu); /* may free m */
    return true;
}

bool xd_start7_key(xd_shell *shell, uint32_t sym, const char *utf8, bool ctrl_or_alt) {
    xd_start7 *menu = shell->start7;
    if (menu == nullptr)
        return false;
    if (menu->sub != nullptr) {
        if (xd_menu_key_chain(menu->sub, sym))
            return true;
        if (sym == XKB_KEY_Left || sym == XKB_KEY_Escape) {
            close_sub(menu);
            return true;
        }
    }
    switch (sym) {
    case XKB_KEY_Escape:
        if (xd_textedit_view(&menu->search).len > 0) {
            xd_textedit_set(&menu->search, XD_STR(""));
            search_changed(menu);
        } else if (menu->mode == MODE_ALL) {
            menu->mode = MODE_HOME;
            rebuild(menu);
        } else {
            xd_menu_close_all(shell);
        }
        return true;
    case XKB_KEY_Up:
    case XKB_KEY_Down:
        step_column(menu, sym == XKB_KEY_Down ? 1 : -1);
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_Left:
        /* While searching they'd move the box's caret (ours stays at the
         * end); Tab still reaches the right column. */
        if (menu->mode == MODE_SEARCH)
            return true;
        if (sym == XKB_KEY_Right)
            key_right(menu);
        else
            key_left(menu);
        return true;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        key_tab(menu, sym == XKB_KEY_Tab ? 1 : -1);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        key_activate(menu); /* may free m */
        return true;
    case XKB_KEY_BackSpace:
        if (menu->xp)
            return true;
        xd_textedit_backspace(&menu->search, ctrl_or_alt);
        search_changed(menu);
        return true;
    default:
        break;
    }
    if (menu->xp && !ctrl_or_alt && utf8 != nullptr && utf8[0] != '\0')
        return xp_letter(menu, utf8); /* may free m */
    /* Any other typing goes to the search box, as on Windows 7 (XP has none). */
    if (!menu->xp && !ctrl_or_alt && utf8 != nullptr && (unsigned char)utf8[0] >= 0x20 &&
        utf8[0] != 0x7F) {
        xd_textedit_move(&menu->search, XD_MOTION_END, false);
        xd_textedit_insert(&menu->search, xd_str_from_cstr(utf8));
        search_changed(menu);
        return true;
    }
    return false;
}
