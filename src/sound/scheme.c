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

static int compare_names(const void *a, const void *b) {
    return g_ascii_strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

/* The theme's sounds, by name: XP listed C:\WINDOWS\Media here. */
static char **theme_sounds(void) {
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "sounds", "freedesktop",
                                           "stereo", nullptr));
    for (const char *const *d = g_get_system_data_dirs(); *d != nullptr; d++)
        g_ptr_array_add(dirs, g_build_filename(*d, "sounds", "freedesktop", "stereo", nullptr));
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
    guint n = 0;
    char **names = (char **)g_hash_table_get_keys_as_array(seen, &n);
    g_hash_table_steal_all(seen); /* the array owns the names now */
    g_hash_table_unref(seen);
    qsort(names, n, sizeof(char *), compare_names);
    return names;
}

static int selected_event(props *p) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->events));
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
static void show_scheme(props *p) {
    GtkTreeModel *m = GTK_TREE_MODEL(p->event_store);
    GtkTreeIter group, it;
    for (bool g = gtk_tree_model_get_iter_first(m, &group); g; g = gtk_tree_model_iter_next(m, &group)) {
        for (bool c = gtk_tree_model_iter_children(m, &it, &group); c;
             c = gtk_tree_model_iter_next(m, &it)) {
            int ev = -1;
            gtk_tree_model_get(m, &it, COL_EVENT, &ev, -1);
            if (ev >= 0)
                gtk_tree_store_set(p->event_store, &it, COL_ICON, row_icon(p->sounds.sounds[ev]),
                                   -1);
        }
    }
    GtkListStore *schemes = GTK_LIST_STORE(gtk_combo_box_get_model(GTK_COMBO_BOX(p->scheme)));
    GtkTreeIter row;
    for (bool r = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(schemes), &row); r;
         r = gtk_tree_model_iter_next(GTK_TREE_MODEL(schemes), &row)) {
        int base = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(schemes), &row, 1, &base, -1);
        char *text = base == (int)p->sounds.base && xd_sound_scheme_modified(&p->sounds)
                         ? g_strdup_printf("%s (modified)", base_labels[base])
                         : g_strdup(base_labels[base]);
        gtk_list_store_set(schemes, &row, 0, text, -1);
        g_free(text);
    }
    p->updating = true;
    gtk_combo_box_set_active(GTK_COMBO_BOX(p->scheme), (int)p->sounds.base);
    p->updating = false;
}

/* The Sounds box: (None), the theme's sounds, and the event's own file. */
static void show_event(props *p) {
    int ev = selected_event(p);
    p->updating = true;
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(p->sound);
    gtk_combo_box_text_remove_all(combo);
    bool on = ev >= 0;
    gtk_widget_set_sensitive(p->sound, on);
    gtk_widget_set_sensitive(p->sound_label, on);
    gtk_widget_set_sensitive(p->browse, on);
    if (on) {
        const char *current = p->sounds.sounds[ev];
        gtk_combo_box_text_append(combo, "", "(None)");
        char **names = theme_sounds();
        for (char **n = names; *n != nullptr; n++)
            gtk_combo_box_text_append(combo, *n, *n);
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
        gtk_widget_set_sensitive(p->play, xd_sound_kind_of(current) != XD_SOUND_NONE);
    } else {
        gtk_widget_set_sensitive(p->play, FALSE);
    }
    p->updating = false;
}

static void set_sound(props *p, int ev, const char *sound) {
    if (ev < 0 || !xd_sound_scheme_set(&p->sounds, (xd_sound_event)ev, sound))
        return;
    p->sounds_edited = true;
    show_scheme(p);
    sound_changed(p);
}

static void on_event_selected([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    show_event(data);
}

static void on_sound([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->sound));
    int ev = selected_event(p);
    set_sound(p, ev, id != nullptr ? id : "");
    gtk_widget_set_sensitive(p->play, xd_sound_kind_of(id) != XD_SOUND_NONE);
}

static void on_scheme([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    int base = gtk_combo_box_get_active(GTK_COMBO_BOX(p->scheme));
    if (base < 0 || (size_t)base >= XD_ARRAY_LEN(base_labels))
        return;
    xd_sound_scheme_free(&p->sounds);
    xd_sound_scheme_init(&p->sounds, (xd_sound_base)base);
    p->sounds_edited = true;
    show_scheme(p);
    show_event(p);
    sound_changed(p);
}

static void played(const char *error, void *data) {
    props *p = data;
    if (error == nullptr || p->closing)
        return;
    char *text = g_strdup_printf("The sound could not be played.\n\n%s", error);
    xd_message_box(GTK_WINDOW(p->window), "Sounds and Audio Devices", XD_MB_ERROR, XD_MB_OK, FALSE,
                   text);
    g_free(text);
}

static void on_play([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    int ev = selected_event(p);
    if (ev >= 0)
        xd_sound_play(p->sounds.sounds[ev], xd_sound_event_get((xd_sound_event)ev)->label, played,
                      p);
}

static void on_browse([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    int ev = selected_event(p);
    if (ev < 0)
        return;
    char *title = g_strdup_printf("Browse for %s sound",
                                  xd_sound_event_get((xd_sound_event)ev)->label);
    GtkWidget *d = gtk_file_chooser_dialog_new(title, GTK_WINDOW(p->window),
                                               GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
                                               GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT,
                                               nullptr);
    g_free(title);
    GtkFileFilter *sounds = gtk_file_filter_new();
    gtk_file_filter_set_name(sounds, "Sounds");
    gtk_file_filter_add_mime_type(sounds, "audio/*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), sounds);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All Files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), all);
    const char *current = p->sounds.sounds[ev];
    if (xd_sound_kind_of(current) == XD_SOUND_FILE)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(d), current);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (file != nullptr) {
            set_sound(p, ev, file);
            show_event(p);
        }
        g_free(file);
    }
    gtk_widget_destroy(d);
}

bool sounds_apply(props *p) {
    if (!p->sounds_edited)
        return true;
    if (!xd_sound_scheme_save(&p->sounds)) {
        xd_message_box(GTK_WINDOW(p->window), "Sounds and Audio Devices", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "The sound scheme could not be saved.");
        return false;
    }
    p->sounds_edited = false;
    return true;
}

GtkWidget *sounds_page(props *p) {
    xd_sound_scheme_load(&p->sounds);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       sound_label("A sound scheme is a set of sounds applied to events in "
                                   "XeonDesktop and programs. You can select an existing scheme "
                                   "or change the sounds of the one you have."),
                       FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new_with_mnemonic("Sound sc_heme:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(page), l, FALSE, FALSE, 0);
    GtkListStore *schemes = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
    for (size_t i = 0; i < XD_ARRAY_LEN(base_labels); i++)
        gtk_list_store_insert_with_values(schemes, nullptr, -1, 0, base_labels[i], 1, (int)i, -1);
    p->scheme = gtk_combo_box_new_with_model(GTK_TREE_MODEL(schemes));
    g_object_unref(schemes);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(p->scheme), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(p->scheme), text, "text", 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), p->scheme);
    g_signal_connect(p->scheme, "changed", G_CALLBACK(on_scheme), p);
    gtk_box_pack_start(GTK_BOX(page), p->scheme, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page),
                       sound_label("To change sounds, click a program event in the following "
                                   "list and then select a sound to apply. You can save the "
                                   "changes as a new sound scheme."),
                       FALSE, FALSE, 0);
    l = gtk_label_new_with_mnemonic("Program _events:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(page), l, FALSE, FALSE, 0);
    p->event_store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    const char *program = nullptr;
    GtkTreeIter group;
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++) {
        const xd_sound_event_info *info = xd_sound_event_get((xd_sound_event)i);
        if (program == nullptr || strcmp(program, info->program) != 0) {
            program = info->program;
            gtk_tree_store_insert_with_values(p->event_store, &group, nullptr, -1, COL_LABEL,
                                              program, COL_EVENT, -1, COL_ICON,
                                              strcmp(program, "Windows") == 0
                                                  ? "computer"
                                                  : "system-file-manager",
                                              -1);
        }
        gtk_tree_store_insert_with_values(p->event_store, nullptr, &group, -1, COL_LABEL,
                                          info->label, COL_EVENT, (int)i, -1);
    }
    p->events = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->event_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->events), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", COL_ICON);
    GtkCellRenderer *label = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, label, TRUE);
    gtk_tree_view_column_add_attribute(col, label, "text", COL_LABEL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->events), col);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(p->events));
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), p->events);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->events)), "changed",
                     G_CALLBACK(on_event_selected), p);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 150);
    gtk_container_add(GTK_CONTAINER(scroll), p->events);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    p->sound_label = gtk_label_new_with_mnemonic("_Sounds:");
    gtk_label_set_xalign(GTK_LABEL(p->sound_label), 0);
    gtk_box_pack_start(GTK_BOX(page), p->sound_label, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->sound = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(p->sound_label), p->sound);
    g_signal_connect(p->sound, "changed", G_CALLBACK(on_sound), p);
    gtk_box_pack_start(GTK_BOX(row), p->sound, TRUE, TRUE, 0);
    p->play = gtk_button_new_from_icon_name("media-playback-start", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(p->play, "Play Sound");
    g_signal_connect(p->play, "clicked", G_CALLBACK(on_play), p);
    gtk_box_pack_start(GTK_BOX(row), p->play, FALSE, FALSE, 0);
    p->browse = gtk_button_new_with_mnemonic("_Browse...");
    g_signal_connect(p->browse, "clicked", G_CALLBACK(on_browse), p);
    gtk_box_pack_start(GTK_BOX(row), p->browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    show_scheme(p);
    show_event(p);
    return page;
}
