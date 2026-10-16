/* SPDX-License-Identifier: MIT
 * The Windows 7 style Start menu.
 *
 * One popup with two columns. Left (white): pinned programs, frequently
 * used programs, "All Programs" (which swaps the list for a folder tree in
 * place) and a search box that takes any typing. Right (dark): the user's
 * picture overhanging the top edge, user folders, Recent Items, Control
 * Panel, Run and a Shut Down split button. ▸ items open ordinary popup
 * menus attached to this surface.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>
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
    [R_DEVICES] = {"Devices and Printers", false, true},
    [R_DEFAULTS] = {"Default Programs", false, true},
    [R_HELP] = {"Help and Support", false, true},
    [R_RUN] = {"Run...", false, false},
};

/* Hit targets on the right besides items. */
enum { HIT_SHUTDOWN = 100, HIT_SHUTDOWN_ARROW = 101, HIT_ALL = 102, HIT_SEARCH = 103 };

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
    int sub_owner;         /* right item / HIT_SHUTDOWN_ARROW that opened `sub` */
    bool pending_hover;

    cairo_surface_t *picture;
    char user[64];
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
            .right_bg = xd_rgba_alpha(xd_rgba_hex(0xC9D6EA), 0.96),
            .right_text = xd_rgba_hex(0x10265A),
            .right_dim = xd_rgba_hex(0x7A8AA8),
            .right_hot = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.55),
            .right_sep = xd_rgba_alpha(xd_rgba_hex(0x5F7598), 0.5),
            .frame = xd_rgba_hex(0x5F7598),
            .separator = xd_rgba_hex(0xD5DCE8),
        };
    case XD_STYLE_LUNA:
    case XD_STYLE_COUNT:
        break;
    }
    /* Luna: white list, dark navy see-through panel (the reference look). */
    return (s7_palette){
        .left_bg = xd_rgba_hex(0xFFFFFF),
        .left_text = xd_rgba_hex(0x000000),
        .left_dim = xd_rgba_hex(0x6D6D6D),
        .left_hot = xd_rgba_hex(0xDCEBFC),
        .left_hot_text = xd_rgba_hex(0x000000),
        .left_hot_border = xd_rgba_hex(0x7DA2CE),
        .right_bg = xd_rgba_alpha(xd_rgba_hex(0x1B3A5C), 0.94),
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
static xd_rect search_rect(void) {
    return (xd_rect){S7_PAD + 4, S7_H - S7_PAD - S7_SEARCH_H - 4, S7_LEFT_W - 2 * S7_PAD - 8,
                     S7_SEARCH_H};
}
static xd_rect all_rect(void) {
    xd_rect s = search_rect();
    return (xd_rect){S7_PAD, s.y - 8 - 28, S7_LEFT_W - 2 * S7_PAD, 28};
}
static xd_rect list_rect(void) {
    xd_rect a = all_rect();
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

static const xd_app *find_app(const xd_shell *shell, const char *id) {
    for (size_t i = 0; i < shell->apps.count; i++) {
        if (strcmp(shell->apps.items[i].id, id) == 0)
            return &shell->apps.items[i];
    }
    return nullptr;
}

/* Pinned ids: ~/.config/xeondesktop/start-pinned, one desktop id per line.
 * Without the file: the browser and the terminal, like Quick Launch. */
static void load_pinned(const xd_shell *shell, const xd_app **out, size_t *n, size_t max) {
    *n = 0;
    xd_strbuf path = {0};
    xd_strbuf text = {0};
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (cfg != nullptr && cfg[0] == '/')
        xd_strbuf_appendf(&path, "%s/xeondesktop/start-pinned", cfg);
    else
        xd_strbuf_appendf(&path, "%s/.config/xeondesktop/start-pinned", home != nullptr ? home : "");
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
    const xd_app *pinned[16];
    size_t np = 0;
    load_pinned(shell, pinned, &np, XD_ARRAY_LEN(pinned));
    for (size_t i = 0; i < np; i++)
        push_row(m, (row){.kind = ROW_APP, .app = pinned[i]});

    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines("start-frequent", &lines, &n, 64);
    bool sep = false;
    size_t shown = 0;
    for (size_t i = 0; i < n && shown < S7_FREQUENT; i++) {
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

static bool matches(const xd_app *app, xd_str query) {
    xd_str name = xd_str_from_cstr(app->name);
    if (query.len == 0 || query.len > name.len)
        return query.len == 0;
    for (size_t i = 0; i + query.len <= name.len; i++) {
        if (xd_str_eq_nocase(xd_str_sub(name, i, query.len), query))
            return true;
    }
    return false;
}

static void build_search(xd_start7 *m) {
    xd_shell *shell = m->shell;
    m->row_count = 0;
    m->scroll = 0;
    xd_str q = xd_str_trim(xd_textedit_view(&m->search));
    push_row(m, (row){.kind = ROW_HEADER, .label = "Programs"});
    /* Names starting with the query first, then other matches. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < shell->apps.count; i++) {
            const xd_app *a = &shell->apps.items[i];
            bool prefix = xd_str_starts_with(xd_str_from_cstr(a->name), q) ||
                          (q.len <= strlen(a->name) &&
                           xd_str_eq_nocase(xd_str_sub(xd_str_from_cstr(a->name), 0, q.len), q));
            if (matches(a, q) && prefix == (pass == 0))
                push_row(m, (row){.kind = ROW_APP, .app = a});
        }
    }
    if (m->row_count == 1)
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
    xd_rect lr = list_rect();
    int y = 0;
    int n = 0;
    for (size_t i = (size_t)m->scroll; i < m->row_count; i++) {
        int h = row_height(m->mode, &m->rows[i]);
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
    xd_rect lr = list_rect();
    if (!contains(lr, x, y))
        return -1;
    double top = lr.y;
    int n = visible_rows(m, nullptr);
    for (int k = 0; k < n; k++) {
        size_t i = (size_t)(m->scroll + k);
        int h = row_height(m->mode, &m->rows[i]);
        if (y >= top && y < top + h)
            return (m->rows[i].kind == ROW_APP || m->rows[i].kind == ROW_FOLDER) ? (int)i : -1;
        top += h;
    }
    return -1;
}

static int right_at(double x, double y) {
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
    xd_rect lr = list_rect();
    int y = lr.y;
    int n = visible_rows(m, nullptr);
    for (int k = 0; k < n; k++) {
        size_t i = (size_t)(m->scroll + k);
        const row *r = &m->rows[i];
        int h = row_height(m->mode, r);
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
    xd_rect ar = all_rect();
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

static void render(xd_start7 *m) {
    xd_shell *shell = m->shell;
    double s = m->scale;
    int dw = dev(S7_W, s);
    int dh = dev(S7_H, s);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&m->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, s);
    s7_palette p = palette(&t);
    cairo_t *cr = cairo_create(buf->surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR); /* the band above is transparent */
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

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
        xd_fill_rect(cr, D((xd_rect){0, S7_TOP, S7_PAD, S7_H - S7_TOP}, s), p.right_bg);
        xd_fill_rect(cr, D((xd_rect){0, S7_TOP, S7_LEFT_W, S7_PAD}, s), p.right_bg);
        xd_fill_rect(cr, D((xd_rect){0, S7_H - S7_PAD, S7_LEFT_W, S7_PAD}, s), p.right_bg);
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

    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(m->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(m->surface, 0, 0, dw, dh);
    if (m->viewport != nullptr)
        wp_viewport_set_destination(m->viewport, S7_W, S7_H);
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

static void open_sub(xd_start7 *m, int owner) {
    if (m->sub_owner == owner && m->sub != nullptr)
        return;
    close_sub(m);
    if (!m->shown)
        return;
    xd_shell *shell = m->shell;
    xd_menu_model *mm = nullptr;
    xd_rect anchor;
    if (owner == R_RECENT) {
        mm = recent_model(m);
        anchor = right_item(R_RECENT);
    } else if (owner == R_CONTROL) {
        mm = xd_menu_model_new(&m->tree);
        add_cmd(&m->tree, mm, "Display", "preferences-desktop-display",
                xd_shell_own_command(shell, "xd-desk", nullptr));
        add_cmd(&m->tree, mm, "Taskbar and Start Menu", "preferences-desktop",
                xd_shell_own_command(shell, "xd-taskbar", nullptr));
        anchor = right_item(R_CONTROL);
    } else if (owner == HIT_SHUTDOWN_ARROW) {
        mm = xd_menu_model_new(&m->tree);
        add_cmd(&m->tree, mm, "Switch user", "system-switch-user", nullptr);
        add_cmd(&m->tree, mm, "Log off", "system-log-out",
                xd_shell_own_command(shell, "xd-shutdown", "--action logoff"));
        add_cmd(&m->tree, mm, "Lock", "system-lock-screen", nullptr);
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
    xd_shell *shell = m->shell;
    char *id = xd_str_dup_cstr(xd_str_from_cstr(app->id));
    xd_strbuf cmd = {0};
    if (app->terminal)
        xd_strbuf_appendf(&cmd, "%s -e %s", shell->terminal, app->exec);
    else
        xd_strbuf_append(&cmd, xd_str_from_cstr(app->exec));
    xd_menu_close_all(shell); /* frees m */
    xd_shell_launch(shell, xd_strbuf_cstr(&cmd));
    xd_start7_count_launch(id);
    xd_strbuf_free(&cmd);
    xd_free(id);
}

static void run_command(xd_start7 *m, const char *command) {
    xd_shell *shell = m->shell;
    char *cmd = xd_str_dup_cstr(xd_str_from_cstr(command));
    xd_menu_close_all(shell); /* frees m */
    xd_shell_launch(shell, cmd);
    xd_free(cmd);
}

static const char *folder_command(const char *xdg_name, const char *fallback) {
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "xdg-open \"$(xdg-user-dir %s 2>/dev/null || echo \"$HOME/%s\")\"", xdg_name,
             fallback);
    return buf;
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
        run_command(m, "xdg-open \"$HOME\"");
        return;
    case R_DOCUMENTS:
        run_command(m, folder_command("DOCUMENTS", "Documents"));
        return;
    case R_PICTURES:
        run_command(m, folder_command("PICTURES", "Pictures"));
        return;
    case R_MUSIC:
        run_command(m, folder_command("MUSIC", "Music"));
        return;
    case R_COMPUTER:
        run_command(m, "xdg-open /");
        return;
    case R_RUN:
        run_command(m, xd_shell_own_command(shell, "xd-run", nullptr));
        return;
    case R_RECENT:
    case R_CONTROL:
    case HIT_SHUTDOWN_ARROW:
        open_sub(m, hit);
        return;
    case HIT_SHUTDOWN:
        run_command(m, xd_shell_own_command(shell, "xd-shutdown", "--action shutdown"));
        return;
    case HIT_ALL:
        m->mode = m->mode == MODE_ALL ? MODE_HOME : MODE_ALL;
        rebuild(m);
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

static void load_user(xd_start7 *m) {
    const char *user = getenv("USER");
    snprintf(m->user, sizeof(m->user), "%s", user != nullptr ? user : "User");
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

void xd_start7_open(xd_taskbar *bar, uint32_t serial) {
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
    xdg_positioner_set_size(pos, S7_W, S7_H);
    xdg_positioner_set_anchor_rect(pos, (int)(r.x / bar->scale), (int)(r.y / bar->scale),
                                   XD_MAX((int)(r.w / bar->scale), 1),
                                   XD_MAX((int)(r.h / bar->scale), 1));
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos,
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
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
    if (h == R_RECENT || h == R_CONTROL || h == HIT_SHUTDOWN_ARROW)
        open_sub(m, h);
    else if (h >= 0 && m->sub != nullptr)
        close_sub(m);
}

void xd_start7_pointer_motion(xd_shell *shell, double x, double y) {
    xd_start7 *m = shell->start7;
    if (m == nullptr)
        return;
    int left = row_at(m, x, y);
    int right = right_at(x, y);
    if (contains(all_rect(), x, y) && m->mode != MODE_SEARCH)
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
    if (m == nullptr || button != BTN_LEFT)
        return;
    int target = m->hot_left >= 0 ? m->hot_left : (m->hot_right >= 0 ? 1000 + m->hot_right : -1);
    if (pressed) {
        m->pressed = target;
        /* ▸ items open on press, like menus. */
        if (m->hot_right == R_RECENT || m->hot_right == R_CONTROL ||
            m->hot_right == HIT_SHUTDOWN_ARROW)
            activate_right(m, m->hot_right);
        return;
    }
    int was = m->pressed;
    m->pressed = -1;
    if (target != was || target < 0)
        return;
    if (target >= 1000) {
        int hit = target - 1000;
        if (hit != R_RECENT && hit != R_CONTROL && hit != HIT_SHUTDOWN_ARROW &&
            !(hit < R_COUNT && right_items[hit].disabled))
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
    case XKB_KEY_Down: {
        int dir = sym == XKB_KEY_Down ? 1 : -1;
        int from = m->hot_left >= 0 ? m->hot_left : (dir > 0 ? -1 : (int)m->row_count);
        m->hot_left = step_selectable(m, from, dir);
        if (m->hot_left >= (int)m->row_count)
            m->hot_left = -1;
        if (m->hot_left >= 0)
            ensure_visible(m, m->hot_left);
        m->dirty = true;
        return true;
    }
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (m->hot_left >= 0)
            activate_left(m, m->hot_left); /* may free m */
        return true;
    case XKB_KEY_BackSpace:
        xd_textedit_backspace(&m->search, ctrl_or_alt);
        search_changed(m);
        return true;
    default:
        break;
    }
    /* Any other typing goes to the search box, as on Windows 7. */
    if (!ctrl_or_alt && utf8 != nullptr && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7F) {
        xd_textedit_move(&m->search, XD_MOTION_END, false);
        xd_textedit_insert(&m->search, xd_str_from_cstr(utf8));
        search_changed(m);
        return true;
    }
    return false;
}
