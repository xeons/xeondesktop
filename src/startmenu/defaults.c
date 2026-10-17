/* SPDX-License-Identifier: MIT
 * xd-defaults: Set Program Access and Defaults (Windows XP SP2; Windows 7's
 * Start menu calls it Default Programs). One section per activity, as in
 * XP's Custom configuration: "Use my current ..." keeps what's set, or a
 * program becomes the default. OK writes it through GIO
 * (g_app_info_set_as_default_for_type: ~/.config/mimeapps.list), which
 * other desktops' programs read too.
 *
 * Not here: XP's other configurations (Computer Manufacturer, Microsoft
 * Windows, Non-Microsoft) and its "Enable access to this program" boxes,
 * which hid programs from the Start menu.
 */
#include <stdlib.h>
#include <string.h>

#include "../gtkapp/gtkapp.h"

typedef struct activity {
    const char *heading; /* XP's wording */
    const char *current; /* "Use my current ..." */
    /* The first type picks the programs offered and the current one; the
     * others follow when the chosen program handles them. */
    const char *const *types;
} activity;

static const char *const web_types[] = {"x-scheme-handler/http", "x-scheme-handler/https",
                                        "text/html", "application/xhtml+xml", nullptr};
static const char *const mail_types[] = {"x-scheme-handler/mailto", nullptr};
static const char *const music_types[] = {"audio/mpeg", "audio/flac", "audio/x-vorbis+ogg",
                                          "audio/ogg", "audio/x-wav", "audio/mp4",
                                          "audio/x-opus+ogg", nullptr};
static const char *const video_types[] = {"video/mp4", "video/x-matroska", "video/webm",
                                          "video/mpeg", "video/x-msvideo", "video/quicktime",
                                          nullptr};
static const char *const image_types[] = {"image/png", "image/jpeg", "image/gif", "image/bmp",
                                          "image/webp", "image/tiff", nullptr};
static const char *const text_types[] = {"text/plain", nullptr};

static const activity activities[] = {
    {"Choose a default Web browser:", "Use my current Web browser", web_types},
    {"Choose a default e-mail program:", "Use my current e-mail program", mail_types},
    {"Choose a default music player:", "Use my current music player", music_types},
    {"Choose a default video player:", "Use my current video player", video_types},
    {"Choose a default picture viewer:", "Use my current picture viewer", image_types},
    {"Choose a default text editor:", "Use my current text editor", text_types},
};

typedef struct choice {
    GtkWidget *keep;   /* "Use my current ..." radio */
    GList *apps;       /* GAppInfo*, one per radio after `keep` */
    GList *radios;     /* GtkWidget* */
} choice;

typedef struct dialog {
    GtkWidget *window;
    choice choices[G_N_ELEMENTS(activities)];
} dialog;

static bool handles(GAppInfo *app, const char *type) {
    const char **types = g_app_info_get_supported_types(app);
    for (size_t i = 0; types != nullptr && types[i] != nullptr; i++) {
        if (g_content_type_equals(types[i], type))
            return true;
    }
    return false;
}

static GtkWidget *section(const activity *a, choice *c) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *heading = gtk_label_new(nullptr);
    char *markup = g_markup_printf_escaped("<b>%s</b>", a->heading);
    gtk_label_set_markup(GTK_LABEL(heading), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 2);

    GAppInfo *now = g_app_info_get_default_for_type(a->types[0], FALSE);
    char *keep_label = now != nullptr
                           ? g_strdup_printf("%s (%s)", a->current, g_app_info_get_display_name(now))
                           : g_strdup(a->current);
    c->keep = gtk_radio_button_new_with_label(nullptr, keep_label);
    g_free(keep_label);
    gtk_widget_set_margin_start(c->keep, 16);
    gtk_box_pack_start(GTK_BOX(box), c->keep, FALSE, FALSE, 0);

    GList *apps = g_app_info_get_all_for_type(a->types[0]);
    for (GList *l = apps; l != nullptr; l = l->next) {
        GAppInfo *app = l->data;
        if (!g_app_info_should_show(app))
            continue;
        GtkWidget *radio = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(c->keep));
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GIcon *icon = g_app_info_get_icon(app);
        GtkWidget *image = icon != nullptr ? gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_MENU)
                                           : gtk_image_new_from_icon_name(
                                                 "application-x-executable", GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(image), 16); /* file icons come at full size */
        gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(g_app_info_get_display_name(app)), FALSE,
                           FALSE, 0);
        gtk_container_add(GTK_CONTAINER(radio), row);
        gtk_widget_set_margin_start(radio, 16);
        gtk_box_pack_start(GTK_BOX(box), radio, FALSE, FALSE, 0);
        c->apps = g_list_append(c->apps, g_object_ref(app));
        c->radios = g_list_append(c->radios, radio);
    }
    g_list_free_full(apps, g_object_unref);
    if (c->apps == nullptr) {
        GtkWidget *none = gtk_label_new("No programs for this are installed.");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_margin_start(none, 40);
        gtk_widget_set_sensitive(none, FALSE);
        gtk_box_pack_start(GTK_BOX(box), none, FALSE, FALSE, 0);
    }
    g_clear_object(&now);
    return box;
}

static bool apply(dialog *d) {
    for (size_t i = 0; i < G_N_ELEMENTS(activities); i++) {
        const activity *a = &activities[i];
        choice *c = &d->choices[i];
        GList *app = c->apps;
        for (GList *r = c->radios; r != nullptr; r = r->next, app = app->next) {
            if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r->data)))
                continue;
            for (size_t t = 0; a->types[t] != nullptr; t++) {
                if (t > 0 && !handles(app->data, a->types[t]))
                    continue;
                GError *error = nullptr;
                if (!g_app_info_set_as_default_for_type(app->data, a->types[t], &error)) {
                    char *text = g_strdup_printf("%s could not be made the default: %s",
                                                 g_app_info_get_display_name(app->data),
                                                 error->message);
                    xd_message_box(GTK_WINDOW(d->window), "Set Program Access and Defaults",
                                   XD_MB_ERROR, XD_MB_OK, FALSE, text);
                    g_free(text);
                    g_error_free(error);
                    return false;
                }
            }
        }
    }
    return true;
}

static void on_response(GtkDialog *w, int response, gpointer data) {
    dialog *d = data;
    if (response == GTK_RESPONSE_OK && !apply(d))
        return;
    gtk_widget_destroy(GTK_WIDGET(w));
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Defaults");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static dialog d;

    d.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d.window), "Set Program Access and Defaults");
    gtk_window_set_icon_name(GTK_WINDOW(d.window), "preferences-desktop-default-applications");
    gtk_window_set_default_size(GTK_WINDOW(d.window), 480, 520);
    gtk_dialog_add_button(GTK_DIALOG(d.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d.window), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d.window), GTK_RESPONSE_OK);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(d.window));
    GtkWidget *intro = gtk_label_new(
        "Choose the programs used by default for certain activities, such as Web browsing or "
        "sending e-mail.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 60);
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    gtk_box_pack_start(GTK_BOX(content), intro, FALSE, FALSE, 4);

    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(list), 8);
    for (size_t i = 0; i < G_N_ELEMENTS(activities); i++)
        gtk_box_pack_start(GTK_BOX(list), section(&activities[i], &d.choices[i]), FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 4);

    g_signal_connect(d.window, "response", G_CALLBACK(on_response), &d);
    g_signal_connect(d.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(d.window);
    gtk_main();
    for (size_t i = 0; i < G_N_ELEMENTS(activities); i++) {
        g_list_free_full(d.choices[i].apps, g_object_unref);
        g_list_free(d.choices[i].radios);
    }
    return EXIT_SUCCESS;
}
