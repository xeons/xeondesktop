/* SPDX-License-Identifier: MIT
 * The Open and Save As dialogs of xdg-desktop-portal-xeondesktop, one form
 * in every display style: XP's (Look in, the Places bar, File name and
 * Files of type) with Windows 7's search box, around libxdfolder's folder
 * view. Folder pickers are the same dialog listing folders only.
 */
#ifndef XD_PORTAL_CHOOSER_H
#define XD_PORTAL_CHOOSER_H

#include <glib.h>
#include <stdint.h>

typedef enum xd_chooser_mode : uint8_t {
    XD_CHOOSER_OPEN,       /* OpenFile */
    XD_CHOOSER_SAVE,       /* SaveFile */
    XD_CHOOSER_SAVE_FILES, /* SaveFiles: a folder for files already named */
} xd_chooser_mode;

/* The portal's responses. */
enum { XD_CHOOSER_OK = 0, XD_CHOOSER_CANCELLED = 1, XD_CHOOSER_ENDED = 2 };

/* What the program asked for (the method's options, as the portal
 * passes them); strings and variants are copied. */
typedef struct xd_chooser_request {
    xd_chooser_mode mode;
    const char *title;
    const char *parent_window; /* "wayland:HANDLE", "x11:XID" or "" */
    GVariant *options;         /* a{sv} */
} xd_chooser_request;

/* Called once with the response and, for XD_CHOOSER_OK, the results
 * (a floating a{sv}; nullptr otherwise). */
typedef void (*xd_chooser_done)(unsigned response, GVariant *results, gpointer data);

typedef struct xd_chooser xd_chooser;

/* Shows the dialog for `request`; `done` is called when the user is
 * finished with it, and the dialog then frees itself. */
xd_chooser *xd_chooser_show(const xd_chooser_request *request, xd_chooser_done done,
                            gpointer data);
/* Closes it (the portal's Request.Close): `done` gets XD_CHOOSER_ENDED. */
void xd_chooser_close(xd_chooser *chooser);

#endif
