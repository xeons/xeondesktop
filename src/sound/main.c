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

void sound_changed(props *p) {
    if (!p->updating)
        gtk_widget_set_sensitive(p->apply, TRUE);
}

GtkWidget *sound_frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

GtkWidget *sound_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 52);
    return l;
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
static void open_mixer(GtkButton *b, gpointer data) {
    props *p = data;
    const char *tab = g_object_get_data(G_OBJECT(b), "tab");
    char *argv[] = {g_strdup("pavucontrol"), g_strdup(tab), nullptr};
    GError *error = nullptr;
    bool ok = g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                            nullptr, &error);
    g_free(argv[0]);
    g_free(argv[1]);
    if (!ok) {
        xd_message_box(GTK_WINDOW(p->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
    }
}

static GtkWidget *mixer_button(props *p, const char *label, const char *tab) {
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    g_object_set_data_full(G_OBJECT(b), "tab", g_strdup(tab), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(open_mixer), p);
    char *path = g_find_program_in_path("pavucontrol");
    gtk_widget_set_sensitive(b, path != nullptr);
    if (path == nullptr)
        gtk_widget_set_tooltip_text(b, "Install pavucontrol for the full volume controls.");
    g_free(path);
    return b;
}

/* ---- the sound server -------------------------------------------------------------- */

static void query(props *p);

static void op_done([[maybe_unused]] pa_context *c, int success, void *data) {
    props *p = data;
    if (!success)
        g_printerr("xd-sound: the sound server refused a change: %s\n",
                   pa_strerror(pa_context_errno(p->ctx)));
    if (--p->pending == 0 && p->closing)
        gtk_main_quit();
}

static void track(props *p, pa_operation *op) {
    if (op == nullptr)
        return;
    p->pending++;
    pa_operation_unref(op);
}

static void sink_info([[maybe_unused]] pa_context *c, const pa_sink_info *info, int eol,
                      void *data) {
    props *p = data;
    if (eol != 0 || info == nullptr || p->sink == nullptr || strcmp(info->name, p->sink) != 0)
        return;
    gtk_label_set_text(GTK_LABEL(p->device_name), info->description);
    gtk_widget_set_sensitive(p->volume_box, TRUE);
    if (p->volume_edited)
        return;
    p->cv = info->volume;
    p->updating = true;
    gtk_range_set_value(GTK_RANGE(p->volume),
                        round(pa_cvolume_max(&info->volume) * 100.0 / PA_VOLUME_NORM));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->mute), info->mute != 0);
    p->updating = false;
}

/* Refills a device list, keeping the user's choice while it's edited. */
static void devices_begin(GtkWidget *combo) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    const char *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    g_object_set_data_full(G_OBJECT(combo), "was", g_strdup(active), g_free);
    g_object_set_data_full(G_OBJECT(combo), "seen", seen, (GDestroyNotify)g_hash_table_unref);
}

static void device_add(props *p, GtkWidget *combo, const char *name, const char *description) {
    GHashTable *seen = g_object_get_data(G_OBJECT(combo), "seen");
    if (seen == nullptr || g_hash_table_contains(seen, name))
        return;
    if (g_hash_table_size(seen) == 0) {
        p->updating = true;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
        p->updating = false;
    }
    g_hash_table_add(seen, g_strdup(name));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), name, description);
}

static void devices_end(props *p, GtkWidget *combo, const char *def) {
    GHashTable *seen = g_object_get_data(G_OBJECT(combo), "seen");
    if (seen != nullptr && g_hash_table_size(seen) == 0) {
        p->updating = true;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
        p->updating = false;
    }
    const char *was = g_object_get_data(G_OBJECT(combo), "was");
    p->updating = true;
    if (!(p->devices_edited && was != nullptr &&
          gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), was)) &&
        !(def != nullptr && gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), def)))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    p->updating = false;
    gtk_widget_set_sensitive(combo, seen != nullptr && g_hash_table_size(seen) > 0);
    g_object_set_data(G_OBJECT(combo), "seen", nullptr);
}

static void sink_list([[maybe_unused]] pa_context *c, const pa_sink_info *info, int eol,
                      void *data) {
    props *p = data;
    if (eol != 0) {
        devices_end(p, p->playback, p->default_sink);
        return;
    }
    if (info != nullptr)
        device_add(p, p->playback, info->name, info->description);
    /* No default output (no session manager): the volume slider shows
     * the first one. */
    if (info != nullptr && p->sink == nullptr) {
        p->sink = g_strdup(info->name);
        sink_info(c, info, 0, p);
    }
}

static void source_list([[maybe_unused]] pa_context *c, const pa_source_info *info, int eol,
                        void *data) {
    props *p = data;
    if (eol != 0) {
        devices_end(p, p->recording, p->default_source);
        return;
    }
    /* An output's monitor isn't a recording device of its own. */
    if (info != nullptr && info->monitor_of_sink == PA_INVALID_INDEX)
        device_add(p, p->recording, info->name, info->description);
}

enum { CARD_NAME, CARD_PROFILE, CARD_DRIVER, CARD_ID, CARD_COLS };

static void card_list([[maybe_unused]] pa_context *c, const pa_card_info *info, int eol,
                      void *data) {
    props *p = data;
    if (eol != 0 || info == nullptr)
        return;
    const char *name = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_DESCRIPTION);
    const pa_card_profile_info2 *prof = info->active_profile2;
    gtk_list_store_insert_with_values(p->cards, nullptr, -1, CARD_NAME,
                                      name != nullptr ? name : info->name, CARD_PROFILE,
                                      prof != nullptr ? prof->description : "", CARD_DRIVER,
                                      info->driver != nullptr ? info->driver : "", CARD_ID,
                                      info->name, -1);
}

static void server_info([[maybe_unused]] pa_context *c, const pa_server_info *info, void *data) {
    props *p = data;
    if (info == nullptr)
        return;
    g_free(p->default_sink);
    g_free(p->default_source);
    p->default_sink = g_strdup(info->default_sink_name);
    p->default_source = g_strdup(info->default_source_name);
    if (p->default_sink != nullptr && !p->volume_edited) {
        g_free(p->sink);
        p->sink = g_strdup(p->default_sink);
    }
    if (p->sink != nullptr) {
        pa_operation *op = pa_context_get_sink_info_by_name(p->ctx, p->sink, sink_info, p);
        if (op != nullptr)
            pa_operation_unref(op);
    }
    devices_begin(p->playback);
    devices_begin(p->recording);
    pa_operation *ops[] = {
        pa_context_get_sink_info_list(p->ctx, sink_list, p),
        pa_context_get_source_info_list(p->ctx, source_list, p),
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(ops); i++) {
        if (ops[i] != nullptr)
            pa_operation_unref(ops[i]);
    }
    gtk_list_store_clear(p->cards);
    pa_operation *op = pa_context_get_card_info_list(p->ctx, card_list, p);
    if (op != nullptr)
        pa_operation_unref(op);
}

static void query(props *p) {
    if (!p->ready)
        return;
    pa_operation *op = pa_context_get_server_info(p->ctx, server_info, p);
    if (op != nullptr)
        pa_operation_unref(op);
}

static gboolean refresh_now(gpointer data) {
    props *p = data;
    p->refresh = 0;
    query(p);
    return G_SOURCE_REMOVE;
}

static void subscribed([[maybe_unused]] pa_context *c, [[maybe_unused]] pa_subscription_event_type_t t,
                       [[maybe_unused]] uint32_t index, void *data) {
    props *p = data;
    /* Changes come in bursts (a device appearing): one query for them. */
    if (p->refresh == 0)
        p->refresh = g_timeout_add(100, refresh_now, p);
}

static void no_server(props *p) {
    gtk_label_set_text(GTK_LABEL(p->device_name), "No sound server is running.");
    gtk_widget_set_sensitive(p->volume_box, FALSE);
    gtk_widget_set_sensitive(p->playback, FALSE);
    gtk_widget_set_sensitive(p->recording, FALSE);
}

static void state_changed(pa_context *c, void *data) {
    props *p = data;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY: {
        p->ready = true;
        pa_context_set_subscribe_callback(c, subscribed, p);
        pa_operation *op = pa_context_subscribe(
            c,
            (pa_subscription_mask_t)(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
                                     PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_CARD),
            nullptr, nullptr);
        if (op != nullptr)
            pa_operation_unref(op);
        query(p);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        p->ready = false;
        no_server(p);
        if (p->closing)
            gtk_main_quit();
        break;
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        break;
    }
}

static void connect_server(props *p) {
    p->loop = pa_glib_mainloop_new(nullptr);
    pa_proplist *pl = pa_proplist_new();
    pa_proplist_sets(pl, PA_PROP_APPLICATION_ID, "org.xeondesktop.Sound");
    pa_proplist_sets(pl, PA_PROP_APPLICATION_ICON_NAME, "preferences-desktop-sound");
    p->ctx = pa_context_new_with_proplist(pa_glib_mainloop_get_api(p->loop), TITLE, pl);
    pa_proplist_free(pl);
    if (p->ctx == nullptr) {
        no_server(p);
        return;
    }
    pa_context_set_state_callback(p->ctx, state_changed, p);
    if (pa_context_connect(p->ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
        no_server(p);
}

/* ---- Volume ---------------------------------------------------------------------- */

static void on_volume([[maybe_unused]] GtkRange *r, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    p->volume_edited = true;
    sound_changed(p);
}

static void on_toggled([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    sound_changed(data);
}

static GtkWidget *volume_page(props *p, const xd_settings *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    p->device_name = gtk_label_new("Connecting to the sound server...");
    gtk_label_set_xalign(GTK_LABEL(p->device_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->device_name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(p->device_name, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(page), icon_row("audio-card", p->device_name), FALSE, FALSE, 0);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    p->volume_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *slider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(slider), gtk_label_new("Low"), FALSE, FALSE, 0);
    p->volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(p->volume), FALSE);
    for (int i = 0; i <= 100; i += 10)
        gtk_scale_add_mark(GTK_SCALE(p->volume), i, GTK_POS_BOTTOM, nullptr);
    g_signal_connect(p->volume, "value-changed", G_CALLBACK(on_volume), p);
    gtk_box_pack_start(GTK_BOX(slider), p->volume, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(slider), gtk_label_new("High"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->volume_box), slider, FALSE, FALSE, 0);
    p->mute = gtk_check_button_new_with_mnemonic("_Mute");
    g_signal_connect(p->mute, "toggled", G_CALLBACK(on_volume), p);
    gtk_box_pack_start(GTK_BOX(p->volume_box), p->mute, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(p->volume_box, FALSE);
    gtk_box_pack_start(GTK_BOX(box), p->volume_box, FALSE, FALSE, 0);
    p->tray_icon = gtk_check_button_new_with_mnemonic("Place vo_lume icon in the taskbar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->tray_icon), s->volume_icon);
    g_signal_connect(p->tray_icon, "toggled", G_CALLBACK(on_toggled), p);
    gtk_box_pack_start(GTK_BOX(box), p->tray_icon, FALSE, FALSE, 0);
    GtkWidget *adv = mixer_button(p, "Ad_vanced...", "--tab=3");
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
    GtkWidget *b = mixer_button(p, "_Program Volume...", "--tab=1");
    gtk_widget_set_halign(b, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(apps), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), sound_frame("Speaker settings", icon_row("audio-speakers", apps)),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- Audio ----------------------------------------------------------------------- */

static void on_device([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    p->devices_edited = true;
    sound_changed(p);
}

static GtkWidget *device_group(props *p, const char *title, const char *icon, GtkWidget **combo,
                               const char *tab) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *l = gtk_label_new_with_mnemonic("_Default device:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    *combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(*combo, 280, -1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), *combo);
    g_signal_connect(*combo, "changed", G_CALLBACK(on_device), p);
    gtk_box_pack_start(GTK_BOX(box), *combo, FALSE, FALSE, 0);
    GtkWidget *b = mixer_button(p, "_Volume...", tab);
    gtk_widget_set_halign(b, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    return sound_frame(title, icon_row(icon, box));
}

static GtkWidget *audio_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       device_group(p, "Sound playback", "audio-speakers", &p->playback,
                                    "--tab=3"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       device_group(p, "Sound recording", "audio-input-microphone",
                                    &p->recording, "--tab=4"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       sound_label("Programs that play to the default device move to the new "
                                   "one when you change it."),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- Hardware -------------------------------------------------------------------- */

static GtkWidget *hardware_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *l = gtk_label_new_with_mnemonic("De_vices:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(page), l, FALSE, FALSE, 0);
    p->cards = gtk_list_store_new(CARD_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->cards));
    static const char *const titles[] = {"Name", "Profile", "Driver"};
    for (int i = 0; i < 3; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *col =
            gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_expand(col, i < 2);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    }
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), view);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 180);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       sound_label("A card's profile picks its outputs and inputs (stereo, "
                                   "surround, digital, a headset's microphone)."),
                       FALSE, FALSE, 0);
    GtkWidget *b = mixer_button(p, "P_roperties", "--tab=5");
    gtk_widget_set_halign(b, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(page), b, FALSE, FALSE, 0);
    return page;
}

/* ---- applying -------------------------------------------------------------------- */

static void save_settings(props *p) {
    /* Re-read: another applet may have saved since we opened. */
    xd_settings s;
    xd_settings_load(&s);
    bool icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->tray_icon));
    if (s.volume_icon == icon)
        return;
    s.volume_icon = icon;
    if (!xd_settings_save(&s))
        xd_message_box(GTK_WINDOW(p->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The settings could not be saved.");
}

static bool apply(props *p) {
    if (p->ready && p->volume_edited && p->sink != nullptr) {
        pa_cvolume cv = p->cv;
        double pct = gtk_range_get_value(GTK_RANGE(p->volume));
        if (cv.channels == 0)
            pa_cvolume_set(&cv, 2, PA_VOLUME_NORM);
        /* Keeps the balance between channels. */
        pa_cvolume_scale(&cv, (pa_volume_t)lround(pct * (double)PA_VOLUME_NORM / 100.0));
        track(p, pa_context_set_sink_volume_by_name(p->ctx, p->sink, &cv, op_done, p));
        bool mute = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->mute));
        track(p, pa_context_set_sink_mute_by_name(p->ctx, p->sink, mute, op_done, p));
        p->cv = cv;
        p->volume_edited = false;
    }
    if (p->ready && p->devices_edited) {
        const char *sink = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->playback));
        const char *source = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->recording));
        if (sink != nullptr && g_strcmp0(sink, p->default_sink) != 0)
            track(p, pa_context_set_default_sink(p->ctx, sink, op_done, p));
        if (source != nullptr && g_strcmp0(source, p->default_source) != 0)
            track(p, pa_context_set_default_source(p->ctx, source, op_done, p));
        p->devices_edited = false;
    }
    save_settings(p);
    if (!sounds_apply(p))
        return false;
    gtk_widget_set_sensitive(p->apply, FALSE);
    return true;
}

static gboolean give_up(gpointer data) {
    [[maybe_unused]] props *p = data;
    g_printerr("xd-sound: the sound server didn't answer; closing anyway\n");
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !apply(p))
        return;
    /* Wait (a little) for the sound server to take the changes. */
    gtk_widget_hide(GTK_WIDGET(d));
    if (p->pending == 0) {
        gtk_main_quit();
        return;
    }
    p->closing = true;
    g_timeout_add_seconds(3, give_up, p);
}

static gboolean on_delete(GtkWidget *w, [[maybe_unused]] GdkEvent *e, gpointer data) {
    on_response(GTK_DIALOG(w), GTK_RESPONSE_CANCEL, data);
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
    static props p;
    xd_settings s;
    xd_settings_load(&s);

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), "Sounds and Audio Devices Properties");
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "preferences-desktop-sound");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), volume_page(&p, &s),
                             gtk_label_new_with_mnemonic("V_olume"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), sounds_page(&p),
                             gtk_label_new_with_mnemonic("_Sounds"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), audio_page(&p),
                             gtk_label_new_with_mnemonic("A_udio"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), hardware_page(&p),
                             gtk_label_new_with_mnemonic("_Hardware"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));
    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "delete-event", G_CALLBACK(on_delete), &p);
    gtk_widget_show_all(p.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), XD_CLAMP(page, 0, 3));
    connect_server(&p);
    gtk_main();

    if (p.ctx != nullptr) {
        pa_context_set_state_callback(p.ctx, nullptr, nullptr);
        pa_context_disconnect(p.ctx);
        pa_context_unref(p.ctx);
    }
    if (p.loop != nullptr)
        pa_glib_mainloop_free(p.loop);
    xd_sound_finish();
    xd_sound_scheme_free(&p.sounds);
    g_free(p.sink);
    g_free(p.default_sink);
    g_free(p.default_source);
    return EXIT_SUCCESS;
}
