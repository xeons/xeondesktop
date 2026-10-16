/* SPDX-License-Identifier: MIT
 * Unit tests for the core library and theme engine.
 */
#include <stdlib.h>

#include "../src/core/ini.h"
#include "../src/apps/runcmd.h"
#include "../src/settings/settings.h"
#include "../src/shell/apps.h"
#include "../src/ui/textedit.h"
#include "../src/theme/theme.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
        }                                                                      \
    } while (0)

static void test_str(void) {
    CHECK(xd_str_eq(xd_str_trim(XD_STR("  hi \t\n")), XD_STR("hi")));
    CHECK(xd_str_eq_nocase(XD_STR("ActiveTitle"), XD_STR("activetitle")));
    CHECK(xd_str_eq(xd_str_sub(XD_STR("abc"), 1, 99), XD_STR("bc")));
    CHECK(xd_str_eq(xd_str_sub(XD_STR("abc"), 99, 1), XD_STR("")));

    int64_t v = 0;
    CHECK(xd_str_parse_i64(XD_STR(" 255 "), 0, 255, &v) && v == 255);
    CHECK(!xd_str_parse_i64(XD_STR("256"), 0, 255, &v));
    CHECK(!xd_str_parse_i64(XD_STR("12x"), 0, 255, &v));
    CHECK(!xd_str_parse_i64(XD_STR("-"), -10, 10, &v));
    CHECK(xd_str_parse_i64(XD_STR("-9223372036854775808"), INT64_MIN, INT64_MAX, &v) &&
          v == INT64_MIN);
    CHECK(!xd_str_parse_i64(XD_STR("9223372036854775808"), INT64_MIN, INT64_MAX, &v));

    double d = 0;
    CHECK(xd_str_parse_double(XD_STR("1.25"), &d) && d > 1.249 && d < 1.251);
    CHECK(!xd_str_parse_double(XD_STR("1.25x"), &d));

    xd_strbuf sb = {0};
    xd_strbuf_append(&sb, XD_STR("ab"));
    xd_strbuf_appendf(&sb, "%d-%s", 42, "z");
    CHECK(xd_str_eq(xd_strbuf_view(&sb), XD_STR("ab42-z")));
    for (int i = 0; i < 1000; i++)
        xd_strbuf_append_char(&sb, 'x');
    CHECK(sb.len == 1006 && xd_strbuf_cstr(&sb)[1006] == '\0');
    xd_strbuf_free(&sb);
    CHECK(sb.data == nullptr && sb.len == 0);
}

static void test_arena(void) {
    xd_arena a;
    xd_arena_init(&a, 256);
    int *ints = XD_ARENA_ARRAY(&a, int, 10);
    for (int i = 0; i < 10; i++)
        CHECK(ints[i] == 0);
    double *dbl = XD_ARENA_NEW(&a, double);
    CHECK(((uintptr_t)dbl % alignof(double)) == 0);
    /* Larger than a block: must get its own block. */
    char *big = xd_arena_alloc(&a, 10000, 1);
    big[9999] = 'x';
    char *s = xd_str_arena_cstr(&a, XD_STR("hello"));
    CHECK(strcmp(s, "hello") == 0);
    xd_arena_destroy(&a);
    CHECK(a.head == nullptr);
}

typedef struct ini_capture {
    int count;
    bool saw_value;
} ini_capture;

static bool ini_cb(void *user, xd_str section, xd_str key, xd_str value) {
    ini_capture *c = user;
    c->count++;
    if (xd_str_eq(section, XD_STR("A")) && xd_str_eq(key, XD_STR("k")) &&
        xd_str_eq(value, XD_STR("v = w")))
        c->saw_value = true;
    return true;
}

static void test_ini(void) {
    ini_capture c = {0};
    xd_ini_error err;
    CHECK(xd_ini_parse(XD_STR("\xEF\xBB\xBF; comment\n[A]\n k = v = w \r\n# x\nz=\n"), ini_cb,
                       &c, &err));
    CHECK(c.count == 2 && c.saw_value);

    CHECK(!xd_ini_parse(XD_STR("[A\n"), ini_cb, &c, &err));
    CHECK(err.line == 1);
    CHECK(!xd_ini_parse(XD_STR("[A]\n\nnoequals\n"), ini_cb, &c, &err));
    CHECK(err.line == 3);
    CHECK(!xd_ini_parse(XD_STR("[]\n"), ini_cb, &c, &err));
    CHECK(xd_ini_parse(XD_STR(""), ini_cb, &c, &err));
}

static void test_scheme(void) {
    xd_scheme s;
    xd_strbuf err = {0};

    CHECK(xd_scheme_parse(XD_STR("[Scheme]\nName=Test\n[Colors]\nActiveTitle=1 2 3\n"
                                 "Background = 4,5,6\n[Metrics]\nCaptionHeight=25\n"
                                 "[Fonts]\nCaptionFont=Arial, 10, bold, italic\n"),
                          &s, &err));
    CHECK(strcmp(s.name, "Test") == 0);
    CHECK(s.colors[XD_COLOR_ACTIVE_TITLE].r == 1 && s.colors[XD_COLOR_ACTIVE_TITLE].b == 3);
    CHECK(s.colors[XD_COLOR_BACKGROUND].g == 5);
    CHECK(s.metrics[XD_METRIC_CAPTION_HEIGHT] == 25);
    CHECK(strcmp(s.fonts[XD_FONT_CAPTION].family, "Arial") == 0);
    CHECK(s.fonts[XD_FONT_CAPTION].bold && s.fonts[XD_FONT_CAPTION].italic);
    /* Unlisted colours keep the Windows Standard defaults. */
    CHECK(s.colors[XD_COLOR_BUTTON_FACE].r == 212);

    static const char *const bad[] = {
        "[Colors]\nActiveTitle=1 2\n",       "[Colors]\nActiveTitle=1 2 3 4\n",
        "[Colors]\nActiveTitle=256 0 0\n",   "[Colors]\nNoSuchColor=1 2 3\n",
        "[Metrics]\nCaptionHeight=-1\n",     "[Fonts]\nCaptionFont=Arial\n",
        "[Fonts]\nCaptionFont=Arial, 10, x\n", "[Bogus]\nk=v\n",
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(bad); i++) {
        xd_strbuf_clear(&err);
        CHECK(!xd_scheme_parse(xd_str_from_cstr(bad[i]), &s, &err));
        CHECK(err.len > 0);
        /* A failed parse leaves the defaults in place. */
        CHECK(strcmp(s.name, "Windows Standard") == 0);
    }
    xd_strbuf_free(&err);
}

static void test_shipped_schemes(void) {
    static const char *const files[] = {
        "windows-standard.ini", "windows-classic.ini", "high-contrast-black.ini",
        "high-contrast-white.ini",
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(files); i++) {
        xd_strbuf path = {0};
        xd_strbuf_appendf(&path, "%s/data/schemes/%s", XD_SOURCE_ROOT, files[i]);
        xd_strbuf err = {0};
        xd_scheme s;
        bool ok = xd_scheme_load(xd_strbuf_cstr(&path), &s, &err);
        if (!ok)
            fprintf(stderr, "%s: %s\n", files[i], xd_strbuf_cstr(&err));
        CHECK(ok);
        xd_strbuf_free(&err);
        xd_strbuf_free(&path);
    }
}

static uint32_t pixel(cairo_surface_t *s, int x, int y) {
    cairo_surface_flush(s);
    int stride = cairo_image_surface_get_stride(s);
    const unsigned char *data = cairo_image_surface_get_data(s);
    uint32_t px;
    memcpy(&px, data + (size_t)y * (size_t)stride + (size_t)x * 4, sizeof(px));
    return px & 0xFFFFFF;
}

static bool near(uint32_t got, uint32_t want, int tol) {
    for (int shift = 0; shift <= 16; shift += 8) {
        int a = (int)((got >> shift) & 0xFF);
        int b = (int)((want >> shift) & 0xFF);
        if (a - b > tol || b - a > tol)
            return false;
    }
    return true;
}

static void test_render(void) {
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    static const double scales[] = {1.0, 1.25, 1.5, 2.0, 3.0};

    for (size_t si = 0; si < XD_STYLE_COUNT; si++) {
        for (size_t k = 0; k < XD_ARRAY_LEN(scales); k++) {
            xd_theme t;
            xd_theme_init(&t, (xd_visual_style)si, &scheme, scales[k]);
            int w = xd_tpx(&t, 300);
            int h = xd_tpx(&t, 200);
            cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            cairo_t *cr = cairo_create(surf);
            xd_frame f = {.title = "Test", .active = true, .buttons = 0x0F};
            xd_theme_draw_frame(&t, cr, (xd_rect){0, 0, w, h}, &f);
            xd_theme_draw_button(&t, cr, (xd_rect){10, 100, 80, 24}, XD_STATE_FOCUSED, "OK");
            xd_theme_draw_checkbox(&t, cr, (xd_rect){10, 130, 200, 20}, XD_STATE_CHECKED, "x");
            xd_theme_draw_taskbar(&t, cr, (xd_rect){0, h - 30, w, 30});
            CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

            /* Frame insets grow with the scale factor. */
            xd_insets in = xd_theme_frame_insets(&t, &f);
            CHECK(in.top >= (int)(18 * scales[k]));
            cairo_destroy(cr);
            cairo_surface_destroy(surf);
        }
    }

    /* Classic at 1x: the outer frame pixel is ButtonLight, the next one
     * in is ButtonHilight (white), matching Win2000's EDGE_RAISED. */
    xd_theme t;
    xd_theme_init(&t, XD_STYLE_CLASSIC, &scheme, 1.0);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
    cairo_t *cr = cairo_create(surf);
    xd_frame f = {.title = "", .active = true, .buttons = 0};
    xd_theme_draw_frame(&t, cr, (xd_rect){0, 0, 100, 100}, &f);
    CHECK(pixel(surf, 50, 0) == 0xD4D0C8);
    CHECK(pixel(surf, 50, 1) == 0xFFFFFF);
    CHECK(pixel(surf, 50, 99) == 0x404040);
    CHECK(pixel(surf, 50, 98) == 0x808080);
    /* Caption starts at 4px and is ActiveTitle on the left. */
    CHECK(near(pixel(surf, 4, 10), 0x0A246A, 3));
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

static void test_hit(void) {
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    static const double scales[] = {1.0, 1.25, 2.0};
    for (size_t si = 0; si < XD_STYLE_COUNT; si++) {
        for (size_t k = 0; k < XD_ARRAY_LEN(scales); k++) {
            xd_theme t;
            xd_theme_init(&t, (xd_visual_style)si, &scheme, scales[k]);
            xd_rect outer = {10, 20, xd_tpx(&t, 300), xd_tpx(&t, 200)};
            xd_frame f = {.title = "x", .active = true, .buttons = 0x07};
            xd_insets in = xd_theme_frame_insets(&t, &f);

            CHECK(xd_theme_frame_hit(&t, outer, &f, 0, 0) == XD_PART_NONE);
            CHECK(xd_theme_frame_hit(&t, outer, &f, outer.x + outer.w / 2,
                                     outer.y + outer.h / 2) == XD_PART_CLIENT);
            CHECK(xd_theme_frame_hit(&t, outer, &f, outer.x, outer.y) == XD_PART_TOP_LEFT);
            CHECK(xd_theme_frame_hit(&t, outer, &f, outer.x + outer.w - 1,
                                     outer.y + outer.h - 1) == XD_PART_BOTTOM_RIGHT);
            CHECK(xd_theme_frame_hit(&t, outer, &f, outer.x + outer.w / 2,
                                     outer.y + outer.h - 1) == XD_PART_BOTTOM);
            CHECK(xd_theme_frame_hit(&t, outer, &f, outer.x, outer.y + outer.h / 2) ==
                  XD_PART_LEFT);
            CHECK(xd_theme_frame_hit(&t, outer, &f, outer.x + outer.w / 2,
                                     outer.y + in.top / 2 + xd_tpx(&t, 2)) == XD_PART_CAPTION);

            /* Every laid-out button hit-tests to itself at its centre. */
            xd_frame_layout l;
            xd_theme_frame_layout(&t, outer, &f, &l);
            CHECK(l.button_count == 3);
            for (size_t i = 0; i < l.button_count; i++) {
                xd_rect b = l.button_rects[i];
                CHECK(xd_theme_frame_hit(&t, outer, &f, b.x + b.w / 2, b.y + b.h / 2) ==
                      l.button_parts[i]);
                /* Buttons sit inside the caption band. */
                CHECK(b.y >= outer.y && b.y + b.h <= outer.y + in.top);
            }

            /* Maximized frames have no resize borders. */
            f.maximized = true;
            CHECK(!xd_frame_part_is_border(xd_theme_frame_hit(&t, outer, &f, outer.x, outer.y)));
        }
    }
}

static void test_apps(void) {
    xd_arena arena;
    xd_arena_init(&arena, 4096);
    setenv("LANG", "de_DE.UTF-8", 1);
    unsetenv("LC_ALL");
    unsetenv("LC_MESSAGES");
    xd_app app;

    CHECK(xd_app_parse(XD_STR("[Desktop Entry]\nType=Application\nName=Editor\n"
                              "Name[de]=Bearbeiter\nName[fr]=Editeur\nIcon=ed\n"
                              "Exec=ed --open %U\nCategories=Utility;TextEditor;\n"
                              "[Desktop Action new]\nName=Should not win\nExec=nope\n"),
                       XD_STR("ed.desktop"), "XeonDesktop", &arena, &app));
    CHECK(strcmp(app.name, "Bearbeiter") == 0);
    CHECK(strcmp(app.exec, "ed --open") == 0);
    CHECK(strcmp(app.folder, "Accessories") == 0);
    CHECK(strcmp(app.icon, "ed") == 0);

    /* Settings wins over System when both are listed. */
    CHECK(xd_app_parse(XD_STR("[Desktop Entry]\nType=Application\nName=Mouse\nExec=m\n"
                              "Categories=System;Settings;\n"),
                       XD_STR("m.desktop"), "XeonDesktop", &arena, &app));
    CHECK(strcmp(app.folder, "Settings") == 0);

    /* Entries that must not be shown. */
    static const char *const hidden[] = {
        "[Desktop Entry]\nType=Application\nName=A\nExec=a\nNoDisplay=true\n",
        "[Desktop Entry]\nType=Application\nName=A\nExec=a\nHidden=true\n",
        "[Desktop Entry]\nType=Application\nName=A\nExec=a\nOnlyShowIn=KDE;LXQt;\n",
        "[Desktop Entry]\nType=Application\nName=A\nExec=a\nNotShowIn=XeonDesktop;\n",
        "[Desktop Entry]\nType=Link\nName=A\nURL=http://x\n",
        "[Desktop Entry]\nType=Application\nName=A\n",
        "[Desktop Entry]\nType=Application\nName=A\nExec=a\nTryExec=/nonexistent/xd\n",
        "not an ini file at all",
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(hidden); i++)
        CHECK(!xd_app_parse(xd_str_from_cstr(hidden[i]), XD_STR("x.desktop"), "XeonDesktop",
                            &arena, &app));
    CHECK(xd_app_parse(XD_STR("[Desktop Entry]\nType=Application\nName=A\nExec=a\n"
                              "OnlyShowIn=XeonDesktop;\n"),
                       XD_STR("a.desktop"), "XeonDesktop", &arena, &app));
    CHECK(strcmp(app.folder, "Other") == 0);

    /* Field codes. */
    char *e = xd_app_expand_exec(&arena, XD_STR("app %f %F %u %U --pct=100%% %c %i"), "It's",
                                 "ico");
    CHECK(strcmp(e, "app     --pct=100% 'It'\\''s' --icon 'ico'") == 0);
    e = xd_app_expand_exec(&arena, XD_STR("trailing %"), "n", nullptr);
    CHECK(strcmp(e, "trailing %") == 0);
    /* An icon name containing a quote is dropped rather than quoted. */
    e = xd_app_expand_exec(&arena, XD_STR("a %i"), "n", "bad'icon");
    CHECK(strcmp(e, "a") == 0);
    xd_arena_destroy(&arena);
}

static void test_menus(void) {
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    for (size_t si = 0; si < XD_STYLE_COUNT; si++) {
        xd_theme t;
        xd_theme_init(&t, (xd_visual_style)si, &scheme, 1.5);
        xd_menu_metrics m = xd_theme_menu_metrics(&t);
        CHECK(m.large_icon == 48 && m.icon == 24 && m.item_h > m.icon);
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 300);
        cairo_t *cr = cairo_create(surf);
        xd_theme_draw_menu_frame(&t, cr, (xd_rect){0, 0, 300, 300});
        xd_theme_draw_start_banner(&t, cr, (xd_rect){4, 4, m.banner_w, 290}, "Xeon", "Pro");
        xd_theme_draw_menu_item(&t, cr, (xd_rect){30, 4, 260, m.item_h}, XD_STATE_HOT, "A",
                                true, false);
        xd_theme_draw_menu_item(&t, cr, (xd_rect){30, 40, 260, m.item_h}, XD_STATE_DISABLED,
                                "B", false, true);
        xd_theme_draw_menu_separator(&t, cr, (xd_rect){30, 90, 260, m.separator_h});
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
    }
}

static bool text_is(const xd_textedit *e, const char *want) {
    return xd_str_eq(xd_textedit_view(e), xd_str_from_cstr(want));
}

static void test_textedit(void) {
    /* UTF-8 validation */
    CHECK(xd_utf8_sequence_len((const unsigned char *)"a", 1) == 1);
    CHECK(xd_utf8_sequence_len((const unsigned char *)"\xC3\xA9", 2) == 2);   /* é */
    CHECK(xd_utf8_sequence_len((const unsigned char *)"\xE2\x82\xAC", 3) == 3); /* € */
    CHECK(xd_utf8_sequence_len((const unsigned char *)"\xF0\x9F\x98\x80", 4) == 4);
    CHECK(xd_utf8_sequence_len((const unsigned char *)"\xC0\xAF", 2) == 0);    /* overlong */
    CHECK(xd_utf8_sequence_len((const unsigned char *)"\xED\xA0\x80", 3) == 0); /* surrogate */
    CHECK(xd_utf8_sequence_len((const unsigned char *)"\xC3", 1) == 0);        /* truncated */
    CHECK(xd_utf8_sequence_len((const unsigned char *)"\x80", 1) == 0);

    xd_textedit e = {0};
    xd_textedit_set(&e, XD_STR("hello"));
    CHECK(text_is(&e, "hello") && xd_textedit_has_selection(&e));
    xd_textedit_insert(&e, XD_STR("x")); /* typing replaces the selection */
    CHECK(text_is(&e, "x") && e.cursor == 1);

    /* Control characters and invalid bytes are dropped. */
    xd_textedit_insert(&e, XD_STR("a\tb\n\x01\xFF\xC3\xA9"));
    CHECK(text_is(&e, "xab\xC3\xA9"));
    /* Backspace removes a whole character, not a byte. */
    xd_textedit_backspace(&e, false);
    CHECK(text_is(&e, "xab"));

    /* Cursor never lands inside a character. */
    xd_textedit_set(&e, XD_STR("\xE2\x82\xAC" "1"));
    xd_textedit_set_cursor(&e, 2, false);
    CHECK(e.cursor == 0);
    xd_textedit_move(&e, XD_MOTION_RIGHT, false);
    CHECK(e.cursor == 3);
    xd_textedit_delete(&e, false);
    CHECK(text_is(&e, "\xE2\x82\xAC"));

    /* Selection, word motion, word delete. */
    xd_textedit_set(&e, XD_STR("ls -la /tmp"));
    xd_textedit_move(&e, XD_MOTION_END, false);
    xd_textedit_move(&e, XD_MOTION_WORD_LEFT, true);
    CHECK(xd_str_eq(xd_textedit_selected(&e), XD_STR("tmp")));
    xd_textedit_move(&e, XD_MOTION_LEFT, false); /* collapses to the left edge */
    CHECK(e.cursor == 8 && !xd_textedit_has_selection(&e));
    xd_textedit_move(&e, XD_MOTION_HOME, false);
    xd_textedit_move(&e, XD_MOTION_WORD_RIGHT, false);
    CHECK(e.cursor == 4);
    xd_textedit_move(&e, XD_MOTION_END, false);
    xd_textedit_backspace(&e, true);
    CHECK(text_is(&e, "ls -la /"));
    xd_textedit_move(&e, XD_MOTION_HOME, false);
    xd_textedit_delete(&e, true);
    CHECK(text_is(&e, "la /"));

    /* Insertion in the middle; max length cut on a character boundary. */
    xd_textedit_set(&e, XD_STR("ac"));
    xd_textedit_set_cursor(&e, 1, false);
    xd_textedit_insert(&e, XD_STR("b"));
    CHECK(text_is(&e, "abc") && e.cursor == 2);
    e.max_len = 5;
    xd_textedit_move(&e, XD_MOTION_END, false);
    xd_textedit_insert(&e, XD_STR("\xC3\xA9\xC3\xA9")); /* 4 bytes, room for 2 */
    CHECK(text_is(&e, "abc\xC3\xA9"));
    xd_textedit_free(&e);
}

static void test_runcmd(void) {
    const char *path = "/usr/bin:/bin";
    xd_run_target t = xd_run_classify(XD_STR("   "), path, "/home/u");
    CHECK(t.kind == XD_RUN_EMPTY);
    xd_run_target_free(&t);

    t = xd_run_classify(XD_STR("https://example.com/x"), path, "/home/u");
    CHECK(t.kind == XD_RUN_OPEN);
    xd_run_target_free(&t);
    t = xd_run_classify(XD_STR("www.example.com"), path, "/home/u");
    CHECK(t.kind == XD_RUN_OPEN);
    xd_run_target_free(&t);
    t = xd_run_classify(XD_STR("mailto:a@b.c"), path, "/home/u");
    CHECK(t.kind == XD_RUN_OPEN);
    xd_run_target_free(&t);

    t = xd_run_classify(XD_STR("/tmp"), path, "/home/u");
    CHECK(t.kind == XD_RUN_OPEN && strcmp(xd_strbuf_cstr(&t.target), "/tmp") == 0);
    xd_run_target_free(&t);
    t = xd_run_classify(XD_STR("~"), path, "/tmp");
    CHECK(t.kind == XD_RUN_OPEN && strcmp(xd_strbuf_cstr(&t.target), "/tmp") == 0);
    xd_run_target_free(&t);
    t = xd_run_classify(XD_STR("/bin/sh"), path, "/home/u");
    CHECK(t.kind == XD_RUN_EXEC);
    xd_run_target_free(&t);

    t = xd_run_classify(XD_STR("sh -c 'echo hi'"), path, "/home/u");
    CHECK(t.kind == XD_RUN_SHELL &&
          strcmp(xd_strbuf_cstr(&t.target), "sh -c 'echo hi'") == 0);
    xd_run_target_free(&t);
    t = xd_run_classify(XD_STR("/bin/sh -c true"), path, "/home/u");
    CHECK(t.kind == XD_RUN_SHELL);
    xd_run_target_free(&t);

    t = xd_run_classify(XD_STR("no-such-program-xd --x"), path, "/home/u");
    CHECK(t.kind == XD_RUN_NOT_FOUND &&
          strcmp(xd_strbuf_cstr(&t.target), "no-such-program-xd") == 0);
    xd_run_target_free(&t);
    /* "sh" isn't found in an empty PATH: nothing runs from the cwd. */
    t = xd_run_classify(XD_STR("sh"), "", "/home/u");
    CHECK(t.kind == XD_RUN_NOT_FOUND);
    xd_run_target_free(&t);
}

static void test_settings(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    CHECK(a.style == XD_STYLE_CLASSIC && a.wallpaper_mode == XD_WALLPAPER_FILL);

    xd_settings_parse(XD_STR("[Appearance]\nStyle=luna\nScheme=windows-classic\n"
                             "FontSize=large\nFontSmoothing=cleartype\nFuture=ignored\n"
                             "[Desktop]\nWallpaper=/w/p.jpg\nWallpaperMode=tile\n"
                             "[Effects]\nLargeIcons=true\n[ScreenSaver]\nWait=7\n"
                             "[SomethingNew]\nKey=v\n"),
                      &a);
    CHECK(a.style == XD_STYLE_LUNA && strcmp(a.scheme, "windows-classic") == 0);
    CHECK(a.font_size == XD_FONT_LARGE && a.font_smoothing == XD_SMOOTH_CLEARTYPE);
    CHECK(strcmp(a.wallpaper, "/w/p.jpg") == 0 && a.wallpaper_mode == XD_WALLPAPER_TILE);
    CHECK(a.large_icons && a.screensaver_wait == 7);

    /* Serialize -> parse round trip. */
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b));
    b.large_icons = false;
    CHECK(!xd_settings_equal(&a, &b));
    xd_strbuf_free(&text);

    /* Bad values keep defaults; a scheme can't be a path. */
    xd_settings_parse(XD_STR("[Appearance]\nStyle=bogus\nScheme=../../etc/x\n"
                             "[Desktop]\nWallpaper=relative.jpg\nWallpaperMode=x\n"
                             "[ScreenSaver]\nWait=0\n"),
                      &a);
    CHECK(a.style == XD_STYLE_CLASSIC && a.scheme[0] == '\0' && a.wallpaper[0] == '\0');
    CHECK(a.wallpaper_mode == XD_WALLPAPER_FILL && a.screensaver_wait == 15);
    /* A syntax error leaves everything at defaults. */
    xd_settings_parse(XD_STR("[Appearance]\nStyle=luna\nbroken line\n"), &a);
    CHECK(a.style == XD_STYLE_CLASSIC);

    /* Font size scales fonts and caption metrics. */
    xd_settings_defaults(&a);
    xd_scheme normal;
    xd_settings_scheme(&a, &normal);
    a.font_size = XD_FONT_EXTRA_LARGE;
    xd_scheme xl;
    xd_settings_scheme(&a, &xl);
    CHECK(xl.fonts[XD_FONT_CAPTION].size_pt > normal.fonts[XD_FONT_CAPTION].size_pt * 1.4);
    CHECK(xl.metrics[XD_METRIC_CAPTION_HEIGHT] == 27);
}

static void test_px(void) {
    CHECK(xd_px(1.0, 1) == 1);
    CHECK(xd_px(1.25, 1) == 1);
    CHECK(xd_px(1.5, 1) == 2);
    CHECK(xd_px(0.5, 1) == 1); /* never vanishes */
    CHECK(xd_px(2.0, 18) == 36);
    CHECK(xd_px(1.25, 18) == 23);
    CHECK(xd_px(2.0, 0) == 0);
}

int main(void) {
    test_str();
    test_arena();
    test_ini();
    test_scheme();
    test_shipped_schemes();
    test_px();
    test_render();
    test_hit();
    test_apps();
    test_menus();
    test_textedit();
    test_runcmd();
    test_settings();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
