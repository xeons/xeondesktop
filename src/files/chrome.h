/* SPDX-License-Identifier: MIT
 * Vista's and 7's Explorer over xd-files' own (XP's) window: where the theme
 * package styles them, the toolbar is drawn as 7's command bar
 * (CommandModule), the address box as its address band, the Folders tree's
 * rows as its navigation pane's (Explorer::TreeView) and the status bar as
 * its details pane (PreviewPane). Without such a package GTK draws them as
 * before.
 */
#ifndef XD_FILES_CHROME_H
#define XD_FILES_CHROME_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

void xd_explorer_chrome_attach(GtkWidget *toolbar, GtkWidget *address, GtkWidget *tree,
                               GtkWidget *status_bar);

G_END_DECLS

#endif
