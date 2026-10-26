/* SPDX-License-Identifier: MIT
 * The Sounds tab: the sound scheme (Windows Default or No Sounds, and
 * the user's changes to it), the program events and the sound each one
 * plays, a sound from the sound theme or any file.
 */
#include <stdlib.h>

#include "../settings/soundplay.h"
#include "sound.h"

enum { COL_LABEL, COL_EVENT, COL_ICON, N_COLS };

static const char *const base_labels[] = {"Windows Default", "No Sounds"};

static int compare_names(const void *left, const void *right) {
    return g_ascii_strcasecmp(*(const char *const *)left, *(const char *const *)right);
}

/* The theme's sounds, by name: XP listed C:\WINDOWS\Media here. */
static char **theme_sounds(void) {
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "sounds", "freedesktop",
                                           "stereo", nullptr));
    for (const char *const *data_dir = g_get_system_data_dirs(); *data_dir != nullptr; data_dir++)
        g_ptr_array_add(dirs,
                        g_build_filename(*data_dir, "sounds", "freedesktop", "stereo", nullptr));
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (guint i = 0; i < dirs->len; i++) {
        GDir *dir = g_dir_open(g_ptr_array_index(dirs, i), 0, nullptr);
        const char *name;
        while (dir != nullptr && (name = g_dir_read_name(dir)) != nullptr) {
            const char *dot = strrchr(name, '.');
            if (dot == nullptr || (strcmp(dot, ".oga") != 0 && strcmp(dot, ".ogg") != 0 &&
                                   strcmp(dot, ".wav") != 0))
                continue;
            g_hash_table_add(seen, g_strndup(name, (gsize)(dot - name)));
        }
        if (dir != nullptr)
            g_dir_close(dir);
    }
    g_ptr_array_free(dirs, TRUE);
    guint count = 0;
    char **names = (char **)g_hash_table_get_keys_as_array(seen, &count);
    g_hash_table_steal_all(seen); /* the array owns the names now */
    g_hash_table_unref(seen);
    qsort(names, count, sizeof(char *), compare_names);
    return names;
}

static int selected_event(props *properties) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->events));
    GtkTreeModel *model;
    GtkTreeIter it;
    int ev = -1;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, COL_EVENT, &ev, -1);
    return ev;
}

static const char *row_icon(const char *sound) {
    return xd_sound_kind_of(sound) == XD_SOUND_NONE ? nullptr : "audio-volume-high";
}

/* The event rows' speaker icons and the scheme's "(modified)". */
static void show_scheme(props *properties) {
    GtkTreeModel *model = GTK_TREE_MODEL(properties->event_store);
    GtkTreeIter group, it;
    for (bool has_group = gtk_tree_model_get_iter_first(model, &group); has_group;
         has_group = gtk_tree_model_iter_next(model, &group)) {
        for (bool has_child = gtk_tree_model_iter_children(model, &it, &group); has_child;
             has_child = gtk_tree_model_iter_next(model, &it)) {
            int ev = -1;
            gtk_tree_model_get(model, &it, COL_EVENT, &ev, -1);
            if (ev >= 0)
                gtk_tree_store_set(properties->event_store, &it, COL_ICON,
                                   row_icon(properties->sounds.sounds[ev]), -1);
        }
    }
    GtkListStore *schemes =
        GTK_LIST_STORE(gtk_combo_box_get_model(GTK_COMBO_BOX(properties->scheme)));
    GtkTreeIter row;
    for (bool has_row = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(schemes), &row); has_row;
         has_row = gtk_tree_model_iter_next(GTK_TREE_MODEL(schemes), &row)) {
        int base = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(schemes), &row, 1, &base, -1);
        char *text =
            base == (int)properties->sounds.base && xd_sound_scheme_modified(&properties->sounds)
                ? g_strdup_printf("%s (modified)", base_labels[base])
                : g_strdup(base_labels[base]);
        gtk_list_store_set(schemes, &row, 0, text, -1);
        g_free(text);
    }
    properties->updating = true;
    gtk_combo_box_set_active(GTK_COMBO_BOX(properties->scheme), (int)properties->sounds.base);
    properties->updating = false;
}

/* The Sounds box: (None), the theme's sounds, and the event's own file. */
static void show_event(props *properties) {
    int ev = selected_event(properties);
    properties->updating = true;
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(properties->sound);
    gtk_combo_box_text_remove_all(combo);
    bool on = ev >= 0;
    gtk_widget_set_sensitive(properties->sound, on);
    gtk_widget_set_sensitive(properties->sound_label, on);
    gtk_widget_set_sensitive(properties->browse, on);
    if (on) {
        const char *current = properties->sounds.sounds[ev];
        gtk_combo_box_text_append(combo, "", "(None)");
        char **names = theme_sounds();
        for (char **name = names; *name != nullptr; name++)
            gtk_combo_box_text_append(combo, *name, *name);
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), current)) {
            /* A file, or a theme sound that isn't installed. */
            char *label = xd_sound_kind_of(current) == XD_SOUND_FILE
                              ? g_path_get_basename(current)
                              : g_strdup(current);
            gtk_combo_box_text_append(combo, current, label);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), current);
            g_free(label);
        }
        g_strfreev(names);
        gtk_widget_set_sensitive(properties->play, xd_sound_kind_of(current) != XD_SOUND_NONE);
    } else {
        gtk_widget_set_sensitive(properties->play, FALSE);
    }
    properties->updating = false;
}

static void set_sound(props *properties, int ev, const char *sound) {
    if (ev < 0 || !xd_sound_scheme_set(&properties->sounds, (xd_sound_event)ev, sound))
        return;
    properties->sounds_edited = true;
    show_scheme(properties);
    sound_changed(properties);
}

static void on_event_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    show_event(data);
}

static void on_sound([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->sound));
    int ev = selected_event(properties);
    set_sound(properties, ev, id != nullptr ? id : "");
    gtk_widget_set_sensitive(properties->play, xd_sound_kind_of(id) != XD_SOUND_NONE);
}

static void on_scheme([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    int base = gtk_combo_box_get_active(GTK_COMBO_BOX(properties->scheme));
    if (base < 0 || (size_t)base >= XD_ARRAY_LEN(base_labels))
        return;
    xd_sound_scheme_free(&properties->sounds);
    xd_sound_scheme_init(&properties->sounds, (xd_sound_base)base);
    properties->sounds_edited = true;
    show_scheme(properties);
    show_event(properties);
    sound_changed(properties);
}

static void played(const char *error, void *data) {
    props *properties = data;
    if (error == nullptr || properties->closing)
        return;
    char *text = g_strdup_printf("The sound could not be played.\n\n%s", error);
    xd_message_box(GTK_WINDOW(properties->window), "Sounds and Audio Devices", XD_MB_ERROR,
                   XD_MB_OK, FALSE, text);
    g_free(text);
}

static void on_play([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    int ev = selected_event(properties);
    if (ev >= 0)
        xd_sound_play(properties->sounds.sounds[ev], xd_sound_event_get((xd_sound_event)ev)->label,
                      played, properties);
}

static void on_browse([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    int ev = selected_event(properties);
    if (ev < 0)
        return;
    char *title = g_strdup_printf("Browse for %s sound",
                                  xd_sound_event_get((xd_sound_event)ev)->label);
    GtkFileChooserNative *file_chooser =
        gtk_file_chooser_native_new(title, GTK_WINDOW(properties->window),
                                    GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    g_free(title);
    GtkFileFilter *sounds = gtk_file_filter_new();
    gtk_file_filter_set_name(sounds, "Sounds");
    gtk_file_filter_add_mime_type(sounds, "audio/*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), sounds);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All Files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), all);
    const char *current = properties->sounds.sounds[ev];
    if (xd_sound_kind_of(current) == XD_SOUND_FILE)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(file_chooser), current);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(file_chooser)) == GTK_RESPONSE_ACCEPT) {
        char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(file_chooser));
        if (file != nullptr) {
            set_sound(properties, ev, file);
            show_event(properties);
        }
        g_free(file);
    }
    g_object_unref(file_chooser);
}

bool sounds_apply(props *properties) {
    if (!properties->sounds_edited)
        return true;
    if (!xd_sound_scheme_save(&properties->sounds)) {
        xd_message_box(GTK_WINDOW(properties->window), "Sounds and Audio Devices", XD_MB_ERROR,
                       XD_MB_OK, FALSE, "The sound scheme could not be saved.");
        return false;
    }
    properties->sounds_edited = false;
    return true;
}

GtkWidget *sounds_page(props *properties) {
    xd_sound_scheme_load(&properties->sounds);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       sound_label("A sound scheme is a set of sounds applied to events in "
                                   "XeonDesktop and programs. You can select an existing scheme "
                                   "or change the sounds of the one you have."),
                       FALSE, FALSE, 0);
    GtkWidget *scheme_label = gtk_label_new_with_mnemonic("Sound sc_heme:");
    gtk_label_set_xalign(GTK_LABEL(scheme_label), 0);
    gtk_box_pack_start(GTK_BOX(page), scheme_label, FALSE, FALSE, 0);
    GtkListStore *schemes = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
    for (size_t i = 0; i < XD_ARRAY_LEN(base_labels); i++)
        gtk_list_store_insert_with_values(schemes, nullptr, -1, 0, base_labels[i], 1, (int)i, -1);
    properties->scheme = gtk_combo_box_new_with_model(GTK_TREE_MODEL(schemes));
    g_object_unref(schemes);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(properties->scheme), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(properties->scheme), text, "text", 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(scheme_label), properties->scheme);
    g_signal_connect(properties->scheme, "changed", G_CALLBACK(on_scheme), properties);
    gtk_box_pack_start(GTK_BOX(page), properties->scheme, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page),
                       sound_label("To change sounds, click a program event in the following "
                                   "list and then select a sound to apply. You can save the "
                                   "changes as a new sound scheme."),
                       FALSE, FALSE, 0);
    scheme_label = gtk_label_new_with_mnemonic("Program _events:");
    gtk_label_set_xalign(GTK_LABEL(scheme_label), 0);
    gtk_box_pack_start(GTK_BOX(page), scheme_label, FALSE, FALSE, 0);
    properties->event_store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    const char *program = nullptr;
    GtkTreeIter group;
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++) {
        const xd_sound_event_info *info = xd_sound_event_get((xd_sound_event)i);
        if (program == nullptr || strcmp(program, info->program) != 0) {
            program = info->program;
            gtk_tree_store_insert_with_values(
                properties->event_store, &group, nullptr, -1, COL_LABEL, program, COL_EVENT, -1,
                COL_ICON, strcmp(program, "Windows") == 0 ? "computer" : "system-file-manager", -1);
        }
        gtk_tree_store_insert_with_values(properties->event_store, nullptr, &group, -1, COL_LABEL,
                                          info->label, COL_EVENT, (int)i, -1);
    }
    properties->events = gtk_tree_view_new_with_model(GTK_TREE_MODEL(properties->event_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(properties->events), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", COL_ICON);
    GtkCellRenderer *label = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, label, TRUE);
    gtk_tree_view_column_add_attribute(col, label, "text", COL_LABEL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(properties->events), col);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(properties->events));
    gtk_label_set_mnemonic_widget(GTK_LABEL(scheme_label), properties->events);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->events)), "changed",
                     G_CALLBACK(on_event_selected), properties);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 150);
    gtk_container_add(GTK_CONTAINER(scroll), properties->events);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    properties->sound_label = gtk_label_new_with_mnemonic("_Sounds:");
    gtk_label_set_xalign(GTK_LABEL(properties->sound_label), 0);
    gtk_box_pack_start(GTK_BOX(page), properties->sound_label, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    properties->sound = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(properties->sound_label), properties->sound);
    g_signal_connect(properties->sound, "changed", G_CALLBACK(on_sound), properties);
    gtk_box_pack_start(GTK_BOX(row), properties->sound, TRUE, TRUE, 0);
    properties->play = gtk_button_new_from_icon_name("media-playback-start", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(properties->play, "Play Sound");
    g_signal_connect(properties->play, "clicked", G_CALLBACK(on_play), properties);
    gtk_box_pack_start(GTK_BOX(row), properties->play, FALSE, FALSE, 0);
    properties->browse = gtk_button_new_with_mnemonic("_Browse...");
    g_signal_connect(properties->browse, "clicked", G_CALLBACK(on_browse), properties);
    gtk_box_pack_start(GTK_BOX(row), properties->browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    show_scheme(properties);
    show_event(properties);
    return page;
}
