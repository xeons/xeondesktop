/* SPDX-License-Identifier: MIT
 * Unit tests for the core library and theme engine.
 */
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <unistd.h>

#include <crypt.h>
#include <poll.h>

#include <fontconfig/fontconfig.h>

#include "../src/admin/admin.h"
#include "../src/appwiz/programs.h"
#include "../src/apps/runcmd.h"
#include "../src/printers/printlib.h"
#include "../src/scanners/scanlib.h"
#include "../src/bluetooth/btinfo.h"
#include "../src/joy/joyinfo.h"
#include "../src/netconn/netconn.h"
#include "../src/intl/intl.h"
#include "../src/keyboard/layouts.h"
#include "../src/mouse/cursors.h"
#include "../src/files/names.h"
#include "../src/folders/filetypes.h"
#include "../src/fonts/fontlist.h"
#include "../src/schedtasks/task.h"
#include "../src/security/passwd.h"
#include "../src/settings/settings.h"
#include "../src/settings/sounds.h"
#include "../src/shell/apps.h"
#include "../src/ui/textedit.h"
#include "../src/users/users.h"
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
            xd_theme_draw_taskbar(&t, cr, (xd_rect){0, h - 30, w, 30}, XD_EDGE_BOTTOM);
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

/* Writes a .desktop file into a fresh temp dir and loads it via GIO. */
static bool load_entry(const char *dir, const char *name, const char *text, xd_arena *arena,
                       xd_app *out) {
    xd_strbuf path = {0};
    xd_strbuf_appendf(&path, "%s/%s", dir, name);
    FILE *f = fopen(xd_strbuf_cstr(&path), "w");
    bool ok = f != nullptr && fputs(text, f) >= 0;
    if (f != nullptr)
        fclose(f);
    ok = ok && xd_app_from_file(xd_strbuf_cstr(&path), arena, out);
    xd_strbuf_free(&path);
    return ok;
}

static void test_apps(void) {
    char tmpl[] = "/tmp/xd-test-apps-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (dir == nullptr)
        return;
    xd_arena arena;
    xd_arena_init(&arena, 4096);
    setenv("LANGUAGE", "de", 1);
    setenv("XDG_CURRENT_DESKTOP", "XeonDesktop", 1);
    xd_app app;

    CHECK(load_entry(dir, "ed.desktop",
                     "[Desktop Entry]\nType=Application\nName=Editor\nName[de]=Bearbeiter\n"
                     "Icon=ed\nExec=true %U\nCategories=Utility;TextEditor;\n"
                     "Keywords=text;notes;\n",
                     &arena, &app));
    CHECK(strcmp(app.name, "Bearbeiter") == 0); /* localized by GIO */
    CHECK(strcmp(app.folder, "Accessories") == 0 && strcmp(app.icon, "ed") == 0);
    CHECK(app.keywords != nullptr && strstr(app.keywords, "notes") != nullptr);
    CHECK(app.visible && app.action == nullptr);
    xd_app_release(&app);

    CHECK(load_entry(dir, "m.desktop",
                     "[Desktop Entry]\nType=Application\nName=Mouse\nExec=true\n"
                     "Categories=System;Settings;\n",
                     &arena, &app));
    CHECK(strcmp(app.folder, "Settings") == 0); /* Settings wins over System */
    xd_app_release(&app);

    /* Visibility rules come from GIO. */
    CHECK(load_entry(dir, "h.desktop",
                     "[Desktop Entry]\nType=Application\nName=H\nExec=true\nNoDisplay=true\n",
                     &arena, &app));
    CHECK(!app.visible);
    xd_app_release(&app);
    CHECK(load_entry(dir, "o.desktop",
                     "[Desktop Entry]\nType=Application\nName=O\nExec=true\nOnlyShowIn=KDE;\n",
                     &arena, &app));
    CHECK(!app.visible);
    xd_app_release(&app);
    CHECK(load_entry(dir, "x.desktop",
                     "[Desktop Entry]\nType=Application\nName=X\nExec=true\n"
                     "OnlyShowIn=XeonDesktop;\n",
                     &arena, &app));
    CHECK(app.visible && strcmp(app.folder, "Other") == 0);
    xd_app_release(&app);
    /* TryExec that doesn't exist: GIO refuses the entry. */
    CHECK(!load_entry(dir, "t.desktop",
                      "[Desktop Entry]\nType=Application\nName=T\nExec=true\n"
                      "TryExec=/nonexistent/xd\n",
                      &arena, &app));
    CHECK(!load_entry(dir, "bad.desktop", "not a desktop file", &arena, &app));

    CHECK(load_entry(dir, "s.desktop",
                     "[Desktop Entry]\nType=Application\nName=Show Desktop\nExec=true\n"
                     "X-XeonDesktop-Action=show-desktop\n",
                     &arena, &app));
    CHECK(app.action != nullptr && strcmp(app.action, "show-desktop") == 0);
    xd_app_release(&app);

    static const char *const files[] = {"ed.desktop", "m.desktop", "h.desktop", "o.desktop",
                                        "x.desktop",  "t.desktop", "bad.desktop", "s.desktop"};
    xd_strbuf p = {0};
    for (size_t i = 0; i < XD_ARRAY_LEN(files); i++) {
        xd_strbuf_clear(&p);
        xd_strbuf_appendf(&p, "%s/%s", dir, files[i]);
        unlink(xd_strbuf_cstr(&p));
    }
    xd_strbuf_free(&p);
    rmdir(dir);
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

static void test_tray_rules(void) {
    xd_settings a;
    xd_settings_parse(XD_STR("[Notifications]\nnm-applet=hide\nsteam=show\nbad=whatever\n"
                             "quiet=auto\n"),
                      &a);
    CHECK(xd_settings_tray_rule(&a, "nm-applet") == XD_TRAY_ALWAYS_HIDE);
    CHECK(xd_settings_tray_rule(&a, "steam") == XD_TRAY_ALWAYS_SHOW);
    CHECK(xd_settings_tray_rule(&a, "bad") == XD_TRAY_AUTO);
    CHECK(xd_settings_tray_rule(&a, "quiet") == XD_TRAY_AUTO && a.tray_rule_count == 2);
    xd_settings_set_tray_rule(&a, "steam", XD_TRAY_AUTO);
    CHECK(xd_settings_tray_rule(&a, "steam") == XD_TRAY_AUTO && a.tray_rule_count == 1);
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && xd_settings_tray_rule(&b, "nm-applet") == XD_TRAY_ALWAYS_HIDE);
    xd_strbuf_free(&text);
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

static void test_power_settings(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    /* XP's defaults: the volume and power icons shown, a password on resume. */
    CHECK(a.volume_icon && a.battery_icon && a.resume_password);
    CHECK(a.power_scheme == XD_POWER_HOME && a.standby == 0 && a.hibernate == 0);
    xd_settings_parse(XD_STR("[Sound]\nVolumeIcon=false\n"
                             "[Power]\nScheme=portable\nStandby=20\nHibernate=180\n"
                             "AlwaysShowIcon=false\nPasswordOnResume=false\n"),
                      &a);
    CHECK(!a.volume_icon && a.power_scheme == XD_POWER_PORTABLE);
    CHECK(a.standby == 20 && a.hibernate == 180 && !a.battery_icon && !a.resume_password);
    CHECK(strcmp(xd_power_scheme_name(a.power_scheme), "portable") == 0);
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && b.standby == 20 && !b.volume_icon);
    xd_strbuf_free(&text);
    /* Out of range or unknown: the defaults stay. */
    xd_settings_parse(XD_STR("[Power]\nScheme=turbo\nStandby=-5\nHibernate=99999\n"), &a);
    CHECK(a.power_scheme == XD_POWER_HOME && a.standby == 0 && a.hibernate == 0);
}

static void test_sound_scheme(void) {
    xd_sound_scheme s;
    xd_sound_scheme_init(&s, XD_SOUNDS_DEFAULT);
    CHECK(!xd_sound_scheme_modified(&s));
    CHECK(strcmp(s.sounds[XD_SOUND_CRITICAL_STOP], "dialog-error") == 0);
    CHECK(xd_sound_kind_of(s.sounds[XD_SOUND_LOGON]) == XD_SOUND_THEMED);
    CHECK(xd_sound_kind_of(s.sounds[XD_SOUND_MAXIMIZE]) == XD_SOUND_NONE);
    CHECK(strcmp(xd_sound_event_get(XD_SOUND_DEFAULT_BEEP)->label, "Default Beep") == 0);
    /* Paths must be absolute; theme names plain. */
    CHECK(xd_sound_scheme_set(&s, XD_SOUND_LOGON, "/usr/share/sounds/x.wav"));
    CHECK(xd_sound_kind_of(s.sounds[XD_SOUND_LOGON]) == XD_SOUND_FILE);
    CHECK(!xd_sound_scheme_set(&s, XD_SOUND_LOGON, "../x.wav"));
    CHECK(!xd_sound_scheme_set(&s, XD_SOUND_LOGON, "a b"));
    CHECK(xd_sound_scheme_set(&s, XD_SOUND_MAXIMIZE, "window-maximized"));
    CHECK(xd_sound_scheme_set(&s, XD_SOUND_ASTERISK, ""));
    CHECK(xd_sound_scheme_modified(&s));

    /* Round trip. */
    xd_strbuf text = {0};
    xd_sound_scheme_serialize(&s, &text);
    xd_sound_scheme t;
    xd_sound_scheme_parse(xd_strbuf_view(&text), &t);
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++)
        CHECK(strcmp(s.sounds[i], t.sounds[i]) == 0);
    xd_sound_scheme_free(&t);
    xd_strbuf_free(&text);
    xd_sound_scheme_free(&s);

    /* No Sounds, with one event set; bad values keep the scheme's. */
    xd_sound_scheme_parse(XD_STR("[Sounds]\nScheme=none\n[Events]\nWindowsLogon=service-login\n"
                                 "CriticalStop=relative/path.wav\nUnknown=bell\n"),
                          &s);
    CHECK(s.base == XD_SOUNDS_NONE && strcmp(s.sounds[XD_SOUND_LOGON], "service-login") == 0);
    CHECK(s.sounds[XD_SOUND_CRITICAL_STOP][0] == '\0' && xd_sound_scheme_modified(&s));
    xd_sound_scheme_free(&s);
    /* A broken file: Windows Default. */
    xd_sound_scheme_parse(XD_STR("[Sounds\nScheme=none\n"), &s);
    CHECK(s.base == XD_SOUNDS_DEFAULT && !xd_sound_scheme_modified(&s));
    xd_sound_scheme_free(&s);
}

static int css_errors;

static void on_css_error([[maybe_unused]] GtkCssProvider *p, [[maybe_unused]] GtkCssSection *s,
                         const GError *err, [[maybe_unused]] gpointer data) {
    css_errors++;
    fprintf(stderr, "gtk css: %s\n", err->message);
}

/* The generated GTK theme must parse cleanly for every style and scheme. */
static void test_gtk_css(void) {
    /* Style properties (-GtkScrollbar-*) need the widget classes. */
    g_type_ensure(GTK_TYPE_SCROLLBAR);
    static const char *const schemes[] = {"", "windows-classic", "high-contrast-black",
                                          "high-contrast-white"};
    for (int style = 0; style < XD_STYLE_COUNT; style++) {
        for (size_t k = 0; k < XD_ARRAY_LEN(schemes); k++) {
            xd_scheme scheme;
            xd_scheme_init_default(&scheme);
            if (schemes[k][0] != '\0') {
                xd_strbuf path = {0};
                xd_strbuf_appendf(&path, "%s/data/schemes/%s.ini", XD_SOURCE_ROOT, schemes[k]);
                CHECK(xd_scheme_load(xd_strbuf_cstr(&path), &scheme, nullptr));
                xd_strbuf_free(&path);
            }
            xd_theme t;
            xd_theme_init(&t, (xd_visual_style)style, &scheme, 1.0);
            xd_strbuf css = {0};
            xd_theme_gtk_css(&t, &css);
            CHECK(strstr(xd_strbuf_cstr(&css), "@define-color theme_selected_bg_color") != nullptr);
            GtkCssProvider *p = gtk_css_provider_new();
            g_signal_connect(p, "parsing-error", G_CALLBACK(on_css_error), nullptr);
            css_errors = 0;
            CHECK(gtk_css_provider_load_from_data(p, css.data, (gssize)css.len, nullptr));
            CHECK(css_errors == 0);
            g_object_unref(p);
            xd_strbuf_free(&css);
        }
    }
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

/* Drives tests/passwd/fake-passwd to the end. */
static xd_passwd_result run_passwd(const char *old_pw, const char *new_pw, char **message) {
    xd_passwd *p = xd_passwd_start(XD_SOURCE_ROOT "/tests/passwd/fake-passwd", old_pw, new_pw,
                                   nullptr);
    if (p == nullptr)
        return XD_PASSWD_FAILED;
    xd_passwd_result r = XD_PASSWD_RUNNING;
    for (int i = 0; i < 200 && r == XD_PASSWD_RUNNING; i++) {
        struct pollfd pfd = {.fd = xd_passwd_fd(p), .events = POLLIN};
        if (poll(&pfd, 1, 5000) <= 0)
            break;
        r = xd_passwd_step(p);
    }
    *message = g_strdup(xd_passwd_message(p));
    xd_passwd_free(p);
    return r;
}

static void test_passwd(void) {
    char *m = nullptr;
    CHECK(run_passwd("old secret", "new secret", &m) == XD_PASSWD_OK);
    g_free(m);
    CHECK(run_passwd("wrong", "new secret", &m) == XD_PASSWD_WRONG_OLD);
    CHECK(m != nullptr && strstr(m, "password unchanged") != nullptr);
    g_free(m);
    CHECK(run_passwd("old secret", "short", &m) == XD_PASSWD_REJECTED);
    CHECK(m != nullptr && strcmp(m, "BAD PASSWORD: The password is shorter than 8 characters") == 0);
    g_free(m);
    /* Nothing typed may come back as a message. */
    CHECK(run_passwd("old secret", "short", &m) == XD_PASSWD_REJECTED && strstr(m, "short\n") == nullptr);
    g_free(m);
}

static void test_intl(void) {
    CHECK(strcmp(xd_intl_long_date_format("%m/%d/%Y"), "%A, %B %-d, %Y") == 0);
    CHECK(strcmp(xd_intl_long_date_format("%d.%m.%Y"), "%A, %-d %B %Y") == 0);
    CHECK(strcmp(xd_intl_long_date_format("%Y\u5e74%m\u6708%d\u65e5"), "%Y %B %-d, %A") == 0);
    CHECK(strcmp(xd_intl_long_date_format("%F"), "%Y %B %-d, %A") == 0);
    CHECK(strcmp(xd_intl_long_date_format("%%%-m/%e"), "%A, %B %-d, %Y") == 0);
    CHECK(strcmp(xd_intl_long_date_format(nullptr), "%A, %-d %B %Y") == 0);

    CHECK(xd_intl_valid_name("de_DE.UTF-8") && xd_intl_valid_name("sr_RS@latin"));
    CHECK(!xd_intl_valid_name("") && !xd_intl_valid_name("de DE") && !xd_intl_valid_name("a\nb"));

    const char *const l1[] = {"LC_TIME=de_DE.UTF-8", "LANG=en_US.UTF-8", nullptr};
    CHECK(strcmp(xd_intl_lang(l1), "en_US.UTF-8") == 0);
    char **set = xd_intl_replace_lang(l1, "fr_FR.UTF-8");
    CHECK(g_strv_length(set) == 2 && strcmp(set[0], "LANG=fr_FR.UTF-8") == 0 &&
          strcmp(set[1], "LC_TIME=de_DE.UTF-8") == 0);
    g_strfreev(set);
    set = xd_intl_replace_lang(nullptr, "C.UTF-8");
    CHECK(g_strv_length(set) == 1 && xd_intl_lang((const char *const *)set) != nullptr);
    g_strfreev(set);

    char *s = xd_intl_first("us,de");
    CHECK(strcmp(s, "us") == 0);
    g_free(s);
    s = xd_intl_first(nullptr);
    CHECK(strcmp(s, "") == 0);
    g_free(s);
    s = xd_intl_replace_first("us,de", "fr");
    CHECK(strcmp(s, "fr,de") == 0);
    g_free(s);
    s = xd_intl_replace_first(",nodeadkeys", "dvorak");
    CHECK(strcmp(s, "dvorak,nodeadkeys") == 0);
    g_free(s);
    s = xd_intl_replace_first("", "fr");
    CHECK(strcmp(s, "fr") == 0);
    g_free(s);

    /* The formats file round trip, and removing it. */
    char *dir = g_dir_make_tmp("xd-intl-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "environment.d", "60-xeondesktop-locale.conf", nullptr);
    CHECK(xd_intl_read_formats(path) == nullptr);
    CHECK(xd_intl_write_formats(path, "C.UTF-8", nullptr));
    s = xd_intl_read_formats(path);
    CHECK(s != nullptr && strcmp(s, "C.UTF-8") == 0);
    g_free(s);
    char *text = nullptr;
    CHECK(g_file_get_contents(path, &text, nullptr, nullptr) &&
          strstr(text, "\nLC_NUMERIC=C.UTF-8\n") != nullptr &&
          strstr(text, "LC_MESSAGES") == nullptr && strstr(text, "LANG") == nullptr);
    g_free(text);
    CHECK(!xd_intl_write_formats(path, "x; rm -rf /", nullptr));
    CHECK(xd_intl_write_formats(path, nullptr, nullptr) && !g_file_test(path, G_FILE_TEST_EXISTS));
    CHECK(xd_intl_write_formats(path, nullptr, nullptr)); /* already gone */

    /* Only installed UTF-8 locales are offered: C.UTF-8 is built in. */
    char *supported = g_build_filename(dir, "SUPPORTED", nullptr);
    g_file_set_contents(supported,
                        "C.UTF-8 UTF-8\nxx_XX.UTF-8 UTF-8\nde_DE ISO-8859-1\n# comment\n\n",
                        -1, nullptr);
    char **locales = xd_intl_installed_locales(supported);
    CHECK(g_strv_length(locales) == 1 && strcmp(locales[0], "C.UTF-8") == 0);
    g_strfreev(locales);
    locales = xd_intl_installed_locales("/nonexistent");
    CHECK(locales != nullptr && locales[0] == nullptr);
    g_strfreev(locales);
    g_remove(supported);
    g_free(supported);

    s = xd_intl_display_name("C.UTF-8");
    CHECK(strcmp(s, "C.UTF-8") == 0);
    g_free(s);
    s = xd_intl_display_name("xx_XX.UTF-8");
    CHECK(strcmp(s, "xx_XX.UTF-8") == 0);
    g_free(s);

    xd_intl_samples smp;
    CHECK(xd_intl_samples_get("C.UTF-8", 0, &smp));
    CHECK(strcmp(smp.number, "123456789.00") == 0 && smp.long_date[0] != '\0' &&
          smp.short_date[0] != '\0' && smp.time[0] != '\0');
    xd_intl_samples_free(&smp);
    CHECK(!xd_intl_samples_get("xx_XX.UTF-8", 0, &smp));

    char *envd = g_path_get_dirname(path);
    g_rmdir(envd);
    g_rmdir(dir);
    g_free(envd);
    g_free(path);
    g_free(dir);
}

static void test_users(void) {
    CHECK(xd_users_valid_name("bob") && xd_users_valid_name("a1_b-c"));
    CHECK(!xd_users_valid_name("") && !xd_users_valid_name("Bob") && !xd_users_valid_name("1bob"));
    CHECK(!xd_users_valid_name("-bob") && !xd_users_valid_name("_bob") &&
          !xd_users_valid_name("bo b") && !xd_users_valid_name(nullptr));
    CHECK(xd_users_valid_name("abcdefghijklmnopqrstuvwxyzabcdef") &&
          !xd_users_valid_name("abcdefghijklmnopqrstuvwxyzabcdefg"));

    char *s = xd_users_suggest_name("Robert Smith");
    CHECK(strcmp(s, "robertsmith") == 0);
    g_free(s);
    s = xd_users_suggest_name("2nd O'Brien 3");
    CHECK(strcmp(s, "ndobrien3") == 0 && xd_users_valid_name(s));
    g_free(s);
    s = xd_users_suggest_name("\u4e2d\u6587");
    CHECK(s != nullptr && !xd_users_valid_name(s)); /* nothing usable: "" or "??" dropped */
    g_free(s);

    /* A hash crypt() accepts for the password and nothing else. */
    char *hash = xd_users_hash_password("s3cret pw");
    CHECK(hash != nullptr && hash[0] == '$');
    if (hash != nullptr) {
        struct crypt_data *cd = g_new0(struct crypt_data, 1);
        const char *again = crypt_r("s3cret pw", hash, cd);
        CHECK(again != nullptr && strcmp(again, hash) == 0);
        const char *wrong = crypt_r("s3cret px", hash, cd);
        CHECK(wrong == nullptr || strcmp(wrong, hash) != 0);
        g_free(cd);
    }
    char *other = xd_users_hash_password("s3cret pw");
    CHECK(other != nullptr && hash != nullptr && strcmp(other, hash) != 0); /* salted */
    g_free(other);
    g_free(hash);

    /* Pictures: the centre square, scaled. */
    char *dir = g_dir_make_tmp("xd-users-XXXXXX", nullptr);
    char *src = g_build_filename(dir, "wide.png", nullptr);
    char *dst = g_build_filename(dir, "face.png", nullptr);
    GdkPixbuf *wide = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 300, 100);
    gdk_pixbuf_fill(wide, 0xff000000);
    /* A green centre, so the crop shows. */
    GdkPixbuf *centre = gdk_pixbuf_new_subpixbuf(wide, 100, 0, 100, 100);
    gdk_pixbuf_fill(centre, 0x00ff0000);
    g_object_unref(centre);
    CHECK(gdk_pixbuf_save(wide, src, "png", nullptr, nullptr));
    g_object_unref(wide);
    CHECK(xd_users_make_picture(src, dst, 96, nullptr));
    GdkPixbuf *face = gdk_pixbuf_new_from_file(dst, nullptr);
    CHECK(face != nullptr && gdk_pixbuf_get_width(face) == 96 && gdk_pixbuf_get_height(face) == 96);
    if (face != nullptr) {
        const guchar *px = gdk_pixbuf_read_pixels(face);
        CHECK(px[0] < 16 && px[1] > 240); /* green, not the red edges */
        g_object_unref(face);
    }
    GError *error = nullptr;
    CHECK(!xd_users_make_picture("/nonexistent.png", dst, 96, &error) && error != nullptr);
    g_clear_error(&error);
    g_remove(src);
    g_remove(dst);
    g_rmdir(dir);
    g_free(src);
    g_free(dst);
    g_free(dir);
}

/* Folder Options: [Folders] in desktop.ini, xd-files' hidden
 * extensions, and the File Types tab's extensions. */
static void test_folder_options(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    CHECK(!a.folders.new_window && !a.folders.single_click && !a.folders.show_hidden);
    CHECK(a.folders.full_path_address && !a.folders.full_path_title && a.folders.remember_views);
    xd_settings_parse(XD_STR("[Folders]\nNewWindow=true\nSingleClick=1\nShowHidden=true\n"
                             "HideExtensions=true\nFullPathAddress=false\nFullPathTitle=true\n"
                             "RememberViews=false\nDefaultView=Details\n"),
                      &a);
    CHECK(a.folders.new_window && a.folders.single_click && a.folders.show_hidden);
    CHECK(a.folders.hide_extensions && !a.folders.full_path_address && a.folders.full_path_title);
    CHECK(!a.folders.remember_views && strcmp(a.folders.default_view, "details") == 0);
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && strcmp(b.folders.default_view, "details") == 0);
    xd_strbuf_free(&text);
    /* An unknown view keeps the default. */
    xd_settings_parse(XD_STR("[Folders]\nDefaultView=../x\n"), &a);
    CHECK(a.folders.default_view[0] == '\0');

    CHECK(xd_name_base_len("photo.jpg") == 5);
    CHECK(xd_name_base_len("archive.tar.gz") == 11);
    CHECK(xd_name_base_len("README") == 6);
    CHECK(xd_name_base_len(".bashrc") == 7);
    CHECK(xd_name_base_len("odd.") == 4);
    CHECK(xd_name_base_len("") == 0);

    static const char xml[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<mime-type xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\" "
        "type=\"text/x-c\">\n"
        "  <comment>C source code</comment>\n"
        "  <glob pattern=\"*.c\" case-sensitive=\"true\"/>\n"
        "  <glob pattern=\"*.H\"/>\n"
        "  <glob pattern=\"*.h\" weight=\"60\"/>\n"
        "  <glob pattern=\"*.[ch]\"/>\n"
        "  <glob pattern=\"Makefile*\"/>\n"
        "  <glob pattern=\"*.\"/>\n"
        "  <sub-class-of type=\"text/plain\"><glob pattern=\"*.nested\"/></sub-class-of>\n"
        "</mime-type>\n";
    GPtrArray *exts = g_ptr_array_new_with_free_func(g_free);
    xd_mime_xml_extensions(xml, -1, exts);
    CHECK(exts->len == 2 && strcmp(g_ptr_array_index(exts, 0), "c") == 0 &&
          strcmp(g_ptr_array_index(exts, 1), "h") == 0);
    /* Malformed XML keeps what came before the error. */
    g_ptr_array_set_size(exts, 0);
    xd_mime_xml_extensions("<mime-type><glob pattern=\"*.png\"/><glob <<", -1, exts);
    CHECK(exts->len == 1 && strcmp(g_ptr_array_index(exts, 0), "png") == 0);
    g_ptr_array_unref(exts);
    char **none = xd_content_type_extensions("../../etc/passwd");
    CHECK(none != nullptr && none[0] == nullptr);
    g_strfreev(none);
}

/* The Fonts folder's names, user fonts and Hide Variations. */
static void test_fonts(void) {
    CHECK(strcmp(xd_font_format_label("TrueType", "/f/a.ttf"), "TrueType") == 0);
    CHECK(strcmp(xd_font_format_label("TrueType", "/f/a.otf"), "OpenType") == 0);
    CHECK(strcmp(xd_font_format_label("CFF", "/f/a.otf"), "OpenType") == 0);
    CHECK(strcmp(xd_font_format_label("Type 1", "/f/a.pfb"), "Type 1") == 0);
    CHECK(strcmp(xd_font_format_label("PCF", "/f/a.pcf.gz"), "Bitmap") == 0);
    CHECK(xd_font_format_label("Unknown", "/f/a") == nullptr);
    CHECK(xd_font_format_label(nullptr, "/f/a") == nullptr);

    char *n = xd_font_display_name("DejaVu Sans Bold", "DejaVu Sans", "Bold", "TrueType");
    CHECK(strcmp(n, "DejaVu Sans Bold (TrueType)") == 0);
    g_free(n);
    n = xd_font_display_name(nullptr, "Foo", "Regular", "OpenType");
    CHECK(strcmp(n, "Foo (OpenType)") == 0);
    g_free(n);
    n = xd_font_display_name("", "Foo", "Italic", nullptr);
    CHECK(strcmp(n, "Foo Italic") == 0);
    g_free(n);

    CHECK(xd_path_in_dir("/home/u/.local/share/fonts/a.ttf", "/home/u/.local/share/fonts"));
    CHECK(xd_path_in_dir("/home/u/.local/share/fonts/sub/a.ttf", "/home/u/.local/share/fonts/"));
    CHECK(!xd_path_in_dir("/home/u/.local/share/fonts", "/home/u/.local/share/fonts"));
    CHECK(!xd_path_in_dir("/home/u/.local/share/fonts/", "/home/u/.local/share/fonts"));
    CHECK(!xd_path_in_dir("/home/u/.local/share/fontsx/a.ttf", "/home/u/.local/share/fonts"));
    CHECK(!xd_path_in_dir("/usr/share/fonts/a.ttf", "/home/u/.local/share/fonts"));

    char fa[] = "A", fb[] = "B";
    xd_font bold = {.family = fa, .weight = FC_WEIGHT_BOLD, .width = FC_WIDTH_NORMAL};
    xd_font italic = {.family = fa, .weight = FC_WEIGHT_REGULAR, .slant = FC_SLANT_ITALIC,
                      .width = FC_WIDTH_NORMAL};
    xd_font regular = {.family = fa, .weight = FC_WEIGHT_REGULAR, .width = FC_WIDTH_NORMAL};
    xd_font only = {.family = fb, .weight = FC_WEIGHT_LIGHT, .slant = FC_SLANT_ITALIC,
                    .width = FC_WIDTH_CONDENSED};
    xd_font *const list[] = {&bold, &italic, &regular, &only};
    xd_fonts_mark_primary(list, G_N_ELEMENTS(list));
    CHECK(!bold.primary && !italic.primary && regular.primary && only.primary);

    char *v = xd_font_version_text(0x00025eb8); /* 2.37 */
    CHECK(strcmp(v, "Version 2.37") == 0);
    g_free(v);
    v = xd_font_version_text(0x0001ffff); /* rounds up to 2.00 */
    CHECK(strcmp(v, "Version 2.00") == 0);
    g_free(v);
    v = xd_font_version_text(0);
    CHECK(v[0] == '\0');
    g_free(v);
}

/* [Mouse], [Accessibility] and the [Keyboard] keys the applets add. */
static void test_input_settings(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    CHECK(!a.mouse.left_handed && a.mouse.acceleration && a.mouse.double_click_ms == 500);
    CHECK(a.mouse.wheel_lines == 3 && a.mouse.cursor_size == 24 && a.mouse.cursor_theme[0] == 0);
    CHECK(a.keyboard.cursor_blink_ms == 530 && !a.access.sticky_keys && a.access.sticky_lock);

    xd_settings_parse(XD_STR("[Mouse]\nLeftHanded=true\nSpeed=-4\nAcceleration=false\n"
                             "DoubleClickMs=350\nWheelLines=6\nCursorTheme=Breeze_Light\n"
                             "CursorSize=48\n[Keyboard]\nCursorBlinkMs=0\n"
                             "[Accessibility]\nStickyKeys=true\nStickyKeysLock=false\n"
                             "BounceKeys=true\nBounceMs=700\n"),
                      &a);
    CHECK(a.mouse.left_handed && a.mouse.speed == -4 && !a.mouse.acceleration);
    CHECK(a.mouse.double_click_ms == 350 && a.mouse.wheel_lines == 6 && a.mouse.cursor_size == 48);
    CHECK(strcmp(a.mouse.cursor_theme, "Breeze_Light") == 0 && a.keyboard.cursor_blink_ms == 0);
    CHECK(a.access.sticky_keys && !a.access.sticky_lock && a.access.bounce_keys &&
          a.access.bounce_ms == 700);

    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && memcmp(&a.mouse, &b.mouse, sizeof(a.mouse)) == 0);
    CHECK(memcmp(&a.access, &b.access, sizeof(a.access)) == 0);
    xd_strbuf_free(&text);

    /* Out of range keeps the default; a theme can't be a path. */
    xd_settings_parse(XD_STR("[Mouse]\nSpeed=11\nDoubleClickMs=50\nWheelLines=0\n"
                             "CursorTheme=../x\nCursorSize=4\n[Accessibility]\nBounceMs=5\n"),
                      &a);
    CHECK(a.mouse.speed == 0 && a.mouse.double_click_ms == 500 && a.mouse.wheel_lines == 3);
    CHECK(a.mouse.cursor_theme[0] == 0 && a.mouse.cursor_size == 24 && a.access.bounce_ms == 500);
    xd_settings_parse(XD_STR("[Mouse]\nCursorTheme=.hidden\n"), &a);
    CHECK(a.mouse.cursor_theme[0] == 0);

    /* High Contrast switches to Classic and back to what was there. */
    xd_settings_defaults(&a);
    a.style = XD_STYLE_LUNA;
    g_strlcpy(a.scheme, "windows-standard", sizeof(a.scheme));
    xd_settings_set_high_contrast(&a, true, "high-contrast-black");
    CHECK(a.access.high_contrast && a.style == XD_STYLE_CLASSIC);
    CHECK(strcmp(a.scheme, "high-contrast-black") == 0);
    /* Another high-contrast scheme keeps the look to go back to. */
    xd_settings_set_high_contrast(&a, true, "high-contrast-white");
    CHECK(strcmp(a.scheme, "high-contrast-white") == 0 && a.access.saved_style == XD_STYLE_LUNA);
    xd_settings_serialize(&a, &text);
    xd_settings_parse(xd_strbuf_view(&text), &b);
    xd_strbuf_free(&text);
    CHECK(b.access.high_contrast && strcmp(b.access.saved_scheme, "windows-standard") == 0);
    xd_settings_set_high_contrast(&b, false, "");
    CHECK(!b.access.high_contrast && b.style == XD_STYLE_LUNA);
    CHECK(strcmp(b.scheme, "windows-standard") == 0);
    /* Off when it's already off changes nothing. */
    xd_settings_set_high_contrast(&b, false, "");
    CHECK(b.style == XD_STYLE_LUNA && strcmp(b.scheme, "windows-standard") == 0);
}

static void test_kbd_layouts(void) {
    xd_kbd_layout l[XD_KBD_LAYOUTS_MAX];
    CHECK(xd_kbd_layouts_parse("", "", l) == 0);
    CHECK(xd_kbd_layouts_parse("us,de", ",nodeadkeys", l) == 2);
    CHECK(strcmp(l[0].name, "us") == 0 && l[0].variant[0] == 0);
    CHECK(strcmp(l[1].name, "de") == 0 && strcmp(l[1].variant, "nodeadkeys") == 0);
    /* Fewer variants than layouts; at most four layouts. */
    CHECK(xd_kbd_layouts_parse("us, fr ,ru,de,gb", "dvorak", l) == 4);
    CHECK(strcmp(l[0].variant, "dvorak") == 0 && strcmp(l[1].name, "fr") == 0 &&
          l[1].variant[0] == 0 && strcmp(l[3].name, "de") == 0);

    char layouts[64], variants[64];
    CHECK(xd_kbd_layouts_format(l, 2, layouts, sizeof(layouts), variants, sizeof(variants)));
    CHECK(strcmp(layouts, "us,fr") == 0 && strcmp(variants, "dvorak,") == 0);
    xd_kbd_layouts_parse("us,de", "", l);
    CHECK(xd_kbd_layouts_format(l, 2, layouts, sizeof(layouts), variants, sizeof(variants)));
    CHECK(strcmp(layouts, "us,de") == 0 && variants[0] == 0);
    CHECK(!xd_kbd_layouts_format(l, 2, layouts, 4, variants, sizeof(variants)) && layouts[0] == 0);

    char *o = xd_kbd_option_in_group("caps:ctrl_modifier,grp:alt_shift_toggle", "grp");
    CHECK(o != nullptr && strcmp(o, "grp:alt_shift_toggle") == 0);
    g_free(o);
    /* "grp_led:" is another group. */
    CHECK(xd_kbd_option_in_group("grp_led:scroll", "grp") == nullptr);
    o = xd_kbd_option_set("caps:ctrl_modifier", "compose:ralt", true);
    CHECK(strcmp(o, "caps:ctrl_modifier,compose:ralt") == 0);
    char *o2 = xd_kbd_option_set(o, "compose:ralt", true); /* never twice */
    CHECK(strcmp(o2, "caps:ctrl_modifier,compose:ralt") == 0);
    g_free(o);
    o = xd_kbd_option_set(o2, "caps:ctrl_modifier", false);
    CHECK(strcmp(o, "compose:ralt") == 0);
    g_free(o2);
    g_free(o);
    o = xd_kbd_options_clear_group("grp:win_space_toggle,grp_led:scroll,,grp:alt_caps_toggle",
                                   "grp");
    CHECK(strcmp(o, "grp_led:scroll") == 0);
    g_free(o);
}

static void test_cursor_themes(void) {
    char *root = g_dir_make_tmp("xd-cursors-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    char *a = g_build_filename(root, "a", nullptr), *b = g_build_filename(root, "b", nullptr);
    /* a: "Zed" (with cursors) and "plain" (an icon theme, no cursors);
     * b: another "Zed" (hidden by a's), "Alpha", "default". */
    static const char *const made[] = {"a/Zed/cursors", "a/plain/48x48", "b/Zed/cursors",
                                       "b/Alpha/cursors", "b/default/cursors"};
    for (size_t i = 0; i < G_N_ELEMENTS(made); i++) {
        char *d = g_build_filename(root, made[i], nullptr);
        g_mkdir_with_parents(d, 0700);
        g_free(d);
    }
    char *index = g_build_filename(a, "Zed", "index.theme", nullptr);
    CHECK(g_file_set_contents(index, "[Icon Theme]\nName=Zed Cursors\n", -1, nullptr));
    const char *dirs[] = {a, b, "/nonexistent", nullptr};
    GPtrArray *list = xd_cursor_themes_list(dirs);
    CHECK(list->len == 2);
    if (list->len == 2) {
        const xd_cursor_theme *t0 = g_ptr_array_index(list, 0);
        const xd_cursor_theme *t1 = g_ptr_array_index(list, 1);
        CHECK(strcmp(t0->dir, "Alpha") == 0 && strcmp(t0->name, "Alpha") == 0);
        CHECK(strcmp(t1->dir, "Zed") == 0 && strcmp(t1->name, "Zed Cursors") == 0);
    }
    g_ptr_array_unref(list);
    char **search = xd_cursor_theme_dirs();
    CHECK(g_strv_length(search) >= 3 && strcmp(search[g_strv_length(search) - 1],
                                               "/usr/share/pixmaps") == 0);
    g_strfreev(search);

    /* Children before parents. */
    static const char *const rm[] = {"a/Zed/index.theme", "a/Zed/cursors", "a/Zed",
                                     "a/plain/48x48", "a/plain", "a", "b/Zed/cursors", "b/Zed",
                                     "b/Alpha/cursors", "b/Alpha", "b/default/cursors",
                                     "b/default", "b"};
    for (size_t i = 0; i < G_N_ELEMENTS(rm); i++) {
        char *path = g_build_filename(root, rm[i], nullptr);
        CHECK(g_remove(path) == 0);
        g_free(path);
    }
    CHECK(g_rmdir(root) == 0);
    g_free(index);
    g_free(a);
    g_free(b);
    g_free(root);
}

/* What systemd itself makes of a calendar expression, when systemd-analyze
 * is installed (it only parses: no manager is involved). */
static void check_systemd_calendar(const char *spec) {
    char *tool = g_find_program_in_path("systemd-analyze");
    if (tool == nullptr)
        return;
    char *argv[] = {tool, g_strdup("calendar"), g_strdup(spec), nullptr};
    char *out = nullptr;
    int status = -1;
    bool ran = g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_STDERR_TO_DEV_NULL, nullptr, nullptr,
                            &out, nullptr, &status, nullptr);
    char *want = g_strdup_printf("Normalized form: %s\n", spec);
    bool ok = ran && status == 0 && out != nullptr && strstr(out, want) != nullptr;
    CHECK(ok);
    if (!ok)
        fprintf(stderr, "  systemd-analyze calendar '%s':\n%s", spec, out != nullptr ? out : "");
    g_free(want);
    g_free(out);
    g_free(argv[2]);
    g_free(argv[1]);
    g_free(tool);
}

/* A schedule's OnCalendar=, what systemd makes of it, and back. */
static void check_calendar(const xd_schedule *s, const char *want) {
    char *cal = xd_schedule_calendar(s);
    CHECK(cal != nullptr && strcmp(cal, want) == 0);
    if (cal != nullptr && strcmp(cal, want) != 0)
        fprintf(stderr, "  calendar: got '%s', want '%s'\n", cal, want);
    check_systemd_calendar(want);
    xd_schedule back;
    CHECK(xd_schedule_from_calendar(want, &back));
    char *again = xd_schedule_calendar(&back);
    CHECK(again != nullptr && strcmp(again, want) == 0);
    g_free(again);
    g_free(cal);
}

static void check_describe(const xd_schedule *s, const char *want) {
    char *text = xd_schedule_describe(s);
    CHECK(strcmp(text, want) == 0);
    if (strcmp(text, want) != 0)
        fprintf(stderr, "  describe: got '%s', want '%s'\n", text, want);
    g_free(text);
}

static void check_str(char *got, const char *want) {
    CHECK(got != nullptr && strcmp(got, want) == 0);
    if (got != nullptr && strcmp(got, want) != 0)
        fprintf(stderr, "  got:\n%s\n  want:\n%s\n", got, want);
    g_free(got);
}

/* Scheduled Tasks: schedules as OnCalendar=, and the unit files. */
static void test_schedtasks(void) {
    xd_schedule daily = {.kind = XD_SCHED_DAILY, .hour = 9, .minute = 0};
    check_calendar(&daily, "*-*-* 09:00:00");
    check_describe(&daily, "At 9:00 AM every day");
    xd_schedule midnight = {.kind = XD_SCHED_DAILY, .hour = 0, .minute = 5};
    check_calendar(&midnight, "*-*-* 00:05:00");
    check_describe(&midnight, "At 12:05 AM every day");

    /* systemd writes runs of three or more days as ranges, Monday first. */
    xd_schedule weekly = {.kind = XD_SCHED_WEEKLY, .hour = 13, .minute = 30, .weekdays = 0x7};
    check_calendar(&weekly, "Mon..Wed *-*-* 13:30:00");
    check_describe(&weekly, "At 1:30 PM every Mon, Tue, Wed of every week");
    weekly.weekdays = 0x5d; /* Mon, Wed, Thu, Fri, Sun */
    check_calendar(&weekly, "Mon,Wed..Fri,Sun *-*-* 13:30:00");
    weekly.weekdays = 0x41; /* Sunday and Monday aren't a run */
    check_calendar(&weekly, "Mon,Sun *-*-* 13:30:00");
    weekly.weekdays = 0x3;
    check_calendar(&weekly, "Mon,Tue *-*-* 13:30:00");
    weekly.weekdays = 0x70;
    check_calendar(&weekly, "Fri..Sun *-*-* 13:30:00");
    /* Every day of the week is every day, and reads back as daily. */
    weekly.weekdays = XD_SCHED_ALL_DAYS;
    char *cal = xd_schedule_calendar(&weekly);
    CHECK(cal != nullptr && strcmp(cal, "*-*-* 13:30:00") == 0);
    g_free(cal);
    weekly.weekdays = 0;
    CHECK(!xd_schedule_valid(&weekly) && xd_schedule_calendar(&weekly) == nullptr);

    xd_schedule monthly = {.kind = XD_SCHED_MONTHLY, .hour = 23, .minute = 59, .day = 15,
                           .months = XD_SCHED_ALL_MONTHS};
    check_calendar(&monthly, "*-*-15 23:59:00");
    check_describe(&monthly, "At 11:59 PM on day 15 of every month");
    monthly.months = 0x9; /* January and April */
    check_calendar(&monthly, "*-01,04-15 23:59:00");
    check_describe(&monthly, "At 11:59 PM on day 15 of Jan, Apr");
    monthly.months = 0x7; /* systemd lists months one by one */
    check_calendar(&monthly, "*-01,02,03-15 23:59:00");
    monthly.day = 0;
    check_calendar(&monthly, "*-01,02,03~01 23:59:00");
    monthly.months = XD_SCHED_ALL_MONTHS;
    check_calendar(&monthly, "*-*~01 23:59:00");
    check_describe(&monthly, "At 11:59 PM on the last day of every month");
    monthly.months = 0;
    CHECK(!xd_schedule_valid(&monthly));

    xd_schedule once = {.kind = XD_SCHED_ONCE, .hour = 12, .minute = 0, .year = 2099,
                        .month = 3, .mday = 7};
    check_calendar(&once, "2099-03-07 12:00:00");
    check_describe(&once, "At 12:00 PM on 3/7/2099");
    once.month = 2;
    once.mday = 30;
    CHECK(!xd_schedule_valid(&once));
    once = (xd_schedule){.kind = XD_SCHED_ONCE, .hour = 24, .year = 2099, .month = 1, .mday = 1};
    CHECK(!xd_schedule_valid(&once));

    xd_schedule startup = {.kind = XD_SCHED_STARTUP}, logon = {.kind = XD_SCHED_LOGON};
    CHECK(xd_schedule_calendar(&startup) == nullptr && xd_schedule_calendar(&logon) == nullptr);
    check_describe(&startup, "Run at system startup");
    check_describe(&logon, "Run at user logon");

    /* Anything we wouldn't have written is someone else's: CUSTOM. */
    static const char *const foreign[] = {
        "*-*-* 09:00:30",         "*-*-* 9:00:00",       "Mon..Sun *-*-* 09:00:00",
        "Wed,Mon *-*-* 09:00:00", "*-*-1/2 09:00:00",    "2099-02-30 09:00:00",
        "*-1-15 09:00:00",        "*-*-* 09:00:00 UTC",  "daily",
        "",                       "Mon *-01-* 09:00:00", "*-13-01 09:00:00",
        "*-*~02 09:00:00",        "*-*-32 09:00:00",     "Mon,Tue,Wed *-*-* 09:00:00",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(foreign); i++) {
        xd_schedule s;
        bool ok = xd_schedule_from_calendar(foreign[i], &s);
        CHECK(!ok && s.kind == XD_SCHED_CUSTOM);
        if (ok)
            fprintf(stderr, "  read '%s' as ours\n", foreign[i]);
    }

    CHECK(xd_task_name_valid("Back up my files"));
    CHECK(xd_task_name_valid("Ünïcødé 100%"));
    CHECK(!xd_task_name_valid(""));
    CHECK(!xd_task_name_valid("   "));
    CHECK(!xd_task_name_valid("a/b"));
    CHECK(!xd_task_name_valid("a\\b"));
    CHECK(!xd_task_name_valid("what?"));
    CHECK(!xd_task_name_valid("two\nlines"));
    CHECK(!xd_task_name_valid(nullptr));
    CHECK(xd_task_workdir_valid("/home/u/My Files"));
    CHECK(!xd_task_workdir_valid("relative"));
    CHECK(!xd_task_workdir_valid("/ends/in/blank "));
    CHECK(!xd_task_workdir_valid("/ends/in\\"));
    CHECK(!xd_task_workdir_valid("/tab\there"));

    check_str(xd_task_unit_base("Back up!  my files"), "xd-task-Back-up-my-files");
    check_str(xd_task_unit_base("  --x__y--  "), "xd-task-x__y");
    check_str(xd_task_unit_base("日本語"), "xd-task-task");

    /* Every word reaches the program as it is. */
    const char *argv[] = {"/opt/My App/run", "a \"quoted\" word", "back\\slash", "100%",
                          "$HOME", "-x", "new\nline", "", "tab\tbell\a", nullptr};
    check_str(xd_task_exec_line(argv),
              "\"/opt/My App/run\" \"a \\\"quoted\\\" word\" \"back\\\\slash\" \"100%%\" "
              "\"$$HOME\" \"-x\" \"new\\nline\" \"\" \"tab\\tbell\\x07\"");
    const char *empty[] = {nullptr};
    CHECK(xd_task_exec_line(empty) == nullptr);

    const char *shown[] = {"/usr/bin/foo", "a b", "it's", "--size=10", nullptr};
    char *line = xd_task_command_line(shown);
    CHECK(strcmp(line, "/usr/bin/foo 'a b' 'it'\\''s' --size=10") == 0);
    char **parsed = nullptr;
    CHECK(g_shell_parse_argv(line, nullptr, &parsed, nullptr) &&
          g_strv_equal((const char *const *)parsed, shown));
    g_strfreev(parsed);
    g_free(line);

    char **ex = xd_task_argv_from_exec("firefox %u");
    CHECK(ex != nullptr && g_strv_length(ex) == 1 && strcmp(ex[0], "firefox") == 0);
    g_strfreev(ex);
    ex = xd_task_argv_from_exec("\"/opt/My App/app\" --name=%c --level=50%% %F");
    CHECK(ex != nullptr && g_strv_length(ex) == 3 && strcmp(ex[0], "/opt/My App/app") == 0 &&
          strcmp(ex[1], "--name=") == 0 && strcmp(ex[2], "--level=50%") == 0);
    g_strfreev(ex);
    CHECK(xd_task_argv_from_exec("%U") == nullptr);
    CHECK(xd_task_argv_from_exec("\"unterminated") == nullptr);
    CHECK(xd_task_argv_from_exec(nullptr) == nullptr);

    const char *run[] = {"/usr/bin/backup", "--to", "/mnt/My Disk", nullptr};
    xd_task t = {.name = "Back up 100%", .argv = run, .workdir = "/home/u",
                 .sched = daily, .max_run_sec = 72 * 3600};
    check_str(xd_task_service_text(&t),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Back up 100%%\n"
              "\n"
              "[Service]\n"
              "Type=exec\n"
              "ExecStart=\"/usr/bin/backup\" \"--to\" \"/mnt/My Disk\"\n"
              "WorkingDirectory=/home/u\n"
              "RuntimeMaxSec=259200\n");
    check_str(xd_task_timer_text(&t),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Back up 100%%\n"
              "\n"
              "[Timer]\n"
              "OnCalendar=*-*-* 09:00:00\n"
              "\n"
              "[Install]\n"
              "WantedBy=timers.target\n");
    t = (xd_task){.name = " Hello ", .argv = run, .sched = startup};
    check_str(xd_task_service_text(&t),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Hello\n"
              "\n"
              "[Service]\n"
              "Type=exec\n"
              "ExecStart=\"/usr/bin/backup\" \"--to\" \"/mnt/My Disk\"\n");
    check_str(xd_task_timer_text(&t),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Hello\n"
              "\n"
              "[Timer]\n"
              "OnStartupSec=0\n"
              "\n"
              "[Install]\n"
              "WantedBy=timers.target\n");
    t.sched = logon;
    check_str(xd_task_timer_text(&t),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Hello\n"
              "PartOf=graphical-session.target\n"
              "After=graphical-session.target\n"
              "\n"
              "[Timer]\n"
              "OnActiveSec=0\n"
              "\n"
              "[Install]\n"
              "WantedBy=graphical-session.target\n");
    /* Nothing is written for what can't be. */
    t.name = "a/b";
    CHECK(xd_task_service_text(&t) == nullptr && xd_task_timer_text(&t) == nullptr);
    t.name = "ok";
    t.workdir = "relative";
    CHECK(xd_task_service_text(&t) == nullptr);
    t.workdir = nullptr;
    t.sched = weekly; /* no days */
    CHECK(xd_task_timer_text(&t) == nullptr);
    t.sched.kind = XD_SCHED_CUSTOM;
    CHECK(xd_task_timer_text(&t) == nullptr);
    t.sched = daily;
    t.argv = empty;
    CHECK(xd_task_service_text(&t) == nullptr);
}

static bool type_is(xd_bt_type t, const char *icon, const char *name) {
    return g_strcmp0(t.icon, icon) == 0 && g_strcmp0(t.name, name) == 0;
}

static void test_bluetooth_info(void) {
    /* Class of Device: major class in bits 8-12, minor in 2-7. */
    CHECK(type_is(xd_bt_class_type(0x5a020c), "phone", "Smartphone"));
    CHECK(type_is(xd_bt_class_type(0x240404), "audio-headset", "Headset"));
    CHECK(type_is(xd_bt_class_type(0x240418), "audio-headphones", "Headphones"));
    CHECK(type_is(xd_bt_class_type(0x10c), "computer-laptop", "Laptop computer"));
    CHECK(type_is(xd_bt_class_type(0x002540), "input-keyboard", "Keyboard"));
    CHECK(type_is(xd_bt_class_type(0x002580), "input-mouse", "Mouse"));
    CHECK(type_is(xd_bt_class_type(0x0025c0), "input-keyboard", "Keyboard and mouse"));
    CHECK(type_is(xd_bt_class_type(0x000508), "input-gaming", "Game pad"));
    CHECK(type_is(xd_bt_class_type(0x040680), "printer", "Printer"));
    CHECK(type_is(xd_bt_class_type(0x020300), "network-wireless", "Network access point"));
    CHECK(xd_bt_class_type(0).name == nullptr);
    CHECK(xd_bt_class_type(0x1f00).name == nullptr); /* Uncategorized */
    /* LE Appearance: category in bits 6-15. */
    CHECK(type_is(xd_bt_appearance_type(0x03c1), "input-keyboard", "Keyboard"));
    CHECK(type_is(xd_bt_appearance_type(0x03c2), "input-mouse", "Mouse"));
    CHECK(type_is(xd_bt_appearance_type(0x03c4), "input-gaming", "Game pad"));
    CHECK(type_is(xd_bt_appearance_type(0x0942), "audio-headset", "Headset"));
    CHECK(type_is(xd_bt_appearance_type(0x0040), "phone", "Phone"));
    CHECK(xd_bt_appearance_type(0).name == nullptr);
    CHECK(type_is(xd_bt_icon_type("audio-headset"), "audio-headset", "Headset"));
    CHECK(type_is(xd_bt_icon_type("nonsense"), "bluetooth", "Bluetooth device"));
    CHECK(type_is(xd_bt_icon_type(nullptr), "bluetooth", "Bluetooth device"));

    /* Service UUIDs: the SIG's 16-bit ones on the Base UUID, any case. */
    CHECK(g_strcmp0(xd_bt_uuid_name("0000110b-0000-1000-8000-00805f9b34fb"), "Audio Sink") == 0);
    CHECK(g_strcmp0(xd_bt_uuid_name("0000110B-0000-1000-8000-00805F9B34FB"), "Audio Sink") == 0);
    CHECK(g_strcmp0(xd_bt_uuid_name("00001124-0000-1000-8000-00805f9b34fb"),
                    "Human Interface Device") == 0);
    CHECK(g_strcmp0(xd_bt_uuid_name("00001101-0000-1000-8000-00805f9b34fb"), "Serial Port") == 0);
    CHECK(g_strcmp0(xd_bt_uuid_name("00001850-0000-1000-8000-00805f9b34fb"),
                    "Published Audio Capabilities") == 0);
    CHECK(xd_bt_uuid_name("0000ffff-0000-1000-8000-00805f9b34fb") == nullptr);
    CHECK(xd_bt_uuid_name("1000110b-0000-1000-8000-00805f9b34fb") == nullptr);
    CHECK(xd_bt_uuid_name("0000110b-0000-1000-8000-00805f9b34fc") == nullptr);
    CHECK(xd_bt_uuid_name("0000110b") == nullptr);
    CHECK(xd_bt_uuid_name(nullptr) == nullptr);

    CHECK(g_strcmp0(xd_bt_version_name(4), "2.1 + EDR") == 0);
    CHECK(g_strcmp0(xd_bt_version_name(9), "5.0") == 0);
    CHECK(g_strcmp0(xd_bt_version_name(12), "5.3") == 0);
    CHECK(xd_bt_version_name(-1) == nullptr);
    CHECK(xd_bt_version_name(99) == nullptr);
}

static bool close_to(double a, double b) { return a - b < 1e-3 && b - a < 1e-3; }

static void test_joystick(void) {
    CHECK(close_to(xd_joy_axis_centered(-32768, -32768, 32767), -1));
    CHECK(close_to(xd_joy_axis_centered(32767, -32768, 32767), 1));
    CHECK(close_to(xd_joy_axis_centered(0, -32768, 32767), 0));
    CHECK(close_to(xd_joy_axis_centered(128, 0, 255), 0.0039));
    CHECK(close_to(xd_joy_axis_centered(40000, -32768, 32767), 1)); /* clamped */
    CHECK(close_to(xd_joy_axis_centered(5, 3, 3), 0));              /* no range */
    CHECK(close_to(xd_joy_axis_fraction(0, 0, 255), 0));
    CHECK(close_to(xd_joy_axis_fraction(255, 0, 255), 1));
    CHECK(close_to(xd_joy_axis_fraction(-10, 0, 255), 0));
    CHECK(close_to(xd_joy_axis_fraction(512, 0, 1023), 0.5005));

    CHECK(g_strcmp0(xd_joy_axis_name(ABS_X), "X Axis") == 0);
    CHECK(g_strcmp0(xd_joy_axis_name(ABS_RZ), "Z Rotation") == 0);
    CHECK(g_strcmp0(xd_joy_axis_name(ABS_THROTTLE), "Throttle") == 0);
    CHECK(xd_joy_axis_name(ABS_HAT0X) == nullptr);
    CHECK(xd_joy_axis_name(ABS_MT_POSITION_X) == nullptr);
    CHECK(xd_joy_axis_is_hat(ABS_HAT0X) && xd_joy_axis_is_hat(ABS_HAT3Y));
    CHECK(!xd_joy_axis_is_hat(ABS_X) && !xd_joy_axis_is_hat(ABS_MISC));

    /* Hats: -1 centred, 0 up, then clockwise. */
    CHECK(xd_joy_hat_direction(0, 0) == -1);
    CHECK(xd_joy_hat_direction(0, -1) == 0);
    CHECK(xd_joy_hat_direction(1, -1) == 1);
    CHECK(xd_joy_hat_direction(1, 0) == 2);
    CHECK(xd_joy_hat_direction(1, 1) == 3);
    CHECK(xd_joy_hat_direction(0, 1) == 4);
    CHECK(xd_joy_hat_direction(-1, 0) == 6);
    CHECK(xd_joy_hat_direction(-1, -1) == 7);
    CHECK(xd_joy_hat_direction(-127, 127) == 5);

    /* joydev's order: gamepad and joystick buttons, then BTN_MISC's;
     * keyboard keys left out. */
    const unsigned codes[] = {BTN_TRIGGER_HAPPY1, BTN_0, KEY_A, BTN_EAST, BTN_SOUTH};
    unsigned out[XD_ARRAY_LEN(codes)];
    size_t n = xd_joy_button_order(codes, XD_ARRAY_LEN(codes), out);
    CHECK(n == 4);
    CHECK(out[0] == BTN_SOUTH && out[1] == BTN_EAST && out[2] == BTN_TRIGGER_HAPPY1 &&
          out[3] == BTN_0);
    CHECK(xd_joy_button_order(codes, 0, out) == 0);
}

static void test_bluetooth_settings(void) {
    xd_settings s;
    xd_settings_defaults(&s);
    CHECK(s.bluetooth_icon);
    xd_settings_parse(XD_STR("[Bluetooth]\nShowIcon=false\n"), &s);
    CHECK(!s.bluetooth_icon);
    xd_strbuf out = {0};
    xd_settings_serialize(&s, &out);
    xd_settings t;
    xd_settings_parse(xd_strbuf_view(&out), &t);
    CHECK(!t.bluetooth_icon);
    xd_strbuf_free(&out);
}

/* Network Connections: XP's groups and status texts for NetworkManager's
 * states, the Status dialog's numbers, the TCP/IP form. */
static void test_netconn(void) {
    CHECK(xd_nc_group_for_type("802-3-ethernet") == XD_NC_LAN &&
          xd_nc_group_for_type("bridge") == XD_NC_LAN);
    CHECK(xd_nc_group_for_type("802-11-wireless") == XD_NC_WIRELESS);
    CHECK(xd_nc_group_for_type("vpn") == XD_NC_VPN && xd_nc_group_for_type("wireguard") == XD_NC_VPN);
    CHECK(xd_nc_group_for_type("gsm") == XD_NC_BROADBAND &&
          xd_nc_group_for_type("pppoe") == XD_NC_BROADBAND);
    CHECK(xd_nc_group_for_type("bluetooth") == XD_NC_PAN);
    CHECK(xd_nc_group_for_type("loopback") == XD_NC_OTHER && xd_nc_group_for_type(nullptr) == XD_NC_OTHER);
    CHECK(strcmp(xd_nc_group_title(XD_NC_LAN), "LAN or High-Speed Internet") == 0);
    CHECK(strcmp(xd_nc_group_title(XD_NC_VPN), "Virtual Private Network") == 0);

    /* An adapter: connected, cable out, disabled (by hand, unmanaged, radio off). */
    xd_nc_state s = {.has_device = true, .device_state = 100, .managed = true, .autoconnect = true,
                     .carrier_known = true, .carrier = true, .connectivity = 4};
    CHECK(strcmp(xd_nc_status_text(&s), "Connected") == 0 && xd_nc_state_connected(&s) &&
          !xd_nc_state_disabled(&s));
    s.connectivity = 3;
    CHECK(strcmp(xd_nc_status_text(&s), "Limited or no connectivity") == 0);
    s.connectivity = 0; /* not checked: connected */
    CHECK(strcmp(xd_nc_status_text(&s), "Connected") == 0);
    s.device_state = 20;
    s.carrier = false;
    CHECK(strcmp(xd_nc_status_text(&s), "Network cable unplugged") == 0 &&
          !xd_nc_state_connected(&s));
    s.carrier_known = false; /* Wi-Fi, say */
    CHECK(strcmp(xd_nc_status_text(&s), "Not connected") == 0);
    s.device_state = 30;
    CHECK(strcmp(xd_nc_status_text(&s), "Not connected") == 0 && !xd_nc_state_disabled(&s));
    s.autoconnect = false;
    CHECK(strcmp(xd_nc_status_text(&s), "Disabled") == 0 && xd_nc_state_disabled(&s));
    s.autoconnect = true;
    s.radio_off = true;
    CHECK(strcmp(xd_nc_status_text(&s), "Disabled") == 0);
    s.radio_off = false;
    s.managed = false;
    s.device_state = 10;
    CHECK(strcmp(xd_nc_status_text(&s), "Disabled") == 0);
    s.managed = true;
    s.device_state = 70;
    CHECK(strcmp(xd_nc_status_text(&s), "Acquiring network address") == 0);
    s.device_state = 60;
    CHECK(strcmp(xd_nc_status_text(&s), "Authenticating") == 0);
    /* A VPN goes by its active connection. */
    xd_nc_state v = {.active_state = 0};
    CHECK(strcmp(xd_nc_status_text(&v), "Disconnected") == 0 && !xd_nc_state_disabled(&v));
    v.active_state = 2;
    CHECK(strcmp(xd_nc_status_text(&v), "Connected") == 0 && xd_nc_state_connected(&v));

    char *t = xd_nc_numbered_name("Local Area Connection", 1);
    CHECK(strcmp(t, "Local Area Connection") == 0);
    g_free(t);
    t = xd_nc_numbered_name("Local Area Connection", 3);
    CHECK(strcmp(t, "Local Area Connection 3") == 0);
    g_free(t);

    static const struct {
        guint64 n;
        const char *duration, *count;
    } nums[] = {
        {0, "00:00:00", "0"},
        {59, "00:00:59", "59"},
        {3725, "01:02:05", "3,725"},
        {86400 + 7384, "1 day 02:03:04", "93,784"},
        {3 * 86400, "3 days 00:00:00", "259,200"},
        {1234567, "14 days 06:56:07", "1,234,567"},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(nums); i++) {
        t = xd_nc_format_duration(nums[i].n);
        CHECK(strcmp(t, nums[i].duration) == 0);
        g_free(t);
        t = xd_nc_format_count(nums[i].n);
        CHECK(strcmp(t, nums[i].count) == 0);
        g_free(t);
    }
    static const struct {
        guint64 kbps;
        const char *text;
    } speeds[] = {{1000000, "1.0 Gbps"}, {2500000, "2.5 Gbps"}, {100000, "100.0 Mbps"},
                  {54000, "54.0 Mbps"},  {866700, "866.7 Mbps"}, {56, "56.0 Kbps"}};
    for (size_t i = 0; i < XD_ARRAY_LEN(speeds); i++) {
        t = xd_nc_format_speed(speeds[i].kbps);
        CHECK(strcmp(t, speeds[i].text) == 0);
        g_free(t);
    }

    unsigned prefix = 99;
    CHECK(xd_nc_netmask_to_prefix("255.255.255.0", &prefix) && prefix == 24);
    CHECK(xd_nc_netmask_to_prefix("255.255.240.0", &prefix) && prefix == 20);
    CHECK(xd_nc_netmask_to_prefix("0.0.0.0", &prefix) && prefix == 0);
    CHECK(xd_nc_netmask_to_prefix("255.255.255.255", &prefix) && prefix == 32);
    CHECK(!xd_nc_netmask_to_prefix("255.0.255.0", &prefix) && !xd_nc_netmask_to_prefix("", &prefix) &&
          !xd_nc_netmask_to_prefix("255.255.255", &prefix) && !xd_nc_netmask_to_prefix("::", &prefix));
    for (unsigned p = 0; p <= 32; p++) {
        t = xd_nc_prefix_to_netmask(p);
        unsigned back = 99;
        CHECK(xd_nc_netmask_to_prefix(t, &back) && back == p);
        g_free(t);
    }
    t = xd_nc_prefix_to_netmask(22);
    CHECK(strcmp(t, "255.255.252.0") == 0);
    g_free(t);
    CHECK(xd_nc_valid_ipv4("10.0.0.1") && !xd_nc_valid_ipv4("10.0.0") && !xd_nc_valid_ipv4("fe80::1") &&
          !xd_nc_valid_ipv4("300.1.1.1") && !xd_nc_valid_ipv4(nullptr));
    CHECK(xd_nc_default_prefix("10.1.2.3") == 8 && xd_nc_default_prefix("172.16.0.5") == 16 &&
          xd_nc_default_prefix("192.168.1.20") == 24 && xd_nc_default_prefix("224.0.0.1") == 0 &&
          xd_nc_default_prefix("x") == 0);

    xd_nc_ipv4_form f = {.manual_ip = true, .manual_dns = true, .address = "192.168.1.20",
                         .netmask = "255.255.255.0", .gateway = "192.168.1.1", .dns1 = "1.1.1.1",
                         .dns2 = ""};
    CHECK(xd_nc_check_ipv4_form(&f) == nullptr);
    f.gateway = "";
    CHECK(xd_nc_check_ipv4_form(&f) == nullptr); /* no gateway is fine */
    f.gateway = "192.168.2.1";
    CHECK(xd_nc_check_ipv4_form(&f) != nullptr); /* another subnet */
    f.gateway = "192.168.1.1";
    f.address = "";
    CHECK(xd_nc_check_ipv4_form(&f) != nullptr);
    f.address = "192.168.1.0"; /* the network's own address */
    CHECK(xd_nc_check_ipv4_form(&f) != nullptr);
    f.address = "192.168.1.255";
    CHECK(xd_nc_check_ipv4_form(&f) != nullptr);
    f.address = "127.0.0.5";
    f.netmask = "255.0.0.0";
    f.gateway = "";
    CHECK(xd_nc_check_ipv4_form(&f) != nullptr);
    f.address = "192.168.1.20";
    f.netmask = "255.0.255.0";
    CHECK(xd_nc_check_ipv4_form(&f) != nullptr);
    f.netmask = "255.255.255.0";
    f.dns2 = "dns.example";
    CHECK(xd_nc_check_ipv4_form(&f) != nullptr);
    /* Automatic: the blanks don't matter. */
    xd_nc_ipv4_form a = {.address = "", .netmask = "", .gateway = "", .dns1 = "", .dns2 = ""};
    CHECK(xd_nc_check_ipv4_form(&a) == nullptr);
    a.manual_dns = true;
    a.dns1 = "8.8.8.8";
    CHECK(xd_nc_check_ipv4_form(&a) == nullptr);
    a.dns1 = "8.8.8";
    CHECK(xd_nc_check_ipv4_form(&a) != nullptr);

    CHECK(strcmp(xd_nc_vpn_host_key("org.freedesktop.NetworkManager.openvpn"), "remote") == 0);
    CHECK(strcmp(xd_nc_vpn_host_key("org.freedesktop.NetworkManager.vpnc"), "IPSec gateway") == 0);
    CHECK(xd_nc_vpn_host_key("org.example.unknown") == nullptr && xd_nc_vpn_host_key(nullptr) == nullptr);

    CHECK(xd_nc_wifi_key_mgmt(0, 0, 0) == nullptr);
    CHECK(strcmp(xd_nc_wifi_key_mgmt(0x1, 0, 0), "none") == 0);
    CHECK(strcmp(xd_nc_wifi_key_mgmt(0x1, 0, 0x188), "wpa-psk") == 0);
    CHECK(strcmp(xd_nc_wifi_key_mgmt(0x1, 0, 0x400), "sae") == 0);
    CHECK(strcmp(xd_nc_wifi_key_mgmt(0x1, 0x200, 0x200), "wpa-eap") == 0);
    CHECK(xd_nc_valid_wifi_key("wpa-psk", "12345678") && !xd_nc_valid_wifi_key("wpa-psk", "1234567"));
    CHECK(xd_nc_valid_wifi_key("wpa-psk",
                               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    CHECK(!xd_nc_valid_wifi_key("wpa-psk",
                                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"));
    CHECK(xd_nc_valid_wifi_key("none", "abcde") && xd_nc_valid_wifi_key("none", "0123456789") &&
          !xd_nc_valid_wifi_key("none", "abcdefghij"));
    CHECK(strcmp(xd_nc_signal_words(90), "Excellent") == 0 && strcmp(xd_nc_signal_words(5), "Very low") == 0);
}

static void test_admin_services(void) {
    char *l = xd_unit_startup_label("enabled");
    CHECK(strcmp(l, "Enabled") == 0);
    g_free(l);
    l = xd_unit_startup_label("masked-runtime");
    CHECK(strcmp(l, "Masked (runtime)") == 0);
    g_free(l);
    l = xd_unit_startup_label(nullptr);
    CHECK(strcmp(l, "") == 0);
    g_free(l);
    CHECK(xd_unit_startup_of("enabled") == XD_STARTUP_ENABLED);
    CHECK(xd_unit_startup_of("disabled") == XD_STARTUP_DISABLED);
    CHECK(xd_unit_startup_of("masked") == XD_STARTUP_MASKED);
    CHECK(xd_unit_startup_of("static") == XD_STARTUP_OTHER);
    CHECK(xd_unit_startup_of("enabled-runtime") == XD_STARTUP_OTHER);

    CHECK(strcmp(xd_unit_status_label("active"), "Started") == 0);
    CHECK(strcmp(xd_unit_status_label("inactive"), "") == 0);
    CHECK(strcmp(xd_unit_status_label("failed"), "Failed") == 0);
    CHECK(strcmp(xd_unit_status_label("deactivating"), "Stopping") == 0);
    CHECK(xd_unit_is_started("active") && xd_unit_is_started("activating"));
    CHECK(!xd_unit_is_started("inactive") && !xd_unit_is_started("failed"));

    char *n = xd_unit_display_name("sshd.service");
    CHECK(strcmp(n, "sshd") == 0);
    g_free(n);
    n = xd_unit_display_name("user@1000.service");
    CHECK(strcmp(n, "user@1000") == 0);
    g_free(n);
    n = xd_unit_display_name("noext");
    CHECK(strcmp(n, "noext") == 0);
    g_free(n);

    CHECK(strcmp(xd_unit_logon_label("http", false, "me"), "http") == 0);
    CHECK(strcmp(xd_unit_logon_label("", false, "me"), "Local System") == 0);
    CHECK(strcmp(xd_unit_logon_label(nullptr, true, "me"), "me") == 0);

    const char *argv[] = {"/usr/bin/nginx", "-g", "pid /run/nginx.pid;", "", nullptr};
    char *cmd = xd_unit_command_line(argv);
    CHECK(strcmp(cmd, "/usr/bin/nginx -g 'pid /run/nginx.pid;' ''") == 0);
    g_free(cmd);
}

static void test_admin_events(void) {
    CHECK(xd_event_log_of(4, "whatever", "syslog", false, 0) == XD_LOG_SECURITY);
    CHECK(xd_event_log_of(10, nullptr, "journal", false, 1000) == XD_LOG_SECURITY);
    CHECK(xd_event_log_of(-1, "sudo", "journal", false, 1000) == XD_LOG_SECURITY);
    CHECK(xd_event_log_of(-1, nullptr, "audit", false, -1) == XD_LOG_SECURITY);
    CHECK(xd_event_log_of(-1, nullptr, "kernel", false, -1) == XD_LOG_SYSTEM);
    CHECK(xd_event_log_of(3, "systemd", "journal", false, 0) == XD_LOG_SYSTEM);
    CHECK(xd_event_log_of(-1, "firefox", "journal", false, 1000) == XD_LOG_APPLICATION);
    CHECK(xd_event_log_of(-1, "systemd", "journal", true, 1000) == XD_LOG_APPLICATION);
    CHECK(xd_event_log_of(-1, "daemon", "journal", false, 65534) == XD_LOG_SYSTEM);

    CHECK(xd_event_type_of(0, XD_LOG_SYSTEM) == XD_EVENT_ERROR);
    CHECK(xd_event_type_of(3, XD_LOG_APPLICATION) == XD_EVENT_ERROR);
    CHECK(xd_event_type_of(4, XD_LOG_SYSTEM) == XD_EVENT_WARNING);
    CHECK(xd_event_type_of(6, XD_LOG_SYSTEM) == XD_EVENT_INFORMATION);
    CHECK(xd_event_type_of(-1, XD_LOG_SYSTEM) == XD_EVENT_INFORMATION);
    CHECK(xd_event_type_of(99, XD_LOG_SYSTEM) == XD_EVENT_INFORMATION);
    CHECK(xd_event_type_of(4, XD_LOG_SECURITY) == XD_EVENT_AUDIT_FAILURE);
    CHECK(xd_event_type_of(5, XD_LOG_SECURITY) == XD_EVENT_AUDIT_SUCCESS);
    CHECK(strcmp(xd_event_type_label(XD_EVENT_AUDIT_FAILURE), "Failure Audit") == 0);
    CHECK(strcmp(xd_event_type_icon(XD_EVENT_ERROR), "dialog-error") == 0);
}

static void test_admin_disks(void) {
    char *t = xd_disk_size_text(250059350016ULL);
    CHECK(strcmp(t, "232.88 GB") == 0);
    g_free(t);
    t = xd_disk_size_text(512 * XD_MIB);
    CHECK(strcmp(t, "512 MB") == 0);
    g_free(t);
    t = xd_disk_size_text(4096);
    CHECK(strcmp(t, "4 KB") == 0);
    g_free(t);
    t = xd_disk_size_text(2ULL * 1024 * 1024 * XD_MIB);
    CHECK(strcmp(t, "2.00 TB") == 0);
    g_free(t);
    t = xd_disk_fs_label("vfat", "FAT32");
    CHECK(strcmp(t, "FAT32") == 0);
    g_free(t);
    t = xd_disk_fs_label("vfat", "FAT16");
    CHECK(strcmp(t, "FAT") == 0);
    g_free(t);
    t = xd_disk_fs_label("iso9660", nullptr);
    CHECK(strcmp(t, "CDFS") == 0);
    g_free(t);
    t = xd_disk_fs_label("ext4", "1.0");
    CHECK(strcmp(t, "ext4") == 0);
    g_free(t);
    t = xd_disk_fs_label("", nullptr);
    CHECK(strcmp(t, "") == 0);
    g_free(t);
    CHECK(xd_disk_percent_free(45, 100) == 45);
    CHECK(xd_disk_percent_free(1, 3) == 33 && xd_disk_percent_free(2, 3) == 67);
    CHECK(xd_disk_percent_free(5, 0) == 0 && xd_disk_percent_free(7, 5) == 100);

    CHECK(xd_disk_align_up(0) == 0 && xd_disk_align_up(1) == XD_MIB);
    CHECK(xd_disk_align_up(XD_MIB) == XD_MIB && xd_disk_align_down(XD_MIB + 5) == XD_MIB);

    /* An 8 GiB MBR disk: a 2 GiB primary partition, a 4 GiB extended
     * one holding a 1 GiB logical drive, and 2 GiB never used. */
    const guint64 G = 1024 * XD_MIB;
    const xd_disk_part parts[] = {
        {.offset = 2 * G + XD_MIB, .size = 4 * G, .index = 1, .container = true},
        {.offset = XD_MIB, .size = 2 * G, .index = 0},
        {.offset = 2 * G + 2 * XD_MIB, .size = G, .index = 2},
    };
    GArray *segs = xd_disk_segments(8 * G, parts, G_N_ELEMENTS(parts));
    CHECK(segs->len == 4);
    if (segs->len == 4) {
        const xd_disk_segment *s = &g_array_index(segs, xd_disk_segment, 0);
        CHECK(s[0].kind == XD_SEGMENT_PARTITION && s[0].index == 0 && !s[0].logical);
        CHECK(s[1].kind == XD_SEGMENT_PARTITION && s[1].index == 2 && s[1].logical);
        CHECK(s[2].kind == XD_SEGMENT_FREE && s[2].logical && s[2].offset == 3 * G + 2 * XD_MIB &&
              s[2].size == 3 * G - XD_MIB);
        CHECK(s[3].kind == XD_SEGMENT_UNALLOCATED && s[3].offset == 6 * G + XD_MIB &&
              s[3].size == 2 * G - XD_MIB);
        /* The logical drive grows into the extended partition's free
         * space; the primary one has a neighbour. */
        CHECK(xd_disk_grow_max(segs, 1, 8 * G) == 4 * G - XD_MIB);
        CHECK(xd_disk_grow_max(segs, 0, 8 * G) == 2 * G);
        guint64 off = 0, max = 0;
        CHECK(xd_disk_new_part_range(s[2].offset, s[2].size, 8 * G, true, &off, &max));
        CHECK(off == 3 * G + 3 * XD_MIB && max == 3 * G - 2 * XD_MIB);
        CHECK(xd_disk_new_part_range(s[3].offset, s[3].size, 8 * G, false, &off, &max));
        CHECK(off == 6 * G + XD_MIB && max == 2 * G - 2 * XD_MIB);
    }
    g_array_unref(segs);
    /* An empty disk is one unallocated block; overlaps are clipped. */
    segs = xd_disk_segments(4 * G, nullptr, 0);
    CHECK(segs->len == 1 &&
          g_array_index(segs, xd_disk_segment, 0).kind == XD_SEGMENT_UNALLOCATED);
    g_array_unref(segs);
    const xd_disk_part overlap[] = {{.offset = XD_MIB, .size = 2 * G, .index = 0},
                                    {.offset = G, .size = 4 * G, .index = 1}};
    segs = xd_disk_segments(4 * G, overlap, 2);
    CHECK(segs->len == 2 && g_array_index(segs, xd_disk_segment, 1).offset == 2 * G + XD_MIB &&
          g_array_index(segs, xd_disk_segment, 1).size == 2 * G - XD_MIB);
    g_array_unref(segs);
    guint64 off = 0, max = 0;
    CHECK(xd_disk_new_part_range(0, 4 * G, 4 * G, false, &off, &max) && off == XD_MIB &&
          max == 4 * G - 2 * XD_MIB);
    CHECK(!xd_disk_new_part_range(XD_MIB, XD_MIB / 2, 4 * G, false, &off, &max));

    xd_new_part_kinds k = xd_disk_new_part_kinds("dos", XD_SEGMENT_UNALLOCATED, 2, true);
    CHECK(k.primary && !k.extended && !k.logical);
    k = xd_disk_new_part_kinds("dos", XD_SEGMENT_UNALLOCATED, 1, false);
    CHECK(k.primary && k.extended && !k.logical);
    k = xd_disk_new_part_kinds("dos", XD_SEGMENT_UNALLOCATED, 4, false);
    CHECK(!k.primary && !k.extended && !k.logical);
    k = xd_disk_new_part_kinds("dos", XD_SEGMENT_FREE, 4, true);
    CHECK(!k.primary && !k.extended && k.logical);
    k = xd_disk_new_part_kinds("gpt", XD_SEGMENT_UNALLOCATED, 10, false);
    CHECK(k.primary && !k.extended && !k.logical);
    k = xd_disk_new_part_kinds("gpt", XD_SEGMENT_PARTITION, 1, false);
    CHECK(!k.primary);

    CHECK(xd_disk_shrink_min(0, 10 * G) == 32 * XD_MIB);
    CHECK(xd_disk_shrink_min(G, 10 * G) == G + 103 * XD_MIB);
    CHECK(xd_disk_shrink_min(10 * XD_MIB, 10 * G) == 74 * XD_MIB);
    CHECK(xd_disk_shrink_min(10 * XD_MIB, 20 * XD_MIB) == 20 * XD_MIB);

    const guint64 ext4 = XD_RESIZE_OFFLINE_SHRINK | XD_RESIZE_OFFLINE_GROW | XD_RESIZE_ONLINE_GROW;
    CHECK(xd_disk_resize_how(ext4, true, true) == XD_RESIZE_NEED_UNMOUNT);
    CHECK(xd_disk_resize_how(ext4, true, false) == XD_RESIZE_AS_IS);
    CHECK(xd_disk_resize_how(ext4, false, true) == XD_RESIZE_AS_IS);
    CHECK(xd_disk_resize_how(XD_RESIZE_ONLINE_SHRINK | XD_RESIZE_ONLINE_GROW, false, false) ==
          XD_RESIZE_NEED_MOUNT);
    CHECK(xd_disk_resize_how(XD_RESIZE_OFFLINE_GROW | XD_RESIZE_ONLINE_GROW, true, true) ==
          XD_RESIZE_NO);

    CHECK(xd_mount_is_system("/") && xd_mount_is_system("/boot/efi"));
    CHECK(!xd_mount_is_system("/run/media/me/USB") && !xd_mount_is_system(nullptr));
    bool unmount = false;
    const char *root[] = {"/", nullptr}, *media[] = {"/run/media/me/Data", nullptr};
    xd_volume_use u = {.mounts = root, .id_type = "ext4"};
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_FORMAT, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_DELETE, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_SHRINK, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_EXTEND, &unmount) == nullptr && !unmount);
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_LABEL, &unmount) == nullptr);
    u.mounts = media;
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_FORMAT, &unmount) == nullptr && unmount);
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_SET_TYPE, &unmount) == nullptr && !unmount);
    u = (xd_volume_use){.id_type = "swap", .swap_active = true};
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_DELETE, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_LABEL, &unmount) != nullptr);
    u.swap_active = false;
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_DELETE, &unmount) == nullptr && !unmount);
    u = (xd_volume_use){.container = true, .has_logical = true};
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_DELETE, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_FORMAT, &unmount) != nullptr);
    u.has_logical = false;
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_DELETE, &unmount) == nullptr);
    u = (xd_volume_use){.id_type = "LVM2_member"};
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_FORMAT, &unmount) != nullptr);
    u = (xd_volume_use){.id_type = "crypto_LUKS", .unlocked = true};
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_DELETE, &unmount) != nullptr);
    u.unlocked = false;
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_DELETE, &unmount) == nullptr);
    CHECK(xd_volume_lock_reason(&u, XD_ACTION_EXTEND, &unmount) != nullptr);

    char *st = xd_volume_status(root, false, false, false);
    CHECK(strcmp(st, "Healthy (System)") == 0);
    g_free(st);
    st = xd_volume_status(nullptr, true, false, true);
    CHECK(strcmp(st, "Healthy (Page File, Active)") == 0);
    g_free(st);
    st = xd_volume_status(media, false, false, false);
    CHECK(strcmp(st, "Healthy") == 0);
    g_free(st);
    st = xd_volume_status(nullptr, false, true, false);
    CHECK(strcmp(st, "Healthy (EFI System Partition)") == 0);
    g_free(st);

    CHECK(g_strcmp0(xd_part_type_name("gpt", "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"),
                    "EFI System Partition") == 0);
    CHECK(g_strcmp0(xd_part_type_name("dos", "0x83"), "Linux") == 0);
    CHECK(xd_part_type_name("dos", "0fc63daf-8483-4772-8e79-3d69d8477de4") == nullptr);
    CHECK(xd_part_type_is_esp("0xef") && !xd_part_type_is_esp("0x83"));

    const guint64 sizes[] = {1, 1, 2};
    int w[3] = {0};
    xd_disk_block_widths(sizes, 3, 400, 50, w);
    CHECK(w[0] + w[1] + w[2] == 400 && w[0] >= 50 && w[2] > w[0] && w[0] == w[1]);
    xd_disk_block_widths(sizes, 3, 100, 50, w);
    CHECK(w[0] == 50 && w[1] == 50 && w[2] == 50);
}

static void test_appwiz(void) {
    /* Usage buckets: nothing without counts at all. */
    CHECK(xd_appwiz_usage_for(50, false) == XD_USAGE_UNKNOWN);
    CHECK(xd_appwiz_usage_name(XD_USAGE_UNKNOWN) == nullptr);
    CHECK(xd_appwiz_usage_for(0, true) == XD_USAGE_RARELY);
    CHECK(xd_appwiz_usage_for(2, true) == XD_USAGE_RARELY);
    CHECK(xd_appwiz_usage_for(3, true) == XD_USAGE_OCCASIONALLY);
    CHECK(xd_appwiz_usage_for(9, true) == XD_USAGE_OCCASIONALLY);
    CHECK(xd_appwiz_usage_for(10, true) == XD_USAGE_FREQUENTLY);
    CHECK(strcmp(xd_appwiz_usage_name(XD_USAGE_OCCASIONALLY), "occasionally") == 0);

    /* The Start menu's launch counts, as xd-shell writes them. */
    char *lines[] = {(char *)(uintptr_t)"12\tfirefox.desktop",
                     (char *)(uintptr_t)"4\torg.gnome.TextEditor.desktop",
                     (char *)(uintptr_t)"3\tfirefox.desktop", (char *)(uintptr_t)"x\tbad.desktop",
                     (char *)(uintptr_t)"7", (char *)(uintptr_t)"5\t",
                     (char *)(uintptr_t)"-1\tneg.desktop"};
    GHashTable *counts = xd_appwiz_parse_launch_counts(lines, G_N_ELEMENTS(lines));
    CHECK(g_hash_table_size(counts) == 2);
    CHECK(xd_appwiz_launches(counts, "firefox.desktop") == 12);
    CHECK(xd_appwiz_launches(counts, "org.gnome.TextEditor.desktop") == 4);
    CHECK(xd_appwiz_launches(counts, "bad.desktop") == 0);
    CHECK(xd_appwiz_launches(nullptr, "firefox.desktop") == 0);
    g_hash_table_unref(counts);

    /* Sizes as XP wrote them. */
    char *s = xd_appwiz_format_size(23477862);
    CHECK(strcmp(s, "22.39MB") == 0);
    g_free(s);
    s = xd_appwiz_format_size(100);
    CHECK(strcmp(s, "0.01MB") == 0);
    g_free(s);
    s = xd_appwiz_format_size(1288490189);
    CHECK(strcmp(s, "1.20GB") == 0);
    g_free(s);
    s = xd_appwiz_format_date(0);
    CHECK(strcmp(s, "") == 0);
    g_free(s);
    s = xd_appwiz_format_date(1160000000);
    CHECK(strstr(s, "06") != nullptr);
    g_free(s);

    /* Sort orders. */
    xd_appwiz_key a = {"Alpha", 100, 1, 50}, b = {"beta", 300, 1, 0}, c = {"Gamma", 0, 9, 70},
                  d = {"delta", 100, 0, 50};
    CHECK(xd_appwiz_compare(&a, &b, XD_SORT_NAME) < 0);
    CHECK(xd_appwiz_compare(&b, &c, XD_SORT_NAME) < 0); /* case doesn't matter */
    CHECK(xd_appwiz_compare(&b, &a, XD_SORT_SIZE) < 0); /* largest first */
    CHECK(xd_appwiz_compare(&a, &c, XD_SORT_SIZE) < 0); /* unknown last */
    CHECK(xd_appwiz_compare(&a, &d, XD_SORT_SIZE) < 0); /* ties by name */
    CHECK(xd_appwiz_compare(&c, &a, XD_SORT_FREQUENCY) < 0);
    CHECK(xd_appwiz_compare(&a, &b, XD_SORT_FREQUENCY) < 0);
    CHECK(xd_appwiz_compare(&c, &a, XD_SORT_DATE) < 0);  /* most recent first */
    CHECK(xd_appwiz_compare(&d, &b, XD_SORT_DATE) < 0);  /* never used last */
    CHECK(xd_appwiz_compare(&a, &a, XD_SORT_DATE) == 0);

    /* PackageKit ids, split by PackageKit. */
    char *name = nullptr, *version = nullptr, *repo = nullptr;
    CHECK(xd_appwiz_split_package_id("gimp;3.0.4-1;x86_64;installed:extra", &name, &version,
                                     &repo));
    CHECK(strcmp(name, "gimp") == 0 && strcmp(version, "3.0.4-1") == 0 &&
          strcmp(repo, "extra") == 0);
    g_free(name);
    g_free(version);
    g_free(repo);
    CHECK(xd_appwiz_split_package_id("alacritty;0.17.0-1.2;x86_64_v3;installed", nullptr,
                                     nullptr, &repo));
    CHECK(strcmp(repo, "") == 0);
    g_free(repo);
    CHECK(xd_appwiz_split_package_id("hello;2.12-1;amd64;debian-main", &name, nullptr, &repo));
    CHECK(strcmp(name, "hello") == 0 && strcmp(repo, "debian-main") == 0);
    g_free(name);
    g_free(repo);
    CHECK(!xd_appwiz_split_package_id("not a package id", &name, nullptr, nullptr));
    CHECK(!xd_appwiz_split_package_id(nullptr, nullptr, nullptr, nullptr));
    CHECK(xd_appwiz_package_installed("gimp;3.0.4-1;x86_64;installed:extra"));
    CHECK(xd_appwiz_package_installed("alacritty;0.17.0-1.2;x86_64_v3;installed"));
    CHECK(!xd_appwiz_package_installed("gimp;3.0.4-1;x86_64;extra"));
    CHECK(!xd_appwiz_package_installed("gimp;3.0.4-1;x86_64;installedfoo"));

    /* Last used, from the recent documents list. */
    static const char xbel[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<xbel version=\"1.0\" "
        "xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\" "
        "xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
        "<bookmark href=\"file:///tmp/a.txt\" added=\"2026-01-01T00:00:00Z\" "
        "modified=\"2026-01-01T00:00:00Z\" visited=\"2026-01-01T00:00:00Z\"><info>"
        "<metadata owner=\"http://freedesktop.org\"><mime:mime-type type=\"text/plain\"/>"
        "<bookmark:applications>"
        "<bookmark:application name=\"org.gnome.TextEditor\" "
        "exec=\"&apos;gnome-text-editor %u&apos;\" modified=\"2026-03-04T05:06:07Z\" "
        "count=\"2\"/>"
        "<bookmark:application name=\"Firefox\" exec=\"&apos;firefox %u&apos;\" "
        "modified=\"2026-02-01T00:00:00Z\" count=\"1\"/>"
        "</bookmark:applications></metadata></info></bookmark>\n"
        "<bookmark href=\"file:///tmp/b.txt\" added=\"2026-01-01T00:00:00Z\" "
        "modified=\"2026-01-01T00:00:00Z\" visited=\"2026-01-01T00:00:00Z\"><info>"
        "<metadata owner=\"http://freedesktop.org\"><mime:mime-type type=\"text/plain\"/>"
        "<bookmark:applications>"
        "<bookmark:application name=\"org.gnome.TextEditor\" "
        "exec=\"&apos;gnome-text-editor %u&apos;\" modified=\"2026-05-01T00:00:00Z\" "
        "count=\"1\"/>"
        "</bookmark:applications></metadata></info></bookmark>\n"
        "</xbel>\n";
    GBookmarkFile *recent = g_bookmark_file_new();
    CHECK(g_bookmark_file_load_from_data(recent, xbel, sizeof xbel - 1, nullptr));
    CHECK(xd_appwiz_last_used(recent, "org.gnome.TextEditor.desktop", "gnome-text-editor") ==
          1777593600); /* 2026-05-01 */
    CHECK(xd_appwiz_last_used(recent, "firefox.desktop", "/usr/lib/firefox/firefox") ==
          1769904000); /* 2026-02-01, by the command */
    CHECK(xd_appwiz_last_used(recent, "gimp.desktop", "gimp") == 0);
    CHECK(xd_appwiz_last_used(nullptr, "gimp.desktop", "gimp") == 0);
    g_bookmark_file_free(recent);
}

/* Printers and Faxes: CUPS's states in XP's words, sizes, ports, names. */
static void test_printers(void) {
    CHECK(strcmp(xd_printer_status_text(3, "none", true), "Ready") == 0);
    CHECK(strcmp(xd_printer_status_text(3, nullptr, true), "Ready") == 0);
    CHECK(strcmp(xd_printer_status_text(4, "none", true), "Printing") == 0);
    CHECK(strcmp(xd_printer_status_text(5, "paused", true), "Paused") == 0);
    CHECK(strcmp(xd_printer_status_text(5, "offline-report,paused", true), "Paused") == 0);
    CHECK(strcmp(xd_printer_status_text(3, "offline-report", true), "Offline") == 0);
    CHECK(strcmp(xd_printer_status_text(4, "media-empty-error", true), "Paper Out") == 0);
    CHECK(strcmp(xd_printer_status_text(3, "toner-low-warning", true), "Toner/Ink Low") == 0);
    CHECK(strcmp(xd_printer_status_text(3, "cups-missing-filter-error", true), "Error") == 0);
    CHECK(strcmp(xd_printer_status_text(5, "none", true), "Error") == 0);
    CHECK(strcmp(xd_printer_status_text(3, "none", false), "Not Accepting Documents") == 0);
    /* A reason that only starts like a known one isn't it. */
    CHECK(strcmp(xd_printer_status_text(3, "pausedx", true), "Ready") == 0);
    CHECK(xd_printer_is_paused(5, "paused"));
    CHECK(!xd_printer_is_paused(3, "paused"));

    CHECK(strcmp(xd_print_job_status_text(3), "") == 0);
    CHECK(strcmp(xd_print_job_status_text(4), "Paused") == 0);
    CHECK(strcmp(xd_print_job_status_text(5), "Printing") == 0);
    CHECK(strcmp(xd_print_job_status_text(8), "Error") == 0);

    static const struct {
        guint64 bytes;
        const char *text;
    } sizes[] = {
        {0, "0 bytes"},           {999, "999 bytes"},    {1000, "0.97 KB"},
        {1024, "1.00 KB"},        {43520, "42.5 KB"},    {435200, "425 KB"},
        {1023 * 1024, "0.99 MB"}, {1440000, "1.37 MB"}, {5ULL << 40, "5.00 TB"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(sizes); i++) {
        char *s = xd_print_size_text(sizes[i].bytes);
        CHECK(strcmp(s, sizes[i].text) == 0);
        if (strcmp(s, sizes[i].text) != 0)
            fprintf(stderr, "  size %" G_GUINT64_FORMAT ": %s\n", sizes[i].bytes, s);
        g_free(s);
    }
    char *p = xd_print_pages_text(0, 0, false);
    CHECK(strcmp(p, "N/A") == 0);
    g_free(p);
    p = xd_print_pages_text(2, 5, true);
    CHECK(strcmp(p, "2/5") == 0);
    g_free(p);
    p = xd_print_pages_text(0, 5, false);
    CHECK(strcmp(p, "5") == 0);
    g_free(p);

    static const char *const ports[][2] = {
        {"socket://192.168.1.5:9100", "IP_192.168.1.5"},
        {"ipp://printer.local/ipp/print", "IP_printer.local"},
        {"dnssd://Office%20Laser._ipp._tcp.local/?uuid=1", "Office Laser"},
        {"usb://HP/LaserJet?serial=1", "USB"},
        {"parallel:/dev/lp0", "LPT1:"},
        {"serial:/dev/ttyS1?baud=9600", "COM2:"},
        {"smb://server/laser", "\\\\server\\laser"},
        {"file:/dev/null", "/dev/null"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(ports); i++) {
        char *s = xd_printer_port_text(ports[i][0]);
        CHECK(strcmp(s, ports[i][1]) == 0);
        if (strcmp(s, ports[i][1]) != 0)
            fprintf(stderr, "  port %s: %s\n", ports[i][0], s);
        g_free(s);
    }
    CHECK(strcmp(xd_printer_port_description("socket://h"), "Standard TCP/IP Port") == 0);
    CHECK(strcmp(xd_printer_port_description("usb://HP/x"), "USB Port") == 0);
    CHECK(xd_printer_uri_is_network("ipps://h/ipp/print"));
    CHECK(!xd_printer_uri_is_network("usb://HP/x"));
    CHECK(!xd_printer_uri_is_network(nullptr));

    CHECK(xd_printer_name_valid("Office_Laser"));
    CHECK(!xd_printer_name_valid("Office Laser"));
    CHECK(!xd_printer_name_valid("a/b"));
    CHECK(!xd_printer_name_valid(""));
    char *n = xd_printer_name_suggest("HP LaserJet 4 (Room #2)");
    CHECK(strcmp(n, "HP_LaserJet_4_(Room_2)") == 0);
    CHECK(xd_printer_name_valid(n));
    g_free(n);
    n = xd_printer_name_suggest("  ");
    CHECK(strcmp(n, "Printer") == 0);
    g_free(n);
    char long_name[300];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    n = xd_printer_name_suggest(long_name);
    CHECK(strlen(n) == 127 && xd_printer_name_valid(n));
    g_free(n);

    CHECK(strcmp(xd_print_orientation_label(4), "Landscape") == 0);
    CHECK(xd_print_orientation_label(7) == nullptr);
    CHECK(strcmp(xd_print_sides_label("two-sided-long-edge"), "Flip on Long Edge") == 0);
    CHECK(strcmp(xd_print_color_label("monochrome"), "Black & White") == 0);
    CHECK(strcmp(xd_print_quality_label(5), "Best") == 0);
    char *k = xd_print_keyword_label("stationery-heavyweight");
    CHECK(strcmp(k, "Stationery heavyweight") == 0);
    g_free(k);
}

/* Scanners and Cameras: SANE modes for XP's picture types, resolutions,
 * file names, and SANE's sample formats. */
static void test_scanners(void) {
    const char *test_modes[] = {"Gray", "Color"};
    bool one_bit = true;
    CHECK(strcmp(xd_scan_mode_for(XD_PICTURE_COLOR, test_modes, 2, &one_bit), "Color") == 0 &&
          !one_bit);
    CHECK(strcmp(xd_scan_mode_for(XD_PICTURE_BW, test_modes, 2, &one_bit), "Gray") == 0 &&
          one_bit);
    const char *other[] = {"Lineart", "24bit Color", "Grayscale"};
    CHECK(strcmp(xd_scan_mode_for(XD_PICTURE_BW, other, 3, &one_bit), "Lineart") == 0 &&
          !one_bit);
    CHECK(strcmp(xd_scan_mode_for(XD_PICTURE_COLOR, other, 3, &one_bit), "24bit Color") == 0);
    CHECK(strcmp(xd_scan_mode_for(XD_PICTURE_GRAY, other, 3, &one_bit), "Grayscale") == 0);
    const char *only_color[] = {"Color"};
    CHECK(xd_scan_mode_for(XD_PICTURE_GRAY, only_color, 1, &one_bit) == nullptr);

    GArray *r = g_array_new(FALSE, FALSE, sizeof(int));
    xd_scan_resolutions_in_range(1, 1200, r);
    CHECK(r->len == 7 && g_array_index(r, int, 0) == 75 && g_array_index(r, int, 6) == 1200);
    g_array_set_size(r, 0);
    xd_scan_resolutions_in_range(110, 140, r);
    CHECK(r->len == 2 && g_array_index(r, int, 0) == 110 && g_array_index(r, int, 1) == 140);
    g_array_unref(r);
    CHECK(xd_scan_preview_dpi(1, 1200) == 50);
    CHECK(xd_scan_preview_dpi(75, 600) == 75);

    CHECK(xd_scan_name_valid("Picture"));
    CHECK(!xd_scan_name_valid("a/b") && !xd_scan_name_valid("..") && !xd_scan_name_valid(""));
    char *dir = g_dir_make_tmp("xd-scan-XXXXXX", nullptr);
    char *f = xd_scan_next_file(dir, "Picture", "png");
    char *want = g_build_filename(dir, "Picture 001.png", nullptr);
    CHECK(strcmp(f, want) == 0);
    CHECK(g_file_set_contents(f, "", 0, nullptr));
    char *f2 = xd_scan_next_file(dir, "Picture", "png");
    CHECK(g_str_has_suffix(f2, "Picture 002.png"));
    CHECK(g_remove(f) == 0);
    CHECK(g_rmdir(dir) == 0);
    g_free(f2);
    g_free(want);
    g_free(f);
    g_free(dir);

    guint8 rgb[3 * 9];
    const SANE_Byte bits[] = {0xA0, 0x80}; /* 1010 0000 1: black, white, black, ... */
    CHECK(xd_scan_convert_line(SANE_FRAME_GRAY, 1, 9, bits, rgb));
    CHECK(rgb[0] == 0 && rgb[3] == 255 && rgb[6] == 0 && rgb[9] == 255 && rgb[24] == 0);
    const SANE_Byte gray8[] = {10, 200};
    CHECK(xd_scan_convert_line(SANE_FRAME_GRAY, 8, 2, gray8, rgb));
    CHECK(rgb[0] == 10 && rgb[2] == 10 && rgb[3] == 200);
    const guint16 wide[3] = {0xFF00, 0x1234, 0x0001};
    SANE_Byte wide_bytes[sizeof(wide)];
    memcpy(wide_bytes, wide, sizeof(wide));
    CHECK(xd_scan_convert_line(SANE_FRAME_RGB, 16, 1, wide_bytes, rgb));
    CHECK(rgb[0] == 0xFF && rgb[1] == 0x12 && rgb[2] == 0x00);
    memset(rgb, 7, sizeof(rgb));
    const SANE_Byte green[] = {99};
    CHECK(xd_scan_convert_line(SANE_FRAME_GREEN, 8, 1, green, rgb));
    CHECK(rgb[0] == 7 && rgb[1] == 99 && rgb[2] == 7);
    CHECK(!xd_scan_convert_line(SANE_FRAME_GRAY, 4, 1, green, rgb));
}

int main(void) {
    test_str();
    test_arena();
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
    test_tray_rules();
    test_power_settings();
    test_sound_scheme();
    test_gtk_css();
    test_passwd();
    test_intl();
    test_users();
    test_folder_options();
    test_input_settings();
    test_kbd_layouts();
    test_cursor_themes();
    test_fonts();
    test_bluetooth_info();
    test_joystick();
    test_bluetooth_settings();
    test_netconn();
    test_admin_services();
    test_admin_events();
    test_admin_disks();
    test_appwiz();
    test_printers();
    test_scanners();
    test_schedtasks();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
