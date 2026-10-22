/* SPDX-License-Identifier: MIT
 * The Mark-up window, as Windows 7's: the snip, to draw on with a pen
 * (red, blue or black) or the highlighter, and an eraser that takes whole
 * strokes away; then Save As (PNG, JPEG or BMP) or Copy. New Snip takes
 * another into the same window, and closing it closes the Snipping Tool;
 * both ask first about a snip that wasn't saved, if Options says to.
 */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

#include "../gtkapp/gtkapp.h"
#include "snip.h"

/* Logical px: the pen's and the highlighter's width, and how near the
 * eraser must come to a stroke. */
static constexpr double PEN_WIDTH = 2.0;
static constexpr double HIGHLIGHTER_WIDTH = 12.0;
static constexpr double ERASER_RADIUS = 4.0;
/* At most this much of the monitor before the snip scrolls. */
static constexpr double MAX_SCREEN_SHARE = 0.8;
static constexpr GdkModifierType NO_MODIFIERS = (GdkModifierType)0;

typedef enum editor_tool : uint8_t { TOOL_PEN, TOOL_HIGHLIGHTER, TOOL_ERASER } editor_tool;

typedef struct pen_color {
    const char *label; /* the Tools menu's */
    double color[3];
} pen_color;

static const pen_color pen_colors[] = {
    {"_Red Pen", {1, 0, 0}},
    {"_Blue Pen", {0, 0, 1}},
    {"Blac_k Pen", {0, 0, 0}},
};

static const double highlighter_color[3] = {1, 1, 0};

struct snip_editor {
    snip_app *app;
    GtkWidget *window;
    GtkWidget *scroller;
    GtkWidget *canvas;
    GtkWidget *tool_buttons[3]; /* by editor_tool */
    GtkWidget *tool_items[3];   /* the Tools menu's */
    GtkWidget *pen_items[G_N_ELEMENTS(pen_colors)];
    cairo_surface_t *snip;
    double scale; /* the snip's pixels to a logical pixel */
    xd_ink ink;
    xd_ink_stroke *stroke; /* being drawn */
    bool erasing;
    editor_tool tool;
    size_t pen;
    bool unsaved;
    bool syncing; /* setting the tool buttons and menu to match */
};

/* The snip with its ink, as it would be saved. */
static cairo_surface_t *flatten(const snip_editor *editor) {
    int width = cairo_image_surface_get_width(editor->snip);
    int height = cairo_image_surface_get_height(editor->snip);
    cairo_surface_t *flat = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    cairo_t *cairo = cairo_create(flat);
    cairo_set_source_surface(cairo, editor->snip, 0, 0);
    cairo_paint(cairo);
    xd_ink_draw(&editor->ink, cairo);
    cairo_destroy(cairo);
    cairo_surface_flush(flat);
    return flat;
}

/* ---- the canvas ---------------------------------------------------------------------------- */

static gboolean draw_canvas([[maybe_unused]] GtkWidget *widget, cairo_t *cairo, gpointer data) {
    snip_editor *editor = data;
    if (editor->snip == nullptr)
        return TRUE;
    cairo_scale(cairo, 1 / editor->scale, 1 / editor->scale);
    cairo_set_source_surface(cairo, editor->snip, 0, 0);
    if (editor->scale == 1)
        cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_NEAREST);
    cairo_paint(cairo);
    xd_ink_draw(&editor->ink, cairo);
    return TRUE;
}

static xd_snip_point snip_point(const snip_editor *editor, double x, double y) {
    return (xd_snip_point){x * editor->scale, y * editor->scale};
}

static void erase_at(snip_editor *editor, xd_snip_point point) {
    if (xd_ink_erase_at(&editor->ink, point, ERASER_RADIUS * editor->scale))
        gtk_widget_queue_draw(editor->canvas);
}

static gboolean on_canvas_press([[maybe_unused]] GtkWidget *widget, GdkEventButton *event,
                                gpointer data) {
    snip_editor *editor = data;
    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY ||
        editor->snip == nullptr)
        return FALSE;
    xd_snip_point point = snip_point(editor, event->x, event->y);
    if (editor->tool == TOOL_ERASER) {
        editor->erasing = true;
        erase_at(editor, point);
        return TRUE;
    }
    bool highlighter = editor->tool == TOOL_HIGHLIGHTER;
    editor->stroke = xd_ink_begin(
        &editor->ink, highlighter ? XD_INK_HIGHLIGHTER : XD_INK_PEN,
        highlighter ? highlighter_color : pen_colors[editor->pen].color,
        (highlighter ? HIGHLIGHTER_WIDTH : PEN_WIDTH) * editor->scale, point);
    editor->unsaved = true;
    gtk_widget_queue_draw(editor->canvas);
    return TRUE;
}

static gboolean on_canvas_motion([[maybe_unused]] GtkWidget *widget, GdkEventMotion *event,
                                 gpointer data) {
    snip_editor *editor = data;
    xd_snip_point point = snip_point(editor, event->x, event->y);
    if (editor->erasing) {
        erase_at(editor, point);
    } else if (editor->stroke != nullptr) {
        xd_ink_extend(editor->stroke, point);
        gtk_widget_queue_draw(editor->canvas);
    }
    return TRUE;
}

static gboolean on_canvas_release([[maybe_unused]] GtkWidget *widget, GdkEventButton *event,
                                  gpointer data) {
    snip_editor *editor = data;
    if (event->button == GDK_BUTTON_PRIMARY) {
        editor->stroke = nullptr;
        editor->erasing = false;
    }
    return TRUE;
}

static void set_canvas_cursor(snip_editor *editor) {
    GdkWindow *window = gtk_widget_get_window(editor->canvas);
    if (window == nullptr)
        return;
    GdkCursor *cursor = gdk_cursor_new_from_name(gtk_widget_get_display(editor->canvas),
                                                 editor->tool == TOOL_ERASER ? "default"
                                                                             : "crosshair");
    gdk_window_set_cursor(window, cursor);
    if (cursor != nullptr)
        g_object_unref(cursor);
}

/* ---- tools --------------------------------------------------------------------------------- */

static void set_tool(snip_editor *editor, editor_tool tool) {
    editor->tool = tool;
    editor->syncing = true;
    gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(editor->tool_buttons[tool]), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(editor->tool_items[tool]), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(editor->pen_items[editor->pen]), TRUE);
    editor->syncing = false;
    set_canvas_cursor(editor);
}

static void on_tool_button(GtkToggleToolButton *button, gpointer data) {
    snip_editor *editor = data;
    if (editor->syncing || !gtk_toggle_tool_button_get_active(button))
        return;
    for (int tool = 0; tool < 3; tool++) {
        if (editor->tool_buttons[tool] == GTK_WIDGET(button))
            set_tool(editor, (editor_tool)tool);
    }
}

static void on_tool_item(GtkCheckMenuItem *item, gpointer data) {
    snip_editor *editor = data;
    if (editor->syncing || !gtk_check_menu_item_get_active(item))
        return;
    for (int tool = 0; tool < 3; tool++) {
        if (editor->tool_items[tool] == GTK_WIDGET(item))
            set_tool(editor, (editor_tool)tool);
    }
}

static void on_pen_item(GtkCheckMenuItem *item, gpointer data) {
    snip_editor *editor = data;
    if (editor->syncing || !gtk_check_menu_item_get_active(item))
        return;
    for (size_t i = 0; i < G_N_ELEMENTS(pen_colors); i++) {
        if (editor->pen_items[i] == GTK_WIDGET(item))
            editor->pen = i;
    }
    set_tool(editor, TOOL_PEN);
}

/* ---- saving and copying ------------------------------------------------------------------------ */

typedef struct file_type {
    const char *name;      /* the Save As list's */
    const char *pattern;   /* its filter */
    const char *extension; /* added when the name has none of its own */
    const char *pixbuf_type;
} file_type;

static const file_type file_types[] = {
    {"Portable Network Graphic file (PNG) (*.png)", "*.png", ".png", "png"},
    {"JPEG file (*.jpg)", "*.jpg", ".jpg", "jpeg"},
    {"Bitmap file (*.bmp)", "*.bmp", ".bmp", "bmp"},
};

static bool save_snip(snip_editor *editor, const char *path, const file_type *type,
                      GError **error) {
    cairo_surface_t *flat = flatten(editor);
    bool saved;
    if (strcmp(type->pixbuf_type, "png") == 0) {
        cairo_status_t status = cairo_surface_write_to_png(flat, path);
        saved = status == CAIRO_STATUS_SUCCESS;
        if (!saved)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                        cairo_status_to_string(status));
    } else {
        GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(flat, 0, 0,
                                                        cairo_image_surface_get_width(flat),
                                                        cairo_image_surface_get_height(flat));
        saved = gdk_pixbuf_save(pixbuf, path, type->pixbuf_type, error,
                                strcmp(type->pixbuf_type, "jpeg") == 0 ? "quality" : nullptr,
                                "95", nullptr);
        g_object_unref(pixbuf);
    }
    cairo_surface_destroy(flat);
    return saved;
}

/* Save As; false if cancelled or it failed. */
static bool save_as(snip_editor *editor) {
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Save As", GTK_WINDOW(editor->window), GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "Cancel");
    GtkFileChooser *file_chooser = GTK_FILE_CHOOSER(chooser);
    gtk_file_chooser_set_do_overwrite_confirmation(file_chooser, TRUE);
    GtkFileFilter *filters[G_N_ELEMENTS(file_types)];
    for (size_t i = 0; i < G_N_ELEMENTS(file_types); i++) {
        filters[i] = gtk_file_filter_new();
        gtk_file_filter_set_name(filters[i], file_types[i].name);
        gtk_file_filter_add_pattern(filters[i], file_types[i].pattern);
        gtk_file_chooser_add_filter(file_chooser, filters[i]);
    }
    /* Pictures; GLib says the home folder when user-dirs.dirs names none. */
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    g_autofree char *home_pictures = g_build_filename(g_get_home_dir(), "Pictures", nullptr);
    if ((pictures == nullptr || strcmp(pictures, g_get_home_dir()) == 0) &&
        g_file_test(home_pictures, G_FILE_TEST_IS_DIR))
        pictures = home_pictures;
    gtk_file_chooser_set_current_folder(file_chooser,
                                        pictures != nullptr ? pictures : g_get_home_dir());
    gtk_file_chooser_set_current_name(file_chooser, "Capture.png");

    bool saved = false;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        g_autofree char *path = gtk_file_chooser_get_filename(file_chooser);
        GtkFileFilter *chosen = gtk_file_chooser_get_filter(file_chooser);
        const file_type *type = &file_types[0];
        /* The name's own extension decides, then the list. */
        bool by_name = false;
        for (size_t i = 0; i < G_N_ELEMENTS(file_types); i++) {
            if (g_str_has_suffix(path, file_types[i].extension) ||
                (i == 1 && (g_str_has_suffix(path, ".jpeg") || g_str_has_suffix(path, ".JPG")))) {
                type = &file_types[i];
                by_name = true;
            }
        }
        if (!by_name) {
            for (size_t i = 0; i < G_N_ELEMENTS(file_types); i++) {
                if (chosen == filters[i])
                    type = &file_types[i];
            }
            char *named = g_strconcat(path, type->extension, nullptr);
            g_free(path);
            path = named;
        }
        GError *error = nullptr;
        saved = save_snip(editor, path, type, &error);
        if (saved) {
            editor->unsaved = false;
        } else {
            g_autofree char *text =
                g_strdup_printf("The snip couldn't be saved.\n\n%s", error->message);
            xd_message_box(GTK_WINDOW(editor->window), SNIP_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           text);
            g_error_free(error);
        }
    }
    g_object_unref(chooser);
    return saved;
}

bool snip_editor_may_discard(snip_editor *editor) {
    if (editor->snip == nullptr || !editor->unsaved || !editor->app->options.prompt_to_save)
        return true;
    static const char *const labels[] = {"_Yes", "_No", "Cancel"};
    int choice = xd_message_box_choose(GTK_WINDOW(editor->window), SNIP_TITLE, XD_MB_WARNING,
                                       "Do you want to save changes to the snip?", labels, 3, 0,
                                       2);
    if (choice == 0)
        return save_as(editor);
    return choice == 1;
}

static void on_save(gpointer data) {
    save_as(data);
}

static void on_copy(gpointer data) {
    snip_editor *editor = data;
    if (editor->snip == nullptr)
        return;
    cairo_surface_t *flat = flatten(editor);
    snip_clipboard_copy(flat);
    cairo_surface_destroy(flat);
}

static void on_new_snip(gpointer data) {
    snip_editor *editor = data;
    if (snip_editor_may_discard(editor))
        snip_app_new_snip(editor->app, editor->app->options.mode);
}

static void on_new_mode(GtkMenuItem *item, gpointer data) {
    snip_editor *editor = data;
    gpointer mode_data = g_object_get_data(G_OBJECT(item), "mode");
    xd_snip_mode mode = (xd_snip_mode)GPOINTER_TO_INT(mode_data);
    if (snip_editor_may_discard(editor))
        snip_app_new_snip(editor->app, mode);
}

static void on_options(gpointer data) {
    snip_editor *editor = data;
    snip_app_show_options(editor->app, GTK_WINDOW(editor->window));
}

static gboolean on_delete([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] GdkEvent *event,
                          gpointer data) {
    snip_editor *editor = data;
    if (snip_editor_may_discard(editor))
        gtk_main_quit();
    return TRUE;
}

static void on_exit_item(gpointer data) {
    on_delete(nullptr, nullptr, data);
}

/* ---- building it ---------------------------------------------------------------------------- */

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback callback, gpointer data,
                            GtkAccelGroup *accel, guint key, GdkModifierType modifiers) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    if (callback != nullptr)
        g_signal_connect_swapped(item, "activate", callback, data);
    if (key != 0)
        gtk_widget_add_accelerator(item, "activate", accel, key, modifiers, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static GtkWidget *submenu(GtkWidget *bar, const char *label) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), item);
    return menu;
}

/* The snip kinds, as New's menu has them. */
static GtkWidget *mode_menu(snip_editor *editor) {
    static const char *const labels[] = {
        [XD_SNIP_FREE_FORM] = "_Free-form Snip",
        [XD_SNIP_RECTANGLE] = "_Rectangular Snip",
        [XD_SNIP_WINDOW] = "_Window Snip",
        [XD_SNIP_FULL_SCREEN] = "F_ull-screen Snip",
    };
    GtkWidget *menu = gtk_menu_new();
    for (size_t i = 0; i < G_N_ELEMENTS(labels); i++) {
        GtkWidget *item = gtk_menu_item_new_with_mnemonic(labels[i]);
        g_object_set_data(G_OBJECT(item), "mode", GINT_TO_POINTER((int)i));
        g_signal_connect(item, "activate", G_CALLBACK(on_new_mode), editor);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    return menu;
}

static GtkWidget *build_menus(snip_editor *editor, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    menu_item(file, "_New Snip", G_CALLBACK(on_new_snip), editor, accel, GDK_KEY_n,
              GDK_CONTROL_MASK);
    menu_item(file, "Save _As...", G_CALLBACK(on_save), editor, accel, GDK_KEY_s,
              GDK_CONTROL_MASK);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "E_xit", G_CALLBACK(on_exit_item), editor, accel, 0, NO_MODIFIERS);

    GtkWidget *edit = submenu(bar, "_Edit");
    menu_item(edit, "_Copy", G_CALLBACK(on_copy), editor, accel, GDK_KEY_c, GDK_CONTROL_MASK);

    GtkWidget *tools = submenu(bar, "_Tools");
    GtkWidget *pen = gtk_menu_item_new_with_mnemonic("_Pen");
    GtkWidget *pens = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(pen), pens);
    GSList *pen_group = nullptr;
    for (size_t i = 0; i < G_N_ELEMENTS(pen_colors); i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(pen_group, pen_colors[i].label);
        pen_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_signal_connect(item, "toggled", G_CALLBACK(on_pen_item), editor);
        gtk_menu_shell_append(GTK_MENU_SHELL(pens), item);
        editor->pen_items[i] = item;
    }
    static const char *const tool_labels[] = {"_Pen", "_Highlighter", "_Eraser"};
    GSList *tool_group = nullptr;
    for (int tool = 0; tool < 3; tool++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(tool_group, tool_labels[tool]);
        tool_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_signal_connect(item, "toggled", G_CALLBACK(on_tool_item), editor);
        editor->tool_items[tool] = item;
    }
    /* The pen's colours are its submenu; the radio item for the pen tool
     * itself isn't shown, only kept in step. */
    gtk_menu_shell_append(GTK_MENU_SHELL(tools), pen);
    gtk_menu_shell_append(GTK_MENU_SHELL(tools), editor->tool_items[TOOL_HIGHLIGHTER]);
    gtk_menu_shell_append(GTK_MENU_SHELL(tools), editor->tool_items[TOOL_ERASER]);
    g_object_ref_sink(editor->tool_items[TOOL_PEN]);
    gtk_menu_shell_append(GTK_MENU_SHELL(tools), gtk_separator_menu_item_new());
    menu_item(tools, "_Options...", G_CALLBACK(on_options), editor, accel, 0, NO_MODIFIERS);
    return bar;
}

static GtkToolItem *tool_button(GtkWidget *toolbar, const char *icon, const char *label,
                                const char *tip, GCallback callback, gpointer data) {
    GtkToolItem *item = gtk_tool_button_new(nullptr, label);
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(item), icon);
    gtk_tool_button_set_use_underline(GTK_TOOL_BUTTON(item), TRUE);
    gtk_tool_item_set_tooltip_text(item, tip);
    gtk_tool_item_set_is_important(item, TRUE);
    g_signal_connect_swapped(item, "clicked", callback, data);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
    return item;
}

static GtkWidget *build_toolbar(snip_editor *editor) {
    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_BOTH_HORIZ);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_SMALL_TOOLBAR);
    /* All of it shows: the window is never narrower. */
    gtk_toolbar_set_show_arrow(GTK_TOOLBAR(toolbar), FALSE);

    GtkToolItem *new_snip = gtk_menu_tool_button_new(nullptr, "_New");
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(new_snip), "edit-cut");
    gtk_tool_button_set_use_underline(GTK_TOOL_BUTTON(new_snip), TRUE);
    gtk_tool_item_set_is_important(new_snip, TRUE);
    gtk_tool_item_set_tooltip_text(new_snip, "New Snip (Ctrl+N)");
    gtk_menu_tool_button_set_menu(GTK_MENU_TOOL_BUTTON(new_snip), mode_menu(editor));
    g_signal_connect_swapped(new_snip, "clicked", G_CALLBACK(on_new_snip), editor);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), new_snip, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
    tool_button(toolbar, "document-save", "_Save", "Save Snip (Ctrl+S)", G_CALLBACK(on_save),
                editor);
    tool_button(toolbar, "edit-copy", "_Copy", "Copy (Ctrl+C)", G_CALLBACK(on_copy), editor);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);

    static const char *const icons[] = {"draw-freehand", "draw-highlight", "draw-eraser"};
    static const char *const labels[] = {"_Pen", "_Highlighter", "_Eraser"};
    GSList *group = nullptr;
    for (int tool = 0; tool < 3; tool++) {
        GtkToolItem *item = gtk_radio_tool_button_new(group);
        group = gtk_radio_tool_button_get_group(GTK_RADIO_TOOL_BUTTON(item));
        gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(item), icons[tool]);
        gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), labels[tool]);
        gtk_tool_button_set_use_underline(GTK_TOOL_BUTTON(item), TRUE);
        gtk_tool_item_set_is_important(item, TRUE);
        g_signal_connect(item, "toggled", G_CALLBACK(on_tool_button), editor);
        gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
        editor->tool_buttons[tool] = GTK_WIDGET(item);
    }
    return toolbar;
}

snip_editor *snip_editor_new(snip_app *app) {
    snip_editor *editor = g_new0(snip_editor, 1);
    editor->app = app;
    xd_ink_init(&editor->ink);

    editor->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(editor->window), SNIP_TITLE);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(editor->window), accel);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), build_menus(editor, accel), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), build_toolbar(editor), FALSE, FALSE, 0);

    editor->canvas = gtk_drawing_area_new();
    gtk_widget_add_events(editor->canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                              GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(editor->canvas, "draw", G_CALLBACK(draw_canvas), editor);
    g_signal_connect(editor->canvas, "button-press-event", G_CALLBACK(on_canvas_press), editor);
    g_signal_connect(editor->canvas, "motion-notify-event", G_CALLBACK(on_canvas_motion), editor);
    g_signal_connect(editor->canvas, "button-release-event", G_CALLBACK(on_canvas_release),
                     editor);
    g_signal_connect_swapped(editor->canvas, "realize", G_CALLBACK(set_canvas_cursor), editor);
    /* The snip at its top left, the window's colour around it. */
    gtk_widget_set_halign(editor->canvas, GTK_ALIGN_START);
    gtk_widget_set_valign(editor->canvas, GTK_ALIGN_START);

    editor->scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(editor->scroller), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(editor->scroller), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(editor->scroller), editor->canvas);
    gtk_box_pack_start(GTK_BOX(box), editor->scroller, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(editor->window), box);
    g_signal_connect(editor->window, "delete-event", G_CALLBACK(on_delete), editor);
    gtk_widget_show_all(box);
    set_tool(editor, TOOL_PEN);
    return editor;
}

void snip_editor_show(snip_editor *editor, cairo_surface_t *snip, double scale) {
    if (editor->snip != nullptr)
        cairo_surface_destroy(editor->snip);
    editor->snip = snip;
    editor->scale = scale > 0 ? scale : 1;
    editor->stroke = nullptr;
    editor->erasing = false;
    xd_ink_clear(&editor->ink);
    editor->unsaved = true;

    int width = (int)(cairo_image_surface_get_width(snip) / editor->scale);
    int height = (int)(cairo_image_surface_get_height(snip) / editor->scale);
    gtk_widget_set_size_request(editor->canvas, width, height);
    /* The window fits the snip, up to most of the screen; a bigger one
     * scrolls. */
    GdkDisplay *display = gtk_widget_get_display(editor->window);
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (monitor == nullptr)
        monitor = gdk_display_get_monitor(display, 0);
    int shown_width = width, shown_height = height;
    if (monitor != nullptr) {
        GdkRectangle area;
        gdk_monitor_get_workarea(monitor, &area);
        shown_width = MIN(width, (int)(area.width * MAX_SCREEN_SHARE));
        shown_height = MIN(height, (int)(area.height * MAX_SCREEN_SHARE));
    }
    GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW(editor->scroller);
    gtk_scrolled_window_set_min_content_width(scroller, shown_width);
    gtk_scrolled_window_set_min_content_height(scroller, shown_height);
    /* Shrink to the new snip, as far as the toolbar lets it. */
    gtk_window_resize(GTK_WINDOW(editor->window), 1, 1);
    gtk_widget_queue_draw(editor->canvas);
    if (editor->app->options.always_copy)
        snip_clipboard_copy(snip);
    snip_editor_present(editor);
}

void snip_editor_present(snip_editor *editor) {
    gtk_widget_show(editor->window);
    gtk_window_present(GTK_WINDOW(editor->window));
}

void snip_editor_hide(snip_editor *editor) {
    gtk_widget_hide(editor->window);
}
