/* SPDX-License-Identifier: MIT */
#include "ini.h"

static bool fail(xd_ini_error *err, size_t line, const char *message) {
    if (err != nullptr) {
        err->line = line;
        err->message = message;
    }
    return false;
}

bool xd_ini_parse(xd_str text, xd_ini_handler handler, void *user, xd_ini_error *err) {
    if (err != nullptr)
        *err = (xd_ini_error){0};

    /* Skip a UTF-8 BOM. */
    if (xd_str_starts_with(text, XD_STR("\xEF\xBB\xBF")))
        text = xd_str_sub(text, 3, text.len);

    xd_str section = {.ptr = "", .len = 0};
    size_t line_no = 0;
    size_t pos = 0;

    while (pos < text.len) {
        line_no++;
        xd_str rest = xd_str_sub(text, pos, text.len - pos);
        size_t nl;
        size_t line_len = xd_str_find_char(rest, '\n', &nl) ? nl : rest.len;
        xd_str line = xd_str_trim(xd_str_sub(rest, 0, line_len));
        pos += line_len + 1;

        if (line.len == 0 || line.ptr[0] == ';' || line.ptr[0] == '#')
            continue;

        if (line.ptr[0] == '[') {
            if (line.ptr[line.len - 1] != ']')
                return fail(err, line_no, "unterminated section header");
            section = xd_str_trim(xd_str_sub(line, 1, line.len - 2));
            if (section.len == 0)
                return fail(err, line_no, "empty section name");
            continue;
        }

        size_t eq;
        if (!xd_str_find_char(line, '=', &eq))
            return fail(err, line_no, "expected key=value");
        xd_str key = xd_str_trim(xd_str_sub(line, 0, eq));
        xd_str value = xd_str_trim(xd_str_sub(line, eq + 1, line.len));
        if (key.len == 0)
            return fail(err, line_no, "empty key");

        if (!handler(user, section, key, value))
            return fail(err, line_no, "rejected by handler");
    }
    return true;
}
