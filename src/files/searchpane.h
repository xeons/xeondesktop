/* SPDX-License-Identifier: MIT
 * Explorer's Search bar, on the left of xd-files' window in place of the
 * task pane or Folders: Windows XP's Search Companion, or Windows 2000's
 * "Search for Files and Folders" where the style is 2000's. It gathers the
 * criteria (search.h) and hands them to the window, which searches and
 * shows the results in its folder view.
 */
#ifndef XD_FILES_SEARCHPANE_H
#define XD_FILES_SEARCHPANE_H

#include <gtk/gtk.h>

#include "folderview.h"
#include "search.h"

G_BEGIN_DECLS

typedef enum xd_search_pane_style : uint8_t {
    XD_SEARCH_PANE_2000,
    XD_SEARCH_PANE_XP,
} xd_search_pane_style;

/* What the pane asks of its window. */
typedef struct xd_search_pane_callbacks {
    void (*search)(const xd_search_criteria *criteria, gpointer data);
    void (*stop)(gpointer data);
    /* "Yes, finished searching" or the bar's close button. */
    void (*close)(gpointer data);
    /* "Start a new search": the results go, the folder is back. */
    void (*new_search)(gpointer data);
    /* "Sort results by category". */
    void (*sort)(XdSortColumn column, gpointer data);
    gpointer data;
} xd_search_pane_callbacks;

GtkWidget *xd_search_pane_new(xd_search_pane_style style,
                              const xd_search_pane_callbacks *callbacks);
/* The folder Look in starts with (and adds to its list): the window's. */
void xd_search_pane_set_folder(GtkWidget *pane, const char *path, const char *label);
/* Back to its first page ("What do you want to search for?" in XP). */
void xd_search_pane_reset(GtkWidget *pane);
/* A search running (XP: "Searching..." and Stop; 2000: Stop Search) and
 * the folder it's in, or over, with how much it found (XP: "Did you find
 * what you wanted?"). */
void xd_search_pane_set_running(GtkWidget *pane, bool running);
void xd_search_pane_set_progress(GtkWidget *pane, const char *folder);
void xd_search_pane_show_done(GtkWidget *pane, guint found);
/* Fills the fields from a saved search (XP's .fnd opened). */
void xd_search_pane_set_criteria(GtkWidget *pane, const xd_search_criteria *criteria);
/* Puts the focus in the first field (Ctrl+F, F3). */
void xd_search_pane_focus(GtkWidget *pane);

G_END_DECLS

#endif
