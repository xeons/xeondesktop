/* SPDX-License-Identifier: MIT
 * xd-sound: Windows XP's Sounds and Audio Devices Properties, on the
 * sound server through libpulse (PulseAudio, or PipeWire's pulse server)
 * on GLib's main loop.
 *
 *   Volume    the default output's volume and mute; the taskbar's volume icon
 *   Sounds    the sound scheme (scheme.c)
 *   Audio     the default output and input devices
 *   Hardware  the sound cards
 *
 *   xd-sound [--page N]   0 Volume ... 3 Hardware
 */
#include <math.h>
#include <stdlib.h>

#include "../settings/soundplay.h"
#include "sound.h"

static constexpr char TITLE[] = "Sounds and Audio Devices";

/* ---- helpers --------------------------------------------------------------------- */

void sound_changed(props *properties) {
    if (!properties->updating)
        gtk_widget_set_sensitive(properties->apply, TRUE);
}

GtkWidget *sound_frame(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

GtkWidget *sound_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 52);
    return label;
}

static GtkWidget *icon_row(const char *icon, GtkWidget *content) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), content, TRUE, TRUE, 0);
    return row;
}

/* pavucontrol, for what XP's Volume Control and Advanced buttons did. */
static void open_mixer(GtkButton *button, gpointer data) {
    props *properties = data;
    const char *tab = g_object_get_data(G_OBJECT(button), "tab");
    char *argv[] = {g_strdup("pavucontrol"), g_strdup(tab), nullptr};
    GError *error = nullptr;
    bool ok = g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                            nullptr, &error);
    g_free(argv[0]);
    g_free(argv[1]);
    if (!ok) {
        xd_message_box(GTK_WINDOW(properties->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
    }
}

static GtkWidget *mixer_button(props *properties, const char *label, const char *tab) {
    GtkWidget *button = gtk_button_new_with_mnemonic(label);
    g_object_set_data_full(G_OBJECT(button), "tab", g_strdup(tab), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(open_mixer), properties);
    char *path = g_find_program_in_path("pavucontrol");
    gtk_widget_set_sensitive(button, path != nullptr);
    if (path == nullptr)
        gtk_widget_set_tooltip_text(button, "Install pavucontrol for the full volume controls.");
    g_free(path);
    return button;
}

/* ---- the sound server -------------------------------------------------------------- */

static void query(props *properties);

static void op_done([[maybe_unused]] pa_context *context, int success, void *data) {
    props *properties = data;
    if (!success)
        g_printerr("xd-sound: the sound server refused a change: %s\n",
                   pa_strerror(pa_context_errno(properties->ctx)));
    if (--properties->pending == 0 && properties->closing)
        gtk_main_quit();
}

static void track(props *properties, pa_operation *op) {
    if (op == nullptr)
        return;
    properties->pending++;
    pa_operation_unref(op);
}

static void sink_info([[maybe_unused]] pa_context *context, const pa_sink_info *info, int eol,
                      void *data) {
    props *properties = data;
    if (eol != 0 || info == nullptr || properties->sink == nullptr ||
        strcmp(info->name, properties->sink) != 0)
        return;
    gtk_label_set_text(GTK_LABEL(properties->device_name), info->description);
    gtk_widget_set_sensitive(properties->volume_box, TRUE);
    if (properties->volume_edited)
        return;
    properties->cv = info->volume;
    properties->updating = true;
    gtk_range_set_value(GTK_RANGE(properties->volume),
                        round(pa_cvolume_max(&info->volume) * 100.0 / PA_VOLUME_NORM));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->mute), info->mute != 0);
    properties->updating = false;
}

/* Refills a device list, keeping the user's choice while it's edited. */
static void devices_begin(GtkWidget *combo) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    const char *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    g_object_set_data_full(G_OBJECT(combo), "was", g_strdup(active), g_free);
    g_object_set_data_full(G_OBJECT(combo), "seen", seen, (GDestroyNotify)g_hash_table_unref);
}

static void device_add(props *properties, GtkWidget *combo, const char *name,
                       const char *description) {
    GHashTable *seen = g_object_get_data(G_OBJECT(combo), "seen");
    if (seen == nullptr || g_hash_table_contains(seen, name))
        return;
    if (g_hash_table_size(seen) == 0) {
        properties->updating = true;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
        properties->updating = false;
    }
    g_hash_table_add(seen, g_strdup(name));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), name, description);
}

static void devices_end(props *properties, GtkWidget *combo, const char *def) {
    GHashTable *seen = g_object_get_data(G_OBJECT(combo), "seen");
    if (seen != nullptr && g_hash_table_size(seen) == 0) {
        properties->updating = true;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
        properties->updating = false;
    }
    const char *was = g_object_get_data(G_OBJECT(combo), "was");
    properties->updating = true;
    if (!(properties->devices_edited && was != nullptr &&
          gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), was)) &&
        !(def != nullptr && gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), def)))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    properties->updating = false;
    gtk_widget_set_sensitive(combo, seen != nullptr && g_hash_table_size(seen) > 0);
    g_object_set_data(G_OBJECT(combo), "seen", nullptr);
}

static void sink_list([[maybe_unused]] pa_context *context, const pa_sink_info *info, int eol,
                      void *data) {
    props *properties = data;
    if (eol != 0) {
        devices_end(properties, properties->playback, properties->default_sink);
        return;
    }
    if (info != nullptr)
        device_add(properties, properties->playback, info->name, info->description);
    /* No default output (no session manager): the volume slider shows
     * the first one. */
    if (info != nullptr && properties->sink == nullptr) {
        properties->sink = g_strdup(info->name);
        sink_info(context, info, 0, properties);
    }
}

static void source_list([[maybe_unused]] pa_context *context, const pa_source_info *info, int eol,
                        void *data) {
    props *properties = data;
    if (eol != 0) {
        devices_end(properties, properties->recording, properties->default_source);
        return;
    }
    /* An output's monitor isn't a recording device of its own. */
    if (info != nullptr && info->monitor_of_sink == PA_INVALID_INDEX)
        device_add(properties, properties->recording, info->name, info->description);
}

enum { CARD_NAME, CARD_PROFILE, CARD_DRIVER, CARD_ID, CARD_COLS };

static void card_list([[maybe_unused]] pa_context *context, const pa_card_info *info, int eol,
                      void *data) {
    props *properties = data;
    if (eol != 0 || info == nullptr)
        return;
    const char *name = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_DESCRIPTION);
    const pa_card_profile_info2 *prof = info->active_profile2;
    gtk_list_store_insert_with_values(
        properties->cards, nullptr, -1, CARD_NAME, name != nullptr ? name : info->name,
        CARD_PROFILE, prof != nullptr ? prof->description : "", CARD_DRIVER,
        info->driver != nullptr ? info->driver : "", CARD_ID, info->name, -1);
}

static void server_info([[maybe_unused]] pa_context *context, const pa_server_info *info,
                        void *data) {
    props *properties = data;
    if (info == nullptr)
        return;
    g_free(properties->default_sink);
    g_free(properties->default_source);
    properties->default_sink = g_strdup(info->default_sink_name);
    properties->default_source = g_strdup(info->default_source_name);
    if (properties->default_sink != nullptr && !properties->volume_edited) {
        g_free(properties->sink);
        properties->sink = g_strdup(properties->default_sink);
    }
    if (properties->sink != nullptr) {
        pa_operation *op = pa_context_get_sink_info_by_name(properties->ctx, properties->sink,
                                                            sink_info, properties);
        if (op != nullptr)
            pa_operation_unref(op);
    }
    devices_begin(properties->playback);
    devices_begin(properties->recording);
    pa_operation *ops[] = {
        pa_context_get_sink_info_list(properties->ctx, sink_list, properties),
        pa_context_get_source_info_list(properties->ctx, source_list, properties),
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(ops); i++) {
        if (ops[i] != nullptr)
            pa_operation_unref(ops[i]);
    }
    gtk_list_store_clear(properties->cards);
    pa_operation *op = pa_context_get_card_info_list(properties->ctx, card_list, properties);
    if (op != nullptr)
        pa_operation_unref(op);
}

static void query(props *properties) {
    if (!properties->ready)
        return;
    pa_operation *op = pa_context_get_server_info(properties->ctx, server_info, properties);
    if (op != nullptr)
        pa_operation_unref(op);
}

static gboolean refresh_now(gpointer data) {
    props *properties = data;
    properties->refresh = 0;
    query(properties);
    return G_SOURCE_REMOVE;
}

static void subscribed([[maybe_unused]] pa_context *context,
                       [[maybe_unused]] pa_subscription_event_type_t event_type,
                       [[maybe_unused]] uint32_t index, void *data) {
    props *properties = data;
    /* Changes come in bursts (a device appearing): one query for them. */
    if (properties->refresh == 0)
        properties->refresh = g_timeout_add(100, refresh_now, properties);
}

static void no_server(props *properties) {
    gtk_label_set_text(GTK_LABEL(properties->device_name), "No sound server is running.");
    gtk_widget_set_sensitive(properties->volume_box, FALSE);
    gtk_widget_set_sensitive(properties->playback, FALSE);
    gtk_widget_set_sensitive(properties->recording, FALSE);
}

static void state_changed(pa_context *context, void *data) {
    props *properties = data;
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY: {
        properties->ready = true;
        pa_context_set_subscribe_callback(context, subscribed, properties);
        pa_operation *op = pa_context_subscribe(
            context,
            (pa_subscription_mask_t)(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
                                     PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_CARD),
            nullptr, nullptr);
        if (op != nullptr)
            pa_operation_unref(op);
        query(properties);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        properties->ready = false;
        no_server(properties);
        if (properties->closing)
            gtk_main_quit();
        break;
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        break;
    }
}

static void connect_server(props *properties) {
    properties->loop = pa_glib_mainloop_new(nullptr);
    pa_proplist *pl = pa_proplist_new();
    pa_proplist_sets(pl, PA_PROP_APPLICATION_ID, "org.xeondesktop.Sound");
    pa_proplist_sets(pl, PA_PROP_APPLICATION_ICON_NAME, "preferences-desktop-sound");
    properties->ctx =
        pa_context_new_with_proplist(pa_glib_mainloop_get_api(properties->loop), TITLE, pl);
    pa_proplist_free(pl);
    if (properties->ctx == nullptr) {
        no_server(properties);
        return;
    }
    pa_context_set_state_callback(properties->ctx, state_changed, properties);
    if (pa_context_connect(properties->ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
        no_server(properties);
}

/* ---- Volume ---------------------------------------------------------------------- */

static void on_volume([[maybe_unused]] GtkRange *range, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    properties->volume_edited = true;
    sound_changed(properties);
}

static void on_toggled([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    sound_changed(data);
}

static GtkWidget *volume_page(props *properties, const xd_settings *settings) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    properties->device_name = gtk_label_new("Connecting to the sound server...");
    gtk_label_set_xalign(GTK_LABEL(properties->device_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(properties->device_name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(properties->device_name, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(page), icon_row("audio-card", properties->device_name), FALSE, FALSE,
                       0);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    properties->volume_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *slider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(slider), gtk_label_new("Low"), FALSE, FALSE, 0);
    properties->volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(properties->volume), FALSE);
    for (int i = 0; i <= 100; i += 10)
        gtk_scale_add_mark(GTK_SCALE(properties->volume), i, GTK_POS_BOTTOM, nullptr);
    g_signal_connect(properties->volume, "value-changed", G_CALLBACK(on_volume), properties);
    gtk_box_pack_start(GTK_BOX(slider), properties->volume, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(slider), gtk_label_new("High"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(properties->volume_box), slider, FALSE, FALSE, 0);
    properties->mute = gtk_check_button_new_with_mnemonic("_Mute");
    g_signal_connect(properties->mute, "toggled", G_CALLBACK(on_volume), properties);
    gtk_box_pack_start(GTK_BOX(properties->volume_box), properties->mute, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(properties->volume_box, FALSE);
    gtk_box_pack_start(GTK_BOX(box), properties->volume_box, FALSE, FALSE, 0);
    properties->tray_icon = gtk_check_button_new_with_mnemonic("Place vo_lume icon in the taskbar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->tray_icon), settings->volume_icon);
    g_signal_connect(properties->tray_icon, "toggled", G_CALLBACK(on_toggled), properties);
    gtk_box_pack_start(GTK_BOX(box), properties->tray_icon, FALSE, FALSE, 0);
    GtkWidget *adv = mixer_button(properties, "Ad_vanced...", "--tab=3");
    gtk_widget_set_halign(adv, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(box), adv, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       sound_frame("Device volume", icon_row("audio-volume-high", box)), FALSE,
                       FALSE, 0);

    GtkWidget *apps = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(apps),
                       sound_label("Use the settings below to change the volume of each program "
                                   "and the levels of each speaker."),
                       FALSE, FALSE, 0);
    GtkWidget *button = mixer_button(properties, "_Program Volume...", "--tab=1");
    gtk_widget_set_halign(button, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(apps), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), sound_frame("Speaker settings", icon_row("audio-speakers", apps)),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- Audio ----------------------------------------------------------------------- */

static void on_device([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    properties->devices_edited = true;
    sound_changed(properties);
}

static GtkWidget *device_group(props *properties, const char *title, const char *icon,
                               GtkWidget **combo, const char *tab) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Default device:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    *combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(*combo, 280, -1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), *combo);
    g_signal_connect(*combo, "changed", G_CALLBACK(on_device), properties);
    gtk_box_pack_start(GTK_BOX(box), *combo, FALSE, FALSE, 0);
    GtkWidget *button = mixer_button(properties, "_Volume...", tab);
    gtk_widget_set_halign(button, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    return sound_frame(title, icon_row(icon, box));
}

static GtkWidget *audio_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       device_group(properties, "Sound playback", "audio-speakers",
                                    &properties->playback, "--tab=3"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       device_group(properties, "Sound recording", "audio-input-microphone",
                                    &properties->recording, "--tab=4"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       sound_label("Programs that play to the default device move to the new "
                                   "one when you change it."),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- Hardware -------------------------------------------------------------------- */

static GtkWidget *hardware_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *label = gtk_label_new_with_mnemonic("De_vices:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, FALSE, 0);
    properties->cards =
        gtk_list_store_new(CARD_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(properties->cards));
    static const char *const titles[] = {"Name", "Profile", "Driver"};
    for (int i = 0; i < 3; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *col =
            gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_expand(col, i < 2);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    }
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), view);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 180);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       sound_label("A card's profile picks its outputs and inputs (stereo, "
                                   "surround, digital, a headset's microphone)."),
                       FALSE, FALSE, 0);
    GtkWidget *button = mixer_button(properties, "P_roperties", "--tab=5");
    gtk_widget_set_halign(button, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(page), button, FALSE, FALSE, 0);
    return page;
}

/* ---- applying -------------------------------------------------------------------- */

static void save_settings(props *properties) {
    /* Re-read: another applet may have saved since we opened. */
    xd_settings settings;
    xd_settings_load(&settings);
    bool icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->tray_icon));
    if (settings.volume_icon == icon)
        return;
    settings.volume_icon = icon;
    if (!xd_settings_save(&settings))
        xd_message_box(GTK_WINDOW(properties->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The settings could not be saved.");
}

static bool apply(props *properties) {
    if (properties->ready && properties->volume_edited && properties->sink != nullptr) {
        pa_cvolume cv = properties->cv;
        double pct = gtk_range_get_value(GTK_RANGE(properties->volume));
        if (cv.channels == 0)
            pa_cvolume_set(&cv, 2, PA_VOLUME_NORM);
        /* Keeps the balance between channels. */
        pa_cvolume_scale(&cv, (pa_volume_t)lround(pct * (double)PA_VOLUME_NORM / 100.0));
        track(properties, pa_context_set_sink_volume_by_name(properties->ctx, properties->sink, &cv,
                                                             op_done, properties));
        bool mute = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->mute));
        track(properties, pa_context_set_sink_mute_by_name(properties->ctx, properties->sink, mute,
                                                           op_done, properties));
        properties->cv = cv;
        properties->volume_edited = false;
    }
    if (properties->ready && properties->devices_edited) {
        const char *sink = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->playback));
        const char *source = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->recording));
        if (sink != nullptr && g_strcmp0(sink, properties->default_sink) != 0)
            track(properties,
                  pa_context_set_default_sink(properties->ctx, sink, op_done, properties));
        if (source != nullptr && g_strcmp0(source, properties->default_source) != 0)
            track(properties,
                  pa_context_set_default_source(properties->ctx, source, op_done, properties));
        properties->devices_edited = false;
    }
    save_settings(properties);
    if (!sounds_apply(properties))
        return false;
    gtk_widget_set_sensitive(properties->apply, FALSE);
    return true;
}

static gboolean give_up(gpointer data) {
    [[maybe_unused]] props *properties = data;
    g_printerr("xd-sound: the sound server didn't answer; closing anyway\n");
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void on_response(GtkDialog *gtk_dialog, int response, gpointer data) {
    props *properties = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply) &&
        !apply(properties))
        return;
    /* Wait (a little) for the sound server to take the changes. */
    gtk_widget_hide(GTK_WIDGET(gtk_dialog));
    if (properties->pending == 0) {
        gtk_main_quit();
        return;
    }
    properties->closing = true;
    g_timeout_add_seconds(3, give_up, properties);
}

static gboolean on_delete(GtkWidget *widget, [[maybe_unused]] GdkEvent *gdk_event, gpointer data) {
    on_response(GTK_DIALOG(widget), GTK_RESPONSE_CANCEL, data);
    return TRUE;
}

int main(int argc, char **argv) {
    int page = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
    }
    xd_gtk_app_preinit("org.xeondesktop.Sound");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props properties;
    xd_settings settings;
    xd_settings_load(&settings);

    properties.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties.window), "Sounds and Audio Devices Properties");
    gtk_window_set_icon_name(GTK_WINDOW(properties.window), "preferences-desktop-sound");
    gtk_window_set_resizable(GTK_WINDOW(properties.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "Cancel", GTK_RESPONSE_CANCEL);
    properties.apply =
        gtk_dialog_add_button(GTK_DIALOG(properties.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(properties.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(properties.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), volume_page(&properties, &settings),
                             gtk_label_new_with_mnemonic("V_olume"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), sounds_page(&properties),
                             gtk_label_new_with_mnemonic("_Sounds"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), audio_page(&properties),
                             gtk_label_new_with_mnemonic("A_udio"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), hardware_page(&properties),
                             gtk_label_new_with_mnemonic("_Hardware"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties.window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties.window, GTK_NOTEBOOK(tabs));
    g_signal_connect(properties.window, "response", G_CALLBACK(on_response), &properties);
    g_signal_connect(properties.window, "delete-event", G_CALLBACK(on_delete), &properties);
    gtk_widget_show_all(properties.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), XD_CLAMP(page, 0, 3));
    connect_server(&properties);
    gtk_main();

    if (properties.ctx != nullptr) {
        pa_context_set_state_callback(properties.ctx, nullptr, nullptr);
        pa_context_disconnect(properties.ctx);
        pa_context_unref(properties.ctx);
    }
    if (properties.loop != nullptr)
        pa_glib_mainloop_free(properties.loop);
    xd_sound_finish();
    xd_sound_scheme_free(&properties.sounds);
    g_free(properties.sink);
    g_free(properties.default_sink);
    g_free(properties.default_source);
    return EXIT_SUCCESS;
}
