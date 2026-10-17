/* SPDX-License-Identifier: MIT
 * xd-help: Help and Support, from the Start menus' Help items.
 *
 * Windows' help described Windows; ours is a few pages about this desktop
 * (starting programs, the keyboard, settings) in the shape of Windows
 * 2000's help window: the topics on the left, the page on the right. The
 * programs themselves are Linux programs with their own help, so the last
 * page says where that is, and opens the system's help viewer (whatever
 * handles help: URIs, e.g. Yelp) when one is installed.
 */
#include <stdlib.h>

#include "../gtkapp/gtkapp.h"

typedef struct topic {
    const char *title;
    const char *markup; /* Pango markup */
} topic;

static const topic topics[] = {
    {"Welcome",
     "<big><b>Welcome to XeonDesktop</b></big>\n\n"
     "XeonDesktop is a desktop for Linux that looks and works like Windows 2000, "
     "Windows XP and Windows 7. The programs you run on it are Linux programs.\n\n"
     "Choose a topic on the left. For help with a program, use that program's own "
     "Help menu; <i>Help for programs</i> says where else to look."},
    {"Starting programs",
     "<big><b>Starting programs</b></big>\n\n"
     "Click <b>Start</b> (or press the Windows key, or Ctrl+Esc) and point to "
     "<b>Programs</b> or <b>All Programs</b>. Installed programs are filed into folders "
     "such as Accessories, Games and Internet.\n\n"
     "In the Windows 7 Start menu you can just start typing: the search box finds "
     "programs and Control Panel items by name.\n\n"
     "<b>Run...</b> starts a program, or opens a folder or file, by name.\n\n"
     "The small icons next to the Start button are the Quick Launch toolbar: one click "
     "starts a program."},
    {"Keyboard shortcuts",
     "<big><b>Keyboard shortcuts</b></big>\n\n"
     "<tt>Windows key, Ctrl+Esc   </tt>Open the Start menu\n"
     "<tt>Alt+Tab                 </tt>Switch between windows\n"
     "<tt>Alt+F4                  </tt>Close the window\n"
     "<tt>Windows key+Up          </tt>Maximize the window\n"
     "<tt>Windows key+Down        </tt>Restore or minimize the window\n"
     "<tt>Windows key+L           </tt>Lock the computer\n"
     "<tt>Ctrl+Alt+Delete         </tt>Windows Security or Task Manager\n"
     "<tt>Ctrl+Shift+Esc          </tt>Task Manager\n"
     "<tt>Ctrl+Alt+T              </tt>Open a terminal\n\n"
     "In the Start menu, the arrow keys move between items, Tab moves between its "
     "columns, Enter opens the selected item and Esc closes the menu."},
    {"Changing settings",
     "<big><b>Changing settings</b></big>\n\n"
     "Open <b>Control Panel</b> from the Start menu (in Windows 2000 style, under "
     "Settings). Its items change the display, sound, mouse, keyboard, date and time, "
     "user accounts, printers and more.\n\n"
     "Right-click the desktop and choose <b>Properties</b> to change the wallpaper, "
     "screen saver, colours and screen resolution. Right-click the taskbar and choose "
     "<b>Properties</b> to change the taskbar and pick a Start menu style."},
    {"Help for programs",
     "<big><b>Help for programs</b></big>\n\n"
     "Most programs have a <b>Help</b> menu or a Help button with their own "
     "documentation.\n\n"
     "Command-line programs have manual pages: in a terminal, type <tt>man</tt> and the "
     "program's name, for example <tt>man ls</tt>.\n\n"
     "Many programs also install documentation for the system's help viewer."},
};

typedef struct help {
    GtkWidget *window, *page, *viewer;
    GAppInfo *viewer_app; /* handles help: URIs, or nullptr */
} help;

static void on_open_viewer([[maybe_unused]] GtkButton *b, gpointer data) {
    help *h = data;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(h->window));
    GError *error = nullptr;
    if (!g_app_info_launch(h->viewer_app, nullptr, G_APP_LAUNCH_CONTEXT(ctx), &error)) {
        xd_message_box(GTK_WINDOW(h->window), "Help and Support", XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
    }
    g_object_unref(ctx);
}

static void on_topic([[maybe_unused]] GtkListBox *list, GtkListBoxRow *row, gpointer data) {
    help *h = data;
    if (row == nullptr)
        return;
    int i = gtk_list_box_row_get_index(row);
    if (i < 0 || (size_t)i >= G_N_ELEMENTS(topics))
        return;
    gtk_label_set_markup(GTK_LABEL(h->page), topics[i].markup);
    /* The viewer's button belongs to the last page. */
    gtk_widget_set_visible(h->viewer, h->viewer_app != nullptr &&
                                          (size_t)i == G_N_ELEMENTS(topics) - 1);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Help");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static help h;
    h.viewer_app = g_app_info_get_default_for_uri_scheme("help");

    h.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(h.window), "Help and Support");
    gtk_window_set_icon_name(GTK_WINDOW(h.window), "help-browser");
    gtk_window_set_default_size(GTK_WINDOW(h.window), 640, 440);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(left), 6);
    GtkWidget *contents = gtk_label_new_with_mnemonic("_Contents");
    gtk_label_set_xalign(GTK_LABEL(contents), 0);
    gtk_box_pack_start(GTK_BOX(left), contents, FALSE, FALSE, 0);
    GtkWidget *list = gtk_list_box_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(contents), list);
    for (size_t i = 0; i < G_N_ELEMENTS(topics); i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(row), 2);
        gtk_box_pack_start(GTK_BOX(row),
                           gtk_image_new_from_icon_name("help-contents", GTK_ICON_SIZE_MENU),
                           FALSE, FALSE, 0);
        GtkWidget *label = gtk_label_new(topics[i].title);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
        gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
    }
    GtkWidget *list_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(list_scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(list_scroll), list);
    gtk_widget_set_size_request(list_scroll, 180, -1);
    gtk_box_pack_start(GTK_BOX(left), list_scroll, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(right), 12);
    h.page = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(h.page), 0);
    gtk_label_set_yalign(GTK_LABEL(h.page), 0);
    gtk_label_set_line_wrap(GTK_LABEL(h.page), TRUE);
    gtk_label_set_selectable(GTK_LABEL(h.page), TRUE);
    gtk_box_pack_start(GTK_BOX(right), h.page, FALSE, FALSE, 0);
    char *viewer_label = h.viewer_app != nullptr
                             ? g_strdup_printf("Open %s", g_app_info_get_display_name(h.viewer_app))
                             : g_strdup("");
    h.viewer = gtk_button_new_with_label(viewer_label);
    g_free(viewer_label);
    gtk_widget_set_halign(h.viewer, GTK_ALIGN_START);
    g_signal_connect(h.viewer, "clicked", G_CALLBACK(on_open_viewer), &h);
    gtk_box_pack_start(GTK_BOX(right), h.viewer, FALSE, FALSE, 0);
    GtkWidget *page_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(page_scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page_scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(page_scroll), right);
    gtk_paned_pack2(GTK_PANED(paned), page_scroll, TRUE, FALSE);
    gtk_container_add(GTK_CONTAINER(h.window), paned);

    g_signal_connect(list, "row-selected", G_CALLBACK(on_topic), &h);
    g_signal_connect(h.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(h.window);
    gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
    gtk_main();
    g_clear_object(&h.viewer_app);
    return EXIT_SUCCESS;
}
