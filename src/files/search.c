/* SPDX-License-Identifier: MIT
 * xd-files' search: criteria, Advanced Query Syntax and matching. See
 * search.h.
 */
#include "search.h"

#include <langinfo.h>
#include <string.h>

#include "extract.h"

/* ---- criteria ------------------------------------------------------------------------ */

void xd_search_criteria_init(xd_search_criteria *criteria) {
    *criteria = (xd_search_criteria){
        .date_property = XD_SEARCH_PROP_MODIFIED,
        .subfolders = true,
        .system = true,
        .partial = true,
        .allow_regex = true,
    };
}

void xd_search_criteria_clear(xd_search_criteria *criteria) {
    g_free(criteria->name);
    g_free(criteria->contents);
    g_strfreev(criteria->look_in);
    g_free(criteria->content_type);
    g_free(criteria->date_from);
    g_free(criteria->date_to);
    g_free(criteria->query);
    xd_search_criteria_init(criteria);
}

void xd_search_criteria_copy(xd_search_criteria *to, const xd_search_criteria *from) {
    *to = *from;
    to->name = g_strdup(from->name);
    to->contents = g_strdup(from->contents);
    to->look_in = g_strdupv(from->look_in);
    to->content_type = g_strdup(from->content_type);
    to->date_from = g_strdup(from->date_from);
    to->date_to = g_strdup(from->date_to);
    to->query = g_strdup(from->query);
}

static const char *const KIND_NAMES[] = {"folder",  "document", "picture", "music",
                                         "video",   "program",  "link",    "email",
                                         "contact", "calendar", "compressed", "other"};
static const char *const WHEN_NAMES[XD_SEARCH_WHEN_COUNT] = {"any", "week", "month", "year",
                                                              "dates"};
static const char *const SIZE_NAMES[XD_SEARCH_SIZE_COUNT] = {"any",   "small",    "medium",
                                                              "large", "at-least", "at-most"};

static int index_of_name(const char *const *names, size_t count, const char *name) {
    for (size_t i = 0; name != nullptr && i < count; i++)
        if (g_ascii_strcasecmp(names[i], name) == 0)
            return (int)i;
    return -1;
}

static void set_text(GKeyFile *key_file, const char *key, const char *value) {
    if (value != nullptr && value[0] != '\0')
        g_key_file_set_string(key_file, "Search", key, value);
}

gboolean xd_search_criteria_save(const xd_search_criteria *criteria, const char *path,
                                 GError **error) {
    g_autoptr(GKeyFile) key_file = g_key_file_new();
    g_key_file_set_integer(key_file, "Search", "Version", 1);
    set_text(key_file, "Name", criteria->name);
    set_text(key_file, "Contents", criteria->contents);
    if (criteria->look_in != nullptr && criteria->look_in[0] != nullptr)
        g_key_file_set_string_list(key_file, "Search", "LookIn",
                                   (const char *const *)criteria->look_in,
                                   g_strv_length(criteria->look_in));
    if (criteria->kinds != 0) {
        const char *kinds[G_N_ELEMENTS(KIND_NAMES)];
        gsize count = 0;
        for (size_t i = 0; i < G_N_ELEMENTS(KIND_NAMES); i++)
            if ((criteria->kinds & (1U << i)) != 0)
                kinds[count++] = KIND_NAMES[i];
        g_key_file_set_string_list(key_file, "Search", "Kinds", kinds, count);
    }
    set_text(key_file, "Type", criteria->content_type);
    g_key_file_set_string(key_file, "Search", "DateProperty",
                          criteria->date_property == XD_SEARCH_PROP_CREATED    ? "created"
                          : criteria->date_property == XD_SEARCH_PROP_ACCESSED ? "accessed"
                                                                                : "modified");
    if (criteria->when < XD_SEARCH_WHEN_COUNT)
        g_key_file_set_string(key_file, "Search", "When", WHEN_NAMES[criteria->when]);
    set_text(key_file, "From", criteria->date_from);
    set_text(key_file, "To", criteria->date_to);
    if (criteria->size < XD_SEARCH_SIZE_COUNT)
        g_key_file_set_string(key_file, "Search", "Size", SIZE_NAMES[criteria->size]);
    g_key_file_set_uint64(key_file, "Search", "SizeKB", criteria->size_kb);
    set_text(key_file, "Query", criteria->query);
    g_key_file_set_boolean(key_file, "Search", "Subfolders", criteria->subfolders);
    g_key_file_set_boolean(key_file, "Search", "CaseSensitive", criteria->case_sensitive);
    g_key_file_set_boolean(key_file, "Search", "Hidden", criteria->hidden);
    g_key_file_set_boolean(key_file, "Search", "System", criteria->system);
    g_key_file_set_boolean(key_file, "Search", "Regex", criteria->regex);
    g_key_file_set_boolean(key_file, "Search", "Archives", criteria->archives);
    g_key_file_set_boolean(key_file, "Search", "Partial", criteria->partial);
    g_key_file_set_boolean(key_file, "Search", "NamesAndContents", criteria->names_and_contents);
    g_key_file_set_boolean(key_file, "Search", "AllowRegex", criteria->allow_regex);
    return g_key_file_save_to_file(key_file, path, error);
}

static char *get_text(GKeyFile *key_file, const char *key) {
    char *value = g_key_file_get_string(key_file, "Search", key, nullptr);
    if (value != nullptr && value[0] == '\0')
        g_clear_pointer(&value, g_free);
    return value;
}

static bool get_flag(GKeyFile *key_file, const char *key, bool fallback) {
    g_autoptr(GError) error = nullptr;
    bool value = g_key_file_get_boolean(key_file, "Search", key, &error);
    return error != nullptr ? fallback : value;
}

gboolean xd_search_criteria_load(xd_search_criteria *criteria, const char *path, GError **error) {
    g_autoptr(GKeyFile) key_file = g_key_file_new();
    if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, error))
        return FALSE;
    if (!g_key_file_has_group(key_file, "Search")) {
        g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
                    "%s isn't a saved search.", path);
        return FALSE;
    }
    xd_search_criteria_clear(criteria);
    criteria->name = get_text(key_file, "Name");
    criteria->contents = get_text(key_file, "Contents");
    criteria->look_in = g_key_file_get_string_list(key_file, "Search", "LookIn", nullptr, nullptr);
    g_auto(GStrv) kinds = g_key_file_get_string_list(key_file, "Search", "Kinds", nullptr, nullptr);
    for (size_t i = 0; kinds != nullptr && kinds[i] != nullptr; i++) {
        int bit = index_of_name(KIND_NAMES, G_N_ELEMENTS(KIND_NAMES), kinds[i]);
        if (bit >= 0)
            criteria->kinds |= 1U << bit;
    }
    criteria->content_type = get_text(key_file, "Type");
    g_autofree char *property = get_text(key_file, "DateProperty");
    criteria->date_property = g_strcmp0(property, "created") == 0    ? XD_SEARCH_PROP_CREATED
                              : g_strcmp0(property, "accessed") == 0 ? XD_SEARCH_PROP_ACCESSED
                                                                      : XD_SEARCH_PROP_MODIFIED;
    g_autofree char *when = get_text(key_file, "When");
    int when_index = index_of_name(WHEN_NAMES, G_N_ELEMENTS(WHEN_NAMES), when);
    criteria->when = when_index >= 0 ? (xd_search_when)when_index : XD_SEARCH_WHEN_ANY;
    criteria->date_from = get_text(key_file, "From");
    criteria->date_to = get_text(key_file, "To");
    g_autofree char *size = get_text(key_file, "Size");
    int size_index = index_of_name(SIZE_NAMES, G_N_ELEMENTS(SIZE_NAMES), size);
    criteria->size = size_index >= 0 ? (xd_search_size)size_index : XD_SEARCH_SIZE_ANY;
    criteria->size_kb = g_key_file_get_uint64(key_file, "Search", "SizeKB", nullptr);
    criteria->query = get_text(key_file, "Query");
    criteria->subfolders = get_flag(key_file, "Subfolders", criteria->subfolders);
    criteria->case_sensitive = get_flag(key_file, "CaseSensitive", criteria->case_sensitive);
    criteria->hidden = get_flag(key_file, "Hidden", criteria->hidden);
    criteria->system = get_flag(key_file, "System", criteria->system);
    criteria->regex = get_flag(key_file, "Regex", criteria->regex);
    criteria->archives = get_flag(key_file, "Archives", criteria->archives);
    criteria->partial = get_flag(key_file, "Partial", criteria->partial);
    criteria->names_and_contents =
        get_flag(key_file, "NamesAndContents", criteria->names_and_contents);
    criteria->allow_regex = get_flag(key_file, "AllowRegex", criteria->allow_regex);
    return TRUE;
}

char *xd_search_criteria_title(const xd_search_criteria *criteria) {
    if (criteria->name != nullptr && criteria->name[0] != '\0')
        return g_strdup_printf("Files named %s", criteria->name);
    if (criteria->query != nullptr && criteria->query[0] != '\0')
        return g_strdup(criteria->query);
    if (criteria->contents != nullptr && criteria->contents[0] != '\0')
        return g_strdup_printf("Files containing %s", criteria->contents);
    return g_strdup("Search results");
}

/* ---- kinds ----------------------------------------------------------------------------- */

static bool has_prefix_in(const char *content_type, const char *const *prefixes, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (g_str_has_prefix(content_type, prefixes[i]))
            return true;
    return false;
}

xd_search_kind xd_search_kind_of(const char *content_type, bool is_dir, bool is_link) {
    if (is_dir)
        return XD_SEARCH_KIND_FOLDER;
    if (is_link)
        return XD_SEARCH_KIND_LINK;
    if (content_type == nullptr)
        return XD_SEARCH_KIND_OTHER;
    if (g_content_type_is_a(content_type, "inode/directory"))
        return XD_SEARCH_KIND_FOLDER;
    if (g_content_type_is_a(content_type, "application/x-desktop"))
        return XD_SEARCH_KIND_LINK;
    if (g_str_has_prefix(content_type, "image/"))
        return XD_SEARCH_KIND_PICTURE;
    if (g_str_has_prefix(content_type, "audio/"))
        return XD_SEARCH_KIND_MUSIC;
    if (g_str_has_prefix(content_type, "video/"))
        return XD_SEARCH_KIND_VIDEO;
    if (g_content_type_is_a(content_type, "message/rfc822"))
        return XD_SEARCH_KIND_EMAIL;
    if (g_content_type_is_a(content_type, "text/vcard") ||
        g_content_type_is_a(content_type, "text/x-vcard") ||
        g_content_type_is_a(content_type, "text/directory"))
        return XD_SEARCH_KIND_CONTACT;
    if (g_content_type_is_a(content_type, "text/calendar"))
        return XD_SEARCH_KIND_CALENDAR;
    static const char *const programs[] = {
        "application/x-executable", "application/x-pie-executable", "application/x-shellscript",
        "application/vnd.microsoft.portable-executable", "application/x-msdownload",
        "application/x-ms-dos-executable", "application/x-msi", "application/vnd.appimage",
        "application/x-sharedlib"};
    for (size_t i = 0; i < G_N_ELEMENTS(programs); i++)
        if (g_content_type_is_a(content_type, programs[i]))
            return XD_SEARCH_KIND_PROGRAM;
    if (xd_is_archive_type(content_type))
        return XD_SEARCH_KIND_COMPRESSED;
    static const char *const documents[] = {
        "text/", "application/pdf", "application/rtf", "application/msword",
        "application/vnd.ms-", "application/vnd.oasis.opendocument.",
        "application/vnd.openxmlformats-officedocument.", "application/postscript",
        "application/x-abiword", "application/epub+zip", "application/vnd.apple."};
    if (has_prefix_in(content_type, documents, G_N_ELEMENTS(documents)) ||
        g_content_type_is_a(content_type, "text/plain"))
        return XD_SEARCH_KIND_DOCUMENT;
    return XD_SEARCH_KIND_OTHER;
}

xd_search_kind xd_search_kind_from_name(const char *name) {
    static const struct {
        const char *name;
        xd_search_kind kind;
    } names[] = {
        {"folder", XD_SEARCH_KIND_FOLDER},       {"folders", XD_SEARCH_KIND_FOLDER},
        {"document", XD_SEARCH_KIND_DOCUMENT},   {"documents", XD_SEARCH_KIND_DOCUMENT},
        {"picture", XD_SEARCH_KIND_PICTURE},     {"pictures", XD_SEARCH_KIND_PICTURE},
        {"photo", XD_SEARCH_KIND_PICTURE},       {"photos", XD_SEARCH_KIND_PICTURE},
        {"image", XD_SEARCH_KIND_PICTURE},       {"images", XD_SEARCH_KIND_PICTURE},
        {"music", XD_SEARCH_KIND_MUSIC},         {"song", XD_SEARCH_KIND_MUSIC},
        {"songs", XD_SEARCH_KIND_MUSIC},         {"audio", XD_SEARCH_KIND_MUSIC},
        {"video", XD_SEARCH_KIND_VIDEO},         {"videos", XD_SEARCH_KIND_VIDEO},
        {"movie", XD_SEARCH_KIND_VIDEO},         {"movies", XD_SEARCH_KIND_VIDEO},
        {"program", XD_SEARCH_KIND_PROGRAM},     {"programs", XD_SEARCH_KIND_PROGRAM},
        {"application", XD_SEARCH_KIND_PROGRAM}, {"link", XD_SEARCH_KIND_LINK},
        {"links", XD_SEARCH_KIND_LINK},          {"shortcut", XD_SEARCH_KIND_LINK},
        {"email", XD_SEARCH_KIND_EMAIL},         {"e-mail", XD_SEARCH_KIND_EMAIL},
        {"contact", XD_SEARCH_KIND_CONTACT},     {"contacts", XD_SEARCH_KIND_CONTACT},
        {"calendar", XD_SEARCH_KIND_CALENDAR},   {"compressed", XD_SEARCH_KIND_COMPRESSED},
        {"archive", XD_SEARCH_KIND_COMPRESSED},
    };
    for (size_t i = 0; name != nullptr && i < G_N_ELEMENTS(names); i++)
        if (g_ascii_strcasecmp(names[i].name, name) == 0)
            return names[i].kind;
    return XD_SEARCH_KIND_NONE;
}

/* ---- the query's tree ---------------------------------------------------------------------- */

typedef enum node_type : uint8_t { NODE_AND, NODE_OR, NODE_NOT, NODE_TERM } node_type;

typedef enum text_mode : uint8_t {
    TEXT_CONTAINS,
    TEXT_WORD, /* whole words (7's Find partial matches off) */
    TEXT_EXACT,
    TEXT_WILDCARD,
    TEXT_REGEX,
} text_mode;

typedef struct node {
    node_type type;
    GPtrArray *children; /* node* (AND, OR, NOT) */
    xd_search_property property;
    char *value; /* as given, for describe */
    /* Names, words and phrases. */
    text_mode mode;
    bool caseless;
    GRegex *regex; /* names, and contents as text */
    GRegex *raw_regex; /* contents as bytes */
    GRegex *span_regex; /* a wildcard's letters, for highlighting */
    /* Sizes and dates: [low, high). */
    int64_t low, high;
    uint32_t kinds;
    char *type_extension, *type_mime, *type_description;
} node;

struct xd_search_query {
    gint refs;
    node *root;
    bool needs_contents;
};

static void free_node(gpointer data) {
    node *tree_node = data;
    if (tree_node == nullptr)
        return;
    if (tree_node->children != nullptr)
        g_ptr_array_unref(tree_node->children);
    g_free(tree_node->value);
    if (tree_node->regex != nullptr)
        g_regex_unref(tree_node->regex);
    if (tree_node->raw_regex != nullptr)
        g_regex_unref(tree_node->raw_regex);
    if (tree_node->span_regex != nullptr)
        g_regex_unref(tree_node->span_regex);
    g_free(tree_node->type_extension);
    g_free(tree_node->type_mime);
    g_free(tree_node->type_description);
    g_free(tree_node);
}

static node *new_group(node_type type) {
    node *group = g_new0(node, 1);
    group->type = type;
    group->children = g_ptr_array_new_with_free_func(free_node);
    return group;
}

static node *new_term(xd_search_property property, const char *value) {
    node *term = g_new0(node, 1);
    term->type = NODE_TERM;
    term->property = property;
    term->value = g_strdup(value != nullptr ? value : "");
    return term;
}

static node *wrap_not(node *operand) {
    node *negation = new_group(NODE_NOT);
    g_ptr_array_add(negation->children, operand);
    return negation;
}

static node *new_pair(node_type type, node *left, node *right) {
    node *group = new_group(type);
    g_ptr_array_add(group->children, left);
    g_ptr_array_add(group->children, right);
    return group;
}

static bool reads_contents(const node *tree_node) {
    if (tree_node->type == NODE_TERM)
        return tree_node->property == XD_SEARCH_PROP_CONTENT;
    for (guint i = 0; i < tree_node->children->len; i++)
        if (reads_contents(g_ptr_array_index(tree_node->children, i)))
            return true;
    return false;
}

/* Contents last in each AND: they're read only when everything else
 * already matches. */
static void order_children(node *tree_node) {
    if (tree_node->type == NODE_TERM)
        return;
    for (guint i = 0; i < tree_node->children->len; i++)
        order_children(g_ptr_array_index(tree_node->children, i));
    if (tree_node->type != NODE_AND)
        return;
    GPtrArray *children = tree_node->children;
    guint cheap = 0;
    for (guint i = 0; i < children->len; i++) {
        if (!reads_contents(g_ptr_array_index(children, i))) {
            gpointer moved = children->pdata[i];
            for (guint k = i; k > cheap; k--)
                children->pdata[k] = children->pdata[k - 1];
            children->pdata[cheap++] = moved;
        }
    }
}

/* ---- compiling words and patterns ------------------------------------------------------- */

/* "Error while compiling regular expression ‘x’ at char 2: missing )":
 * the last part, which says what's wrong. */
static void set_regex_error(GError **error, const char *pattern, const GError *cause) {
    const char *reason = cause->message;
    const char *colon = strrchr(reason, ':');
    if (colon != nullptr && colon[1] == ' ')
        reason = colon + 2;
    g_set_error(error, G_REGEX_ERROR, cause->code, "\"%s\" isn't a valid regular expression: %s.",
                pattern, reason);
}

static GRegex *compile(const char *pattern, GRegexCompileFlags flags, const char *shown,
                       GError **error) {
    GError *cause = nullptr;
    GRegex *regex = g_regex_new(pattern, (GRegexCompileFlags)(flags | G_REGEX_OPTIMIZE),
                                G_REGEX_MATCH_DEFAULT, &cause);
    if (regex == nullptr) {
        set_regex_error(error, shown, cause);
        g_error_free(cause);
    }
    return regex;
}

static bool is_word_char(gunichar character) {
    return g_unichar_isalnum(character) || character == '_';
}

/* A wildcard pattern as a regular expression: * any run, ? one
 * character, the rest as itself. */
static void append_glob(GString *pattern, const char *glob, size_t length) {
    const char *end = glob + length;
    const char *literal = glob;
    for (const char *cursor = glob; cursor < end; cursor++) {
        if (*cursor != '*' && *cursor != '?')
            continue;
        if (cursor > literal) {
            g_autofree char *escaped = g_regex_escape_string(literal, (gint)(cursor - literal));
            g_string_append(pattern, escaped);
        }
        g_string_append(pattern, *cursor == '*' ? ".*" : ".");
        literal = cursor + 1;
    }
    if (end > literal) {
        g_autofree char *escaped = g_regex_escape_string(literal, (gint)(end - literal));
        g_string_append(pattern, escaped);
    }
}

/* The bytes of `text` (and its UTF-16LE form) as a pattern over raw
 * bytes: letters as themselves (so CASELESS takes either case of ASCII
 * letters, as XP's byte search did), everything else as \x escapes. */
static void append_raw_bytes(GString *pattern, const guint8 *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (g_ascii_isalnum((char)bytes[i]))
            g_string_append_c(pattern, (char)bytes[i]);
        else
            g_string_append_printf(pattern, "\\x%02x", (unsigned)bytes[i]);
    }
}

static GRegex *compile_raw_phrase(const char *text, bool caseless) {
    GString *pattern = g_string_new("(?:");
    append_raw_bytes(pattern, (const guint8 *)text, strlen(text));
    glong units = 0;
    gunichar2 *wide = g_utf8_to_utf16(text, -1, nullptr, &units, nullptr);
    if (wide != nullptr && units > 0) {
        g_string_append(pattern, ")|(?:");
        for (glong i = 0; i < units; i++) {
            guint8 pair[2] = {(guint8)(wide[i] & 0xFF), (guint8)(wide[i] >> 8)};
            append_raw_bytes(pattern, pair, 2);
        }
    }
    g_free(wide);
    g_string_append_c(pattern, ')');
    GRegexCompileFlags flags = G_REGEX_RAW;
    if (caseless)
        flags |= G_REGEX_CASELESS;
    GRegex *regex = g_regex_new(pattern->str, flags, G_REGEX_MATCH_DEFAULT, nullptr);
    g_string_free(pattern, TRUE);
    return regex;
}

/* Compiles a name, word or phrase term's patterns. */
static bool compile_text(node *term, text_mode mode, bool caseless, GError **error) {
    const char *value = term->value;
    term->mode = mode;
    term->caseless = caseless;
    GRegexCompileFlags flags = caseless ? G_REGEX_CASELESS : (GRegexCompileFlags)0;
    g_autoptr(GString) pattern = g_string_new(nullptr);
    switch (mode) {
    case TEXT_REGEX:
        g_string_assign(pattern, value);
        break;
    case TEXT_WILDCARD: {
        g_string_append_c(pattern, '^');
        append_glob(pattern, value, strlen(value));
        g_string_append_c(pattern, '$');
        /* What to highlight: the letters, anchored where no * is. */
        size_t start = 0, end = strlen(value);
        while (start < end && value[start] == '*')
            start++;
        while (end > start && value[end - 1] == '*')
            end--;
        if (end > start) {
            g_autoptr(GString) span = g_string_new(start == 0 ? "^" : nullptr);
            append_glob(span, value + start, end - start);
            if (end == strlen(value))
                g_string_append_c(span, '$');
            term->span_regex = g_regex_new(span->str, flags, G_REGEX_MATCH_DEFAULT, nullptr);
        }
        break;
    }
    case TEXT_EXACT: {
        g_autofree char *escaped = g_regex_escape_string(value, -1);
        g_string_printf(pattern, "^%s$", escaped);
        break;
    }
    case TEXT_WORD: {
        g_autofree char *escaped = g_regex_escape_string(value, -1);
        gunichar first = g_utf8_get_char(value);
        gunichar last = g_utf8_get_char(g_utf8_find_prev_char(value, value + strlen(value)));
        g_string_printf(pattern, "%s%s%s", is_word_char(first) ? "(?<!\\w)" : "", escaped,
                        is_word_char(last) ? "(?!\\w)" : "");
        break;
    }
    case TEXT_CONTAINS: {
        g_autofree char *escaped = g_regex_escape_string(value, -1);
        g_string_assign(pattern, escaped);
        break;
    }
    }
    term->regex = compile(pattern->str, flags, value, error);
    if (term->regex == nullptr)
        return false;
    if (term->property == XD_SEARCH_PROP_CONTENT) {
        term->raw_regex = mode == TEXT_REGEX
                              ? g_regex_new(value, (GRegexCompileFlags)(flags | G_REGEX_RAW),
                                            G_REGEX_MATCH_DEFAULT, nullptr)
                              : compile_raw_phrase(value, caseless);
    }
    return true;
}

/* ---- dates and sizes ------------------------------------------------------------------- */

typedef struct calendar {
    GDateTime *now; /* local */
    int first_weekday; /* 1 Monday .. 7 Sunday */
} calendar;

static int locale_first_weekday(void) {
    /* glibc counts from _NL_TIME_WEEK_1STDAY, a Sunday in every locale
     * it ships: 1 is Sunday. */
    const char *first = nl_langinfo(_NL_TIME_FIRST_WEEKDAY);
    int sunday_based = first != nullptr ? first[0] : 2;
    if (sunday_based < 1 || sunday_based > 7)
        return 1;
    return sunday_based == 1 ? 7 : sunday_based - 1;
}

static int64_t day_start(GDateTime *when, int add_days) {
    g_autoptr(GDateTime) midnight =
        g_date_time_new_local(g_date_time_get_year(when), g_date_time_get_month(when),
                              g_date_time_get_day_of_month(when), 0, 0, 0);
    g_autoptr(GDateTime) moved = g_date_time_add_days(midnight, add_days);
    return g_date_time_to_unix(moved);
}

static int64_t month_start(GDateTime *when, int add_months) {
    g_autoptr(GDateTime) first =
        g_date_time_new_local(g_date_time_get_year(when), g_date_time_get_month(when), 1, 0, 0, 0);
    g_autoptr(GDateTime) moved = g_date_time_add_months(first, add_months);
    return g_date_time_to_unix(moved);
}

static int64_t year_start(GDateTime *when, int add_years) {
    g_autoptr(GDateTime) first =
        g_date_time_new_local(g_date_time_get_year(when) + add_years, 1, 1, 0, 0, 0);
    return g_date_time_to_unix(first);
}

/* A date word, or a date, as the days it covers: [start, end). */
static bool parse_date(const calendar *days, const char *text, int64_t *start, int64_t *end) {
    g_autofree char *lower = g_utf8_strdown(text, -1);
    g_strstrip(lower);
    GDateTime *now = days->now;
    int weekday = g_date_time_get_day_of_week(now);
    int into_week = (weekday - days->first_weekday + 7) % 7;
    int64_t today = day_start(now, 0);
    int64_t week = day_start(now, -into_week);
    int64_t last_week = day_start(now, -into_week - 7);
    int64_t month = month_start(now, 0);
    int64_t year = year_start(now, 0);
    static const char *const words[] = {
        "today",     "yesterday",  "this week",  "last week",          "this month",
        "last month", "this year", "last year",  "earlier this week", "earlier this month",
        "earlier this year", "a long time ago"};
    int word = index_of_name(words, G_N_ELEMENTS(words), lower);
    switch (word) {
    case 0:
        *start = today, *end = day_start(now, 1);
        return true;
    case 1:
        *start = day_start(now, -1), *end = today;
        return true;
    case 2:
        *start = week, *end = day_start(now, 7 - into_week);
        return true;
    case 3:
        *start = last_week, *end = week;
        return true;
    case 4:
        *start = month, *end = month_start(now, 1);
        return true;
    case 5:
        *start = month_start(now, -1), *end = month;
        return true;
    case 6:
        *start = year, *end = year_start(now, 1);
        return true;
    case 7:
        *start = year_start(now, -1), *end = year;
        return true;
    case 8: /* 7's calendar: this week before yesterday */
        *start = week, *end = MAX(week, day_start(now, -1));
        return true;
    case 9: /* this month before last week */
        *start = month, *end = MAX(month, last_week);
        return true;
    case 10: /* this year before that */
        *start = year, *end = MAX(year, MIN(month, last_week));
        return true;
    case 11:
        *start = INT64_MIN, *end = year;
        return true;
    default:
        break;
    }
    /* A year alone. */
    if (strlen(lower) == 4 && g_ascii_isdigit(lower[0]) && g_ascii_isdigit(lower[1]) &&
        g_ascii_isdigit(lower[2]) && g_ascii_isdigit(lower[3])) {
        int number = (int)g_ascii_strtoll(lower, nullptr, 10);
        if (number < 1 || number > 9998)
            return false;
        g_autoptr(GDateTime) first = g_date_time_new_local(number, 1, 1, 0, 0, 0);
        g_autoptr(GDateTime) next = g_date_time_new_local(number + 1, 1, 1, 0, 0, 0);
        *start = g_date_time_to_unix(first), *end = g_date_time_to_unix(next);
        return true;
    }
    /* A day, written as the locale writes dates (GLib's parser), or
     * ISO 8601. */
    GDate date;
    g_date_clear(&date, 1);
    g_date_set_parse(&date, lower);
    if (!g_date_valid(&date))
        return false;
    g_autoptr(GDateTime) midnight =
        g_date_time_new_local(g_date_get_year(&date), g_date_get_month(&date),
                              g_date_get_day(&date), 0, 0, 0);
    if (midnight == nullptr)
        return false;
    *start = g_date_time_to_unix(midnight);
    *end = day_start(midnight, 1);
    return true;
}

static constexpr int64_t KB = 1024;

/* A size, a size word, as bytes: [start, end). A number covers itself. */
static bool parse_size(const char *text, int64_t *start, int64_t *end) {
    static const struct {
        const char *name;
        int64_t low, high;
    } words[] = {
        {"empty", 0, 1},
        {"tiny", 0, 10 * KB},
        {"small", 10 * KB, 100 * KB},
        {"medium", 100 * KB, 1024 * KB},
        {"large", 1024 * KB, 16 * 1024 * KB},
        {"huge", 16 * 1024 * KB, 128 * 1024 * KB},
        {"gigantic", 128 * 1024 * KB, INT64_MAX},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(words); i++) {
        if (g_ascii_strcasecmp(text, words[i].name) == 0) {
            *start = words[i].low, *end = words[i].high;
            return true;
        }
    }
    char *unit = nullptr;
    double number = g_ascii_strtod(text, &unit);
    if (unit == text || !(number >= 0) || number > 1e15)
        return false;
    while (*unit == ' ')
        unit++;
    static const struct {
        const char *name;
        double factor;
    } units[] = {
        {"", 1},         {"b", 1},       {"k", 1024.0},         {"kb", 1024.0},
        {"m", 1048576.0}, {"mb", 1048576.0}, {"g", 1073741824.0}, {"gb", 1073741824.0},
        {"t", 1099511627776.0}, {"tb", 1099511627776.0},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(units); i++) {
        if (g_ascii_strcasecmp(unit, units[i].name) == 0) {
            double bytes = number * units[i].factor;
            if (bytes > 9.0e18)
                return false;
            *start = (int64_t)bytes;
            *end = *start + 1;
            return true;
        }
    }
    return false;
}

typedef enum compare_op : uint8_t {
    OP_NONE,
    OP_EQUAL,
    OP_CONTAINS,
    OP_NOT_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
} compare_op;

/* [low, high) for a value's [start, end) and the operator. */
static void apply_op(node *term, compare_op op, int64_t start, int64_t end) {
    term->low = INT64_MIN, term->high = INT64_MAX;
    switch (op) {
    case OP_GREATER:
        term->low = end;
        break;
    case OP_GREATER_EQUAL:
        term->low = start;
        break;
    case OP_LESS:
        term->high = start;
        break;
    case OP_LESS_EQUAL:
        term->high = end;
        break;
    case OP_NONE:
    case OP_EQUAL:
    case OP_CONTAINS:
    case OP_NOT_EQUAL:
        term->low = start, term->high = end;
        break;
    }
}

static bool is_range_property(xd_search_property property) {
    return property == XD_SEARCH_PROP_SIZE || property == XD_SEARCH_PROP_MODIFIED ||
           property == XD_SEARCH_PROP_CREATED || property == XD_SEARCH_PROP_ACCESSED;
}

static bool parse_range_value(const calendar *days, xd_search_property property, const char *text,
                              int64_t *start, int64_t *end, GError **error) {
    bool ok = property == XD_SEARCH_PROP_SIZE ? parse_size(text, start, end)
                                              : parse_date(days, text, start, end);
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    property == XD_SEARCH_PROP_SIZE ? "\"%s\" isn't a size."
                                                    : "\"%s\" isn't a date.",
                    text);
    return ok;
}

/* type:'s value: ".txt" or "txt" (an extension), "image/png" (a content
 * type, with its subtypes), or words of the type's description. */
static void set_type(node *term, const char *value) {
    g_autofree char *lower = g_utf8_strdown(value, -1);
    if (lower[0] == '.')
        term->type_extension = g_strdup(lower + 1);
    else if (strchr(lower, '/') != nullptr)
        term->type_mime = g_strdup(lower);
    else {
        if (strchr(lower, ' ') == nullptr)
            term->type_extension = g_strdup(lower);
        term->type_description = g_utf8_casefold(lower, -1);
    }
}

/* ---- Advanced Query Syntax ------------------------------------------------------------------ */

typedef struct parser {
    const char *cursor;
    const xd_search_criteria *flags;
    calendar days;
    GError *error;
} parser;

typedef enum value_kind : uint8_t { VALUE_WORD, VALUE_QUOTED, VALUE_REGEX } value_kind;

static void skip_space(parser *state) {
    while (*state->cursor == ' ' || *state->cursor == '\t' || *state->cursor == '\n')
        state->cursor++;
}

static bool ends_word(char character) {
    return character == '\0' || character == ' ' || character == '\t' || character == '\n' ||
           character == '(' || character == ')';
}

/* AND, OR, NOT: upper case, a word of their own. */
static bool take_keyword(parser *state, const char *keyword) {
    size_t length = strlen(keyword);
    if (strncmp(state->cursor, keyword, length) != 0 || !ends_word(state->cursor[length]))
        return false;
    state->cursor += length;
    return true;
}

static bool at_keyword(const parser *state, const char *keyword) {
    size_t length = strlen(keyword);
    return strncmp(state->cursor, keyword, length) == 0 && ends_word(state->cursor[length]);
}

static xd_search_property property_named(const char *name, size_t length, bool *known) {
    static const struct {
        const char *name;
        xd_search_property property;
    } names[] = {
        {"name", XD_SEARCH_PROP_NAME},
        {"filename", XD_SEARCH_PROP_NAME},
        {"content", XD_SEARCH_PROP_CONTENT},
        {"contents", XD_SEARCH_PROP_CONTENT},
        {"kind", XD_SEARCH_PROP_KIND},
        {"type", XD_SEARCH_PROP_TYPE},
        {"ext", XD_SEARCH_PROP_TYPE},
        {"extension", XD_SEARCH_PROP_TYPE},
        {"datemodified", XD_SEARCH_PROP_MODIFIED},
        {"modified", XD_SEARCH_PROP_MODIFIED},
        {"date", XD_SEARCH_PROP_MODIFIED},
        {"datecreated", XD_SEARCH_PROP_CREATED},
        {"created", XD_SEARCH_PROP_CREATED},
        {"dateaccessed", XD_SEARCH_PROP_ACCESSED},
        {"accessed", XD_SEARCH_PROP_ACCESSED},
        {"size", XD_SEARCH_PROP_SIZE},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        if (strlen(names[i].name) == length && g_ascii_strncasecmp(names[i].name, name, length) == 0) {
            *known = true;
            return names[i].property;
        }
    }
    *known = false;
    return XD_SEARCH_PROP_ANY;
}

static compare_op take_op(parser *state) {
    static const struct {
        const char *text;
        compare_op op;
    } ops[] = {{">=", OP_GREATER_EQUAL}, {"<=", OP_LESS_EQUAL}, {"<>", OP_NOT_EQUAL},
               {">", OP_GREATER},        {"<", OP_LESS},        {"=", OP_EQUAL},
               {"~", OP_CONTAINS}};
    for (size_t i = 0; i < G_N_ELEMENTS(ops); i++) {
        size_t length = strlen(ops[i].text);
        if (strncmp(state->cursor, ops[i].text, length) == 0) {
            state->cursor += length;
            return ops[i].op;
        }
    }
    return OP_NONE;
}

static char *take_word(parser *state) {
    const char *start = state->cursor;
    while (!ends_word(*state->cursor) && *state->cursor != '"')
        state->cursor++;
    return g_strndup(start, (gsize)(state->cursor - start));
}

/* Date words of more than one word ("this week", "a long time ago"). */
static char *take_date_words(parser *state, char *first) {
    static const char *const phrases[] = {"this week", "last week", "this month", "last month",
                                          "this year", "last year", "earlier this week",
                                          "earlier this month", "earlier this year",
                                          "a long time ago"};
    for (size_t i = 0; i < G_N_ELEMENTS(phrases); i++) {
        size_t first_length = strlen(first);
        if (g_ascii_strncasecmp(phrases[i], first, first_length) != 0 ||
            phrases[i][first_length] != ' ')
            continue;
        const char *rest = phrases[i] + first_length;
        size_t rest_length = strlen(rest);
        if (g_ascii_strncasecmp(state->cursor, rest, rest_length) == 0 &&
            ends_word(state->cursor[rest_length])) {
            state->cursor += rest_length;
            char *phrase = g_strdup(phrases[i]);
            g_free(first);
            return phrase;
        }
    }
    return first;
}

static node *make_text_term(parser *state, xd_search_property property, compare_op op,
                            const char *value, value_kind kind, bool regex_caseless) {
    const xd_search_criteria *flags = state->flags;
    bool caseless = !flags->case_sensitive;
    text_mode mode = TEXT_CONTAINS;
    if (kind == VALUE_REGEX) {
        mode = TEXT_REGEX;
        caseless = regex_caseless;
    } else if (kind == VALUE_WORD && strpbrk(value, "*?") != nullptr &&
               property != XD_SEARCH_PROP_CONTENT) {
        mode = TEXT_WILDCARD;
    } else if (op == OP_EQUAL || op == OP_NOT_EQUAL) {
        mode = property == XD_SEARCH_PROP_CONTENT ? TEXT_CONTAINS : TEXT_EXACT;
    } else if (property == XD_SEARCH_PROP_ANY && kind == VALUE_WORD && op == OP_NONE &&
               !flags->partial) {
        mode = TEXT_WORD;
    }
    xd_search_property name_property =
        property == XD_SEARCH_PROP_CONTENT ? XD_SEARCH_PROP_CONTENT : property;
    node *term = new_term(name_property, value);
    if (!compile_text(term, mode, caseless, state->error == nullptr ? &state->error : nullptr)) {
        free_node(term);
        return nullptr;
    }
    node *result = term;
    /* 7's "Always search file names and contents": a bare word or phrase
     * is looked for in the contents too. */
    if (property == XD_SEARCH_PROP_ANY && flags->names_and_contents && mode != TEXT_WILDCARD &&
        mode != TEXT_EXACT) {
        node *contents = new_term(XD_SEARCH_PROP_CONTENT, value);
        if (compile_text(contents, mode == TEXT_WORD ? TEXT_CONTAINS : mode, caseless, nullptr))
            result = new_pair(NODE_OR, term, contents);
        else
            free_node(contents);
    }
    return op == OP_NOT_EQUAL ? wrap_not(result) : result;
}

static node *make_term(parser *state, xd_search_property property, compare_op op, char *value,
                       char *upper, value_kind kind, bool regex_caseless) {
    node *term = nullptr;
    if (value[0] == '\0' && kind == VALUE_WORD)
        return new_group(NODE_AND); /* "size:" alone: nothing asked yet */
    switch (property) {
    case XD_SEARCH_PROP_ANY:
    case XD_SEARCH_PROP_NAME:
    case XD_SEARCH_PROP_CONTENT:
        return make_text_term(state, property, op, value, kind, regex_caseless);
    case XD_SEARCH_PROP_KIND:
        term = new_term(property, value);
        term->kinds = xd_search_kind_from_name(value);
        break;
    case XD_SEARCH_PROP_TYPE:
        term = new_term(property, value);
        set_type(term, value);
        break;
    case XD_SEARCH_PROP_MODIFIED:
    case XD_SEARCH_PROP_CREATED:
    case XD_SEARCH_PROP_ACCESSED:
    case XD_SEARCH_PROP_SIZE: {
        int64_t start = 0, end = 0;
        GError **error = state->error == nullptr ? &state->error : nullptr;
        if (!parse_range_value(&state->days, property, value, &start, &end, error))
            return nullptr;
        if (upper != nullptr && upper[0] != '\0') {
            int64_t upper_start = 0, upper_end = 0;
            if (!parse_range_value(&state->days, property, upper, &upper_start, &upper_end, error))
                return nullptr;
            end = upper_end;
            op = OP_NONE;
        }
        term = new_term(property, value);
        apply_op(term, op, start, end);
        break;
    }
    case XD_SEARCH_PROP_COUNT:
        return nullptr;
    }
    return op == OP_NOT_EQUAL ? wrap_not(term) : term;
}

static node *parse_or(parser *state, xd_search_property property);

static node *parse_term(parser *state, xd_search_property property) {
    /* prop: */
    const char *start = state->cursor;
    while (g_ascii_isalpha(*state->cursor))
        state->cursor++;
    bool known = false;
    if (*state->cursor == ':' && state->cursor > start) {
        xd_search_property named = property_named(start, (size_t)(state->cursor - start), &known);
        if (known) {
            property = named;
            state->cursor++;
        }
    }
    if (!known)
        state->cursor = start;
    /* prop:(a OR b) */
    if (known && *state->cursor == '(') {
        state->cursor++;
        node *group = parse_or(state, property);
        skip_space(state);
        if (*state->cursor == ')')
            state->cursor++;
        return group;
    }
    compare_op op = take_op(state);
    value_kind kind = VALUE_WORD;
    bool regex_caseless = false;
    char *value = nullptr;
    char *upper = nullptr;
    bool text_property = property == XD_SEARCH_PROP_ANY || property == XD_SEARCH_PROP_NAME ||
                         property == XD_SEARCH_PROP_CONTENT;
    if (*state->cursor == '"') {
        const char *quoted = ++state->cursor;
        while (*state->cursor != '\0' && *state->cursor != '"')
            state->cursor++;
        value = g_strndup(quoted, (gsize)(state->cursor - quoted));
        if (*state->cursor == '"')
            state->cursor++;
        kind = VALUE_QUOTED;
    } else if (*state->cursor == '/' && text_property && state->flags->allow_regex) {
        /* /pattern/flags: \/ is a slash in the pattern. */
        GString *pattern = g_string_new(nullptr);
        state->cursor++;
        while (*state->cursor != '\0' && *state->cursor != '/') {
            if (state->cursor[0] == '\\' && state->cursor[1] == '/') {
                g_string_append_c(pattern, '/');
                state->cursor += 2;
                continue;
            }
            g_string_append_c(pattern, *state->cursor++);
        }
        if (*state->cursor == '/') {
            state->cursor++;
            while (g_ascii_isalpha(*state->cursor)) {
                if (*state->cursor == 'i')
                    regex_caseless = true;
                state->cursor++;
            }
        }
        value = g_string_free(pattern, FALSE);
        kind = VALUE_REGEX;
    } else {
        value = take_word(state);
        if (property == XD_SEARCH_PROP_MODIFIED || property == XD_SEARCH_PROP_CREATED ||
            property == XD_SEARCH_PROP_ACCESSED) {
            if (*state->cursor == ' ')
                value = take_date_words(state, value);
        }
        /* a..b */
        char *dots = is_range_property(property) ? strstr(value, "..") : nullptr;
        if (dots != nullptr) {
            upper = g_strdup(dots + 2);
            *dots = '\0';
        }
    }
    node *term = make_term(state, property, op, value, upper, kind, regex_caseless);
    g_free(value);
    g_free(upper);
    return term;
}

static node *parse_unary(parser *state, xd_search_property property);

static node *parse_primary(parser *state, xd_search_property property) {
    if (*state->cursor == '(') {
        state->cursor++;
        node *group = parse_or(state, property);
        skip_space(state);
        if (*state->cursor == ')')
            state->cursor++;
        return group;
    }
    return parse_term(state, property);
}

static node *parse_unary(parser *state, xd_search_property property) {
    skip_space(state);
    if (take_keyword(state, "NOT")) {
        skip_space(state);
        if (*state->cursor == '\0' || *state->cursor == ')')
            return nullptr; /* NOT with nothing after it */
        node *operand = parse_unary(state, property);
        return operand != nullptr ? wrap_not(operand) : nullptr;
    }
    if (*state->cursor == '-') {
        state->cursor++;
        if (ends_word(*state->cursor))
            return nullptr; /* a dash alone */
        node *operand = parse_primary(state, property);
        return operand != nullptr ? wrap_not(operand) : nullptr;
    }
    return parse_primary(state, property);
}

/* Terms side by side (or with AND between them): all of them. */
static node *parse_and(parser *state, xd_search_property property) {
    node *group = new_group(NODE_AND);
    for (;;) {
        skip_space(state);
        if (*state->cursor == '\0' || *state->cursor == ')' || at_keyword(state, "OR"))
            break;
        if (take_keyword(state, "AND"))
            continue;
        node *operand = parse_unary(state, property);
        if (operand != nullptr)
            g_ptr_array_add(group->children, operand);
    }
    if (group->children->len == 0) {
        free_node(group);
        return nullptr;
    }
    if (group->children->len == 1) {
        node *only = g_ptr_array_steal_index(group->children, 0);
        free_node(group);
        return only;
    }
    return group;
}

static node *parse_or(parser *state, xd_search_property property) {
    node *group = new_group(NODE_OR);
    for (;;) {
        node *operand = parse_and(state, property);
        if (operand != nullptr)
            g_ptr_array_add(group->children, operand);
        skip_space(state);
        if (!take_keyword(state, "OR"))
            break;
    }
    if (group->children->len == 0) {
        free_node(group);
        return nullptr;
    }
    if (group->children->len == 1) {
        node *only = g_ptr_array_steal_index(group->children, 0);
        free_node(group);
        return only;
    }
    return group;
}

/* The whole text: a stray ")" is passed over, an unclosed "(" closes at
 * the end, an operator with nothing after it is dropped. */
static node *parse_text(parser *state) {
    node *all = new_group(NODE_AND);
    for (;;) {
        node *part = parse_or(state, XD_SEARCH_PROP_ANY);
        if (part != nullptr)
            g_ptr_array_add(all->children, part);
        skip_space(state);
        if (*state->cursor == '\0')
            break;
        state->cursor++; /* ")" */
    }
    if (all->children->len == 1) {
        node *only = g_ptr_array_steal_index(all->children, 0);
        free_node(all);
        return only;
    }
    return all;
}

static void init_calendar(calendar *days, GDateTime *now, int first_weekday) {
    days->now = now != nullptr ? g_date_time_to_local(now) : g_date_time_new_now_local();
    days->first_weekday = first_weekday >= 1 && first_weekday <= 7 ? first_weekday
                                                                    : locale_first_weekday();
}

static xd_search_query *finish(node *root) {
    order_children(root);
    xd_search_query *query = g_new0(xd_search_query, 1);
    query->refs = 1;
    query->root = root;
    query->needs_contents = reads_contents(root);
    return query;
}

xd_search_query *xd_search_query_parse(const char *text, const xd_search_criteria *flags,
                                       GDateTime *now, int first_weekday, GError **error) {
    parser state = {.cursor = text != nullptr ? text : "", .flags = flags};
    init_calendar(&state.days, now, first_weekday);
    node *root = parse_text(&state);
    g_date_time_unref(state.days.now);
    if (state.error != nullptr) {
        g_propagate_error(error, state.error);
        free_node(root);
        return nullptr;
    }
    return finish(root);
}

/* ---- the criteria as a query -------------------------------------------------------------- */

/* XP's name field: several names apart by commas or semicolons, any of
 * which may match; "quoted" text as it is. */
static node *name_terms(const xd_search_criteria *criteria, GError **error) {
    const char *text = criteria->name;
    if (criteria->regex) {
        node *term = new_term(XD_SEARCH_PROP_NAME, text);
        if (!compile_text(term, TEXT_REGEX, !criteria->case_sensitive, error)) {
            free_node(term);
            return nullptr;
        }
        return term;
    }
    node *any = new_group(NODE_OR);
    const char *cursor = text;
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ';' || *cursor == ' ')
            cursor++;
        if (*cursor == '\0')
            break;
        g_autofree char *piece = nullptr;
        text_mode mode = TEXT_CONTAINS;
        if (*cursor == '"') {
            const char *start = ++cursor;
            while (*cursor != '\0' && *cursor != '"')
                cursor++;
            piece = g_strndup(start, (gsize)(cursor - start));
            if (*cursor == '"')
                cursor++;
        } else {
            const char *start = cursor;
            while (*cursor != '\0' && *cursor != ',' && *cursor != ';')
                cursor++;
            piece = g_strstrip(g_strndup(start, (gsize)(cursor - start)));
            if (strpbrk(piece, "*?") != nullptr)
                mode = TEXT_WILDCARD;
        }
        if (piece[0] == '\0')
            continue;
        node *term = new_term(XD_SEARCH_PROP_NAME, piece);
        if (!compile_text(term, mode, !criteria->case_sensitive, error)) {
            free_node(term);
            free_node(any);
            return nullptr;
        }
        g_ptr_array_add(any->children, term);
    }
    if (any->children->len == 1) {
        node *only = g_ptr_array_steal_index(any->children, 0);
        free_node(any);
        return only;
    }
    if (any->children->len == 0) {
        free_node(any);
        return new_group(NODE_AND); /* only spaces and separators: any name */
    }
    return any;
}

static bool add_dates(node *all, const xd_search_criteria *criteria, const calendar *days,
                      GError **error) {
    xd_search_property property = criteria->date_property;
    if (property != XD_SEARCH_PROP_CREATED && property != XD_SEARCH_PROP_ACCESSED)
        property = XD_SEARCH_PROP_MODIFIED;
    node *term = new_term(property, WHEN_NAMES[criteria->when]);
    term->low = INT64_MIN, term->high = INT64_MAX;
    switch (criteria->when) {
    case XD_SEARCH_WHEN_WEEK: {
        g_autoptr(GDateTime) since = g_date_time_add_days(days->now, -7);
        term->low = g_date_time_to_unix(since);
        break;
    }
    case XD_SEARCH_WHEN_MONTH: {
        g_autoptr(GDateTime) since = g_date_time_add_months(days->now, -1);
        term->low = g_date_time_to_unix(since);
        break;
    }
    case XD_SEARCH_WHEN_YEAR: {
        g_autoptr(GDateTime) since = g_date_time_add_years(days->now, -1);
        term->low = g_date_time_to_unix(since);
        break;
    }
    case XD_SEARCH_WHEN_DATES: {
        int64_t start = 0, end = 0;
        if (criteria->date_from != nullptr && criteria->date_from[0] != '\0') {
            if (!parse_range_value(days, property, criteria->date_from, &start, &end, error)) {
                free_node(term);
                return false;
            }
            term->low = start;
        }
        if (criteria->date_to != nullptr && criteria->date_to[0] != '\0') {
            if (!parse_range_value(days, property, criteria->date_to, &start, &end, error)) {
                free_node(term);
                return false;
            }
            term->high = end;
        }
        break;
    }
    case XD_SEARCH_WHEN_ANY:
    case XD_SEARCH_WHEN_COUNT:
        free_node(term);
        return true;
    }
    g_ptr_array_add(all->children, term);
    return true;
}

static void add_size(node *all, const xd_search_criteria *criteria) {
    int64_t low = INT64_MIN, high = INT64_MAX;
    int64_t given = criteria->size_kb > (uint64_t)(INT64_MAX / KB) ? INT64_MAX / KB
                                                                   : (int64_t)criteria->size_kb;
    switch (criteria->size) {
    case XD_SEARCH_SIZE_SMALL:
        low = 0, high = 100 * KB;
        break;
    case XD_SEARCH_SIZE_MEDIUM:
        low = 0, high = 1024 * KB;
        break;
    case XD_SEARCH_SIZE_LARGE:
        low = 1024 * KB + 1;
        break;
    case XD_SEARCH_SIZE_AT_LEAST:
        low = given * KB;
        break;
    case XD_SEARCH_SIZE_AT_MOST:
        low = 0, high = given * KB + 1;
        break;
    case XD_SEARCH_SIZE_ANY:
    case XD_SEARCH_SIZE_COUNT:
        return;
    }
    node *term = new_term(XD_SEARCH_PROP_SIZE, SIZE_NAMES[criteria->size]);
    term->low = low, term->high = high;
    g_ptr_array_add(all->children, term);
}

xd_search_query *xd_search_query_new(const xd_search_criteria *criteria, GDateTime *now,
                                     int first_weekday, GError **error) {
    calendar days;
    init_calendar(&days, now, first_weekday);
    node *all = new_group(NODE_AND);
    bool ok = true;
    if (criteria->name != nullptr && criteria->name[0] != '\0') {
        node *names = name_terms(criteria, error);
        ok = names != nullptr;
        if (ok)
            g_ptr_array_add(all->children, names);
    }
    if (ok && criteria->contents != nullptr && criteria->contents[0] != '\0') {
        node *term = new_term(XD_SEARCH_PROP_CONTENT, criteria->contents);
        ok = compile_text(term, criteria->regex ? TEXT_REGEX : TEXT_CONTAINS,
                          !criteria->case_sensitive, error);
        if (ok)
            g_ptr_array_add(all->children, term);
        else
            free_node(term);
    }
    if (ok && criteria->kinds != 0) {
        node *term = new_term(XD_SEARCH_PROP_KIND, "");
        term->kinds = criteria->kinds;
        g_ptr_array_add(all->children, term);
    }
    if (ok && criteria->content_type != nullptr && criteria->content_type[0] != '\0') {
        node *term = new_term(XD_SEARCH_PROP_TYPE, criteria->content_type);
        term->type_mime = g_strdup(criteria->content_type);
        g_ptr_array_add(all->children, term);
    }
    if (ok)
        ok = add_dates(all, criteria, &days, error);
    if (ok)
        add_size(all, criteria);
    g_date_time_unref(days.now);
    if (ok && criteria->query != nullptr && criteria->query[0] != '\0') {
        xd_search_query *typed =
            xd_search_query_parse(criteria->query, criteria, now, first_weekday, error);
        ok = typed != nullptr;
        if (ok) {
            g_ptr_array_add(all->children, typed->root);
            typed->root = nullptr;
            g_free(typed);
        }
    }
    if (!ok) {
        free_node(all);
        return nullptr;
    }
    return finish(all);
}

xd_search_query *xd_search_query_ref(xd_search_query *query) {
    g_atomic_int_inc(&query->refs);
    return query;
}

void xd_search_query_unref(xd_search_query *query) {
    if (query == nullptr || !g_atomic_int_dec_and_test(&query->refs))
        return;
    free_node(query->root);
    g_free(query);
}

bool xd_search_query_needs_contents(const xd_search_query *query) {
    return query->needs_contents;
}

/* ---- matching ------------------------------------------------------------------------------ */

void xd_search_item_clear(xd_search_item *item) {
    g_clear_pointer(&item->display_name, g_free);
    g_clear_pointer(&item->contents, g_bytes_unref);
    item->contents_read = false;
}

static const char *display_name(xd_search_item *item) {
    if (item->display_name == nullptr)
        item->display_name = g_filename_display_name(item->name != nullptr ? item->name : "");
    return item->display_name;
}

static bool match_contents(const node *term, xd_search_item *item) {
    if (!item->contents_read) {
        item->contents_read = true;
        if (item->read_contents != nullptr && !item->is_dir)
            item->contents = item->read_contents(item->read_data);
    }
    if (item->contents == nullptr)
        return false;
    gsize length = 0;
    const char *data = g_bytes_get_data(item->contents, &length);
    if (data == nullptr || length == 0)
        return false;
    const guint8 *bytes = (const guint8 *)data;
    /* Text as text: UTF-8, and UTF-16 marked with its byte order mark. */
    g_autofree char *converted = nullptr;
    const char *text = nullptr;
    gsize text_length = 0;
    if (length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        text = data + 3, text_length = length - 3;
    } else if (length >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
                               (bytes[0] == 0xFE && bytes[1] == 0xFF))) {
        converted = g_convert(data + 2, (gssize)(length - 2), "UTF-8",
                              bytes[0] == 0xFF ? "UTF-16LE" : "UTF-16BE", nullptr, &text_length,
                              nullptr);
        text = converted;
    } else {
        text = data, text_length = length;
    }
    if (text != nullptr && g_utf8_validate_len(text, text_length, nullptr))
        return g_regex_match_full(term->regex, text, (gssize)text_length, 0, 0, nullptr, nullptr);
    /* Anything else as XP did: its bytes. */
    return term->raw_regex != nullptr &&
           g_regex_match_full(term->raw_regex, data, (gssize)length, 0, 0, nullptr, nullptr);
}

static bool match_type(const node *term, xd_search_item *item) {
    if (term->type_mime != nullptr && item->content_type != nullptr &&
        g_content_type_is_a(item->content_type, term->type_mime))
        return true;
    if (term->type_extension != nullptr) {
        g_autofree char *folded = g_utf8_casefold(display_name(item), -1);
        g_autofree char *suffix = g_strconcat(".", term->type_extension, nullptr);
        g_autofree char *folded_suffix = g_utf8_casefold(suffix, -1);
        if (g_str_has_suffix(folded, folded_suffix))
            return true;
    }
    if (term->type_description != nullptr && item->content_type != nullptr) {
        g_autofree char *description = item->is_dir ? g_strdup("folder")
                                                    : g_content_type_get_description(
                                                          item->content_type);
        g_autofree char *folded = g_utf8_casefold(description != nullptr ? description : "", -1);
        if (strstr(folded, term->type_description) != nullptr)
            return true;
    }
    return false;
}

static bool in_range(const node *term, int64_t value) {
    return value >= term->low && value < term->high;
}

static bool match_node(const node *tree_node, xd_search_item *item) {
    switch (tree_node->type) {
    case NODE_AND:
        for (guint i = 0; i < tree_node->children->len; i++)
            if (!match_node(g_ptr_array_index(tree_node->children, i), item))
                return false;
        return true;
    case NODE_OR:
        for (guint i = 0; i < tree_node->children->len; i++)
            if (match_node(g_ptr_array_index(tree_node->children, i), item))
                return true;
        return false;
    case NODE_NOT:
        return !match_node(g_ptr_array_index(tree_node->children, 0), item);
    case NODE_TERM:
        break;
    }
    switch (tree_node->property) {
    case XD_SEARCH_PROP_ANY:
    case XD_SEARCH_PROP_NAME:
        return g_regex_match(tree_node->regex, display_name(item), G_REGEX_MATCH_DEFAULT,
                             nullptr);
    case XD_SEARCH_PROP_CONTENT:
        return match_contents(tree_node, item);
    case XD_SEARCH_PROP_KIND: {
        xd_search_kind kind = xd_search_kind_of(item->content_type, item->is_dir, item->is_link);
        return (tree_node->kinds & kind) != 0;
    }
    case XD_SEARCH_PROP_TYPE:
        return match_type(tree_node, item);
    case XD_SEARCH_PROP_SIZE:
        /* Folders have no size in Windows: size: leaves them out. */
        return !item->is_dir && item->size <= (uint64_t)INT64_MAX &&
               in_range(tree_node, (int64_t)item->size);
    case XD_SEARCH_PROP_MODIFIED:
        return item->modified != 0 && in_range(tree_node, item->modified);
    case XD_SEARCH_PROP_CREATED:
        return item->created != 0 && in_range(tree_node, item->created);
    case XD_SEARCH_PROP_ACCESSED:
        return item->accessed != 0 && in_range(tree_node, item->accessed);
    case XD_SEARCH_PROP_COUNT:
        break;
    }
    return false;
}

bool xd_search_query_matches(const xd_search_query *query, xd_search_item *item) {
    return match_node(query->root, item);
}

/* ---- describing and highlighting ------------------------------------------------------------ */

static const char *const PROPERTY_NAMES[XD_SEARCH_PROP_COUNT] = {
    "any", "name", "content", "kind", "type", "modified", "created", "accessed", "size"};

static void describe_node(GString *out, const node *tree_node) {
    static const char *const groups[] = {"AND", "OR", "NOT"};
    if (tree_node->type != NODE_TERM) {
        g_string_append_printf(out, "(%s", groups[tree_node->type]);
        for (guint i = 0; i < tree_node->children->len; i++) {
            g_string_append_c(out, ' ');
            describe_node(out, g_ptr_array_index(tree_node->children, i));
        }
        g_string_append_c(out, ')');
        return;
    }
    const char *property = PROPERTY_NAMES[tree_node->property];
    static const char *const modes[] = {"~", "w", "=", "*", "/"};
    switch (tree_node->property) {
    case XD_SEARCH_PROP_ANY:
    case XD_SEARCH_PROP_NAME:
    case XD_SEARCH_PROP_CONTENT:
        g_string_append_printf(out, "%s%s%s%s", property, modes[tree_node->mode],
                               tree_node->caseless ? "i:" : ":", tree_node->value);
        return;
    case XD_SEARCH_PROP_KIND:
        g_string_append_printf(out, "kind:0x%x", (unsigned)tree_node->kinds);
        return;
    case XD_SEARCH_PROP_TYPE:
        g_string_append_printf(out, "type:%s%s%s%s%s%s", tree_node->type_extension != nullptr ? "ext=" : "",
                               tree_node->type_extension != nullptr ? tree_node->type_extension : "",
                               tree_node->type_mime != nullptr ? "mime=" : "",
                               tree_node->type_mime != nullptr ? tree_node->type_mime : "",
                               tree_node->type_description != nullptr ? "|desc=" : "",
                               tree_node->type_description != nullptr ? tree_node->type_description
                                                                      : "");
        return;
    case XD_SEARCH_PROP_MODIFIED:
    case XD_SEARCH_PROP_CREATED:
    case XD_SEARCH_PROP_ACCESSED:
    case XD_SEARCH_PROP_SIZE:
        g_string_append_printf(out, "%s[", property);
        if (tree_node->low == INT64_MIN)
            g_string_append(out, "-inf");
        else
            g_string_append_printf(out, "%" G_GINT64_FORMAT, (gint64)tree_node->low);
        g_string_append_c(out, ',');
        if (tree_node->high == INT64_MAX)
            g_string_append(out, "inf");
        else
            g_string_append_printf(out, "%" G_GINT64_FORMAT, (gint64)tree_node->high);
        g_string_append_c(out, ')');
        return;
    case XD_SEARCH_PROP_COUNT:
        break;
    }
}

char *xd_search_query_describe(const xd_search_query *query) {
    GString *out = g_string_new(nullptr);
    describe_node(out, query->root);
    return g_string_free(out, FALSE);
}

static void collect_spans(const node *tree_node, const char *label, GArray *spans) {
    if (tree_node->type == NODE_NOT)
        return; /* what isn't wanted isn't in the name to show */
    if (tree_node->type != NODE_TERM) {
        for (guint i = 0; i < tree_node->children->len; i++)
            collect_spans(g_ptr_array_index(tree_node->children, i), label, spans);
        return;
    }
    if (tree_node->property != XD_SEARCH_PROP_ANY && tree_node->property != XD_SEARCH_PROP_NAME)
        return;
    GRegex *regex = tree_node->mode == TEXT_WILDCARD ? tree_node->span_regex : tree_node->regex;
    if (regex == nullptr)
        return;
    if (tree_node->mode == TEXT_WILDCARD &&
        !g_regex_match(tree_node->regex, label, G_REGEX_MATCH_DEFAULT, nullptr))
        return; /* the name as a whole doesn't match */
    GMatchInfo *match = nullptr;
    g_regex_match(regex, label, G_REGEX_MATCH_DEFAULT, &match);
    while (g_match_info_matches(match)) {
        gint start = 0, end = 0;
        if (g_match_info_fetch_pos(match, 0, &start, &end) && end > start) {
            xd_search_span span = {(size_t)start, (size_t)end};
            g_array_append_val(spans, span);
        }
        if (!g_match_info_next(match, nullptr))
            break;
    }
    g_match_info_free(match);
}

static gint compare_spans(gconstpointer left, gconstpointer right) {
    const xd_search_span *first = left, *second = right;
    return (first->start > second->start) - (first->start < second->start);
}

GArray *xd_search_query_name_spans(const xd_search_query *query, const char *label) {
    GArray *spans = g_array_new(FALSE, FALSE, sizeof(xd_search_span));
    if (query == nullptr || label == nullptr || !g_utf8_validate(label, -1, nullptr))
        return spans;
    collect_spans(query->root, label, spans);
    g_array_sort(spans, compare_spans);
    /* Overlapping or touching: one. */
    guint kept = 0;
    for (guint i = 0; i < spans->len; i++) {
        xd_search_span span = g_array_index(spans, xd_search_span, i);
        if (kept > 0 && span.start <= g_array_index(spans, xd_search_span, kept - 1).end) {
            xd_search_span *last = &g_array_index(spans, xd_search_span, kept - 1);
            last->end = MAX(last->end, span.end);
            continue;
        }
        g_array_index(spans, xd_search_span, kept++) = span;
    }
    g_array_set_size(spans, kept);
    return spans;
}
