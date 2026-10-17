/* SPDX-License-Identifier: MIT
 * A Windows wizard (Add Printer, Scanner and Camera Wizard): Welcome and
 * Completing pages with the blue band down the left ("exterior" pages),
 * the others with a white header holding the step's title and subtitle
 * ("interior" pages), and < Back, Next > (or Finish) and Cancel at the
 * bottom.
 */
#ifndef XD_CPL_WIZARD_H
#define XD_CPL_WIZARD_H

#include <gtk/gtk.h>

typedef struct xd_wizard xd_wizard;

/* Where Next goes from `page`: another page's index, or -1 to stay (the
 * page showed why). Without one, Next goes to the following page. */
typedef int (*xd_wizard_next_fn)(xd_wizard *w, int page, gpointer data);
/* Called each time `page` is shown, going forward or back. */
typedef void (*xd_wizard_enter_fn)(xd_wizard *w, int page, gpointer data);
/* Finish was pressed on the last page: true closes the wizard. */
typedef bool (*xd_wizard_finish_fn)(xd_wizard *w, gpointer data);

/* A new, hidden wizard. It frees itself when its window is destroyed
 * (Cancel, Finish, the close button); `destroy` then frees `data`. */
xd_wizard *xd_wizard_new(GtkWindow *parent, const char *title, const char *icon_name,
                         gpointer data, GDestroyNotify destroy);
GtkWidget *xd_wizard_window(xd_wizard *w);

/* Adds a page and returns its index (from 0). A page added with
 * `final` shows Finish instead of Next. */
int xd_wizard_add_exterior(xd_wizard *w, const char *heading, GtkWidget *body, bool final);
int xd_wizard_add_interior(xd_wizard *w, const char *heading, const char *subheading,
                           GtkWidget *body, bool final);

void xd_wizard_set_next_func(xd_wizard *w, xd_wizard_next_fn fn);
void xd_wizard_set_enter_func(xd_wizard *w, xd_wizard_enter_fn fn);
void xd_wizard_set_finish_func(xd_wizard *w, xd_wizard_finish_fn fn);

/* While a page waits for something (a search, a scan): Next and Back
 * greyed out. */
void xd_wizard_set_next_sensitive(xd_wizard *w, bool sensitive);
void xd_wizard_set_back_sensitive(xd_wizard *w, bool sensitive);

/* Jumps to `page` as Next would (it can be gone back from). */
void xd_wizard_go(xd_wizard *w, int page);
int xd_wizard_page(xd_wizard *w);

/* Shows the first page. */
void xd_wizard_show(xd_wizard *w);

#endif
