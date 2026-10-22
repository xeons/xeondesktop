/* SPDX-License-Identifier: MIT
 * xd-sound: Windows XP's Sounds and Audio Devices Properties.
 */
#ifndef XD_SOUND_SOUND_H
#define XD_SOUND_SOUND_H

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "../settings/sounds.h"

typedef struct props {
    GtkWidget *window, *apply;
    bool updating; /* we're setting widgets ourselves */
    bool closing;  /* OK was pressed: quit once the sound server has the changes */
    int pending;   /* sound server operations not finished yet */

    /* The sound server (libpulse: PulseAudio or PipeWire's pulse server). */
    pa_glib_mainloop *loop;
    pa_context *ctx;
    bool ready;
    guint refresh; /* coalesces change events */
    char *default_sink, *default_source;

    /* Volume */
    GtkWidget *device_name, *volume, *mute, *tray_icon, *volume_box;
    char *sink;     /* whose volume the slider shows */
    pa_cvolume cv;  /* its channels, for keeping the balance */
    bool volume_edited;

    /* Audio */
    GtkWidget *playback, *recording;
    bool devices_edited;

    /* Sounds (scheme.c) */
    GtkWidget *scheme, *events, *sound, *play, *browse, *sound_label;
    GtkTreeStore *event_store;
    xd_sound_scheme sounds;
    bool sounds_edited;

    /* Hardware */
    GtkListStore *cards;
    GtkWidget *card_props;
} props;

/* Something changed: Apply lights up. */
void sound_changed(props *properties);
GtkWidget *sound_frame(const char *title, GtkWidget *child);
GtkWidget *sound_label(const char *text);

/* scheme.c: the Sounds tab. */
GtkWidget *sounds_page(props *properties);
bool sounds_apply(props *properties);

#endif
