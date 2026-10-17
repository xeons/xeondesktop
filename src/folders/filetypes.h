/* SPDX-License-Identifier: MIT
 * File types for Folder Options' File Types tab: the extensions each
 * MIME type registers in shared-mime-info, and the user's own changes to
 * them (New and Delete).
 *
 * Changes are package files of ours in the user's database,
 * $XDG_DATA_HOME/mime/packages/xeondesktop-MEDIA-SUBTYPE.xml, one per
 * type, which update-mime-database compiles into the user's database. A
 * new type is application/x-extension-EXT, "EXT File" as XP named them;
 * an extension taken from a type the system defines is a <glob-deleteall/>
 * followed by the type's other globs, which hides the system's ones.
 */
#ifndef XD_FOLDERS_FILETYPES_H
#define XD_FOLDERS_FILETYPES_H

#include <glib.h>

/* Adds the extensions (without the dot) that one MIME type's XML (the
 * per-type <type>.xml update-mime-database writes) registers with simple
 * "*.ext" globs to `out` (char *, g_free), skipping ones already there.
 * Case-insensitive globs are lowercased. Malformed XML adds what was
 * read before the error. Returns whether the type drops the globs of
 * the databases below it (<glob-deleteall/>). */
bool xd_mime_xml_extensions(const char *xml, gssize len, GPtrArray *out);

/* A type's extensions from the user's and the system's MIME databases
 * ($XDG_DATA_HOME/mime, $XDG_DATA_DIRS/mime). Free with g_strfreev. */
char **xd_content_type_extensions(const char *type);

typedef struct xd_mime_glob {
    char *pattern;
    int weight; /* 0 = the default, 50 */
    bool case_sensitive;
} xd_mime_glob;

/* One type as an XML file defines it. */
typedef struct xd_mime_package {
    char *type;
    char *comment;   /* the untranslated one; nullptr if it has none */
    bool delete_all; /* <glob-deleteall/> */
    GPtrArray *globs; /* xd_mime_glob * */
} xd_mime_package;

/* Reads `type` from a package file (<mime-info>) or a per-type file
 * (<mime-type>). False if the XML is malformed or doesn't define it;
 * `out` is filled either way, and cleared with xd_mime_package_clear. */
bool xd_mime_package_parse(const char *xml, gssize len, const char *type, xd_mime_package *out);
void xd_mime_package_clear(xd_mime_package *pkg);
/* A package file holding just `pkg`. g_free it. */
char *xd_mime_package_to_xml(const xd_mime_package *pkg);

/* $XDG_DATA_HOME then $XDG_DATA_DIRS: the databases in order of
 * precedence. g_strfreev. */
char **xd_mime_data_dirs(void);
/* A type's globs as the databases in `data_dirs` add up (the first is
 * the user's, which changes are written to). Array of xd_mime_glob *. */
GPtrArray *xd_mime_type_globs(const char *const *data_dirs, const char *type);

/* Whether `ext` (without the dot) can be an extension: a "*.ext" glob
 * that matches only that, and a type name's last part. */
bool xd_mime_extension_valid(const char *ext);
/* The type New gives an extension of a type of its own:
 * "application/x-extension-xyz". g_free it. */
char *xd_mime_type_for_new_extension(const char *ext);

/* Registers "*.ext" for `type` in the user's database (data_dirs[0]);
 * `comment` names a new type ("XYZ File"), nullptr for an existing one. */
bool xd_mime_add_extension(const char *const *data_dirs, const char *type, const char *ext,
                           const char *comment, GError **error);
/* Takes "*.ext" from `type`. A type of the user's own that is left
 * without extensions goes too (`*type_removed`). */
bool xd_mime_remove_extension(const char *const *data_dirs, const char *type, const char *ext,
                              bool *type_removed, GError **error);
/* Runs update-mime-database on `data_dir`/mime. */
bool xd_mime_update_database(const char *data_dir, GError **error);

#endif
