/* SPDX-License-Identifier: MIT
 * The installed fonts, from fontconfig, for the Fonts folder (xd-fonts).
 */
#ifndef XD_FONTS_FONTLIST_H
#define XD_FONTS_FONTLIST_H

#include <glib.h>

typedef struct xd_font {
    char *file;   /* the font file */
    int index;    /* the face in a collection (or a variable font's instance) */
    char *family; /* its first family name */
    char *style;
    char *name;   /* as the folder shows it: "DejaVu Sans Bold (TrueType)" */
    int weight, slant, width; /* fontconfig's FC_WEIGHT, FC_SLANT, FC_WIDTH */
    bool user;    /* in the user's own font folder: it can be deleted */
    bool primary; /* its family's regular face (shown with Hide Variations) */
} xd_font;

/* XP's kind of font, from fontconfig's FC_FONTFORMAT and the file name:
 * "TrueType", "OpenType" (CFF outlines, or a .otf), "Type 1" or
 * "Bitmap"; nullptr when unknown. */
const char *xd_font_format_label(const char *fontformat, const char *file);

/* The folder's name: the full name (else family and style, without
 * "Regular"), then the kind in brackets. Free with g_free. */
char *xd_font_display_name(const char *fullname, const char *family, const char *style,
                           const char *format_label);

/* Whether `path` is inside `dir` (not `dir` itself), by path components. */
bool xd_path_in_dir(const char *path, const char *dir);

/* Marks each family's face closest to upright, regular weight and
 * normal width as its primary one (the first, on a tie). */
void xd_fonts_mark_primary(xd_font *const *fonts, size_t count);

/* "Version 2.37" from FC_FONTVERSION (16.16 fixed point). Free with g_free. */
char *xd_font_version_text(int fontversion);

/* The user's font folder ($XDG_DATA_HOME/fonts), where fonts are
 * installed, and whether `file` is in one of the user's folders (that
 * one or the legacy ~/.fonts). */
char *xd_fonts_user_dir(void);
bool xd_font_is_user(const char *file);

/* Every installed face, sorted by name, primary faces marked. Free with
 * g_ptr_array_unref. */
GPtrArray *xd_fonts_list(void);

/* After installing or deleting fonts: rewrites the user folder's cache
 * (so other programs see the change) and reloads fontconfig here. */
void xd_fonts_rescan(void);

#endif
