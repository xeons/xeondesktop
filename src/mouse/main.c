/* SPDX-License-Identifier: MIT
 * xd-mouse: Windows XP's Mouse Properties. Writes [Mouse] in desktop.ini;
 * the compositor applies it to every libinput pointer (see input.c).
 *
 *   Buttons          Switch primary and secondary buttons, double-click
 *                    speed with XP's test folder, ClickLock (done by the
 *                    compositor, access.c)
 *   Pointers         the XCursor theme (with a preview of its cursors)
 *                    and size
 *   Pointer Options  speed, Enhance pointer precision (acceleration)
 *   Wheel            lines a notch scrolls
 *   Touchpad         [Touchpad]: tapping, scrolling, clicking, the
 *                    touchpad's own speed and the desktop's three-finger
 *                    swipes, as touchpad drivers' tabs added to XP's and
 *                    7's Mouse Properties; shown only with a touchpad
 *                    (or XD_SHOW_TOUCHPAD=1, for screenshots and tests)
 *   Hardware         the pointing devices udev knows
 *
 * Not here: XP's Snap To, pointer trails, hiding the pointer while typing and
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
    GtkWidget *switch_buttons, *picture, *folder, *double_click, *click_lock, *lock_settings;
    bool folder_open;
    guint32 last_press; /* the test folder's last click */
    /* Pointers */
    GtkWidget *themes, *sizes, *roles, *big_preview;
    GPtrArray *theme_list; /* xd_cursor_theme */
    /* Pointer Options */
    GtkWidget *speed, *precision;
    /* Wheel */
    GtkWidget *lines;
    /* Touchpad */
    GtkWidget *tap, *tap_drag, *drag_lock, *scroll, *natural, *scroll_speed, *click;
    GtkWidget *typing, *with_mouse, *pad_speed, *pad_precision, *gestures;
} props;

static void changed(props *properties) {
    if (!properties->updating)
        gtk_widget_set_sensitive(properties->apply, TRUE);
}

/* A slider's notch. */
static int range_int(GtkRange *range) {
    double value = gtk_range_get_value(range);
    return (int)lround(value);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(widget), child);
    return widget;
}

static GtkWidget *wrapped(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 46);
    return widget;
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
    GtkWidget *widget = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    for (double mark = lo; mark <= hi; mark += step)
        gtk_scale_add_mark(GTK_SCALE(widget), mark, GTK_POS_BOTTOM, nullptr);
    gtk_range_set_round_digits(GTK_RANGE(widget), 0);
    return widget;
}

/* ---- Buttons ------------------------------------------------------------------- */

/* A mouse seen from above, its primary button lit, as XP draws it. */
static gboolean draw_mouse(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    props *properties = data;
    double width = gtk_widget_get_allocated_width(widget),
           height = gtk_widget_get_allocated_height(widget);
    double mw = 56, mh = 86, radius = mw / 2;
    double x = floor((width - mw) / 2) + 0.5, y = floor((height - mh) / 2) + 0.5;
    double cx = x + radius, seam = y + radius + 8; /* where the buttons end */
    cairo_set_line_width(cairo, 1);
    /* The body: round at both ends. */
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, y + radius, radius, XD_PI, 2 * XD_PI);
    cairo_arc(cairo, cx, y + mh - radius, radius, 0, XD_PI);
    cairo_close_path(cairo);
    cairo_set_source_rgb(cairo, 0.96, 0.95, 0.92);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0.35, 0.35, 0.35);
    cairo_stroke(cairo);
    /* The primary button. */
    cairo_new_path(cairo);
    if (!properties->mouse.left_handed) {
        cairo_move_to(cairo, cx, seam);
        cairo_line_to(cairo, x, seam);
        cairo_arc(cairo, cx, y + radius, radius, XD_PI, 1.5 * XD_PI);
    } else {
        cairo_move_to(cairo, cx, seam);
        cairo_line_to(cairo, cx, y);
        cairo_arc(cairo, cx, y + radius, radius, 1.5 * XD_PI, 2 * XD_PI);
        cairo_line_to(cairo, x + mw, seam);
    }
    cairo_close_path(cairo);
    cairo_set_source_rgb(cairo, 0.19, 0.42, 0.77);
    cairo_fill(cairo);
    /* The seams between the buttons and the body. */
    cairo_set_source_rgb(cairo, 0.35, 0.35, 0.35);
    cairo_move_to(cairo, cx, y);
    cairo_line_to(cairo, cx, seam);
    cairo_move_to(cairo, x, seam);
    cairo_line_to(cairo, x + mw, seam);
    cairo_stroke(cairo);
    return FALSE;
}

static void on_switch(GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    properties->mouse.left_handed = gtk_toggle_button_get_active(toggle);
    gtk_widget_queue_draw(properties->picture);
    changed(properties);
}

static void show_folder(props *properties) {
    gtk_image_set_from_icon_name(GTK_IMAGE(properties->folder),
                                 properties->folder_open ? "folder-open" : "folder",
                                 GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(properties->folder), 32);
}

/* The test folder opens or closes on a double-click at the speed the
 * slider shows, before it's applied. */
static gboolean on_folder_press([[maybe_unused]] GtkWidget *widget, GdkEventButton *button_event,
                                gpointer data) {
    props *properties = data;
    if (button_event->type != GDK_BUTTON_PRESS || button_event->button != GDK_BUTTON_PRIMARY)
        return TRUE;
    if (properties->last_press != 0 &&
        button_event->time - properties->last_press <= (guint32)properties->mouse.double_click_ms) {
        properties->folder_open = !properties->folder_open;
        show_folder(properties);
        properties->last_press = 0;
    } else {
        properties->last_press = button_event->time;
    }
    return TRUE;
}

static void on_double_click(GtkRange *range, gpointer data) {
    props *properties = data;
    properties->mouse.double_click_ms = range_int(range);
    changed(properties);
}

static void on_click_lock(GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    properties->mouse.click_lock = gtk_toggle_button_get_active(toggle);
    gtk_widget_set_sensitive(properties->lock_settings, properties->mouse.click_lock);
    changed(properties);
}

/* XP's Settings for ClickLock: how long the press that locks is. */
static void on_lock_settings([[maybe_unused]] GtkWidget *button, gpointer data) {
    props *properties = data;
    GtkWidget *lock_dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(lock_dialog), "Settings for ClickLock");
    gtk_window_set_modal(GTK_WINDOW(lock_dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(lock_dialog), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(lock_dialog), GTK_WINDOW(properties->window));
    gtk_dialog_add_button(GTK_DIALOG(lock_dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(lock_dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(lock_dialog), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *text = wrapped("Adjust how long you need to hold down a mouse or trackball "
                              "button before your click is \"locked\"");
    gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
    GtkWidget *scale = ticked_scale(XD_CLICK_LOCK_MIN, XD_CLICK_LOCK_MAX, 200);
    gtk_range_set_value(GTK_RANGE(scale), properties->mouse.click_lock_ms);
    gtk_box_pack_start(GTK_BOX(box), slider_row("Short", scale, "Long"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(lock_dialog))), box, TRUE,
                       TRUE, 0);
    gtk_widget_show_all(lock_dialog);
    if (gtk_dialog_run(GTK_DIALOG(lock_dialog)) == GTK_RESPONSE_OK &&
        range_int(GTK_RANGE(scale)) != properties->mouse.click_lock_ms) {
        properties->mouse.click_lock_ms = range_int(GTK_RANGE(scale));
        changed(properties);
    }
    gtk_widget_destroy(lock_dialog);
}

static GtkWidget *buttons_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *config = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    properties->switch_buttons =
        gtk_check_button_new_with_mnemonic("_Switch primary and secondary buttons");
    g_signal_connect(properties->switch_buttons, "toggled", G_CALLBACK(on_switch), properties);
    gtk_box_pack_start(GTK_BOX(left), properties->switch_buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left),
                       wrapped("Select this check box to make the button on the right the one "
                               "you use for primary functions such as selecting and dragging."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(config), left, TRUE, TRUE, 0);
    properties->picture = gtk_drawing_area_new();
    gtk_widget_set_size_request(properties->picture, 80, 100);
    g_signal_connect(properties->picture, "draw", G_CALLBACK(draw_mouse), properties);
    gtk_box_pack_start(GTK_BOX(config), properties->picture, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Button configuration", config), FALSE, FALSE, 0);

    GtkWidget *dbl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *dleft = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(dleft),
                       wrapped("Double-click the folder to test your setting. If the folder "
                               "does not open or close, try using a slower setting."),
                       FALSE, FALSE, 0);
    properties->double_click = ticked_scale(XD_DOUBLE_CLICK_MIN, XD_DOUBLE_CLICK_MAX, 100);
    /* Slow on the left: the longest time. */
    gtk_range_set_inverted(GTK_RANGE(properties->double_click), TRUE);
    g_signal_connect(properties->double_click, "value-changed", G_CALLBACK(on_double_click),
                     properties);
    GtkWidget *speed_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *speed_label = gtk_label_new_with_mnemonic("Spee_d:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(speed_label), properties->double_click);
    gtk_box_pack_start(GTK_BOX(speed_row), speed_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(speed_row), slider_row("Slow", properties->double_click, "Fast"),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(dleft), speed_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dbl), dleft, TRUE, TRUE, 0);
    GtkWidget *events = gtk_event_box_new();
    properties->folder = gtk_image_new();
    show_folder(properties);
    gtk_container_add(GTK_CONTAINER(events), properties->folder);
    gtk_widget_set_size_request(events, 80, -1);
    gtk_widget_set_valign(events, GTK_ALIGN_CENTER);
    g_signal_connect(events, "button-press-event", G_CALLBACK(on_folder_press), properties);
    gtk_box_pack_start(GTK_BOX(dbl), events, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Double-click speed", dbl), FALSE, FALSE, 0);

    GtkWidget *lock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *lock_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    properties->click_lock = gtk_check_button_new_with_mnemonic("Turn on Click_Lock");
    gtk_box_pack_start(GTK_BOX(lock_row), properties->click_lock, TRUE, TRUE, 0);
    properties->lock_settings = gtk_button_new_with_mnemonic("S_ettings");
    gtk_widget_set_size_request(properties->lock_settings, 75, -1);
    gtk_box_pack_end(GTK_BOX(lock_row), properties->lock_settings, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lock), lock_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lock),
                       wrapped("Enables you to highlight or drag without holding down the mouse "
                               "button. To set, briefly press the mouse button. To release, "
                               "click the mouse button again."),
                       FALSE, FALSE, 0);
    g_signal_connect(properties->click_lock, "toggled", G_CALLBACK(on_click_lock), properties);
    g_signal_connect(properties->lock_settings, "clicked", G_CALLBACK(on_lock_settings),
                     properties);
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
    cairo_surface_t *surface =
        img->width <= 1024 && img->height <= 1024
            ? cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)img->width, (int)img->height)
            : nullptr;
    /* A theme's cursor can say any size (Xcursor allows 32767): none
     * drawn rather than writing through a surface that failed. */
    if (surface == nullptr || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface != nullptr)
            cairo_surface_destroy(surface);
        XcursorImageDestroy(img);
        return nullptr;
    }
    cairo_surface_flush(surface);
    unsigned char *dst = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (unsigned y = 0; y < img->height; y++)
        memcpy(dst + (size_t)y * (size_t)stride, img->pixels + (size_t)y * img->width,
               img->width * sizeof(XcursorPixel));
    cairo_surface_mark_dirty(surface);
    XcursorImageDestroy(img);
    return surface;
}

static GtkWidget *cursor_image(const char *theme, int size, const char *const *names) {
    cairo_surface_t *surface = load_cursor(theme, size, names);
    GtkWidget *img = surface != nullptr
                         ? gtk_image_new_from_surface(surface)
                         : gtk_image_new_from_icon_name("image-missing", GTK_ICON_SIZE_BUTTON);
    if (surface != nullptr)
        cairo_surface_destroy(surface);
    return img;
}

/* The theme's cursors in the list, and its arrow large on the right. */
static void show_theme(props *properties) {
    const char *theme =
        properties->mouse.cursor_theme[0] != '\0' ? properties->mouse.cursor_theme : "default";
    int size = properties->mouse.cursor_size;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(properties->roles));
    for (GList *list = rows; list != nullptr; list = list->next)
        gtk_widget_destroy(list->data);
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
        gtk_list_box_insert(GTK_LIST_BOX(properties->roles), row, -1);
    }
    gtk_widget_show_all(properties->roles);
    GtkWidget *parent = gtk_widget_get_parent(properties->big_preview);
    gtk_widget_destroy(properties->big_preview);
    properties->big_preview = cursor_image(theme, 48, roles[0].names);
    gtk_widget_set_size_request(properties->big_preview, 64, 64);
    gtk_container_add(GTK_CONTAINER(parent), properties->big_preview);
    gtk_widget_show(properties->big_preview);
}

static void on_theme(GtkComboBox *combo, gpointer data) {
    props *properties = data;
    const char *id = gtk_combo_box_get_active_id(combo);
    if (id == nullptr)
        return;
    g_strlcpy(properties->mouse.cursor_theme, id, sizeof(properties->mouse.cursor_theme));
    show_theme(properties);
    changed(properties);
}

static void on_size(GtkComboBox *combo, gpointer data) {
    props *properties = data;
    const char *id = gtk_combo_box_get_active_id(combo);
    if (id == nullptr)
        return;
    properties->mouse.cursor_size = atoi(id);
    show_theme(properties);
    changed(properties);
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

static GtkWidget *pointers_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    properties->themes = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->themes), "", "(System default)");
    char **dirs = xd_cursor_theme_dirs();
    properties->theme_list = xd_cursor_themes_list((const char *const *)dirs);
    g_strfreev(dirs);
    bool found = properties->mouse.cursor_theme[0] == '\0';
    for (guint i = 0; i < properties->theme_list->len; i++) {
        const xd_cursor_theme *cursor_theme = g_ptr_array_index(properties->theme_list, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->themes), cursor_theme->dir,
                                  cursor_theme->name);
        found = found || strcmp(cursor_theme->dir, properties->mouse.cursor_theme) == 0;
    }
    /* A theme the file names but that isn't installed (any more). */
    if (!found)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->themes),
                                  properties->mouse.cursor_theme, properties->mouse.cursor_theme);
    gtk_widget_set_hexpand(properties->themes, TRUE);
    GtkWidget *scheme_label = gtk_label_new_with_mnemonic("Sch_eme:");
    gtk_label_set_xalign(GTK_LABEL(scheme_label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(scheme_label), properties->themes);
    gtk_grid_attach(GTK_GRID(grid), scheme_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), properties->themes, 1, 0, 1, 1);
    properties->sizes = gtk_combo_box_text_new();
    fill_sizes(properties->sizes, properties->mouse.cursor_size);
    GtkWidget *size_label = gtk_label_new_with_mnemonic("Si_ze:");
    gtk_label_set_xalign(GTK_LABEL(size_label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(size_label), properties->sizes);
    gtk_grid_attach(GTK_GRID(grid), size_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), properties->sizes, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(top), frame("Scheme", grid), TRUE, TRUE, 0);
    GtkWidget *preview = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(preview), GTK_SHADOW_IN);
    properties->big_preview = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(preview), properties->big_preview);
    gtk_widget_set_valign(preview, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(top), preview, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *custom = gtk_label_new_with_mnemonic("_Customize:");
    gtk_label_set_xalign(GTK_LABEL(custom), 0);
    gtk_box_pack_start(GTK_BOX(page), custom, FALSE, FALSE, 0);
    properties->roles = gtk_list_box_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(custom), properties->roles);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 190);
    gtk_container_add(GTK_CONTAINER(scroll), properties->roles);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    GtkWidget *note = wrapped("Programs started from now on use the new pointers; some keep "
                              "theirs until they're restarted.");
    gtk_box_pack_start(GTK_BOX(page), note, FALSE, FALSE, 0);

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->themes), properties->mouse.cursor_theme);
    char size_id[16];
    g_snprintf(size_id, sizeof(size_id), "%d", properties->mouse.cursor_size);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->sizes), size_id);
    show_theme(properties);
    g_signal_connect(properties->themes, "changed", G_CALLBACK(on_theme), properties);
    g_signal_connect(properties->sizes, "changed", G_CALLBACK(on_size), properties);
    return page;
}

/* ---- Pointer Options ------------------------------------------------------------- */

static void on_speed(GtkRange *range, gpointer data) {
    props *properties = data;
    properties->mouse.speed = range_int(range);
    changed(properties);
}

static void on_precision(GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    properties->mouse.acceleration = gtk_toggle_button_get_active(toggle);
    changed(properties);
}

static GtkWidget *options_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *motion = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("Select a pointer _speed:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(motion), label, FALSE, FALSE, 0);
    properties->speed = ticked_scale(XD_MOUSE_SPEED_MIN, XD_MOUSE_SPEED_MAX, 1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->speed);
    gtk_range_set_value(GTK_RANGE(properties->speed), properties->mouse.speed);
    g_signal_connect(properties->speed, "value-changed", G_CALLBACK(on_speed), properties);
    gtk_box_pack_start(GTK_BOX(motion), slider_row("Slow", properties->speed, "Fast"), FALSE, FALSE,
                       0);
    properties->precision = gtk_check_button_new_with_mnemonic("_Enhance pointer precision");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->precision),
                                 properties->mouse.acceleration);
    g_signal_connect(properties->precision, "toggled", G_CALLBACK(on_precision), properties);
    gtk_box_pack_start(GTK_BOX(motion), properties->precision, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(motion),
                       wrapped("With enhanced precision the pointer goes further when you "
                               "move the mouse quickly, and less far when you move it slowly."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Motion", motion), FALSE, FALSE, 0);
    return page;
}

/* ---- Wheel -------------------------------------------------------------------------- */

static void on_lines(GtkSpinButton *spin, gpointer data) {
    props *properties = data;
    properties->mouse.wheel_lines = gtk_spin_button_get_value_as_int(spin);
    changed(properties);
}

static GtkWidget *wheel_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box), wrapped("Roll the wheel one notch to scroll:"), FALSE,
                       FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new_with_mnemonic("The following _number of lines at a time:");
    properties->lines = gtk_spin_button_new_with_range(1, 30, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(properties->lines), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(properties->lines), properties->mouse.wheel_lines);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->lines);
    g_signal_connect(properties->lines, "value-changed", G_CALLBACK(on_lines), properties);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 20);
    gtk_box_pack_start(GTK_BOX(row), properties->lines, FALSE, FALSE, 0);
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

/* ---- Touchpad ---------------------------------------------------------------------- */

/* The tab's widgets into `touchpad`, all at once (they depend on each
 * other: tap-and-drag needs tapping, drag lock tap-and-drag). */
static void read_touchpad(props *properties) {
    xd_touchpad_settings *touchpad = &properties->mouse.touchpad;
    touchpad->tap = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->tap));
    touchpad->tap_drag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->tap_drag));
    touchpad->drag_lock = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->drag_lock));
    int scroll = gtk_combo_box_get_active(GTK_COMBO_BOX(properties->scroll));
    if (scroll >= 0 && scroll < XD_TOUCHPAD_SCROLL_COUNT)
        touchpad->scroll = (xd_touchpad_scroll)scroll;
    touchpad->natural_scroll = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->natural));
    touchpad->scroll_speed = range_int(GTK_RANGE(properties->scroll_speed));
    int click = gtk_combo_box_get_active(GTK_COMBO_BOX(properties->click));
    if (click >= 0 && click < XD_TOUCHPAD_CLICK_COUNT)
        touchpad->click = (xd_touchpad_click)click;
    touchpad->disable_typing = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->typing));
    touchpad->disable_with_mouse =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->with_mouse));
    touchpad->speed = range_int(GTK_RANGE(properties->pad_speed));
    touchpad->acceleration =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->pad_precision));
    touchpad->gestures = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->gestures));
}

static void show_touchpad_dependents(props *properties) {
    const xd_touchpad_settings *touchpad = &properties->mouse.touchpad;
    gtk_widget_set_sensitive(properties->tap_drag, touchpad->tap);
    gtk_widget_set_sensitive(properties->drag_lock, touchpad->tap && touchpad->tap_drag);
    bool scrolls = touchpad->scroll != XD_TOUCHPAD_SCROLL_NONE;
    gtk_widget_set_sensitive(properties->natural, scrolls);
    gtk_widget_set_sensitive(properties->scroll_speed, scrolls);
}

static void on_touchpad(GtkWidget *widget, gpointer data) {
    (void)widget;
    props *properties = data;
    if (properties->updating)
        return;
    read_touchpad(properties);
    show_touchpad_dependents(properties);
    changed(properties);
}

static GtkWidget *touchpad_check(props *properties, const char *label, bool active) {
    GtkWidget *check = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), active);
    g_signal_connect(check, "toggled", G_CALLBACK(on_touchpad), properties);
    return check;
}

static GtkWidget *touchpad_page(props *properties) {
    const xd_touchpad_settings *touchpad = &properties->mouse.touchpad;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *taps = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    properties->tap = touchpad_check(properties, "_Tap to click", touchpad->tap);
    gtk_box_pack_start(GTK_BOX(taps), properties->tap, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(taps),
                       wrapped("Tap with two fingers to right-click, with three to "
                               "middle-click."),
                       FALSE, FALSE, 0);
    properties->tap_drag =
        touchpad_check(properties, "Tap and _drag (tap, then touch and move)", touchpad->tap_drag);
    gtk_box_pack_start(GTK_BOX(taps), properties->tap_drag, FALSE, FALSE, 0);
    properties->drag_lock = touchpad_check(
        properties, "_Keep dragging for a moment after lifting the finger", touchpad->drag_lock);
    gtk_box_pack_start(GTK_BOX(taps), properties->drag_lock, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Tapping", taps), FALSE, FALSE, 0);

    GtkWidget *scrolling = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Scroll with:");
    properties->scroll = gtk_combo_box_text_new();
    static const char *const scroll_names[XD_TOUCHPAD_SCROLL_COUNT] = {
        [XD_TOUCHPAD_SCROLL_TWO_FINGER] = "Two fingers",
        [XD_TOUCHPAD_SCROLL_EDGE] = "One finger along the edge",
        [XD_TOUCHPAD_SCROLL_NONE] = "Nothing (no scrolling)",
    };
    for (int i = 0; i < XD_TOUCHPAD_SCROLL_COUNT; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(properties->scroll), scroll_names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(properties->scroll), (int)touchpad->scroll);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->scroll);
    g_signal_connect(properties->scroll, "changed", G_CALLBACK(on_touchpad), properties);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), properties->scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(scrolling), row, FALSE, FALSE, 0);
    properties->natural =
        touchpad_check(properties, "Re_verse scrolling direction (content follows the fingers)",
                       touchpad->natural_scroll);
    gtk_box_pack_start(GTK_BOX(scrolling), properties->natural, FALSE, FALSE, 0);
    properties->scroll_speed = gtk_scale_new_with_range(
        GTK_ORIENTATION_HORIZONTAL, XD_TOUCHPAD_SCROLL_SPEED_MIN, XD_TOUCHPAD_SCROLL_SPEED_MAX, 25);
    gtk_scale_add_mark(GTK_SCALE(properties->scroll_speed), 100, GTK_POS_BOTTOM, nullptr);
    gtk_range_set_round_digits(GTK_RANGE(properties->scroll_speed), 0);
    gtk_range_set_value(GTK_RANGE(properties->scroll_speed), touchpad->scroll_speed);
    g_signal_connect(properties->scroll_speed, "value-changed", G_CALLBACK(on_touchpad),
                     properties);
    gtk_box_pack_start(GTK_BOX(scrolling), slider_row("Slow", properties->scroll_speed, "Fast"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Scrolling", scrolling), FALSE, FALSE, 0);

    GtkWidget *clicking = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    label = gtk_label_new_with_mnemonic("_Right-click by pressing:");
    properties->click = gtk_combo_box_text_new();
    static const char *const click_names[XD_TOUCHPAD_CLICK_COUNT] = {
        [XD_TOUCHPAD_CLICK_DEFAULT] = "As the touchpad comes",
        [XD_TOUCHPAD_CLICK_AREAS] = "Its bottom right corner",
        [XD_TOUCHPAD_CLICK_FINGERS] = "With two fingers",
    };
    for (int i = 0; i < XD_TOUCHPAD_CLICK_COUNT; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(properties->click), click_names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(properties->click), (int)touchpad->click);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->click);
    g_signal_connect(properties->click, "changed", G_CALLBACK(on_touchpad), properties);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), properties->click, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(clicking), row, FALSE, FALSE, 0);
    properties->typing =
        touchpad_check(properties, "Turn the touchpad off while t_yping", touchpad->disable_typing);
    gtk_box_pack_start(GTK_BOX(clicking), properties->typing, FALSE, FALSE, 0);
    properties->with_mouse =
        touchpad_check(properties, "Turn the touchpad off while a _mouse is plugged in",
                       touchpad->disable_with_mouse);
    gtk_box_pack_start(GTK_BOX(clicking), properties->with_mouse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Clicking", clicking), FALSE, FALSE, 0);

    GtkWidget *motion = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    properties->pad_speed = ticked_scale(XD_MOUSE_SPEED_MIN, XD_MOUSE_SPEED_MAX, 1);
    gtk_range_set_value(GTK_RANGE(properties->pad_speed), touchpad->speed);
    g_signal_connect(properties->pad_speed, "value-changed", G_CALLBACK(on_touchpad), properties);
    gtk_box_pack_start(GTK_BOX(motion), slider_row("Slow", properties->pad_speed, "Fast"), FALSE,
                       FALSE, 0);
    properties->pad_precision =
        touchpad_check(properties, "Enhance pointer prec_ision", touchpad->acceleration);
    gtk_box_pack_start(GTK_BOX(motion), properties->pad_precision, FALSE, FALSE, 0);
    properties->gestures =
        touchpad_check(properties,
                       "Swipe with three _fingers: down shows the desktop, up brings the "
                       "windows back, sideways switches windows",
                       touchpad->gestures);
    gtk_label_set_line_wrap(GTK_LABEL(gtk_bin_get_child(GTK_BIN(properties->gestures))), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(gtk_bin_get_child(GTK_BIN(properties->gestures))), 46);
    gtk_box_pack_start(GTK_BOX(motion), properties->gestures, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Pointer speed and gestures", motion), FALSE, FALSE, 0);
    show_touchpad_dependents(properties);
    return page;
}

/* ---- applying ----------------------------------------------------------------------- */

static bool apply(props *properties) {
    /* Everything else in the file as it is now: other applets may have
     * changed it since we started. */
    xd_settings settings;
    xd_settings_load(&settings);
    settings.mouse = properties->mouse;
    if (!xd_settings_save(&settings)) {
        xd_message_box(GTK_WINDOW(properties->window), "Mouse Properties", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "The settings could not be saved.");
        return false;
    }
    gtk_widget_set_sensitive(properties->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *dialog, int response, gpointer data) {
    props *properties = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply) &&
        !apply(properties))
        return;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Mouse");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props properties;
    xd_settings settings;
    xd_settings_load(&settings);
    properties.mouse = settings.mouse;

    properties.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties.window), "Mouse Properties");
    gtk_window_set_icon_name(GTK_WINDOW(properties.window), "input-mouse");
    gtk_window_set_resizable(GTK_WINDOW(properties.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "Cancel", GTK_RESPONSE_CANCEL);
    properties.apply =
        gtk_dialog_add_button(GTK_DIALOG(properties.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(properties.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    properties.updating = true;
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), buttons_page(&properties),
                             gtk_label_new_with_mnemonic("_Buttons"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), pointers_page(&properties),
                             gtk_label_new_with_mnemonic("_Pointers"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), options_page(&properties),
                             gtk_label_new_with_mnemonic("Pointer _Options"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), wheel_page(&properties),
                             gtk_label_new_with_mnemonic("_Wheel"));
    const char *show_touchpad = getenv("XD_SHOW_TOUCHPAD");
    if (xd_input_has_touchpad() || (show_touchpad != nullptr && strcmp(show_touchpad, "1") == 0))
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), touchpad_page(&properties),
                                 gtk_label_new_with_mnemonic("Tou_chpad"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), xd_input_devices_page(XD_INPUT_POINTERS),
                             gtk_label_new_with_mnemonic("_Hardware"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties.switch_buttons),
                                 properties.mouse.left_handed);
    gtk_range_set_value(GTK_RANGE(properties.double_click), properties.mouse.double_click_ms);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties.click_lock),
                                 properties.mouse.click_lock);
    gtk_widget_set_sensitive(properties.lock_settings, properties.mouse.click_lock);
    properties.updating = false;
    gtk_widget_set_sensitive(properties.apply, FALSE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties.window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties.window, GTK_NOTEBOOK(tabs));

    g_signal_connect(properties.window, "response", G_CALLBACK(on_response), &properties);
    g_signal_connect(properties.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(properties.window);
    gtk_main();
    g_ptr_array_unref(properties.theme_list);
    return EXIT_SUCCESS;
}
