/* SPDX-License-Identifier: MIT
 * Windows 7's taskbar thumbnails (Aero's taskbar): hovering a running
 * program's button shows a live picture of each of its windows above it,
 * side by side, each with its title and, while hovered, a close button.
 * Hovering a picture a moment peeks at that window (Aero Peek: the others
 * turn to glass outlines); clicking it brings the window forward, and the
 * middle button closes it. When the pictures wouldn't fit across the
 * screen, the windows are listed by title instead, as Windows 7 did. On a
 * taskbar at the screen's left or right they stack down beside it, and are
 * listed when they wouldn't fit down the screen.
 *
 * A media player's picture has Previous, Play/Pause and Next buttons under
 * it, as Windows Media Player's thumbnail toolbar had: through MPRIS
 * (mpris.c), for a program with one window and one player that names it
 * (its DesktopEntry), since nothing tells which window a player's process
 * drew.
 *
 * The pictures come from the compositor through ext-image-copy-capture-v1
 * with ext-foreign-toplevel-image-capture-source-v1, in shared memory, at
 * most thumbnail size (the compositor scales them down). Which
 * ext-foreign-toplevel-list handle is which window, xd-window-arranger-v1
 * tells. xd-compositor offers these only to the shell it started; under
 * another compositor, or without them, a group's button shows the list of
 * its windows' titles and a single window's its tooltip, as Windows 7
 * Basic did.
 *
 * A popup of the taskbar's layer surface without a grab, so the pointer
 * moves between the button and the pictures freely.
 */
#include <linux/input-event-codes.h>
#include <math.h>

#include <glib.h>

#include "../core/alloc.h"
#include "mpris.h"
#include "shell.h"

/* Logical px (from memory of Windows 7 at 96 dpi). */
enum {
    PAD = 6,        /* the panel's edge to the tiles */
    GAP = 2,        /* between tiles */
    TILE_W = 208,   /* one window's tile */
    IMG_W = 192,    /* the largest picture */
    IMG_H = 120,
    TITLE_H = 22,   /* the title band at a tile's top */
    TILE_H = TITLE_H + IMG_H + 12,
    CLOSE_W = 22,
    CLOSE_H = 18,
    ROW_H = 26,     /* the list of titles */
    LIST_W = 300,
    TOOLBAR_H = 30, /* a media player's buttons, under its picture */
    TBTN_W = 30,
    TBTN_H = 24,
    TBTN_COUNT = 3, /* Previous, Play/Pause, Next */
};

static constexpr int SHOW_MS = 400;     /* hover before they show (Windows' hover time) */
static constexpr int HIDE_MS = 400;     /* after the pointer leaves button and pictures */
static constexpr int PEEK_MS = 500;     /* hover on a picture before Aero Peek */
static constexpr int RECAPTURE_MS = 100; /* live pictures: at most ten a second */
static constexpr int RETRY_MS = 500;

/* An ext-foreign-toplevel-list handle and its identifier. */
typedef struct ext_handle {
    struct wl_list link; /* xd_thumbs.handles */
    struct ext_foreign_toplevel_handle_v1 *handle;
    char *identifier;
} ext_handle;

/* One window's picture, kept up to date while shown. */
typedef struct thumb {
    xd_thumbs *th;
    uint64_t window_id;
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    int want_w, want_h; /* the session's buffer size */
    bool argb;          /* it takes ARGB8888 shm buffers */
    bool stopped;
    xd_shm_ring ring;
    cairo_surface_t *pic; /* the last picture; nullptr until one came */
    guint again;
} thumb;

struct xd_thumbs {
    xd_shell *shell;
    struct ext_foreign_toplevel_list_v1 *list;
    struct ext_foreign_toplevel_image_capture_source_manager_v1 *sources;
    struct ext_image_copy_capture_manager_v1 *copier;
    struct wl_list handles;

    /* The button the pointer rests on, waiting for SHOW_MS. */
    guint show_timer, hide_timer, peek_timer;
    xd_taskbar *want_bar;
    char *want_key;
    uint64_t want_window;

    /* The open popup. */
    bool open;
    xd_taskbar *bar;
    char *key;
    uint64_t window_id; /* a window's own button: just it */
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional;
    xd_shm_ring ring;
    int width, height; /* logical */
    double scale;
    bool configured, dirty;
    thumb **items;
    size_t count;
    bool titles; /* titles only, no pictures */
    bool side;   /* stacked down beside a side taskbar */
    int hot, pressed;
    bool hot_close, pressed_close;
    /* The media players (from the first time pictures showed), the one
     * tile's player's bus name (or nullptr), and its button under the
     * pointer and pressed (-1: none). */
    xd_mpris *mpris;
    char *player;
    int hot_button, pressed_button;
    uint64_t peeking; /* the window peeked at, or 0 */
    uint64_t peek_next;
};

/* ---- ext-foreign-toplevel-list ---------------------------------------------------- */

static void handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
    ext_handle *e = data;
    wl_list_remove(&e->link);
    ext_foreign_toplevel_handle_v1_destroy(h);
    g_free(e->identifier);
    g_free(e);
}

static void handle_done([[maybe_unused]] void *data,
                        [[maybe_unused]] struct ext_foreign_toplevel_handle_v1 *h) {}
static void handle_title([[maybe_unused]] void *data,
                         [[maybe_unused]] struct ext_foreign_toplevel_handle_v1 *h,
                         [[maybe_unused]] const char *title) {}
static void handle_app_id([[maybe_unused]] void *data,
                          [[maybe_unused]] struct ext_foreign_toplevel_handle_v1 *h,
                          [[maybe_unused]] const char *app_id) {}

static void handle_identifier(void *data, [[maybe_unused]] struct ext_foreign_toplevel_handle_v1 *h,
                              const char *identifier) {
    ext_handle *e = data;
    g_free(e->identifier);
    e->identifier = g_strdup(identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
    .closed = handle_closed,
    .done = handle_done,
    .title = handle_title,
    .app_id = handle_app_id,
    .identifier = handle_identifier,
};

static void list_toplevel(void *data, [[maybe_unused]] struct ext_foreign_toplevel_list_v1 *l,
                          struct ext_foreign_toplevel_handle_v1 *h) {
    xd_thumbs *th = data;
    ext_handle *e = g_new0(ext_handle, 1);
    e->handle = h;
    wl_list_insert(&th->handles, &e->link);
    ext_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, e);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *l) {
    xd_thumbs *th = data;
    ext_foreign_toplevel_list_v1_destroy(l);
    th->list = nullptr;
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel = list_toplevel,
    .finished = list_finished,
};

static ext_handle *handle_for(xd_thumbs *th, const char *identifier) {
    if (identifier == nullptr)
        return nullptr;
    XD_LIST_FOR_EACH(e, &th->handles, ext_handle, link) {
        if (e->identifier != nullptr && strcmp(e->identifier, identifier) == 0)
            return e;
    }
    return nullptr;
}

bool xd_thumbs_bind(xd_shell *shell, struct wl_registry *reg, uint32_t name, const char *iface,
                    [[maybe_unused]] uint32_t version) {
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr)
        return false;
    if (strcmp(iface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        th->list = wl_registry_bind(reg, name, &ext_foreign_toplevel_list_v1_interface, 1);
        ext_foreign_toplevel_list_v1_add_listener(th->list, &list_listener, th);
    } else if (strcmp(iface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) ==
               0) {
        th->sources = wl_registry_bind(
            reg, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(iface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        th->copier = wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface, 1);
    } else {
        return false;
    }
    return true;
}

static bool can_capture(const xd_shell *shell) {
    const xd_thumbs *th = shell->thumbs;
    return th != nullptr && th->list != nullptr && th->sources != nullptr &&
           th->copier != nullptr && shell->arranger != nullptr &&
           xd_window_arranger_v1_get_version(shell->arranger) >=
               XD_WINDOW_ARRANGER_V1_IDENTIFY_SINCE_VERSION;
}

bool xd_thumbs_cover(const xd_shell *shell, size_t windows) {
    return windows > 0 && (windows > 1 || can_capture(shell));
}

void xd_thumbs_identifier(xd_shell *shell, struct zwlr_foreign_toplevel_handle_v1 *handle,
                          const char *identifier) {
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        if (tl->handle == handle) {
            g_free(tl->identifier);
            tl->identifier = g_strdup(identifier);
        }
    }
}

static xd_toplevel *toplevel_by_id(xd_shell *shell, uint64_t id) {
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        if (tl->id == id)
            return tl;
    }
    return nullptr;
}

/* ---- capturing ---------------------------------------------------------------------- */

static void capture(thumb *t);

static void stop_capture(thumb *t) {
    if (t->frame != nullptr)
        ext_image_copy_capture_frame_v1_destroy(t->frame);
    if (t->session != nullptr)
        ext_image_copy_capture_session_v1_destroy(t->session);
    if (t->source != nullptr)
        ext_image_capture_source_v1_destroy(t->source);
    t->frame = nullptr;
    t->session = nullptr;
    t->source = nullptr;
}

static gboolean capture_again(gpointer data) {
    thumb *t = data;
    t->again = 0;
    capture(t);
    return G_SOURCE_REMOVE;
}

static void later(thumb *t, int ms) {
    if (t->again == 0)
        t->again = g_timeout_add((guint)ms, capture_again, t);
}

static void frame_transform([[maybe_unused]] void *data,
                            [[maybe_unused]] struct ext_image_copy_capture_frame_v1 *f,
                            [[maybe_unused]] uint32_t transform) {}
static void frame_damage([[maybe_unused]] void *data,
                         [[maybe_unused]] struct ext_image_copy_capture_frame_v1 *f,
                         [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                         [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}
static void frame_time([[maybe_unused]] void *data,
                       [[maybe_unused]] struct ext_image_copy_capture_frame_v1 *f,
                       [[maybe_unused]] uint32_t hi, [[maybe_unused]] uint32_t lo,
                       [[maybe_unused]] uint32_t nsec) {}

/* The picture is in the buffer: kept, and the next one asked for soon
 * (the compositor sends it when the window next changes). */
static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
    thumb *t = data;
    ext_image_copy_capture_frame_v1_destroy(f);
    t->frame = nullptr;
    xd_shm_buffer *buf = &t->ring.slots[0];
    if (buf->surface != nullptr) {
        cairo_surface_mark_dirty(buf->surface);
        if (t->pic == nullptr || cairo_image_surface_get_width(t->pic) != buf->width ||
            cairo_image_surface_get_height(t->pic) != buf->height) {
            if (t->pic != nullptr)
                cairo_surface_destroy(t->pic);
            t->pic = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, buf->width, buf->height);
        }
        cairo_t *cr = cairo_create(t->pic);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, buf->surface, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        t->th->dirty = true;
    }
    later(t, RECAPTURE_MS);
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
    thumb *t = data;
    ext_image_copy_capture_frame_v1_destroy(f);
    t->frame = nullptr;
    switch (reason) {
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
        t->stopped = true;
        stop_capture(t);
        break;
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
        capture(t); /* the window's new size came before this */
        break;
    default:
        later(t, RETRY_MS);
        break;
    }
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void capture(thumb *t) {
    xd_shell *shell = t->th->shell;
    if (t->stopped || t->session == nullptr || t->frame != nullptr || !t->argb ||
        t->want_w <= 0 || t->want_h <= 0)
        return;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&t->ring, shell->shm, t->want_w, t->want_h);
    if (buf == nullptr)
        return;
    t->frame = ext_image_copy_capture_session_v1_create_frame(t->session);
    ext_image_copy_capture_frame_v1_add_listener(t->frame, &frame_listener, t);
    ext_image_copy_capture_frame_v1_attach_buffer(t->frame, buf->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(t->frame, 0, 0, t->want_w, t->want_h);
    ext_image_copy_capture_frame_v1_capture(t->frame);
}

static void session_size(void *data, [[maybe_unused]] struct ext_image_copy_capture_session_v1 *s,
                         uint32_t w, uint32_t h) {
    thumb *t = data;
    t->want_w = (int)XD_MIN(w, 4096u);
    t->want_h = (int)XD_MIN(h, 4096u);
    t->argb = false;
}

static void session_shm_format(void *data,
                               [[maybe_unused]] struct ext_image_copy_capture_session_v1 *s,
                               uint32_t format) {
    thumb *t = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        t->argb = true;
}

static void session_dmabuf_device([[maybe_unused]] void *data,
                                  [[maybe_unused]] struct ext_image_copy_capture_session_v1 *s,
                                  [[maybe_unused]] struct wl_array *device) {}
static void session_dmabuf_format([[maybe_unused]] void *data,
                                  [[maybe_unused]] struct ext_image_copy_capture_session_v1 *s,
                                  [[maybe_unused]] uint32_t format,
                                  [[maybe_unused]] struct wl_array *modifiers) {}

static void session_done(void *data, [[maybe_unused]] struct ext_image_copy_capture_session_v1 *s) {
    thumb *t = data;
    if (t->again == 0)
        capture(t);
}

static void session_stopped(void *data,
                            [[maybe_unused]] struct ext_image_copy_capture_session_v1 *s) {
    thumb *t = data;
    t->stopped = true;
    stop_capture(t);
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_size,
    .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device,
    .dmabuf_format = session_dmabuf_format,
    .done = session_done,
    .stopped = session_stopped,
};

static thumb *thumb_new(xd_thumbs *th, xd_toplevel *tl) {
    thumb *t = XD_NEW(thumb);
    t->th = th;
    t->window_id = tl->id;
    ext_handle *e = can_capture(th->shell) ? handle_for(th, tl->identifier) : nullptr;
    if (e != nullptr) {
        t->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
            th->sources, e->handle);
        t->session = ext_image_copy_capture_manager_v1_create_session(th->copier, t->source, 0);
        ext_image_copy_capture_session_v1_add_listener(t->session, &session_listener, t);
    }
    return t;
}

static void thumb_free(thumb *t) {
    if (t->again != 0)
        g_source_remove(t->again);
    stop_capture(t);
    xd_shm_ring_finish(&t->ring);
    if (t->pic != nullptr)
        cairo_surface_destroy(t->pic);
    xd_free(t);
}

/* ---- layout ------------------------------------------------------------------------- */

static int tile_h(const xd_thumbs *th) { return TILE_H + (th->player != nullptr ? TOOLBAR_H : 0); }

static xd_rect tile_rect(const xd_thumbs *th, size_t i) {
    if (th->titles)
        return (xd_rect){PAD, PAD + (int)i * ROW_H, th->width - 2 * PAD, ROW_H};
    if (th->side)
        return (xd_rect){PAD, PAD + (int)i * (tile_h(th) + GAP), TILE_W, tile_h(th)};
    return (xd_rect){PAD + (int)i * (TILE_W + GAP), PAD, TILE_W, tile_h(th)};
}

/* A media player's button k, in a row centred under the picture. */
static xd_rect button_of(xd_rect tile, int k) {
    int row = TBTN_COUNT * TBTN_W + (TBTN_COUNT - 1) * GAP;
    return (xd_rect){tile.x + (tile.w - row) / 2 + k * (TBTN_W + GAP),
                     tile.y + TITLE_H + 4 + IMG_H + 4 + (TOOLBAR_H - TBTN_H) / 2, TBTN_W, TBTN_H};
}

static xd_rect close_rect(xd_rect tile) {
    return (xd_rect){tile.x + tile.w - CLOSE_W - 4, tile.y + 3, CLOSE_W, CLOSE_H};
}

/* Logical width there is across the taskbar's output. */
static int room_across(const xd_taskbar *bar) {
    if (bar->desktop != nullptr && bar->desktop->configured)
        return bar->desktop->width;
    return XD_MAX(bar->width, 640);
}

/* Logical height there is down the taskbar's output. */
static int room_down(const xd_taskbar *bar) {
    if (bar->desktop != nullptr && bar->desktop->configured)
        return bar->desktop->height;
    return XD_MAX(bar->height, 480);
}

/* The one window's media player, when the program has just that window
 * and one player of its own. */
static void find_player(xd_thumbs *th) {
    g_free(th->player);
    th->player = nullptr;
    xd_toplevel *wins[2];
    if (th->mpris == nullptr || th->count != 1 ||
        xd_superbar_windows(th->shell, th->key, 0, wins, XD_ARRAY_LEN(wins)) != 1)
        return;
    th->player = g_strdup(xd_mpris_player_of(th->mpris, wins[0]->desktop_id, wins[0]->app_id));
}

static void measure(xd_thumbs *th) {
    size_t n = XD_MAX(th->count, 1);
    th->side = th->shell->settings.edge == XD_EDGE_LEFT ||
               th->shell->settings.edge == XD_EDGE_RIGHT;
    find_player(th);
    int tiles = (int)n * (th->side ? tile_h(th) : TILE_W) + ((int)n - 1) * GAP + 2 * PAD;
    th->titles = !can_capture(th->shell) ||
                 tiles > (th->side ? room_down(th->bar) : room_across(th->bar));
    if (th->titles) {
        g_free(th->player);
        th->player = nullptr;
        th->width = LIST_W;
        th->height = 2 * PAD + (int)n * ROW_H;
    } else if (th->side) {
        th->width = 2 * PAD + TILE_W;
        th->height = tiles;
    } else {
        th->width = tiles;
        th->height = 2 * PAD + tile_h(th);
    }
}

/* ---- drawing ------------------------------------------------------------------------ */

static int dev(double v, double s) { return (int)lround(v * s); }

static xd_rect to_device(xd_rect r, double s) {
    int x = dev(r.x, s), y = dev(r.y, s);
    return (xd_rect){x, y, dev(r.x + r.w, s) - x, dev(r.y + r.h, s) - y};
}

static cairo_surface_t *window_icon(xd_shell *shell, const xd_toplevel *tl, int px) {
    const char *names[] = {tl->icon, tl->app_id, "application-x-executable"};
    return xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names), px);
}

static void paint_icon(cairo_t *cr, cairo_surface_t *icon, int x, int y) {
    if (icon == nullptr)
        return;
    cairo_set_source_surface(cr, icon, x, y);
    cairo_paint(cr);
}

static const char *title_of(const xd_toplevel *tl) {
    if (tl->title != nullptr && tl->title[0] != '\0')
        return tl->title;
    return tl->app_id != nullptr ? tl->app_id : "";
}

/* A media button's symbol, white as on Windows Media Player's: skip back
 * (a bar and two triangles), play (a triangle) or pause (two bars), skip
 * forward. */
static void draw_media_glyph(cairo_t *cr, xd_rect r, int k, bool playing, double s,
                             bool enabled) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double h = 10 * s, w = 7 * s;
    cairo_save(cr);
    cairo_new_path(cr);
    if (k == 1 && playing) {
        double bw = 3 * s, gap = 3 * s;
        cairo_rectangle(cr, cx - gap / 2 - bw, cy - h / 2, bw, h);
        cairo_rectangle(cr, cx + gap / 2, cy - h / 2, bw, h);
    } else if (k == 1) {
        cairo_move_to(cr, cx - w / 2, cy - h / 2);
        cairo_line_to(cr, cx + w / 2 + s, cy);
        cairo_line_to(cr, cx - w / 2, cy + h / 2);
        cairo_close_path(cr);
    } else {
        /* Two triangles and a bar, pointing the way it skips. */
        double dir = k == 2 ? 1 : -1, tw = 5 * s, bar = 2 * s;
        double x0 = cx - dir * (tw + bar / 2);
        for (int j = 0; j < 2; j++) {
            double bx = x0 + dir * j * tw;
            cairo_move_to(cr, bx, cy - h / 2);
            cairo_line_to(cr, bx + dir * tw, cy);
            cairo_line_to(cr, bx, cy + h / 2);
            cairo_close_path(cr);
        }
        double barx = x0 + dir * 2 * tw;
        cairo_rectangle(cr, k == 2 ? barx : barx - bar, cy - h / 2, bar, h);
    }
    cairo_set_source_rgba(cr, 0, 0, 0, enabled ? 0.55 : 0.25);
    cairo_set_line_width(cr, 1.5 * s);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, enabled ? 1.0 : 0.4);
    cairo_fill(cr);
    cairo_restore(cr);
}

static bool button_enabled(const xd_mpris_state *st, int k) {
    return k == 0 ? st->can_previous : k == 1 ? st->can_play_pause : st->can_next;
}

static void draw_toolbar(xd_thumbs *th, const xd_theme *t, cairo_t *cr, xd_rect tile) {
    xd_mpris_state st;
    if (!xd_mpris_state_of(th->mpris, th->player, &st))
        return;
    for (int k = 0; k < TBTN_COUNT; k++) {
        xd_rect r = to_device(button_of(tile, k), th->scale);
        bool enabled = button_enabled(&st, k);
        uint8_t state = 0;
        if (enabled && th->hot_button == k)
            state |= XD_STATE_HOT;
        if (enabled && th->hot_button == k && th->pressed_button == k)
            state |= XD_STATE_PRESSED;
        xd_theme_draw_thumb_tile(t, cr, r, state);
        draw_media_glyph(cr, r, k, st.playing, th->scale, enabled);
    }
}

static void draw_tile(xd_thumbs *th, const xd_theme *t, cairo_t *cr, size_t i) {
    xd_shell *shell = th->shell;
    double s = th->scale;
    xd_toplevel *tl = toplevel_by_id(shell, th->items[i]->window_id);
    if (tl == nullptr)
        return;
    xd_rect lr = tile_rect(th, i);
    xd_rect r = to_device(lr, s);
    bool hot = th->hot == (int)i;
    uint8_t state = 0;
    if (hot)
        state |= XD_STATE_HOT;
    if (hot && th->pressed == (int)i && !th->pressed_close)
        state |= XD_STATE_PRESSED;
    xd_theme_draw_thumb_tile(t, cr, r, state);
    int icon = xd_tpx(t, 16);
    int title_h = dev(th->titles ? ROW_H : TITLE_H, s);
    paint_icon(cr, window_icon(shell, tl, icon), r.x + xd_tpx(t, 8), r.y + (title_h - icon) / 2);
    int tx = r.x + xd_tpx(t, 8) + icon + xd_tpx(t, 6);
    int tr = hot && !th->titles ? to_device(close_rect(lr), s).x - xd_tpx(t, 4)
                              : r.x + r.w - xd_tpx(t, 6);
    xd_theme_draw_thumb_title(t, cr, (xd_rect){tx, r.y, tr - tx, title_h}, title_of(tl));
    if (th->titles)
        return;
    if (hot) {
        uint8_t cs = th->hot_close ? XD_STATE_HOT : 0;
        if (th->hot_close && th->pressed == (int)i && th->pressed_close)
            cs |= XD_STATE_PRESSED;
        xd_theme_draw_thumb_close(t, cr, to_device(close_rect(lr), s), cs);
    }
    /* The picture, fitted to the box below the title, keeping its shape. */
    xd_rect box = to_device((xd_rect){lr.x + (TILE_W - IMG_W) / 2, lr.y + TITLE_H + 4, IMG_W,
                                      IMG_H},
                            s);
    if (th->player != nullptr)
        draw_toolbar(th, t, cr, lr);
    cairo_surface_t *pic = th->items[i]->pic;
    if (pic == nullptr) {
        /* Nothing yet (or no capture): its icon, large. */
        int big = xd_tpx(t, 48);
        paint_icon(cr, window_icon(shell, tl, big), box.x + (box.w - big) / 2,
                   box.y + (box.h - big) / 2);
        return;
    }
    double pw = cairo_image_surface_get_width(pic), ph = cairo_image_surface_get_height(pic);
    double k = fmin(box.w / pw, box.h / ph);
    double w = pw * k, h = ph * k;
    cairo_save(cr);
    cairo_translate(cr, box.x + (box.w - w) / 2, box.y + (box.h - h) / 2);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_clip(cr);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, pic, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void render(xd_thumbs *th) {
    xd_shell *shell = th->shell;
    double fw = ceil(th->width * th->scale), fh = ceil(th->height * th->scale);
    int dw = (int)fw, dh = (int)fh;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&th->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, th->scale);
    cairo_t *cr = cairo_create(buf->surface);
    xd_theme_draw_thumb_panel(&t, cr, (xd_rect){0, 0, dw, dh});
    for (size_t i = 0; i < th->count; i++)
        draw_tile(th, &t, cr, i);
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(th->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(th->surface, 0, 0, dw, dh);
    if (th->viewport != nullptr)
        wp_viewport_set_destination(th->viewport, th->width, th->height);
    wl_surface_commit(th->surface);
    buf->busy = true;
    th->dirty = false;
}

void xd_thumbs_render_dirty(xd_shell *shell) {
    xd_thumbs *th = shell->thumbs;
    if (th != nullptr && th->open && th->configured && th->dirty)
        render(th);
}

/* ---- the popup ---------------------------------------------------------------------- */

static void xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    xd_thumbs *th = data;
    xdg_surface_ack_configure(s, serial);
    th->configured = true;
    th->dirty = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *p,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *p) {
    xd_thumbs_close(((xd_thumbs *)data)->shell);
}

static void popup_repositioned([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *p,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static void preferred_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *f,
                            uint32_t scale_120) {
    xd_thumbs *th = data;
    double scale = scale_120 / 120.0;
    if (scale >= 0.5 && scale <= 8.0 && scale != th->scale) {
        th->scale = scale;
        th->dirty = true;
    }
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = preferred_scale,
};

static void destroy_surface(xd_thumbs *th) {
    if (th->surface == nullptr)
        return;
    if (th->shell->pointer_thumbs)
        th->shell->pointer_thumbs = false;
    xdg_popup_destroy(th->popup);
    xdg_surface_destroy(th->xdg_surface);
    if (th->viewport != nullptr)
        wp_viewport_destroy(th->viewport);
    if (th->fractional != nullptr)
        wp_fractional_scale_v1_destroy(th->fractional);
    wl_surface_destroy(th->surface);
    xd_shm_ring_finish(&th->ring);
    th->surface = nullptr;
    th->xdg_surface = nullptr;
    th->popup = nullptr;
    th->viewport = nullptr;
    th->fractional = nullptr;
    th->configured = false;
}

/* The button showing the open thumbnails (device px), if it's still there. */
static bool button_rect(xd_thumbs *th, xd_rect *out) {
    xd_taskbar *bar = th->bar;
    for (size_t i = 0; i < bar->task_count; i++) {
        uint64_t wid = 0;
        const char *key = xd_superbar_key(bar, (int)i, &wid);
        if (key != nullptr && strcmp(key, th->key) == 0 && wid == th->window_id) {
            *out = bar->task_rects[i];
            return true;
        }
    }
    return false;
}

/* Above the button, centred on it (beside it on a side taskbar). */
static void create_surface(xd_thumbs *th) {
    xd_shell *shell = th->shell;
    xd_taskbar *bar = th->bar;
    xd_rect r;
    if (!button_rect(th, &r))
        return;
    xd_rect a = {(int)(r.x / bar->scale), (int)(r.y / bar->scale), (int)(r.w / bar->scale),
                 (int)(r.h / bar->scale)};
    th->scale = bar->scale;
    th->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        th->fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(shell->fractional_manager,
                                                                th->surface);
        wp_fractional_scale_v1_add_listener(th->fractional, &fractional_listener, th);
        th->viewport = wp_viewporter_get_viewport(shell->viewporter, th->surface);
    } else {
        th->scale = 1.0;
    }
    th->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, th->surface);
    xdg_surface_add_listener(th->xdg_surface, &xdg_surface_listener, th);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, th->width, th->height);
    xdg_positioner_set_anchor_rect(pos, a.x, a.y, XD_MAX(a.w, 1), XD_MAX(a.h, 1));
    uint32_t anchor = XDG_POSITIONER_ANCHOR_TOP, gravity = XDG_POSITIONER_GRAVITY_TOP;
    uint32_t adjust = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
    int dx = 0, dy = -2;
    switch (shell->settings.edge) {
    case XD_EDGE_TOP:
        anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
        gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
        dy = 2;
        break;
    case XD_EDGE_LEFT:
        anchor = XDG_POSITIONER_ANCHOR_RIGHT;
        gravity = XDG_POSITIONER_GRAVITY_RIGHT;
        dx = 2;
        dy = 0;
        break;
    case XD_EDGE_RIGHT:
        anchor = XDG_POSITIONER_ANCHOR_LEFT;
        gravity = XDG_POSITIONER_GRAVITY_LEFT;
        dx = -2;
        dy = 0;
        break;
    case XD_EDGE_BOTTOM:
        break;
    }
    xdg_positioner_set_anchor(pos, anchor);
    xdg_positioner_set_gravity(pos, gravity);
    xdg_positioner_set_offset(pos, dx, dy);
    xdg_positioner_set_constraint_adjustment(pos, adjust);
    th->popup = xdg_surface_get_popup(th->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(bar->layer, th->popup);
    xdg_popup_add_listener(th->popup, &popup_listener, th);
    wl_surface_commit(th->surface);
}

static void set_peek(xd_thumbs *th, uint64_t window_id) {
    xd_shell *shell = th->shell;
    if (th->peek_timer != 0) {
        g_source_remove(th->peek_timer);
        th->peek_timer = 0;
    }
    if (th->peeking == window_id)
        return;
    th->peeking = window_id;
    if (shell->arranger == nullptr || xd_window_arranger_v1_get_version(shell->arranger) <
                                          XD_WINDOW_ARRANGER_V1_PEEK_WINDOW_SINCE_VERSION)
        return;
    xd_toplevel *tl = window_id != 0 ? toplevel_by_id(shell, window_id) : nullptr;
    xd_window_arranger_v1_peek_window(shell->arranger, tl != nullptr ? tl->handle : nullptr);
}

static gboolean peek_now(gpointer data) {
    xd_thumbs *th = data;
    th->peek_timer = 0;
    set_peek(th, th->peek_next);
    return G_SOURCE_REMOVE;
}

static void stop_timer(guint *id) {
    if (*id != 0)
        g_source_remove(*id);
    *id = 0;
}

void xd_thumbs_close(xd_shell *shell) {
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr)
        return;
    stop_timer(&th->show_timer);
    stop_timer(&th->hide_timer);
    if (!th->open)
        return;
    set_peek(th, 0);
    destroy_surface(th);
    for (size_t i = 0; i < th->count; i++)
        thumb_free(th->items[i]);
    XD_FREE(&th->items);
    th->count = 0;
    g_free(th->key);
    th->key = nullptr;
    g_free(th->player);
    th->player = nullptr;
    th->open = false;
    th->hot = th->pressed = -1;
    th->hot_button = th->pressed_button = -1;
}

/* Brings the tiles in line with the button's windows: pictures of the
 * ones still there are kept. Returns false if there are none left. */
static bool sync_windows(xd_thumbs *th) {
    xd_toplevel *wins[64];
    size_t n = xd_superbar_windows(th->shell, th->key, th->window_id, wins, XD_ARRAY_LEN(wins));
    thumb **items = xd_alloc_array(XD_MAX(n, 1), sizeof(*items));
    bool same = n == th->count;
    for (size_t i = 0; i < n; i++) {
        items[i] = nullptr;
        for (size_t k = 0; k < th->count; k++) {
            if (th->items[k] != nullptr && th->items[k]->window_id == wins[i]->id) {
                items[i] = th->items[k];
                th->items[k] = nullptr;
                same = same && k == i;
            }
        }
        if (items[i] == nullptr) {
            items[i] = thumb_new(th, wins[i]);
            same = false;
        }
    }
    for (size_t k = 0; k < th->count; k++) {
        if (th->items[k] != nullptr)
            thumb_free(th->items[k]);
    }
    xd_free(th->items);
    th->items = items;
    th->count = n;
    if (!same) {
        th->hot = th->pressed = -1;
        th->hot_close = false;
    }
    return n > 0;
}

static void players_changed(void *data) { xd_thumbs_refresh(((xd_thumbs *)data)->shell); }

static void open_for(xd_thumbs *th, xd_taskbar *bar, const char *key, uint64_t window_id) {
    xd_thumbs_close(th->shell);
    /* The media players are followed from the first time pictures show
     * (the Aero taskbar's), not in the other styles. */
    if (th->mpris == nullptr && can_capture(th->shell))
        th->mpris = xd_mpris_new(players_changed, th);
    xd_tooltip_hide(th->shell);
    th->open = true;
    th->bar = bar;
    th->key = g_strdup(key);
    th->window_id = window_id;
    th->hot = th->pressed = -1;
    if (!sync_windows(th)) {
        xd_thumbs_close(th->shell);
        return;
    }
    measure(th);
    create_surface(th);
    if (th->surface == nullptr)
        xd_thumbs_close(th->shell);
}

void xd_thumbs_refresh(xd_shell *shell) {
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr || !th->open)
        return;
    if (!sync_windows(th)) {
        xd_thumbs_close(shell);
        return;
    }
    int w = th->width, h = th->height;
    measure(th);
    th->dirty = true;
    if (w != th->width || h != th->height) {
        /* Another size: a new popup, placed again over the button. */
        destroy_surface(th);
        create_surface(th);
        if (th->surface == nullptr)
            xd_thumbs_close(shell);
    }
}

/* ---- hovering the taskbar ----------------------------------------------------------- */

static gboolean show_now(gpointer data) {
    xd_thumbs *th = data;
    th->show_timer = 0;
    if (th->want_bar != nullptr && th->want_key != nullptr)
        open_for(th, th->want_bar, th->want_key, th->want_window);
    return G_SOURCE_REMOVE;
}

static gboolean hide_now(gpointer data) {
    xd_thumbs *th = data;
    th->hide_timer = 0;
    xd_thumbs_close(th->shell);
    return G_SOURCE_REMOVE;
}

static void arm_hide(xd_thumbs *th) {
    if (th->open && th->hide_timer == 0)
        th->hide_timer = g_timeout_add(HIDE_MS, hide_now, th);
}

void xd_thumbs_hover(xd_taskbar *bar, int hit) {
    xd_shell *shell = bar->shell;
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr)
        return;
    uint64_t wid = 0;
    const char *key = xd_superbar_key(bar, hit, &wid);
    xd_toplevel *wins[2];
    size_t n = key != nullptr ? xd_superbar_windows(shell, key, wid, wins, XD_ARRAY_LEN(wins)) : 0;
    bool busy = shell->start_menu != nullptr || shell->start7 != nullptr ||
                bar->drag_key != nullptr || bar->pressed != XD_HIT_NONE;
    if (key == nullptr || busy || !xd_thumbs_cover(shell, n)) {
        stop_timer(&th->show_timer);
        if (busy && th->open)
            xd_thumbs_close(shell);
        else
            arm_hide(th);
        return;
    }
    stop_timer(&th->hide_timer);
    if (th->open && th->bar == bar && wid == th->window_id && strcmp(key, th->key) == 0)
        return;
    g_free(th->want_key);
    th->want_key = g_strdup(key);
    th->want_bar = bar;
    th->want_window = wid;
    stop_timer(&th->show_timer);
    if (th->open) /* moving along the taskbar: at once */
        open_for(th, bar, key, wid);
    else
        th->show_timer = g_timeout_add(SHOW_MS, show_now, th);
}

void xd_thumbs_bar_gone(xd_shell *shell, xd_taskbar *bar) {
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr)
        return;
    if (th->want_bar == bar) {
        stop_timer(&th->show_timer);
        th->want_bar = nullptr;
    }
    if (th->open && th->bar == bar)
        xd_thumbs_close(shell);
}

/* ---- the pointer on the pictures ---------------------------------------------------- */

bool xd_thumbs_owns(xd_shell *shell, struct wl_surface *surface) {
    xd_thumbs *th = shell->thumbs;
    return th != nullptr && th->open && surface != nullptr && th->surface == surface;
}

static bool inside(xd_rect r, double x, double y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void xd_thumbs_pointer_motion(xd_shell *shell, double x, double y) {
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr || !th->open)
        return;
    stop_timer(&th->hide_timer);
    int hot = -1, hot_button = -1;
    bool close = false;
    for (size_t i = 0; i < th->count; i++) {
        xd_rect r = tile_rect(th, i);
        if (inside(r, x, y)) {
            hot = (int)i;
            close = !th->titles && inside(close_rect(r), x, y);
            for (int k = 0; th->player != nullptr && k < TBTN_COUNT; k++) {
                if (inside(button_of(r, k), x, y))
                    hot_button = k;
            }
        }
    }
    if (hot != th->hot || close != th->hot_close || hot_button != th->hot_button) {
        th->hot = hot;
        th->hot_close = close;
        th->hot_button = hot_button;
        th->dirty = true;
    }
    /* Aero Peek at the hovered window: after a moment, then at once as
     * the pointer moves on to another. */
    uint64_t id = hot >= 0 ? th->items[hot]->window_id : 0;
    if (id == 0) {
        set_peek(th, 0);
    } else if (id != th->peeking && (th->peek_timer == 0 || th->peek_next != id)) {
        th->peek_next = id;
        stop_timer(&th->peek_timer);
        if (th->peeking != 0)
            set_peek(th, id);
        else
            th->peek_timer = g_timeout_add(PEEK_MS, peek_now, th);
    }
}

void xd_thumbs_pointer_leave(xd_shell *shell) {
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr || !th->open)
        return;
    set_peek(th, 0);
    th->hot = th->pressed = -1;
    th->hot_button = th->pressed_button = -1;
    th->hot_close = false;
    th->dirty = true;
    arm_hide(th);
}

void xd_thumbs_pointer_button(xd_shell *shell, uint32_t button, bool pressed) {
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr || !th->open)
        return;
    if (pressed) {
        th->pressed = th->hot;
        th->pressed_close = th->hot_close;
        th->pressed_button = button == BTN_LEFT ? th->hot_button : -1;
        th->dirty = true;
        return;
    }
    int target = th->pressed;
    bool on_close = th->pressed_close;
    int media = th->pressed_button;
    th->pressed = th->pressed_button = -1;
    th->dirty = true;
    /* A media player's button: the player is told, the pictures stay. */
    if (media >= 0) {
        xd_mpris_state st;
        static const char *const methods[TBTN_COUNT] = {"Previous", "PlayPause", "Next"};
        if (button == BTN_LEFT && media == th->hot_button &&
            xd_mpris_state_of(th->mpris, th->player, &st) && button_enabled(&st, media))
            xd_mpris_send(th->mpris, th->player, methods[media]);
        return;
    }
    if (target < 0 || target != th->hot || on_close != th->hot_close)
        return; /* released elsewhere */
    xd_toplevel *tl = toplevel_by_id(shell, th->items[target]->window_id);
    if (tl == nullptr)
        return;
    if (button == BTN_MIDDLE || (button == BTN_LEFT && on_close)) {
        /* Closing it: the popup stays for the others (refreshed when the
         * window goes). */
        set_peek(th, 0);
        zwlr_foreign_toplevel_handle_v1_close(tl->handle);
        return;
    }
    if (button != BTN_LEFT)
        return;
    /* Activating first: the peek then ends with it in front. */
    if (tl->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(tl->handle);
    if (shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(tl->handle, shell->seat);
    xd_thumbs_close(shell);
}

/* ---- lifetime ----------------------------------------------------------------------- */

void xd_thumbs_init(xd_shell *shell) {
    xd_thumbs *th = XD_NEW(xd_thumbs);
    th->shell = shell;
    th->hot = th->pressed = -1;
    th->hot_button = th->pressed_button = -1;
    th->scale = 1.0;
    wl_list_init(&th->handles);
    shell->thumbs = th;
}

void xd_thumbs_finish(xd_shell *shell) {
    xd_thumbs *th = shell->thumbs;
    if (th == nullptr)
        return;
    xd_thumbs_close(shell);
    stop_timer(&th->peek_timer);
    XD_LIST_FOR_EACH_SAFE(e, tmp, &th->handles, ext_handle, link) {
        wl_list_remove(&e->link);
        ext_foreign_toplevel_handle_v1_destroy(e->handle);
        g_free(e->identifier);
        g_free(e);
    }
    if (th->list != nullptr)
        ext_foreign_toplevel_list_v1_destroy(th->list);
    if (th->sources != nullptr)
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(th->sources);
    if (th->copier != nullptr)
        ext_image_copy_capture_manager_v1_destroy(th->copier);
    g_free(th->want_key);
    xd_mpris_free(th->mpris);
    xd_free(th);
    shell->thumbs = nullptr;
}
