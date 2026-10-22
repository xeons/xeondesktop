/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gdesktopappinfo.h>

#include "runcmd.h"

static bool looks_like_url(xd_str text) {
    size_t colon;
    if (xd_str_find_char(text, ':', &colon) && colon > 0) {
        /* scheme: letters, digits, + - . and starting with a letter */
        bool scheme = (text.ptr[0] >= 'a' && text.ptr[0] <= 'z') ||
                      (text.ptr[0] >= 'A' && text.ptr[0] <= 'Z');
        for (size_t i = 1; i < colon && scheme; i++) {
            char character = text.ptr[i];
            scheme = (character >= 'a' && character <= 'z') ||
                     (character >= 'A' && character <= 'Z') ||
                     (character >= '0' && character <= '9') || character == '+' ||
                     character == '-' || character == '.';
        }
        xd_str rest = xd_str_sub(text, colon, text.len);
        if (scheme && (xd_str_starts_with(rest, XD_STR("://")) ||
                       xd_str_eq_nocase(xd_str_sub(text, 0, colon), XD_STR("mailto"))))
            return true;
    }
    return xd_str_starts_with(text, XD_STR("www."));
}

static bool is_path_like(xd_str text) {
    return xd_str_starts_with(text, XD_STR("/")) || xd_str_starts_with(text, XD_STR("~")) ||
           xd_str_starts_with(text, XD_STR("./")) || xd_str_starts_with(text, XD_STR("../"));
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
    xd_run_target run_target = {.kind = XD_RUN_EMPTY};
    xd_str trimmed = xd_str_trim(input);
    if (trimmed.len == 0)
        return run_target;
    if (path_env == nullptr)
        path_env = getenv("PATH");
    if (path_env == nullptr)
        path_env = "/usr/local/bin:/usr/bin:/bin";

    if (looks_like_url(trimmed)) {
        run_target.kind = XD_RUN_OPEN;
        xd_strbuf_append(&run_target.target, trimmed);
        return run_target;
    }

    if (is_path_like(trimmed)) {
        if (trimmed.ptr[0] == '~') {
            xd_strbuf_append(&run_target.target, xd_str_from_cstr(home != nullptr ? home : ""));
            xd_strbuf_append(&run_target.target, xd_str_sub(trimmed, 1, trimmed.len));
        } else {
            xd_strbuf_append(&run_target.target, trimmed);
        }
        struct stat st;
        const char *target_path = xd_strbuf_cstr(&run_target.target);
        if (stat(target_path, &st) == 0) {
            bool exec = S_ISREG(st.st_mode) && access(target_path, X_OK) == 0;
            run_target.kind = exec ? XD_RUN_EXEC : XD_RUN_OPEN;
            return run_target;
        }
        /* A path with arguments ("/usr/bin/foo --bar") falls through to
         * the command-line check below. */
        xd_strbuf_clear(&run_target.target);
    }

    /* Command line: the first word must be a program. */
    size_t sp = trimmed.len;
    for (size_t i = 0; i < trimmed.len; i++) {
        if (trimmed.ptr[i] == ' ' || trimmed.ptr[i] == '\t') {
            sp = i;
            break;
        }
    }
    xd_str word = xd_str_sub(trimmed, 0, sp);
    bool found;
    if (xd_str_find_char(word, '/', &(size_t){0})) {
        xd_strbuf program_path = {0};
        xd_strbuf_append(&program_path, word);
        found = access(xd_strbuf_cstr(&program_path), X_OK) == 0;
        xd_strbuf_free(&program_path);
    } else {
        found = in_path(word, path_env);
    }
    if (found) {
        run_target.kind = XD_RUN_SHELL;
        xd_strbuf_append(&run_target.target, trimmed);
    } else {
        run_target.kind = XD_RUN_NOT_FOUND;
        xd_strbuf_append(&run_target.target, word);
    }
    return run_target;
}

void xd_run_target_free(xd_run_target *run_target) { xd_strbuf_free(&run_target->target); }

char *xd_run_app_path(xd_str input) {
    xd_str trimmed = xd_str_trim(input);
    if (trimmed.len == 0 || xd_str_find_char(trimmed, ' ', &(size_t){0}) ||
        xd_str_find_char(trimmed, '/', &(size_t){0}))
        return nullptr;
    char *name = g_strndup(trimmed.ptr, trimmed.len);
    if (trimmed.len > 4 && g_ascii_strcasecmp(name + trimmed.len - 4, ".exe") == 0)
        name[trimmed.len - 4] = '\0';
    char *found = nullptr;
    GList *all = g_app_info_get_all(); /* NoDisplay ones too: the tools are */
    for (GList *list = all; list != nullptr && found == nullptr; list = list->next) {
        if (!G_IS_DESKTOP_APP_INFO(list->data))
            continue;
        char **names =
            g_desktop_app_info_get_string_list(list->data, "X-XeonDesktop-AppPath", nullptr);
        for (size_t i = 0; names != nullptr && names[i] != nullptr && found == nullptr; i++) {
            if (g_ascii_strcasecmp(names[i], name) == 0)
                found = g_strdup(g_app_info_get_id(list->data));
        }
        g_strfreev(names);
    }
    g_list_free_full(all, g_object_unref);
    g_free(name);
    return found;
}
