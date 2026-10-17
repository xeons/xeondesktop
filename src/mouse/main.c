/* SPDX-License-Identifier: MIT
 * xd-mouse: Windows XP's Mouse Properties. Writes [Mouse] in desktop.ini;
 * the compositor applies it to every libinput pointer (see input.c).
 *
 *   Buttons          Switch primary and secondary buttons, double-click
 *                    speed with XP's test folder
 *   Pointers         the XCursor theme (with a preview of its cursors)
 *                    and size
 *   Pointer Options  speed, Enhance pointer precision (acceleration)
 *   Wheel            lines a notch scrolls
 *   Hardware         the pointing devices udev knows
 *
 * Not here: ClickLock (greyed: the compositor doesn't do it yet), and
 * XP's Snap To, pointer trails, hiding the pointer while typing and
 * showing it with Ctrl, which the compositor doesn't do either.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xcursor/Xcursor.h>

#include "../core/common.h"
#include "../cpl/inputdevs.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "cursors.h"

typedef struct props {
    GtkWidget *window, *apply;
    xd_mouse_settings mouse; /* what the widgets show */
    bool updating;           /* we're setting the widgets ourselves */
    /* Buttons */
    GtkWidget *switch_buttons, *picture, *folder, *double_click;
    bool folder_open;
    guint32 last_press; /* the test folder's last click */
    /* Pointers */
    GtkWidget *themes, *sizes, *roles, *big_preview;
    GPtrArray *theme_list; /* xd_cursor_theme */
    /* Pointer Options */
    GtkWidget *speed, *precision;
    /* Wheel */
    GtkWidget *lines;
} props;

static void changed(props *p) {
    if (!p->updating)
        gtk_widget_set_sensitive(p->apply, TRUE);
}

/* A slider's notch. */
static int range_int(GtkRange *r) {
    double v = gtk_range_get_value(r);
    return (int)lround(v);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *wrapped(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 46);
    return l;
}

/* "Slow [-----|-----] Fast" */
static GtkWidget *slider_row(const char *left, GtkWidget *scale, const char *right) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_size_request(scale, 160, -1);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(left), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(right), FALSE, FALSE, 0);
    return row;
}

static GtkWidget *ticked_scale(double lo, double hi, double step) {
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    for (double v = lo; v <= hi; v += step)
        gtk_scale_add_mark(GTK_SCALE(s), v, GTK_POS_BOTTOM, nullptr);
    gtk_range_set_round_digits(GTK_RANGE(s), 0);
    return s;
}

/* ---- Buttons ------------------------------------------------------------------- */

/* A mouse seen from above, its primary button lit, as XP draws it. */
static gboolean draw_mouse(GtkWidget *w, cairo_t *cr, gpointer data) {
    props *p = data;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double mw = 56, mh = 86, r = mw / 2;
    double x = floor((W - mw) / 2) + 0.5, y = floor((H - mh) / 2) + 0.5;
    double cx = x + r, seam = y + r + 8; /* where the buttons end */
    cairo_set_line_width(cr, 1);
    /* The body: round at both ends. */
    cairo_new_path(cr);
    cairo_arc(cr, cx, y + r, r, XD_PI, 2 * XD_PI);
    cairo_arc(cr, cx, y + mh - r, r, 0, XD_PI);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.96, 0.95, 0.92);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_stroke(cr);
    /* The primary button. */
    cairo_new_path(cr);
    if (!p->mouse.left_handed) {
        cairo_move_to(cr, cx, seam);
        cairo_line_to(cr, x, seam);
        cairo_arc(cr, cx, y + r, r, XD_PI, 1.5 * XD_PI);
    } else {
        cairo_move_to(cr, cx, seam);
        cairo_line_to(cr, cx, y);
        cairo_arc(cr, cx, y + r, r, 1.5 * XD_PI, 2 * XD_PI);
        cairo_line_to(cr, x + mw, seam);
    }
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.19, 0.42, 0.77);
    cairo_fill(cr);
    /* The seams between the buttons and the body. */
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_move_to(cr, cx, y);
    cairo_line_to(cr, cx, seam);
    cairo_move_to(cr, x, seam);
    cairo_line_to(cr, x + mw, seam);
    cairo_stroke(cr);
    return FALSE;
}

static void on_switch(GtkToggleButton *b, gpointer data) {
    props *p = data;
    p->mouse.left_handed = gtk_toggle_button_get_active(b);
    gtk_widget_queue_draw(p->picture);
    changed(p);
}

static void show_folder(props *p) {
    gtk_image_set_from_icon_name(GTK_IMAGE(p->folder), p->folder_open ? "folder-open" : "folder",
                                 GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(p->folder), 32);
}

/* The test folder opens or closes on a double-click at the speed the
 * slider shows, before it's applied. */
static gboolean on_folder_press([[maybe_unused]] GtkWidget *w, GdkEventButton *e, gpointer data) {
    props *p = data;
    if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_PRIMARY)
        return TRUE;
    if (p->last_press != 0 && e->time - p->last_press <= (guint32)p->mouse.double_click_ms) {
        p->folder_open = !p->folder_open;
        show_folder(p);
        p->last_press = 0;
    } else {
        p->last_press = e->time;
    }
    return TRUE;
}

static void on_double_click(GtkRange *r, gpointer data) {
    props *p = data;
    p->mouse.double_click_ms = range_int(r);
    changed(p);
}

static GtkWidget *buttons_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *config = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    p->switch_buttons =
        gtk_check_button_new_with_mnemonic("_Switch primary and secondary buttons");
    g_signal_connect(p->switch_buttons, "toggled", G_CALLBACK(on_switch), p);
    gtk_box_pack_start(GTK_BOX(left), p->switch_buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left),
                       wrapped("Select this check box to make the button on the right the one "
                               "you use for primary functions such as selecting and dragging."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(config), left, TRUE, TRUE, 0);
    p->picture = gtk_drawing_area_new();
    gtk_widget_set_size_request(p->picture, 80, 100);
    g_signal_connect(p->picture, "draw", G_CALLBACK(draw_mouse), p);
    gtk_box_pack_start(GTK_BOX(config), p->picture, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Button configuration", config), FALSE, FALSE, 0);

    GtkWidget *dbl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *dleft = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(dleft),
                       wrapped("Double-click the folder to test your setting. If the folder "
                               "does not open or close, try using a slower setting."),
                       FALSE, FALSE, 0);
    p->double_click = ticked_scale(XD_DOUBLE_CLICK_MIN, XD_DOUBLE_CLICK_MAX, 100);
    /* Slow on the left: the longest time. */
    gtk_range_set_inverted(GTK_RANGE(p->double_click), TRUE);
    g_signal_connect(p->double_click, "value-changed", G_CALLBACK(on_double_click), p);
    GtkWidget *speed_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *speed_label = gtk_label_new_with_mnemonic("Spee_d:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(speed_label), p->double_click);
    gtk_box_pack_start(GTK_BOX(speed_row), speed_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(speed_row), slider_row("Slow", p->double_click, "Fast"), TRUE,
                       TRUE, 0);
    gtk_box_pack_start(GTK_BOX(dleft), speed_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dbl), dleft, TRUE, TRUE, 0);
    GtkWidget *events = gtk_event_box_new();
    p->folder = gtk_image_new();
    show_folder(p);
    gtk_container_add(GTK_CONTAINER(events), p->folder);
    gtk_widget_set_size_request(events, 80, -1);
    gtk_widget_set_valign(events, GTK_ALIGN_CENTER);
    g_signal_connect(events, "button-press-event", G_CALLBACK(on_folder_press), p);
    gtk_box_pack_start(GTK_BOX(dbl), events, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Double-click speed", dbl), FALSE, FALSE, 0);

    /* ClickLock needs the compositor to hold a button down for the user;
     * it doesn't yet, so the option is shown off and greyed. */
    GtkWidget *lock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *lock_check = gtk_check_button_new_with_mnemonic("Turn on Click_Lock");
    gtk_widget_set_sensitive(lock_check, FALSE);
    gtk_box_pack_start(GTK_BOX(lock), lock_check, FALSE, FALSE, 0);
    GtkWidget *lock_text = wrapped("Enables you to highlight or drag without holding down the "
                                   "mouse button. Not available yet.");
    gtk_widget_set_sensitive(lock_text, FALSE);
    gtk_box_pack_start(GTK_BOX(lock), lock_text, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("ClickLock", lock), FALSE, FALSE, 0);
    return page;
}

/* ---- Pointers ------------------------------------------------------------------ */

/* XP's list of pointers, with the XCursor names that stand for each. */
static const struct {
    const char *title;
    const char *names[3];
} roles[] = {
    {"Normal Select", {"default", "left_ptr"}},
    {"Help Select", {"help", "question_arrow", "left_ptr_help"}},
    {"Working In Background", {"progress", "left_ptr_watch"}},
    {"Busy", {"wait", "watch"}},
    {"Precision Select", {"crosshair", "cross"}},
    {"Text Select", {"text", "xterm"}},
    {"Handwriting", {"pencil"}},
    {"Unavailable", {"not-allowed", "crossed_circle", "forbidden"}},
    {"Vertical Resize", {"ns-resize", "sb_v_double_arrow", "size_ver"}},
    {"Horizontal Resize", {"ew-resize", "sb_h_double_arrow", "size_hor"}},
    {"Diagonal Resize 1", {"nwse-resize", "size_fdiag", "bd_double_arrow"}},
    {"Diagonal Resize 2", {"nesw-resize", "size_bdiag", "fd_double_arrow"}},
    {"Move", {"move", "fleur", "all-scroll"}},
    {"Alternate Select", {"up-arrow", "center_ptr"}},
    {"Link Select", {"pointer", "hand2", "hand1"}},
};

/* One cursor of a theme as a cairo surface, or nullptr. libXcursor
 * follows the theme's Inherits= and picks the nearest size. */
static cairo_surface_t *load_cursor(const char *theme, int size, const char *const *names) {
    XcursorImage *img = nullptr;
    for (size_t i = 0; i < 3 && names[i] != nullptr && img == nullptr; i++)
        img = XcursorLibraryLoadImage(names[i], theme, size);
    if (img == nullptr)
        return nullptr;
    /* Both are premultiplied ARGB in native byte order. */
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)img->width,
                                                    (int)img->height);
    cairo_surface_flush(s);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (unsigned y = 0; y < img->height; y++)
        memcpy(dst + (size_t)y * (size_t)stride, img->pixels + (size_t)y * img->width,
               img->width * sizeof(XcursorPixel));
    cairo_surface_mark_dirty(s);
    XcursorImageDestroy(img);
    return s;
}

static GtkWidget *cursor_image(const char *theme, int size, const char *const *names) {
    cairo_surface_t *s = load_cursor(theme, size, names);
    GtkWidget *img = s != nullptr ? gtk_image_new_from_surface(s)
                                  : gtk_image_new_from_icon_name("image-missing",
                                                                 GTK_ICON_SIZE_BUTTON);
    if (s != nullptr)
        cairo_surface_destroy(s);
    return img;
}

/* The theme's cursors in the list, and its arrow large on the right. */
static void show_theme(props *p) {
    const char *theme = p->mouse.cursor_theme[0] != '\0' ? p->mouse.cursor_theme : "default";
    int size = p->mouse.cursor_size;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(p->roles));
    for (GList *l = rows; l != nullptr; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(rows);
    for (size_t i = 0; i < G_N_ELEMENTS(roles); i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(row), 2);
        GtkWidget *title = gtk_label_new(roles[i].title);
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_box_pack_start(GTK_BOX(row), title, TRUE, TRUE, 4);
        GtkWidget *img = cursor_image(theme, size, roles[i].names);
        gtk_widget_set_size_request(img, 48, 32);
        gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 4);
        gtk_list_box_insert(GTK_LIST_BOX(p->roles), row, -1);
    }
    gtk_widget_show_all(p->roles);
    GtkWidget *parent = gtk_widget_get_parent(p->big_preview);
    gtk_widget_destroy(p->big_preview);
    p->big_preview = cursor_image(theme, 48, roles[0].names);
    gtk_widget_set_size_request(p->big_preview, 64, 64);
    gtk_container_add(GTK_CONTAINER(parent), p->big_preview);
    gtk_widget_show(p->big_preview);
}

static void on_theme(GtkComboBox *c, gpointer data) {
    props *p = data;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id == nullptr)
        return;
    g_strlcpy(p->mouse.cursor_theme, id, sizeof(p->mouse.cursor_theme));
    show_theme(p);
    changed(p);
}

static void on_size(GtkComboBox *c, gpointer data) {
    props *p = data;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id == nullptr)
        return;
    p->mouse.cursor_size = atoi(id);
    show_theme(p);
    changed(p);
}

/* Normal, Large and Extra Large as in XP's schemes, plus the size the
 * file has if it's another. */
static void fill_sizes(GtkWidget *combo, int current) {
    static const struct {
        int size;
        const char *name;
    } sizes[] = {{24, "Normal"}, {32, "Large"}, {48, "Extra Large"}};
    bool listed = false;
    for (size_t i = 0; i < G_N_ELEMENTS(sizes); i++) {
        char id[16], text[64];
        g_snprintf(id, sizeof(id), "%d", sizes[i].size);
        g_snprintf(text, sizeof(text), "%s (%d pixels)", sizes[i].name, sizes[i].size);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, text);
        listed = listed || sizes[i].size == current;
    }
    if (!listed) {
        char id[16], text[64];
        g_snprintf(id, sizeof(id), "%d", current);
        g_snprintf(text, sizeof(text), "%d pixels", current);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, text);
    }
}

static GtkWidget *pointers_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    p->themes = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->themes), "", "(System default)");
    char **dirs = xd_cursor_theme_dirs();
    p->theme_list = xd_cursor_themes_list((const char *const *)dirs);
    g_strfreev(dirs);
    bool found = p->mouse.cursor_theme[0] == '\0';
    for (guint i = 0; i < p->theme_list->len; i++) {
        const xd_cursor_theme *t = g_ptr_array_index(p->theme_list, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->themes), t->dir, t->name);
        found = found || strcmp(t->dir, p->mouse.cursor_theme) == 0;
    }
    /* A theme the file names but that isn't installed (any more). */
    if (!found)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->themes), p->mouse.cursor_theme,
                                  p->mouse.cursor_theme);
    gtk_widget_set_hexpand(p->themes, TRUE);
    GtkWidget *scheme_label = gtk_label_new_with_mnemonic("Sch_eme:");
    gtk_label_set_xalign(GTK_LABEL(scheme_label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(scheme_label), p->themes);
    gtk_grid_attach(GTK_GRID(grid), scheme_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->themes, 1, 0, 1, 1);
    p->sizes = gtk_combo_box_text_new();
    fill_sizes(p->sizes, p->mouse.cursor_size);
    GtkWidget *size_label = gtk_label_new_with_mnemonic("Si_ze:");
    gtk_label_set_xalign(GTK_LABEL(size_label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(size_label), p->sizes);
    gtk_grid_attach(GTK_GRID(grid), size_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->sizes, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(top), frame("Scheme", grid), TRUE, TRUE, 0);
    GtkWidget *preview = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(preview), GTK_SHADOW_IN);
    p->big_preview = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(preview), p->big_preview);
    gtk_widget_set_valign(preview, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(top), preview, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *custom = gtk_label_new_with_mnemonic("_Customize:");
    gtk_label_set_xalign(GTK_LABEL(custom), 0);
    gtk_box_pack_start(GTK_BOX(page), custom, FALSE, FALSE, 0);
    p->roles = gtk_list_box_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(custom), p->roles);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 190);
    gtk_container_add(GTK_CONTAINER(scroll), p->roles);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    GtkWidget *note = wrapped("Programs started from now on use the new pointers; some keep "
                              "theirs until they're restarted.");
    gtk_box_pack_start(GTK_BOX(page), note, FALSE, FALSE, 0);

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->themes), p->mouse.cursor_theme);
    char size_id[16];
    g_snprintf(size_id, sizeof(size_id), "%d", p->mouse.cursor_size);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->sizes), size_id);
    show_theme(p);
    g_signal_connect(p->themes, "changed", G_CALLBACK(on_theme), p);
    g_signal_connect(p->sizes, "changed", G_CALLBACK(on_size), p);
    return page;
}

/* ---- Pointer Options ------------------------------------------------------------- */

static void on_speed(GtkRange *r, gpointer data) {
    props *p = data;
    p->mouse.speed = range_int(r);
    changed(p);
}

static void on_precision(GtkToggleButton *b, gpointer data) {
    props *p = data;
    p->mouse.acceleration = gtk_toggle_button_get_active(b);
    changed(p);
}

static GtkWidget *options_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *motion = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("Select a pointer _speed:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(motion), label, FALSE, FALSE, 0);
    p->speed = ticked_scale(XD_MOUSE_SPEED_MIN, XD_MOUSE_SPEED_MAX, 1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->speed);
    gtk_range_set_value(GTK_RANGE(p->speed), p->mouse.speed);
    g_signal_connect(p->speed, "value-changed", G_CALLBACK(on_speed), p);
    gtk_box_pack_start(GTK_BOX(motion), slider_row("Slow", p->speed, "Fast"), FALSE, FALSE, 0);
    p->precision = gtk_check_button_new_with_mnemonic("_Enhance pointer precision");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->precision), p->mouse.acceleration);
    g_signal_connect(p->precision, "toggled", G_CALLBACK(on_precision), p);
    gtk_box_pack_start(GTK_BOX(motion), p->precision, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(motion),
                       wrapped("With enhanced precision the pointer goes further when you "
                               "move the mouse quickly, and less far when you move it slowly."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Motion", motion), FALSE, FALSE, 0);
    return page;
}

/* ---- Wheel -------------------------------------------------------------------------- */

static void on_lines(GtkSpinButton *s, gpointer data) {
    props *p = data;
    p->mouse.wheel_lines = gtk_spin_button_get_value_as_int(s);
    changed(p);
}

static GtkWidget *wheel_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box), wrapped("Roll the wheel one notch to scroll:"), FALSE,
                       FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new_with_mnemonic("The following _number of lines at a time:");
    p->lines = gtk_spin_button_new_with_range(1, 30, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(p->lines), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->lines), p->mouse.wheel_lines);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->lines);
    g_signal_connect(p->lines, "value-changed", G_CALLBACK(on_lines), p);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 20);
    gtk_box_pack_start(GTK_BOX(row), p->lines, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    /* Clients decide how far one step goes; the compositor can only send
     * more or fewer steps. */
    gtk_box_pack_start(GTK_BOX(box),
                       wrapped("Programs scroll about 3 lines for a notch; other numbers "
                               "scroll proportionally more or less. Touchpads aren't "
                               "affected."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Scrolling", box), FALSE, FALSE, 0);
    return page;
}

/* ---- applying ----------------------------------------------------------------------- */

static bool apply(props *p) {
    /* Everything else in the file as it is now: other applets may have
     * changed it since we started. */
    xd_settings s;
    xd_settings_load(&s);
    s.mouse = p->mouse;
    if (!xd_settings_save(&s)) {
        xd_message_box(GTK_WINDOW(p->window), "Mouse Properties", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The settings could not be saved.");
        return false;
    }
    gtk_widget_set_sensitive(p->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !apply(p))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Mouse");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props p;
    xd_settings s;
    xd_settings_load(&s);
    p.mouse = s.mouse;

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), "Mouse Properties");
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "input-mouse");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    p.updating = true;
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), buttons_page(&p),
                             gtk_label_new_with_mnemonic("_Buttons"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), pointers_page(&p),
                             gtk_label_new_with_mnemonic("_Pointers"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), options_page(&p),
                             gtk_label_new_with_mnemonic("Pointer _Options"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), wheel_page(&p),
                             gtk_label_new_with_mnemonic("_Wheel"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), xd_input_devices_page(XD_INPUT_POINTERS),
                             gtk_label_new_with_mnemonic("_Hardware"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p.switch_buttons), p.mouse.left_handed);
    gtk_range_set_value(GTK_RANGE(p.double_click), p.mouse.double_click_ms);
    p.updating = false;
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));

    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_main();
    g_ptr_array_unref(p.theme_list);
    return EXIT_SUCCESS;
}
