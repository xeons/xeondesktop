/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runcmd.h"

static bool looks_like_url(xd_str s) {
    size_t colon;
    if (xd_str_find_char(s, ':', &colon) && colon > 0) {
        /* scheme: letters, digits, + - . and starting with a letter */
        bool scheme = (s.ptr[0] >= 'a' && s.ptr[0] <= 'z') || (s.ptr[0] >= 'A' && s.ptr[0] <= 'Z');
        for (size_t i = 1; i < colon && scheme; i++) {
            char c = s.ptr[i];
            scheme = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '+' || c == '-' || c == '.';
        }
        xd_str rest = xd_str_sub(s, colon, s.len);
        if (scheme && (xd_str_starts_with(rest, XD_STR("://")) ||
                       xd_str_eq_nocase(xd_str_sub(s, 0, colon), XD_STR("mailto"))))
            return true;
    }
    return xd_str_starts_with(s, XD_STR("www."));
}

static bool is_path_like(xd_str s) {
    return xd_str_starts_with(s, XD_STR("/")) || xd_str_starts_with(s, XD_STR("~")) ||
           xd_str_starts_with(s, XD_STR("./")) || xd_str_starts_with(s, XD_STR("../"));
}

/* Is `name` an executable file somewhere in path_env? */
static bool in_path(xd_str name, const char *path_env) {
    xd_str rest = xd_str_from_cstr(path_env);
    xd_strbuf buf = {0};
    bool found = false;
    while (rest.len > 0 && !found) {
        size_t colon = rest.len;
        bool more = xd_str_find_char(rest, ':', &colon);
        xd_str dir = xd_str_sub(rest, 0, colon);
        if (dir.len > 0) {
            xd_strbuf_clear(&buf);
            xd_strbuf_append(&buf, dir);
            xd_strbuf_append_char(&buf, '/');
            xd_strbuf_append(&buf, name);
            struct stat st;
            found = stat(xd_strbuf_cstr(&buf), &st) == 0 && S_ISREG(st.st_mode) &&
                    access(xd_strbuf_cstr(&buf), X_OK) == 0;
        }
        rest = more ? xd_str_sub(rest, colon + 1, rest.len) : XD_STR("");
    }
    xd_strbuf_free(&buf);
    return found;
}

xd_run_target xd_run_classify(xd_str input, const char *path_env, const char *home) {
    xd_run_target t = {.kind = XD_RUN_EMPTY};
    xd_str s = xd_str_trim(input);
    if (s.len == 0)
        return t;
    if (path_env == nullptr)
        path_env = getenv("PATH");
    if (path_env == nullptr)
        path_env = "/usr/local/bin:/usr/bin:/bin";

    if (looks_like_url(s)) {
        t.kind = XD_RUN_OPEN;
        xd_strbuf_append(&t.target, s);
        return t;
    }

    if (is_path_like(s)) {
        if (s.ptr[0] == '~') {
            xd_strbuf_append(&t.target, xd_str_from_cstr(home != nullptr ? home : ""));
            xd_strbuf_append(&t.target, xd_str_sub(s, 1, s.len));
        } else {
            xd_strbuf_append(&t.target, s);
        }
        struct stat st;
        const char *p = xd_strbuf_cstr(&t.target);
        if (stat(p, &st) == 0) {
            bool exec = S_ISREG(st.st_mode) && access(p, X_OK) == 0;
            t.kind = exec ? XD_RUN_EXEC : XD_RUN_OPEN;
            return t;
        }
        /* A path with arguments ("/usr/bin/foo --bar") falls through to
         * the command-line check below. */
        xd_strbuf_clear(&t.target);
    }

    /* Command line: the first word must be a program. */
    size_t sp = s.len;
    for (size_t i = 0; i < s.len; i++) {
        if (s.ptr[i] == ' ' || s.ptr[i] == '\t') {
            sp = i;
            break;
        }
    }
    xd_str word = xd_str_sub(s, 0, sp);
    bool found;
    if (xd_str_find_char(word, '/', &(size_t){0})) {
        xd_strbuf p = {0};
        xd_strbuf_append(&p, word);
        found = access(xd_strbuf_cstr(&p), X_OK) == 0;
        xd_strbuf_free(&p);
    } else {
        found = in_path(word, path_env);
    }
    if (found) {
        t.kind = XD_RUN_SHELL;
        xd_strbuf_append(&t.target, s);
    } else {
        t.kind = XD_RUN_NOT_FOUND;
        xd_strbuf_append(&t.target, word);
    }
    return t;
}

void xd_run_target_free(xd_run_target *t) { xd_strbuf_free(&t->target); }
