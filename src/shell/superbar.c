/* SPDX-License-Identifier: MIT
 * Windows 7's taskbar (Aero): one row of buttons for programs, pinned
 * ones staying when they close, running ones framed in glass with their
 * windows stacked behind. "Always combine, hide labels" gives each program
 * one icon; "Combine when taskbar is full" and "Never combine" give each
 * window a labelled button where its program's pin was.
 *
 * Pinned programs are ~/.config/xeondesktop/taskbar-pinned, one desktop
 * file id per line (as the Start menu's start-pinned); running programs
 * that aren't pinned follow them in the order they started, or were
 * dragged into, for the session.
 *
 * Right-clicking a button opens its jump list, in Windows 7's own look:
 * the files pinned to it (~/.config/xeondesktop/jumplist-pinned, a group
 * per program), its recent files (recently-used.xbel), its desktop
 * actions as Tasks, then the band with the program, Pin/Unpin and Close
 * window. A hovered file has a pin button; dropping a file on a button
 * pins it there, and dropping a program on the bar pins the program.
 *
 * Hovering a running program's button shows its windows' thumbnails
 * (thumbs.c).
 */
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <sys/stat.h>

#include "../core/alloc.h"
#include "jumplist.h"
#include "recent.h"
#include "shell.h"

/* Logical px. Windows 7's large-icon buttons: a 32px icon on a button
 * about 60px wide; labelled ones as wide as XP's. */
enum {
    APP_W = 58,
    APP_ICON = 32,
    LABEL_ICON = 24,
    LABEL_MAX_W = 160,
    LABEL_MIN_W = 64,
    LABEL_COMBINE_W = 100, /* "when full": combine below this */
    GAP = 2,
    RECENT_MAX = 10, /* Windows 7's "Number of recent items to display in Jump Lists" */
};

/* Logical px the pointer moves with a button held before it drags. */
static constexpr double DRAG_SLOP = 5;

struct xd_superbar {
    GPtrArray *pins;       /* desktop ids, in order (owned) */
    GPtrArray *order;      /* running programs that aren't pinned, in order (owned keys) */
    GHashTable *infos;     /* desktop id -> GDesktopAppInfo, or nullptr: not installed */
    bool pins_changed;     /* by a drag, not saved yet */
    GBookmarkFile *recent; /* recently-used.xbel as last read */
    gint64 recent_stamp;   /* its mtime (ns) and size then */
    gint64 recent_size;
    GKeyFile *jl_pins;     /* jumplist-pinned: files pinned to programs' jump lists */
    /* The open jump list's button, to build it again after a pin. */
    char *jl_key;
    uint64_t jl_window; /* a window's own button: its id; else 0 */
};

/* ---- programs and pins --------------------------------------------------------- */

static void unref_info(gpointer p) {
    if (p != nullptr)
        g_object_unref(p);
}

static GDesktopAppInfo *info_for(xd_superbar *sb, const char *id) {
    gpointer v = nullptr;
    if (g_hash_table_lookup_extended(sb->infos, id, nullptr, &v))
        return v;
    /* A launcher pinned from elsewhere (the desktop) is kept by its path. */
    GDesktopAppInfo *info = g_path_is_absolute(id) ? g_desktop_app_info_new_from_filename(id)
                                                   : g_desktop_app_info_new(id);
    g_hash_table_insert(sb->infos, g_strdup(id), info);
    return info;
}

/* A window's program: its desktop file, else its app id, else itself. */
static bool is_desktop_key(const char *key) {
    return !g_str_has_prefix(key, "app:") && !g_str_has_prefix(key, "win:");
}

static char *window_key(const xd_toplevel *tl) {
    if (tl->desktop_id != nullptr)
        return g_strdup(tl->desktop_id);
    if (tl->app_id != nullptr && tl->app_id[0] != '\0')
        return g_strconcat("app:", tl->app_id, nullptr);
    return g_strdup_printf("win:%" PRIu64, tl->id);
}

static bool window_is(const xd_toplevel *tl, const char *key) {
    char *k = window_key(tl);
    bool same = strcmp(k, key) == 0;
    g_free(k);
    return same;
}

static char *pins_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "taskbar-pinned", nullptr);
}

static bool pin_index(const xd_superbar *sb, const char *id, guint *at) {
    for (guint i = 0; i < sb->pins->len; i++) {
        if (strcmp(g_ptr_array_index(sb->pins, i), id) == 0) {
            if (at != nullptr)
                *at = i;
            return true;
        }
    }
    return false;
}

/* Without the file, Windows 7's three: the browser, the file manager and
 * the media player, each if installed. */
static void default_pins(xd_superbar *sb) {
    static const char *const browsers[] = {"firefox.desktop", "org.mozilla.firefox.desktop",
                                           "chromium.desktop", "google-chrome.desktop", nullptr};
    static const char *const files[] = {"org.xeondesktop.Files.desktop", nullptr};
    static const char *const players[] = {"vlc.desktop",
                                          "org.videolan.VLC.desktop",
                                          "mpv.desktop",
                                          "io.github.celluloid_player.Celluloid.desktop",
                                          "org.gnome.Totem.desktop",
                                          "org.kde.dragonplayer.desktop",
                                          nullptr};
    const char *const *groups[] = {browsers, files, players};
    for (size_t g = 0; g < XD_ARRAY_LEN(groups); g++) {
        for (const char *const *id = groups[g]; *id != nullptr; id++) {
            if (info_for(sb, *id) != nullptr) {
                g_ptr_array_add(sb->pins, g_strdup(*id));
                break;
            }
        }
    }
}

static void load_pins(xd_superbar *sb) {
    g_ptr_array_set_size(sb->pins, 0);
    char *path = pins_path();
    char *text = nullptr;
    if (g_file_get_contents(path, &text, nullptr, nullptr)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l != nullptr; l++) {
            g_strstrip(*l);
            if ((*l)[0] != '\0' && !pin_index(sb, *l, nullptr))
                g_ptr_array_add(sb->pins, g_strdup(*l));
        }
        g_strfreev(lines);
        g_free(text);
    } else {
        default_pins(sb);
    }
    g_free(path);
}

static char *jl_pins_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "jumplist-pinned", nullptr);
}

static void save_jl_pins(xd_superbar *sb) {
    char *path = jl_pins_path();
    char *dir = g_path_get_dirname(path);
    GError *error = nullptr;
    if (g_mkdir_with_parents(dir, 0700) != 0 ||
        !g_key_file_save_to_file(sb->jl_pins, path, &error)) {
        fprintf(stderr, "xd-shell: cannot save %s: %s\n", path,
                error != nullptr ? error->message : g_strerror(errno));
        g_clear_error(&error);
    }
    g_free(dir);
    g_free(path);
}

static void save_pins(xd_superbar *sb) {
    sb->pins_changed = false;
    GString *text = g_string_new(nullptr);
    for (guint i = 0; i < sb->pins->len; i++)
        g_string_append_printf(text, "%s\n", (const char *)g_ptr_array_index(sb->pins, i));
    char *path = pins_path();
    char *dir = g_path_get_dirname(path);
    GError *error = nullptr;
    if (g_mkdir_with_parents(dir, 0700) != 0 ||
        !g_file_set_contents(path, text->str, (gssize)text->len, &error)) {
        fprintf(stderr, "xd-shell: cannot save %s: %s\n", path,
                error != nullptr ? error->message : g_strerror(errno));
        g_clear_error(&error);
    }
    g_free(dir);
    g_free(path);
    g_string_free(text, TRUE);
}

void xd_superbar_init(xd_shell *shell) {
    xd_superbar *sb = XD_NEW(xd_superbar);
    sb->pins = g_ptr_array_new_with_free_func(g_free);
    sb->order = g_ptr_array_new_with_free_func(g_free);
    sb->infos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, unref_info);
    sb->recent_stamp = -1;
    sb->jl_pins = g_key_file_new();
    char *jl = jl_pins_path();
    g_key_file_load_from_file(sb->jl_pins, jl, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_free(jl);
    shell->superbar = sb;
    load_pins(sb);
}

void xd_superbar_finish(xd_shell *shell) {
    xd_superbar *sb = shell->superbar;
    if (sb == nullptr)
        return;
    if (sb->pins_changed)
        save_pins(sb);
    g_ptr_array_free(sb->pins, TRUE);
    g_ptr_array_free(sb->order, TRUE);
    g_hash_table_destroy(sb->infos);
    if (sb->recent != nullptr)
        g_bookmark_file_free(sb->recent);
    g_key_file_free(sb->jl_pins);
    g_free(sb->jl_key);
    xd_free(sb);
    shell->superbar = nullptr;
}

bool xd_superbar_pinned(xd_shell *shell, const char *desktop_id) {
    return shell->superbar != nullptr && pin_index(shell->superbar, desktop_id, nullptr);
}

void xd_superbar_set_pinned(xd_shell *shell, const char *desktop_id, bool pin) {
    xd_superbar *sb = shell->superbar;
    if (sb == nullptr)
        return;
    guint at = 0;
    bool present = pin_index(sb, desktop_id, &at);
    if (pin == present)
        return;
    if (pin) {
        /* A running program keeps its place: it's pinned where it is, at
         * the end of the pinned ones. */
        g_ptr_array_add(sb->pins, g_strdup(desktop_id));
    } else {
        g_ptr_array_remove_index(sb->pins, at);
    }
    save_pins(sb);
    xd_taskbars_forget_layout(shell);
}

/* The recently used list, read again when it changes. */
static GBookmarkFile *recent_file(xd_superbar *sb) {
    char *path = g_build_filename(g_get_user_data_dir(), "recently-used.xbel", nullptr);
    GStatBuf st;
    gint64 stamp = -1, size = -1;
    if (g_stat(path, &st) == 0) {
        stamp = (gint64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
        size = (gint64)st.st_size;
    }
    if (stamp != sb->recent_stamp || size != sb->recent_size) {
        if (sb->recent != nullptr)
            g_bookmark_file_free(sb->recent);
        sb->recent = g_bookmark_file_new();
        if (stamp < 0 || !g_bookmark_file_load_from_file(sb->recent, path, nullptr)) {
            g_bookmark_file_free(sb->recent);
            sb->recent = nullptr;
        }
        sb->recent_stamp = stamp;
        sb->recent_size = size;
    }
    g_free(path);
    return sb->recent;
}

/* Takes out of `uris` what Privacy says not to remember (any more). The
 * file itself is left alone: GTK clamps it the next time it writes. */
static void drop_forgotten(GBookmarkFile *recent, char **uris) {
    bool remember;
    int max_age;
    xd_recent_privacy(&remember, &max_age);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    size_t k = 0;
    for (size_t i = 0; uris[i] != nullptr; i++) {
        GDateTime *dt = recent != nullptr ? g_bookmark_file_get_modified_date_time(
                                                recent, uris[i], nullptr)
                                          : nullptr;
        if (xd_recent_shown(remember, max_age, dt != nullptr ? g_date_time_to_unix(dt) : 0, now))
            uris[k++] = uris[i];
        else
            g_free(uris[i]);
    }
    uris[k] = nullptr;
}

/* A program on the taskbar this time round. */
typedef struct program {
    char *key;
    GDesktopAppInfo *info; /* the cache's; nullptr if not installed */
    bool pinned;
    GPtrArray *windows; /* xd_toplevel *, in the order they opened */
    char *icon;         /* the desktop file's Icon (g_free) */
    bool combined;      /* "when full": one button for its windows */
} program;

typedef struct programs {
    program *items;
    size_t count, cap;
} programs;

static program *add_program(programs *ps, char *key, GDesktopAppInfo *info, bool pinned) {
    if (ps->count == ps->cap) {
        ps->cap = ps->cap == 0 ? 16 : ps->cap * 2;
        ps->items = xd_realloc_array(ps->items, ps->cap, sizeof(*ps->items));
    }
    program *p = &ps->items[ps->count++];
    *p = (program){
        .key = key,
        .info = info,
        .pinned = pinned,
        .windows = g_ptr_array_new(),
        .icon = info != nullptr ? g_desktop_app_info_get_string(info, "Icon") : nullptr,
    };
    return p;
}

static void programs_free(programs *ps) {
    for (size_t i = 0; i < ps->count; i++) {
        g_free(ps->items[i].key);
        g_free(ps->items[i].icon);
        g_ptr_array_free(ps->items[i].windows, TRUE);
    }
    xd_free(ps->items);
    *ps = (programs){0};
}

static guint order_of(const xd_superbar *sb, const char *key) {
    for (guint i = 0; i < sb->order->len; i++) {
        if (strcmp(g_ptr_array_index(sb->order, i), key) == 0)
            return i;
    }
    return G_MAXUINT;
}

static int by_order(const void *a, const void *b, void *data) {
    const xd_superbar *sb = data;
    guint x = order_of(sb, ((const program *)a)->key);
    guint y = order_of(sb, ((const program *)b)->key);
    return (x > y) - (x < y);
}

/* Pinned programs (installed ones), then the others that are running. */
static void programs_build(xd_shell *shell, programs *ps) {
    xd_superbar *sb = shell->superbar;
    *ps = (programs){0};
    for (guint i = 0; i < sb->pins->len; i++) {
        const char *id = g_ptr_array_index(sb->pins, i);
        GDesktopAppInfo *info = info_for(sb, id);
        if (info != nullptr)
            add_program(ps, g_strdup(id), info, true);
    }
    size_t pinned = ps->count;
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        char *key = window_key(tl);
        program *p = nullptr;
        for (size_t i = 0; i < ps->count && p == nullptr; i++) {
            if (strcmp(ps->items[i].key, key) == 0)
                p = &ps->items[i];
        }
        if (p == nullptr) {
            GDesktopAppInfo *info = is_desktop_key(key) ? info_for(sb, key) : nullptr;
            p = add_program(ps, key, info, false);
            if (order_of(sb, key) == G_MAXUINT)
                g_ptr_array_add(sb->order, g_strdup(key));
        } else {
            g_free(key);
        }
        g_ptr_array_add(p->windows, tl);
    }
    /* Forget closed programs: opened again, they go at the end. */
    for (guint i = sb->order->len; i-- > 0;) {
        const char *key = g_ptr_array_index(sb->order, i);
        bool running = false;
        for (size_t k = pinned; k < ps->count && !running; k++)
            running = strcmp(ps->items[k].key, key) == 0;
        if (!running)
            g_ptr_array_remove_index(sb->order, i);
    }
    if (ps->count > pinned + 1)
        g_sort_array(ps->items + pinned, ps->count - pinned, sizeof(*ps->items), by_order, sb);
}

static const char *program_name(const program *p) {
    if (p->info != nullptr)
        return g_app_info_get_name(G_APP_INFO(p->info));
    const xd_toplevel *tl = p->windows->len > 0 ? g_ptr_array_index(p->windows, 0) : nullptr;
    if (tl == nullptr)
        return "";
    if (tl->app_name != nullptr)
        return tl->app_name;
    if (tl->title != nullptr && tl->title[0] != '\0')
        return tl->title;
    return tl->app_id != nullptr ? tl->app_id : "";
}

static const char *window_title(const xd_toplevel *tl) {
    if (tl->title != nullptr && tl->title[0] != '\0')
        return tl->title;
    return tl->app_id != nullptr ? tl->app_id : "";
}

/* ---- layout and drawing ------------------------------------------------------------ */

/* One button: a program's (icon, or labelled when combined with labels),
 * or one window's labelled one. */
typedef struct item {
    size_t prog;
    xd_toplevel *window; /* a window's own button */
    bool labelled;
    int w;
    size_t page;
    xd_rect r;
} item;

static size_t make_items(const programs *ps, item *items, xd_taskbar_buttons mode) {
    size_t n = 0;
    for (size_t i = 0; i < ps->count; i++) {
        const program *p = &ps->items[i];
        if (mode == XD_BUTTONS_ALWAYS_COMBINE || p->windows->len == 0) {
            items[n++] = (item){.prog = i};
        } else if (p->combined) {
            items[n++] = (item){.prog = i, .labelled = true};
        } else {
            for (guint k = 0; k < p->windows->len; k++)
                items[n++] = (item){.prog = i, .window = g_ptr_array_index(p->windows, k),
                                    .labelled = true};
        }
    }
    return n;
}

/* Widths: icons are fixed; labelled buttons share what's left of `rows`
 * rows, between their minimum and maximum. Returns the shared width
 * before clamping to the minimum (to tell when to combine). */
static int size_items(item *items, size_t n, const xd_theme *t, int avail, int rows, bool side) {
    int gap = xd_tpx(t, GAP);
    int icon_w = xd_tpx(t, APP_W);
    if (side) {
        for (size_t i = 0; i < n; i++)
            items[i].w = avail;
        return avail;
    }
    int fixed = 0;
    size_t labelled = 0;
    for (size_t i = 0; i < n; i++) {
        if (items[i].labelled)
            labelled++;
        else
            fixed += icon_w + gap;
    }
    int lw = xd_tpx(t, LABEL_MAX_W);
    if (labelled > 0)
        lw = XD_MIN(lw, (rows * avail - fixed) / (int)labelled - gap);
    int shared = lw;
    lw = XD_MAX(lw, xd_tpx(t, LABEL_MIN_W));
    for (size_t i = 0; i < n; i++)
        items[i].w = items[i].labelled ? XD_MIN(lw, avail) : XD_MIN(icon_w, avail);
    return shared;
}

/* Places the buttons in rows (a column on side taskbars), page after
 * page. Returns the number of pages. */
static size_t flow(item *items, size_t n, const xd_theme *t, xd_rect band, int avail, int rows,
                   int row_h, bool side, size_t per_page_side) {
    int gap = xd_tpx(t, GAP);
    int inset = xd_tpx(t, 1);
    if (side) {
        int th = row_h - 2 * inset;
        for (size_t i = 0; i < n; i++) {
            size_t k = i % per_page_side;
            items[i].page = i / per_page_side;
            items[i].r = (xd_rect){band.x, band.y + (int)k * (th + gap), avail, th};
        }
        return n == 0 ? 1 : (n + per_page_side - 1) / per_page_side;
    }
    int x = band.x;
    int row = 0;
    size_t page = 0;
    for (size_t i = 0; i < n; i++) {
        if (x > band.x && x + items[i].w > band.x + avail) {
            x = band.x;
            if (++row >= rows) {
                row = 0;
                page++;
            }
        }
        items[i].page = page;
        items[i].r = (xd_rect){x, band.y + row * row_h + inset, items[i].w, row_h - 2 * inset};
        x += items[i].w + gap;
    }
    return page + 1;
}

/* The icon's main colour, for the hover glow: its pixels' average,
 * weighted to the most colourful, brightened. */
static xd_rgba icon_tint(cairo_surface_t *icon) {
    xd_rgba fallback = {0.45, 0.75, 1.0, 1};
    if (icon == nullptr || cairo_surface_get_type(icon) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_image_surface_get_format(icon) != CAIRO_FORMAT_ARGB32)
        return fallback;
    cairo_surface_flush(icon);
    const unsigned char *data = cairo_image_surface_get_data(icon);
    int w = cairo_image_surface_get_width(icon);
    int h = cairo_image_surface_get_height(icon);
    int stride = cairo_image_surface_get_stride(icon);
    double sr = 0, sg = 0, sb = 0, sw = 0;
    for (int y = 0; data != nullptr && y < h; y++) {
        const uint32_t *row = (const uint32_t *)(const void *)(data + (ptrdiff_t)y * stride);
        for (int x = 0; x < w; x++) {
            uint32_t px = row[x];
            double a = (px >> 24) / 255.0;
            if (a < 0.5)
                continue;
            double r = ((px >> 16) & 0xFF) / 255.0 / a;
            double g = ((px >> 8) & 0xFF) / 255.0 / a;
            double b = (px & 0xFF) / 255.0 / a;
            double sat = fmax(r, fmax(g, b)) - fmin(r, fmin(g, b));
            double weight = sat * sat + 0.01;
            sr += r * weight;
            sg += g * weight;
            sb += b * weight;
            sw += weight;
        }
    }
    if (sw <= 0)
        return fallback;
    double r = sr / sw, g = sg / sw, b = sb / sw;
    double top = fmax(r, fmax(g, b));
    if (top <= 0.05)
        return fallback;
    return (xd_rgba){r / top, g / top, b / top, 1};
}

static bool window_lit(const xd_toplevel *tl) { return tl->activated && !tl->minimized; }
static bool window_flashing(const xd_toplevel *tl) {
    return tl->attention && tl->flashes % 2 == 0;
}

static void draw_item(xd_taskbar *bar, const xd_theme *t, cairo_t *cr, const programs *ps,
                      const item *it) {
    xd_shell *shell = bar->shell;
    const program *p = &ps->items[it->prog];
    size_t i = bar->task_count++;
    xd_toplevel *first = p->windows->len > 0 ? g_ptr_array_index(p->windows, 0) : nullptr;
    size_t windows = it->window != nullptr ? 1 : p->windows->len;
    bar->task_rects[i] = it->r;
    bar->task_views[i] = it->window != nullptr ? it->window : first;
    bar->task_counts[i] = windows;
    bar->bar_buttons[i] = (xd_bar_button){
        .key = g_string_chunk_insert(bar->bar_keys, p->key),
        .window = it->window,
        .windows = windows,
        .pinned = p->pinned,
    };

    uint8_t state = 0;
    if (bar->hot == (int)i)
        state |= XD_STATE_HOT;
    if (bar->pressed == (int)i && bar->hot == (int)i && bar->drag_key == nullptr)
        state |= XD_STATE_PRESSED;
    bool lit = false, flash = false;
    for (guint k = 0; k < p->windows->len; k++) {
        xd_toplevel *tl = g_ptr_array_index(p->windows, k);
        if (it->window != nullptr && tl != it->window)
            continue;
        /* The minimize animation goes to the button. */
        xd_taskbar_tell_button(bar, tl, it->r);
        lit = lit || window_lit(tl);
        flash = flash || window_flashing(tl);
    }
    if (lit)
        state |= XD_STATE_CHECKED;
    else if (flash)
        state |= XD_STATE_FLASH;

    const char *window_icon = first != nullptr ? first->icon : nullptr;
    const char *names[] = {window_icon != nullptr ? window_icon : p->icon,
                           first != nullptr ? first->app_id : nullptr,
                           "application-x-executable"};
    cairo_surface_t *icon = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names),
                                             xd_tpx(t, it->labelled ? LABEL_ICON : APP_ICON));
    xd_app_button ab = {
        .icon = icon,
        .label = !it->labelled            ? nullptr
                 : it->window != nullptr ? window_title(it->window)
                                         : program_name(p),
        .windows = (int)windows,
        .glow_x = (state & XD_STATE_HOT) ? bar->pointer_x * bar->scale : -1,
        .tint = icon_tint(icon),
    };
    xd_theme_draw_app_button(t, cr, it->r, state, &ab);
}

void xd_superbar_layout(xd_taskbar *bar, const xd_theme *t, cairo_t *cr, xd_rect band, int rows,
                        int row_h) {
    xd_shell *shell = bar->shell;
    bar->superbar = true;
    bar->task_count = 0;
    bar->pages = 1;
    bar->page_up_rect = bar->page_down_rect = (xd_rect){0};
    if (bar->bar_keys == nullptr)
        bar->bar_keys = g_string_chunk_new(256);
    g_string_chunk_clear(bar->bar_keys);
    if (shell->superbar == nullptr || band.w <= 0 || band.h <= 0)
        return;
    programs ps;
    programs_build(shell, &ps);
    size_t most = ps.count + (size_t)wl_list_length(&shell->toplevels);
    xd_taskbar_reserve(bar, most);
    item *items = xd_alloc_array(XD_MAX(most, 1), sizeof(*items));

    bool side = shell->settings.edge == XD_EDGE_LEFT || shell->settings.edge == XD_EDGE_RIGHT;
    xd_taskbar_buttons mode = shell->settings.buttons;
    int gap = xd_tpx(t, GAP);
    int arrows_w = xd_theme_dropdown_button_width(t);
    int th = row_h - 2 * xd_tpx(t, 1);
    size_t fits = (size_t)XD_MAX(1, (band.h + gap) / (th + gap));

    size_t n = make_items(&ps, items, mode);
    int shared = size_items(items, n, t, band.w, rows, side);
    size_t pages = flow(items, n, t, band, band.w, rows, row_h, side, fits);
    /* "Combine when taskbar is full": programs are combined one at a time,
     * the least recently used first, until the buttons fit. */
    if (mode == XD_BUTTONS_WHEN_FULL) {
        size_t *wins = xd_alloc_array(XD_MAX(ps.count, 1), sizeof(*wins));
        uint64_t *used = xd_alloc_array(XD_MAX(ps.count, 1), sizeof(*used));
        bool *done = xd_alloc_array(XD_MAX(ps.count, 1), sizeof(*done));
        for (size_t i = 0; i < ps.count; i++) {
            wins[i] = ps.items[i].windows->len;
            used[i] = 0;
            for (guint k = 0; k < ps.items[i].windows->len; k++)
                used[i] = XD_MAX(used[i], ((xd_toplevel *)g_ptr_array_index(ps.items[i].windows,
                                                                            k))->active_seq);
            done[i] = false;
        }
        while (pages > 1 || (!side && shared < xd_tpx(t, LABEL_COMBINE_W))) {
            size_t next = xd_jumplist_combine_next(wins, used, done, ps.count);
            if (next == SIZE_MAX)
                break;
            done[next] = ps.items[next].combined = true;
            n = make_items(&ps, items, mode);
            shared = size_items(items, n, t, band.w, rows, side);
            pages = flow(items, n, t, band, band.w, rows, row_h, side, fits);
        }
        xd_free(wins);
        xd_free(used);
        xd_free(done);
    }
    int avail = band.w;
    if (pages > 1) {
        /* Pages, with Windows 7's up and down arrows at the end. */
        if (side) {
            size_t per_page = fits > 1 ? fits - 1 : 1;
            pages = flow(items, n, t, band, band.w, rows, row_h, side, per_page);
            int ay = band.y + (int)per_page * (th + gap);
            bar->page_up_rect = (xd_rect){band.x, ay, band.w / 2, th};
            bar->page_down_rect = (xd_rect){band.x + band.w / 2, ay, band.w - band.w / 2, th};
        } else {
            avail = band.w - arrows_w - gap;
            size_items(items, n, t, avail, rows, side);
            pages = flow(items, n, t, band, avail, rows, row_h, side, fits);
            int ax = band.x + band.w - arrows_w;
            int ah = rows * row_h - 2 * xd_tpx(t, 3);
            int ay = band.y + xd_tpx(t, 3);
            bar->page_up_rect = (xd_rect){ax, ay, arrows_w, ah / 2};
            bar->page_down_rect = (xd_rect){ax, ay + ah / 2, arrows_w, ah - ah / 2};
        }
    }
    bar->pages = pages;
    /* Show the active window's page when it changes. */
    uint64_t active_id = 0;
    size_t active_page = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        const program *p = &ps.items[items[i].prog];
        for (guint k = 0; k < p->windows->len; k++) {
            xd_toplevel *tl = g_ptr_array_index(p->windows, k);
            if ((items[i].window == nullptr || items[i].window == tl) && window_lit(tl)) {
                active_id = tl->id;
                active_page = items[i].page;
            }
        }
    }
    if (active_page != SIZE_MAX && active_id != bar->page_active_id)
        bar->page = active_page;
    bar->page_active_id = active_id;
    if (bar->page >= bar->pages)
        bar->page = bar->pages - 1;
    /* A button being dragged stays under the pointer as it moves along. */
    if (bar->drag_key != nullptr) {
        size_t k = 0;
        while (k < n && strcmp(ps.items[items[k].prog].key, bar->drag_key) != 0)
            k++;
        if (k < n)
            bar->page = items[k].page;
    }
    if (pages > 1)
        xd_taskbar_draw_page_arrows(bar, t, cr);
    cairo_save(cr);
    cairo_rectangle(cr, band.x, band.y, side ? band.w : XD_MAX(avail, 0), band.h);
    cairo_clip(cr);
    bool found_drag = false;
    for (size_t i = 0; i < n; i++) {
        if (items[i].page != bar->page)
            continue;
        if (bar->drag_key != nullptr && !found_drag &&
            strcmp(ps.items[items[i].prog].key, bar->drag_key) == 0) {
            found_drag = true;
            bar->pressed = bar->hot = (int)bar->task_count;
        }
        draw_item(bar, t, cr, &ps, &items[i]);
    }
    cairo_restore(cr);
    xd_free(items);
    programs_free(&ps);
    xd_thumbs_refresh(shell);
}

/* ---- input ----------------------------------------------------------------------- */

static void activate_window(xd_shell *shell, xd_toplevel *tl) {
    if (tl->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(tl->handle);
    if (shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(tl->handle, shell->seat);
}

/* Starts the program (again). */
static void launch(xd_shell *shell, const char *key) {
    if (!is_desktop_key(key) || shell->superbar == nullptr)
        return;
    GDesktopAppInfo *info = info_for(shell->superbar, key);
    if (info != nullptr && xd_app_launch_info(info, nullptr))
        xd_start7_count_launch(key);
}

bool xd_superbar_is_button(const xd_taskbar *bar, int hit) {
    return bar->superbar && hit >= 0 && (size_t)hit < bar->task_count;
}

void xd_superbar_click(xd_taskbar *bar, int hit, uint32_t button) {
    xd_shell *shell = bar->shell;
    if (!xd_superbar_is_button(bar, hit))
        return;
    xd_thumbs_close(shell);
    const xd_bar_button *b = &bar->bar_buttons[hit];
    /* The middle button starts another one, as Shift+click does. */
    if (button == BTN_MIDDLE) {
        launch(shell, b->key);
        return;
    }
    if (b->window != nullptr) {
        xd_toplevel_click(b->window);
        return;
    }
    xd_toplevel *wins[64];
    size_t n = 0;
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        if (n < XD_ARRAY_LEN(wins) && window_is(tl, b->key))
            wins[n++] = tl;
    }
    if (n == 0) {
        launch(shell, b->key);
    } else if (n == 1) {
        xd_toplevel_click(wins[0]);
    } else {
        /* Several windows: the one used last, then each in turn while one
         * of them is active (Windows 7's Ctrl+click; the thumbnails show
         * them all to pick from). */
        size_t active = SIZE_MAX, last = 0;
        for (size_t i = 0; i < n; i++) {
            if (window_lit(wins[i]))
                active = i;
            if (wins[i]->active_seq > wins[last]->active_seq)
                last = i;
        }
        activate_window(shell, wins[active != SIZE_MAX ? (active + 1) % n : last]);
    }
}

char *xd_superbar_tooltip(xd_taskbar *bar, int hit) {
    if (!xd_superbar_is_button(bar, hit))
        return nullptr;
    const xd_bar_button *b = &bar->bar_buttons[hit];
    const xd_toplevel *tl = bar->task_views[hit];
    if (xd_thumbs_cover(bar->shell, b->windows))
        return nullptr; /* its thumbnails say it */
    if (b->windows == 1 && tl != nullptr)
        return xd_str_dup_cstr(xd_str_from_cstr(window_title(tl)));
    if (is_desktop_key(b->key)) {
        GDesktopAppInfo *info = info_for(bar->shell->superbar, b->key);
        if (info != nullptr)
            return xd_str_dup_cstr(xd_str_from_cstr(g_app_info_get_name(G_APP_INFO(info))));
    }
    if (tl != nullptr && tl->app_name != nullptr)
        return xd_str_dup_cstr(xd_str_from_cstr(tl->app_name));
    return tl != nullptr ? xd_str_dup_cstr(xd_str_from_cstr(window_title(tl))) : nullptr;
}

/* Moves the dragged program to where `onto` is: pinned ones among the
 * pinned (saved when the button comes up), others among the others. */
static bool move_program(xd_superbar *sb, const char *dragged, const char *onto) {
    guint from = 0, to = 0;
    if (pin_index(sb, dragged, &from)) {
        if (!pin_index(sb, onto, &to) || !xd_jumplist_move(sb->pins->pdata, sb->pins->len, from, to))
            return false;
        sb->pins_changed = true;
        return true;
    }
    from = order_of(sb, dragged);
    to = order_of(sb, onto);
    return from != G_MAXUINT && to != G_MAXUINT &&
           xd_jumplist_move(sb->order->pdata, sb->order->len, from, to);
}

bool xd_superbar_drag(xd_taskbar *bar, double x, double y) {
    xd_shell *shell = bar->shell;
    if (!xd_superbar_is_button(bar, bar->pressed) && bar->drag_key == nullptr)
        return false;
    if (bar->drag_key == nullptr) {
        if (fabs(x - bar->press_x) < DRAG_SLOP && fabs(y - bar->press_y) < DRAG_SLOP)
            return false;
        bar->drag_key = g_strdup(bar->bar_buttons[bar->pressed].key);
        xd_thumbs_close(shell);
        xd_tooltip_hide(shell);
        xd_tooltip_disarm(shell);
        bar->dirty = true;
    }
    double fx = floor(x * bar->scale), fy = floor(y * bar->scale);
    int px = (int)fx, py = (int)fy;
    for (size_t i = 0; i < bar->task_count; i++) {
        xd_rect r = bar->task_rects[i];
        bool across = px >= r.x && px < r.x + r.w;
        bool down = py >= r.y - (int)bar->scale * 4 && py < r.y + r.h + (int)bar->scale * 4;
        if (!across || !down || strcmp(bar->bar_buttons[i].key, bar->drag_key) == 0)
            continue;
        if (move_program(shell->superbar, bar->drag_key, bar->bar_buttons[i].key)) {
            /* Buttons moved: no more moves until they're laid out again. */
            bar->task_count = 0;
            xd_taskbars_mark_dirty(shell);
        }
        break;
    }
    return true;
}

bool xd_superbar_drop(xd_taskbar *bar) {
    if (bar->drag_key == nullptr)
        return false;
    g_free(bar->drag_key);
    bar->drag_key = nullptr;
    bar->dirty = true;
    if (bar->shell->superbar != nullptr && bar->shell->superbar->pins_changed)
        save_pins(bar->shell->superbar);
    return true;
}

/* ---- jump lists ------------------------------------------------------------------- */

/* What a jump list item acts on, kept in the menu tree until it runs. */
typedef struct jump_ref {
    const char *key; /* the program */
    const char *arg; /* a file's URI, or an action's name */
    uint64_t window; /* Close window on a window's own button: its id */
} jump_ref;

static jump_ref *ref(xd_menu_tree *tree, const char *key, const char *arg) {
    jump_ref *r = XD_ARENA_NEW(&tree->arena, jump_ref);
    r->key = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(key));
    r->arg = arg != nullptr ? xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(arg)) : nullptr;
    r->window = 0;
    return r;
}

static GDesktopAppInfo *ref_info(xd_shell *shell, const jump_ref *r) {
    return shell->superbar != nullptr && is_desktop_key(r->key) ? info_for(shell->superbar, r->key)
                                                                : nullptr;
}

/* A recent file opens in this program, as in Windows 7. */
static void act_open_recent(xd_shell *shell, void *data) {
    const jump_ref *r = data;
    GDesktopAppInfo *info = ref_info(shell, r);
    if (info == nullptr)
        return;
    GList *uris = g_list_append(nullptr, g_strdup(r->arg));
    xd_app_launch_info(info, uris);
    g_list_free_full(uris, g_free);
}

static void act_task(xd_shell *shell, void *data) {
    const jump_ref *r = data;
    GDesktopAppInfo *info = ref_info(shell, r);
    if (info != nullptr)
        xd_app_launch_action(info, r->arg);
}

static void act_launch(xd_shell *shell, void *data) { launch(shell, ((const jump_ref *)data)->key); }

static xd_menu_model *taskbar_jump_list(xd_shell *shell, xd_menu_tree *tree, const char *key,
                                        uint64_t window_id);

/* The open jump list shows the program's again (after a pin, a removal,
 * a move): the taskbar's, or the Start menu's beside a program. Its old
 * items, and their jump_refs, are freed. */
static void jump_list_changed(xd_shell *shell, const char *key) {
    xd_superbar *sb = shell->superbar;
    xd_menu *menu = shell->start_menu;
    bool taskbar = menu != nullptr && shell->context_menu && menu->model->jumplist &&
                   sb->jl_key != nullptr;
    if (!taskbar)
        menu = xd_start7_submenu(shell);
    if (menu == nullptr)
        return;
    xd_menu_tree tree;
    xd_menu_tree_init(&tree);
    xd_menu_model *m = taskbar ? taskbar_jump_list(shell, &tree, sb->jl_key, sb->jl_window)
                               : xd_jumplist_model(shell, &tree, key);
    xd_menu_replace_model(menu, m);
    xd_menu_tree_free(&shell->context_tree);
    shell->context_tree = tree;
}

static bool is_pinned_item(xd_superbar *sb, const char *key, const char *uri) {
    bool pinned = false;
    char **list = xd_jumplist_pinned(sb->jl_pins, key);
    for (char **u = list; *u != nullptr && !pinned; u++)
        pinned = strcmp(*u, uri) == 0;
    g_strfreev(list);
    return pinned;
}

/* A file pinned to or unpinned from the open jump list: it moves between
 * Pinned and Recent, and the list stays open. */
static void act_pin_item(xd_shell *shell, void *data) {
    const jump_ref *r = data;
    xd_superbar *sb = shell->superbar;
    if (sb == nullptr || r->arg == nullptr)
        return;
    if (xd_jumplist_set_pinned(sb->jl_pins, r->key, r->arg, !is_pinned_item(sb, r->key, r->arg)))
        save_jl_pins(sb);
    char *key = g_strdup(r->key);
    jump_list_changed(shell, key); /* may free `r` */
    g_free(key);
}

/* A pinned file dragged onto another: it takes that one's place. */
static void act_move_item(xd_shell *shell, void *data, void *onto) {
    const jump_ref *r = data, *o = onto;
    xd_superbar *sb = shell->superbar;
    if (sb == nullptr || r->arg == nullptr || o->arg == nullptr ||
        !xd_jumplist_move_pinned(sb->jl_pins, r->key, r->arg, o->arg))
        return;
    save_jl_pins(sb);
    char *key = g_strdup(r->key);
    jump_list_changed(shell, key); /* frees `r` and `o` */
    g_free(key);
}

/* "Remove from this list": the recently used list no longer says this
 * program used the file (other programs' lists keep it), so it's gone from
 * Recent until the program opens it again, as in Windows 7. */
static void act_remove_item(xd_shell *shell, void *data) {
    const jump_ref *r = data;
    xd_superbar *sb = shell->superbar;
    GDesktopAppInfo *info = ref_info(shell, r);
    GBookmarkFile *recent = sb != nullptr ? recent_file(sb) : nullptr;
    if (info == nullptr || recent == nullptr || r->arg == nullptr ||
        !xd_jumplist_forget(recent, r->arg, r->key, g_app_info_get_executable(G_APP_INFO(info))))
        return;
    char *path = g_build_filename(g_get_user_data_dir(), "recently-used.xbel", nullptr);
    GError *error = nullptr;
    if (!g_bookmark_file_to_file(recent, path, &error)) {
        fprintf(stderr, "xd-shell: cannot save %s: %s\n", path, error->message);
        g_clear_error(&error);
    }
    g_free(path);
    jump_list_changed(shell, r->key);
}

/* A file's Properties, in Explorer's dialog. */
static void act_item_properties(xd_shell *shell, void *data) {
    const jump_ref *r = data;
    char *path = r->arg != nullptr ? g_filename_from_uri(r->arg, nullptr, nullptr) : nullptr;
    if (path == nullptr)
        return;
    char *exe = xd_shell_own_program("xd-files");
    char *qexe = g_shell_quote(exe);
    char *qpath = g_shell_quote(path);
    char *cmd = g_strdup_printf("%s --properties %s", qexe, qpath);
    xd_shell_launch(shell, cmd);
    g_free(cmd);
    g_free(qpath);
    g_free(qexe);
    g_free(exe);
    g_free(path);
}

void xd_superbar_pin_file(xd_shell *shell, const char *desktop_id, const char *uri) {
    xd_superbar *sb = shell->superbar;
    if (sb != nullptr && xd_jumplist_set_pinned(sb->jl_pins, desktop_id, uri, true))
        save_jl_pins(sb);
}

static void act_pin(xd_shell *shell, void *data) {
    xd_superbar_set_pinned(shell, ((const jump_ref *)data)->key, true);
}

static void act_unpin(xd_shell *shell, void *data) {
    xd_superbar_set_pinned(shell, ((const jump_ref *)data)->key, false);
}

static void act_close_all(xd_shell *shell, void *data) {
    const jump_ref *r = data;
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        if (window_is(tl, r->key))
            zwlr_foreign_toplevel_handle_v1_close(tl->handle);
    }
}

static void act_close_one(xd_shell *shell, void *data) {
    const jump_ref *r = data;
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        if (tl->id == r->window)
            zwlr_foreign_toplevel_handle_v1_close(tl->handle);
    }
}

static xd_menu_item *action(xd_menu_tree *tree, xd_menu_model *m, const char *label,
                            const char *icon, void (*fn)(xd_shell *, void *), void *data) {
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, label, icon);
    it->action = fn;
    it->action_data = data;
    return it;
}

/* A file's icon: its type's, from the icon theme. */
static const char *file_icon(xd_menu_tree *tree, const char *path) {
    char *type = g_content_type_guess(path, nullptr, 0, nullptr);
    char *name = type != nullptr ? g_content_type_get_generic_icon_name(type) : nullptr;
    GIcon *icon = type != nullptr ? g_content_type_get_icon(type) : nullptr;
    const char *best = name;
    if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
        const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
        if (names != nullptr && names[0] != nullptr)
            best = names[0];
    }
    const char *out = best != nullptr ? xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(best))
                                      : nullptr;
    if (icon != nullptr)
        g_object_unref(icon);
    g_free(name);
    g_free(type);
    return out;
}

/* A file's right-click menu in a jump list, as Windows 7's (from memory):
 * Open; Pin to (or Unpin from) this list, and for a recent one Remove from
 * this list; Properties. Pinning and removing leave the jump list open. Its
 * jump_refs are its own tree's, as the list's may be freed first. */
static xd_menu_model *file_context(xd_shell *shell, xd_menu_tree *tree, void *data) {
    const jump_ref *from = data;
    xd_superbar *sb = shell->superbar;
    if (sb == nullptr || from->arg == nullptr)
        return nullptr;
    jump_ref *r = ref(tree, from->key, from->arg);
    xd_menu_model *m = xd_menu_model_new(tree);
    action(tree, m, "Open", nullptr, act_open_recent, r)->bold = true;
    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
    bool pinned = is_pinned_item(sb, r->key, r->arg);
    action(tree, m, pinned ? "Unpin from this list" : "Pin to this list", nullptr, act_pin_item, r)
        ->keep_open = true;
    if (!pinned)
        action(tree, m, "Remove from this list", nullptr, act_remove_item, r)->keep_open = true;
    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
    action(tree, m, "Properties", nullptr, act_item_properties, r);
    return m;
}

/* A task's: just Open. */
static xd_menu_model *task_context([[maybe_unused]] xd_shell *shell, xd_menu_tree *tree,
                                   void *data) {
    const jump_ref *from = data;
    xd_menu_model *m = xd_menu_model_new(tree);
    action(tree, m, "Open", nullptr, act_task, ref(tree, from->key, from->arg))->bold = true;
    return m;
}

/* One file of a jump list, opening in the program; with its pin button and
 * right-click menu. Pinned ones are dragged into another order. */
static void file_item(xd_menu_tree *tree, xd_menu_model *m, const char *key, const char *uri,
                      bool pinned) {
    char *path = g_filename_from_uri(uri, nullptr, nullptr);
    if (path == nullptr)
        return;
    char *label = g_filename_display_basename(path);
    xd_menu_item *it = action(tree, m, label, nullptr, act_open_recent, ref(tree, key, uri));
    it->icon = file_icon(tree, path);
    it->fallback_icon = "text-x-generic";
    it->pin = act_pin_item;
    it->pinned = pinned;
    it->context = file_context;
    it->move = pinned ? act_move_item : nullptr;
    g_free(label);
    g_free(path);
}

/* Pinned, Recent and Tasks, as both jump lists start. */
static void jump_items(xd_shell *shell, xd_menu_tree *tree, xd_menu_model *m, const char *key) {
    xd_superbar *sb = shell->superbar;
    GDesktopAppInfo *info = sb != nullptr && is_desktop_key(key) ? info_for(sb, key) : nullptr;
    if (info == nullptr)
        return;
    char **pinned = xd_jumplist_pinned(sb->jl_pins, key);
    if (pinned[0] != nullptr)
        xd_menu_add(tree, m, XD_MENU_HEADER, "Pinned", nullptr);
    for (char **u = pinned; *u != nullptr; u++)
        file_item(tree, m, key, *u, true);
    GBookmarkFile *recent = recent_file(sb);
    char **uris = xd_jumplist_recent(recent, key, g_app_info_get_executable(G_APP_INFO(info)),
                                     RECENT_MAX);
    xd_jumplist_drop_pinned(uris, pinned);
    drop_forgotten(recent, uris);
    if (uris[0] != nullptr)
        xd_menu_add(tree, m, XD_MENU_HEADER, "Recent", nullptr);
    for (char **u = uris; *u != nullptr; u++)
        file_item(tree, m, key, *u, false);
    g_strfreev(uris);
    g_strfreev(pinned);
    const char *const *actions = g_desktop_app_info_list_actions(info);
    if (actions != nullptr && actions[0] != nullptr)
        xd_menu_add(tree, m, XD_MENU_HEADER, "Tasks", nullptr);
    char *icon = g_desktop_app_info_get_string(info, "Icon");
    for (size_t i = 0; actions != nullptr && actions[i] != nullptr; i++) {
        char *name = g_desktop_app_info_get_action_name(info, actions[i]);
        xd_menu_item *it = action(tree, m, name != nullptr ? name : actions[i], icon, act_task,
                                  ref(tree, key, actions[i]));
        it->fallback_icon = "application-x-executable";
        it->context = task_context;
        g_free(name);
    }
    g_free(icon);
}

bool xd_jumplist_has_items(xd_shell *shell, const char *desktop_id) {
    xd_menu_tree tree;
    xd_menu_tree_init(&tree);
    xd_menu_model *m = xd_menu_model_new(&tree);
    jump_items(shell, &tree, m, desktop_id);
    bool any = m->count > 0;
    xd_menu_tree_free(&tree);
    return any;
}

xd_menu_model *xd_jumplist_model(xd_shell *shell, xd_menu_tree *tree, const char *key) {
    xd_menu_model *m = xd_menu_model_new(tree);
    jump_items(shell, tree, m, key);
    return m;
}

/* The taskbar's, in Windows 7's look: Pinned, Recent, Tasks, then the
 * band with the program, Pin/Unpin and Close. `window_id`: a window's own
 * (labelled) button. */
static xd_menu_model *taskbar_jump_list(xd_shell *shell, xd_menu_tree *tree, const char *key,
                                        uint64_t window_id) {
    xd_menu_model *m = xd_jumplist_model(shell, tree, key);
    m->jumplist = true;
    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr); /* the band */
    GDesktopAppInfo *info =
        is_desktop_key(key) && shell->superbar != nullptr ? info_for(shell->superbar, key)
                                                           : nullptr;
    const xd_toplevel *tl = nullptr;
    XD_LIST_FOR_EACH(w, &shell->toplevels, xd_toplevel, link) {
        if ((window_id != 0 && w->id == window_id) || (window_id == 0 && tl == nullptr &&
                                                       window_is(w, key)))
            tl = w;
    }
    jump_ref *r = ref(tree, key, nullptr);
    if (info != nullptr) {
        char *icon = g_desktop_app_info_get_string(info, "Icon");
        xd_menu_item *it =
            action(tree, m, g_app_info_get_name(G_APP_INFO(info)), icon, act_launch, r);
        it->fallback_icon = "application-x-executable";
        it->band = true;
        g_free(icon);
        bool pinned = xd_superbar_pinned(shell, key);
        it = action(tree, m,
                    pinned ? "Unpin this program from taskbar" : "Pin this program to taskbar",
                    nullptr, pinned ? act_unpin : act_pin, r);
        it->band = true;
        it->pushpin = pinned ? 2 : 1;
    } else if (tl != nullptr) {
        /* Not from an installed program: nothing to start or pin. */
        xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION,
                                       tl->app_name != nullptr ? tl->app_name : window_title(tl),
                                       tl->icon);
        it->fallback_icon = "application-x-executable";
        it->disabled = true;
        it->band = true;
    }
    size_t windows = 0;
    XD_LIST_FOR_EACH(w, &shell->toplevels, xd_toplevel, link) {
        if (window_is(w, key))
            windows++;
    }
    xd_menu_item *close = nullptr;
    if (window_id != 0 && tl != nullptr) {
        /* A window's own button (with labels): that window alone, by the
         * id that is never reused. */
        close = action(tree, m, "Close window", nullptr, act_close_one, ref(tree, key, nullptr));
        ((jump_ref *)close->action_data)->window = window_id;
    } else if (windows > 0) {
        close = action(tree, m, windows > 1 ? "Close all windows" : "Close window", nullptr,
                       act_close_all, r);
    }
    if (close != nullptr) {
        close->icon = "window-close";
        close->fallback_icon = "process-stop";
        close->band = true;
    }
    return m;
}

void xd_superbar_jump_list(xd_taskbar *bar, int hit, uint32_t serial) {
    xd_shell *shell = bar->shell;
    if (!xd_superbar_is_button(bar, hit))
        return;
    xd_menu_close_all(shell);
    xd_thumbs_close(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    const xd_bar_button *b = &bar->bar_buttons[hit];
    xd_superbar *sb = shell->superbar;
    g_free(sb->jl_key);
    sb->jl_key = g_strdup(b->key);
    sb->jl_window = b->window != nullptr ? b->window->id : 0;
    xd_menu_model *model = taskbar_jump_list(shell, &shell->context_tree, sb->jl_key,
                                             sb->jl_window);
    /* Above the button (device px -> surface-local logical). */
    xd_rect r = bar->task_rects[hit];
    xd_rect anchor = {(int)(r.x / bar->scale), (int)(r.y / bar->scale), (int)(r.w / bar->scale),
                      (int)(r.h / bar->scale)};
    xd_taskbar_take_keyboard(bar);
    shell->context_menu = true;
    xd_menu_open_root_bar(shell, model, bar, anchor, serial);
    shell->start_bar = bar;
    bar->dirty = true;
}

/* ---- for thumbnails and drops ------------------------------------------------------ */

size_t xd_superbar_windows(xd_shell *shell, const char *key, uint64_t window_id,
                           xd_toplevel **out, size_t cap) {
    size_t n = 0;
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        bool mine = window_id != 0 ? tl->id == window_id : window_is(tl, key);
        if (mine && n < cap)
            out[n++] = tl;
    }
    return n;
}

const char *xd_superbar_key(const xd_taskbar *bar, int hit, uint64_t *window_id) {
    if (!xd_superbar_is_button(bar, hit))
        return nullptr;
    const xd_bar_button *b = &bar->bar_buttons[hit];
    if (window_id != nullptr)
        *window_id = b->window != nullptr ? b->window->id : 0;
    return b->key;
}

/* A launcher's desktop id: its file name when a launcher by that name is
 * installed (a copy on the desktop is that program, and its windows group
 * with its button), else its path. nullptr: not a launcher. */
static char *launcher_id(xd_superbar *sb, const char *path) {
    if (!g_str_has_suffix(path, ".desktop"))
        return nullptr;
    GDesktopAppInfo *own = g_desktop_app_info_new_from_filename(path);
    if (own == nullptr)
        return nullptr;
    g_object_unref(own);
    char *base = g_path_get_basename(path);
    if (info_for(sb, base) != nullptr)
        return base;
    g_free(base);
    return g_strdup(path);
}

void xd_superbar_dropped(xd_shell *shell, const char *key, char **uris) {
    xd_superbar *sb = shell->superbar;
    if (sb == nullptr)
        return;
    bool changed = false;
    for (char **u = uris; *u != nullptr; u++) {
        char *path = g_filename_from_uri(*u, nullptr, nullptr);
        if (path == nullptr)
            continue;
        char *id = launcher_id(sb, path);
        if (id != nullptr) {
            /* A program: pinned to the taskbar. */
            if (!pin_index(sb, id, nullptr)) {
                g_ptr_array_add(sb->pins, g_strdup(id));
                changed = true;
            }
        } else if (key != nullptr && is_desktop_key(key) && info_for(sb, key) != nullptr) {
            /* A file on a program's button: pinned to its jump list. */
            xd_superbar_pin_file(shell, key, *u);
        } else {
            /* Elsewhere on the bar: to the jump list of the program that
             * opens it, which is pinned too, as Windows 7 does. */
            char *type = g_content_type_guess(path, nullptr, 0, nullptr);
            GAppInfo *app = type != nullptr ? g_app_info_get_default_for_type(type, FALSE)
                                            : nullptr;
            const char *app_id = app != nullptr && G_IS_DESKTOP_APP_INFO(app)
                                     ? g_app_info_get_id(app)
                                     : nullptr;
            if (app_id != nullptr) {
                xd_superbar_pin_file(shell, app_id, *u);
                if (!pin_index(sb, app_id, nullptr)) {
                    g_ptr_array_add(sb->pins, g_strdup(app_id));
                    changed = true;
                }
            }
            if (app != nullptr)
                g_object_unref(app);
            g_free(type);
        }
        g_free(id);
        g_free(path);
    }
    if (changed) {
        save_pins(sb);
        xd_taskbars_forget_layout(shell);
    }
}
