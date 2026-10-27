/* SPDX-License-Identifier: MIT
 * xd-files' search: what to look for and how it's matched, shared by XP's
 * Search Companion (and 2000's Search pane) and 7's search box.
 *
 * Both forms fill in an xd_search_criteria: XP's fields one by one, 7's
 * box as Advanced Query Syntax text (`query`). xd_search_query_new turns
 * that into one query, a tree of AND, OR and NOT over terms (a property,
 * an operator and a value), which the walk (searchwalk.c) evaluates for
 * each file. The matching here does no I/O but read a file's contents
 * through the callback it's given, so it's unit-tested on its own.
 *
 * Names, words and phrases are all matched with GRegex (PCRE2): a word is
 * an escaped pattern, a wildcard a translated one, a regular expression
 * the user's own. Contents are read up to XD_SEARCH_CONTENT_MAX bytes:
 * UTF-8 text (and UTF-16 with a byte order mark, converted) is searched as
 * text; anything else, as XP did, as bytes, ASCII letters in either case,
 * the phrase in single-byte and in UTF-16LE form.
 *
 * Saved searches are a key file (.xdsearch, application/x-xeondesktop-
 * search) holding the criteria, as XP's .fnd did.
 */
#ifndef XD_FILES_SEARCH_H
#define XD_FILES_SEARCH_H

#include <gio/gio.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Files bigger than this aren't read for "A word or phrase in the file"
 * or content: (32 MiB). */
enum { XD_SEARCH_CONTENT_MAX = 32 * 1024 * 1024 };

#define XD_SEARCH_FILE_EXTENSION ".xdsearch"
#define XD_SEARCH_MIME_TYPE "application/x-xeondesktop-search"

/* What a term looks at. */
typedef enum xd_search_property : uint8_t {
    XD_SEARCH_PROP_ANY, /* a bare word: the name (and contents, if asked) */
    XD_SEARCH_PROP_NAME,
    XD_SEARCH_PROP_CONTENT,
    XD_SEARCH_PROP_KIND,
    XD_SEARCH_PROP_TYPE, /* extension, content type or the type's description */
    XD_SEARCH_PROP_MODIFIED,
    XD_SEARCH_PROP_CREATED,
    XD_SEARCH_PROP_ACCESSED,
    XD_SEARCH_PROP_SIZE,
    XD_SEARCH_PROP_COUNT,
} xd_search_property;

/* Windows 7's kinds (kind:), and XP's Pictures, music, or video and
 * Documents pages. A file has one; a term may take several. */
typedef enum xd_search_kind : uint32_t {
    XD_SEARCH_KIND_NONE = 0,
    XD_SEARCH_KIND_FOLDER = 1U << 0,
    XD_SEARCH_KIND_DOCUMENT = 1U << 1,
    XD_SEARCH_KIND_PICTURE = 1U << 2,
    XD_SEARCH_KIND_MUSIC = 1U << 3,
    XD_SEARCH_KIND_VIDEO = 1U << 4,
    XD_SEARCH_KIND_PROGRAM = 1U << 5,
    XD_SEARCH_KIND_LINK = 1U << 6,
    XD_SEARCH_KIND_EMAIL = 1U << 7,
    XD_SEARCH_KIND_CONTACT = 1U << 8,
    XD_SEARCH_KIND_CALENDAR = 1U << 9,
    XD_SEARCH_KIND_COMPRESSED = 1U << 10,
    XD_SEARCH_KIND_OTHER = 1U << 11,
} xd_search_kind;

/* XP's "When was it modified?" */
typedef enum xd_search_when : uint8_t {
    XD_SEARCH_WHEN_ANY, /* Don't remember */
    XD_SEARCH_WHEN_WEEK, /* Within the last week */
    XD_SEARCH_WHEN_MONTH, /* Past month */
    XD_SEARCH_WHEN_YEAR, /* Within the past year */
    XD_SEARCH_WHEN_DATES, /* Specify dates: from and to, both days included */
    XD_SEARCH_WHEN_COUNT,
} xd_search_when;

/* XP's "What size is it?" */
typedef enum xd_search_size : uint8_t {
    XD_SEARCH_SIZE_ANY, /* Don't remember */
    XD_SEARCH_SIZE_SMALL, /* less than 100 KB */
    XD_SEARCH_SIZE_MEDIUM, /* less than 1 MB */
    XD_SEARCH_SIZE_LARGE, /* more than 1 MB */
    XD_SEARCH_SIZE_AT_LEAST, /* Specify size (in KB): at least */
    XD_SEARCH_SIZE_AT_MOST, /* ...at most */
    XD_SEARCH_SIZE_COUNT,
} xd_search_size;

/* A search as the user gave it (and as a saved search holds it). Strings
 * are owned; empty or nullptr means not given. */
typedef struct xd_search_criteria {
    char *name; /* All or part of the file name (XP), named (2000) */
    char *contents; /* A word or phrase in the file, Containing text */
    char **look_in; /* paths searched, each with its subfolders if asked */
    uint32_t kinds; /* xd_search_kind bits; 0: any */
    char *content_type; /* Type of file; nullptr: all files and folders */
    xd_search_property date_property; /* MODIFIED, CREATED or ACCESSED */
    xd_search_when when;
    char *date_from, *date_to; /* "YYYY-MM-DD", for XD_SEARCH_WHEN_DATES */
    xd_search_size size;
    uint64_t size_kb; /* for AT_LEAST and AT_MOST */
    char *query; /* 7's search box: Advanced Query Syntax */
    bool subfolders; /* Search subfolders */
    bool case_sensitive;
    bool hidden; /* Search hidden files and folders */
    bool system; /* Search system folders (7: Include system directories) */
    bool regex; /* Use regular expressions: name and phrase are patterns */
    bool archives; /* 7's Include compressed files (ZIP, CAB...) */
    /* 7's Folder Options: bare words anywhere in a name, not only whole
     * words; bare words in the contents too; /pattern/ terms. */
    bool partial;
    bool names_and_contents;
    bool allow_regex;
} xd_search_criteria;

/* XP's defaults: subfolders and system folders searched, hidden files
 * not, ignoring case; 7's partial matches and /pattern/ on. */
void xd_search_criteria_init(xd_search_criteria *criteria);
void xd_search_criteria_clear(xd_search_criteria *criteria);
void xd_search_criteria_copy(xd_search_criteria *to, const xd_search_criteria *from);

/* Saved searches. Load fills `criteria` (initialised first) and fails on
 * a file that isn't one. */
gboolean xd_search_criteria_save(const xd_search_criteria *criteria, const char *path,
                                 GError **error);
gboolean xd_search_criteria_load(xd_search_criteria *criteria, const char *path, GError **error);
/* The name XP gave a saved search: "Files named NAME" (or "Search
 * results"), without the extension. Free with g_free. */
char *xd_search_criteria_title(const xd_search_criteria *criteria);

/* A file as the query sees it. `name` is its own name (any bytes); the
 * contents are read through `read_contents` when a content term needs
 * them (nullptr: no contents), once. Clear with xd_search_item_clear. */
typedef GBytes *(*xd_search_read_func)(const void *data);
typedef struct xd_search_item {
    const char *name;
    bool is_dir, is_link;
    uint64_t size;
    int64_t modified, created, accessed; /* Unix time; 0: unknown */
    const char *content_type; /* may be nullptr */
    xd_search_read_func read_contents;
    const void *read_data;
    /* Filled in as needed. */
    char *display_name;
    GBytes *contents;
    bool contents_read;
} xd_search_item;
void xd_search_item_clear(xd_search_item *item);

/* The kind a file is, by its content type. */
xd_search_kind xd_search_kind_of(const char *content_type, bool is_dir, bool is_link);
/* kind:'s value ("picture", "pictures", "music", "movie"...), or NONE. */
xd_search_kind xd_search_kind_from_name(const char *name);

typedef struct xd_search_query xd_search_query;

/* The query for `criteria`, its 7 text and fields together (AND). `now`
 * is when relative dates count from (nullptr: now); `first_weekday` is
 * where a week starts, 1 Monday to 7 Sunday (0: the locale's). An
 * invalid regular expression, size or date is an error whose message
 * says so plainly. */
xd_search_query *xd_search_query_new(const xd_search_criteria *criteria, GDateTime *now,
                                     int first_weekday, GError **error);
/* AQS text alone, with the criteria's flags (case, partial matches,
 * names and contents, /pattern/). */
xd_search_query *xd_search_query_parse(const char *text, const xd_search_criteria *flags,
                                       GDateTime *now, int first_weekday, GError **error);
xd_search_query *xd_search_query_ref(xd_search_query *query);
void xd_search_query_unref(xd_search_query *query);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(xd_search_query, xd_search_query_unref)

bool xd_search_query_matches(const xd_search_query *query, xd_search_item *item);
/* Whether any term reads the contents (the walk reads only then). */
bool xd_search_query_needs_contents(const xd_search_query *query);
/* The query as text, for tests: (AND ...), (OR ...), (NOT ...), and
 * terms "prop<op>value" or "prop[low,high)". Free with g_free. */
char *xd_search_query_describe(const xd_search_query *query);

/* The byte ranges of `label` (UTF-8) that the query's name terms match,
 * for drawing them highlighted: every term outside a NOT, each match a
 * span, sorted and merged where they overlap. A wildcard's span is what
 * its letters cover ("*.txt" the ".txt"). */
typedef struct xd_search_span {
    size_t start, end;
} xd_search_span;
GArray *xd_search_query_name_spans(const xd_search_query *query, const char *label);

/* ---- the walk (searchwalk.c) ---- */

/* A file found: its GFile and what GIO says of it (name, type, size,
 * dates, content type, icon), or, inside an archive, the archive's path
 * and the entry's path in it. */
typedef struct xd_search_hit {
    GFile *file;
    GFileInfo *info;
    char *archive; /* nullptr: an ordinary file */
} xd_search_hit;

/* Called on the main thread: hits found since last time (xd_search_hit*,
 * the array's own; take what's wanted), how far it got (a fraction that
 * only grows, estimated from the folders still to do) and the folder
 * being searched; then once done. None is called after
 * xd_search_job_cancel. */
typedef struct xd_search_callbacks {
    void (*found)(GPtrArray *hits, gpointer data);
    void (*progress)(double fraction, const char *folder, gpointer data);
    void (*done)(guint found, gpointer data);
    gpointer data;
} xd_search_callbacks;

typedef struct xd_search_job xd_search_job;

/* Searches `criteria`'s Look in with `query` on a worker thread: links
 * aren't followed and no folder is searched twice; /proc, /sys, /dev and
 * /run only when Look in is inside them; other system folders (/usr,
 * /etc, /var...) only with criteria->system; hidden ones only with
 * criteria->hidden. */
xd_search_job *xd_search_job_start(const xd_search_criteria *criteria, xd_search_query *query,
                                   const xd_search_callbacks *callbacks);
/* Stops it; the job frees itself once its thread has seen that. */
void xd_search_job_cancel(xd_search_job *job);

void xd_search_hit_free(gpointer hit);

/* Whether `path` is one of the system folders the walk keeps out of
 * unless asked (pseudo: /proc, /sys, /dev, /run). */
bool xd_search_is_system_folder(const char *path, bool *pseudo);

G_END_DECLS

#endif
