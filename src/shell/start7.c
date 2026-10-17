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
    XP_BODY_H = 372,
    XP_PAD = 6,
    XP_PIN_ROW = 42,
    XP_ROW = 36,
    XP_ALL_H = 30,
    XP_SEP = 9,
    XP_BOLD_ROW = 30,
    XP_RIGHT_ROW = 26,
    XP_TALL_ROW = 36,
    XP_MFU = 6,
    XP_PICTURE = 48,
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

    left_mode mode;
    row *rows;
    size_t row_count, row_cap;
    int scroll;            /* first row shown in ALL/SEARCH */
    bool *expanded;        /* ALL: per folder */
    const char **folders;  /* ALL: folder names in order */
    const char **folder_icons;
    size_t folder_count;
    xd_textedit search;

    int hot_left;          /* row index or -1 */
    int hot_right;         /* right_kind, HIT_*, or -1 */
    int pressed;           /* hit code pressed, for click-on-release */

    xd_menu_tree tree;     /* submenu models */
    xd_menu *sub;
    int sub_owner;         /* right item / HIT_SHUTDOWN_ARROW / OWNER_CONTEXT that opened `sub` */
    int context_row;       /* the program row right-clicked (OWNER_CONTEXT) */
    double ptr_x, ptr_y;   /* the pointer, where the right-click menu opens */
    bool pending_hover;

    cairo_surface_t *picture;
    char user[128];
    uint32_t serial;
};

/* ---- palettes ------------------------------------------------------------------------ */

typedef struct s7_palette {
    xd_rgba left_bg, left_text, left_dim, left_hot, left_hot_text, left_hot_border;
    xd_rgba right_bg, right_text, right_dim, right_hot, right_sep;
    xd_rgba frame, separator;
    bool classic;
} s7_palette;

static s7_palette palette(const xd_theme *t) {
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    switch (t->style) {
    case XD_STYLE_CLASSIC:
        return (s7_palette){
            .left_bg = xd_sys(t, XD_COLOR_WINDOW),
            .left_text = xd_sys(t, XD_COLOR_WINDOW_TEXT),
            .left_dim = xd_sys(t, XD_COLOR_GRAY_TEXT),
            .left_hot = xd_sys(t, XD_COLOR_HILIGHT),
            .left_hot_text = xd_sys(t, XD_COLOR_HILIGHT_TEXT),
            .left_hot_border = none,
            .right_bg = xd_sys(t, XD_COLOR_MENU),
            .right_text = xd_sys(t, XD_COLOR_MENU_TEXT),
            .right_dim = xd_sys(t, XD_COLOR_GRAY_TEXT),
            .right_hot = xd_sys(t, XD_COLOR_HILIGHT),
            .right_sep = xd_sys(t, XD_COLOR_BUTTON_SHADOW),
            .frame = xd_sys(t, XD_COLOR_BUTTON_SHADOW),
            .separator = xd_sys(t, XD_COLOR_BUTTON_SHADOW),
            .classic = true,
        };
    case XD_STYLE_WHISTLER:
        return (s7_palette){
            .left_bg = xd_rgba_hex(0xFFFFFF),
            .left_text = xd_rgba_hex(0x10265A),
            .left_dim = xd_rgba_hex(0x8A93A8),
            .left_hot = xd_rgba_hex(0xE4ECF8),
            .left_hot_text = xd_rgba_hex(0x10265A),
            .left_hot_border = xd_rgba_hex(0x9AB0D6),
            .right_bg = xd_rgba_hex(0xC9D6EA),
            .right_text = xd_rgba_hex(0x10265A),
            .right_dim = xd_rgba_hex(0x7A8AA8),
            .right_hot = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.55),
            .right_sep = xd_rgba_alpha(xd_rgba_hex(0x5F7598), 0.5),
            .frame = xd_rgba_hex(0x5F7598),
            .separator = xd_rgba_hex(0xD5DCE8),
        };
    case XD_STYLE_LUNA:
    case XD_STYLE_AERO: /* Windows 7's own colours */
    case XD_STYLE_COUNT:
        break;
    }
    /* Luna: white list, dark navy panel (the reference look). The panels are
     * opaque: Windows 7 blurs what's behind its glass, and without blur in
     * the compositor a see-through panel just shows the wallpaper sharply.
     * Aero with the compositor's blur: the panel and frame are glass. */
    if (t->style == XD_STYLE_AERO && t->glass)
        return (s7_palette){
            .left_bg = xd_rgba_hex(0xFFFFFF),
            .left_text = xd_rgba_hex(0x000000),
            .left_dim = xd_rgba_hex(0x6D6D6D),
            .left_hot = xd_rgba_hex(0xDCEBFC),
            .left_hot_text = xd_rgba_hex(0x000000),
            .left_hot_border = xd_rgba_hex(0x7DA2CE),
            .right_bg = xd_rgba_alpha(xd_rgba_hex(0x1B3A5C), 0.45),
            .right_text = xd_rgba_hex(0xFFFFFF),
            .right_dim = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.55),
            .right_hot = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.22),
            .right_sep = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.3),
            .frame = xd_rgba_alpha(xd_rgba_hex(0x0E2440), 0.7),
            .separator = xd_rgba_hex(0xD6E5F5),
        };
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

/* ---- geometry (logical) ------------------------------------------------------------ */

static xd_rect frame_rect(void) { return (xd_rect){0, S7_TOP, S7_W, S7_H - S7_TOP}; }

/* ---- Windows XP geometry ---- */
static bool watercolor(const xd_start7 *m) { return m->look == XD_STYLE_WHISTLER; }
static int xp_head(const xd_start7 *m) { return watercolor(m) ? 24 : 56; }
static int xp_foot(const xd_start7 *m) { return watercolor(m) ? 34 : 40; }
/* The white program list; on Watercolor it's inset in the blue body. */
static xd_rect xp_left_panel(const xd_start7 *m) {
    int top = xp_head(m);
    if (watercolor(m))
        return (xd_rect){4, top + 3, XP_LEFT_W - 4, XP_BODY_H - 6};
    return (xd_rect){0, top, XP_LEFT_W, XP_BODY_H};
}
static int xp_item_height(const xp_item *it) {
    return it->hit == R_SEP ? XP_SEP : it->tall ? XP_TALL_ROW : it->bold ? XP_BOLD_ROW : XP_RIGHT_ROW;
}
static xd_rect xp_item_rect(const xd_start7 *m, size_t index) {
    int y = xp_head(m) + XP_PAD;
    for (size_t i = 0; i < index; i++)
        y += xp_item_height(&xp_items[i]);
    return (xd_rect){XP_LEFT_W + 4, y, m->w - XP_LEFT_W - 8, xp_item_height(&xp_items[index])};
}
static xd_rect xp_turnoff_rect(const xd_start7 *m) {
    int f = xp_foot(m);
    return (xd_rect){m->w - 8 - 140, m->h - f + (f - 28) / 2, 140, 28};
}
static xd_rect xp_logoff_rect(const xd_start7 *m) {
    xd_rect t = xp_turnoff_rect(m);
    return (xd_rect){t.x - 6 - 82, t.y, 82, t.h};
}
static xd_rect search_rect(void) {
    return (xd_rect){S7_PAD + 4, S7_H - S7_PAD - S7_SEARCH_H - 4, S7_LEFT_W - 2 * S7_PAD - 8,
                     S7_SEARCH_H};
}
static xd_rect all_rect(const xd_start7 *m) {
    if (m->xp) {
        xd_rect p = xp_left_panel(m);
        return (xd_rect){p.x + 2, p.y + p.h - XP_PAD - XP_ALL_H, p.w - 4, XP_ALL_H};
    }
    xd_rect s = search_rect();
    return (xd_rect){S7_PAD, s.y - 8 - 28, S7_LEFT_W - 2 * S7_PAD, 28};
}
static xd_rect list_rect(const xd_start7 *m) {
    xd_rect a = all_rect(m);
    if (m->xp) {
        xd_rect p = xp_left_panel(m);
        return (xd_rect){p.x + 2, p.y + XP_PAD, p.w - 4, a.y - XP_SEP - (p.y + XP_PAD)};
    }
    return (xd_rect){S7_PAD, S7_TOP + S7_PAD, S7_LEFT_W - 2 * S7_PAD, a.y - 6 - (S7_TOP + S7_PAD)};
}
static int right_x(void) { return S7_LEFT_W; }
static xd_rect right_row(int i) {
    return (xd_rect){right_x() + 6, S7_TOP + S7_PICTURE - 4 + i * S7_RIGHT_ROW,
                     S7_W - right_x() - 12, i == R_SEP ? 10 : S7_RIGHT_ROW};
}
static xd_rect right_item(int k) {
    /* Rows after the separator are shifted by its height difference. */
    xd_rect r = right_row(k);
    if (k > R_SEP)
        r.y -= S7_RIGHT_ROW - 10;
    return r;
}
static xd_rect shutdown_rect(void) {
    return (xd_rect){right_x() + 12, S7_H - S7_PAD - 30, 96, 26};
}
static xd_rect shutdown_arrow_rect(void) {
    xd_rect s = shutdown_rect();
    return (xd_rect){s.x + s.w, s.y, 24, s.h};
}
static xd_rect picture_rect(void) {
    int cx = right_x() + (S7_W - right_x()) / 2;
    return (xd_rect){cx - S7_PICTURE / 2, 2, S7_PICTURE, S7_PICTURE};
}

static int row_height(left_mode mode, const row *r);
static int row_h(const xd_start7 *m, const row *r) {
    if (m->xp)
        return r->kind == ROW_SEP ? XP_SEP : r->pinned ? XP_PIN_ROW : XP_ROW;
    return row_height(m->mode, r);
}

static int row_height(left_mode mode, const row *r) {
    switch (r->kind) {
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

static bool contains(xd_rect r, double x, double y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* ---- left column content -------------------------------------------------------- */

static void push_row(xd_start7 *m, row r) {
    if (m->row_count == m->row_cap) {
        m->row_cap = m->row_cap == 0 ? 32 : m->row_cap * 2;
        m->rows = xd_realloc_array(m->rows, m->row_cap, sizeof(*m->rows));
    }
    m->rows[m->row_count++] = r;
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
static void load_pinned(const xd_shell *shell, const xd_app **out, size_t *n, size_t max) {
    *n = 0;
    xd_strbuf path = {0};
    xd_strbuf text = {0};
    pinned_path(&path);
    if (xd_read_file(xd_strbuf_cstr(&path), 64 * 1024, &text)) {
        xd_str rest = xd_strbuf_view(&text);
        while (rest.len > 0 && *n < max) {
            size_t nl = rest.len;
            bool more = xd_str_find_char(rest, '\n', &nl);
            char *id = xd_str_dup_cstr(xd_str_trim(xd_str_sub(rest, 0, nl)));
            const xd_app *app = id[0] != '\0' ? find_app(shell, id) : nullptr;
            if (app != nullptr)
                out[(*n)++] = app;
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
        for (size_t i = 0; i < XD_ARRAY_LEN(defaults) && *n < max; i++) {
            bool browser = i < 4;
            if ((browser && have_browser) || (!browser && have_terminal))
                continue;
            const xd_app *app = find_app(shell, defaults[i]);
            if (app != nullptr) {
                out[(*n)++] = app;
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
    size_t n = 0;
    load_pinned(shell, apps, &n, MAX_PINNED);
    GString *text = g_string_new(nullptr);
    bool present = false;
    for (size_t i = 0; i < n; i++) {
        bool same = strcmp(apps[i]->id, id) == 0;
        present = present || same;
        if (!same || pin)
            g_string_append_printf(text, "%s\n", apps[i]->id);
    }
    if (pin && !present && n < MAX_PINNED)
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

/* Remove from this list: forgets a program's launches. */
static void forget_launches(const char *id) {
    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines("start-frequent", &lines, &n, 200);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab != nullptr && strcmp(tab + 1, id) == 0) {
            xd_free(lines[i]);
            continue;
        }
        lines[m++] = lines[i];
    }
    if (!xd_state_write_lines("start-frequent", lines, m))
        fprintf(stderr, "xd-shell: cannot save launch counts\n");
    xd_free_lines(lines, m);
}

void xd_start7_count_launch(const char *id) {
    /* "count<TAB>id" lines; the most used come first. */
    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines("start-frequent", &lines, &n, 200);
    int64_t count = 0;
    char **out = xd_alloc_array(n + 1, sizeof(char *));
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        xd_str line = xd_str_from_cstr(lines[i]);
        size_t tab;
        int64_t c;
        if (!xd_str_find_char(line, '\t', &tab) ||
            !xd_str_parse_i64(xd_str_sub(line, 0, tab), 0, 1000000, &c))
            continue;
        if (xd_str_eq(xd_str_sub(line, tab + 1, line.len), xd_str_from_cstr(id))) {
            count = c;
            continue;
        }
        out[m++] = xd_str_dup_cstr(line);
    }
    xd_strbuf entry = {0};
    xd_strbuf_appendf(&entry, "%lld\t%s", (long long)(count + 1), id);
    /* Insert in descending count order. */
    size_t pos = 0;
    while (pos < m && strtoll(out[pos], nullptr, 10) > count + 1)
        pos++;
    memmove(out + pos + 1, out + pos, (m - pos) * sizeof(char *));
    out[pos] = xd_str_dup_cstr(xd_strbuf_view(&entry));
    m++;
    if (!xd_state_write_lines("start-frequent", out, m))
        fprintf(stderr, "xd-shell: cannot save launch counts\n");
    xd_strbuf_free(&entry);
    xd_free_lines(out, m);
    xd_free_lines(lines, n);
}

static void build_home(xd_start7 *m) {
    xd_shell *shell = m->shell;
    m->row_count = 0;
    m->scroll = 0;
    const xd_app *pinned[MAX_PINNED];
    size_t np = 0;
    load_pinned(shell, pinned, &np, XD_ARRAY_LEN(pinned));
    for (size_t i = 0; i < np; i++)
        push_row(m, (row){.kind = ROW_APP, .app = pinned[i], .pinned = true});

    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines("start-frequent", &lines, &n, 64);
    bool sep = false;
    size_t shown = 0;
    size_t most = m->xp ? XP_MFU : S7_FREQUENT;
    for (size_t i = 0; i < n && shown < most; i++) {
        xd_str line = xd_str_from_cstr(lines[i]);
        size_t tab;
        if (!xd_str_find_char(line, '\t', &tab))
            continue;
        char *id = xd_str_dup_cstr(xd_str_sub(line, tab + 1, line.len));
        const xd_app *app = find_app(shell, id);
        xd_free(id);
        bool is_pinned = false;
        for (size_t p = 0; p < np; p++)
            is_pinned = is_pinned || pinned[p] == app;
        if (app == nullptr || is_pinned)
            continue;
        if (!sep && np > 0) {
            push_row(m, (row){.kind = ROW_SEP});
            sep = true;
        }
        push_row(m, (row){.kind = ROW_APP, .app = app});
        shown++;
    }
    xd_free_lines(lines, n);
}

static void build_all(xd_start7 *m) {
    xd_shell *shell = m->shell;
    m->row_count = 0;
    /* Folder list (apps are sorted by folder). */
    if (m->folders == nullptr) {
        m->folders = xd_alloc_array(XD_MAX(shell->apps.count, 1), sizeof(char *));
        m->folder_icons = xd_alloc_array(XD_MAX(shell->apps.count, 1), sizeof(char *));
        m->expanded = xd_alloc_zeroed(XD_MAX(shell->apps.count, 1), sizeof(bool));
        for (size_t i = 0; i < shell->apps.count; i++) {
            const xd_app *a = &shell->apps.items[i];
            if (m->folder_count == 0 || strcmp(m->folders[m->folder_count - 1], a->folder) != 0) {
                m->folders[m->folder_count] = a->folder;
                m->folder_icons[m->folder_count] = a->folder_icon;
                m->folder_count++;
            }
        }
    }
    size_t a = 0;
    for (size_t f = 0; f < m->folder_count; f++) {
        push_row(m, (row){.kind = ROW_FOLDER,
                          .label = m->folders[f],
                          .icon = m->folder_icons[f],
                          .folder = (int)f});
        for (; a < shell->apps.count && strcmp(shell->apps.items[a].folder, m->folders[f]) == 0;
             a++) {
            if (m->expanded[f])
                push_row(m, (row){.kind = ROW_APP, .app = &shell->apps.items[a], .indent = 1});
        }
    }
}

static bool contains_nocase(const char *text, xd_str query) {
    xd_str s = xd_str_from_cstr(text);
    for (size_t i = 0; query.len <= s.len && i + query.len <= s.len; i++) {
        if (xd_str_eq_nocase(xd_str_sub(s, i, query.len), query))
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
static void search_group(xd_start7 *m, const char *heading, const xd_app_list *list, xd_str q,
                         const xd_app_list *skip) {
    size_t header = m->row_count;
    push_row(m, (row){.kind = ROW_HEADER, .label = heading});
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < list->count; i++) {
            const xd_app *a = &list->items[i];
            bool prefix = xd_str_starts_with(xd_str_from_cstr(a->name), q) ||
                          (q.len <= strlen(a->name) &&
                           xd_str_eq_nocase(xd_str_sub(xd_str_from_cstr(a->name), 0, q.len), q));
            if (matches(a, q) && prefix == (pass == 0) &&
                (skip == nullptr || find_in(skip, a->id) == nullptr))
                push_row(m, (row){.kind = ROW_APP, .app = a});
        }
    }
    if (m->row_count == header + 1)
        m->row_count = header;
}

/* Windows 7 groups the results: Programs, then Control Panel (the applets
 * are hidden from All Programs, so this is where they're found). */
static void build_search(xd_start7 *m) {
    xd_shell *shell = m->shell;
    m->row_count = 0;
    m->scroll = 0;
    xd_str q = xd_str_trim(xd_textedit_view(&m->search));
    search_group(m, "Programs", &shell->apps, q, nullptr);
    search_group(m, "Control Panel", &shell->control_apps, q, &shell->apps);
    if (m->row_count == 0)
        push_row(m, (row){.kind = ROW_HEADER, .label = "No items match your search."});
}

static void rebuild(xd_start7 *m) {
    switch (m->mode) {
    case MODE_HOME:
        build_home(m);
        break;
    case MODE_ALL:
        build_all(m);
        break;
    case MODE_SEARCH:
        build_search(m);
        break;
    }
    m->hot_left = -1;
    m->dirty = true;
}

/* Rows that fit, from `scroll`. */
static int visible_rows(const xd_start7 *m, int *heights_total) {
    xd_rect lr = list_rect(m);
    int y = 0;
    int n = 0;
    for (size_t i = (size_t)m->scroll; i < m->row_count; i++) {
        int h = row_h(m, &m->rows[i]);
        if (y + h > lr.h)
            break;
        y += h;
        n++;
    }
    if (heights_total != nullptr)
        *heights_total = y;
    return n;
}

static int row_at(const xd_start7 *m, double x, double y) {
    xd_rect lr = list_rect(m);
    if (!contains(lr, x, y))
        return -1;
    double top = lr.y;
    int n = visible_rows(m, nullptr);
    for (int k = 0; k < n; k++) {
        size_t i = (size_t)(m->scroll + k);
        int h = row_h(m, &m->rows[i]);
        if (y >= top && y < top + h)
            return (m->rows[i].kind == ROW_APP || m->rows[i].kind == ROW_FOLDER) ? (int)i : -1;
        top += h;
    }
    return -1;
}

static int right_at(const xd_start7 *m, double x, double y) {
    if (m->xp) {
        if (contains(xp_logoff_rect(m), x, y))
            return HIT_LOGOFF;
        if (contains(xp_turnoff_rect(m), x, y))
            return HIT_TURNOFF;
        for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++) {
            if (xp_items[i].hit != R_SEP && contains(xp_item_rect(m, i), x, y))
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

static int dev(double v, double s) {
    double r = round(v * s);
    XD_ASSERT(r > -65536 && r < 65536);
    return (int)r;
}

static xd_rect D(xd_rect r, double s) {
    int x0 = dev(r.x, s);
    int y0 = dev(r.y, s);
    return (xd_rect){x0, y0, dev(r.x + r.w, s) - x0, dev(r.y + r.h, s) - y0};
}

static void draw_icon(xd_shell *shell, cairo_t *cr, const char *icon, const char *fallback,
                      xd_rect r) {
    const char *names[] = {icon, fallback, "application-x-executable"};
    cairo_surface_t *img = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names), r.w);
    if (img != nullptr) {
        cairo_set_source_surface(cr, img, r.x, r.y);
        cairo_paint(cr);
    }
}

static void draw_hot(cairo_t *cr, const xd_theme *t, xd_rect r, const s7_palette *p) {
    if (p->classic) {
        xd_fill_rect(cr, r, p->left_hot);
        return;
    }
    double rad = xd_tpx(t, 3);
    xd_path_rounded(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1, rad, rad, rad, rad);
    xd_set_color(cr, p->left_hot);
    cairo_fill_preserve(cr);
    xd_set_color(cr, p->left_hot_border);
    cairo_set_line_width(cr, xd_tpx(t, 1));
    cairo_stroke(cr);
}

static void draw_triangle(cairo_t *cr, const xd_theme *t, xd_rect r, xd_rgba c, bool left) {
    double w = xd_tpx(t, 4);
    double h = xd_tpx(t, 7);
    double x = r.x + (r.w - w) / 2;
    double y = r.y + (r.h - h) / 2;
    xd_set_color(cr, c);
    if (left) {
        cairo_move_to(cr, x + w, y);
        cairo_line_to(cr, x, y + h / 2);
        cairo_line_to(cr, x + w, y + h);
    } else {
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x + w, y + h / 2);
        cairo_line_to(cr, x, y + h);
    }
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_left(xd_start7 *m, cairo_t *cr, const xd_theme *t, const s7_palette *p) {
    double s = t->scale;
    xd_shell *shell = m->shell;
    xd_rect lr = list_rect(m);
    int y = lr.y;
    int n = visible_rows(m, nullptr);
    for (int k = 0; k < n; k++) {
        size_t i = (size_t)(m->scroll + k);
        const row *r = &m->rows[i];
        int h = row_h(m, r);
        xd_rect rr = {lr.x, y, lr.w, h};
        y += h;
        if (r->kind == ROW_SEP) {
            xd_fill_rect(cr, D((xd_rect){rr.x + 8, rr.y + h / 2, rr.w - 16, 1}, s), p->separator);
            continue;
        }
        if (r->kind == ROW_HEADER) {
            xd_text_style st = xd_text(t, XD_FONT_MENU, p->left_dim, XD_ALIGN_START);
            xd_draw_text(cr, D(xd_rect_inset4(rr, 8, 0, 4, 0), s), r->label, &st);
            continue;
        }
        bool hot = (int)i == m->hot_left;
        if (hot)
            draw_hot(cr, t, D(rr, s), p);
        bool large = m->mode == MODE_HOME;
        int icon = large ? 32 : 16;
        int ix = rr.x + 4 + r->indent * 18;
        const char *label = r->kind == ROW_APP ? r->app->name : r->label;
        if (r->kind == ROW_APP)
            draw_icon(shell, cr, r->app->icon, nullptr,
                      D((xd_rect){ix, rr.y + (h - icon) / 2, icon, icon}, s));
        else
            draw_icon(shell, cr, r->icon, "folder", D((xd_rect){ix, rr.y + (h - icon) / 2, icon, icon}, s));
        xd_text_style st = xd_text(t, XD_FONT_MENU, hot ? p->left_hot_text : p->left_text,
                                   XD_ALIGN_START);
        int tx = ix + icon + 8;
        xd_draw_text(cr, D((xd_rect){tx, rr.y, rr.x + rr.w - tx - 4, h}, s), label, &st);
    }

    /* Scroll bar when the list overflows (All Programs, search). */
    int total = (int)m->row_count;
    if (m->mode != MODE_HOME && (m->scroll > 0 || (size_t)(m->scroll + n) < m->row_count)) {
        xd_rect sb = {lr.x + lr.w - 12, lr.y, 12, lr.h};
        xd_theme_draw_vscrollbar(t, cr, D(sb, s), m->scroll, n, total);
    }

    /* Separator, then All Programs / Back. */
    xd_rect ar = all_rect(m);
    xd_fill_rect(cr, D((xd_rect){ar.x + 8, ar.y - 4, ar.w - 16, 1}, s), p->separator);
    if (m->mode != MODE_SEARCH) {
        bool hot = m->hot_right == HIT_ALL;
        if (hot)
            draw_hot(cr, t, D(ar, s), p);
        xd_rgba c = hot ? p->left_hot_text : p->left_text;
        draw_triangle(cr, t, D((xd_rect){ar.x + 8, ar.y, 16, ar.h}, s), c, m->mode == MODE_ALL);
        xd_font_spec bold = t->scheme.fonts[XD_FONT_MENU];
        bold.bold = true;
        xd_text_style st = xd_text(t, XD_FONT_MENU, c, XD_ALIGN_START);
        st.font = &bold;
        xd_draw_text(cr, D((xd_rect){ar.x + 36, ar.y, ar.w - 40, ar.h}, s),
                     m->mode == MODE_ALL ? "Back" : "All Programs", &st);
    }

    /* Search box: takes any typing. */
    xd_rect sr = D(search_rect(), s);
    xd_rect inner = xd_theme_draw_field(t, cr, sr, false);
    xd_str q = xd_textedit_view(&m->search);
    int pad = xd_tpx(t, 4);
    xd_rect tr = xd_rect_inset4(inner, pad, 0, pad, 0);
    if (q.len == 0) {
        xd_font_spec it = t->scheme.fonts[XD_FONT_MENU];
        it.italic = true;
        xd_text_style st = xd_text(t, XD_FONT_MENU, p->left_dim, XD_ALIGN_START);
        st.font = &it;
        xd_draw_text(cr, tr, "Search programs and files", &st);
        xd_fill_rect(cr, (xd_rect){tr.x, tr.y + xd_tpx(t, 4), XD_MAX(xd_tpx(t, 1), 1),
                                   tr.h - xd_tpx(t, 8)},
                     p->left_text);
    } else {
        xd_text_style st = xd_text(t, XD_FONT_MENU, p->left_text, XD_ALIGN_START);
        char *text = xd_str_dup_cstr(q);
        xd_draw_text(cr, tr, text, &st);
        int w = XD_MIN(xd_text_width(cr, text, &st), tr.w - 1);
        xd_fill_rect(cr, (xd_rect){tr.x + w + 1, tr.y + xd_tpx(t, 4), XD_MAX(xd_tpx(t, 1), 1),
                                   tr.h - xd_tpx(t, 8)},
                     p->left_text);
        xd_free(text);
    }
    /* Magnifier glyph on the right of the box. */
    int g = xd_tpx(t, 10);
    double gx = inner.x + inner.w - g - pad;
    double gy = inner.y + (inner.h - g) / 2.0;
    xd_set_color(cr, p->classic ? p->left_text : xd_rgba_hex(0x2C628B));
    cairo_set_line_width(cr, XD_MAX(xd_tpx(t, 1), 1) * 1.5);
    cairo_arc(cr, gx + g * 0.6, gy + g * 0.4, g * 0.35, 0, 2 * XD_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, gx + g * 0.35, gy + g * 0.65);
    cairo_line_to(cr, gx, gy + g);
    cairo_stroke(cr);
}

static void draw_right(xd_start7 *m, cairo_t *cr, const xd_theme *t, const s7_palette *p) {
    double s = t->scale;
    for (int k = 0; k < R_COUNT; k++) {
        xd_rect r = right_item(k);
        if (k == R_SEP) {
            xd_fill_rect(cr, D((xd_rect){r.x + 4, r.y + r.h / 2, r.w - 8, 1}, s), p->right_sep);
            continue;
        }
        bool hot = (m->hot_right == k || m->sub_owner == k) && !right_items[k].disabled;
        if (hot) {
            if (p->classic)
                xd_fill_rect(cr, D(r, s), p->right_hot);
            else {
                double rad = xd_tpx(t, 3);
                xd_rect d = D(r, s);
                xd_path_rounded(cr, d.x, d.y, d.w, d.h, rad, rad, rad, rad);
                xd_set_color(cr, p->right_hot);
                cairo_fill(cr);
            }
        }
        xd_rgba c = right_items[k].disabled ? p->right_dim
                    : (hot && p->classic)    ? xd_sys(t, XD_COLOR_HILIGHT_TEXT)
                                             : p->right_text;
        xd_font_spec font = t->scheme.fonts[XD_FONT_MENU];
        font.bold = k == R_USER;
        xd_text_style st = xd_text(t, XD_FONT_MENU, c, XD_ALIGN_START);
        st.font = &font;
        const char *label = k == R_USER ? m->user : right_items[k].label;
        xd_draw_text(cr, D(xd_rect_inset4(r, 8, 0, 20, 0), s), label, &st);
        if (right_items[k].submenu)
            draw_triangle(cr, t, D((xd_rect){r.x + r.w - 16, r.y, 12, r.h}, s), c, false);
    }

    /* Shut Down split button. */
    xd_rect b = D(shutdown_rect(), s);
    xd_rect a = D(shutdown_arrow_rect(), s);
    uint8_t bs = m->hot_right == HIT_SHUTDOWN ? XD_STATE_HOT : 0;
    uint8_t as = m->hot_right == HIT_SHUTDOWN_ARROW || m->sub_owner == HIT_SHUTDOWN_ARROW
                     ? XD_STATE_HOT
                     : 0;
    if (p->classic) {
        xd_theme_draw_button(t, cr, b, bs, "Shut Down");
        xd_theme_draw_button(t, cr, a, as, "");
        draw_triangle(cr, t, a, xd_sys(t, XD_COLOR_BUTTON_TEXT), false);
    } else {
        double rad = xd_tpx(t, 2);
        xd_rect whole = {b.x, b.y, a.x + a.w - b.x, b.h};
        xd_path_rounded(cr, whole.x + 0.5, whole.y + 0.5, whole.w - 1, whole.h - 1, rad, rad, rad,
                        rad);
        xd_set_color(cr, xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), bs || as ? 0.22 : 0.1));
        cairo_fill_preserve(cr);
        xd_set_color(cr, xd_rgba_alpha(p->right_text, 0.6));
        cairo_set_line_width(cr, xd_tpx(t, 1));
        cairo_stroke(cr);
        xd_fill_rect(cr, (xd_rect){a.x, a.y + xd_tpx(t, 3), xd_tpx(t, 1), a.h - xd_tpx(t, 6)},
                     xd_rgba_alpha(p->right_text, 0.6));
        xd_text_style st = xd_text(t, XD_FONT_MENU, p->right_text, XD_ALIGN_CENTER);
        xd_draw_text(cr, b, "Shut Down", &st);
        draw_triangle(cr, t, a, p->right_text, false);
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

static xp_palette xp_colors(const xd_theme *t) {
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    xd_rgba white = xd_rgba_hex(0xFFFFFF);
    switch (t->style) {
    case XD_STYLE_CLASSIC:
        return (xp_palette){
            .head_a = xd_sys(t, XD_COLOR_ACTIVE_TITLE),
            .head_b = xd_sys(t, XD_COLOR_GRADIENT_ACTIVE_TITLE),
            .foot_a = xd_sys(t, XD_COLOR_ACTIVE_TITLE),
            .foot_b = xd_sys(t, XD_COLOR_GRADIENT_ACTIVE_TITLE),
            .head_horizontal = true,
            .left_bg = xd_sys(t, XD_COLOR_WINDOW),
            .left_text = xd_sys(t, XD_COLOR_WINDOW_TEXT),
            .left_sub = xd_sys(t, XD_COLOR_GRAY_TEXT),
            .left_sep = xd_sys(t, XD_COLOR_BUTTON_SHADOW),
            .panel_border = none,
            .right_bg = xd_sys(t, XD_COLOR_MENU),
            .right_text = xd_sys(t, XD_COLOR_MENU_TEXT),
            .right_dim = xd_sys(t, XD_COLOR_GRAY_TEXT),
            .right_sep = xd_sys(t, XD_COLOR_BUTTON_SHADOW),
            .column_edge = xd_sys(t, XD_COLOR_BUTTON_SHADOW),
            .hot = xd_sys(t, XD_COLOR_HILIGHT),
            .hot_text = xd_sys(t, XD_COLOR_HILIGHT_TEXT),
            .right_hot = xd_sys(t, XD_COLOR_HILIGHT),
            .right_hot_text = xd_sys(t, XD_COLOR_HILIGHT_TEXT),
            .name = xd_sys(t, XD_COLOR_TITLE_TEXT),
            .foot_text = xd_sys(t, XD_COLOR_TITLE_TEXT),
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
    return (xp_palette){
        .head_a = xd_rgba_hex(0x3D85EE),
        .head_b = xd_rgba_hex(0x1B58CF),
        .foot_a = xd_rgba_hex(0x3D82EC),
        .foot_b = xd_rgba_hex(0x1A52C6),
        .frame_light = xd_rgba_hex(0x0E3FAA),
        .frame_dark = xd_rgba_hex(0x0E3FAA),
        .left_bg = white,
        .left_text = xd_rgba_hex(0x000000),
        .left_sub = xd_rgba_hex(0x6D6D6D),
        .left_sep = xd_rgba_hex(0xE0DED8),
        .panel_border = none,
        .right_bg = xd_rgba_hex(0xD3E5FA),
        .right_text = xd_rgba_hex(0x0A246A),
        .right_dim = xd_rgba_hex(0x7A8AA8),
        .right_sep = xd_rgba_hex(0xA9C3EA),
        .column_edge = xd_rgba_hex(0x95BDEE),
        .hot = xd_rgba_hex(0x316AC5),
        .hot_text = white,
        .right_hot = xd_rgba_hex(0x316AC5),
        .right_hot_text = white,
        .name = white,
        .foot_text = white,
        .foot_hot = xd_rgba_alpha(white, 0.2),
        .accent = xd_rgba_hex(0xF09A3C),
    };
}

static void fill_gradient2(cairo_t *cr, xd_rect r, xd_rgba a, xd_rgba b, bool horizontal) {
    xd_fill_gradient(cr, r, a, b, horizontal);
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

static void draw_xp_arrow(cairo_t *cr, const xd_theme *t, xd_rect r, const xp_palette *p) {
    /* XP's round green arrow; Watercolor uses a plain "»". */
    if (p->whistler) {
        xd_font_spec bold = t->scheme.fonts[XD_FONT_MENU];
        bold.bold = true;
        xd_text_style st = xd_text(t, XD_FONT_MENU, p->left_text, XD_ALIGN_CENTER);
        st.font = &bold;
        xd_draw_text(cr, r, "»", &st);
        return;
    }
    double d = XD_MIN(r.w, r.h);
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, d / 2, 0, 2 * XD_PI);
    xd_set_color(cr, xd_rgba_hex(0x3C9D3C));
    cairo_fill_preserve(cr);
    xd_set_color(cr, xd_rgba_hex(0x24782A));
    cairo_set_line_width(cr, XD_MAX(xd_tpx(t, 1), 1));
    cairo_stroke(cr);
    double a = d * 0.22;
    cairo_move_to(cr, cx - a * 0.7, cy - a);
    cairo_line_to(cr, cx + a, cy);
    cairo_line_to(cr, cx - a * 0.7, cy + a);
    cairo_close_path(cr);
    xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
    cairo_fill(cr);
}

static void draw_left_xp(xd_start7 *m, cairo_t *cr, const xd_theme *t, const xp_palette *p) {
    double s = t->scale;
    xd_rect lr = list_rect(m);
    int y = lr.y;
    for (size_t i = 0; i < m->row_count; i++) {
        const row *r = &m->rows[i];
        int h = row_h(m, r);
        if (y + h > lr.y + lr.h)
            break;
        xd_rect rr = {lr.x, y, lr.w, h};
        y += h;
        if (r->kind == ROW_SEP) {
            xd_fill_rect(cr, D((xd_rect){rr.x + 6, rr.y + h / 2, rr.w - 12, 1}, s), p->left_sep);
            continue;
        }
        if (r->kind != ROW_APP)
            continue;
        bool hot = (int)i == m->hot_left;
        if (hot)
            xd_fill_rect(cr, D(rr, s), p->hot);
        int icon = 32;
        draw_icon(m->shell, cr, r->app->icon, nullptr,
                  D((xd_rect){rr.x + 4, rr.y + (h - icon) / 2, icon, icon}, s));
        int tx = rr.x + 4 + icon + 6;
        xd_rect text = {tx, rr.y, rr.x + rr.w - tx - 4, h};
        xd_rgba c = hot ? p->hot_text : p->left_text;
        xd_font_spec bold = t->scheme.fonts[XD_FONT_MENU];
        bold.bold = true;
        const char *role = r->pinned ? app_role(r->app) : nullptr;
        if (role != nullptr) {
            /* Two lines: the role in bold, the program's name below. */
            xd_text_style st = xd_text(t, XD_FONT_MENU, c, XD_ALIGN_START);
            st.font = &bold;
            xd_draw_text(cr, D((xd_rect){text.x, text.y + 4, text.w, h / 2 - 3}, s), role, &st);
            xd_text_style sub = xd_text(t, XD_FONT_MENU, hot ? p->hot_text : p->left_sub,
                                        XD_ALIGN_START);
            xd_draw_text(cr, D((xd_rect){text.x, text.y + h / 2 - 1, text.w, h / 2 - 3}, s),
                         r->app->name, &sub);
        } else {
            xd_text_style st = xd_text(t, XD_FONT_MENU, c, XD_ALIGN_START);
            if (r->pinned)
                st.font = &bold;
            xd_draw_text(cr, D(text, s), r->app->name, &st);
        }
    }

    /* Separator, then All Programs (cascades). */
    xd_rect ar = all_rect(m);
    xd_fill_rect(cr, D((xd_rect){ar.x + 6, ar.y - XP_SEP / 2 - 1, ar.w - 12, 1}, s), p->left_sep);
    bool hot = m->hot_right == HIT_ALL || m->sub_owner == HIT_ALL;
    if (hot)
        xd_fill_rect(cr, D(ar, s), p->hot);
    xd_font_spec bold = t->scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style st = xd_text(t, XD_FONT_MENU, hot ? p->hot_text : p->left_text, XD_ALIGN_END);
    st.font = &bold;
    int mid = ar.x + ar.w / 2 + 24;
    xd_draw_text(cr, D((xd_rect){ar.x, ar.y, mid - ar.x, ar.h}, s), "All Programs", &st);
    xd_rect arrow = {mid + 8, ar.y + (ar.h - 16) / 2, 16, 16};
    xp_palette ap = *p;
    if (hot)
        ap.left_text = p->hot_text;
    draw_xp_arrow(cr, t, D(arrow, s), &ap);
}

static void draw_right_xp(xd_start7 *m, cairo_t *cr, const xd_theme *t, const xp_palette *p) {
    double s = t->scale;
    for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++) {
        const xp_item *it = &xp_items[i];
        xd_rect r = xp_item_rect(m, i);
        if (it->hit == R_SEP) {
            xd_fill_rect(cr, D((xd_rect){r.x + 4, r.y + r.h / 2, r.w - 8, 1}, s), p->right_sep);
            continue;
        }
        bool hot = !it->disabled && (m->hot_right == it->hit || m->sub_owner == it->hit);
        if (hot)
            xd_fill_rect(cr, D(r, s), p->right_hot);
        int icon = 24;
        draw_icon(m->shell, cr, it->icon, "folder",
                  D((xd_rect){r.x + 4, r.y + (r.h - icon) / 2, icon, icon}, s));
        xd_rgba c = it->disabled ? p->right_dim : hot ? p->right_hot_text : p->right_text;
        xd_font_spec font = t->scheme.fonts[XD_FONT_MENU];
        font.bold = it->bold;
        xd_text_style st = xd_text(t, XD_FONT_MENU, c, XD_ALIGN_START);
        st.font = &font;
        if (it->tall) {
            st.wrap = true;
            st.ellipsize = false;
        }
        xd_draw_text(cr, D(xd_rect_inset4(r, 4 + icon + 6, 0, it->submenu ? 16 : 4, 0), s),
                     it->label, &st);
        if (it->submenu)
            draw_triangle(cr, t, D((xd_rect){r.x + r.w - 14, r.y, 12, r.h}, s), c, false);
    }
}

/* XP's footer icons: an orange tile with a padlock (Log Off) and a red
 * tile with the power symbol (Turn Off Computer). */
static void draw_foot_icon(cairo_t *cr, const xd_theme *t, xd_rect r, bool power) {
    double rad = xd_tpx(t, 3);
    double lw = XD_MAX(xd_tpx(t, 1), 1);
    cairo_new_path(cr);
    xd_path_rounded(cr, r.x + lw / 2, r.y + lw / 2, r.w - lw, r.h - lw, rad, rad, rad, rad);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    xd_rgba top = xd_rgba_hex(power ? 0xF07B5E : 0xF9C15A);
    xd_rgba bottom = xd_rgba_hex(power ? 0xC2310F : 0xD9780A);
    cairo_pattern_add_color_stop_rgb(g, 0, top.r, top.g, top.b);
    cairo_pattern_add_color_stop_rgb(g, 1, bottom.r, bottom.g, bottom.b);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);

    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, u = r.w / 22.0;
    xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (power) {
        cairo_set_line_width(cr, 2.2 * u);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy + u, 6 * u, -XD_PI / 2 + 0.75, -XD_PI / 2 - 0.75 + 2 * XD_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, cy - 6 * u);
        cairo_line_to(cr, cx, cy + u);
        cairo_stroke(cr);
    } else {
        /* Padlock: shackle and body. */
        cairo_set_line_width(cr, 2 * u);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy - 2 * u, 4 * u, XD_PI, 2 * XD_PI);
        cairo_stroke(cr);
        cairo_rectangle(cr, cx - 6 * u, cy - u, 12 * u, 9 * u);
        cairo_fill(cr);
        xd_set_color(cr, bottom);
        cairo_rectangle(cr, cx - u, cy + 2 * u, 2 * u, 3 * u);
        cairo_fill(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static void draw_foot_button(cairo_t *cr, const xd_theme *t, const xp_palette *p, xd_rect r,
                             bool hot, bool power, const char *label) {
    double s = t->scale;
    if (hot) {
        xd_rect d = D(r, s);
        if (p->classic) {
            xd_fill_rect(cr, d, p->foot_hot);
        } else {
            double rad = xd_tpx(t, 3);
            xd_path_rounded(cr, d.x, d.y, d.w, d.h, rad, rad, rad, rad);
            xd_set_color(cr, p->foot_hot);
            cairo_fill(cr);
        }
    }
    int is = 22;
    draw_foot_icon(cr, t, D((xd_rect){r.x + 3, r.y + (r.h - is) / 2, is, is}, s), power);
    xd_text_style st = xd_text(t, XD_FONT_MENU, p->foot_text, XD_ALIGN_START);
    xd_draw_text(cr, D((xd_rect){r.x + 3 + is + 5, r.y, r.w - is - 10, r.h}, s), label, &st);
}

static void render_xp(xd_start7 *m, cairo_t *cr, const xd_theme *t) {
    double s = t->scale;
    xp_palette p = xp_colors(t);
    int head = xp_head(m), foot = xp_foot(m);
    xd_rect whole = D((xd_rect){0, 0, m->w, m->h}, s);
    xd_rect head_r = D((xd_rect){0, 0, m->w, head}, s);
    xd_rect body_r = D((xd_rect){0, head, m->w, XP_BODY_H}, s);
    xd_rect foot_r = D((xd_rect){0, head + XP_BODY_H, m->w, foot}, s);
    double rad = p.classic || p.whistler ? 0 : xd_tpx(t, 7);

    cairo_save(cr);
    xd_path_rounded(cr, whole.x, whole.y, whole.w, whole.h, rad, rad, 0, 0);
    cairo_clip(cr);

    /* Header */
    fill_gradient2(cr, head_r, p.head_a, p.head_b, p.head_horizontal);
    if (p.whistler) {
        int mx = dev(m->w * 0.58, s);
        xd_fill_rect(cr, (xd_rect){mx, head_r.y, head_r.x + head_r.w - mx, head_r.h}, p.body);
        xd_theme_draw_whistler_mosaic(t, cr, head_r, mx);
        xd_fill_rect(cr, (xd_rect){head_r.x, head_r.y, head_r.w, XD_MAX(xd_tpx(t, 1), 1)},
                     xd_rgba_hex(0x7DB1FB));
    }

    /* Body: left list and right column */
    xd_rect left = D(xp_left_panel(m), s);
    if (p.whistler) {
        xd_fill_rect(cr, body_r, p.body);
        xd_fill_rect(cr, left, p.left_bg);
        xd_draw_bevel(cr, left, XD_MAX(xd_tpx(t, 1), 1), p.panel_border, p.panel_border);
    } else {
        xd_rect right = D((xd_rect){XP_LEFT_W, head, m->w - XP_LEFT_W, XP_BODY_H}, s);
        xd_fill_rect(cr, left, p.left_bg);
        xd_fill_rect(cr, right, p.right_bg);
        xd_fill_rect(cr, (xd_rect){right.x, right.y, XD_MAX(xd_tpx(t, 1), 1), right.h},
                     p.column_edge);
    }

    /* Footer */
    fill_gradient2(cr, foot_r, p.foot_a, p.foot_b, p.head_horizontal);
    cairo_restore(cr);

    /* XP's orange line under the header, fading out at both ends. */
    if (p.accent.a > 0) {
        xd_rect line = D((xd_rect){0, head, m->w, 2}, s);
        cairo_pattern_t *g = cairo_pattern_create_linear(line.x, 0, line.x + line.w, 0);
        cairo_pattern_add_color_stop_rgba(g, 0, p.accent.r, p.accent.g, p.accent.b, 0);
        cairo_pattern_add_color_stop_rgba(g, 0.5, p.accent.r, p.accent.g, p.accent.b, 1);
        cairo_pattern_add_color_stop_rgba(g, 1, p.accent.r, p.accent.g, p.accent.b, 0);
        cairo_rectangle(cr, line.x, line.y, line.w, line.h);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* Frame */
    if (p.classic) {
        xd_draw_bevel(cr, whole, XD_MAX(xd_tpx(t, 1), 1), xd_sys(t, XD_COLOR_BUTTON_LIGHT),
                      xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
    } else if (p.whistler) {
        xd_draw_bevel(cr, whole, XD_MAX(xd_tpx(t, 1), 1), p.frame_light, p.frame_dark);
    } else {
        double lw = XD_MAX(xd_tpx(t, 1), 1);
        xd_path_rounded(cr, whole.x + lw / 2, whole.y + lw / 2, whole.w - lw, whole.h - lw, rad,
                        rad, 0, 0);
        xd_set_color(cr, p.frame_dark);
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);
    }

    /* User picture and name */
    xd_font_spec name_font = {.family = XD_FONT_FAMILY_TREBUCHET,
                              .size_pt = p.whistler ? 9 : 13,
                              .bold = true};
    xd_text_style ns = {
        .font = &name_font,
        .scale = s,
        .color = p.name,
        .align = XD_ALIGN_START,
        .ellipsize = true,
        .shadow = !p.classic && !p.whistler,
        .shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.45),
        .smoothing = t->scheme.font_smoothing,
    };
    if (p.whistler) {
        xd_draw_text(cr, D((xd_rect){8, 0, m->w / 2, head}, s), m->user, &ns);
    } else {
        xd_rect pic = D((xd_rect){8, (head - XP_PICTURE) / 2, XP_PICTURE, XP_PICTURE}, s);
        int bw = XD_MAX(xd_tpx(t, 2), 2);
        xd_rect outer = xd_rect_inset(pic, -bw, -bw);
        double prad = p.classic ? 0 : xd_tpx(t, 3);
        xd_path_rounded(cr, outer.x, outer.y, outer.w, outer.h, prad, prad, prad, prad);
        xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
        cairo_fill(cr);
        if (m->picture != nullptr) {
            cairo_save(cr);
            cairo_rectangle(cr, pic.x, pic.y, pic.w, pic.h);
            cairo_clip(cr);
            xd_image_paint_wallpaper(cr, m->picture, pic, 3 /* fill */, 1.0);
            cairo_restore(cr);
        } else {
            draw_icon(m->shell, cr, "avatar-default", "user-identity", pic);
        }
        xd_draw_text(cr, D((xd_rect){8 + XP_PICTURE + 10, 0, m->w - XP_PICTURE - 30, head}, s),
                     m->user, &ns);
    }

    draw_left_xp(m, cr, t, &p);
    draw_right_xp(m, cr, t, &p);
    draw_foot_button(cr, t, &p, xp_logoff_rect(m), m->hot_right == HIT_LOGOFF, false,
                     "Log Off");
    draw_foot_button(cr, t, &p, xp_turnoff_rect(m), m->hot_right == HIT_TURNOFF, true,
                     "Turn Off Computer");
}

static void render_7(xd_start7 *m, cairo_t *cr, const xd_theme *tp) {
    xd_shell *shell = m->shell;
    double s = m->scale;
    xd_theme t = *tp;
    s7_palette p = palette(&t);
    xd_rect fr = D(frame_rect(), s);
    xd_rect left = D((xd_rect){0, S7_TOP, S7_LEFT_W, S7_H - S7_TOP}, s);
    xd_rect right = D((xd_rect){S7_LEFT_W, S7_TOP, S7_W - S7_LEFT_W, S7_H - S7_TOP}, s);
    if (p.classic) {
        xd_theme_draw_menu_frame(&t, cr, fr);
        xd_menu_metrics mm = xd_theme_menu_metrics(&t);
        xd_fill_rect(cr, (xd_rect){right.x, right.y + mm.border, right.w - mm.border,
                                   right.h - 2 * mm.border},
                     p.right_bg);
        xd_fill_rect(cr, (xd_rect){left.x + mm.border, left.y + mm.border, left.w - mm.border,
                                   left.h - 2 * mm.border},
                     p.left_bg);
    } else {
        xd_fill_rect(cr, right, p.right_bg);
        /* The white list sits inset in the panel, as on Windows 7. */
        xd_rect inset = D((xd_rect){S7_PAD - 2, S7_TOP + S7_PAD - 2, S7_LEFT_W - 2 * S7_PAD + 4,
                                    S7_H - S7_TOP - 2 * S7_PAD + 4},
                          s);
        /* The whole panel behind the list, including the gap between the
         * list and the right column. */
        xd_fill_rect(cr, left, p.right_bg);
        xd_fill_rect(cr, inset, p.left_bg);
        xd_draw_bevel(cr, inset, XD_MAX(xd_tpx(&t, 1), 1), p.frame, p.frame);
        xd_draw_bevel(cr, fr, XD_MAX(xd_tpx(&t, 1), 1), p.frame, p.frame);
    }
    draw_left(m, cr, &t, &p);
    draw_right(m, cr, &t, &p);

    /* User picture, overhanging the top edge. */
    xd_rect pic = D(picture_rect(), s);
    int bw = XD_MAX(xd_tpx(&t, 2), 2);
    xd_fill_rect(cr, xd_rect_inset(pic, -bw, -bw), xd_rgba_hex(0xFFFFFF));
    xd_draw_bevel(cr, xd_rect_inset(pic, -bw, -bw), 1, xd_rgba_hex(0xA0A0A0), xd_rgba_hex(0x606060));
    if (m->picture != nullptr) {
        cairo_save(cr);
        cairo_rectangle(cr, pic.x, pic.y, pic.w, pic.h);
        cairo_clip(cr);
        xd_image_paint_wallpaper(cr, m->picture, pic, 3 /* fill */, 1.0);
        cairo_restore(cr);
    } else {
        draw_icon(shell, cr, "avatar-default", "user-identity", pic);
    }
}

static void render(xd_start7 *m) {
    xd_shell *shell = m->shell;
    double s = m->scale;
    int dw = dev(m->w, s);
    int dh = dev(m->h, s);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&m->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, s);
    cairo_t *cr = cairo_create(buf->surface);
    /* Transparent outside the shape (Windows 7's top band, XP's corners). */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (m->xp)
        render_xp(m, cr, &t);
    else
        render_7(m, cr, &t);

    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(m->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(m->surface, 0, 0, dw, dh);
    if (m->viewport != nullptr)
        wp_viewport_set_destination(m->viewport, m->w, m->h);
    wl_surface_commit(m->surface);
    buf->busy = true;
    m->dirty = false;
    m->shown = true;
}

/* ---- submenus ------------------------------------------------------------------------ */

static void close_sub(xd_start7 *m) {
    if (m->sub != nullptr) {
        xd_menu_destroy(m->sub);
        m->sub = nullptr;
        m->dirty = true;
    }
    m->sub_owner = -1;
}

static xd_menu_item *add_cmd(xd_menu_tree *tree, xd_menu_model *mm, const char *label,
                             const char *icon, const char *cmd) {
    xd_menu_item *it = xd_menu_add(tree, mm, XD_MENU_ACTION, label, icon);
    it->command = cmd;
    it->disabled = cmd == nullptr;
    return it;
}

static xd_menu_model *recent_model(xd_start7 *m) {
    xd_menu_model *mm = xd_menu_model_new(&m->tree);
    char **paths = nullptr;
    size_t n = 0;
    xd_recent_load(&paths, &n, 15);
    xd_strbuf cmd = {0};
    for (size_t i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        xd_strbuf_clear(&cmd);
        xd_strbuf_append(&cmd, XD_STR("xdg-open '"));
        for (const char *c = paths[i]; *c != '\0'; c++) {
            if (*c == '\'')
                xd_strbuf_append(&cmd, XD_STR("'\\''"));
            else
                xd_strbuf_append_char(&cmd, *c);
        }
        xd_strbuf_append_char(&cmd, '\'');
        add_cmd(&m->tree, mm, base != nullptr ? base + 1 : paths[i], "text-x-generic",
                xd_str_arena_cstr(&m->tree.arena, xd_strbuf_view(&cmd)));
    }
    xd_strbuf_free(&cmd);
    xd_free_lines(paths, n);
    if (mm->count == 0)
        xd_menu_add(&m->tree, mm, XD_MENU_ACTION, "(Empty)", nullptr)->disabled = true;
    return mm;
}

/* Where a right-column item is, in either layout. */
static xd_rect xp_or_right_item(const xd_start7 *m, int hit) {
    if (m->xp) {
        for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++) {
            if (xp_items[i].hit == hit)
                return xp_item_rect(m, i);
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
static void ctx_forget([[maybe_unused]] xd_shell *shell, void *data) {
    forget_launches(((const xd_app *)data)->id);
}

/* Windows 7's menu for a program: Open, Pin to / Unpin from Start menu,
 * and Remove from this list for the frequently used ones. */
static xd_menu_model *context_model(xd_start7 *m, const row *r) {
    xd_menu_model *mm = xd_menu_model_new(&m->tree);
    void *id = (void *)(uintptr_t)r->app;
    const xd_app *pinned[MAX_PINNED];
    size_t np = 0;
    load_pinned(m->shell, pinned, &np, MAX_PINNED);
    bool is_pinned = false;
    for (size_t i = 0; i < np; i++)
        is_pinned = is_pinned || pinned[i] == r->app;
    xd_menu_item *it = xd_menu_add(&m->tree, mm, XD_MENU_ACTION, "Open", nullptr);
    it->bold = true;
    it->action = ctx_open;
    it->action_data = id;
    xd_menu_add(&m->tree, mm, XD_MENU_SEPARATOR, nullptr, nullptr);
    it = xd_menu_add(&m->tree, mm, XD_MENU_ACTION,
                     is_pinned ? "Unpin from Start menu" : "Pin to Start menu", nullptr);
    it->action = is_pinned ? ctx_unpin : ctx_pin;
    it->action_data = id;
    it->disabled = !is_pinned && np >= MAX_PINNED;
    if (m->mode == MODE_HOME && !is_pinned) {
        it = xd_menu_add(&m->tree, mm, XD_MENU_ACTION, "Remove from this list", nullptr);
        it->action = ctx_forget;
        it->action_data = id;
    }
    return mm;
}

/* Items that open a submenu (on hover, or on press). */
static bool opens_sub(const xd_start7 *m, int hit) {
    return hit == R_RECENT || hit == R_CONTROL || hit == HIT_SHUTDOWN_ARROW ||
           (m->xp && hit == HIT_ALL);
}

/* ...on press. Control Panel's applets cascade on hover, but clicking it
 * opens the folder, as XP's and Windows 7's default link does. */
static bool opens_on_press(const xd_start7 *m, int hit) {
    return opens_sub(m, hit) && hit != R_CONTROL;
}

static void switch_user(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_account_switch_user(shell);
}

static void open_sub(xd_start7 *m, int owner) {
    /* A right-click on another program replaces its menu. */
    if (m->sub_owner == owner && m->sub != nullptr && owner != OWNER_CONTEXT)
        return;
    close_sub(m);
    if (!m->shown)
        return;
    xd_shell *shell = m->shell;
    xd_menu_model *mm = nullptr;
    xd_rect anchor;
    if (owner == OWNER_CONTEXT) {
        if (m->context_row < 0 || (size_t)m->context_row >= m->row_count ||
            m->rows[m->context_row].kind != ROW_APP)
            return;
        mm = context_model(m, &m->rows[m->context_row]);
        anchor = (xd_rect){(int)m->ptr_x, (int)m->ptr_y, 1, 1};
    } else if (m->xp && owner == HIT_ALL) {
        mm = xd_start_programs_model(shell, &m->tree);
        anchor = all_rect(m);
    } else if (owner == R_RECENT) {
        mm = recent_model(m);
        anchor = xp_or_right_item(m, R_RECENT);
    } else if (owner == R_CONTROL) {
        mm = xd_start_control_model(shell, &m->tree);
        anchor = xp_or_right_item(m, R_CONTROL);
    } else if (owner == HIT_SHUTDOWN_ARROW) {
        mm = xd_menu_model_new(&m->tree);
        xd_menu_item *sw = xd_menu_add(&m->tree, mm, XD_MENU_ACTION, "Switch user",
                                       "system-switch-user");
        sw->action = switch_user;
        sw->disabled = !xd_account_can_switch_user(shell);
        add_cmd(&m->tree, mm, "Log off", "system-log-out",
                xd_shell_own_command(shell, "xd-shutdown", "--action logoff"));
        add_cmd(&m->tree, mm, "Lock", "system-lock-screen",
                xd_shell_own_command(shell, "xd-lock", nullptr));
        xd_menu_add(&m->tree, mm, XD_MENU_SEPARATOR, nullptr, nullptr);
        add_cmd(&m->tree, mm, "Restart", "system-reboot",
                xd_shell_own_command(shell, "xd-shutdown", "--action restart"));
        add_cmd(&m->tree, mm, "Sleep", "system-suspend",
                xd_shell_own_command(shell, "xd-shutdown", "--action standby"));
        add_cmd(&m->tree, mm, "Hibernate", "system-suspend-hibernate",
                xd_shell_own_command(shell, "xd-shutdown", "--action hibernate"));
        anchor = shutdown_arrow_rect();
    } else {
        return;
    }
    m->sub = xd_menu_open_attached(shell, mm, m->xdg_surface, anchor, m->serial);
    m->sub_owner = owner;
    m->dirty = true;
}

xd_menu *xd_start7_submenu(xd_shell *shell) {
    return shell->start7 != nullptr ? shell->start7->sub : nullptr;
}

/* ---- actions ----------------------------------------------------------------------- */

static void launch_app(xd_start7 *m, const xd_app *app) {
    xd_menu_close_all(m->shell); /* frees m; `app` belongs to shell->apps */
    if (xd_app_launch(app))
        xd_start7_count_launch(app->id);
}

static void run_command(xd_start7 *m, const char *command) {
    xd_shell *shell = m->shell;
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

/* Activates a left row or right target. May free `m`. */
static void activate_left(xd_start7 *m, int index) {
    if (index < 0 || (size_t)index >= m->row_count)
        return;
    const row *r = &m->rows[index];
    if (r->kind == ROW_APP) {
        launch_app(m, r->app);
    } else if (r->kind == ROW_FOLDER) {
        m->expanded[r->folder] = !m->expanded[r->folder];
        int keep = m->scroll;
        rebuild(m);
        m->scroll = keep;
        m->hot_left = index;
    }
}

static void activate_right(xd_start7 *m, int hit) {
    xd_shell *shell = m->shell;
    switch (hit) {
    case R_USER:
        run_command(m, xd_shell_own_command(shell, "xd-files", "\"$HOME\""));
        return;
    case R_DOCUMENTS:
        run_command(m, folder_command(shell, "DOCUMENTS", "Documents"));
        return;
    case R_PICTURES:
        run_command(m, folder_command(shell, "PICTURES", "Pictures"));
        return;
    case R_MUSIC:
        run_command(m, folder_command(shell, "MUSIC", "Music"));
        return;
    case R_COMPUTER:
        run_command(m, xd_shell_own_command(shell, "xd-files", "computer:///"));
        return;
    case R_DEVICES:
    case HIT_XP_PRINTERS:
        run_command(m, xd_shell_own_command(shell, "xd-printers", nullptr));
        return;
    case R_DEFAULTS:
        run_command(m, xd_shell_own_command(shell, "xd-defaults", nullptr));
        return;
    case R_HELP:
        run_command(m, xd_shell_own_command(shell, "xd-help", nullptr));
        return;
    case HIT_XP_SEARCH:
        run_command(m, xd_shell_own_command(shell, "xd-search", nullptr));
        return;
    case R_RUN:
        run_command(m, xd_shell_own_command(shell, "xd-run", nullptr));
        return;
    case R_CONTROL:
        run_command(m, xd_shell_own_command(shell, "xd-control", nullptr));
        return;
    case R_RECENT:
    case HIT_SHUTDOWN_ARROW:
        open_sub(m, hit);
        return;
    case HIT_SHUTDOWN:
        run_command(m, xd_shell_own_command(shell, "xd-shutdown", "--action shutdown"));
        return;
    case HIT_ALL:
        if (m->xp) {
            open_sub(m, HIT_ALL); /* All Programs cascades */
            return;
        }
        m->mode = m->mode == MODE_ALL ? MODE_HOME : MODE_ALL;
        rebuild(m);
        return;
    case HIT_LOGOFF:
        run_command(m, xd_shell_own_command(shell, "xd-shutdown", "--logoff"));
        return;
    case HIT_TURNOFF:
        run_command(m, xd_shell_own_command(shell, "xd-shutdown", nullptr));
        return;
    default:
        return;
    }
}

/* ---- popup plumbing -------------------------------------------------------------------- */

static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    xd_start7 *m = data;
    xdg_surface_ack_configure(xdg, serial);
    m->configured = true;
    m->dirty = true;
}
static const struct xdg_surface_listener xdg_listener = {.configure = xdg_configure};

static void popup_configure([[maybe_unused]] void *d, [[maybe_unused]] struct xdg_popup *p,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}
static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *p) {
    xd_start7 *m = data;
    xd_menu_close_all(m->shell);
}
static void popup_repositioned([[maybe_unused]] void *d, [[maybe_unused]] struct xdg_popup *p,
                               [[maybe_unused]] uint32_t token) {}
static const struct xdg_popup_listener popup_listener = {popup_configure, popup_done,
                                                         popup_repositioned};

static void preferred_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *f,
                            uint32_t v) {
    xd_start7 *m = data;
    double s = v / 120.0;
    if (s >= 0.5 && s <= 8.0 && s != m->scale) {
        m->scale = s;
        m->dirty = true;
    }
}
static const struct wp_fractional_scale_v1_listener fractional_listener = {preferred_scale};

/* XP's header shows the account's full name; Windows 7's top right item
 * is the user's folder, labelled with the login name. */
static void load_user_name(xd_start7 *m) {
    const char *user = getenv("USER");
    const char *name = m->xp ? xd_account_full_name(m->shell) : user != nullptr ? user : "User";
    g_strlcpy(m->user, name, sizeof(m->user));
}

void xd_start7_user_changed(xd_shell *shell) {
    xd_start7 *m = shell->start7;
    if (m == nullptr)
        return;
    load_user_name(m);
    m->dirty = true;
}

static void load_user(xd_start7 *m) {
    load_user_name(m);
    xd_account_refresh(m->shell);
    const char *home = getenv("HOME");
    if (home == nullptr)
        return;
    xd_strbuf p = {0};
    static const char *const names[] = {".face", ".face.icon"};
    for (size_t i = 0; i < XD_ARRAY_LEN(names) && m->picture == nullptr; i++) {
        xd_strbuf_clear(&p);
        xd_strbuf_appendf(&p, "%s/%s", home, names[i]);
        if (access(xd_strbuf_cstr(&p), R_OK) == 0)
            m->picture = xd_image_load(xd_strbuf_cstr(&p), 256, 256);
    }
    xd_strbuf_free(&p);
}

void xd_start7_open(xd_taskbar *bar, uint32_t serial, bool xp) {
    xd_shell *shell = bar->shell;
    xd_menu_close_all(shell);
    xd_start7 *m = XD_NEW(xd_start7);
    m->shell = shell;
    m->bar = bar;
    m->scale = bar->scale;
    m->hot_left = -1;
    m->hot_right = -1;
    m->pressed = -1;
    m->sub_owner = -1;
    m->serial = serial;
    m->search.max_len = 256;
    m->xp = xp;
    m->look = shell->style;
    m->w = xp ? XP_W : S7_W;
    m->h = xp ? xp_head(m) + XP_BODY_H + xp_foot(m) : S7_H;
    xd_menu_tree_init(&m->tree);
    load_user(m);
    shell->start7 = m;
    rebuild(m);

    m->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        m->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            shell->fractional_manager, m->surface);
        wp_fractional_scale_v1_add_listener(m->fractional, &fractional_listener, m);
        m->viewport = wp_viewporter_get_viewport(shell->viewporter, m->surface);
    }
    m->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, m->surface);
    xdg_surface_add_listener(m->xdg_surface, &xdg_listener, m);

    xd_taskbar_take_keyboard(bar); /* before the grab, see there */
    xd_rect r = bar->start_rect;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, m->w, m->h);
    xd_taskbar_popup_position(bar,
                              (xd_rect){(int)(r.x / bar->scale), (int)(r.y / bar->scale),
                                        (int)(r.w / bar->scale), (int)(r.h / bar->scale)},
                              pos);
    m->popup = xdg_surface_get_popup(m->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(bar->layer, m->popup);
    xdg_popup_add_listener(m->popup, &popup_listener, m);
    if (serial != 0 && shell->seat != nullptr)
        xdg_popup_grab(m->popup, shell->seat, serial);
    wl_surface_commit(m->surface);

    shell->start_bar = bar;
    bar->dirty = true;
}

void xd_start7_close(xd_shell *shell) {
    xd_start7 *m = shell->start7;
    if (m == nullptr)
        return;
    close_sub(m);
    shell->start7 = nullptr;
    shell->pending_start7 = false;
    xdg_popup_destroy(m->popup);
    xdg_surface_destroy(m->xdg_surface);
    if (m->viewport != nullptr)
        wp_viewport_destroy(m->viewport);
    if (m->fractional != nullptr)
        wp_fractional_scale_v1_destroy(m->fractional);
    wl_surface_destroy(m->surface);
    xd_shm_ring_finish(&m->ring);
    if (m->picture != nullptr)
        cairo_surface_destroy(m->picture);
    xd_menu_tree_free(&m->tree);
    xd_textedit_free(&m->search);
    xd_free(m->rows);
    xd_free(m->folders);
    xd_free(m->folder_icons);
    xd_free(m->expanded);
    xd_free(m);
    xd_shell_start_menu_closed(shell);
}

bool xd_start7_owns(xd_shell *shell, struct wl_surface *surface) {
    return shell->start7 != nullptr && shell->start7->surface == surface;
}

void xd_start7_render_dirty(xd_shell *shell) {
    xd_start7 *m = shell->start7;
    if (m != nullptr && m->dirty && m->configured)
        render(m);
}

/* ---- input ------------------------------------------------------------------------- */

static void arm_hover(xd_start7 *m) {
    m->shell->pending_start7 = true;
    if (m->shell->menu_timer_fd < 0)
        return;
    struct itimerspec spec = {.it_value = {.tv_nsec = S7_DELAY_MS * 1000000L}};
    timerfd_settime(m->shell->menu_timer_fd, 0, &spec, nullptr);
}

void xd_start7_timer_fired(xd_shell *shell) {
    xd_start7 *m = shell->start7;
    shell->pending_start7 = false;
    if (m == nullptr)
        return;
    int h = m->hot_right;
    if (opens_sub(m, h))
        open_sub(m, h);
    else if (h >= 0 && m->sub != nullptr)
        close_sub(m);
}

void xd_start7_pointer_motion(xd_shell *shell, double x, double y) {
    xd_start7 *m = shell->start7;
    if (m == nullptr)
        return;
    m->ptr_x = x;
    m->ptr_y = y;
    int left = row_at(m, x, y);
    int right = right_at(m, x, y);
    if (contains(all_rect(m), x, y) && m->mode != MODE_SEARCH)
        right = HIT_ALL;
    if (left != m->hot_left || right != m->hot_right) {
        m->hot_left = left;
        m->hot_right = right;
        m->dirty = true;
        if (right >= 0 && right != m->sub_owner)
            arm_hover(m);
    }
}

void xd_start7_pointer_leave(xd_shell *shell) {
    xd_start7 *m = shell->start7;
    if (m != nullptr && (m->hot_left >= 0 || m->hot_right >= 0)) {
        m->hot_left = -1;
        m->hot_right = -1;
        m->dirty = true;
    }
}

void xd_start7_pointer_button(xd_shell *shell, uint32_t button, bool pressed) {
    xd_start7 *m = shell->start7;
    if (m == nullptr)
        return;
    /* A program's right-click menu, on release as in Windows. */
    if (button == BTN_RIGHT && !pressed && m->hot_left >= 0 &&
        m->rows[m->hot_left].kind == ROW_APP) {
        m->context_row = m->hot_left;
        open_sub(m, OWNER_CONTEXT);
        return;
    }
    if (button != BTN_LEFT)
        return;
    int target = m->hot_left >= 0 ? m->hot_left : (m->hot_right >= 0 ? 1000 + m->hot_right : -1);
    if (pressed) {
        m->pressed = target;
        /* ▸ items open on press, like menus. */
        if (opens_on_press(m, m->hot_right))
            activate_right(m, m->hot_right);
        return;
    }
    int was = m->pressed;
    m->pressed = -1;
    if (target != was || target < 0)
        return;
    if (target >= 1000) {
        int hit = target - 1000;
        if (!opens_on_press(m, hit) && !(hit < R_COUNT && right_items[hit].disabled))
            activate_right(m, hit); /* may free m */
    } else {
        activate_left(m, target); /* may free m */
    }
}

void xd_start7_pointer_axis(xd_shell *shell, double value) {
    xd_start7 *m = shell->start7;
    if (m == nullptr || m->mode == MODE_HOME)
        return;
    int n = visible_rows(m, nullptr);
    int max_scroll = XD_MAX((int)m->row_count - n, 0);
    m->scroll = XD_CLAMP(m->scroll + (value > 0 ? 3 : -3), 0, max_scroll);
    m->hot_left = -1;
    m->dirty = true;
}

static int step_selectable(const xd_start7 *m, int from, int dir) {
    int n = (int)m->row_count;
    for (int i = from + dir; i >= 0 && i < n; i += dir) {
        if (m->rows[i].kind == ROW_APP || m->rows[i].kind == ROW_FOLDER)
            return i;
    }
    return from;
}

static void ensure_visible(xd_start7 *m, int index) {
    if (index < m->scroll)
        m->scroll = index;
    while (index >= m->scroll + visible_rows(m, nullptr) && m->scroll < index)
        m->scroll++;
}

static void search_changed(xd_start7 *m) {
    bool has = xd_textedit_view(&m->search).len > 0;
    m->mode = has ? MODE_SEARCH : MODE_HOME;
    rebuild(m);
    /* Enter launches the best match. */
    if (has)
        m->hot_left = step_selectable(m, -1, 1);
}

/* ---- keyboard ---------------------------------------------------------------------- */

/* The keyboard moves through columns: the program list with All Programs
 * under it, the right column (Windows 7: with the Shut Down button), and
 * XP's footer (Log Off, Turn Off Computer). Up and Down stay in a column
 * and wrap, Left and Right go across, Tab goes round them all. */
typedef enum kcol : uint8_t { KCOL_NONE, KCOL_LEFT, KCOL_RIGHT, KCOL_FOOT } kcol;

/* A stop in the left column: a row index, or STOP_ALL for All Programs. */
static constexpr int STOP_ALL = -2;

static size_t left_stops(const xd_start7 *m, int *out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < m->row_count && n < max; i++) {
        if (m->rows[i].kind == ROW_APP || m->rows[i].kind == ROW_FOLDER)
            out[n++] = (int)i;
    }
    if (m->mode != MODE_SEARCH && n < max)
        out[n++] = STOP_ALL;
    return n;
}

/* Hit codes of the right column or the footer, in order. */
static size_t right_stops(const xd_start7 *m, kcol col, int *out) {
    size_t n = 0;
    if (m->xp) {
        if (col == KCOL_FOOT) {
            out[n++] = HIT_LOGOFF;
            out[n++] = HIT_TURNOFF;
            return n;
        }
        for (size_t i = 0; i < XD_ARRAY_LEN(xp_items); i++) {
            if (xp_items[i].hit != R_SEP && !xp_items[i].disabled)
                out[n++] = xp_items[i].hit;
        }
        return n;
    }
    if (col == KCOL_FOOT)
        return 0;
    for (int k = 0; k < R_COUNT; k++) {
        if (k != R_SEP && !right_items[k].disabled)
            out[n++] = k;
    }
    out[n++] = HIT_SHUTDOWN;
    out[n++] = HIT_SHUTDOWN_ARROW;
    return n;
}

static constexpr size_t MAX_RIGHT_STOPS = 32;

static kcol key_column(const xd_start7 *m) {
    if (m->hot_left >= 0 || m->hot_right == HIT_ALL)
        return KCOL_LEFT;
    if (m->hot_right < 0)
        return KCOL_NONE;
    if (m->xp && (m->hot_right == HIT_LOGOFF || m->hot_right == HIT_TURNOFF))
        return KCOL_FOOT;
    return KCOL_RIGHT;
}

static void set_hot(xd_start7 *m, int left, int right) {
    m->hot_left = left;
    m->hot_right = right;
    if (left >= 0)
        ensure_visible(m, left);
    m->dirty = true;
}

static void set_stop(xd_start7 *m, int stop) {
    if (stop == STOP_ALL)
        set_hot(m, -1, HIT_ALL);
    else
        set_hot(m, stop, -1);
}

/* Moves into column `col` at its first (dir > 0) or last item. */
static void enter_column(xd_start7 *m, kcol col, int dir) {
    if (col == KCOL_LEFT) {
        size_t cap = m->row_count + 1;
        int *stops = xd_alloc_array(cap, sizeof(int));
        size_t n = left_stops(m, stops, cap);
        if (n > 0)
            set_stop(m, stops[dir > 0 ? 0 : n - 1]);
        xd_free(stops);
        return;
    }
    int stops[MAX_RIGHT_STOPS];
    size_t n = right_stops(m, col, stops);
    if (n > 0)
        set_hot(m, -1, stops[dir > 0 ? 0 : n - 1]);
}

/* Up and Down: the next item in the same column, wrapping round. */
static void step_column(xd_start7 *m, int dir) {
    kcol col = key_column(m);
    if (col == KCOL_NONE) {
        enter_column(m, KCOL_LEFT, dir);
        return;
    }
    if (col == KCOL_LEFT) {
        size_t cap = m->row_count + 1;
        int *stops = xd_alloc_array(cap, sizeof(int));
        size_t n = left_stops(m, stops, cap);
        int now = m->hot_left >= 0 ? m->hot_left : STOP_ALL;
        size_t at = 0;
        while (at < n && stops[at] != now)
            at++;
        if (n > 0)
            set_stop(m, stops[at == n ? 0 : (at + n + (size_t)(dir > 0 ? 1 : n - 1)) % n]);
        xd_free(stops);
        return;
    }
    int stops[MAX_RIGHT_STOPS];
    size_t n = right_stops(m, col, stops);
    size_t at = 0;
    while (at < n && stops[at] != m->hot_right)
        at++;
    if (n > 0)
        set_hot(m, -1, stops[at == n ? 0 : (at + (size_t)(dir > 0 ? 1 : n - 1)) % n]);
}

/* Opens a ▸ item's submenu from the keyboard, on its first item. */
static void open_sub_keyboard(xd_start7 *m, int owner) {
    open_sub(m, owner);
    xd_menu *sub = m->sub;
    if (sub == nullptr || m->sub_owner != owner)
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
static bool tree_key(xd_start7 *m, bool right) {
    if (m->xp || m->mode != MODE_ALL || m->hot_left < 0)
        return false;
    const row *r = &m->rows[m->hot_left];
    if (r->kind == ROW_FOLDER && m->expanded[r->folder] != right) {
        activate_left(m, m->hot_left);
        return true;
    }
    if (!right && r->kind == ROW_APP && r->indent > 0) {
        int i = m->hot_left;
        while (i > 0 && m->rows[i].kind != ROW_FOLDER)
            i--;
        set_hot(m, i, -1);
        return true;
    }
    return false;
}

static void key_right(xd_start7 *m) {
    switch (key_column(m)) {
    case KCOL_NONE:
        enter_column(m, KCOL_RIGHT, 1);
        return;
    case KCOL_LEFT:
        if (m->xp && m->hot_right == HIT_ALL)
            open_sub_keyboard(m, HIT_ALL); /* All Programs cascades */
        else if (!tree_key(m, true))
            enter_column(m, KCOL_RIGHT, 1);
        return;
    case KCOL_RIGHT:
        if (m->hot_right == HIT_SHUTDOWN)
            set_hot(m, -1, HIT_SHUTDOWN_ARROW);
        else if (opens_sub(m, m->hot_right))
            open_sub_keyboard(m, m->hot_right);
        return;
    case KCOL_FOOT:
        if (m->hot_right == HIT_LOGOFF)
            set_hot(m, -1, HIT_TURNOFF);
        return;
    }
}

static void key_left(xd_start7 *m) {
    switch (key_column(m)) {
    case KCOL_NONE:
        enter_column(m, KCOL_LEFT, 1);
        return;
    case KCOL_LEFT:
        tree_key(m, false);
        return;
    case KCOL_RIGHT:
        if (m->hot_right == HIT_SHUTDOWN_ARROW)
            set_hot(m, -1, HIT_SHUTDOWN);
        else
            enter_column(m, KCOL_LEFT, 1);
        return;
    case KCOL_FOOT:
        if (m->hot_right == HIT_TURNOFF)
            set_hot(m, -1, HIT_LOGOFF);
        return;
    }
}

/* Tab and Shift+Tab: the next column round. */
static void key_tab(xd_start7 *m, int dir) {
    static const kcol xp_order[] = {KCOL_LEFT, KCOL_RIGHT, KCOL_FOOT};
    static const kcol w7_order[] = {KCOL_LEFT, KCOL_RIGHT};
    const kcol *order = m->xp ? xp_order : w7_order;
    size_t n = m->xp ? XD_ARRAY_LEN(xp_order) : XD_ARRAY_LEN(w7_order);
    kcol col = key_column(m);
    size_t at = 0;
    while (at < n && order[at] != col)
        at++;
    size_t next = at == n ? (dir > 0 ? 0 : n - 1) : (at + (dir > 0 ? 1 : n - 1)) % n;
    enter_column(m, order[next], 1);
}

/* Enter. May free `m`. */
static void key_activate(xd_start7 *m) {
    if (m->hot_left >= 0) {
        activate_left(m, m->hot_left);
        return;
    }
    int hit = m->hot_right;
    if (hit < 0 || (hit < R_COUNT && right_items[hit].disabled))
        return;
    if (hit == R_RECENT || hit == HIT_SHUTDOWN_ARROW || (m->xp && hit == HIT_ALL))
        open_sub_keyboard(m, hit);
    else
        activate_right(m, hit);
}

bool xd_start7_key(xd_shell *shell, uint32_t sym, const char *utf8, bool ctrl_or_alt) {
    xd_start7 *m = shell->start7;
    if (m == nullptr)
        return false;
    if (m->sub != nullptr) {
        if (xd_menu_key_chain(m->sub, sym))
            return true;
        if (sym == XKB_KEY_Left || sym == XKB_KEY_Escape) {
            close_sub(m);
            return true;
        }
    }
    switch (sym) {
    case XKB_KEY_Escape:
        if (xd_textedit_view(&m->search).len > 0) {
            xd_textedit_set(&m->search, XD_STR(""));
            search_changed(m);
        } else if (m->mode == MODE_ALL) {
            m->mode = MODE_HOME;
            rebuild(m);
        } else {
            xd_menu_close_all(shell);
        }
        return true;
    case XKB_KEY_Up:
    case XKB_KEY_Down:
        step_column(m, sym == XKB_KEY_Down ? 1 : -1);
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_Left:
        /* While searching they'd move the box's caret (ours stays at the
         * end); Tab still reaches the right column. */
        if (m->mode == MODE_SEARCH)
            return true;
        if (sym == XKB_KEY_Right)
            key_right(m);
        else
            key_left(m);
        return true;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        key_tab(m, sym == XKB_KEY_Tab ? 1 : -1);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        key_activate(m); /* may free m */
        return true;
    case XKB_KEY_BackSpace:
        if (m->xp)
            return true;
        xd_textedit_backspace(&m->search, ctrl_or_alt);
        search_changed(m);
        return true;
    default:
        break;
    }
    /* Any other typing goes to the search box, as on Windows 7 (XP has none). */
    if (!m->xp && !ctrl_or_alt && utf8 != nullptr && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7F) {
        xd_textedit_move(&m->search, XD_MOTION_END, false);
        xd_textedit_insert(&m->search, xd_str_from_cstr(utf8));
        search_changed(m);
        return true;
    }
    return false;
}
