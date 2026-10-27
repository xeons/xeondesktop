/* SPDX-License-Identifier: MIT
 * The Open and Save As dialogs' workings apart from GTK and D-Bus, so the
 * unit tests can check them: the portal's file filters as the "Files of
 * type" list, which names they match, the extension Save As adds, and
 * what the text typed in "File name" stands for.
 */
#ifndef XD_PORTAL_CHOOSERLIB_H
#define XD_PORTAL_CHOOSERLIB_H

#include <glib.h>
#include <stdint.h>

/* ---- file filters ------------------------------------------------------------------ */

/* One entry of "Files of type": the portal's (sa(us)), a name and its
 * rules, each a glob ("*.txt", "*.[Jj][Pp][Gg]") or a MIME type
 * ("image/png", or "image/" and a star for every image). */
typedef struct xd_chooser_filter {
    char *name;
    GPtrArray *globs; /* char* */
    GPtrArray *mimes; /* char* */
    GVariant *source; /* the (sa(us)) it came from, handed back as current_filter */
} xd_chooser_filter;

/* One filter from its (sa(us)), or nullptr if the variant isn't one. */
xd_chooser_filter *xd_chooser_filter_parse(GVariant *variant);
/* The filters of an a(sa(us)) (xd_chooser_filter*), in order; those not
 * understood are left out. Free with g_ptr_array_unref. */
GPtrArray *xd_chooser_filters_parse(GVariant *filters);
void xd_chooser_filter_free(xd_chooser_filter *filter);

/* Which of `filters` the portal's current_filter is: the one equal to it,
 * else one of the same name; -1 if none (the caller adds it). */
int xd_chooser_filters_find(const GPtrArray *filters, GVariant *current);

/* Whether a file named `name` (of `content_type`, or nullptr to guess it
 * from the name) is shown under `filter`. Globs match regardless of case,
 * as Windows' do; a filter with no rules matches everything. */
bool xd_chooser_filter_matches(const xd_chooser_filter *filter, const char *name,
                               const char *content_type);

/* The extension Save As adds under `filter` (".txt"): its first glob's
 * "*.ext", with [Tt] brackets taken in lower case; nullptr when it has
 * none ("*", "*.*", a MIME type only). Free with g_free. */
char *xd_chooser_filter_get_extension(const xd_chooser_filter *filter);

/* What Save As writes when `name` is typed under `filter`: the name with
 * the filter's extension added when it has none, as XP did. A name in
 * quotes is taken as it is, and a trailing dot means "no extension".
 * Free with g_free. */
char *xd_chooser_add_extension(const char *name, const xd_chooser_filter *filter);

/* Whether `text` (typed in "File name", such as "*.txt" or "a?c") is a
 * wildcard pattern rather than a name. */
bool xd_chooser_is_pattern(const char *text);

/* ---- the File name box ------------------------------------------------------------- */

typedef enum xd_typed_kind : uint8_t {
    XD_TYPED_EMPTY,   /* nothing typed */
    XD_TYPED_FILE,    /* one or more files: `paths` */
    XD_TYPED_FOLDER,  /* an existing folder to go to: `paths[0]` */
    XD_TYPED_PATTERN, /* show only matching names: `pattern`, in `paths[0]` (the folder) */
} xd_typed_kind;

typedef struct xd_typed_name {
    xd_typed_kind kind;
    char **paths; /* absolute, canonical; nullptr-terminated (nullptr for EMPTY) */
    char *pattern;
} xd_typed_name;

/* What `text` typed in "File name" means in `folder` (an absolute path):
 * a name or path (relative to the folder, or "~/..."), several names in
 * quotes ("a.txt" "b.txt") when `multiple`, an existing folder, or a
 * wildcard pattern, which may follow a folder ("Letters" then "/" then
 * "*.txt").
 * Free with xd_typed_name_free. */
xd_typed_name xd_chooser_parse_name(const char *folder, const char *text, bool multiple);
void xd_typed_name_free(xd_typed_name *typed);

#endif
