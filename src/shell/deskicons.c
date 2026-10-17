/* SPDX-License-Identifier: MIT
 * Desktop icons, on the first monitor's desktop as in Windows: My
 * Documents, My Computer and the Recycle Bin, then the Desktop folder's
 * files, on XP's 75x75 grid in columns from the top left.
 *
 * Click selects, a drag on the background selects a rectangle, a drag on
 * an icon moves the selection to other grid cells (remembered in the
 * "desktop-icons" state file), a double-click opens. Right-click menus
 * follow XP's; file commands (Delete, Rename, Properties, Empty Recycle
 * Bin) run in xd-files, which has the dialogs. The folder is watched, so
 * the icons follow what other programs do to it.
 */
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <stdlib.h>

#include "../core/alloc.h"
#include "../settings/state.h"
#include "apps.h"
#include "menu.h"
#include "shell.h"

static constexpr int CELL_W = 75; /* XP's icon spacing */
static constexpr int CELL_H = 75;
static constexpr int ICON = 32;
static constexpr int MAX_ICONS = 4096;
static constexpr int DOUBLE_CLICK_MS = 500;
static constexpr double DRAG_THRESHOLD = 4.0;

#define SPECIAL_DOCUMENTS "::documents"
#define SPECIAL_COMPUTER "::computer"
#define SPECIAL_TRASH "::trash"

typedef struct icon {
    char *id; /* a special's "::name", else the file's name in the folder */
    char *label;
    char *icon_names[4];
    char *uri; /* what opening it opens */
    char *path; /* the file, for files; nullptr for specials */
    bool special;
    int rank; /* specials' order: My Documents, My Computer, Recycle Bin */
    bool is_dir;
    bool is_launcher; /* a .desktop file */
    char *sort_type;
    guint64 size;
    gint64 mtime;
    int col, row;
    bool placed;
    bool selected;
    xd_rect hit; /* logical, from the last frame */
} icon;

struct xd_deskicons {
    xd_shell *shell;
    GPtrArray *icons; /* icon* */
    GFile *dir;
    GFileMonitor *dir_monitor;
    GFileMonitor *trash_monitor;
    guint reload_timer;
    GHashTable *saved; /* id -> packed cell (col << 16 | row) */
    xd_rect work;      /* logical: the desktop less the taskbar */
    int cols, rows;
    bool have_work;    /* the desktop has been laid out once */
    /* The pointer. */
    double x, y;
    bool pressed;
    int press_index; /* the icon pressed, or -1 for the background */
    double press_x, press_y;
    bool dragging; /* moving the selection */
    bool banding;  /* selecting with a rectangle */
    uint32_t last_click_time;
    int last_click_index;
};

/* floor/ceil to int (an intermediate double, for -Wbad-function-cast). */
static int ifloor(double v) {
    double f = floor(v);
    return (int)f;
}
static int iceil(double v) {
    double c = ceil(v);
    return (int)c;
}

static bool rect_contains(xd_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static bool rects_overlap(xd_rect a, xd_rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* ---- the items ---------------------------------------------------------------------- */

static void icon_free(gpointer p) {
    icon *ic = p;
    g_free(ic->id);
    g_free(ic->label);
    for (size_t i = 0; i < XD_ARRAY_LEN(ic->icon_names); i++)
        g_free(ic->icon_names[i]);
    g_free(ic->uri);
    g_free(ic->path);
    g_free(ic->sort_type);
    xd_free(ic);
}

static void set_icon_names(icon *ic, GIcon *gicon, const char *fallback) {
    size_t n = 0;
    if (gicon != nullptr && G_IS_THEMED_ICON(gicon)) {
        const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(gicon));
        for (; names != nullptr && *names != nullptr && n + 1 < XD_ARRAY_LEN(ic->icon_names);
             names++)
            ic->icon_names[n++] = g_strdup(*names);
    } else if (gicon != nullptr && G_IS_FILE_ICON(gicon)) {
        ic->icon_names[n++] = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(gicon)));
    }
    if (n < XD_ARRAY_LEN(ic->icon_names))
        ic->icon_names[n] = g_strdup(fallback);
}

static icon *special(const char *id, const char *label, const char *icon_name, const char *uri) {
    icon *ic = XD_NEW(icon);
    ic->id = g_strdup(id);
    ic->label = g_strdup(label);
    ic->icon_names[0] = g_strdup(icon_name);
    ic->icon_names[1] = g_strdup("folder");
    ic->uri = g_strdup(uri);
    ic->special = true;
    ic->is_dir = true;
    ic->sort_type = g_strdup("");
    return ic;
}

static bool trash_empty(void) {
    char *path = g_build_filename(g_get_user_data_dir(), "Trash", "files", nullptr);
    GDir *d = g_dir_open(path, 0, nullptr);
    g_free(path);
    bool empty = d == nullptr || g_dir_read_name(d) == nullptr;
    if (d != nullptr)
        g_dir_close(d);
    return empty;
}

static icon *file_icon(GFile *dir, GFileInfo *info) {
    icon *ic = XD_NEW(icon);
    GFile *f = g_file_get_child(dir, g_file_info_get_name(info));
    ic->id = g_strdup(g_file_info_get_name(info));
    ic->path = g_file_get_path(f);
    ic->uri = g_file_get_uri(f);
    ic->is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    ic->size = (guint64)MAX(g_file_info_get_size(info), 0);
    GDateTime *dt = g_file_info_get_modification_date_time(info);
    if (dt != nullptr) {
        ic->mtime = g_date_time_to_unix(dt);
        g_date_time_unref(dt);
    }
    const char *ctype = g_file_info_get_content_type(info);
    ic->sort_type = g_strdup(ctype != nullptr ? ctype : "");
    /* A launcher shows its program's name and icon, as a .lnk does. */
    GDesktopAppInfo *app = ctype != nullptr && g_content_type_is_a(ctype, "application/x-desktop") &&
                                   ic->path != nullptr
                               ? g_desktop_app_info_new_from_filename(ic->path)
                               : nullptr;
    if (app != nullptr) {
        ic->is_launcher = true;
        ic->label = g_strdup(g_app_info_get_name(G_APP_INFO(app)));
        set_icon_names(ic, g_app_info_get_icon(G_APP_INFO(app)), "application-x-executable");
        g_object_unref(app);
    } else {
        ic->label = g_strdup(g_file_info_get_display_name(info));
        set_icon_names(ic, g_file_info_get_icon(info),
                       ic->is_dir ? "folder" : "text-x-generic");
    }
    g_object_unref(f);
    return ic;
}

/* The Desktop folder (XDG_DESKTOP_DIR), created if missing. */
static GFile *desktop_dir(void) {
    const char *path = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    char *fallback = path == nullptr ? g_build_filename(g_get_home_dir(), "Desktop", nullptr)
                                     : nullptr;
    GFile *f = g_file_new_for_path(path != nullptr ? path : fallback);
    g_free(fallback);
    g_file_make_directory_with_parents(f, nullptr, nullptr);
    return f;
}

static int compare_icons(gconstpointer pa, gconstpointer pb);

static GPtrArray *load_icons(GFile *dir) {
    GPtrArray *out = g_ptr_array_new_with_free_func(icon_free);
    const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    GFile *docs_file = g_file_new_for_path(docs != nullptr ? docs : g_get_home_dir());
    char *docs_uri = g_file_get_uri(docs_file);
    g_object_unref(docs_file);
    g_ptr_array_add(out, special(SPECIAL_DOCUMENTS, "My Documents", "folder-documents", docs_uri));
    g_free(docs_uri);
    g_ptr_array_add(out, special(SPECIAL_COMPUTER, "My Computer", "computer", "computer:///"));
    g_ptr_array_add(out, special(SPECIAL_TRASH, "Recycle Bin",
                                 trash_empty() ? "user-trash" : "user-trash-full", "trash:///"));

    GFileEnumerator *en = g_file_enumerate_children(
        dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME
        "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_ICON
        "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN
        "," G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP "," G_FILE_ATTRIBUTE_STANDARD_SIZE
        "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    GFileInfo *info;
    while (en != nullptr && out->len < MAX_ICONS &&
           (info = g_file_enumerator_next_file(en, nullptr, nullptr)) != nullptr) {
        if (!g_file_info_get_is_hidden(info) && !g_file_info_get_is_backup(info))
            g_ptr_array_add(out, file_icon(dir, info));
        g_object_unref(info);
    }
    g_clear_object(&en);
    for (guint i = 0; i < 3 && i < out->len; i++)
        ((icon *)g_ptr_array_index(out, i))->rank = (int)i;
    /* Icons without a place yet go in name order, folders first. */
    g_ptr_array_sort(out, compare_icons);
    return out;
}

/* ---- the grid ------------------------------------------------------------------------ */

static xd_desktop *host(xd_shell *shell) {
    if (wl_list_empty(&shell->taskbars))
        return nullptr;
    xd_taskbar *first = wl_container_of(shell->taskbars.next, first, link);
    return first->desktop;
}

static void redraw(xd_deskicons *di) {
    xd_desktop *d = host(di->shell);
    if (d != nullptr)
        d->dirty = true;
}

/* The work area: the desktop less a taskbar that's always there. */
static void update_work(xd_deskicons *di, const xd_desktop *d) {
    xd_rect w = {0, 0, d->width, d->height};
    const xd_settings *s = &di->shell->settings;
    if (!s->auto_hide) {
        int t = xd_taskbar_thickness(di->shell);
        switch (s->edge) {
        case XD_EDGE_BOTTOM:
            w.h -= t;
            break;
        case XD_EDGE_TOP:
            w.y += t;
            w.h -= t;
            break;
        case XD_EDGE_LEFT:
            w.x += t;
            w.w -= t;
            break;
        case XD_EDGE_RIGHT:
            w.w -= t;
            break;
        }
    }
    di->work = w;
    di->cols = XD_MAX(w.w / CELL_W, 1);
    di->rows = XD_MAX((w.h - 2) / CELL_H, 1);
}

static icon *at_cell(xd_deskicons *di, int col, int row) {
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (ic->placed && ic->col == col && ic->row == row)
            return ic;
    }
    return nullptr;
}

/* The first free cell in column order from (col, row). */
static void next_free(xd_deskicons *di, int *col, int *row) {
    for (int c = *col; c < di->cols * 4; c++) {
        for (int r = c == *col ? *row : 0; r < di->rows; r++) {
            if (at_cell(di, c, r) == nullptr) {
                *col = c;
                *row = r;
                return;
            }
        }
    }
    *col = di->cols;
    *row = 0;
}

static guint pack_cell(int col, int row) { return (guint)col << 16 | (guint)row; }

/* Saved cells first, then everything else in the free cells, in order;
 * those are remembered too, so icons stay put as files come and go. */
static void place_all(xd_deskicons *di) {
    if (!di->have_work)
        return;
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        ic->placed = false;
    }
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        gpointer v;
        if (!g_hash_table_lookup_extended(di->saved, ic->id, nullptr, &v))
            continue;
        guint packed = GPOINTER_TO_UINT(v);
        int col = (int)(packed >> 16);
        int row = (int)(packed & 0xffff);
        if (col < di->cols && row < di->rows && at_cell(di, col, row) == nullptr) {
            ic->col = col;
            ic->row = row;
            ic->placed = true;
        }
    }
    int col = 0, row = 0;
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (ic->placed)
            continue;
        next_free(di, &col, &row);
        ic->col = col;
        ic->row = row;
        ic->placed = true;
        g_hash_table_replace(di->saved, g_strdup(ic->id), GUINT_TO_POINTER(pack_cell(col, row)));
    }
}

static void save_positions(xd_deskicons *di) {
    g_hash_table_remove_all(di->saved);
    char **lines = g_new0(char *, di->icons->len + 1);
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        g_hash_table_replace(di->saved, g_strdup(ic->id),
                             GUINT_TO_POINTER(pack_cell(ic->col, ic->row)));
        lines[i] = g_strdup_printf("%s\t%d\t%d", ic->id, ic->col, ic->row);
    }
    if (!xd_state_write_lines("desktop-icons", lines, di->icons->len))
        fprintf(stderr, "xd-shell: cannot save the desktop icons' places\n");
    g_strfreev(lines);
}

static void load_positions(xd_deskicons *di) {
    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines("desktop-icons", &lines, &n, MAX_ICONS);
    for (size_t i = 0; i < n; i++) {
        char **f = g_strsplit(lines[i], "\t", 3);
        if (f[0] != nullptr && f[1] != nullptr && f[2] != nullptr) {
            long col = strtol(f[1], nullptr, 10);
            long row = strtol(f[2], nullptr, 10);
            if (col >= 0 && col < 0x7fff && row >= 0 && row < 0xffff)
                g_hash_table_replace(di->saved, g_strdup(f[0]),
                                     GUINT_TO_POINTER(pack_cell((int)col, (int)row)));
        }
        g_strfreev(f);
    }
    xd_free_lines(lines, n);
}

static void reload(xd_deskicons *di) {
    /* The selection survives a reload (a file added beside it). */
    GHashTable *selected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    if (di->icons != nullptr) {
        for (guint i = 0; i < di->icons->len; i++) {
            icon *ic = g_ptr_array_index(di->icons, i);
            if (ic->selected)
                g_hash_table_add(selected, g_strdup(ic->id));
        }
        g_ptr_array_unref(di->icons);
    }
    di->icons = load_icons(di->dir);
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        ic->selected = g_hash_table_contains(selected, ic->id);
    }
    g_hash_table_unref(selected);
    di->press_index = -1;
    di->last_click_index = -1;
    place_all(di);
    redraw(di);
}

static gboolean reload_timeout(gpointer data) {
    xd_deskicons *di = data;
    di->reload_timer = 0;
    reload(di);
    return G_SOURCE_REMOVE;
}

static void folder_changed([[maybe_unused]] GFileMonitor *m, [[maybe_unused]] GFile *f,
                           [[maybe_unused]] GFile *other, [[maybe_unused]] GFileMonitorEvent ev,
                           gpointer data) {
    xd_deskicons *di = data;
    if (di->reload_timer == 0)
        di->reload_timer = g_timeout_add(200, reload_timeout, di);
}

/* ---- drawing --------------------------------------------------------------------------- */

static xd_rect cell_rect(const xd_deskicons *di, int col, int row) {
    return (xd_rect){di->work.x + col * CELL_W, di->work.y + 2 + row * CELL_H, CELL_W, CELL_H};
}

static void draw_icon(xd_deskicons *di, const xd_theme *t, cairo_t *cr, icon *ic, double s,
                      double dx, double dy, bool record) {
    xd_shell *shell = di->shell;
    xd_rect cell = cell_rect(di, ic->col, ic->row);
    double cx = cell.x + dx;
    double cy = cell.y + dy;
    long px_l = lround(ICON * s);
    int px = (int)px_l;
    const char *names[XD_ARRAY_LEN(ic->icon_names)];
    size_t n = 0;
    for (size_t i = 0; i < XD_ARRAY_LEN(ic->icon_names) && ic->icon_names[i] != nullptr; i++)
        names[n++] = ic->icon_names[i];
    cairo_surface_t *img = xd_icons_get_any(shell->icons, names, n, px);
    double ix = round((cx + (CELL_W - ICON) / 2.0) * s);
    double iy = round((cy + 3) * s);
    xd_rgba hi = xd_rgba_from_rgb(shell->scheme.colors[XD_COLOR_HILIGHT]);
    if (img != nullptr) {
        cairo_set_source_surface(cr, img, ix, iy);
        cairo_paint(cr);
        if (ic->selected) {
            /* Windows tints a selected icon with the highlight colour. */
            cairo_set_source_rgba(cr, hi.r, hi.g, hi.b, 0.5);
            cairo_mask_surface(cr, img, ix, iy);
        }
    }

    xd_text_style st = xd_text(t, XD_FONT_ICON, xd_rgba_hex(0xFFFFFF), XD_ALIGN_CENTER);
    PangoLayout *l = xd_text_layout(cr, ic->label, &st);
    long text_w_l = lround((CELL_W - 1) * s);
    int text_w = (int)text_w_l;
    pango_layout_set_width(l, text_w * PANGO_SCALE);
    pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    /* Two lines unless selected, when the whole name shows. */
    if (!ic->selected) {
        pango_layout_set_height(l, -2);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(l, &ink, &logical);
    double tx = round((cx + 0.5) * s);
    double ty = round((cy + 3 + ICON + 3) * s);
    double bx = tx + logical.x - round(2 * s);
    double bw = logical.width + round(4 * s);
    if (ic->selected) {
        cairo_set_source_rgb(cr, hi.r, hi.g, hi.b);
        cairo_rectangle(cr, bx, ty, bw, logical.height);
        cairo_fill(cr);
        xd_rgba ht = xd_rgba_from_rgb(shell->scheme.colors[XD_COLOR_HILIGHT_TEXT]);
        cairo_set_source_rgb(cr, ht.r, ht.g, ht.b);
    } else {
        /* XP's drop shadow under the label. */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
        cairo_move_to(cr, tx + round(s), ty + round(s));
        pango_cairo_show_layout(cr, l);
        cairo_set_source_rgb(cr, 1, 1, 1);
    }
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);

    if (record) {
        /* Hits: the icon and the label, as one box (logical). */
        double top = iy / s;
        double bottom = (ty + logical.height) / s;
        double left = XD_MIN(bx / s, ix / s);
        double right = XD_MAX((bx + bw) / s, (ix + px) / s);
        ic->hit = (xd_rect){ifloor(left), ifloor(top), iceil(right - left), iceil(bottom - top)};
    }
}

void xd_deskicons_draw(xd_shell *shell, xd_desktop *d, cairo_t *cr) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr || d != host(shell))
        return;
    int cols = di->cols, rows = di->rows;
    update_work(di, d);
    if (!di->have_work || cols != di->cols || rows != di->rows) {
        di->have_work = true;
        place_all(di);
    }
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, d->scale);
    double s = d->scale;
    /* The selection on top: its labels show in full and may overlap the
     * icons below. */
    for (int pass = 0; pass < 2; pass++) {
        for (guint i = 0; i < di->icons->len; i++) {
            icon *ic = g_ptr_array_index(di->icons, i);
            if (ic->selected == (pass == 1))
                draw_icon(di, &t, cr, ic, s, 0, 0, true);
        }
    }
    if (di->dragging) {
        /* The selection follows the pointer, faded, until dropped. */
        cairo_push_group(cr);
        for (guint i = 0; i < di->icons->len; i++) {
            icon *ic = g_ptr_array_index(di->icons, i);
            if (ic->selected)
                draw_icon(di, &t, cr, ic, s, di->x - di->press_x, di->y - di->press_y, false);
        }
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.5);
    }
    if (di->banding) {
        xd_rgba hi = xd_rgba_from_rgb(shell->scheme.colors[XD_COLOR_HILIGHT]);
        double x0 = XD_MIN(di->press_x, di->x) * s, y0 = XD_MIN(di->press_y, di->y) * s;
        double w = fabs(di->x - di->press_x) * s, h = fabs(di->y - di->press_y) * s;
        cairo_set_source_rgba(cr, hi.r, hi.g, hi.b, 0.3);
        cairo_rectangle(cr, x0, y0, w, h);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, hi.r, hi.g, hi.b);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, floor(x0) + 0.5, floor(y0) + 0.5, floor(w), floor(h));
        cairo_stroke(cr);
    }
}

/* ---- opening and commands ------------------------------------------------------------- */

static void run_files(xd_shell *shell, const char *option, GPtrArray *args) {
    char *exe = xd_shell_own_program("xd-files");
    GString *cmd = g_string_new(nullptr);
    char *q = g_shell_quote(exe);
    g_string_append(cmd, q);
    g_free(q);
    if (option != nullptr)
        g_string_append_printf(cmd, " %s", option);
    for (guint i = 0; args != nullptr && i < args->len; i++) {
        q = g_shell_quote(g_ptr_array_index(args, i));
        g_string_append_printf(cmd, " %s", q);
        g_free(q);
    }
    xd_shell_launch(shell, cmd->str);
    g_string_free(cmd, TRUE);
    g_free(exe);
}

static void open_icon(xd_deskicons *di, icon *ic) {
    xd_shell *shell = di->shell;
    if (ic->special || ic->is_dir) {
        GPtrArray *args = g_ptr_array_new();
        g_ptr_array_add(args, ic->uri);
        run_files(shell, nullptr, args);
        g_ptr_array_unref(args);
        return;
    }
    GDesktopAppInfo *app = nullptr;
    if (ic->is_launcher && ic->path != nullptr) {
        app = g_desktop_app_info_new_from_filename(ic->path);
        if (app != nullptr && !xd_app_launch_info(app, nullptr))
            fprintf(stderr, "xd-shell: cannot start %s\n", ic->path);
        g_clear_object(&app);
        return;
    }
    /* A document: the program its type opens with. */
    GAppInfo *handler = ic->sort_type[0] != '\0'
                            ? g_app_info_get_default_for_type(ic->sort_type, FALSE)
                            : nullptr;
    if (handler != nullptr && G_IS_DESKTOP_APP_INFO(handler)) {
        GList uris = {.data = ic->uri};
        if (!xd_app_launch_info(G_DESKTOP_APP_INFO(handler), &uris))
            fprintf(stderr, "xd-shell: cannot open %s\n", ic->uri);
    } else {
        fprintf(stderr, "xd-shell: no program opens %s\n", ic->uri);
    }
    g_clear_object(&handler);
}

/* The selection's files (paths), for xd-files. */
static GPtrArray *selected_paths(xd_deskicons *di) {
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (ic->selected && ic->path != nullptr)
            g_ptr_array_add(out, ic->path);
    }
    return out;
}

static void act_open(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_deskicons *di = shell->deskicons;
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (ic->selected)
            open_icon(di, ic);
    }
}

static void act_files(xd_shell *shell, void *data) {
    GPtrArray *paths = selected_paths(shell->deskicons);
    if (paths->len > 0)
        run_files(shell, data, paths);
    g_ptr_array_unref(paths);
}

static void act_empty_trash(xd_shell *shell, [[maybe_unused]] void *data) {
    run_files(shell, "--empty-trash", nullptr);
}

static void act_documents_properties(xd_shell *shell, [[maybe_unused]] void *data) {
    const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, (gpointer)(uintptr_t)(docs != nullptr ? docs : g_get_home_dir()));
    run_files(shell, "--properties", args);
    g_ptr_array_unref(args);
}

static void act_command(xd_shell *shell, void *data) {
    char *exe = xd_shell_own_program(data);
    char *q = g_shell_quote(exe);
    xd_shell_launch(shell, q);
    g_free(q);
    g_free(exe);
}

typedef enum { ARRANGE_NAME, ARRANGE_SIZE, ARRANGE_TYPE, ARRANGE_MODIFIED } arrange_by;

static int arrange_mode;

static int compare_icons(gconstpointer pa, gconstpointer pb) {
    const icon *a = *(const icon *const *)pa;
    const icon *b = *(const icon *const *)pb;
    /* My Documents, My Computer and the Recycle Bin stay first, as in
     * Windows; then folders. */
    if (a->special != b->special)
        return a->special ? -1 : 1;
    if (a->special)
        return a->rank - b->rank;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    int c = 0;
    switch ((arrange_by)arrange_mode) {
    case ARRANGE_SIZE:
        c = (a->size > b->size) - (a->size < b->size);
        break;
    case ARRANGE_TYPE:
        c = strcmp(a->sort_type, b->sort_type);
        break;
    case ARRANGE_MODIFIED:
        c = (a->mtime < b->mtime) - (a->mtime > b->mtime); /* newest first */
        break;
    case ARRANGE_NAME:
        break;
    }
    return c != 0 ? c : g_utf8_collate(a->label, b->label);
}

static void act_arrange(xd_shell *shell, void *data) {
    xd_deskicons *di = shell->deskicons;
    arrange_mode = GPOINTER_TO_INT(data);
    g_ptr_array_sort(di->icons, compare_icons);
    g_hash_table_remove_all(di->saved);
    place_all(di);
    save_positions(di);
    redraw(di);
}

static void act_refresh(xd_shell *shell, [[maybe_unused]] void *data) { reload(shell->deskicons); }

/* New ▸ Folder / Text Document: made in the Desktop folder, selected. */
static void act_new(xd_shell *shell, void *data) {
    xd_deskicons *di = shell->deskicons;
    bool folder = data != nullptr;
    for (guint n = 1; n < 1000; n++) {
        char *name = n == 1 ? g_strdup(folder ? "New Folder" : "New Text Document.txt")
                     : folder ? g_strdup_printf("New Folder (%u)", n)
                              : g_strdup_printf("New Text Document (%u).txt", n);
        GFile *f = g_file_get_child(di->dir, name);
        bool made;
        if (folder) {
            made = g_file_make_directory(f, nullptr, nullptr);
        } else {
            GFileOutputStream *out = g_file_create(f, G_FILE_CREATE_NONE, nullptr, nullptr);
            made = out != nullptr;
            g_clear_object(&out);
        }
        bool exists = !made && g_file_query_exists(f, nullptr);
        g_object_unref(f);
        if (made) {
            reload(di);
            for (guint i = 0; i < di->icons->len; i++) {
                icon *ic = g_ptr_array_index(di->icons, i);
                ic->selected = strcmp(ic->id, name) == 0;
            }
            redraw(di);
        }
        g_free(name);
        if (!exists)
            return;
    }
}

static xd_menu_item *add_action(xd_menu_tree *tree, xd_menu_model *m, const char *label,
                                void (*action)(xd_shell *, void *), void *data) {
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, label, nullptr);
    it->action = action;
    it->action_data = data;
    return it;
}

static void separator(xd_menu_tree *tree, xd_menu_model *m) {
    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
}

static xd_menu_model *icon_menu(xd_deskicons *di, xd_menu_tree *tree, icon *ic) {
    xd_menu_model *m = xd_menu_model_new(tree);
    add_action(tree, m, "Open", act_open, nullptr)->bold = true;
    if (strcmp(ic->id, SPECIAL_COMPUTER) == 0) {
        separator(tree, m);
        /* XP: My Computer's Properties are System Properties. */
        add_action(tree, m, "Properties", act_command, (void *)(uintptr_t)"xd-sysdm");
    } else if (strcmp(ic->id, SPECIAL_TRASH) == 0) {
        separator(tree, m);
        add_action(tree, m, "Empty Recycle Bin", act_empty_trash, nullptr)->disabled =
            trash_empty();
    } else if (strcmp(ic->id, SPECIAL_DOCUMENTS) == 0) {
        separator(tree, m);
        add_action(tree, m, "Properties", act_documents_properties, nullptr);
    } else {
        separator(tree, m);
        add_action(tree, m, "Delete", act_files, (void *)(uintptr_t)"--trash");
        /* Rename works on one item. */
        guint n = 0;
        for (guint i = 0; i < di->icons->len; i++)
            n += ((icon *)g_ptr_array_index(di->icons, i))->selected ? 1u : 0u;
        add_action(tree, m, "Rename", act_files, (void *)(uintptr_t)"--rename")->disabled = n != 1;
        separator(tree, m);
        add_action(tree, m, "Properties", act_files, (void *)(uintptr_t)"--properties");
    }
    return m;
}

static xd_menu_model *background_menu(xd_menu_tree *tree) {
    xd_menu_model *m = xd_menu_model_new(tree);
    xd_menu_model *arrange = xd_menu_model_new(tree);
    add_action(tree, arrange, "Name", act_arrange, GINT_TO_POINTER(ARRANGE_NAME));
    add_action(tree, arrange, "Size", act_arrange, GINT_TO_POINTER(ARRANGE_SIZE));
    add_action(tree, arrange, "Type", act_arrange, GINT_TO_POINTER(ARRANGE_TYPE));
    add_action(tree, arrange, "Modified", act_arrange, GINT_TO_POINTER(ARRANGE_MODIFIED));
    xd_menu_add(tree, m, XD_MENU_SUBMENU, "Arrange Icons By", nullptr)->submenu = arrange;
    add_action(tree, m, "Refresh", act_refresh, nullptr);
    separator(tree, m);
    xd_menu_model *new_menu = xd_menu_model_new(tree);
    add_action(tree, new_menu, "Folder", act_new, (void *)(uintptr_t)"folder");
    separator(tree, new_menu);
    add_action(tree, new_menu, "Text Document", act_new, nullptr);
    xd_menu_add(tree, m, XD_MENU_SUBMENU, "New", nullptr)->submenu = new_menu;
    separator(tree, m);
    /* XP: the desktop's Properties are Display Properties. */
    add_action(tree, m, "Properties", act_command, (void *)(uintptr_t)"xd-desk");
    return m;
}

static void context_menu(xd_deskicons *di, xd_desktop *d, int index, uint32_t serial) {
    xd_shell *shell = di->shell;
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *m = index >= 0
                           ? icon_menu(di, &shell->context_tree,
                                       g_ptr_array_index(di->icons, (guint)index))
                           : background_menu(&shell->context_tree);
    xd_rect anchor = {(int)di->x, (int)di->y, 1, 1};
    xd_menu_open_root_dir(shell, m, d->layer, anchor, serial, true);
}

/* ---- the pointer --------------------------------------------------------------------- */

static int icon_at(xd_deskicons *di, double x, double y) {
    for (guint i = di->icons->len; i > 0; i--) {
        icon *ic = g_ptr_array_index(di->icons, i - 1);
        if (rect_contains(ic->hit, ifloor(x), ifloor(y)))
            return (int)(i - 1);
    }
    return -1;
}

static void select_only(xd_deskicons *di, int index) {
    for (guint i = 0; i < di->icons->len; i++)
        ((icon *)g_ptr_array_index(di->icons, i))->selected = (int)i == index;
}

static void band_select(xd_deskicons *di) {
    xd_rect band = {ifloor(XD_MIN(di->press_x, di->x)), ifloor(XD_MIN(di->press_y, di->y)),
                    iceil(fabs(di->x - di->press_x)), iceil(fabs(di->y - di->press_y))};
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        ic->selected = rects_overlap(band, ic->hit);
    }
}

/* Drops the moved selection on the cells under it; a taken or off-screen
 * cell sends an icon to the nearest free one. */
static void drop_moved(xd_deskicons *di) {
    long dc_l = lround((di->x - di->press_x) / CELL_W);
    long dr_l = lround((di->y - di->press_y) / CELL_H);
    int dc = (int)dc_l, dr = (int)dr_l;
    if (dc == 0 && dr == 0)
        return;
    GPtrArray *moving = g_ptr_array_new();
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (ic->selected) {
            ic->placed = false; /* frees its cell for the others */
            g_ptr_array_add(moving, ic);
        }
    }
    for (guint i = 0; i < moving->len; i++) {
        icon *ic = g_ptr_array_index(moving, i);
        int col = CLAMP(ic->col + dc, 0, di->cols - 1);
        int row = CLAMP(ic->row + dr, 0, di->rows - 1);
        if (at_cell(di, col, row) != nullptr) {
            /* The nearest free cell. */
            int best = INT_MAX, best_col = col, best_row = row;
            for (int c = 0; c < di->cols; c++) {
                for (int r = 0; r < di->rows; r++) {
                    int dist = (c - col) * (c - col) + (r - row) * (r - row);
                    if (dist < best && at_cell(di, c, r) == nullptr) {
                        best = dist;
                        best_col = c;
                        best_row = r;
                    }
                }
            }
            if (best == INT_MAX) {
                next_free(di, &col, &row);
            } else {
                col = best_col;
                row = best_row;
            }
        }
        ic->col = col;
        ic->row = row;
        ic->placed = true;
    }
    g_ptr_array_unref(moving);
    save_positions(di);
}

void xd_deskicons_motion(xd_shell *shell, xd_desktop *d, double x, double y) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr || d != host(shell))
        return;
    di->x = x;
    di->y = y;
    if (!di->pressed)
        return;
    double dist = hypot(x - di->press_x, y - di->press_y);
    if (!di->dragging && !di->banding && dist >= DRAG_THRESHOLD) {
        if (di->press_index >= 0)
            di->dragging = true;
        else
            di->banding = true;
    }
    if (di->banding)
        band_select(di);
    if (di->dragging || di->banding)
        redraw(di);
}

void xd_deskicons_button(xd_shell *shell, xd_desktop *d, uint32_t button, bool pressed,
                         uint32_t time, uint32_t serial) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr || d != host(shell))
        return;
    int index = icon_at(di, di->x, di->y);
    if (button == BTN_RIGHT) {
        if (!pressed)
            return;
        if (index < 0 || !((icon *)g_ptr_array_index(di->icons, (guint)index))->selected)
            select_only(di, index);
        redraw(di);
        context_menu(di, d, index, serial);
        return;
    }
    if (button != BTN_LEFT)
        return;
    if (pressed) {
        di->pressed = true;
        di->press_index = index;
        di->press_x = di->x;
        di->press_y = di->y;
        if (index < 0 || !((icon *)g_ptr_array_index(di->icons, (guint)index))->selected)
            select_only(di, index);
        bool twice = index >= 0 && index == di->last_click_index &&
                     time - di->last_click_time < (uint32_t)DOUBLE_CLICK_MS;
        di->last_click_index = twice ? -1 : index;
        di->last_click_time = time;
        redraw(di);
        if (twice) {
            di->pressed = false;
            open_icon(di, g_ptr_array_index(di->icons, (guint)index));
        }
        return;
    }
    if (di->dragging)
        drop_moved(di);
    else if (!di->banding && di->press_index >= 0)
        select_only(di, di->press_index); /* a click in a selection picks one */
    di->pressed = false;
    di->dragging = false;
    di->banding = false;
    redraw(di);
}

/* ---- life ------------------------------------------------------------------------------- */

void xd_deskicons_init(xd_shell *shell) {
    xd_deskicons *di = XD_NEW(xd_deskicons);
    di->shell = shell;
    di->press_index = -1;
    di->last_click_index = -1;
    di->saved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    di->dir = desktop_dir();
    di->dir_monitor = g_file_monitor_directory(di->dir, G_FILE_MONITOR_WATCH_MOVES, nullptr,
                                               nullptr);
    if (di->dir_monitor != nullptr)
        g_signal_connect(di->dir_monitor, "changed", G_CALLBACK(folder_changed), di);
    /* The Recycle Bin's icon shows whether it's empty. */
    char *trash = g_build_filename(g_get_user_data_dir(), "Trash", "files", nullptr);
    GFile *trash_dir = g_file_new_for_path(trash);
    g_free(trash);
    g_file_make_directory_with_parents(trash_dir, nullptr, nullptr);
    di->trash_monitor = g_file_monitor_directory(trash_dir, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_object_unref(trash_dir);
    if (di->trash_monitor != nullptr)
        g_signal_connect(di->trash_monitor, "changed", G_CALLBACK(folder_changed), di);
    load_positions(di);
    di->cols = di->rows = 1;
    shell->deskicons = di;
    reload(di);
}

void xd_deskicons_relayout(xd_shell *shell) {
    if (shell->deskicons != nullptr)
        redraw(shell->deskicons);
}

void xd_deskicons_finish(xd_shell *shell) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr)
        return;
    if (di->reload_timer != 0)
        g_source_remove(di->reload_timer);
    if (di->dir_monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(di->dir_monitor, di);
        g_object_unref(di->dir_monitor);
    }
    if (di->trash_monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(di->trash_monitor, di);
        g_object_unref(di->trash_monitor);
    }
    g_clear_object(&di->dir);
    if (di->icons != nullptr)
        g_ptr_array_unref(di->icons);
    g_hash_table_unref(di->saved);
    xd_free(di);
    shell->deskicons = nullptr;
}
