/* SPDX-License-Identifier: MIT
 * Desktop icons, on the first monitor's desktop as in Windows: My
 * Documents, My Computer and the Recycle Bin, then the Desktop folder's
 * files, on XP's 75x75 grid in columns from the top left.
 *
 * Click selects (Ctrl adds or removes, Shift selects a run), a drag on
 * the background selects a rectangle, a drag on an icon moves the
 * selection to other grid cells (remembered in the "desktop-icons" state
 * file), a double-click opens. A click gives the desktop the keyboard:
 * arrows, Enter, Delete, F2, Ctrl+A, Ctrl+X/C/V, F5, Alt+Enter, typing a
 * name's first letters, and Alt+F4 for Shut Down. Rename edits the label
 * in place. Right-click menus follow XP's; file commands (Delete,
 * Properties, Paste, Empty Recycle Bin) run in xd-files, which has the
 * dialogs. The folder is watched, so the icons follow what other programs
 * do to it.
 */
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../core/alloc.h"
#include "../files/rename.h"
#include "../settings/state.h"
#include "../ui/shm.h"
#include "../ui/textedit.h"
#include "apps.h"
#include "menu.h"
#include "shell.h"

static constexpr int CELL_W = 75; /* XP's icon spacing */
static constexpr int CELL_H = 75;
static constexpr int ICON = 32;
static constexpr int MAX_ICONS = 4096;
static constexpr int DOUBLE_CLICK_MS = 500;
static constexpr double DRAG_THRESHOLD = 4.0;
static constexpr int TYPEAHEAD_MS = 1000; /* typed letters add up within this */

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
    bool is_link;     /* a symbolic link */
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
    bool press_ctrl, press_shift;
    bool was_sole; /* the pressed icon was the only one selected: slow click renames */
    /* The keyboard. */
    char *focus_id;  /* the icon with the focus (dotted outline) */
    char *anchor_id; /* where Shift selections start */
    bool cues;       /* a key was used: show the focus, as XP does */
    char typed[64];  /* type-ahead */
    uint32_t typed_time;
    /* Rename in place. */
    char *edit_id;
    xd_textedit edit;
    guint rename_timer;
    /* Cut, until pasted or replaced on the clipboard (drawn faded). */
    GHashTable *cut;
    /* Drag and drop: our drag of the selection, and a drop's target. */
    bool wl_drag;
    uint32_t press_serial;
    struct wl_surface *drag_icon;
    xd_shm_ring drag_ring;
    int drop_hot; /* the folder icon a drop would go into, or -1 */
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

static void redraw(xd_deskicons *di);

static void mods(const xd_shell *shell, bool *ctrl, bool *shift, bool *alt) {
    struct xkb_state *st = shell->xkb_state;
    *ctrl = st != nullptr &&
            xkb_state_mod_name_is_active(st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    *shift = st != nullptr &&
             xkb_state_mod_name_is_active(st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
    *alt = st != nullptr &&
           xkb_state_mod_name_is_active(st, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void set_id(char **slot, const char *id) {
    char *copy = id != nullptr ? g_strdup(id) : nullptr;
    g_free(*slot);
    *slot = copy;
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
    ic->is_link = g_file_info_get_is_symlink(info);
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
        "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK
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

static int index_of(const xd_deskicons *di, const char *id) {
    for (guint i = 0; id != nullptr && i < di->icons->len; i++) {
        if (strcmp(((icon *)g_ptr_array_index(di->icons, i))->id, id) == 0)
            return (int)i;
    }
    return -1;
}

/* Grid order: down each column, then the next column. */
static int grid_order(const icon *ic) { return ic->col * 0x10000 + ic->row; }

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
    bool cut = g_hash_table_contains(di->cut, ic->id);
    /* The folder a drag is over shows selected, as in Windows. */
    bool lit = ic->selected || (di->drop_hot >= 0 && (guint)di->drop_hot < di->icons->len &&
                                g_ptr_array_index(di->icons, (guint)di->drop_hot) == ic);
    if (img != nullptr) {
        cairo_set_source_surface(cr, img, ix, iy);
        /* Cut: faded until pasted, as in Windows. */
        cairo_paint_with_alpha(cr, cut ? 0.5 : 1.0);
        if (lit) {
            /* Windows tints a selected icon with the highlight colour. */
            cairo_set_source_rgba(cr, hi.r, hi.g, hi.b, 0.5);
            cairo_mask_surface(cr, img, ix, iy);
        }
    }
    if (ic->is_launcher || ic->is_link)
        xd_draw_shortcut_arrow(cr, ix, iy, px);

    bool editing = di->edit_id != nullptr && strcmp(di->edit_id, ic->id) == 0;
    xd_text_style st = xd_text(t, XD_FONT_ICON, xd_rgba_hex(0xFFFFFF), XD_ALIGN_CENTER);
    xd_str edit_text = xd_textedit_view(&di->edit);
    char *edit_cstr = editing ? xd_str_dup_cstr(edit_text) : nullptr;
    PangoLayout *l = xd_text_layout(cr, editing ? edit_cstr : ic->label, &st);
    long text_w_l = lround((CELL_W - 1) * s);
    int text_w = (int)text_w_l;
    pango_layout_set_width(l, text_w * PANGO_SCALE);
    pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    /* Two lines unless selected, when the whole name shows. */
    if (!ic->selected && !editing) {
        pango_layout_set_height(l, -2);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(l, &ink, &logical);
    double tx = round((cx + 0.5) * s);
    double ty = round((cy + 3 + ICON + 3) * s);
    double bx = tx + logical.x - round(2 * s);
    double bw = logical.width + round(4 * s);
    if (editing) {
        /* Rename: an edit box over the label, black on white with the
         * selection highlighted and a caret. */
        size_t a = 0, b = 0;
        xd_textedit_selection(&di->edit, &a, &b);
        PangoAttrList *attrs = pango_attr_list_new();
        if (b > a) {
            PangoAttribute *bg = pango_attr_background_new((guint16)(hi.r * 65535),
                                                           (guint16)(hi.g * 65535),
                                                           (guint16)(hi.b * 65535));
            xd_rgba ht = xd_rgba_from_rgb(shell->scheme.colors[XD_COLOR_HILIGHT_TEXT]);
            PangoAttribute *fg = pango_attr_foreground_new((guint16)(ht.r * 65535),
                                                           (guint16)(ht.g * 65535),
                                                           (guint16)(ht.b * 65535));
            bg->start_index = fg->start_index = (guint)a;
            bg->end_index = fg->end_index = (guint)b;
            pango_attr_list_insert(attrs, bg);
            pango_attr_list_insert(attrs, fg);
        }
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
        double pad = round(2 * s);
        double ex = XD_MIN(bx, tx + logical.x - pad), ew = XD_MAX(bw, round(12 * s));
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, ex, ty - pad, ew, logical.height + 2 * pad);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, floor(ex) + 0.5, floor(ty - pad) + 0.5, floor(ew) - 1,
                        floor(logical.height + 2 * pad) - 1);
        cairo_stroke(cr);
        cairo_move_to(cr, tx, ty);
        pango_cairo_show_layout(cr, l);
        PangoRectangle caret;
        pango_layout_get_cursor_pos(l, (int)di->edit.cursor, &caret, nullptr);
        cairo_rectangle(cr, round(tx + caret.x / (double)PANGO_SCALE), ty + caret.y / (double)PANGO_SCALE,
                        XD_MAX(round(s), 1), caret.height / (double)PANGO_SCALE);
        cairo_fill(cr);
        g_object_unref(l);
        g_free(edit_cstr);
        if (record) {
            double top = iy / s, bottom = (ty + logical.height + pad) / s;
            double left = XD_MIN(ex / s, ix / s), right = XD_MAX((ex + ew) / s, (ix + px) / s);
            ic->hit = (xd_rect){ifloor(left), ifloor(top), iceil(right - left), iceil(bottom - top)};
        }
        return;
    }
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
    g_free(edit_cstr);

    /* The keyboard focus: a dotted outline round the label, once the
     * keyboard has been used (XP hides it until then). */
    xd_desktop *d = host(shell);
    if (record && di->cues && d != nullptr && shell->kb_surface == d->surface &&
        di->focus_id != nullptr && strcmp(di->focus_id, ic->id) == 0) {
        static const double dash[] = {1.0, 1.0};
        cairo_save(cr);
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_line_width(cr, 1);
        if (ic->selected) {
            xd_rgba ht = xd_rgba_from_rgb(shell->scheme.colors[XD_COLOR_HILIGHT_TEXT]);
            cairo_set_source_rgb(cr, ht.r, ht.g, ht.b);
        } else {
            cairo_set_source_rgb(cr, 1, 1, 1);
        }
        cairo_rectangle(cr, floor(bx) + 0.5, floor(ty) + 0.5, floor(bw) - 1, floor(logical.height) - 1);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

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

void xd_shell_open_uri(xd_shell *shell, const char *uri) {
    GFile *f = g_file_new_for_uri(uri);
    GFileInfo *info = g_file_query_info(f,
                                        G_FILE_ATTRIBUTE_STANDARD_TYPE
                                        "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    char *path = g_file_get_path(f);
    g_object_unref(f);
    bool dir = info == nullptr || g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    const char *ctype = info != nullptr ? g_file_info_get_content_type(info) : nullptr;
    if (dir) {
        /* Folders, and places only xd-files knows (My Computer). */
        GPtrArray *args = g_ptr_array_new();
        g_ptr_array_add(args, (gpointer)(uintptr_t)uri);
        run_files(shell, nullptr, args);
        g_ptr_array_unref(args);
    } else if (ctype != nullptr && g_content_type_is_a(ctype, "application/x-desktop") &&
               path != nullptr) {
        /* A launcher runs its program. */
        GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(path);
        if (app == nullptr || !xd_app_launch_info(app, nullptr))
            fprintf(stderr, "xd-shell: cannot start %s\n", path);
        g_clear_object(&app);
    } else {
        /* A document: the program its type opens with. */
        GAppInfo *handler = ctype != nullptr ? g_app_info_get_default_for_type(ctype, FALSE)
                                             : nullptr;
        if (handler != nullptr && G_IS_DESKTOP_APP_INFO(handler)) {
            GList uris = {.data = (gpointer)(uintptr_t)uri};
            if (!xd_app_launch_info(G_DESKTOP_APP_INFO(handler), &uris))
                fprintf(stderr, "xd-shell: cannot open %s\n", uri);
        } else {
            fprintf(stderr, "xd-shell: no program opens %s\n", uri);
        }
        g_clear_object(&handler);
    }
    g_clear_object(&info);
    g_free(path);
}

static void open_icon(xd_deskicons *di, icon *ic) {
    xd_shell_open_uri(di->shell, ic->uri);
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

/* ---- rename in place ----------------------------------------------------------------- */

static void stop_rename_timer(xd_deskicons *di) {
    if (di->rename_timer != 0)
        g_source_remove(di->rename_timer);
    di->rename_timer = 0;
}

static void start_rename(xd_deskicons *di, icon *ic) {
    stop_rename_timer(di);
    if (ic->special || ic->path == nullptr)
        return;
    set_id(&di->edit_id, ic->id);
    xd_textedit_set(&di->edit, xd_str_from_cstr(ic->label));
    /* The name without its extension is selected, as in Windows. */
    const char *dot = ic->is_dir || ic->is_launcher ? nullptr : strrchr(ic->label, '.');
    if (dot != nullptr && dot != ic->label) {
        xd_textedit_set_cursor(&di->edit, 0, false);
        xd_textedit_set_cursor(&di->edit, (size_t)(dot - ic->label), true);
    }
    redraw(di);
}

/* Windows' "Error Renaming File or Folder" box (xd-message). */
static void rename_failed(xd_shell *shell, const icon *ic, const char *text, const GError *err) {
    char *msg;
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS))
        msg = g_strdup_printf("Cannot rename %s: A file with the name you specified already "
                              "exists. Specify a different file name.",
                              ic->label);
    else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME) ||
             strchr(text, '/') != nullptr)
        msg = g_strdup("A file name cannot contain the following character:\n/");
    else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        msg = g_strdup_printf("Cannot rename %s: Access is denied.", ic->label);
    else
        msg = g_strdup_printf("Cannot rename %s: %s", ic->label, err->message);
    char *exe = xd_shell_own_program("xd-message");
    char *q_exe = g_shell_quote(exe), *q_msg = g_shell_quote(msg);
    char *cmd = g_strdup_printf("%s --error 'Error Renaming File or Folder' %s", q_exe, q_msg);
    xd_shell_launch(shell, cmd);
    g_free(cmd);
    g_free(q_msg);
    g_free(q_exe);
    g_free(exe);
    g_free(msg);
}

static void end_rename(xd_deskicons *di, bool commit) {
    if (di->edit_id == nullptr)
        return;
    int i = index_of(di, di->edit_id);
    char *text = g_strstrip(xd_str_dup_cstr(xd_textedit_view(&di->edit)));
    g_free(di->edit_id);
    di->edit_id = nullptr;
    icon *ic = i >= 0 ? g_ptr_array_index(di->icons, (guint)i) : nullptr;
    if (commit && ic != nullptr && *text != '\0' && strcmp(text, ic->label) != 0) {
        GFile *f = g_file_new_for_path(ic->path);
        GError *err = nullptr;
        GFile *renamed = xd_rename(f, text, &err);
        if (renamed != nullptr) {
            /* It keeps its place (and selection) under its new name. */
            char *name = g_file_get_basename(renamed);
            bool focused = g_strcmp0(di->focus_id, ic->id) == 0;
            g_free(ic->id);
            ic->id = name;
            g_free(ic->label);
            ic->label = g_strdup(text);
            if (focused)
                set_id(&di->focus_id, ic->id);
            save_positions(di);
            g_object_unref(renamed);
        } else {
            rename_failed(di->shell, ic, text, err);
            g_error_free(err);
        }
        g_object_unref(f);
    }
    g_free(text);
    redraw(di);
}

static gboolean rename_later(gpointer data) {
    xd_deskicons *di = data;
    di->rename_timer = 0;
    int i = index_of(di, di->focus_id);
    if (i >= 0)
        start_rename(di, g_ptr_array_index(di->icons, (guint)i));
    return G_SOURCE_REMOVE;
}

/* The desktop's keys edit the name while renaming. */
static void rename_key(xd_deskicons *di, uint32_t sym, const char *utf8, bool ctrl, bool shift) {
    xd_textedit *e = &di->edit;
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        end_rename(di, true);
        return;
    case XKB_KEY_Escape:
        end_rename(di, false);
        return;
    case XKB_KEY_Left:
        xd_textedit_move(e, ctrl ? XD_MOTION_WORD_LEFT : XD_MOTION_LEFT, shift);
        break;
    case XKB_KEY_Right:
        xd_textedit_move(e, ctrl ? XD_MOTION_WORD_RIGHT : XD_MOTION_RIGHT, shift);
        break;
    case XKB_KEY_Home:
        xd_textedit_move(e, XD_MOTION_HOME, shift);
        break;
    case XKB_KEY_End:
        xd_textedit_move(e, XD_MOTION_END, shift);
        break;
    case XKB_KEY_BackSpace:
        xd_textedit_backspace(e, ctrl);
        break;
    case XKB_KEY_Delete:
        xd_textedit_delete(e, ctrl);
        break;
    default:
        if (ctrl && (sym == XKB_KEY_a || sym == XKB_KEY_A))
            xd_textedit_select_all(e);
        else if (!ctrl && utf8 != nullptr && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7F)
            xd_textedit_insert(e, xd_str_from_cstr(utf8));
        break;
    }
    redraw(di);
}

/* ---- the clipboard -------------------------------------------------------------------- */

static void copy_selection(xd_deskicons *di, bool cut, uint32_t serial) {
    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_remove_all(di->cut);
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (!ic->selected || ic->path == nullptr)
            continue;
        g_ptr_array_add(uris, g_strdup(ic->uri));
        if (cut)
            g_hash_table_add(di->cut, g_strdup(ic->id));
    }
    g_ptr_array_add(uris, nullptr);
    if (uris->len > 1)
        xd_clipboard_set_files(di->shell, (char *const *)uris->pdata, cut, serial);
    g_ptr_array_unref(uris);
    redraw(di);
}

static void got_paste(xd_shell *shell, char **uris, bool cut, [[maybe_unused]] void *user) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr || uris == nullptr)
        return;
    /* xd-files copies or moves, with its progress and questions. */
    char *dir = g_file_get_path(di->dir);
    char *q = g_shell_quote(dir);
    char *option = g_strconcat(cut ? "--move-to=" : "--copy-to=", q, nullptr);
    GPtrArray *args = g_ptr_array_new();
    for (char **u = uris; *u != nullptr; u++)
        g_ptr_array_add(args, *u);
    run_files(shell, option, args);
    g_ptr_array_unref(args);
    g_free(option);
    g_free(q);
    g_free(dir);
    /* What was cut is gone from the clipboard once pasted, as in Windows. */
    if (cut)
        xd_clipboard_clear_cut(shell);
}

static void act_cut(xd_shell *shell, [[maybe_unused]] void *data) {
    copy_selection(shell->deskicons, true, shell->menu_serial);
}
static void act_copy(xd_shell *shell, [[maybe_unused]] void *data) {
    copy_selection(shell->deskicons, false, shell->menu_serial);
}
static void act_paste(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_clipboard_get_files(shell, got_paste, nullptr);
}
static void act_rename(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_deskicons *di = shell->deskicons;
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (ic->selected) {
            set_id(&di->focus_id, ic->id);
            start_rename(di, ic);
            return;
        }
    }
}

void xd_deskicons_clipboard_lost(xd_shell *shell) {
    xd_deskicons *di = shell->deskicons;
    if (di != nullptr && g_hash_table_size(di->cut) > 0) {
        g_hash_table_remove_all(di->cut);
        redraw(di);
    }
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
        add_action(tree, m, "Cut", act_cut, nullptr);
        add_action(tree, m, "Copy", act_copy, nullptr);
        separator(tree, m);
        add_action(tree, m, "Delete", act_files, (void *)(uintptr_t)"--trash");
        /* Rename works on one item. */
        guint n = 0;
        for (guint i = 0; i < di->icons->len; i++)
            n += ((icon *)g_ptr_array_index(di->icons, i))->selected ? 1u : 0u;
        add_action(tree, m, "Rename", act_rename, nullptr)->disabled = n != 1;
        separator(tree, m);
        add_action(tree, m, "Properties", act_files, (void *)(uintptr_t)"--properties");
    }
    return m;
}

static xd_menu_model *background_menu(xd_shell *shell, xd_menu_tree *tree) {
    xd_menu_model *m = xd_menu_model_new(tree);
    xd_menu_model *arrange = xd_menu_model_new(tree);
    add_action(tree, arrange, "Name", act_arrange, GINT_TO_POINTER(ARRANGE_NAME));
    add_action(tree, arrange, "Size", act_arrange, GINT_TO_POINTER(ARRANGE_SIZE));
    add_action(tree, arrange, "Type", act_arrange, GINT_TO_POINTER(ARRANGE_TYPE));
    add_action(tree, arrange, "Modified", act_arrange, GINT_TO_POINTER(ARRANGE_MODIFIED));
    xd_menu_add(tree, m, XD_MENU_SUBMENU, "Arrange Icons By", nullptr)->submenu = arrange;
    add_action(tree, m, "Refresh", act_refresh, nullptr);
    separator(tree, m);
    add_action(tree, m, "Paste", act_paste, nullptr)->disabled = !xd_clipboard_has_files(shell);
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

static void context_menu(xd_deskicons *di, xd_desktop *d, int index, xd_rect anchor,
                         uint32_t serial) {
    xd_shell *shell = di->shell;
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *m = index >= 0
                           ? icon_menu(di, &shell->context_tree,
                                       g_ptr_array_index(di->icons, (guint)index))
                           : background_menu(shell, &shell->context_tree);
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

/* Shift: everything in grid order from the anchor to `index`. */
static void select_run(xd_deskicons *di, int index, bool add) {
    int a = index_of(di, di->anchor_id);
    if (a < 0)
        a = index;
    int lo = grid_order(g_ptr_array_index(di->icons, (guint)a));
    int hi = grid_order(g_ptr_array_index(di->icons, (guint)index));
    if (lo > hi) {
        int t = lo;
        lo = hi;
        hi = t;
    }
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        bool in = grid_order(ic) >= lo && grid_order(ic) <= hi;
        ic->selected = in || (add && ic->selected);
    }
}

static int selected_count(const xd_deskicons *di) {
    int n = 0;
    for (guint i = 0; i < di->icons->len; i++)
        n += ((icon *)g_ptr_array_index(di->icons, i))->selected ? 1 : 0;
    return n;
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

static bool start_wl_drag(xd_deskicons *di, xd_desktop *d);

void xd_deskicons_motion(xd_shell *shell, xd_desktop *d, double x, double y) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr || d != host(shell))
        return;
    di->x = x;
    di->y = y;
    if (!di->pressed)
        return;
    double dist = hypot(x - di->press_x, y - di->press_y);
    if (!di->dragging && !di->banding && !di->wl_drag && dist >= DRAG_THRESHOLD) {
        if (di->press_index >= 0 && start_wl_drag(di, d))
            return;
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
    /* A click outside the name being edited ends the rename (keeping it). */
    if (pressed && di->edit_id != nullptr) {
        int editing = index_of(di, di->edit_id);
        if (index == editing && index >= 0)
            return;
        end_rename(di, true);
        index = icon_at(di, di->x, di->y);
    }
    stop_rename_timer(di);
    if (button == BTN_RIGHT) {
        if (!pressed)
            return;
        if (index < 0 || !((icon *)g_ptr_array_index(di->icons, (guint)index))->selected)
            select_only(di, index);
        if (index >= 0)
            set_id(&di->focus_id, ((icon *)g_ptr_array_index(di->icons, (guint)index))->id);
        redraw(di);
        context_menu(di, d, index, (xd_rect){(int)di->x, (int)di->y, 1, 1}, serial);
        return;
    }
    if (button != BTN_LEFT)
        return;
    if (pressed) {
        bool ctrl, shift, alt;
        mods(shell, &ctrl, &shift, &alt);
        di->pressed = true;
        di->press_index = index;
        di->press_x = di->x;
        di->press_y = di->y;
        di->press_ctrl = ctrl;
        di->press_shift = shift;
        di->press_serial = serial;
        icon *hit = index >= 0 ? g_ptr_array_index(di->icons, (guint)index) : nullptr;
        di->was_sole = hit != nullptr && hit->selected && selected_count(di) == 1;
        if (hit != nullptr && shift) {
            select_run(di, index, ctrl);
            set_id(&di->focus_id, hit->id);
        } else if (hit != nullptr && ctrl) {
            hit->selected = !hit->selected;
            set_id(&di->focus_id, hit->id);
            set_id(&di->anchor_id, hit->id);
        } else {
            if (index < 0 || !hit->selected)
                select_only(di, index);
            if (hit != nullptr) {
                set_id(&di->focus_id, hit->id);
                set_id(&di->anchor_id, hit->id);
            }
        }
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
    if (!di->pressed)
        return; /* the press went to the name being edited */
    bool plain = !di->press_ctrl && !di->press_shift;
    if (di->dragging) {
        drop_moved(di);
    } else if (!di->banding && di->press_index >= 0 && plain) {
        select_only(di, di->press_index); /* a click in a selection picks one */
        /* A second, slow click on a lone selected icon's name renames it. */
        icon *ic = g_ptr_array_index(di->icons, (guint)di->press_index);
        if (di->was_sole && di->last_click_index == di->press_index &&
            di->press_y >= ic->hit.y + ICON + 3)
            di->rename_timer = g_timeout_add(DOUBLE_CLICK_MS, rename_later, di);
    }
    di->pressed = false;
    di->dragging = false;
    di->banding = false;
    redraw(di);
}

/* ---- drag and drop ----------------------------------------------------------------------- */

/* The drag image: the selection, faded, the pointer where it was pressed. */
static struct wl_surface *drag_image(xd_deskicons *di, xd_desktop *d) {
    xd_shell *shell = di->shell;
    xd_rect box = {0};
    bool any = false;
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (!ic->selected)
            continue;
        if (!any) {
            box = ic->hit;
            any = true;
            continue;
        }
        int x2 = XD_MAX(box.x + box.w, ic->hit.x + ic->hit.w);
        int y2 = XD_MAX(box.y + box.h, ic->hit.y + ic->hit.h);
        box.x = XD_MIN(box.x, ic->hit.x);
        box.y = XD_MIN(box.y, ic->hit.y);
        box.w = x2 - box.x;
        box.h = y2 - box.y;
    }
    if (!any || box.w <= 0 || box.h <= 0 || box.w > 4096 || box.h > 4096 ||
        shell->compositor == nullptr || shell->shm == nullptr)
        return nullptr;
    double up = ceil(d->scale);
    int bs = XD_CLAMP((int)up, 1, 4);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&di->drag_ring, shell->shm, box.w * bs, box.h * bs);
    if (buf == nullptr)
        return nullptr;
    cairo_t *cr = cairo_create(buf->surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -box.x * bs, -box.y * bs);
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, bs);
    cairo_push_group(cr);
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (ic->selected)
            draw_icon(di, &t, cr, ic, bs, 0, 0, false);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.5);
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);
    struct wl_surface *s = wl_compositor_create_surface(shell->compositor);
    wl_surface_set_buffer_scale(s, bs);
    /* Its top left, from the pointer (wl_compositor 4: attach's offset). */
    long ox = lround(box.x - di->press_x), oy = lround(box.y - di->press_y);
    wl_surface_attach(s, buf->buffer, (int32_t)ox, (int32_t)oy);
    wl_surface_damage_buffer(s, 0, 0, INT32_MAX, INT32_MAX);
    buf->busy = true;
    wl_surface_commit(s);
    return s;
}

/* Files only: My Computer and the others move about the grid as before. */
static bool start_wl_drag(xd_deskicons *di, xd_desktop *d) {
    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    bool files_only = true;
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        if (!ic->selected)
            continue;
        if (ic->path == nullptr)
            files_only = false;
        else
            g_ptr_array_add(uris, g_strdup(ic->uri));
    }
    g_ptr_array_add(uris, nullptr);
    bool ok = false;
    if (files_only && uris->len > 1) {
        di->drag_icon = drag_image(di, d);
        ok = xd_dnd_start_drag(di->shell, (char *const *)uris->pdata, d->surface, di->drag_icon,
                               di->press_serial);
        if (ok) {
            di->wl_drag = true;
        } else if (di->drag_icon != nullptr) {
            wl_surface_destroy(di->drag_icon);
            di->drag_icon = nullptr;
        }
    }
    g_ptr_array_unref(uris);
    return ok;
}

void xd_deskicons_drag_ended(xd_shell *shell) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr)
        return;
    /* The drag had the pointer: the press never ends here. */
    di->wl_drag = false;
    di->pressed = false;
    di->dragging = false;
    if (di->drag_icon != nullptr) {
        wl_surface_destroy(di->drag_icon);
        di->drag_icon = nullptr;
    }
    xd_shm_ring_finish(&di->drag_ring);
    redraw(di);
}

/* What's under (x, y) for a drop: a folder, the Recycle Bin, or the
 * desktop itself. Our own dragged icons aren't targets. */
static xd_drop_kind drop_target(xd_deskicons *di, double x, double y, bool own, int *index) {
    *index = icon_at(di, x, y);
    if (*index < 0)
        return XD_DROP_HERE;
    icon *ic = g_ptr_array_index(di->icons, (guint)*index);
    if (own && ic->selected) {
        *index = -1;
        return XD_DROP_HERE;
    }
    if (strcmp(ic->id, SPECIAL_TRASH) == 0)
        return XD_DROP_TRASH;
    if (strcmp(ic->id, SPECIAL_COMPUTER) == 0)
        return XD_DROP_NONE;
    if (ic->is_dir)
        return XD_DROP_FOLDER;
    *index = -1;
    return XD_DROP_HERE; /* onto a file: beside it */
}

xd_drop_kind xd_deskicons_drag_over(xd_shell *shell, xd_desktop *d, double x, double y,
                                    bool own) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr || d != host(shell))
        return XD_DROP_NONE;
    int index = -1;
    xd_drop_kind k = x < 0 ? XD_DROP_NONE : drop_target(di, x, y, own, &index);
    if (x < 0)
        index = -1;
    if (index != di->drop_hot) {
        di->drop_hot = index;
        redraw(di);
    }
    return k;
}

/* The folder a drop goes into: a folder icon's, else the Desktop. */
static char *drop_dir(xd_deskicons *di, int index) {
    if (index < 0)
        return g_file_get_path(di->dir);
    icon *ic = g_ptr_array_index(di->icons, (guint)index);
    if (ic->path != nullptr)
        return g_strdup(ic->path);
    GFile *f = g_file_new_for_uri(ic->uri);
    char *path = g_file_get_path(f);
    g_object_unref(f);
    return path;
}

/* Dropped files land in free cells from the drop's, in order. */
static void place_dropped(xd_deskicons *di, char **uris, double x, double y) {
    long col_l = lround((x - di->work.x - CELL_W / 2.0) / CELL_W);
    long row_l = lround((y - di->work.y - 2 - CELL_H / 2.0) / CELL_H);
    int col = CLAMP((int)col_l, 0, di->cols - 1), row = CLAMP((int)row_l, 0, di->rows - 1);
    for (char **u = uris; *u != nullptr; u++) {
        GFile *f = g_file_new_for_uri(*u);
        char *name = g_file_get_basename(f);
        g_object_unref(f);
        if (name != nullptr && index_of(di, name) < 0) {
            /* Not taken by an icon, nor promised to an earlier one. */
            int c = col, r = row;
            for (;;) {
                next_free(di, &c, &r);
                bool promised = false;
                GHashTableIter it;
                gpointer v;
                g_hash_table_iter_init(&it, di->saved);
                while (!promised && g_hash_table_iter_next(&it, nullptr, &v))
                    promised = GPOINTER_TO_UINT(v) == pack_cell(c, r);
                if (!promised || c >= di->cols)
                    break;
                if (++r >= di->rows) {
                    r = 0;
                    c++;
                }
            }
            g_hash_table_replace(di->saved, g_strdup(name), GUINT_TO_POINTER(pack_cell(c, r)));
        }
        g_free(name);
    }
}

void xd_deskicons_drop(xd_shell *shell, xd_desktop *d, char **uris, double x, double y,
                       bool move) {
    xd_deskicons *di = shell->deskicons;
    if (di == nullptr || d != host(shell))
        return;
    bool own = uris == nullptr;
    int index = -1;
    xd_drop_kind k = drop_target(di, x, y, own, &index);
    di->drop_hot = -1;
    if (k == XD_DROP_NONE)
        return;
    bool ctrl, shift, alt;
    mods(shell, &ctrl, &shift, &alt);
    if (own && k == XD_DROP_HERE && !ctrl) {
        /* Our icons to another place on the desktop. */
        di->x = x;
        di->y = y;
        drop_moved(di);
        redraw(di);
        return;
    }
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    if (own) {
        for (guint i = 0; i < di->icons->len; i++) {
            icon *ic = g_ptr_array_index(di->icons, i);
            if (ic->selected && ic->path != nullptr)
                g_ptr_array_add(list, g_strdup(ic->uri));
        }
        move = !ctrl;
    } else {
        for (char **u = uris; *u != nullptr; u++)
            g_ptr_array_add(list, g_strdup(*u));
    }
    g_ptr_array_add(list, nullptr);
    if (list->len > 1) {
        GPtrArray *args = g_ptr_array_new();
        for (guint i = 0; i + 1 < list->len; i++)
            g_ptr_array_add(args, g_ptr_array_index(list, i));
        if (k == XD_DROP_TRASH) {
            run_files(shell, "--trash", args);
        } else {
            char *dir = drop_dir(di, index);
            if (dir != nullptr) {
                if (index < 0)
                    place_dropped(di, (char **)list->pdata, x, y);
                char *q = g_shell_quote(dir);
                char *option = g_strconcat(move ? "--move-to=" : "--copy-to=", q, nullptr);
                run_files(shell, option, args);
                g_free(option);
                g_free(q);
            }
            g_free(dir);
        }
        g_ptr_array_unref(args);
    }
    g_ptr_array_unref(list);
    redraw(di);
}

/* ---- the keyboard ------------------------------------------------------------------------ */

/* The icon an arrow key goes to from `from`: the nearest that way, in the
 * same row or column if there's one. */
static int step(const xd_deskicons *di, int from, int dc, int dr) {
    const icon *f = g_ptr_array_index(di->icons, (guint)from);
    int best = -1;
    long best_score = LONG_MAX;
    for (guint i = 0; i < di->icons->len; i++) {
        const icon *ic = g_ptr_array_index(di->icons, i);
        int along = dc != 0 ? (ic->col - f->col) * dc : (ic->row - f->row) * dr;
        int across = dc != 0 ? abs(ic->row - f->row) : abs(ic->col - f->col);
        if (along <= 0)
            continue;
        long score = (long)across * 10000 + along;
        if (score < best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    return best;
}

static int first_or_last(const xd_deskicons *di, bool last) {
    int best = -1;
    for (guint i = 0; i < di->icons->len; i++) {
        const icon *ic = g_ptr_array_index(di->icons, i);
        if (best < 0 || (grid_order(ic) < grid_order(g_ptr_array_index(di->icons, (guint)best))) != last)
            best = (int)i;
    }
    return best;
}

/* Typing a name's first letters goes to the next icon that starts so. */
static void type_ahead(xd_deskicons *di, const char *utf8, uint32_t now) {
    size_t len = strlen(di->typed);
    if (now - di->typed_time > (uint32_t)TYPEAHEAD_MS)
        len = 0;
    di->typed_time = now;
    if (len + strlen(utf8) >= sizeof(di->typed))
        return;
    memcpy(di->typed + len, utf8, strlen(utf8) + 1);
    char *want = g_utf8_casefold(di->typed, -1);
    /* A repeated letter cycles through the icons that start with it. */
    bool cycle = g_utf8_strlen(di->typed, -1) == 1 || (len > 0 && strcmp(di->typed + len, utf8) == 0 &&
                                                         strspn(di->typed, utf8) == strlen(di->typed));
    int from = index_of(di, di->focus_id);
    int from_order = from >= 0 ? grid_order(g_ptr_array_index(di->icons, (guint)from)) : -1;
    int found = -1, first = -1;
    char *one = g_utf8_casefold(utf8, -1);
    for (guint i = 0; i < di->icons->len; i++) {
        icon *ic = g_ptr_array_index(di->icons, i);
        char *label = g_utf8_casefold(ic->label, -1);
        bool match = g_str_has_prefix(label, cycle ? one : want);
        g_free(label);
        if (!match)
            continue;
        int o = grid_order(ic);
        if (first < 0 || o < grid_order(g_ptr_array_index(di->icons, (guint)first)))
            first = (int)i;
        bool after = cycle ? o > from_order : o >= from_order;
        if (after && (found < 0 || o < grid_order(g_ptr_array_index(di->icons, (guint)found))))
            found = (int)i;
    }
    g_free(one);
    g_free(want);
    if (found < 0)
        found = first;
    if (found >= 0) {
        select_only(di, found);
        icon *ic = g_ptr_array_index(di->icons, (guint)found);
        set_id(&di->focus_id, ic->id);
        set_id(&di->anchor_id, ic->id);
    }
}

bool xd_deskicons_key(xd_shell *shell, uint32_t sym, const char *utf8, uint32_t serial) {
    xd_deskicons *di = shell->deskicons;
    xd_desktop *d = host(shell);
    if (di == nullptr || d == nullptr)
        return false;
    bool ctrl, shift, alt;
    mods(shell, &ctrl, &shift, &alt);
    di->cues = true;
    if (di->edit_id != nullptr) {
        rename_key(di, sym, utf8, ctrl, shift);
        return true;
    }
    int focus = index_of(di, di->focus_id);
    int to = -1;
    switch (sym) {
    case XKB_KEY_Left:
    case XKB_KEY_Right:
    case XKB_KEY_Up:
    case XKB_KEY_Down:
        if (focus < 0) {
            to = first_or_last(di, false);
        } else {
            int dc = sym == XKB_KEY_Left ? -1 : sym == XKB_KEY_Right ? 1 : 0;
            int dr = sym == XKB_KEY_Up ? -1 : sym == XKB_KEY_Down ? 1 : 0;
            to = step(di, focus, dc, dr);
        }
        break;
    case XKB_KEY_Home:
    case XKB_KEY_End:
        to = first_or_last(di, sym == XKB_KEY_End);
        break;
    case XKB_KEY_space:
        if (ctrl && focus >= 0) {
            icon *ic = g_ptr_array_index(di->icons, (guint)focus);
            ic->selected = !ic->selected;
        } else if (focus >= 0) {
            select_only(di, focus);
        }
        redraw(di);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (alt) {
            /* Properties of the selection. */
            if (focus >= 0 && ((icon *)g_ptr_array_index(di->icons, (guint)focus))->special) {
                icon *ic = g_ptr_array_index(di->icons, (guint)focus);
                if (strcmp(ic->id, SPECIAL_COMPUTER) == 0)
                    act_command(shell, (void *)(uintptr_t)"xd-sysdm");
                else if (strcmp(ic->id, SPECIAL_DOCUMENTS) == 0)
                    act_documents_properties(shell, nullptr);
            } else {
                act_files(shell, (void *)(uintptr_t)"--properties");
            }
        } else {
            act_open(shell, nullptr);
        }
        return true;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        act_files(shell, (void *)(uintptr_t)(shift ? "--delete" : "--trash"));
        return true;
    case XKB_KEY_F2:
        if (selected_count(di) == 1)
            act_rename(shell, nullptr);
        return true;
    case XKB_KEY_F5:
        reload(di);
        return true;
    case XKB_KEY_F4:
        if (alt) {
            /* Alt+F4 on the desktop: Shut Down, as in Windows. */
            act_command(shell, (void *)(uintptr_t)"xd-shutdown");
            return true;
        }
        return false;
    case XKB_KEY_Menu:
    case XKB_KEY_F10:
        if (sym == XKB_KEY_F10 && !shift)
            return false;
        {
            /* The context menu, under the focused icon. */
            icon *ic = focus >= 0 ? g_ptr_array_index(di->icons, (guint)focus) : nullptr;
            bool on_icon = ic != nullptr && ic->selected;
            xd_rect at = on_icon ? (xd_rect){ic->hit.x + ic->hit.w / 2, ic->hit.y + ic->hit.h / 2, 1, 1}
                                 : (xd_rect){di->work.x + 8, di->work.y + 8, 1, 1};
            context_menu(di, d, on_icon ? focus : -1, at, serial);
        }
        return true;
    default:
        break;
    }
    if (ctrl && !alt) {
        switch (sym) {
        case XKB_KEY_a:
        case XKB_KEY_A:
            for (guint i = 0; i < di->icons->len; i++)
                ((icon *)g_ptr_array_index(di->icons, i))->selected = true;
            redraw(di);
            return true;
        case XKB_KEY_c:
        case XKB_KEY_C:
        case XKB_KEY_Insert:
            copy_selection(di, false, serial);
            return true;
        case XKB_KEY_x:
        case XKB_KEY_X:
            copy_selection(di, true, serial);
            return true;
        case XKB_KEY_v:
        case XKB_KEY_V:
            xd_clipboard_get_files(shell, got_paste, nullptr);
            return true;
        default:
            return false;
        }
    }
    if (to >= 0) {
        icon *ic = g_ptr_array_index(di->icons, (guint)to);
        if (shift) {
            select_run(di, to, ctrl);
        } else if (!ctrl) {
            select_only(di, to);
            set_id(&di->anchor_id, ic->id);
        }
        set_id(&di->focus_id, ic->id);
        redraw(di);
        return true;
    }
    if (!alt && utf8 != nullptr && (unsigned char)utf8[0] > 0x20 && utf8[0] != 0x7F) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        type_ahead(di, utf8, (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
        redraw(di);
        return true;
    }
    return sym == XKB_KEY_Escape || sym == XKB_KEY_Tab;
}

/* ---- life ------------------------------------------------------------------------------- */

void xd_deskicons_init(xd_shell *shell) {
    xd_deskicons *di = XD_NEW(xd_deskicons);
    di->shell = shell;
    di->press_index = -1;
    di->last_click_index = -1;
    di->saved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    di->cut = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    di->drop_hot = -1;
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
    stop_rename_timer(di);
    if (di->drag_icon != nullptr)
        wl_surface_destroy(di->drag_icon);
    xd_shm_ring_finish(&di->drag_ring);
    xd_textedit_free(&di->edit);
    g_free(di->edit_id);
    g_free(di->focus_id);
    g_free(di->anchor_id);
    g_hash_table_unref(di->cut);
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
